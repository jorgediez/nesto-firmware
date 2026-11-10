#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/Input.hpp"
#include "room/Room.hpp"

namespace nesto::room {

/// What a turn of the ring changes on an item's screen.
enum class Control {
  Power,
  Brightness,
  ColorTemp,
  Hue,
  Effect,
  Target,
  TargetLow,
  TargetHigh,
  HvacMode,
  Preset,
  FanMode,
  Position,
  Tilt,
  CoverMove,
};

/// The controls an item's screen offers, in the order a press steps through.
std::vector<Control> controls_for(const Item& item);

/// A short description of an item's state, for the home screen.
std::string summary(const Item& item, const ItemState& state);

/// An `intent` for Home Assistant (PROTOCOL.md section 11), without its id.
struct Intent {
  std::string key;
  std::string action;
  Json args = Json::object();
};

/// Everything the screens need to draw, rebuilt after each change.
struct RoomView {
  enum class Screen { Empty, Home, Item };
  struct Entry {
    std::string name;
    std::string summary;
    bool available = true;
  };

  Screen screen = Screen::Empty;
  bool offline = true;
  std::string title;

  // Home
  std::vector<Entry> entries;
  int selected = 0;

  // Item
  std::string item_name;
  std::string control;  // what the ring changes, e.g. "Brightness"
  std::string value;    // its value, e.g. "60%"
  double fraction = -1;  // 0 to 1 for a gauge; negative for none
  int control_index = 0;
  int control_count = 0;
  bool available = true;
};

/// The panel's controls for a room: which screen is shown, what the ring and
/// press do on it, and the intents that result, sent at the pace PROTOCOL.md
/// section 23 sets:
///
/// - lights and covers follow the ring live, at most one intent per 300 ms
///   while it turns and the last value within 300 ms of it stopping;
/// - climate sends once, 1.5 seconds after the ring stops;
/// - switching on or off, scenes and opening or closing are sent at once.
///
/// Values change on screen straight away. Home Assistant's states take over
/// again 2 seconds after the last turn, or at once if an intent fails.
class Controller {
 public:
  static constexpr Millis kLiveInterval = 300;
  static constexpr Millis kSettleDelay = 1500;
  static constexpr Millis kHold = 2000;

  using Send = std::function<void(const Intent&)>;

  explicit Controller(Send send);

  void set_connected(bool connected);
  bool apply_layout(const Json& message, std::string& error);
  void apply_states(const Json& message);
  /// Home Assistant couldn't carry out an intent: show its state again.
  void intent_failed(const std::string& key);

  void rotate(int steps, Millis now);
  void press(Millis now);
  void long_press(Millis now);
  /// Sends due intents and ends expired holds. Call regularly.
  void tick(Millis now);

  RoomView view() const;
  /// Increases whenever the view may have changed, so the screen knows to redraw.
  unsigned version() const { return version_; }
  /// A message to show briefly, set when an action fails; taken once.
  std::optional<std::string> take_message();

 private:
  struct Pending {
    Intent intent;
    bool settle = false;  // climate: wait for the ring to stop
    bool due = false;     // not yet sent
    Millis changed = 0;
    Millis sent = 0;
    bool ever_sent = false;
  };

  const Item* open_item() const;
  const Item* selected_item() const;
  ItemState shown(const std::string& key) const;
  bool can_act(const Item& item) const;

  void act_now(const Item& item, const std::string& action, Json args, Millis now);
  void act_later(const Item& item, const std::string& channel, const std::string& action,
                 Json args, bool settle, Millis now);
  void show(const std::string& key, const ItemState& state, Millis now);
  void toggle(const Item& item, Millis now);
  void adjust(const Item& item, Control control, int steps, Millis now);
  void changed() { ++version_; }

  Send send_;
  Room room_;
  bool connected_ = false;

  RoomView::Screen screen_ = RoomView::Screen::Empty;
  int selected_ = 0;
  std::string open_key_;
  int control_ = 0;

  std::map<std::string, ItemState> shown_;  // values set on the panel, ahead of HA
  std::map<std::string, Millis> hold_until_;
  std::map<std::string, Pending> pending_;  // by item key and channel
  std::optional<std::string> message_;
  unsigned version_ = 1;
};

}  // namespace nesto::room
