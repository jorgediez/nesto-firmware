#include <doctest/doctest.h>

#include "core/InputScript.hpp"

using namespace nesto;

TEST_CASE("an empty script does nothing") {
  std::string error;
  auto script = parse_input_script("", error);
  REQUIRE(script);
  CHECK(script->events.empty());
  CHECK(script->duration == 0);
}

TEST_CASE("steps run one after another with their own durations") {
  std::string error;
  auto script = parse_input_script("r+2, r-1 p w500 h", error);
  REQUIRE(script);
  const auto& e = script->events;
  REQUIRE(e.size() == 6);

  CHECK(e[0].offset == 0);
  CHECK(std::get<RingTurn>(e[0].event).ticks == 2 * nesto::kTicksPerClick);
  CHECK(e[1].offset == 50);
  CHECK(std::get<RingTurn>(e[1].event).ticks == -nesto::kTicksPerClick);

  CHECK(e[2].offset == 100);  // press down
  CHECK(std::get<ButtonEdge>(e[2].event).down);
  CHECK(e[3].offset == 180);  // released 80 ms later
  CHECK_FALSE(std::get<ButtonEdge>(e[3].event).down);

  // 120 ms after the release, then a 500 ms wait.
  CHECK(e[4].offset == 800);  // hold down
  CHECK(e[5].offset == 1700);  // released 900 ms later
  CHECK(script->duration == 1820);
}

TEST_CASE("m: someone comes near") {
  std::string error;
  auto script = parse_input_script("m w100 m f", error);
  REQUIRE(script);
  REQUIRE(script->events.size() == 3);
  CHECK_FALSE(std::get<nesto::Presence>(script->events[0].event).far);
  CHECK(script->events[1].offset == 150);
  CHECK(std::get<nesto::Presence>(script->events[2].event).far);  // f: moving in the room
}

TEST_CASE("hN: a hold of N milliseconds") {
  std::string error;
  auto script = parse_input_script("h2000", error);
  REQUIRE(script);
  REQUIRE(script->events.size() == 2);
  CHECK(script->events[1].offset == 2000);
  CHECK(script->duration == 2120);
}

TEST_CASE("lN: a light sensor reading") {
  std::string error;
  auto script = parse_input_script("l100 w500 l2000", error);
  REQUIRE(script);
  REQUIRE(script->events.size() == 2);
  CHECK(std::get<nesto::Light>(script->events[0].event).raw == 100);
  CHECK(script->events[1].offset == 500);
  CHECK(std::get<nesto::Light>(script->events[1].event).raw == 2000);
}

TEST_CASE("unknown steps are reported") {
  for (const char* bad : {"x", "r1", "r+", "r+0", "r+a", "p2", "w", "w-5", "hh", "m2", "l", "l-1"}) {
    std::string error;
    CAPTURE(bad);
    CHECK_FALSE(parse_input_script(bad, error));
    CHECK(error.find(bad) != std::string::npos);
  }
}
