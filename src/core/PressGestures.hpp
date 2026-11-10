#pragma once

#include <optional>
#include <vector>

#include "core/Input.hpp"

namespace nesto {

/// What the panel was doing when a press started. See PROTOCOL.md, section 17.
enum class PressContext { Ui, Standby, Off };

enum class Gesture { Single, Double, Long };

struct GestureEvent {
  Gesture gesture;
  PressContext context;

  bool operator==(const GestureEvent& other) const {
    return gesture == other.gesture && context == other.context;
  }
};

/// Turns press and release edges into gestures, following PROTOCOL.md
/// sections 17 and 23:
///
/// - Held for `long_press` ms: Long, as soon as the time passes, while still
///   held. Its release produces nothing.
/// - In Ui: released earlier, Single, at once. Double presses are never
///   recognised in Ui, so single presses aren't delayed.
/// - In Standby and Off: released earlier, the recogniser waits up to
///   `double_window` ms after the release. A second press within that time
///   gives Double, at the moment it goes down; its release produces nothing.
///   Otherwise Single when the window ends.
///
/// The context is taken when the first press goes down and kept for the whole
/// gesture. The recogniser has no clock of its own: the caller passes the time
/// to each method and calls tick() regularly, at least every few tens of ms.
class PressGestures {
 public:
  struct Timing {
    Millis long_press = 800;
    Millis double_window = 350;
  };

  PressGestures() = default;
  explicit PressGestures(Timing timing) : timing_(timing) {}

  std::vector<GestureEvent> on_down(Millis now, PressContext context);
  std::vector<GestureEvent> on_up(Millis now);
  std::vector<GestureEvent> tick(Millis now);

  /// True while a gesture is in progress, so the caller knows tick() matters.
  bool busy() const { return state_ != State::Idle; }

 private:
  enum class State {
    Idle,
    Held,            // first press down
    WaitingSecond,   // first press released, a second one may follow
    SwallowRelease,  // the press already produced its gesture; ignore its release
  };

  Timing timing_{};
  State state_ = State::Idle;
  Millis since_ = 0;  // when the current state started
  PressContext context_ = PressContext::Ui;
};

}  // namespace nesto
