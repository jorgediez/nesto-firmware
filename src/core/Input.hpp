#pragma once

#include <cstdint>
#include <variant>

namespace nesto {

/// Milliseconds on a monotonic clock. Only differences are meaningful.
using Millis = std::uint64_t;

/// The ring turned by some steps: positive is clockwise.
struct RingTurn {
  int steps = 0;
};

/// The display was pressed (the Nest's only button) or released.
struct ButtonEdge {
  bool down = false;
};

/// The program should stop, for example because the simulator's window was
/// closed. A non-zero exit code reports a failure to whoever started it.
struct QuitRequest {
  int exit_code = 0;
};

using InputEvent = std::variant<RingTurn, ButtonEdge, QuitRequest>;

struct TimedInput {
  Millis at = 0;
  InputEvent event;
};

}  // namespace nesto
