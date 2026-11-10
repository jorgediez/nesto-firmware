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

/// What colour an item's state is drawn in.
enum class Tone {
  Off,      // off, closed, unavailable
  Neutral,  // nothing to signal: scenes, sensors, idle climate, menus
  Light,    // a light that's on
  Heat,     // climate heating, or set to heat
  Cool,     // climate cooling, or set to cool
  Active,   // a switch that's on, an open cover
};

Tone tone_for(const Item& item, const ItemState& state);

/// The icon an item gets when Home Assistant sends none (PROTOCOL.md appendix C).
const char* default_icon(Kind kind);

/// An `intent` for Home Assistant (PROTOCOL.md section 11), without its id.
struct Intent {
  std::string key;
  std::string action;
  Json args = Json::object();
};

/// Everything the screens need to draw, rebuilt after each change.
struct RoomView {
  enum class Screen { Empty, Home, Item, Settings, Confirm, About };
  struct Entry {
    std::string name;
    std::string summary;
    bool available = true;
    std::string icon;          // as Home Assistant named it, or the kind's default
    std::string default_icon;  // if the panel has no icon by that name
    Tone tone = Tone::Neutral;
  };

  Screen screen = Screen::Empty;
  bool offline = true;
  std::string title;

  // Home
  std::vector<Entry> entries;
  int selected = 0;

  // Item; also the question on Confirm
  std::string item_name;
  std::string icon;
  std::string default_icon;
  Tone tone = Tone::Neutral;
  std::string control;  // what the ring changes, e.g. "Brightness"
  std::string value;    // its value, e.g. "60%"
  double fraction = -1;  // 0 to 1 for a gauge; negative for none
  int control_index = 0;
  int control_count = 0;
  bool available = true;

  // Confirm: how to answer. About: the lines to show.
  std::string prompt;
  std::vector<std::string> lines;
};

/// What the Settings menu asks of the rest of the panel.
struct SettingsActions {
  std::function<void()> open_pairing;  // Pair with Home Assistant
  std::function<void()> forget;        // Forget Home Assistant, once confirmed
  std::function<std::vector<std::string>()> about;
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
///
/// The last entry on the home screen, and a press on the waiting screen, open
/// Settings: pairing, forgetting Home Assistant (after a confirmation) and
/// About. A long press goes back from any of them.
class Controller {
 public:
  static constexpr Millis kLiveInterval = 300;
  static constexpr Millis kSettleDelay = 1500;
  static constexpr Millis kHold = 2000;
  static constexpr Millis kClickRest = 800;

  using Send = std::function<void(const Intent&)>;

  explicit Controller(Send send, SettingsActions actions = {});

  void set_connected(bool connected);
  bool apply_layout(const Json& message, std::string& error);
  void apply_states(const Json& message);
  /// Home Assistant couldn't carry out an intent: show its state again.
  void intent_failed(const std::string& key);

  /// The ring turned by `ticks` (core/Input.hpp). Smooth values, brightness,
  /// colour, and a cover's position and tilt, change with every tick. The
  /// rest moves a place per click: the home screen, Settings, lists of
  /// choices, switching, and climate targets. Ticks short of a click wait for
  /// the next turn; they are dropped when the ring turns back, rests for
  /// kClickRest, or a press changes what it controls.
  void rotate(int ticks, Millis now);
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

  void open_settings();
  void leave_settings();
  int home_count() const;
  const Item* open_item() const;
  const Item* selected_item() const;
  ItemState shown(const std::string& key) const;
  bool can_act(const Item& item) const;

  void act_now(const Item& item, const std::string& action, Json args, Millis now);
  void act_later(const Item& item, const std::string& channel, const std::string& action,
                 Json args, bool settle, Millis now);
  void show(const std::string& key, const ItemState& state, Millis now);
  void toggle(const Item& item, Millis now);
  /// Adds ticks towards whole clicks; returns the clicks they complete.
  int clicks(int ticks, Millis now);
  /// `steps` are ticks for smooth controls, clicks for the rest.
  void adjust(const Item& item, Control control, int steps, Millis now);
  void changed() { ++version_; }

  Send send_;
  SettingsActions actions_;
  Room room_;
  bool connected_ = false;

  RoomView::Screen screen_ = RoomView::Screen::Empty;
  int selected_ = 0;
  std::string open_key_;
  int control_ = 0;
  int setting_ = 0;  // the selected Settings entry
  int click_ticks_ = 0;  // ticks towards the next click, with their sign
  Millis last_turn_ = 0;

  std::map<std::string, ItemState> shown_;  // values set on the panel, ahead of HA
  std::map<std::string, Millis> hold_until_;
  std::map<std::string, Pending> pending_;  // by item key and channel
  std::optional<std::string> message_;
  unsigned version_ = 1;
};

}  // namespace nesto::room
