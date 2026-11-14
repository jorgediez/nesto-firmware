#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Input.hpp"

namespace nesto {

/// One input event at a time offset from the start of a script.
struct ScriptedInput {
  Millis offset = 0;
  InputEvent event;
};

struct InputScript {
  std::vector<ScriptedInput> events;
  Millis duration = 0;  // when the last step has finished
};

/// Parses an input script for the simulator, so screens can be driven and
/// captured without a person at the controls. Steps are separated by spaces or
/// commas, run one after another, and each takes some time:
///
///   r+N / r-N  turn the ring N steps clockwise / anticlockwise (50 ms)
///   p          press and release (80 ms down, then 120 ms)
///   h          hold for a long press (900 ms down, then 120 ms)
///   hN         hold for N milliseconds (then 120 ms)
///   m          someone comes near (50 ms)
///   f          someone moves in the room, for the far-field sensor (50 ms)
///   lN         the light sensor reads N: 100 dark, 2000 a lit room (no time)
///   wN         wait N milliseconds
///
/// Returns nothing and fills `error` if a step isn't understood.
std::optional<InputScript> parse_input_script(std::string_view script, std::string& error);

}  // namespace nesto
