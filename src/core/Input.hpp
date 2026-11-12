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

/// Someone is near: on the thermostat, the base's proximity sensor (a hand
/// at the glass) or its far-field motion sensor (someone moving in the room).
struct Presence {
  bool far = false;  // the far-field sensor
};

/// A light sensor reading: on the thermostat, the base's, higher brighter
/// (docs/BACKPLATE.md); about 100 dark, 2000 a lit room.
struct Light {
  int raw = 0;
};

/// The program should stop, for example because the simulator's window was
/// closed. A non-zero exit code reports a failure to whoever started it.
struct QuitRequest {
  int exit_code = 0;
};

using InputEvent = std::variant<RingTurn, ButtonEdge, Presence, Light, QuitRequest>;

struct TimedInput {
  Millis at = 0;
  InputEvent event;
};

}  // namespace nesto
