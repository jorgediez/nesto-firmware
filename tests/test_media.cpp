// Media players (PROTOCOL.md sections 9.1, 10 and 11): parsing, the controls
// the ring steps through, and the intents they send.
#include <doctest/doctest.h>

#include <algorithm>

#include "room/Controller.hpp"

using namespace nesto;
using namespace nesto::room;

namespace {

Json layout() {
  return Json::parse(R"({
    "t": "layout", "rev": 1, "title": "Lounge",
    "items": [
      {"key": "m1", "kind": "media_player", "name": "Speaker",
       "features": {"volume": true, "mute": true, "play_pause": true, "tracks": true,
                    "sources": ["Radio", "Turntable"], "power": true}},
      {"key": "m2", "kind": "media_player", "name": "TV", "icon": "tv",
       "features": {"power": true}},
      {"key": "m3", "kind": "media_player", "name": "Cast"}
    ]})");
}

Json states() {
  return Json::parse(R"({
    "t": "states", "full": true,
    "items": {
      "m1": {"state": "paused", "volume": 30, "muted": false, "title": "Morning tide",
             "artist": "The Keepers", "source": "Radio"},
      "m2": {"state": "off"},
      "m3": {"state": "idle"}
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

TEST_CASE("a media player's features and state parse") {
  std::string error;
  const auto parsed = parse_layout(layout(), error);
  REQUIRE(parsed);
  REQUIRE(parsed->items.size() == 3);
  const auto& m = parsed->items[0].media;
  CHECK(parsed->items[0].kind == Kind::MediaPlayer);
  CHECK(m.volume);
  CHECK(m.mute);
  CHECK(m.play_pause);
  CHECK(m.tracks);
  CHECK(m.power);
  CHECK(m.sources == std::vector<std::string>{"Radio", "Turntable"});
  CHECK_FALSE(parsed->items[2].media.power);

  ItemState s;
  merge_state(s, Json::parse(R"({"state": "playing", "volume": 140, "muted": true,
                                 "title": "A", "artist": "B", "source": "Radio"})"));
  CHECK(s.state == "playing");
  CHECK(s.volume == 100);
  CHECK(s.muted);
  CHECK(s.title == "A");
  CHECK(s.artist == "B");
  CHECK(s.source == "Radio");
  merge_state(s, Json::parse(R"({"state": null, "title": null})"));
  CHECK(s.state.empty());  // unknown
  CHECK_FALSE(s.title);
  const auto kinds = kind_names();
  CHECK(std::find(kinds.begin(), kinds.end(), "media_player") != kinds.end());
}

TEST_CASE("a media player's controls follow its features") {
  std::string error;
  const auto parsed = parse_layout(layout(), error);
  REQUIRE(parsed);
  CHECK(controls_for(parsed->items[0]) ==
        std::vector<Control>{Control::Volume, Control::Playback, Control::Track,
                             Control::Source, Control::Mute, Control::Power});
  CHECK(controls_for(parsed->items[1]) == std::vector<Control>{Control::Power});
  CHECK(controls_for(parsed->items[2]).empty());
}

TEST_CASE("the home screen shows what's playing, and its tone") {
  Fixture f;
  auto entries = f.view().entries;
  REQUIRE(entries.size() == 4);  // three players and Settings
  CHECK(entries[0].summary == "Paused - Morning tide");
  CHECK(entries[0].tone == Tone::Neutral);
  CHECK(entries[0].icon == "speaker");
  CHECK(entries[1].summary == "Off");
  CHECK(entries[1].tone == Tone::Off);
  CHECK(entries[1].icon == "tv");
  CHECK(entries[2].summary == "Idle");

  f.controller.apply_states(Json::parse(
      R"({"t": "states", "items": {"m1": {"state": "playing"}, "m3": {"state": null}}})"));
  entries = f.view().entries;
  CHECK(entries[0].summary == "Playing - Morning tide");
  CHECK(entries[0].tone == Tone::Active);
  CHECK(entries[2].summary == "Unknown");
  CHECK(entries[2].tone == Tone::Off);
}

TEST_CASE("volume follows every tick, paced like a light") {
  Fixture f;
  f.press();
  CHECK(f.view().control == "Volume");
  CHECK(f.view().value == "30%");
  f.turn(1);
  f.turn(1);
  f.turn(1);
  CHECK(f.view().value == "33%");
  REQUIRE(f.sent.size() == 1);  // the first at once, the rest paced
  CHECK(f.sent[0].action == "set_volume");
  CHECK(f.sent[0].args == Json{{"volume", 31}});
  f.after(400);
  CHECK(f.sent.back().args == Json{{"volume", 33}});
}

TEST_CASE("playback, tracks, source and mute") {
  Fixture f;
  f.press();
  f.press();
  CHECK(f.view().control == "Playback");
  CHECK(f.view().value == "Paused");
  f.rotate(-1);  // paused already
  CHECK(f.sent.empty());
  f.rotate(1);
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "play");
  CHECK(f.view().value == "Playing");

  f.press();
  CHECK(f.view().control == "Track");
  CHECK(f.view().value == "Morning tide");
  f.rotate(2);  // two clicks: two tracks on
  f.rotate(-1);
  REQUIRE(f.sent.size() == 4);
  CHECK(f.sent[1].action == "next");
  CHECK(f.sent[2].action == "next");
  CHECK(f.sent[3].action == "previous");

  f.press();
  CHECK(f.view().control == "Source");
  CHECK(f.view().value == "Radio");
  f.rotate(1);
  CHECK(f.view().value == "Turntable");
  CHECK(f.sent.size() == 4);  // waits for the ring to stop
  f.after(1600);
  REQUIRE(f.sent.size() == 5);
  CHECK(f.sent[4].action == "select_source");
  CHECK(f.sent[4].args == Json{{"source", "Turntable"}});

  f.press();
  CHECK(f.view().control == "Mute");
  CHECK(f.view().value == "Off");
  f.rotate(1);
  REQUIRE(f.sent.size() == 6);
  CHECK(f.sent[5].action == "mute");
  CHECK(f.sent[5].args == Json{{"muted", true}});
  CHECK(f.view().value == "On");

  f.press();
  CHECK(f.view().control == "Power");
  CHECK(f.view().value == "On");
  f.rotate(-1);
  REQUIRE(f.sent.size() == 7);
  CHECK(f.sent[6].action == "turn_off");
  CHECK(f.view().value == "Off");
}

TEST_CASE("holding on the home screen plays or pauses, or switches on or off") {
  Fixture f;
  f.hold();  // the speaker, paused
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].action == "play");
  f.hold();
  REQUIRE(f.sent.size() == 2);
  CHECK(f.sent[1].action == "pause");

  f.select(1);  // the TV, off, with only power
  f.hold();
  REQUIRE(f.sent.size() == 3);
  CHECK(f.sent[2].key == "m2");
  CHECK(f.sent[2].action == "turn_on");

  f.select(2);  // nothing to do
  f.hold();
  f.press();
  CHECK(f.sent.size() == 3);
  CHECK(f.view().screen == RoomView::Screen::Home);  // no controls: nothing to open
}
