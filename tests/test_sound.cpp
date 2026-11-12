#include <doctest/doctest.h>

#include "room/Sound.hpp"

using nesto::Millis;
using nesto::room::SoundPlayer;
namespace sounds = nesto::room::sounds;

TEST_CASE("a pattern plays step by step, then falls silent") {
  SoundPlayer p;
  CHECK_FALSE(p.tick(0));
  p.play({{2500, 150}, {0, 100}, {3000, 50}}, 1000);
  CHECK(p.playing());
  CHECK(p.tick(1000) == 2500);
  CHECK_FALSE(p.tick(1149));
  CHECK(p.tick(1150) == 0);  // the pause
  CHECK_FALSE(p.tick(1200));
  CHECK(p.tick(1250) == 3000);
  CHECK(p.tick(1300) == 0);  // the end
  CHECK_FALSE(p.playing());
  CHECK_FALSE(p.tick(1400));
}

TEST_CASE("a late loop doesn't stretch the pattern") {
  SoundPlayer p;
  p.play({{2500, 150}, {3000, 150}}, 0);
  CHECK(p.tick(0) == 2500);
  CHECK(p.tick(170) == 3000);  // 20 ms late
  CHECK(p.tick(300) == 0);     // still ends at 300
}

TEST_CASE("a new pattern replaces one playing") {
  SoundPlayer p;
  p.play(sounds::alert(), 0);
  CHECK(p.tick(0) == 3200);
  p.play(sounds::short_beep(), 100);
  CHECK(p.tick(100) == 2500);
  CHECK(p.tick(250) == 0);
  CHECK_FALSE(p.playing());
}

TEST_CASE("the panel's sounds") {
  CHECK(sounds::short_beep().size() == 1);
  CHECK(sounds::alert().front().hz == 3200);
  const auto id = sounds::identify();
  REQUIRE(id.size() == 5);
  Millis total = 0;
  for (const auto& s : id) total += s.ms;
  CHECK(total == 3 * 150 + 2 * 850);
}
