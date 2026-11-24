#include "room/Room.hpp"

#include <algorithm>
#include <set>
#include <type_traits>

namespace nesto::room {
namespace {

const std::pair<const char*, Kind> kKinds[] = {
    {"light", Kind::Light},     {"switch", Kind::Switch}, {"scene", Kind::Scene},
    {"climate", Kind::Climate}, {"cover", Kind::Cover},   {"sensor", Kind::Sensor},
    {"fan", Kind::Fan},         {"lock", Kind::Lock},     {"media_player", Kind::MediaPlayer},
};

std::optional<Kind> kind_from(const std::string& name) {
  for (const auto& [text, kind] : kKinds) {
    if (name == text) return kind;
  }
  return std::nullopt;
}

bool flag(const Json& features, const char* name) {
  const auto it = features.find(name);
  return it != features.end() && it->is_boolean() && it->get<bool>();
}

std::string text(const Json& object, const char* name) {
  const auto it = object.find(name);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool strings(const Json& object, const char* name, std::vector<std::string>& out,
             std::string& error) {
  out.clear();
  const auto it = object.find(name);
  if (it == object.end() || it->is_null()) return true;
  if (!it->is_array() || it->size() > kMaxChoices) {
    error = std::string("`") + name + "` must be a list of at most 32 strings";
    return false;
  }
  for (const Json& s : *it) {
    if (!s.is_string()) {
      error = std::string("`") + name + "` must hold strings";
      return false;
    }
    out.push_back(s.get<std::string>());
  }
  return true;
}

double number(const Json& object, const char* name, double fallback) {
  const auto it = object.find(name);
  return it != object.end() && it->is_number() ? it->get<double>() : fallback;
}

template <typename T>
void optional_field(const Json& fields, const char* name, std::optional<T>& out) {
  const auto it = fields.find(name);
  if (it == fields.end()) return;
  if (it->is_null()) {
    out.reset();
    return;
  }
  if constexpr (std::is_same_v<T, std::string>) {
    if (it->is_string()) out = it->get<std::string>();
  } else if constexpr (std::is_same_v<T, int>) {
    if (it->is_number()) out = static_cast<int>(it->get<double>() + 0.5);
  } else {
    if (it->is_number()) out = it->get<double>();
  }
}

}  // namespace

std::vector<std::string> kind_names() {
  std::vector<std::string> out;
  for (const auto& entry : kKinds) out.emplace_back(entry.first);
  return out;
}

std::optional<Layout> parse_layout(const Json& message, std::string& error) {
  Layout layout;
  const auto rev = message.find("rev");
  if (rev == message.end() || !rev->is_number_integer()) {
    error = "`rev` must be an integer";
    return std::nullopt;
  }
  layout.rev = rev->get<long long>();
  layout.title = text(message, "title").substr(0, kMaxName);
  if (const auto wake = message.find("wake"); wake != message.end() && !wake->is_null()) {
    if (!wake->is_string()) {
      error = "`wake` must be an item's key";
      return std::nullopt;
    }
    layout.wake = wake->get<std::string>();  // one that's no item is ignored
  }

  const auto items = message.find("items");
  if (items == message.end() || !items->is_array()) {
    error = "`items` must be a list";
    return std::nullopt;
  }
  if (items->size() > kMaxItems) {
    error = "more than 48 items";
    return std::nullopt;
  }

  std::set<std::string> keys;
  for (const Json& entry : *items) {
    if (!entry.is_object()) {
      error = "every item must be an object";
      return std::nullopt;
    }
    Item item;
    item.key = text(entry, "key");
    if (item.key.empty() || item.key.size() > 8 || !keys.insert(item.key).second) {
      error = "item keys must be unique, 1 to 8 characters";
      return std::nullopt;
    }
    const auto kind = kind_from(text(entry, "kind"));
    if (!kind) continue;  // a newer kind this panel doesn't know
    item.kind = *kind;
    item.name = text(entry, "name").substr(0, kMaxName);
    item.icon = text(entry, "icon");

    const auto f = entry.find("features");
    const Json features = f != entry.end() && f->is_object() ? *f : Json::object();
    switch (item.kind) {
      case Kind::Light: {
        item.light.brightness = flag(features, "brightness");
        item.light.color = flag(features, "color");
        const auto ct = features.find("color_temp");
        if (ct != features.end() && ct->is_object()) {
          const int lo = static_cast<int>(number(*ct, "min_k", 0));
          const int hi = static_cast<int>(number(*ct, "max_k", 0));
          if (lo > 0 && hi > lo) item.light.color_temp = std::make_pair(lo, hi);
        }
        if (!strings(features, "effects", item.light.effects, error)) return std::nullopt;
        break;
      }
      case Kind::Climate: {
        auto& c = item.climate;
        const std::string target = text(features, "target");
        c.target = target == "single"  ? ClimateFeatures::Target::Single
                   : target == "range" ? ClimateFeatures::Target::Range
                                       : ClimateFeatures::Target::None;
        c.min = number(features, "min", c.min);
        c.max = number(features, "max", c.max);
        c.step = number(features, "step", c.step);
        if (c.step <= 0) c.step = 0.5;
        if (c.max < c.min) std::swap(c.min, c.max);
        c.unit = text(features, "unit");
        if (!strings(features, "hvac_modes", c.hvac_modes, error) ||
            !strings(features, "presets", c.presets, error) ||
            !strings(features, "fan_modes", c.fan_modes, error)) {
          return std::nullopt;
        }
        break;
      }
      case Kind::Cover:
        item.cover = {flag(features, "open"), flag(features, "close"), flag(features, "stop"),
                      flag(features, "position"), flag(features, "tilt")};
        break;
      case Kind::Sensor:
        item.unit = text(features, "unit");
        break;
      case Kind::Fan: {
        auto& fan = item.fan;
        fan.speed = flag(features, "speed");
        fan.speed_step = number(features, "speed_step", 1);
        if (fan.speed_step < 1 || fan.speed_step > 100) fan.speed_step = 1;
        fan.oscillate = flag(features, "oscillate");
        fan.direction = flag(features, "direction");
        if (!strings(features, "presets", fan.presets, error)) return std::nullopt;
        break;
      }
      case Kind::Lock:
        item.lock.open = flag(features, "open");
        break;
      case Kind::MediaPlayer: {
        auto& m = item.media;
        m.volume = flag(features, "volume");
        m.mute = flag(features, "mute");
        m.play_pause = flag(features, "play_pause");
        m.tracks = flag(features, "tracks");
        m.power = flag(features, "power");
        if (!strings(features, "sources", m.sources, error)) return std::nullopt;
        break;
      }
      case Kind::Switch:
      case Kind::Scene:
        break;
    }
    layout.items.push_back(std::move(item));
  }
  return layout;
}

void merge_state(ItemState& s, const Json& fields) {
  if (!fields.is_object()) return;
  auto boolean = [&](const char* name, bool& out) {
    const auto it = fields.find(name);
    if (it != fields.end() && it->is_boolean()) out = it->get<bool>();
  };
  auto string = [&](const char* name, std::string& out) {
    const auto it = fields.find(name);
    if (it != fields.end() && it->is_string()) out = it->get<std::string>();
  };

  boolean("available", s.available);
  boolean("on", s.on);
  if (const auto it = fields.find("brightness"); it != fields.end() && it->is_number()) {
    s.brightness = std::clamp(static_cast<int>(it->get<double>() + 0.5), 0, 100);
  }
  optional_field(fields, "color_temp_k", s.color_temp_k);
  if (const auto it = fields.find("hs"); it != fields.end()) {
    if (it->is_array() && it->size() == 2 && (*it)[0].is_number() && (*it)[1].is_number()) {
      s.hs = std::make_pair((*it)[0].get<double>(), (*it)[1].get<double>());
    } else if (it->is_null()) {
      s.hs.reset();
    }
  }
  optional_field(fields, "effect", s.effect);

  string("hvac_mode", s.hvac_mode);
  optional_field(fields, "hvac_action", s.hvac_action);
  optional_field(fields, "current", s.current);
  optional_field(fields, "target", s.target);
  optional_field(fields, "target_low", s.target_low);
  optional_field(fields, "target_high", s.target_high);
  optional_field(fields, "preset", s.preset);
  optional_field(fields, "fan_mode", s.fan_mode);
  optional_field(fields, "current_humidity", s.current_humidity);

  string("state", s.state);
  if (const auto it = fields.find("state"); it != fields.end() && it->is_null()) s.state.clear();
  optional_field(fields, "position", s.position);
  optional_field(fields, "tilt", s.tilt);

  optional_field(fields, "percentage", s.percentage);
  if (s.percentage) s.percentage = std::clamp(*s.percentage, 0, 100);
  boolean("oscillating", s.oscillating);
  string("direction", s.direction);

  optional_field(fields, "volume", s.volume);
  if (s.volume) s.volume = std::clamp(*s.volume, 0, 100);
  boolean("muted", s.muted);
  optional_field(fields, "title", s.title);
  optional_field(fields, "artist", s.artist);
  optional_field(fields, "source", s.source);

  string("value", s.value);
}

bool Room::apply_layout(const Json& message, std::string& error) {
  auto layout = parse_layout(message, error);
  if (!layout) return false;
  layout_ = std::move(*layout);
  has_layout_ = true;
  std::map<std::string, ItemState> kept;
  for (const Item& item : layout_.items) {
    if (const auto it = states_.find(item.key); it != states_.end()) kept.insert(*it);
  }
  states_ = std::move(kept);
  return true;
}

std::vector<std::string> Room::apply_states(const Json& message) {
  std::vector<std::string> changed;
  const bool full = message.value("full", false);
  if (full) states_.clear();
  const auto items = message.find("items");
  if (items == message.end() || !items->is_object()) return changed;
  for (const auto& [key, fields] : items->items()) {
    if (!find(key)) continue;
    merge_state(states_[key], fields);
    changed.push_back(key);
  }
  return changed;
}

const Item* Room::find(const std::string& key) const {
  for (const Item& item : layout_.items) {
    if (item.key == key) return &item;
  }
  return nullptr;
}

const ItemState& Room::state(const std::string& key) const {
  static const ItemState kUnknown{};
  const auto it = states_.find(key);
  return it != states_.end() ? it->second : kUnknown;
}

}  // namespace nesto::room
