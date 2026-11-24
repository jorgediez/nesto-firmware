#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/Input.hpp"
#include "protocol/Message.hpp"
#include "room/Controller.hpp"

namespace nesto::room {

/// PROTOCOL.md section 14.
enum class Priority { Low, Normal, High, Critical };

struct Notification {
  struct Action {
    std::string id;
    std::string label;
  };
  std::string nid;
  std::string title;
  std::string message;
  std::string icon;
  Priority priority = Priority::Normal;
  unsigned duration_s = 15;  // 0: on screen until answered
  enum class Beep { None, Short, Alert } beep = Beep::Short;
  std::vector<Action> actions;

  /// From a `notify` request; false with `error` set if it's not valid.
  static bool parse(const protocol::Json& body, Notification& out, std::string& error);
};

/// The notifications Home Assistant sent: the 10 most recent, their pop-ups,
/// and the list, with what the ring and press do on them. Events for Home
/// Assistant (`notify_event`) go out through `send`.
///
/// - A notification of `normal` priority or above pops up if the display is
///   lit (the App wakes it first, as section 13 says), for `duration_s`; then
///   it's "expired" and stays in the list. One with no time limit, critical
///   or `duration_s` 0, stays until answered, and pops up again each time
///   the display wakes.
/// - On a pop-up, or one opened from the list, the ring chooses among its
///   actions and Dismiss, and a press sends the choice: either removes it.
///   A long press closes it, keeping it in the list, except a critical one.
/// - The list, newest first, opens from the home screen; a press opens a
///   notification, a long press goes back. A notification opened from it is
///   interrupted only by a critical one, and comes back once that's
///   answered; a pop-up over the list goes back to the list once it's done.
class Notifications {
 public:
  static constexpr std::size_t kKept = 10;

  using Send = std::function<void(const protocol::Json&)>;
  explicit Notifications(Send send) : send_(std::move(send)) {}

  /// A new notification, or one replacing another with its `nid`. `lit`: the
  /// display is active, so it can pop up.
  void add(Notification n, bool lit, Millis now);
  /// `notify_clear`: no events.
  void clear(const std::string& nid);
  void clear_all();

  /// Whether a notification screen is showing: then the ring and press are
  /// meant for it.
  bool active() const { return mode_ != Mode::None; }
  void open_list();
  /// `clicks` of the ring (core/Input.hpp).
  void rotate(int clicks);
  void press();
  void long_press();
  /// Ends a timed pop-up once its time is up.
  void tick(Millis now);

  /// The display rested: a timed pop-up expires, one without a time limit
  /// waits to be shown again, and the list closes.
  void display_rested();
  /// The display woke: a notification still waiting for an answer pops up.
  void display_woke(Millis now);
  /// Home Assistant showed an item: the list, or one opened from it,
  /// closes; a pop-up stays, as it's waiting to be seen.
  void close_list();
  /// A timed pop-up is showing: it keeps the display from resting.
  bool holds_display() const;

  /// How far the opened notification's message can scroll, in lines, as
  /// the screen found it; the ring scrolls it when there are no actions.
  void set_scroll_limit(int lines);

  int count() const { return static_cast<int>(kept_.size()); }
  /// A critical notification with a beep is waiting for an answer: its alert
  /// repeats every 30 s (PROTOCOL.md section 14).
  bool critical_waiting() const;
  int unread() const;
  /// The screen to draw while active().
  RoomView view() const;
  unsigned version() const { return version_; }

 private:
  enum class Mode { None, Popup, List, Open };
  struct Kept {
    Notification n;
    bool read = false;
    bool answered = false;  // no longer waiting to pop up again
  };

  Kept* find(const std::string& nid);
  Kept* current();
  void pop_up(const std::string& nid, Millis now);
  /// The pop-up or opened one leaves the screen.
  void close_current();
  /// After a pop-up: the next one waiting, if any, or back to the room.
  void after_popup(Millis now);
  void remove(const std::string& nid);
  void event(const std::string& nid, const char* what, const std::string& action = "");
  void mark_shown(Kept& k);
  int choice_count() const;
  void changed() { ++version_; }

  Send send_;
  std::vector<Kept> kept_;  // newest first
  Mode mode_ = Mode::None;
  std::string current_;  // the pop-up's or opened one's nid
  Millis shown_at_ = 0;
  int selected_ = 0;  // in the list
  int choice_ = 0;
  int scroll_ = 0;
  int scroll_limit_ = 0;
  Millis now_ = 0;  // the last time seen, for pop-ups that follow others
  bool back_to_list_ = false;  // the pop-up came over the list
  std::string back_to_open_;   // or over this one, opened from it
  unsigned version_ = 1;
};

}  // namespace nesto::room
