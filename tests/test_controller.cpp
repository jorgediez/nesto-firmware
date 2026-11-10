// The ring and press on the room's screens, with a fake clock, checking the
// intents and their timing against PROTOCOL.md sections 11 and 23.
#include <doctest/doctest.h>

#include "room/Controller.hpp"

using namespace nesto;
using namespace nesto::room;

namespace {

Json layout() {
  return Json::parse(R"({
    "t": "layout", "rev": 1, "title": "Living room",
    "items": [
      {"key": "l1", "kind": "light", "name": "Ceiling",
       "features": {"brightness": true, "color_temp": {"min_k": 2200, "max_k": 6500},
                    "color": true, "effects": ["Candle", "Fireplace"]}},
      {"key": "c1", "kind": "climate", "name": "Radiator",
       "features": {"target": "single", "min": 5, "max": 30, "step": 0.5, "unit": "C",
                    "hvac_modes": ["off", "heat"], "presets": ["eco", "comfort"]}},
      {"key": "v1", "kind": "cover", "name": "Blind",
       "features": {"open": true, "close": true, "stop": true, "position": true}},
      {"key": "s1", "kind": "scene", "name": "Film night"},
      {"key": "w1", "kind": "switch", "name": "Fan"},
      {"key": "t1", "kind": "sensor", "name": "CO2", "features": {"unit": "ppm"}},
      {"key": "b1", "kind": "cover", "name": "Garage", "features": {"open": true, "close": true, "stop": true}}
    ]})");
}

Json states() {
  return Json::parse(R"({
    "t": "states", "full": true,
    "items": {
      "l1": {"on": true, "brightness": 50, "color_temp_k": 3000},
      "c1": {"hvac_mode": "heat", "current": 20.5, "target": 21},
      "v1": {"state": "open", "position": 40},
      "s1": {},
      "w1": {"on": false},
      "t1": {"value": "612"},
      "b1": {"state": "closed"}
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
  void rotate(int steps) { controller.rotate(steps, now); }
  void press() { controller.press(now); }
  void hold() { controller.long_press(now); }
  void select(int index) {
    rotate(-10);
    rotate(index);
  }
  RoomView view() const { return controller.view(); }
};

}  // namespace

TEST_CASE("the home screen lists every item with its state") {
  Fixture f;
  const RoomView v = f.view();
  CHECK(v.screen == RoomView::Screen::Home);
  CHECK(v.title == "Living room");
  REQUIRE(v.entries.size() == 8);  // the items, then Settings
  CHECK(v.entries[0].summary == "On - 50%");
  CHECK(v.entries[1].summary == "20.5C - set to 21.0C");
  CHECK(v.entries[2].summary == "Open - 40%");
  CHECK(v.entries[3].summary == "Scene");
  CHECK(v.entries[4].summary == "Off");
  CHECK(v.entries[5].summary == "612 ppm");
}

TEST_CASE("before any layout the screen is empty") {
  Controller c([](const Intent&) {});
  CHECK(c.view().screen == RoomView::Screen::Empty);
  CHECK(c.view().offline);
}

TEST_CASE("turning the ring on the home screen moves the selection, within the list") {
  Fixture f;
  f.rotate(2);
  CHECK(f.view().selected == 2);
  f.rotate(20);
  CHECK(f.view().selected == 7);  // Settings, after the items
  f.rotate(-20);
  CHECK(f.view().selected == 0);
  CHECK(f.sent.empty());
}

TEST_CASE("a press on a switch toggles it; a press on a scene activates it") {
  Fixture f;
  f.select(4);
  f.press();
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].key == "w1");
  CHECK(f.sent[0].action == "turn_on");
  CHECK(f.view().entries[4].summary == "On");  // at once, before Home Assistant answers

  f.select(3);
  f.press();
  REQUIRE(f.sent.size() == 2);
  CHECK(f.sent[1].action == "activate");
  CHECK(f.view().screen == RoomView::Screen::Home);
}

TEST_CASE("a long press on the home screen toggles a light") {
  Fixture f;
  f.hold();
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "turn_off");
  CHECK(f.view().entries[0].summary == "Off");
}

