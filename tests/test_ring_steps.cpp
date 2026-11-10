#include <doctest/doctest.h>

#include "core/RingSteps.hpp"

using nesto::RingSteps;

namespace {
RingSteps ring(int counts_per_step, bool reverse = false) {
  RingSteps::Config config;
  config.counts_per_step = counts_per_step;
  config.reverse = reverse;
  return RingSteps(config);
}
}  // namespace

TEST_CASE("counts add up to whole steps, and the leftover carries on") {
  RingSteps r = ring(10);
  CHECK(r.add(1000, 4) == 0);
  CHECK(r.add(1010, 4) == 0);
  CHECK(r.add(1020, 4) == 1);  // 12: one step, 2 left
  CHECK(r.add(1030, 8) == 1);  // 10
  CHECK(r.add(1040, 35) == 3);
}

TEST_CASE("turning anticlockwise gives negative steps") {
  RingSteps r = ring(10);
  CHECK(r.add(1000, -7) == 0);
  CHECK(r.add(1010, -7) == -1);
  CHECK(r.add(1020, -26) == -3);  // -4 left over, and -26
}

TEST_CASE("turning back drops the leftover, so a wobble never makes a step") {
  RingSteps r = ring(10);
  CHECK(r.add(1000, 9) == 0);
  CHECK(r.add(1010, -9) == 0);
  CHECK(r.add(1020, 9) == 0);
  CHECK(r.add(1030, -9) == 0);
  CHECK(r.add(1040, 1) == 0);  // after the turn back, 1 is all there is
}

TEST_CASE("resting drops the leftover") {
  RingSteps r = ring(10);
  CHECK(r.add(1000, 9) == 0);
  CHECK(r.add(1499, 1) == 1);  // still moving
  CHECK(r.add(1600, 9) == 0);
  CHECK(r.add(2100, 1) == 0);  // rested 500 ms: the 9 are gone
}

TEST_CASE("a reversed sensor swaps the direction") {
  RingSteps r = ring(10, true);
  CHECK(r.add(1000, 25) == -2);
  CHECK(r.add(1010, -5) == 0);  // turned back: the -5 left over is dropped
}

TEST_CASE("a step of one count passes every count through, and zero means one") {
  CHECK(ring(1).add(1000, 7) == 7);
  RingSteps zero = ring(0);
  CHECK(zero.config().counts_per_step == 1);
  CHECK(zero.add(1000, -3) == -3);
}
