#pragma once

#include <optional>
#include <vector>

#include "core/Input.hpp"

namespace nesto::room {

/// The panel's sounds, chosen by ear on a thermostat with --sound-test
/// (docs/DEVICE.md): its beeper plays one tone at a time, with no volume.
namespace sounds {

/// A click: on each tick of the ring, and on pressing the display.
constexpr int kClickHz = 2000;
constexpr int kClickUs = 2000;
/// Clicks closer together than this are skipped: a fast turn sends more
/// ticks than make separate clicks.
constexpr Millis kClickGap = 15;

/// A tone, or silence at 0 Hz, for so long.
struct Step {
  int hz = 0;
  Millis ms = 0;
};
using Pattern = std::vector<Step>;

/// A notification's `short` beep.
Pattern short_beep();
/// A notification's `alert`: a two-tone warble.
Pattern alert();
/// Home Assistant's `identify`: three short beeps, a second apart.
Pattern identify();

}  // namespace sounds

/// Plays a pattern on time without holding up the main loop: tick() says
/// what the beeper should do now. A new pattern replaces one playing.
class SoundPlayer {
 public:
  void play(sounds::Pattern pattern, Millis now);
  /// The tone to set now, in Hz (0 for silence), when it changes.
  std::optional<int> tick(Millis now);
  bool playing() const { return playing_; }

 private:
  sounds::Pattern pattern_;
  std::size_t step_ = 0;
  Millis step_end_ = 0;
  bool playing_ = false;
  bool started_ = false;  // the current step's tone has been set
};

}  // namespace nesto::room
