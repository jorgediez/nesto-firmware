#include <doctest/doctest.h>

#include "core/RingTicks.hpp"

using nesto::RingTicks;

namespace {
RingTicks ring(int counts_per_tick, bool reverse = false) {
  RingTicks::Config config;
  config.counts_per_tick = counts_per_tick;
  config.reverse = reverse;
  return RingTicks(config);
}
}  // namespace

TEST_CASE("counts add up to whole ticks, and the leftover carries on") {
  RingTicks r = ring(10);
  CHECK(r.add(1000, 4) == 0);
  CHECK(r.add(1010, 4) == 0);
  CHECK(r.add(1020, 4) == 1);  // 12: one tick, 2 left
  CHECK(r.add(1030, 8) == 1);  // 10
  CHECK(r.add(1040, 35) == 3);
}

TEST_CASE("turning anticlockwise gives negative ticks") {
  RingTicks r = ring(10);
  CHECK(r.add(1000, -7) == 0);
  CHECK(r.add(1010, -7) == -1);
  CHECK(r.add(1020, -26) == -3);  // -4 left over, and -26
}

TEST_CASE("turning back drops the leftover, so a wobble never makes a tick") {
  RingTicks r = ring(10);
  CHECK(r.add(1000, 9) == 0);
  CHECK(r.add(1010, -9) == 0);
  CHECK(r.add(1020, 9) == 0);
  CHECK(r.add(1030, -9) == 0);
  CHECK(r.add(1040, 1) == 0);  // after the turn back, 1 is all there is
}

TEST_CASE("resting drops the leftover") {
  RingTicks r = ring(10);
  CHECK(r.add(1000, 9) == 0);
  CHECK(r.add(1499, 1) == 1);  // still moving
  CHECK(r.add(1600, 9) == 0);
  CHECK(r.add(2100, 1) == 0);  // rested 500 ms: the 9 are gone
}

TEST_CASE("a reversed sensor swaps the direction") {
  RingTicks r = ring(10, true);
  CHECK(r.add(1000, 25) == -2);
  CHECK(r.add(1010, -5) == 0);  // turned back: the -5 left over is dropped
}

TEST_CASE("a tick of one count passes every count through, and zero means one") {
  CHECK(ring(1).add(1000, 7) == 7);
  RingTicks zero = ring(0);
  CHECK(zero.config().counts_per_tick == 1);
  CHECK(zero.add(1000, -3) == -3);
}
