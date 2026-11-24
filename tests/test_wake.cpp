// The item the panel wakes on, and Home Assistant's `show` (PROTOCOL.md
// sections 9 and 13).
#include <doctest/doctest.h>

#include "room/Controller.hpp"
#include "room/Notifications.hpp"

using namespace nesto;
using namespace nesto::room;

namespace {

Json layout(const Json& wake) {
  Json out = Json::parse(R"({
    "t": "layout", "rev": 1, "title": "Lounge",
    "items": [
      {"key": "l1", "kind": "light", "name": "Lamp", "features": {"brightness": true}},
      {"key": "w1", "kind": "switch", "name": "Plug"},
      {"key": "v9", "kind": "vacuum", "name": "Robot"},
      {"key": "c1", "kind": "climate", "name": "Radiator",
       "features": {"target": "single", "min": 5, "max": 30, "step": 0.5, "unit": "°C"}}
    ]})");
  if (!wake.is_null()) out["wake"] = wake;
  return out;
}

struct Fixture {
  std::vector<Intent> sent;
  Controller controller{[this](const Intent& i) { sent.push_back(i); }};
  Millis now = 10000;

  explicit Fixture(const Json& wake = nullptr) {
    std::string error;
    REQUIRE(controller.apply_layout(layout(wake), error));
    controller.apply_states(Json::parse(R"({"t": "states", "full": true, "items": {
        "l1": {"on": true, "brightness": 60}, "w1": {"on": false},
        "c1": {"hvac_mode": "heat", "target": 21}}})"));
    controller.set_connected(true);
  }
  RoomView view() const { return controller.view(); }
  /// The selected entry's name on the home screen.
  std::string selected() const {
    const RoomView v = view();
    return v.entries.at(static_cast<std::size_t>(v.selected)).name;
  }
};

}  // namespace

TEST_CASE("the layout's wake key parses") {
  std::string error;
  auto parsed = parse_layout(layout("c1"), error);
  REQUIRE(parsed);
  CHECK(parsed->wake == "c1");
  parsed = parse_layout(layout(nullptr), error);
  REQUIRE(parsed);
  CHECK(parsed->wake.empty());
  CHECK_FALSE(parse_layout(layout(3), error));
  CHECK(error == "`wake` must be an item's key");
}

TEST_CASE("waking selects the wake item, unless a notification is unread") {
  Fixture f("c1");
  f.controller.rotate(kTicksPerClick, f.now);
  f.controller.press(f.now);  // Plug switches; still on the home screen
  f.controller.go_home();
  CHECK(f.selected() == "Radiator");

  // Read notifications only: the wake item, past the Notifications entry.
  f.controller.set_notifications(2, 0);
  f.controller.go_home();
  CHECK(f.selected() == "Radiator");
  // An unread one comes first.
  f.controller.set_notifications(3, 1);
  f.controller.go_home();
  CHECK(f.selected() == "Notifications");
}

TEST_CASE("without a wake item, or with one that's gone, waking goes to the top") {
  Fixture f;
  f.controller.rotate(2 * kTicksPerClick, f.now);
  f.controller.go_home();
  CHECK(f.selected() == "Lamp");

  Fixture skipped("v9");  // a kind this panel skips
  skipped.controller.go_home();
  CHECK(skipped.selected() == "Lamp");
  Fixture unknown("zz");
  unknown.controller.go_home();
  CHECK(unknown.selected() == "Lamp");
  unknown.controller.set_notifications(1, 0);
  unknown.controller.go_home();
  CHECK(unknown.selected() == "Notifications");
}

TEST_CASE("show selects an item, and opens it when a press would") {
  Fixture f;
  f.controller.set_notifications(1, 1);
  CHECK(f.controller.show_item("c1", false));
  CHECK(f.view().screen == RoomView::Screen::Home);
  CHECK(f.selected() == "Radiator");

  CHECK(f.controller.show_item("l1", true));
  CHECK(f.view().screen == RoomView::Screen::Item);
  CHECK(f.view().item_name == "Lamp");
  CHECK(f.view().control == "Brightness");
  // Back goes to the home screen, on the item.
  f.controller.long_press(f.now);
  CHECK(f.view().screen == RoomView::Screen::Home);
  CHECK(f.selected() == "Lamp");

  // A switch isn't opened, only selected, and nothing is sent.
  CHECK(f.controller.show_item("w1", true));
  CHECK(f.view().screen == RoomView::Screen::Home);
  CHECK(f.selected() == "Plug");
  CHECK(f.sent.empty());
}

TEST_CASE("show closes Settings, and an unknown key changes nothing") {
  Fixture f;
  f.controller.rotate(-10 * kTicksPerClick, f.now);
  f.controller.rotate(10 * kTicksPerClick, f.now);  // Settings, last
  f.controller.press(f.now);
  REQUIRE(f.view().screen == RoomView::Screen::Settings);
  CHECK_FALSE(f.controller.has_item("v9"));
  CHECK_FALSE(f.controller.show_item("v9", true));
  CHECK(f.view().screen == RoomView::Screen::Settings);
  CHECK(f.controller.show_item("c1", true));
  CHECK(f.view().screen == RoomView::Screen::Item);
  CHECK(f.view().control == "Target");
}

TEST_CASE("show closes the notification list, not a pop-up") {
  std::vector<Json> events;
  Notifications n{[&](const Json& e) { events.push_back(e); }};
  Notification a;
  a.nid = "a";
  a.message = "Kept";
  n.add(a, false, 1000);
  n.open_list();
  REQUIRE(n.active());
  n.close_list();
  CHECK_FALSE(n.active());

  Notification b = a;
  b.nid = "b";
  b.message = "Now";
  n.add(b, true, 2000);
  REQUIRE(n.active());
  n.close_list();
  CHECK(n.active());
}