TEST_CASE("a press opens a light; presses step through its controls; a long press goes back") {
  Fixture f;
  f.press();
  RoomView v = f.view();
  CHECK(v.screen == RoomView::Screen::Item);
  CHECK(v.item_name == "Ceiling");
  CHECK(v.control_count == 4);
  CHECK(v.control == "Brightness");
  CHECK(v.value == "50%");
  CHECK(v.fraction == doctest::Approx(0.5));

  f.press();
  CHECK(f.view().control == "Colour temperature");
  CHECK(f.view().value == "3000 K");
  f.press();
  CHECK(f.view().control == "Colour");
  f.press();
  CHECK(f.view().control == "Effect");
  f.press();
  CHECK(f.view().control == "Brightness");  // wraps round

  f.hold();
  CHECK(f.view().screen == RoomView::Screen::Home);
  CHECK(f.sent.empty());
}

TEST_CASE("brightness follows the ring live, at most every 300 ms, and the last value is sent") {
  Fixture f;
  f.press();

  f.rotate(1);  // the first turn goes out at once
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "turn_on");
  CHECK(f.sent[0].args["brightness"] == 55);

  f.after(100);
  f.rotate(1);
  f.after(100);
  f.rotate(1);
  CHECK(f.sent.size() == 1);  // paced
  CHECK(f.view().value == "65%");  // but shown at once

  f.after(100);  // 300 ms after the first
  REQUIRE(f.sent.size() == 2);
  CHECK(f.sent[1].args["brightness"] == 65);

  f.after(1000);
  CHECK(f.sent.size() == 2);  // nothing more to send
}

TEST_CASE("turning brightness down to zero turns the light off") {
  Fixture f;
  f.press();
  f.rotate(-10);
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "turn_off");
  CHECK(f.view().value == "Off");
  f.after(500);
  f.rotate(-1);  // already off: nothing
  f.after(500);
  CHECK(f.sent.size() == 1);
}

TEST_CASE("colour temperature, hue and effect send turn_on with one argument each") {
  Fixture f;
  f.press();
  f.press();  // colour temperature
  f.rotate(2);
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].args == Json{{"color_temp_k", 3200}});

  f.after(400);
  f.press();  // hue
  f.rotate(3);
  REQUIRE(f.sent.size() == 2);
  CHECK(f.sent[1].args["hs"][0] == 30.0);
  CHECK(f.sent[1].args["hs"][1] == 100.0);

  f.after(400);
  f.press();  // effect
  f.rotate(1);
  REQUIRE(f.sent.size() == 3);
  CHECK(f.sent[2].args == Json{{"effect", "Candle"}});
  f.after(400);
  f.rotate(5);  // clamped at the last effect
  REQUIRE(f.sent.size() == 4);
  CHECK(f.sent[3].args == Json{{"effect", "Fireplace"}});
}

TEST_CASE("colour temperature stays within the light's range") {
  Fixture f;
  f.press();
  f.press();
  f.rotate(100);
  CHECK(f.view().value == "6500 K");
  CHECK(f.view().fraction == doctest::Approx(1.0));
}

TEST_CASE("climate sends one intent 1.5 seconds after the ring stops") {
  Fixture f;
  f.select(1);
  f.press();
  CHECK(f.view().control == "Target");
  CHECK(f.view().value == "21.0C");

  f.rotate(1);
  f.after(500);
  f.rotate(1);
  f.after(500);
  f.rotate(1);
  CHECK(f.view().value == "22.5C");
  f.after(1499);
  CHECK(f.sent.empty());
  f.after(1);
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "set_target");
  CHECK(f.sent[0].args["value"] == 22.5);
}

TEST_CASE("the climate target is clamped and snapped to the step") {
  Fixture f;
  f.select(1);
  f.press();
  f.rotate(100);
  CHECK(f.view().value == "30.0C");
  f.rotate(-200);
  CHECK(f.view().value == "5.0C");
}

TEST_CASE("climate mode and preset choices step through their lists") {
  Fixture f;
  f.select(1);
  f.press();
  f.press();
  CHECK(f.view().control == "Mode");
  CHECK(f.view().value == "Heat");
  f.rotate(-1);
  CHECK(f.view().value == "Off");
  f.after(1500);
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "set_hvac_mode");
  CHECK(f.sent[0].args == Json{{"mode", "off"}});

  f.press();
  CHECK(f.view().control == "Preset");
  f.rotate(1);
  f.after(1500);
  REQUIRE(f.sent.size() == 2);
  CHECK(f.sent[1].args == Json{{"preset", "eco"}});
}

