// Fans (PROTOCOL.md sections 9.1, 10 and 11): parsing, the controls the ring
// steps through, and the intents they send.
#include <doctest/doctest.h>

#include <algorithm>

#include "room/Controller.hpp"

using namespace nesto;
using namespace nesto::room;

namespace {

Json layout() {
  return Json::parse(R"({
    "t": "layout", "rev": 1, "title": "Bedroom",
    "items": [
      {"key": "f1", "kind": "fan", "name": "Ceiling fan",
       "features": {"speed": true, "speed_step": 25, "presets": ["breeze", "sleep"],
                    "oscillate": true, "direction": true}},
      {"key": "f2", "kind": "fan", "name": "Desk fan",
       "features": {"speed": true, "speed_step": 1}},
      {"key": "f3", "kind": "fan", "name": "Extractor"}
    ]})");
}

Json states() {
  return Json::parse(R"({
    "t": "states", "full": true,
    "items": {
      "f1": {"on": true, "percentage": 50, "preset": null, "oscillating": false,
             "direction": "forward"},
      "f2": {"on": false, "percentage": 0},
      "f3": {"on": false}
    }})");
}

struct Fixture {
  std::vector<Intent> sent;
  Controller controller{[this](const Intent& i) { sent.push_back(i); }};
  Millis now = 10000;

  Fixture() {
    std::string error;
    REQUIRE(controller.apply_layout(layout(), error));
    controller.apply_states(states());
    controller.set_connected(true);
  }

  void after(Millis ms) {
    now += ms;
    controller.tick(now);
  }
  void rotate(int clicks) { controller.rotate(clicks * kTicksPerClick, now); }
  void turn(int ticks) { controller.rotate(ticks, now); }
  void press() { controller.press(now); }
  void hold() { controller.long_press(now); }
  void select(int index) {
    rotate(-10);
    rotate(index);
  }
  RoomView view() const { return controller.view(); }
};

}  // namespace

TEST_CASE("a fan's features and state parse") {
  std::string error;
  const auto parsed = parse_layout(layout(), error);
  REQUIRE(parsed);
  REQUIRE(parsed->items.size() == 3);
  const Item& fan = parsed->items[0];
  CHECK(fan.kind == Kind::Fan);
  CHECK(fan.fan.speed);
  CHECK(fan.fan.speed_step == 25);
  CHECK(fan.fan.presets == std::vector<std::string>{"breeze", "sleep"});
  CHECK(fan.fan.oscillate);
  CHECK(fan.fan.direction);
  CHECK_FALSE(parsed->items[2].fan.speed);
  CHECK(parsed->items[2].fan.speed_step == 1);

  ItemState s;
  merge_state(s, Json::parse(R"({"on": true, "percentage": 120, "oscillating": true,
                                 "direction": "reverse", "preset": "sleep"})"));
  CHECK(s.on);
  CHECK(s.percentage == 100);
  CHECK(s.oscillating);
  CHECK(s.direction == "reverse");
  CHECK(s.preset == "sleep");
  merge_state(s, Json::parse(R"({"percentage": null})"));
  CHECK_FALSE(s.percentage);
}

TEST_CASE("caps.kinds lists every kind the panel draws, fan included") {
  const auto kinds = kind_names();
  for (const char* kind : {"light", "switch", "scene", "climate", "cover", "sensor", "fan"}) {
    CAPTURE(kind);
    CHECK(std::find(kinds.begin(), kinds.end(), kind) != kinds.end());
  }
}

TEST_CASE("a fan's controls follow its features") {
  std::string error;
  const auto parsed = parse_layout(layout(), error);
  REQUIRE(parsed);
  CHECK(controls_for(parsed->items[0]) ==
        std::vector<Control>{Control::Speed, Control::Preset, Control::Oscillate,
                             Control::Direction});
  CHECK(controls_for(parsed->items[1]) == std::vector<Control>{Control::Speed});
  CHECK(controls_for(parsed->items[2]) == std::vector<Control>{Control::Power});
}

TEST_CASE("the home screen shows a fan's speed, and its tone") {
  Fixture f;
  const auto entries = f.view().entries;
  REQUIRE(entries.size() == 4);  // three fans and Settings
  CHECK(entries[0].summary == "On - 50%");
  CHECK(entries[0].tone == Tone::Active);
  CHECK(entries[0].icon == "fan");
  CHECK(entries[1].summary == "Off");
  CHECK(entries[1].tone == Tone::Off);
}

TEST_CASE("a fan with a few speeds moves one speed per click") {
  Fixture f;
  f.press();
  CHECK(f.view().control == "Speed");
  CHECK(f.view().value == "50%");
  f.turn(kTicksPerClick - 1);
  CHECK(f.sent.empty());  // short of a click
  f.turn(1);
  CHECK(f.view().value == "75%");
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "set_percentage");
  CHECK(f.sent[0].args == Json{{"percentage", 75}});

  f.after(400);
  f.rotate(-3);  // 75 → 0: off
  f.after(400);
  CHECK(f.view().value == "Off");
  CHECK(f.sent.back().action == "turn_off");
  CHECK(f.sent.back().args == Json::object());
}

TEST_CASE("a fan with close speeds follows every tick, paced like a light") {
  Fixture f;
  f.select(1);
  f.press();
  CHECK(f.view().value == "Off");
  f.turn(1);
  f.turn(1);
  f.turn(1);
  CHECK(f.view().value == "3%");
  REQUIRE(f.sent.size() == 1);  // the first at once, the rest paced
  CHECK(f.sent[0].args == Json{{"percentage", 1}});
  f.after(400);
  CHECK(f.sent.back().args == Json{{"percentage", 3}});
}

TEST_CASE("a fan's preset, oscillation and direction") {
  Fixture f;
  f.press();
  f.press();
  CHECK(f.view().control == "Preset");
  f.rotate(1);
  CHECK(f.view().value == "Breeze");
  f.after(1600);  // presets wait for the ring to stop, as climate's do
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "set_preset");
  CHECK(f.sent[0].args == Json{{"preset", "breeze"}});

  f.press();
  CHECK(f.view().control == "Oscillate");
  CHECK(f.view().value == "Off");
  f.rotate(1);
  CHECK(f.view().value == "On");
  CHECK(f.sent.back().action == "oscillate");
  CHECK(f.sent.back().args == Json{{"oscillating", true}});
  f.rotate(1);  // already on: nothing more
  CHECK(f.sent.size() == 2);

  f.press();
  CHECK(f.view().control == "Direction");
  CHECK(f.view().value == "Forward");
  f.rotate(-1);
  CHECK(f.view().value == "Reverse");
  CHECK(f.sent.back().action == "set_direction");
  CHECK(f.sent.back().args == Json{{"direction", "reverse"}});
}

TEST_CASE("a long press on the home screen toggles a fan; a fan without speed opens to Power") {
  Fixture f;
  f.hold();
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "turn_off");

  f.select(2);
  f.press();
  CHECK(f.view().control == "Power");
  f.rotate(1);
  CHECK(f.sent.back().key == "f3");
  CHECK(f.sent.back().action == "turn_on");
}
