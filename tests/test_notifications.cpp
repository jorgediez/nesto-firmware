#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "room/Notifications.hpp"

using nesto::protocol::Json;
using nesto::room::Notification;
using nesto::room::Notifications;
using nesto::room::Priority;
using Screen = nesto::room::RoomView::Screen;

namespace {

struct Fixture {
  std::vector<Json> events;
  Notifications n{[this](const Json& e) { events.push_back(e); }};

  /// The events since the last call, as "nid event[ action]".
  std::vector<std::string> take() {
    std::vector<std::string> out;
    for (const Json& e : events) {
      std::string s = e["nid"].get<std::string>() + " " + e["event"].get<std::string>();
      if (e.contains("action")) s += " " + e["action"].get<std::string>();
      out.push_back(s);
    }
    events.clear();
    return out;
  }
};

Notification make(const std::string& nid, Priority priority = Priority::Normal, unsigned duration = 15) {
  Notification out;
  out.nid = nid;
  out.title = "Title " + nid;
  out.message = "Message " + nid;
  out.priority = priority;
  out.duration_s = duration;
  return out;
}

using Events = std::vector<std::string>;

}  // namespace

TEST_CASE("notify requests are checked") {
  Notification n;
  std::string error;
  CHECK(Notification::parse(Json{{"nid", "door-1"}, {"message", "Someone is at the door"}}, n, error));
  CHECK(n.priority == Priority::Normal);
  CHECK(n.duration_s == 15);

  const Json full = Json::parse(R"({"nid": "door-1", "title": "Front door", "message": "Hi", "icon": "door",
      "priority": "critical", "duration_s": 0, "beep": "alert",
      "actions": [{"id": "open", "label": "Open"}, {"id": "ignore", "label": "Ignore"}]})");
  REQUIRE(Notification::parse(full, n, error));
  CHECK(n.priority == Priority::Critical);
  CHECK(n.duration_s == 0);
  REQUIRE(n.actions.size() == 2);
  CHECK(n.actions[1].label == "Ignore");

  for (const char* bad : {R"({"message": "no nid"})", R"({"nid": "a"})", R"({"nid": "a", "message": ""})",
                          R"({"nid": "a", "message": "m", "priority": "urgent"})",
                          R"({"nid": "a", "message": "m", "duration_s": -1})",
                          R"({"nid": "a", "message": "m", "actions": [{"id": "x"}]})",
                          R"({"nid": "a", "message": "m", "actions": [{"id": "1", "label": "1"},
                             {"id": "2", "label": "2"}, {"id": "3", "label": "3"}, {"id": "4", "label": "4"}]})"}) {
    CAPTURE(bad);
    CHECK_FALSE(Notification::parse(Json::parse(bad), n, error));
    CHECK_FALSE(error.empty());
  }
  // 256 characters, counted as characters: "é" is two bytes.
  std::string message;
  for (int i = 0; i < 256; ++i) message += "\xC3\xA9";
  CHECK(Notification::parse(Json{{"nid", "a"}, {"message", message}}, n, error));
  CHECK_FALSE(Notification::parse(Json{{"nid", "a"}, {"message", message + "x"}}, n, error));
}

TEST_CASE("a pop-up shows, then expires into the list") {
  Fixture f;
  f.n.add(make("a"), true, 1000);
  CHECK(f.n.active());
  CHECK(f.n.view().screen == Screen::Notice);
  CHECK(f.n.view().item_name == "Title a");
  CHECK(f.n.view().choices == std::vector<std::string>{"Dismiss"});
  CHECK(f.n.holds_display());
  CHECK(f.take() == Events{"a shown"});

  f.n.tick(1000 + 14999);
  CHECK(f.n.active());
  f.n.tick(1000 + 15000);
  CHECK_FALSE(f.n.active());
  CHECK(f.take() == Events{"a expired"});
  CHECK(f.n.count() == 1);
  CHECK(f.n.unread() == 0);
}

TEST_CASE("low ones don't pop up, and dark the others wait in the list") {
  Fixture f;
  f.n.add(make("low", Priority::Low), true, 0);
  CHECK_FALSE(f.n.active());
  f.n.add(make("normal"), false, 0);  // the display stayed dark
  CHECK_FALSE(f.n.active());
  CHECK(f.n.unread() == 2);
  CHECK(f.take().empty());
  // A timed one doesn't pop up later on waking.
  f.n.display_woke(1000);
  CHECK_FALSE(f.n.active());

  // Opened from the list: shown then.
  f.n.open_list();
  REQUIRE(f.n.view().screen == Screen::Notifications);
  CHECK(f.n.view().entries[0].name == "Title normal");  // newest first
  f.n.rotate(1);
  f.n.press();
  CHECK(f.n.view().screen == Screen::Notice);
  CHECK_FALSE(f.n.holds_display());  // no timer when opened
  CHECK(f.take() == Events{"low shown"});
  f.n.long_press();
  CHECK(f.n.view().screen == Screen::Notifications);
  f.n.long_press();
  CHECK_FALSE(f.n.active());
}

