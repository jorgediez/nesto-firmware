// Settings → Wi-Fi's screens: choosing a network, typing its password,
// joining, and going back.
#include <doctest/doctest.h>

#include <algorithm>

#include "room/WifiSetup.hpp"

using namespace nesto;
using namespace nesto::room;
using Screen = RoomView::Screen;

namespace {

struct Setup {
  int scans = 0;
  std::vector<WifiConnect> joined;
  std::vector<std::string> forgotten;
  WifiSetup wifi{WifiSetup::Actions{[this] { ++scans; }, [this](const WifiConnect& c) { joined.push_back(c); },
                                    [this](const WifiNetwork& n) { forgotten.push_back(n.name); }}};

  Setup() {
    wifi.open();
    wifi.set_networks({{"/home", "Home", WifiSecurity::Wpa, "WPA Personal", 80, true, true},
                       {"/next", "Next door", WifiSecurity::Wpa, "WPA Personal", 50, false, false},
                       {"/cafe", "Cafe", WifiSecurity::Open, "Open", 40, false, false},
                       {"/work", "Work", WifiSecurity::Unsupported, "Enterprise", 30, false, false},
                       {"/old", "Old flat", WifiSecurity::Wpa, "WPA Personal", 20, true, false}},
                      true);
  }
  RoomView view() const { return wifi.view(); }
  std::string message() {
    auto m = wifi.take_message();
    return m ? *m : "";
  }
  /// Types `text` on the keyboard: each character found round the ring.
  void type(const std::string& text) {
    for (char c : text) {
      const auto keys = view().keys;
      const auto it = std::find_if(keys.begin(), keys.end(), [&](const RingKeyboard::Key& k) {
        return k.special == RingKeyboard::Special::None && k.text == std::string(1, c);
      });
      REQUIRE(it != keys.end());
      wifi.rotate(static_cast<int>(it - keys.begin()) - view().selected);
      wifi.press();
    }
  }
  void done() {
    wifi.rotate(-view().selected);  // OK is the first key
    wifi.press();
  }
};

}  // namespace

TEST_CASE("opening scans, and the list shows each network and Other network") {
  Setup s;
  CHECK(s.scans == 1);
  const RoomView v = s.view();
  CHECK(v.screen == Screen::WifiList);
  CHECK(v.title == "Wi-Fi");
  REQUIRE(v.entries.size() == 6);
  CHECK(v.entries[0].summary == "Connected");
  CHECK(v.entries[0].tone == Tone::Active);
  CHECK(v.entries[0].icon == "wifi-4");
  CHECK(v.entries[1].summary == "WPA Personal");
  CHECK(v.entries[2].summary == "Open");
  CHECK(v.entries[3].summary == "Enterprise, not supported");
  CHECK_FALSE(v.entries[3].available);
  CHECK(v.entries[4].summary == "Saved");
  CHECK(v.entries[5].name == "Other network");
}

TEST_CASE("before the first scan, a placeholder the selection skips") {
  int scans = 0;
  WifiSetup w{WifiSetup::Actions{[&] { ++scans; }, {}, {}}};
  w.open();
  RoomView v = w.view();
  REQUIRE(v.entries.size() == 2);
  CHECK(v.entries[1].name == "Other network");
  CHECK(v.selected == 1);
}

TEST_CASE("a network with a password: typed, then joined") {
  Setup s;
  s.wifi.rotate(1);  // Next door
  s.wifi.press();
  RoomView v = s.view();
  CHECK(v.screen == Screen::Keyboard);
  CHECK(v.item_name == "Next door");
  CHECK(v.icon == "lock");
  s.type("short");
  s.done();
  CHECK(s.message() == "Passwords have 8 to 63 characters");
  CHECK(s.joined.empty());
  s.type("erone");
  CHECK(s.view().value == "shorterone");
  s.done();
  REQUIRE(s.joined.size() == 1);
  CHECK(s.joined[0].path == "/next");
  CHECK(s.joined[0].passphrase == "shorterone");
  CHECK(s.view().screen == Screen::WifiBusy);
  CHECK(s.wifi.joining());
  // Input waits while joining.
  s.wifi.long_press();
  CHECK(s.wifi.joining());

  s.wifi.set_state(WifiState::Connected);
  CHECK(s.message() == "Connected to Next door");
  CHECK(s.view().screen == Screen::WifiList);
}

