#include "room/DisplayPower.hpp"

#include <algorithm>

namespace nesto::room {
namespace {

const protocol::Json& section(const protocol::Json& config, const char* key) {
  static const protocol::Json kEmpty = protocol::Json::object();
  if (!config.is_object()) return kEmpty;
  const auto it = config.find(key);
  return it != config.end() && it->is_object() ? *it : kEmpty;
}

int percent(const protocol::Json& object, const char* key, int fallback) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number()) return fallback;
  const double value = it->get<double>();
  return value >= 0 && value <= 100 ? static_cast<int>(value + 0.5) : fallback;
}

bool flag(const protocol::Json& object, const char* key, bool fallback) {
  const auto it = object.find(key);
  return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

const char* name(StandbyMode mode) {
  switch (mode) {
    case StandbyMode::Clock: return "clock";
    case StandbyMode::Carousel: return "carousel";
    case StandbyMode::Off: return "off";
  }
  return "clock";
}

}  // namespace

DisplayConfig DisplayConfig::from_config(const protocol::Json& config) {
  DisplayConfig out;
  const protocol::Json& display = section(config, "display");
  out.brightness = percent(display, "brightness", out.brightness);
  out.auto_brightness = flag(display, "auto_brightness", out.auto_brightness);
  if (const auto it = display.find("timeout_s");
      it != display.end() && it->is_number() && it->get<double>() >= 0) {
    out.timeout_s = static_cast<unsigned>(std::min(it->get<double>(), 86400.0));
  }
  out.wake_on_motion = flag(display, "wake_on_motion", out.wake_on_motion);
  out.wake_on_input_when_off = flag(display, "wake_on_input_when_off", out.wake_on_input_when_off);

  const protocol::Json& standby = section(config, "standby");
  if (const auto it = standby.find("mode"); it != standby.end() && it->is_string()) {
    const std::string mode = it->get<std::string>();
    if (mode == "clock") out.standby_mode = StandbyMode::Clock;
    if (mode == "carousel") out.standby_mode = StandbyMode::Carousel;
    if (mode == "off") out.standby_mode = StandbyMode::Off;
  }
  out.standby_brightness = percent(standby, "brightness", out.standby_brightness);
  return out;
}

protocol::Json DisplayConfig::to_config() const {
  return protocol::Json{{"display",
                         {{"brightness", brightness},
                          {"auto_brightness", auto_brightness},
                          {"timeout_s", timeout_s},
                          {"wake_on_motion", wake_on_motion},
                          {"wake_on_input_when_off", wake_on_input_when_off}}},
                        {"standby", {{"mode", name(standby_mode)}, {"brightness", standby_brightness}}}};
}

bool DisplayConfig::operator==(const DisplayConfig& o) const {
  return brightness == o.brightness && auto_brightness == o.auto_brightness &&
         timeout_s == o.timeout_s && wake_on_motion == o.wake_on_motion &&
         wake_on_input_when_off == o.wake_on_input_when_off && standby_mode == o.standby_mode &&
         standby_brightness == o.standby_brightness;
}

const char* name(DisplayState state) {
  switch (state) {
    case DisplayState::Active: return "active";
    case DisplayState::Standby: return "standby";
    case DisplayState::Off: return "off";
  }
  return "active";
}

const char* name(DisplayCause cause) {
  switch (cause) {
    case DisplayCause::Input: return "input";
    case DisplayCause::Motion: return "motion";
    case DisplayCause::Timeout: return "timeout";
    case DisplayCause::Ha: return "ha";
    case DisplayCause::Notification: return "notification";
  }
  return "input";
}

std::optional<DisplayState> parse_display_state(const std::string& text) {
  if (text == "active") return DisplayState::Active;
  if (text == "standby") return DisplayState::Standby;
  if (text == "off") return DisplayState::Off;
  return std::nullopt;
}

DisplayPower::DisplayPower(Millis now, bool has_standby_screen)
    : has_standby_screen_(has_standby_screen), last_activity_(now) {}

void DisplayPower::configure(const DisplayConfig& config, Millis now) {
  const int before = brightness();
  config_ = config;
  // A new timeout counts from now; a new brightness shows at once.
  last_activity_ = now;
  if (brightness() != before) change_ = Change{state_, brightness(), DisplayCause::Ha};
}

DisplayState DisplayPower::resting_state() const {
  return config_.standby_mode == StandbyMode::Off ? DisplayState::Off : DisplayState::Standby;
}

int DisplayPower::brightness() const {
  switch (state_) {
    case DisplayState::Active: return config_.brightness;
    // Until there's a clock to show, standby is dark, but still wakes on motion.
    case DisplayState::Standby: return has_standby_screen_ ? config_.standby_brightness : 0;
    case DisplayState::Off: return 0;
  }
  return 0;
}

void DisplayPower::enter(DisplayState state, DisplayCause cause, Millis now) {
  last_activity_ = now;
  if (state == state_) return;
  state_ = state;
  change_ = Change{state_, brightness(), cause};
}

bool DisplayPower::input(Millis now) {
  switch (state_) {
    case DisplayState::Active:
      last_activity_ = now;
      return true;
    case DisplayState::Standby:
      enter(DisplayState::Active, DisplayCause::Input, now);
      return false;
    case DisplayState::Off:
      if (config_.wake_on_input_when_off) enter(DisplayState::Active, DisplayCause::Input, now);
      return false;
  }
  return false;
}

void DisplayPower::motion(Millis now) {
  if (!config_.wake_on_motion) return;
  if (state_ == DisplayState::Active) {
    last_activity_ = now;  // someone is still there
  } else if (state_ == DisplayState::Standby) {
    enter(DisplayState::Active, DisplayCause::Motion, now);
  }
}

void DisplayPower::set(DisplayState state, Millis now) { enter(state, DisplayCause::Ha, now); }

void DisplayPower::tick(Millis now) {
  if (state_ != DisplayState::Active || config_.timeout_s == 0) return;
  if (now - last_activity_ >= static_cast<Millis>(config_.timeout_s) * 1000) {
    enter(resting_state(), DisplayCause::Timeout, now);
  }
}

std::optional<DisplayPower::Change> DisplayPower::take_change() {
  std::optional<Change> out;
  out.swap(change_);
  return out;
}

void BacklightFade::go(int target, Millis now) {
  from_ = level(now);
  to_ = target;
  start_ = now;
}

int BacklightFade::level(Millis now) const {
  if (to_ >= from_) return to_;  // up: at once
  const Millis elapsed = now > start_ ? now - start_ : 0;
  if (elapsed >= kDuration) return to_;
  // Linear, rounded up, so it reaches the target only at the end.
  const Millis left = kDuration - elapsed;
  const Millis span = static_cast<Millis>(from_ - to_) * left;
  return to_ + static_cast<int>((span + kDuration - 1) / kDuration);
}

}  // namespace nesto::room
