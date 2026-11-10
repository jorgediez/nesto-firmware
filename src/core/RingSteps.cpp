#include "core/RingSteps.hpp"

namespace nesto {

RingSteps::RingSteps(Config config) : config_(config) {
  if (config_.counts_per_step < 1) config_.counts_per_step = 1;
}

int RingSteps::add(Millis now, int counts) {
  if (config_.reverse) counts = -counts;
  if (now - last_ >= config_.settle || (pending_ > 0 && counts < 0) || (pending_ < 0 && counts > 0)) {
    pending_ = 0;
  }
  last_ = now;
  pending_ += counts;
  // Division truncates towards zero, which keeps the leftover's sign.
  const int steps = pending_ / config_.counts_per_step;
  pending_ -= steps * config_.counts_per_step;
  return steps;
}

}  // namespace nesto
