#include "room/Controller.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>

namespace nesto::room {
namespace {

std::string format(double value, double step) {
  char text[32];
  std::snprintf(text, sizeof text, step < 1 ? "%.1f" : "%.0f", value);
  return text;
}

std::string capitalised(std::string s) {
  if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  std::replace(s.begin(), s.end(), '_', ' ');
  return s;
}

double snap(double value, const ClimateFeatures& c) {
  const double snapped = c.min + std::round((value - c.min) / c.step) * c.step;
  return std::clamp(snapped, c.min, c.max);
}

/// The index of `value` in `list`, or -1.
int index_of(const std::vector<std::string>& list, const std::string& value) {
  const auto it = std::find(list.begin(), list.end(), value);
  return it == list.end() ? -1 : static_cast<int>(it - list.begin());
}

/// The choice after turning by `steps` from `current`, clamped at the ends.
std::optional<std::string> step_choice(const std::vector<std::string>& list,
                                       const std::string& current, int steps) {
  if (list.empty()) return std::nullopt;
  const int at = index_of(list, current);
  const int from = at < 0 ? (steps > 0 ? -1 : static_cast<int>(list.size())) : at;
  const int to = std::clamp(from + steps, 0, static_cast<int>(list.size()) - 1);
  if (to == at) return std::nullopt;
  return list[static_cast<std::size_t>(to)];
}

/// Controls that change with every tick of the ring, rather than per click.
bool is_smooth(Control control) {
  switch (control) {
    case Control::Brightness:
    case Control::ColorTemp:
    case Control::Hue:
    case Control::Position:
    case Control::Tilt:
      return true;
    default:
      return false;
  }
}

/// Time since `since`, never negative (see PanelSession).
Millis elapsed(Millis now, Millis since) {
  return now > since ? now - since : 0;
}

double fraction_of(double value, double lo, double hi) {
  return hi > lo ? std::clamp((value - lo) / (hi - lo), 0.0, 1.0) : -1;
}

// The Settings menu, in order: short names for the list, what each does
// (shown below it), and icons.
const char* const kSettings[] = {"Pair", "Forget", "About"};
const char* const kSettingsIcons[] = {"pair", "forget", "info"};
const char* const kSettingsSummaries[] = {"Pair with Home Assistant", "Forget Home Assistant",
                                          "Version and address"};
constexpr int kSettingsCount = 3;
constexpr int kPair = 0;
constexpr int kForget = 1;
constexpr int kAbout = 2;

const char* label(Control control) {
  switch (control) {
    case Control::Power: return "Power";
    case Control::Brightness: return "Brightness";
    case Control::ColorTemp: return "Colour temperature";
    case Control::Hue: return "Colour";
    case Control::Effect: return "Effect";
    case Control::Target: return "Target";
    case Control::TargetLow: return "Heat to";
    case Control::TargetHigh: return "Cool to";
    case Control::HvacMode: return "Mode";
    case Control::Preset: return "Preset";
    case Control::FanMode: return "Fan";
    case Control::Position: return "Position";
    case Control::Tilt: return "Tilt";
    case Control::CoverMove: return "Open or close";
  }
  return "";
}

}  // namespace

std::vector<Control> controls_for(const Item& item) {
  std::vector<Control> out;
  switch (item.kind) {
    case Kind::Light:
      out.push_back(item.light.brightness ? Control::Brightness : Control::Power);
      if (item.light.color_temp) out.push_back(Control::ColorTemp);
      if (item.light.color) out.push_back(Control::Hue);
      if (!item.light.effects.empty()) out.push_back(Control::Effect);
      break;
    case Kind::Switch:
      out.push_back(Control::Power);
      break;
    case Kind::Climate: {
      const auto& c = item.climate;
      if (c.target == ClimateFeatures::Target::Single) out.push_back(Control::Target);
      if (c.target == ClimateFeatures::Target::Range) {
        out.push_back(Control::TargetLow);
        out.push_back(Control::TargetHigh);
      }
      if (c.hvac_modes.size() > 1) out.push_back(Control::HvacMode);
      if (!c.presets.empty()) out.push_back(Control::Preset);
      if (!c.fan_modes.empty()) out.push_back(Control::FanMode);
      break;
    }
    case Kind::Cover:
      if (item.cover.position) {
        out.push_back(Control::Position);
      } else if (item.cover.open || item.cover.close) {
        out.push_back(Control::CoverMove);
      }
      if (item.cover.tilt) out.push_back(Control::Tilt);
      break;
    case Kind::Scene:
    case Kind::Sensor:
      break;
  }
  return out;
}

std::string summary(const Item& item, const ItemState& s) {
  if (!s.available) return "Unavailable";
  switch (item.kind) {
    case Kind::Light:
      if (!s.on) return "Off";
      return item.light.brightness ? "On - " + std::to_string(s.brightness) + "%" : "On";
    case Kind::Switch:
      return s.on ? "On" : "Off";
    case Kind::Scene:
      return "Scene";
    case Kind::Climate: {
      const auto& c = item.climate;
      if (s.hvac_mode == "off") return "Off";
      std::string out;
      if (s.current) out = format(*s.current, c.step) + c.unit;
      std::string target;
      if (s.target) target = format(*s.target, c.step) + c.unit;
      if (s.target_low && s.target_high) {
        target = format(*s.target_low, c.step) + "-" + format(*s.target_high, c.step) + c.unit;
      }
      if (!target.empty()) out += (out.empty() ? "Set to " : " - set to ") + target;
      return out.empty() ? capitalised(s.hvac_mode) : out;
    }
    case Kind::Cover: {
      std::string out = s.cover_state.empty() ? "Unknown" : capitalised(s.cover_state);
      if (s.position) out += " - " + std::to_string(*s.position) + "%";
      return out;
    }
    case Kind::Sensor:
      return item.unit.empty() ? s.value : s.value + " " + item.unit;
  }
  return "";
}

Tone tone_for(const Item& item, const ItemState& s) {
  if (!s.available) return Tone::Off;
  switch (item.kind) {
    case Kind::Light:
      return s.on ? Tone::Light : Tone::Off;
    case Kind::Switch:
      return s.on ? Tone::Active : Tone::Off;
    case Kind::Climate: {
      const std::string action = s.hvac_action.value_or("");
      if (action == "heating" || action == "preheating") return Tone::Heat;
      if (action == "cooling") return Tone::Cool;
      if (s.hvac_mode == "off" || action == "off") return Tone::Off;
      if (s.hvac_mode == "heat") return Tone::Heat;
      if (s.hvac_mode == "cool") return Tone::Cool;
      return Tone::Neutral;
    }
    case Kind::Cover:
      return s.cover_state == "closed" ? Tone::Off : Tone::Active;
    case Kind::Scene:
    case Kind::Sensor:
      return Tone::Neutral;
  }
  return Tone::Neutral;
}

const char* default_icon(Kind kind) {
  switch (kind) {
    case Kind::Light: return "lightbulb";
    case Kind::Switch: return "switch";
    case Kind::Scene: return "scene";
    case Kind::Climate: return "thermostat";
    case Kind::Cover: return "blind";
    case Kind::Sensor: return "sensor";
  }
  return "unknown";
}

Controller::Controller(Send send, SettingsActions actions)
    : send_(std::move(send)), actions_(std::move(actions)) {}

void Controller::set_connected(bool connected) {
  if (connected == connected_) return;
  connected_ = connected;
  if (!connected) {
    // Section 23: nothing is queued for later.
    pending_.clear();
    shown_.clear();
    hold_until_.clear();
  }
  changed();
}

void Controller::set_notifications(int count, int unread) {
  if (count == notification_count_ && unread == notification_unread_) return;
  // The entry comes or goes at the top: the selection stays on what it was.
  const int before = notices_entry();
  notification_count_ = count;
  notification_unread_ = unread;
  selected_ = std::clamp(selected_ + notices_entry() - before, 0, home_count() - 1);
  changed();
}

bool Controller::apply_layout(const Json& message, std::string& error) {
  if (!room_.apply_layout(message, error)) return false;
  click_ticks_ = 0;
  if (screen_ == RoomView::Screen::Empty) screen_ = RoomView::Screen::Home;
  if (screen_ == RoomView::Screen::Item && !room_.find(open_key_)) {
    screen_ = RoomView::Screen::Home;
  }
  selected_ = std::clamp(selected_, 0, home_count() - 1);  // the last is Settings
  if (const Item* item = open_item()) {
    control_ = std::clamp(control_, 0, std::max(0, static_cast<int>(controls_for(*item).size()) - 1));
  }
  for (auto it = shown_.begin(); it != shown_.end();) {
    it = room_.find(it->first) ? std::next(it) : shown_.erase(it);
  }
  for (auto it = pending_.begin(); it != pending_.end();) {
    it = room_.find(it->second.intent.key) ? std::next(it) : pending_.erase(it);
  }
  changed();
  return true;
}

void Controller::apply_states(const Json& message) {
  for (const std::string& key : room_.apply_states(message)) {
    // An item being adjusted keeps the panel's value until its hold ends.
    if (!hold_until_.count(key)) shown_.erase(key);
  }
  changed();
}

void Controller::intent_failed(const std::string& key) {
  shown_.erase(key);
  hold_until_.erase(key);
  for (auto it = pending_.begin(); it != pending_.end();) {
    it = it->second.intent.key == key ? pending_.erase(it) : std::next(it);
  }
  if (const Item* item = room_.find(key)) message_ = "Couldn't change " + item->name;
  changed();
}

std::optional<std::string> Controller::take_message() {
  auto out = std::move(message_);
  message_.reset();
  return out;
}

const Item* Controller::open_item() const {
  return screen_ == RoomView::Screen::Item ? room_.find(open_key_) : nullptr;
}

const Item* Controller::selected_item() const {
  const auto& items = room_.layout().items;
  const int at = selected_ - notices_entry();
  if (at < 0 || at >= static_cast<int>(items.size())) return nullptr;  // Notifications or Settings
  return &items[static_cast<std::size_t>(at)];
}

int Controller::home_count() const {
  // Notifications, if any, the items, then Settings.
  return notices_entry() + static_cast<int>(room_.layout().items.size()) + 1;
}

void Controller::open_settings() {
  screen_ = RoomView::Screen::Settings;
  setting_ = 0;
  changed();
}

void Controller::leave_settings() {
  screen_ = room_.has_layout() ? RoomView::Screen::Home : RoomView::Screen::Empty;
  changed();
}

ItemState Controller::shown(const std::string& key) const {
  const auto it = shown_.find(key);
  return it != shown_.end() ? it->second : room_.state(key);
}

bool Controller::can_act(const Item& item) const {
  return connected_ && shown(item.key).available;
}

int Controller::clicks(int ticks, Millis now) {
  if (elapsed(now, last_turn_) >= kClickRest || (click_ticks_ > 0 && ticks < 0) ||
      (click_ticks_ < 0 && ticks > 0)) {
    click_ticks_ = 0;
  }
  last_turn_ = now;
  click_ticks_ += ticks;
  // Division truncates towards zero, which keeps the leftover's sign.
  const int whole = click_ticks_ / kTicksPerClick;
  click_ticks_ -= whole * kTicksPerClick;
  return whole;
}

void Controller::rotate(int ticks, Millis now) {
  if (ticks == 0) return;
  if (screen_ == RoomView::Screen::Item) {
    const Item* item = open_item();
    if (!item) return;
    const auto controls = controls_for(*item);
    if (!can_act(*item) || controls.empty()) return;
    const Control control = controls[static_cast<std::size_t>(control_)];
    if (is_smooth(control)) {
      adjust(*item, control, ticks, now);
    } else if (const int n = clicks(ticks, now)) {
      adjust(*item, control, n, now);
    }
    return;
  }
  const int steps = clicks(ticks, now);
  if (steps == 0) return;
  if (screen_ == RoomView::Screen::Home) {
    const int next = std::clamp(selected_ + steps, 0, home_count() - 1);
    if (next != selected_) {
      selected_ = next;
      changed();
    }
  } else if (screen_ == RoomView::Screen::Settings) {
    const int next = std::clamp(setting_ + steps, 0, kSettingsCount - 1);
    if (next != setting_) {
      setting_ = next;
      changed();
    }
  }
}

void Controller::press(Millis now) {
  click_ticks_ = 0;  // what the ring controls may change
  switch (screen_) {
    case RoomView::Screen::Empty:
      open_settings();
      return;
    case RoomView::Screen::Settings:
      if (setting_ == kPair && actions_.open_pairing) actions_.open_pairing();
      if (setting_ == kForget) {
        screen_ = RoomView::Screen::Confirm;
        changed();
      }
      if (setting_ == kAbout) {
        screen_ = RoomView::Screen::About;
        changed();
      }
      return;
    case RoomView::Screen::Confirm:
      if (actions_.forget) actions_.forget();
      // Without Home Assistant there's no room any more.
      room_ = Room();
      shown_.clear();
      hold_until_.clear();
      pending_.clear();
      selected_ = 0;
      screen_ = RoomView::Screen::Empty;
      changed();
      return;
    case RoomView::Screen::About:
      screen_ = RoomView::Screen::Settings;
      changed();
      return;
    case RoomView::Screen::Home:
    case RoomView::Screen::Item:
    case RoomView::Screen::Notifications:
    case RoomView::Screen::Notice:
      break;
  }

  if (screen_ == RoomView::Screen::Home) {
    if (notices_entry() && selected_ == 0) {
      if (actions_.open_notifications) actions_.open_notifications();
      return;
    }
    const Item* item = selected_item();
    if (!item) {
      open_settings();
      return;
    }
    switch (item->kind) {
      case Kind::Light:
      case Kind::Climate:
      case Kind::Cover:
        if (controls_for(*item).empty()) return;
        screen_ = RoomView::Screen::Item;
        open_key_ = item->key;
        control_ = 0;
        changed();
        break;
      case Kind::Switch:
        if (can_act(*item)) toggle(*item, now);
        break;
      case Kind::Scene:
        if (can_act(*item)) act_now(*item, "activate", Json::object(), now);
        break;
      case Kind::Sensor:
        break;
    }
  } else if (const Item* item = open_item()) {
    const auto controls = controls_for(*item);
    if (controls.size() > 1) {
      control_ = (control_ + 1) % static_cast<int>(controls.size());
      changed();
    } else if (item->kind == Kind::Cover && item->cover.stop && can_act(*item)) {
      // A cover with a single control: a press stops it.
      act_now(*item, "stop", Json::object(), now);
    }
  }
}

void Controller::long_press(Millis now) {
  click_ticks_ = 0;
  switch (screen_) {
    case RoomView::Screen::Item:
      screen_ = RoomView::Screen::Home;
      changed();
      return;
    case RoomView::Screen::Settings:
      leave_settings();
      return;
    case RoomView::Screen::Confirm:
    case RoomView::Screen::About:
      screen_ = RoomView::Screen::Settings;
      changed();
      return;
    case RoomView::Screen::Empty:
    case RoomView::Screen::Home:
    case RoomView::Screen::Notifications:
    case RoomView::Screen::Notice:
      break;
  }
  const Item* item = selected_item();
  if (!item || screen_ != RoomView::Screen::Home || !can_act(*item)) return;
  if (item->kind == Kind::Light || item->kind == Kind::Switch) toggle(*item, now);
  if (item->kind == Kind::Scene) act_now(*item, "activate", Json::object(), now);
}

void Controller::tick(Millis now) {
  for (auto& [channel, p] : pending_) {
    if (!p.due) continue;
    const bool ready = p.settle ? elapsed(now, p.changed) >= kSettleDelay
                                 : elapsed(now, p.sent) >= kLiveInterval;
    if (ready) {
      send_(p.intent);
      p.due = false;
      p.sent = now;
      p.ever_sent = true;
    }
  }
  bool released = false;
  for (auto it = hold_until_.begin(); it != hold_until_.end();) {
    if (now >= it->second) {
      shown_.erase(it->first);
      it = hold_until_.erase(it);
      released = true;
    } else {
      ++it;
    }
  }
  if (released) changed();
}

void Controller::act_now(const Item& item, const std::string& action, Json args, Millis /*now*/) {
  send_(Intent{item.key, action, std::move(args)});
}

void Controller::act_later(const Item& item, const std::string& channel,
                           const std::string& action, Json args, bool settle, Millis now) {
  Pending& p = pending_[item.key + "/" + channel];
  p.intent = Intent{item.key, action, std::move(args)};
  p.settle = settle;
  p.due = true;
  p.changed = now;
  // The first turn of a live control goes out at once; later ones are paced.
  if (!settle && (!p.ever_sent || elapsed(now, p.sent) >= kLiveInterval)) {
    send_(p.intent);
    p.due = false;
    p.sent = now;
    p.ever_sent = true;
  }
}

void Controller::show(const std::string& key, const ItemState& state, Millis now) {
  shown_[key] = state;
  hold_until_[key] = now + kHold;
  changed();
}

void Controller::toggle(const Item& item, Millis now) {
  ItemState s = shown(item.key);
  s.on = !s.on;
  // A switch overrides any brightness or colour still waiting to be sent.
  for (auto it = pending_.begin(); it != pending_.end();) {
    it = it->second.intent.key == item.key ? pending_.erase(it) : std::next(it);
  }
  show(item.key, s, now);
  act_now(item, s.on ? "turn_on" : "turn_off", Json::object(), now);
}

void Controller::adjust(const Item& item, Control control, int steps, Millis now) {
  // Smooth controls get ticks: a click (kTicksPerClick of them) moves 5%,
  // 100 K or 10 degrees. The rest get clicks.
  ItemState s = shown(item.key);
  const auto& c = item.climate;

  switch (control) {
    case Control::Power: {
      const bool on = steps > 0;
      if (s.on == on) return;
      s.on = on;
      show(item.key, s, now);
      act_now(item, on ? "turn_on" : "turn_off", Json::object(), now);
      return;
    }
    case Control::Brightness: {
      const int current = s.on ? s.brightness : 0;
      const int next = std::clamp(current + steps, 0, 100);
      if (next == current) return;
      s.on = next > 0;
      if (s.on) s.brightness = next;
      show(item.key, s, now);
      act_later(item, "level", s.on ? "turn_on" : "turn_off",
                s.on ? Json{{"brightness", next}} : Json::object(), false, now);
      return;
    }
    case Control::ColorTemp: {
      const auto [lo, hi] = *item.light.color_temp;
      const int current = s.color_temp_k.value_or((lo + hi) / 2);
      const int next = std::clamp(current + 20 * steps, lo, hi);
      if (next == current && s.color_temp_k) return;
      s.on = true;
      s.color_temp_k = next;
      show(item.key, s, now);
      act_later(item, "colour", "turn_on", Json{{"color_temp_k", next}}, false, now);
      return;
    }
    case Control::Hue: {
      const double hue = s.hs ? s.hs->first : 0;
      const double saturation = s.hs && s.hs->second >= 1 ? s.hs->second : 100;
      const double next = std::fmod(std::fmod(hue + 2.0 * steps, 360.0) + 360.0, 360.0);
      s.on = true;
      s.hs = std::make_pair(next, saturation);
      show(item.key, s, now);
      act_later(item, "colour", "turn_on", Json{{"hs", {next, saturation}}}, false, now);
      return;
    }
    case Control::Effect: {
      const auto next = step_choice(item.light.effects, s.effect.value_or(""), steps);
      if (!next) return;
      s.on = true;
      s.effect = *next;
      show(item.key, s, now);
      act_later(item, "effect", "turn_on", Json{{"effect", *next}}, false, now);
      return;
    }
    case Control::Target: {
      const double current = s.target.value_or(snap(s.current.value_or((c.min + c.max) / 2), c));
      const double next = snap(current + c.step * steps, c);
      if (next == current && s.target) return;
      s.target = next;
      show(item.key, s, now);
      act_later(item, "target", "set_target", Json{{"value", next}}, true, now);
      return;
    }
    case Control::TargetLow:
    case Control::TargetHigh: {
      double low = s.target_low.value_or(c.min);
      double high = s.target_high.value_or(c.max);
      if (control == Control::TargetLow) {
        low = std::min(snap(low + c.step * steps, c), high);
      } else {
        high = std::max(snap(high + c.step * steps, c), low);
      }
      s.target_low = low;
      s.target_high = high;
      show(item.key, s, now);
      act_later(item, "range", "set_range", Json{{"low", low}, {"high", high}}, true, now);
      return;
    }
    case Control::HvacMode: {
      const auto next = step_choice(c.hvac_modes, s.hvac_mode, steps);
      if (!next) return;
      s.hvac_mode = *next;
      show(item.key, s, now);
      act_later(item, "mode", "set_hvac_mode", Json{{"mode", *next}}, true, now);
      return;
    }
    case Control::Preset: {
      const auto next = step_choice(c.presets, s.preset.value_or(""), steps);
      if (!next) return;
      s.preset = *next;
      show(item.key, s, now);
      act_later(item, "preset", "set_preset", Json{{"preset", *next}}, true, now);
      return;
    }
    case Control::FanMode: {
      const auto next = step_choice(c.fan_modes, s.fan_mode.value_or(""), steps);
      if (!next) return;
      s.fan_mode = *next;
      show(item.key, s, now);
      act_later(item, "fan", "set_fan_mode", Json{{"fan_mode", *next}}, true, now);
      return;
    }
    case Control::Position:
    case Control::Tilt: {
      const bool tilt = control == Control::Tilt;
      std::optional<int>& value = tilt ? s.tilt : s.position;
      const int current = value.value_or(!tilt && s.cover_state == "open" ? 100 : 0);
      const int next = std::clamp(current + steps, 0, 100);
      if (next == current && value) return;
      value = next;
      show(item.key, s, now);
      act_later(item, tilt ? "tilt" : "position", tilt ? "set_tilt" : "set_position",
                Json{{tilt ? "tilt" : "position", next}}, false, now);
      return;
    }
    case Control::CoverMove: {
      const bool open = steps > 0;
      if ((open && !item.cover.open) || (!open && !item.cover.close)) return;
      s.cover_state = open ? "opening" : "closing";
      show(item.key, s, now);
      act_now(item, open ? "open" : "close", Json::object(), now);
      return;
    }
  }
}

RoomView Controller::view() const {
  RoomView v;
  v.offline = !connected_;
  v.screen = screen_;

  switch (screen_) {
    case RoomView::Screen::Settings:
      v.title = "Settings";
      for (int i = 0; i < kSettingsCount; ++i) {
        v.entries.push_back({kSettings[i], kSettingsSummaries[i], true, kSettingsIcons[i],
                             "settings", Tone::Neutral});
      }
      v.selected = setting_;
      return v;
    case RoomView::Screen::Confirm:
      v.title = "Settings";
      v.item_name = "Forget Home Assistant?";
      v.prompt = "Press to forget it,\nhold to keep it";
      return v;
    case RoomView::Screen::About:
      v.title = "About";
      if (actions_.about) v.lines = actions_.about();
      return v;
    case RoomView::Screen::Empty:
    case RoomView::Screen::Home:
    case RoomView::Screen::Item:
    case RoomView::Screen::Notifications:
    case RoomView::Screen::Notice:
      break;
  }

  if (!room_.has_layout()) return v;
  v.title = room_.layout().title;
  v.selected = selected_;
  if (notices_entry()) {
    const std::string summary = notification_unread_ > 0 ? std::to_string(notification_unread_) + " unread"
                                                         : std::to_string(notification_count_) + " kept";
    v.entries.push_back({"Notifications", summary, true, "bell", "bell",
                         notification_unread_ > 0 ? Tone::Heat : Tone::Neutral});
  }
  for (const Item& item : room_.layout().items) {
    const ItemState s = shown(item.key);
    const char* fallback = default_icon(item.kind);
    v.entries.push_back({item.name, summary(item, s), s.available,
                         item.icon.empty() ? fallback : item.icon, fallback, tone_for(item, s)});
  }
  v.entries.push_back({"Settings", "Pairing and about", true, "settings", "settings",
                       Tone::Neutral});

  const Item* item = open_item();
  if (!item) return v;
  const ItemState s = shown(item->key);
  const auto controls = controls_for(*item);
  v.item_name = item->name;
  v.default_icon = default_icon(item->kind);
  v.icon = item->icon.empty() ? v.default_icon : item->icon;
  v.tone = tone_for(*item, s);
  v.available = s.available;
  v.control_count = static_cast<int>(controls.size());
  v.control_index = control_;
  if (controls.empty()) return v;

  const Control control = controls[static_cast<std::size_t>(control_)];
  const auto& c = item->climate;
  v.control = label(control);
  switch (control) {
    case Control::Power:
      v.value = s.on ? "On" : "Off";
      v.fraction = s.on ? 1 : 0;
      break;
    case Control::Brightness:
      v.value = s.on ? std::to_string(s.brightness) + "%" : "Off";
      v.fraction = s.on ? s.brightness / 100.0 : 0;
      break;
    case Control::ColorTemp: {
      const auto [lo, hi] = *item->light.color_temp;
      v.value = s.color_temp_k ? std::to_string(*s.color_temp_k) + " K" : "-";
      v.fraction = s.color_temp_k ? fraction_of(*s.color_temp_k, lo, hi) : -1;
      break;
    }
    case Control::Hue:
      v.value = s.hs ? "Hue " + std::to_string(static_cast<int>(s.hs->first)) : "-";
      v.fraction = s.hs ? s.hs->first / 360.0 : -1;
      break;
    case Control::Effect: {
      v.value = s.effect.value_or("None");
      const int at = index_of(item->light.effects, v.value);
      const auto n = item->light.effects.size();
      v.fraction = at >= 0 && n > 1 ? static_cast<double>(at) / static_cast<double>(n - 1) : -1;
      break;
    }
    case Control::Target:
      v.value = s.target ? format(*s.target, c.step) + c.unit : "-";
      v.fraction = s.target ? fraction_of(*s.target, c.min, c.max) : -1;
      break;
    case Control::TargetLow:
      v.value = s.target_low ? format(*s.target_low, c.step) + c.unit : "-";
      v.fraction = s.target_low ? fraction_of(*s.target_low, c.min, c.max) : -1;
      break;
    case Control::TargetHigh:
      v.value = s.target_high ? format(*s.target_high, c.step) + c.unit : "-";
      v.fraction = s.target_high ? fraction_of(*s.target_high, c.min, c.max) : -1;
      break;
    case Control::HvacMode:
      v.value = s.hvac_mode.empty() ? "-" : capitalised(s.hvac_mode);
      break;
    case Control::Preset:
      v.value = s.preset ? capitalised(*s.preset) : "-";
      break;
    case Control::FanMode:
      v.value = s.fan_mode ? capitalised(*s.fan_mode) : "-";
      break;
    case Control::Position:
      v.value = s.position ? std::to_string(*s.position) + "%" : "-";
      v.fraction = s.position ? *s.position / 100.0 : -1;
      break;
    case Control::Tilt:
      v.value = s.tilt ? std::to_string(*s.tilt) + "%" : "-";
      v.fraction = s.tilt ? *s.tilt / 100.0 : -1;
      break;
    case Control::CoverMove:
      v.value = s.cover_state.empty() ? "-" : capitalised(s.cover_state);
      break;
  }
  return v;
}

}  // namespace nesto::room
