// The Settings menu: how it's reached, what each entry does, and going back.
#include <doctest/doctest.h>

#include "room/Controller.hpp"

using namespace nesto;
using namespace nesto::room;

namespace {

struct Menu {
  int pairing_opened = 0;
  int forgotten = 0;
  std::vector<Intent> sent;
  Controller controller{[this](const Intent& i) { sent.push_back(i); },
                        SettingsActions{[this] { ++pairing_opened; }, [this] { ++forgotten; },
                                        [] { return std::vector<std::string>{"Nesto 0.1.0", "Panel x"}; }}};

  void with_room() {
    std::string error;
    REQUIRE(controller.apply_layout(
        Json::parse(R"({"rev":1,"title":"Room","items":[
          {"key":"w1","kind":"switch","name":"Fan"},
          {"key":"l1","kind":"light","name":"Lamp","features":{"brightness":true}}]})"),
        error));
    controller.set_connected(true);
  }
  void rotate(int steps) { controller.rotate(steps, 0); }
  void press() { controller.press(0); }
  void hold() { controller.long_press(0); }
  RoomView view() const { return controller.view(); }
};

}  // namespace

TEST_CASE("Settings is the last entry on the home screen") {
  Menu m;
  m.with_room();
  const RoomView v = m.view();
  REQUIRE(v.entries.size() == 3);
  CHECK(v.entries.back().name == "Settings");
  m.rotate(5);
  CHECK(m.view().selected == 2);
  m.hold();  // a long press on Settings does nothing
  CHECK(m.view().screen == RoomView::Screen::Home);
  m.press();
  CHECK(m.view().screen == RoomView::Screen::Settings);
  CHECK(m.sent.empty());
}

TEST_CASE("a press on the waiting screen opens Settings, and holding goes back to it") {
  Menu m;
  CHECK(m.view().screen == RoomView::Screen::Empty);
  m.press();
  const RoomView v = m.view();
  CHECK(v.screen == RoomView::Screen::Settings);
  CHECK(v.title == "Settings");
  REQUIRE(v.entries.size() == 3);
  CHECK(v.entries[0].name == "Pair");
  CHECK(v.entries[0].summary == "Pair with Home Assistant");
  CHECK(v.entries[1].name == "Forget");
  CHECK(v.entries[1].summary == "Forget Home Assistant");
  CHECK(v.entries[2].name == "About");
  m.hold();
  CHECK(m.view().screen == RoomView::Screen::Empty);
}

TEST_CASE("Pair with Home Assistant asks the panel to open pairing") {
  Menu m;
  m.press();
  m.press();
  CHECK(m.pairing_opened == 1);
  CHECK(m.view().screen == RoomView::Screen::Settings);  // stays, so it can be used again
}

TEST_CASE("Forget asks first: holding keeps Home Assistant") {
  Menu m;
  m.with_room();
  m.rotate(5);
  m.press();     // Settings
  m.rotate(1);   // Forget
  m.press();
  RoomView v = m.view();
  CHECK(v.screen == RoomView::Screen::Confirm);
  CHECK(v.item_name == "Forget Home Assistant?");
  CHECK_FALSE(v.prompt.empty());
  m.hold();
  CHECK(m.view().screen == RoomView::Screen::Settings);
  CHECK(m.forgotten == 0);
}

TEST_CASE("Forget, confirmed, forgets and empties the room") {
  Menu m;
  m.with_room();
  m.rotate(5);
  m.press();
  m.rotate(1);
  m.press();
  m.press();  // confirm
  CHECK(m.forgotten == 1);
  CHECK(m.view().screen == RoomView::Screen::Empty);
  CHECK(m.view().entries.empty());
}

TEST_CASE("About shows the panel's lines; a press or a hold goes back") {
  Menu m;
  m.press();
  m.rotate(2);
  m.press();
  RoomView v = m.view();
  CHECK(v.screen == RoomView::Screen::About);
  CHECK(v.lines == std::vector<std::string>{"Nesto 0.1.0", "Panel x"});
  m.press();
  CHECK(m.view().screen == RoomView::Screen::Settings);
  m.rotate(2);
  m.press();
  m.hold();
  CHECK(m.view().screen == RoomView::Screen::Settings);
}

TEST_CASE("the Settings selection stays within the menu") {
  Menu m;
  m.press();
  m.rotate(10);
  CHECK(m.view().selected == 2);
  m.rotate(-10);
  CHECK(m.view().selected == 0);
}

TEST_CASE("leaving Settings goes back home when there's a room") {
  Menu m;
  m.with_room();
  m.rotate(5);
  m.press();
  m.hold();
  CHECK(m.view().screen == RoomView::Screen::Home);
}

TEST_CASE("a layout arriving while in Settings doesn't leave it") {
  Menu m;
  m.press();
  m.with_room();
  CHECK(m.view().screen == RoomView::Screen::Settings);
  m.hold();
  CHECK(m.view().screen == RoomView::Screen::Home);
}
