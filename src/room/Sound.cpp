#include "room/Sound.hpp"

#include <utility>

namespace nesto::room {
namespace sounds {

Pattern short_beep() { return {{2500, 150}}; }

Pattern alert() { return {{3200, 150}, {2400, 150}, {3200, 150}, {2400, 150}, {3200, 150}}; }

Pattern identify() {
  Pattern out;
  for (int i = 0; i < 3; ++i) {
    out.push_back({2500, 150});
    if (i < 2) out.push_back({0, 850});
  }
  return out;
}

}  // namespace sounds

void SoundPlayer::play(sounds::Pattern pattern, Millis now) {
  pattern_ = std::move(pattern);
  step_ = 0;
  step_end_ = now;
  playing_ = !pattern_.empty();
  started_ = false;
}

std::optional<int> SoundPlayer::tick(Millis now) {
  if (!playing_) return std::nullopt;
  if (started_ && now < step_end_) return std::nullopt;
  if (started_) ++step_;  // this step is over
  if (step_ >= pattern_.size()) {
    playing_ = false;
    return 0;  // silence at the end
  }
  // From when the last step ended, so a late loop doesn't stretch the pattern.
  step_end_ = (started_ ? step_end_ : now) + pattern_[step_].ms;
  started_ = true;
  return pattern_[step_].hz;
}

}  // namespace nesto::room
