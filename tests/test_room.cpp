#include <doctest/doctest.h>

#include "room/Room.hpp"

using namespace nesto::room;

namespace {

// The example layout of PROTOCOL.md section 9.
Json example_layout() {
  return Json::parse(R"({
    "t": "layout", "rev": 7, "title": "Living room",
    "items": [
      {"key": "a1", "kind": "light", "name": "All lights", "features": {"brightness": true}},
      {"key": "l1", "kind": "light", "name": "Ceiling", "icon": "ceiling-light",
       "features": {"brightness": true, "color_temp": {"min_k": 2200, "max_k": 6500},
                    "color": true, "effects": ["Candle", "Fireplace", "Sunrise"]}},
      {"key": "c1", "kind": "climate", "name": "Radiator",
       "features": {"target": "single", "min": 5, "max": 30, "step": 0.5, "unit": "C",
                    "hvac_modes": ["off", "heat", "auto"], "presets": ["eco", "comfort"],
                    "fan_modes": []}},
      {"key": "v1", "kind": "cover", "name": "Window blind",
       "features": {"open": true, "close": true, "stop": true, "position": true, "tilt": false}},
      {"key": "s1", "kind": "scene", "name": "Film night"},
      {"key": "w1", "kind": "switch", "name": "Fan"},
      {"key": "t1", "kind": "sensor", "name": "CO2", "features": {"unit": "ppm"}}
    ]})");
}

}  // namespace

TEST_CASE("the example layout parses with every feature") {
  std::string error;
  const auto layout = parse_layout(example_layout(), error);
  REQUIRE_MESSAGE(layout, error);
  CHECK(layout->rev == 7);
  CHECK(layout->title == "Living room");
  REQUIRE(layout->items.size() == 7);

  const Item& light = layout->items[1];
  CHECK(light.kind == Kind::Light);
  CHECK(light.icon == "ceiling-light");
  CHECK(light.light.brightness);
  CHECK(light.light.color);
  REQUIRE(light.light.color_temp);
  CHECK(light.light.color_temp->first == 2200);
  CHECK(light.light.effects.size() == 3);

  const Item& climate = layout->items[2];
  CHECK(climate.climate.target == ClimateFeatures::Target::Single);
  CHECK(climate.climate.min == 5);
  CHECK(climate.climate.step == 0.5);
  CHECK(climate.climate.hvac_modes.size() == 3);

  CHECK(layout->items[3].cover.position);
  CHECK_FALSE(layout->items[3].cover.tilt);
  CHECK(layout->items[6].unit == "ppm");
}

TEST_CASE("items of unknown kinds are skipped, as section 9 says") {
  Json m = example_layout();
  m["items"].push_back({{"key", "v9"}, {"kind", "vacuum"}, {"name", "Robot"}});
  std::string error;
  const auto layout = parse_layout(m, error);
  REQUIRE(layout);
  CHECK(layout->items.size() == 7);
}

TEST_CASE("malformed layouts are refused with a reason") {
  const char* bad[] = {
      R"({"t":"layout","items":[]})",
      R"({"t":"layout","rev":1})",
      R"({"t":"layout","rev":1,"items":[{"kind":"light"}]})",
      R"({"t":"layout","rev":1,"items":[{"key":"a","kind":"light"},{"key":"a","kind":"switch"}]})",
      R"({"t":"layout","rev":1,"items":[{"key":"toolongkey","kind":"light"}]})",
      R"({"t":"layout","rev":1,"items":[{"key":"l","kind":"light","features":{"effects":"x"}}]})",
  };
  for (const char* text : bad) {
    CAPTURE(text);
    std::string error;
    CHECK_FALSE(parse_layout(Json::parse(text), error));
    CHECK_FALSE(error.empty());
  }
}

TEST_CASE("more than 48 items is refused") {
  Json m = {{"rev", 1}, {"items", Json::array()}};
  for (int i = 0; i < 49; ++i) {
    m["items"].push_back({{"key", "k" + std::to_string(i)}, {"kind", "switch"}});
  }
  std::string error;
  CHECK_FALSE(parse_layout(m, error));
}

TEST_CASE("partial states merge, and null clears optional fields") {
  ItemState s;
  merge_state(s, Json::parse(R"({"on":true,"brightness":64,"color_temp_k":2700,"hs":[30,80]})"));
  CHECK(s.on);
  CHECK(s.brightness == 64);
  CHECK(s.color_temp_k == 2700);
  REQUIRE(s.hs);
  CHECK(s.hs->first == 30);

  merge_state(s, Json::parse(R"({"brightness":10})"));
  CHECK(s.on);  // untouched
  CHECK(s.brightness == 10);

  merge_state(s, Json::parse(R"({"color_temp_k":null,"hs":null,"available":false})"));
  CHECK_FALSE(s.color_temp_k);
  CHECK_FALSE(s.hs);
  CHECK_FALSE(s.available);
}

TEST_CASE("brightness outside 0 to 100 is clamped") {
  ItemState s;
  merge_state(s, Json::parse(R"({"brightness":250})"));
  CHECK(s.brightness == 100);
}

TEST_CASE("the room keeps states for its items only") {
  Room room;
  std::string error;
  REQUIRE(room.apply_layout(example_layout(), error));

  auto changed = room.apply_states(Json::parse(
      R"({"t":"states","full":true,"items":{"l1":{"on":true,"brightness":40},"zz":{"on":true}}})"));
  CHECK(changed == std::vector<std::string>{"l1"});
  CHECK(room.state("l1").brightness == 40);
  CHECK_FALSE(room.state("w1").on);

  // A full update replaces everything; a partial one merges.
  room.apply_states(Json::parse(R"({"t":"states","full":true,"items":{"w1":{"on":true}}})"));
  CHECK(room.state("w1").on);
  CHECK(room.state("l1").brightness == 0);
  room.apply_states(Json::parse(R"({"t":"states","items":{"l1":{"brightness":70}}})"));
  CHECK(room.state("w1").on);
  CHECK(room.state("l1").brightness == 70);

  // A new layout drops the states of items that are gone.
  REQUIRE(room.apply_layout(Json::parse(
      R"({"rev":8,"items":[{"key":"w1","kind":"switch","name":"Fan"}]})"), error));
  CHECK(room.state("w1").on);
  CHECK(room.find("l1") == nullptr);
}
