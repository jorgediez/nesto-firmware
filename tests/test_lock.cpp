// Locks (PROTOCOL.md sections 9.1, 10 and 11): locking at once, and unlocking
// or opening only once a press on the Confirm screen says so.
#include <doctest/doctest.h>

#include <algorithm>

#include "room/Controller.hpp"

using namespace nesto;
using namespace nesto::room;

namespace {

Json layout(bool with_front_door = true) {
  Json items = Json::array();
  if (with_front_door) {
    items.push_back({{"key", "k1"}, {"kind", "lock"}, {"name", "Front door"},
                     {"features", {{"open", true}}}});
  }
  items.push_back({{"key", "k2"}, {"kind", "lock"}, {"name", "Back door"}});
  items.push_back({{"key", "k3"}, {"kind", "lock"}, {"name", "Garage door"}});
  return Json{{"t", "layout"}, {"rev", 1}, {"title", "Hall"}, {"items", items}};
}

Json states() {
  return Json::parse(R"({
    "t": "states", "full": true,
    "items": {
      "k1": {"state": "locked"},
      "k2": {"state": "unlocked"},
      "k3": {"state": "jammed"}
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

  void rotate(int clicks) { controller.rotate(clicks * kTicksPerClick, now); }
  void press() { controller.press(now); }
  void hold() { controller.long_press(now); }
  void select(int index) {
    rotate(-10);
    rotate(index);
  }
  RoomView view() const { return controller.view(); }
};

}  // namespace

TEST_CASE("a lock's features and state parse") {
  std::string error;
  const auto parsed = parse_layout(layout(), error);
  REQUIRE(parsed);
  REQUIRE(parsed->items.size() == 3);
  CHECK(parsed->items[0].kind == Kind::Lock);
  CHECK(parsed->items[0].lock.open);
  CHECK_FALSE(parsed->items[1].lock.open);

  ItemState s;
  merge_state(s, Json::parse(R"({"state": "jammed"})"));
  CHECK(s.state == "jammed");
  const auto kinds = kind_names();
  CHECK(std::find(kinds.begin(), kinds.end(), "lock") != kinds.end());
}

TEST_CASE("a lock's controls: Lock, and Open if it can open the latch") {
  std::string error;
  const auto parsed = parse_layout(layout(), error);
  REQUIRE(parsed);
  CHECK(controls_for(parsed->items[0]) == std::vector<Control>{Control::Lock, Control::Open});
  CHECK(controls_for(parsed->items[1]) == std::vector<Control>{Control::Lock});
}

TEST_CASE("the home screen shows a lock's state, tone and icon") {
  Fixture f;
  const auto entries = f.view().entries;
  REQUIRE(entries.size() == 4);  // three locks and Settings
  CHECK(entries[0].summary == "Locked");
  CHECK(entries[0].tone == Tone::Off);
  CHECK(entries[0].icon == "lock");
  CHECK(entries[1].summary == "Unlocked");
  CHECK(entries[1].tone == Tone::Active);
  CHECK(entries[1].icon == "lock-open");
  CHECK(entries[1].default_icon == "lock");
  CHECK(entries[2].summary == "Jammed");
  CHECK(entries[2].tone == Tone::Alert);
  CHECK(entries[2].icon == "lock");
}

TEST_CASE("turning right asks before unlocking; holding keeps it locked") {
  Fixture f;
  f.press();
  CHECK(f.view().control == "Lock");
  CHECK(f.view().value == "Locked");
  f.rotate(1);
  RoomView v = f.view();
  CHECK(v.screen == RoomView::Screen::Confirm);
  CHECK(v.title == "Hall");
  CHECK(v.item_name == "Unlock Front door?");
  CHECK(v.prompt == "Press to unlock it,\nhold to keep it locked");
  f.rotate(3);  // the ring does nothing here
  CHECK(f.sent.empty());

  f.hold();
  v = f.view();
  CHECK(v.screen == RoomView::Screen::Item);
  CHECK(v.control == "Lock");
  CHECK(v.value == "Locked");
  CHECK(f.sent.empty());
}

TEST_CASE("a press on the question unlocks, and turning left locks at once") {
  Fixture f;
  f.press();
  f.rotate(1);
  f.press();
  CHECK(f.view().screen == RoomView::Screen::Item);
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].key == "k1");
  CHECK(f.sent[0].action == "unlock");
  CHECK(f.sent[0].args == Json::object());
  CHECK(f.view().value == "Unlocking");
  CHECK(f.view().icon == "lock-open");
  CHECK(f.view().tone == Tone::Active);

  f.rotate(1);  // unlocking already: nothing to ask
  CHECK(f.view().screen == RoomView::Screen::Item);
  f.rotate(-1);
  REQUIRE(f.sent.size() == 2);
  CHECK(f.sent[1].action == "lock");
  CHECK(f.view().value == "Locking");
  f.rotate(-1);  // locking already
  CHECK(f.sent.size() == 2);
}

TEST_CASE("Open asks too, and opens the latch") {
  Fixture f;
  f.press();
  f.press();
  CHECK(f.view().control == "Open");
  CHECK(f.view().value == "Closed");
  f.rotate(-1);  // nothing to close
  CHECK(f.view().screen == RoomView::Screen::Item);
  f.rotate(1);
  CHECK(f.view().item_name == "Open Front door?");
  CHECK(f.view().prompt == "Press to open it,\nhold to leave it");
  f.press();
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "open");
  CHECK(f.view().control == "Open");
  CHECK(f.view().value == "Opening");
}

TEST_CASE("an unlocked lock doesn't ask; a jammed one can be locked again") {
  Fixture f;
  f.select(1);
  f.press();
  f.rotate(1);
  CHECK(f.view().screen == RoomView::Screen::Item);
  CHECK(f.sent.empty());

  f.hold();
  f.select(2);
  f.press();
  CHECK(f.view().value == "Jammed");
  f.rotate(-1);
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].key == "k3");
  CHECK(f.sent[0].action == "lock");
}

TEST_CASE("holding on the home screen locks, or asks before unlocking") {
  Fixture f;
  f.hold();  // the front door, locked: asks
  CHECK(f.view().screen == RoomView::Screen::Confirm);
  CHECK(f.view().item_name == "Unlock Front door?");
  f.press();
  CHECK(f.view().screen == RoomView::Screen::Home);
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "unlock");

  f.select(1);  // the back door, unlocked: locks at once
  f.hold();
  CHECK(f.view().screen == RoomView::Screen::Home);
  REQUIRE(f.sent.size() == 2);
  CHECK(f.sent[1].key == "k2");
  CHECK(f.sent[1].action == "lock");

  f.select(0);
  f.hold();
  f.hold();  // asked again; holding leaves it, back home
  CHECK(f.view().screen == RoomView::Screen::Home);
  CHECK(f.sent.size() == 2);
}

TEST_CASE("the question goes with its lock, and does nothing offline") {
  Fixture f;
  f.press();
  f.rotate(1);
  std::string error;
  REQUIRE(f.controller.apply_layout(layout(false), error));
  CHECK(f.view().screen == RoomView::Screen::Home);

  Fixture g;
  g.press();
  g.rotate(1);
  g.controller.set_connected(false);
  g.press();
  CHECK(g.view().screen == RoomView::Screen::Item);
  CHECK(g.sent.empty());
}
