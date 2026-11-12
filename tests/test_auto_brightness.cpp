#include <doctest/doctest.h>

#include "room/AutoBrightness.hpp"

using nesto::Millis;
using nesto::room::AutoBrightness;

TEST_CASE("without readings the brightness is unchanged") {
  AutoBrightness a;
  CHECK_FALSE(a.known(0));
  CHECK(a.scale(80, 0) == 80);
  CHECK(a.scale(0, 0) == 0);
}

TEST_CASE("a lit room keeps the brightness, a dark one takes a quarter") {
  AutoBrightness a;
  a.reading(2000, 1000);  // lights on, as measured
  REQUIRE(a.known(1000));
  CHECK(a.scale(80, 1000) == 80);
  CHECK(a.scale(0, 1000) == 0);

  AutoBrightness torch;
  torch.reading(19000, 0);
  CHECK(torch.scale(80, 0) == 80);

  AutoBrightness dark;
  dark.reading(105, 0);  // lights off
  CHECK(dark.scale(80, 0) == 21);  // a quarter, nearly: 105 is just above dark

  AutoBrightness darker;
  darker.reading(50, 0);
  CHECK(darker.scale(80, 0) == 20);
  CHECK(darker.scale(20, 0) == 5);  // the standby clock: never below 5%
  CHECK(darker.scale(3, 0) == 3);   // nor above what's configured
}

TEST_CASE("half way on a logarithmic scale") {
  AutoBrightness a;
  a.reading(388, 0);  // about the geometric mean of 100 and 1500
  CHECK(a.light() == doctest::Approx(0.5).epsilon(0.01));
  CHECK(a.scale(100, 0) == 63);  // 25 + 75 / 2
}

TEST_CASE("towards dark slowly, towards light quickly") {
  AutoBrightness a;
  a.reading(2000, 0);
  // Covered by a hand for 2 s: still nearly full.
  for (Millis t = 1000; t <= 2000; t += 1000) a.reading(200, t);
  CHECK(a.scale(80, 2000) >= 70);
  // Dark for a minute: dark.
  for (Millis t = 3000; t <= 62000; t += 1000) a.reading(100, t);
  CHECK(a.scale(80, 62000) == 20);
  // The lights on: most of the way within 4 s.
  for (Millis t = 63000; t <= 66000; t += 1000) a.reading(2000, t);
  CHECK(a.scale(80, 66000) >= 70);
}

TEST_CASE("old readings stop counting") {
  AutoBrightness a;
  a.reading(100, 0);
  CHECK(a.scale(80, AutoBrightness::kStale) == 20);
  CHECK_FALSE(a.known(AutoBrightness::kStale + 1));
  CHECK(a.scale(80, AutoBrightness::kStale + 1) == 80);
  // A new reading after the gap starts afresh, without the old one.
  a.reading(2000, 30000);
  CHECK(a.scale(80, 30000) == 80);
}
