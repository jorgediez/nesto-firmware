#include "core/RingTicks.hpp"

namespace nesto {

RingTicks::RingTicks(Config config) : config_(config) {
  if (config_.counts_per_tick < 1) config_.counts_per_tick = 1;
}

int RingTicks::add(Millis now, int counts) {
  if (config_.reverse) counts = -counts;
  if (now - last_ >= config_.settle || (pending_ > 0 && counts < 0) || (pending_ < 0 && counts > 0)) {
    pending_ = 0;
  }
  last_ = now;
  pending_ += counts;
  // Division truncates towards zero, which keeps the leftover's sign.
  const int ticks = pending_ / config_.counts_per_tick;
  pending_ -= ticks * config_.counts_per_tick;
  return ticks;
}

}  // namespace nesto
