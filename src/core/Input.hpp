#pragma once

#include <cstdint>
#include <variant>

namespace nesto {

/// Milliseconds on a monotonic clock. Only differences are meaningful.
using Millis = std::uint64_t;

/// The ring turns in ticks, fine enough for smooth values such as
/// brightness; lists move one place per click of kTicksPerClick ticks. The
/// simulator's wheel notch, its arrow keys and a script's r+1 are one click.
constexpr int kTicksPerClick = 5;

/// The ring turned by some ticks: positive is clockwise.
struct RingTurn {
  int ticks = 0;
};

/// The display was pressed (the Nest's only button) or released.
struct ButtonEdge {
  bool down = false;
};

/// Someone is near: on the thermostat, the base's proximity sensor.
struct Presence {};

/// The program should stop, for example because the simulator's window was
/// closed. A non-zero exit code reports a failure to whoever started it.
struct QuitRequest {
  int exit_code = 0;
};

using InputEvent = std::variant<RingTurn, ButtonEdge, Presence, QuitRequest>;

struct TimedInput {
  Millis at = 0;
  InputEvent event;
};

}  // namespace nesto
