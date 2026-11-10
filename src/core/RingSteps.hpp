#pragma once

#include "core/Input.hpp"

namespace nesto {

/// Turns the ring sensor's raw counts into the steps the screens move by.
///
/// The Nest's ring has no clicks: an optical sensor under it reports how far
/// it moved, in counts much finer than a step. Counts add up until they make
/// a whole step; what's left over carries on to the next movement, so turning
/// slowly moves as far as turning fast. The leftover is dropped when the ring
/// turns back, or rests for `settle` ms, so a slight wobble never adds up to
/// a step by itself.
class RingSteps {
 public:
  struct Config {
    int counts_per_step = 16;  // must be at least 1
    bool reverse = false;      // the sensor counts the other way round
    Millis settle = 500;
  };

  RingSteps() = default;
  explicit RingSteps(Config config);

  /// Adds `counts` read at `now`; returns the whole steps they complete,
  /// positive clockwise.
  int add(Millis now, int counts);

  const Config& config() const { return config_; }

 private:
  Config config_;
  int pending_ = 0;  // counts not yet making a step, already reversed if needed
  Millis last_ = 0;
};

}  // namespace nesto