TEST_CASE("a cover's position follows the ring live") {
  Fixture f;
  f.select(2);
  f.press();
  CHECK(f.view().control == "Position");
  CHECK(f.view().value == "40%");
  f.rotate(4);
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "set_position");
  CHECK(f.sent[0].args == Json{{"position", 60}});
}

TEST_CASE("a cover without position opens and closes with the ring, and a press stops it") {
  Fixture f;
  f.select(6);
  f.press();
  CHECK(f.view().control == "Open or close");
  f.rotate(1);
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "open");
  CHECK(f.view().value == "Opening");
  f.press();
  REQUIRE(f.sent.size() == 2);
  CHECK(f.sent[1].action == "stop");
  f.rotate(-1);
  CHECK(f.sent.back().action == "close");
}

TEST_CASE("Home Assistant's state takes over two seconds after the last turn") {
  Fixture f;
  f.press();
  f.rotate(2);  // 60% on screen
  // Home Assistant reports an older value meanwhile: the panel keeps its own.
  f.controller.apply_states(Json::parse(R"({"t":"states","items":{"l1":{"brightness":52}}})"));
  CHECK(f.view().value == "60%");
  f.after(Controller::kHold - 1);
  CHECK(f.view().value == "60%");
  f.after(1);
  CHECK(f.view().value == "52%");  // what Home Assistant last said

  // Without a hold, a new state shows at once.
  f.controller.apply_states(Json::parse(R"({"t":"states","items":{"l1":{"brightness":60}}})"));
  CHECK(f.view().value == "60%");
}

TEST_CASE("a failed intent shows Home Assistant's state again, with a message") {
  Fixture f;
  f.press();
  f.rotate(3);
  f.rotate(1);  // pending
  CHECK(f.view().value == "70%");
  f.controller.intent_failed("l1");
  CHECK(f.view().value == "50%");
  CHECK(f.controller.take_message() == std::optional<std::string>("Couldn't change Ceiling"));
  CHECK_FALSE(f.controller.take_message());
  f.after(1000);
  CHECK(f.sent.size() == 1);  // the pending value was dropped
}

TEST_CASE("a toggle drops brightness still waiting to be sent") {
  Fixture f;
  f.press();
  f.rotate(1);
  f.rotate(1);  // waiting
  f.hold();     // back home
  f.hold();     // toggle off
  REQUIRE(f.sent.size() == 2);
  CHECK(f.sent[1].action == "turn_off");
  f.after(1000);
  CHECK(f.sent.size() == 2);
}

TEST_CASE("while disconnected nothing is sent, and nothing is queued for later") {
  Fixture f;
  f.controller.set_connected(false);
  CHECK(f.view().offline);
  f.select(4);
  f.press();
  f.hold();
  CHECK(f.sent.empty());
  f.controller.set_connected(true);
  f.after(1000);
  CHECK(f.sent.empty());
}

TEST_CASE("unavailable items ignore input") {
  Fixture f;
  f.controller.apply_states(Json::parse(R"({"t":"states","items":{"w1":{"available":false}}})"));
  f.select(4);
  CHECK(f.view().entries[4].summary == "Unavailable");
  f.press();
  CHECK(f.sent.empty());
}

TEST_CASE("a new layout without the open item goes back home") {
  Fixture f;
  f.press();
  CHECK(f.view().screen == RoomView::Screen::Item);
  std::string error;
  REQUIRE(f.controller.apply_layout(
      Json::parse(R"({"rev":2,"title":"Bedroom","items":[{"key":"w1","kind":"switch","name":"Fan"}]})"),
      error));
  CHECK(f.view().screen == RoomView::Screen::Home);
  CHECK(f.view().title == "Bedroom");
  CHECK(f.view().selected == 0);
}

TEST_CASE("the version changes with every visible change") {
  Fixture f;
  const unsigned before = f.controller.version();
  f.rotate(1);
  CHECK(f.controller.version() > before);
}

TEST_CASE("controls follow the features") {
  std::string error;
  Room room;
  REQUIRE(room.apply_layout(layout(), error));
  CHECK(controls_for(*room.find("w1")) == std::vector<Control>{Control::Power});
  CHECK(controls_for(*room.find("s1")).empty());
  CHECK(controls_for(*room.find("t1")).empty());
  CHECK(controls_for(*room.find("c1")) ==
        std::vector<Control>{Control::Target, Control::HvacMode, Control::Preset});
}