TEST_CASE("a failed join goes back to the password, as typed") {
  Setup s;
  s.wifi.rotate(1);
  s.wifi.press();
  s.type("wrongpass");
  s.done();
  s.wifi.set_state(WifiState::Failed);
  CHECK(s.message() == "Couldn't join Next door");
  CHECK(s.view().screen == Screen::Keyboard);
  CHECK(s.view().value == "wrongpass");
}

TEST_CASE("an open network joins at once; an unsupported one doesn't") {
  Setup s;
  s.wifi.rotate(3);  // Work
  s.wifi.press();
  CHECK(s.message() == "Enterprise isn't supported");
  s.wifi.rotate(-1);  // Cafe
  s.wifi.press();
  REQUIRE(s.joined.size() == 1);
  CHECK(s.joined[0].passphrase.empty());
  s.wifi.set_state(WifiState::Failed);
  CHECK(s.view().screen == Screen::WifiList);  // nothing to correct
}

TEST_CASE("the network in use can only be forgotten") {
  Setup s;
  s.wifi.press();  // Home, connected
  RoomView v = s.view();
  CHECK(v.title == "Home");
  REQUIRE(v.entries.size() == 1);
  CHECK(v.entries[0].name == "Forget");
  CHECK(v.entries[0].summary == "Disconnects it");
  s.wifi.long_press();  // changed my mind
  CHECK(s.view().title == "Wi-Fi");
  CHECK(s.forgotten.empty());
  s.wifi.press();
  s.wifi.press();
  CHECK(s.forgotten == std::vector<std::string>{"Home"});
  CHECK(s.message() == "Forgot Home");
  CHECK(s.view().title == "Wi-Fi");
}

TEST_CASE("a saved network: join it with its password, or forget it") {
  Setup s;
  s.wifi.rotate(4);  // Old flat
  s.wifi.press();
  RoomView v = s.view();
  REQUIRE(v.entries.size() == 2);
  CHECK(v.entries[0].name == "Join");
  CHECK(v.entries[1].name == "Forget");
  s.wifi.press();  // Join
  REQUIRE(s.joined.size() == 1);
  CHECK(s.joined[0].path == "/old");
  CHECK(s.joined[0].passphrase.empty());  // connman's own
  // Its password changed: asked for anew.
  s.wifi.set_state(WifiState::Failed);
  CHECK(s.view().screen == Screen::Keyboard);
  CHECK(s.view().value.empty());
  s.wifi.long_press();
  s.wifi.press();
  s.wifi.rotate(1);
  s.wifi.press();  // Forget
  CHECK(s.forgotten == std::vector<std::string>{"Old flat"});
}

TEST_CASE("a hidden network: its name, its security, then its password") {
  Setup s;
  s.wifi.rotate(10);  // Other network
  s.wifi.press();
  CHECK(s.view().screen == Screen::Keyboard);
  CHECK(s.view().title == "Network name");
  s.done();
  CHECK(s.message() == "Type the network's name");
  s.type("attic");
  s.done();
  RoomView v = s.view();
  CHECK(v.screen == Screen::WifiList);
  CHECK(v.title == "Security");
  s.wifi.long_press();  // back to the name, kept
  CHECK(s.view().value == "attic");
  s.done();
  s.wifi.press();  // WPA Personal
  CHECK(s.view().title == "Password");
  s.type("atticpass");
  s.done();
  REQUIRE(s.joined.size() == 1);
  CHECK(s.joined[0].hidden);
  CHECK(s.joined[0].name == "attic");
  CHECK(s.joined[0].path.empty());
}

TEST_CASE("an open hidden network needs no password") {
  Setup s;
  s.wifi.rotate(10);
  s.wifi.press();
  s.type("attic");
  s.done();
  s.wifi.rotate(1);  // Open
  s.wifi.press();
  REQUIRE(s.joined.size() == 1);
  CHECK(s.joined[0].security == WifiSecurity::Open);
}

TEST_CASE("holding goes back a step, and out") {
  Setup s;
  s.wifi.rotate(1);
  s.wifi.press();
  s.wifi.long_press();
  CHECK(s.view().screen == Screen::WifiList);
  s.wifi.long_press();
  CHECK_FALSE(s.wifi.active());
}

TEST_CASE("the selection follows its network as the list changes") {
  Setup s;
  s.wifi.rotate(2);  // Cafe
  s.wifi.set_networks({{"/cafe", "Cafe", WifiSecurity::Open, "Open", 40, false, false},
                       {"/home", "Home", WifiSecurity::Wpa, "WPA Personal", 80, true, true}},
                      true);
  CHECK(s.view().entries[static_cast<std::size_t>(s.view().selected)].name == "Cafe");
}
