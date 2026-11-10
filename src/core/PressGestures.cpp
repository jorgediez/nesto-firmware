#include "core/PressGestures.hpp"

namespace nesto {

std::vector<GestureEvent> PressGestures::on_down(Millis now, PressContext context) {
  // A missed tick() must not swallow a finished single press.
  std::vector<GestureEvent> out = tick(now);

  switch (state_) {
    case State::Idle:
      state_ = State::Held;
      since_ = now;
      context_ = context;
      break;
    case State::WaitingSecond:
      out.push_back({Gesture::Double, context_});
      state_ = State::SwallowRelease;
      since_ = now;
      break;
    case State::Held:
    case State::SwallowRelease:
      // Two downs without an up in between: a glitch in the input. Ignore it.
      break;
  }
  return out;
}

std::vector<GestureEvent> PressGestures::on_up(Millis now) {
  std::vector<GestureEvent> out = tick(now);

  switch (state_) {
    case State::Held:
      if (context_ == PressContext::Ui) {
        out.push_back({Gesture::Single, context_});
        state_ = State::Idle;
      } else {
        state_ = State::WaitingSecond;
        since_ = now;
      }
      break;
    case State::SwallowRelease:
      state_ = State::Idle;
      break;
    case State::Idle:
    case State::WaitingSecond:
      // An up without a down, for example after a long press was reported
      // by tick() and the state already moved on. Nothing to do.
      break;
  }
  return out;
}

std::vector<GestureEvent> PressGestures::tick(Millis now) {
  std::vector<GestureEvent> out;
  const Millis elapsed = now >= since_ ? now - since_ : 0;

  if (state_ == State::Held && elapsed >= timing_.long_press) {
    out.push_back({Gesture::Long, context_});
    state_ = State::SwallowRelease;
  } else if (state_ == State::WaitingSecond && elapsed > timing_.double_window) {
    out.push_back({Gesture::Single, context_});
    state_ = State::Idle;
  }
  return out;
}

}  // namespace nesto
