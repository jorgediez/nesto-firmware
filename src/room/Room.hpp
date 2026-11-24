#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "protocol/Message.hpp"

namespace nesto::room {

using protocol::Json;

/// Item kinds of PROTOCOL.md section 9.1.
enum class Kind { Light, Switch, Scene, Climate, Cover, Sensor, Fan, Lock, MediaPlayer };

/// The names of every kind this panel draws, for hello's `caps.kinds`.
std::vector<std::string> kind_names();

struct LightFeatures {
  bool brightness = false;
  std::optional<std::pair<int, int>> color_temp;  // min and max kelvin
  bool color = false;
  std::vector<std::string> effects;
};

struct ClimateFeatures {
  enum class Target { None, Single, Range };
  Target target = Target::None;
  double min = 7;
  double max = 35;
  double step = 0.5;
  std::string unit;
  std::vector<std::string> hvac_modes;
  std::vector<std::string> presets;
  std::vector<std::string> fan_modes;
};

struct CoverFeatures {
  bool open = false;
  bool close = false;
  bool stop = false;
  bool position = false;
  bool tilt = false;
};

struct FanFeatures {
  bool speed = false;
  double speed_step = 1;  // percent between speeds: 33.3 for three
  std::vector<std::string> presets;
  bool oscillate = false;
  bool direction = false;
};

struct LockFeatures {
  bool open = false;  // it can open the latch, not only unlock
};

struct MediaFeatures {
  bool volume = false;
  bool mute = false;
  bool play_pause = false;
  bool tracks = false;  // next and previous
  std::vector<std::string> sources;
  bool power = false;
};

/// One thing on the panel (section 9). Only the features for its kind are set.
struct Item {
  std::string key;
  Kind kind = Kind::Switch;
  std::string name;
  std::string icon;
  LightFeatures light;
  ClimateFeatures climate;
  CoverFeatures cover;
  FanFeatures fan;
  LockFeatures lock;
  MediaFeatures media;
  std::string unit;  // sensors
};

struct Layout {
  long long rev = 0;
  std::string title;
  std::vector<Item> items;
};

/// Section 10's fields, for every kind; each kind uses its own.
struct ItemState {
  bool available = true;
  // light, switch, fan
  bool on = false;
  int brightness = 0;
  std::optional<int> color_temp_k;
  std::optional<std::pair<double, double>> hs;  // hue 0-360, saturation 0-100
  std::optional<std::string> effect;
  // climate
  std::string hvac_mode;
  std::optional<std::string> hvac_action;
  std::optional<double> current;
  std::optional<double> target;
  std::optional<double> target_low;
  std::optional<double> target_high;
  std::optional<std::string> preset;  // climate, fan
  std::optional<std::string> fan_mode;
  std::optional<double> current_humidity;
  // cover: open, closed, opening, closing; lock: locked, unlocked, locking,
  // unlocking, jammed, open, opening; media player: off, on, idle, playing,
  // paused, standby, buffering
  std::string state;
  std::optional<int> position;
  std::optional<int> tilt;
  // fan
  std::optional<int> percentage;
  bool oscillating = false;
  std::string direction;
  // media player
  std::optional<int> volume;
  bool muted = false;
  std::optional<std::string> title;
  std::optional<std::string> artist;
  std::optional<std::string> source;
  // sensor
  std::string value;
};

/// Limits of section 21.
constexpr std::size_t kMaxItems = 48;
constexpr std::size_t kMaxName = 64;
constexpr std::size_t kMaxChoices = 32;

/// Reads a `layout` message. Items of unknown kinds are skipped, as section 9
/// says; anything malformed fails the whole layout, with the reason in `error`.
std::optional<Layout> parse_layout(const Json& message, std::string& error);

/// Merges an item's fields from a `states` message into `state`. Missing
/// fields keep their value; null clears optional ones.
void merge_state(ItemState& state, const Json& fields);

/// The layout and the state of every item, as Home Assistant last reported.
class Room {
 public:
  bool has_layout() const { return has_layout_; }
  const Layout& layout() const { return layout_; }

  /// Replaces the layout. States of items that are gone are dropped.
  bool apply_layout(const Json& message, std::string& error);

  /// Applies a `states` message: a full one replaces every state, a partial
  /// one merges into them. Keys not in the layout are ignored. Returns the keys
  /// that changed.
  std::vector<std::string> apply_states(const Json& message);

  const Item* find(const std::string& key) const;
  const ItemState& state(const std::string& key) const;

 private:
  bool has_layout_ = false;
  Layout layout_;
  std::map<std::string, ItemState> states_;
};

}  // namespace nesto::room
