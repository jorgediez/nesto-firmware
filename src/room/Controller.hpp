#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/Input.hpp"
#include "room/RingKeyboard.hpp"
#include "room/Room.hpp"
#include "room/TechInfo.hpp"

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
  Speed,
  Oscillate,
  Direction,
  Lock,
  Open,
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
  Active,   // a switch or fan that's on, an open cover, an unlocked lock
  Alert,    // a jammed lock
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
  // Notifications and Notice come from room/Notifications: the list, and one
  // notification, popped up or opened from the list.
  // WifiList, Keyboard and WifiBusy come from room/WifiSetup: networks (or a
  // hidden one's security), typing on the ring, and joining.
  enum class Screen {
    Empty,
    Home,
    Item,
    Settings,
    Confirm,
    Info,
    Notifications,
    Notice,
    WifiList,
    Keyboard,
    WifiBusy,
  };
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

  // Confirm and Notice: how to answer.
  std::string prompt;
  // Info: Technical info's entries; `scroll` is how many are scrolled past.
  std::vector<InfoLine> info;

  // Keyboard: the keys round the ring, `selected` the highlighted one, the
  // text typed in `value`, what it's for in `item_name`, and `icon`.
  std::vector<RingKeyboard::Key> keys;

  // Notice: the title is item_name, with icon and tone.
  std::string message;
  std::vector<std::string> choices;  // the actions, then Dismiss
  int choice = 0;
  int scroll = 0;  // lines of the message, or Info's entries, scrolled past
};

/// What the Settings menu asks of the rest of the panel.
struct SettingsActions {
  std::function<void()> open_pairing;  // Pair with Home Assistant
  std::function<void()> forget;        // Forget Home Assistant, once confirmed
  std::function<void()> open_info;     // Technical info opened: gather it (set_info)
  std::function<void()> open_wifi;     // Wi-Fi: the networks and joining one (room/WifiSetup)
  std::function<void()> open_notifications;  // the home screen's Notifications entry
};

/// The panel's controls for a room: which screen is shown, what the ring and
/// press do on it, and the intents that result, sent at the pace PROTOCOL.md
/// section 23 sets:
///
/// - lights and covers follow the ring live, at most one intent per 300 ms
///   while it turns and the last value within 300 ms of it stopping;
/// - climate sends once, 1.5 seconds after the ring stops;
/// - switching on or off, scenes, opening or closing, and locking are sent
///   at once; unlocking or opening a lock only once a press confirms it.
///
/// Values change on screen straight away. Home Assistant's states take over
/// again 2 seconds after the last turn, or at once if an intent fails.
///
/// Confirm asks before something that can't wait to be undone: a press does
/// it, a long press goes back to where it was asked.
///
/// The last entry on the home screen, and a press on the waiting screen, open
/// Settings: pairing, forgetting Home Assistant (after a confirmation) and
/// Technical info, which the ring scrolls. A long press goes back from any
/// of them.
class Controller {
 public:
  static constexpr Millis kLiveInterval = 300;
  static constexpr Millis kSettleDelay = 1500;
  static constexpr Millis kHold = 2000;
  static constexpr Millis kClickRest = 800;

  using Send = std::function<void(const Intent&)>;

  explicit Controller(Send send, SettingsActions actions = {});

  void set_connected(bool connected);
  /// How many notifications are kept, and how many unread: with any, the
  /// home screen starts with a Notifications entry.
  void set_notifications(int count, int unread);
  /// The display woke from rest: back to the top of the home screen, so the
  /// Notifications entry, when there is one, is what shows first.
  void go_home();
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

  /// Technical info's entries, gathered by the app; redrawn when they change.
  void set_info(std::vector<InfoLine> lines);
  /// How far Technical info can scroll: the entries past which the rest
  /// still fills the screen.
  void set_info_scroll_limit(int limit);
  bool info_open() const { return screen_ == RoomView::Screen::Info; }

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

  /// A question on the Confirm screen: forgetting Home Assistant, or
  /// unlocking or opening a lock.
  struct Question {
    std::string title;
    std::string text;
    std::string prompt;
    RoomView::Screen back = RoomView::Screen::Settings;  // where a long press goes
    std::string key;     // the lock; empty for Forget
    std::string action;  // the lock's intent: "unlock" or "open"
  };

  void open_settings();
  void leave_settings();
  void ask(Question question);
  /// Asks before unlocking a lock, or opening its latch.
  void ask_lock(const Item& item, bool open);
  void lock(const Item& item, Millis now);
  int home_count() const;
  /// 1 while the home screen starts with the Notifications entry, else 0.
  int notices_entry() const { return notification_count_ > 0 ? 1 : 0; }
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
  int notification_count_ = 0;
  int notification_unread_ = 0;

  RoomView::Screen screen_ = RoomView::Screen::Empty;
  int selected_ = 0;
  std::string open_key_;
  int control_ = 0;
  int setting_ = 0;  // the selected Settings entry
  Question question_;
  std::vector<InfoLine> info_;
  int info_scroll_ = 0;
  int info_scroll_limit_ = 0;
  int click_ticks_ = 0;  // ticks towards the next click, with their sign
  Millis last_turn_ = 0;

  std::map<std::string, ItemState> shown_;  // values set on the panel, ahead of HA
  std::map<std::string, Millis> hold_until_;
  std::map<std::string, Pending> pending_;  // by item key and channel
  std::optional<std::string> message_;
  unsigned version_ = 1;
};

}  // namespace nesto::room