TEST_CASE("the ring chooses an action and a press sends it") {
  Fixture f;
  Notification door = make("door");
  door.actions = {{"open", "Open"}, {"ignore", "Ignore"}};
  f.n.add(door, true, 0);
  f.take();
  CHECK(f.n.view().choices == std::vector<std::string>{"Open", "Ignore", "Dismiss"});
  f.n.rotate(1);
  CHECK(f.n.view().choice == 1);
  f.n.rotate(5);
  CHECK(f.n.view().choice == 2);
  f.n.rotate(-2);
  f.n.press();
  CHECK(f.take() == Events{"door action open"});
  CHECK(f.n.count() == 0);  // answered: gone
  CHECK_FALSE(f.n.active());

  f.n.add(make("b"), true, 0);
  f.take();
  f.n.press();  // only Dismiss
  CHECK(f.take() == Events{"b dismissed"});
  CHECK(f.n.count() == 0);
}

TEST_CASE("a long press keeps it for later, but not a critical one") {
  Fixture f;
  f.n.add(make("a"), true, 0);
  f.n.long_press();
  CHECK_FALSE(f.n.active());
  CHECK(f.n.count() == 1);
  f.take();

  f.n.add(make("alarm", Priority::Critical), true, 0);
  f.n.long_press();
  CHECK(f.n.active());
  CHECK(f.n.view().prompt == "Press to answer");
  CHECK_FALSE(f.n.holds_display());  // no timer: the display may rest
  // Resting, it waits; waking, it's back.
  f.n.display_rested();
  CHECK_FALSE(f.n.active());
  f.n.display_woke(5000);
  CHECK(f.n.active());
  CHECK(f.n.view().item_name == "Title alarm");
  CHECK(f.take() == Events{"alarm shown"});  // shown once
  f.n.press();
  CHECK(f.take() == Events{"alarm dismissed"});
}

TEST_CASE("resting ends a timed pop-up") {
  Fixture f;
  f.n.add(make("a"), true, 0);
  f.n.display_rested();
  CHECK_FALSE(f.n.active());
  CHECK(f.take() == Events{"a shown", "a expired"});
  f.n.display_woke(1000);
  CHECK_FALSE(f.n.active());
}

TEST_CASE("newer pop-ups come first, but don't push a critical one aside") {
  Fixture f;
  f.n.add(make("a"), true, 0);
  f.n.add(make("b"), true, 100);
  CHECK(f.n.view().item_name == "Title b");
  CHECK(f.take() == Events{"a shown", "a expired", "b shown"});

  f.n.add(make("alarm", Priority::Critical), true, 200);
  f.n.add(make("c"), true, 300);
  CHECK(f.n.view().item_name == "Title alarm");
  f.n.press();  // dismissed: nothing else waits, c was timed
  CHECK_FALSE(f.n.active());
  CHECK(f.take() == Events{"b expired", "alarm shown", "alarm dismissed"});
  CHECK(f.n.unread() == 1);  // c, in the list
}

TEST_CASE("only a critical one interrupts one opened from the list") {
  Fixture f;
  f.n.add(make("a", Priority::Low), true, 0);
  f.n.open_list();
  f.n.press();  // opened
  REQUIRE(f.n.view().screen == Screen::Notice);
  f.n.add(make("b"), true, 100);
  CHECK(f.n.view().item_name == "Title a");  // still reading
  f.n.add(make("alarm", Priority::Critical), true, 200);
  CHECK(f.n.view().item_name == "Title alarm");
  CHECK(f.take() == Events{"a shown", "alarm shown"});
  // Answered: back to the list, not to the room.
  f.n.press();
  CHECK(f.n.view().screen == Screen::Notifications);
  CHECK(f.take() == Events{"alarm dismissed"});
  f.n.long_press();
  CHECK_FALSE(f.n.active());

  // A pop-up over the list goes back to it too.
  f.n.open_list();
  f.n.add(make("c"), true, 300);
  CHECK(f.n.view().screen == Screen::Notice);
  f.n.tick(300 + 15000);
  CHECK(f.n.view().screen == Screen::Notifications);
}

TEST_CASE("the same nid replaces, and the list keeps ten") {
  Fixture f;
  f.n.add(make("a"), false, 0);
  Notification again = make("a");
  again.message = "Updated";
  f.n.add(again, false, 0);
  CHECK(f.n.count() == 1);

  f.n.add(make("alarm", Priority::Critical), false, 0);
  for (int i = 0; i < 10; ++i) f.n.add(make("n" + std::to_string(i)), false, 0);
  CHECK(f.n.count() == 10);
  f.n.open_list();
  const auto entries = f.n.view().entries;
  // "a" went first; the critical one, older than the rest, stayed.
  CHECK(entries.back().name == "Title alarm");
  for (const auto& e : entries) CHECK(e.name != "Title a");
}

TEST_CASE("clearing removes without events") {
  Fixture f;
  f.n.add(make("a"), true, 0);
  f.n.add(make("b", Priority::Low), true, 0);
  f.take();
  f.n.clear("a");
  CHECK_FALSE(f.n.active());
  CHECK(f.n.count() == 1);
  f.n.clear_all();
  CHECK(f.n.count() == 0);
  CHECK(f.take().empty());
}

TEST_CASE("with only Dismiss, the ring scrolls the message") {
  Fixture f;
  f.n.add(make("a", Priority::Normal, 0), true, 0);
  f.n.set_scroll_limit(2);
  f.n.rotate(5);
  CHECK(f.n.view().scroll == 2);
  f.n.rotate(-1);
  CHECK(f.n.view().scroll == 1);
}
