#include <doctest/doctest.h>

#include "room/DisplayPower.hpp"

using nesto::room::DisplayCause;
using nesto::room::DisplayConfig;
using nesto::room::ClockStyle;
using nesto::room::DisplayPower;
using nesto::room::DisplayState;
using nesto::room::StandbyMode;
using Json = nesto::protocol::Json;

namespace {

DisplayConfig config(unsigned timeout_s, StandbyMode mode = StandbyMode::Clock) {
  DisplayConfig c;
  c.timeout_s = timeout_s;
  c.standby_mode = mode;
  return c;
}

/// The change taken, as (state, brightness, cause); fails if there's none.
void expect_change(DisplayPower& d, DisplayState state, int brightness, DisplayCause cause) {
  const auto change = d.take_change();
  REQUIRE(change);
  CHECK(change->state == state);
  CHECK(change->brightness == brightness);
  CHECK(change->cause == cause);
}

}  // namespace

TEST_CASE("the config's defaults are the protocol's example, and missing fields keep them") {
  const DisplayConfig empty = DisplayConfig::from_config(Json{{"t", "config"}});
  CHECK(empty == DisplayConfig{});
  CHECK(empty.brightness == 80);
  CHECK(empty.timeout_s == 30);
  CHECK(empty.standby_mode == StandbyMode::Clock);
  CHECK(empty.standby_brightness == 20);
  CHECK(empty.clock_style == ClockStyle::Analog);
  CHECK_FALSE(empty.clock_seconds);
  CHECK(empty.clock_date);
  CHECK_FALSE(empty.hour12);

  const DisplayConfig c = DisplayConfig::from_config(Json::parse(R"({
    "t": "config",
    "display": {"brightness": 55, "timeout_s": 12, "wake_on_motion": false,
                "wake_on_input_when_off": false, "auto_brightness": false},
    "standby": {"mode": "off", "brightness": 5,
                "clock": {"style": "digital", "seconds": true, "date": false}},
    "locale": {"language": "es", "hour12": true}
  })"));
  CHECK(c.brightness == 55);
  CHECK(c.timeout_s == 12);
  CHECK_FALSE(c.wake_on_motion);
  CHECK_FALSE(c.wake_on_input_when_off);
  CHECK_FALSE(c.auto_brightness);
  CHECK(c.standby_mode == StandbyMode::Off);
  CHECK(c.standby_brightness == 5);
  CHECK(c.clock_style == ClockStyle::Digital);
  CHECK(c.clock_seconds);
  CHECK_FALSE(c.clock_date);
  CHECK(c.hour12);
  CHECK(DisplayConfig::from_config(c.to_config()) == c);  // stored and read back
}

TEST_CASE("out-of-range or mistyped config fields take the default") {
  const DisplayConfig c = DisplayConfig::from_config(Json::parse(R"({
    "display": {"brightness": 140, "timeout_s": -3, "wake_on_motion": "yes"},
    "standby": {"mode": "fireworks", "brightness": "dim"}
  })"));
  CHECK(c == DisplayConfig{});
  CHECK(DisplayConfig::from_config(Json::parse("[1, 2]")) == DisplayConfig{});
}

TEST_CASE("active times out to standby, dark until there's a clock; input wakes it, swallowed") {
  DisplayPower d(0);
  d.configure(config(10), 0);
  CHECK(d.state() == DisplayState::Active);
  CHECK(d.brightness() == 80);
  CHECK_FALSE(d.take_change());

  CHECK(d.input(5000));  // active: meant for the controls, and restarts the timeout
  d.tick(14999);
  CHECK(d.state() == DisplayState::Active);
  d.tick(15000);
  CHECK(d.state() == DisplayState::Standby);
  expect_change(d, DisplayState::Standby, 0, DisplayCause::Timeout);

  CHECK_FALSE(d.input(20000));  // only wakes it
  expect_change(d, DisplayState::Active, 80, DisplayCause::Input);
  CHECK(d.input(20100));
}

TEST_CASE("with a standby screen, standby has its own brightness") {
  DisplayPower d(0, true);
  d.configure(config(10), 0);
  d.tick(10000);
  expect_change(d, DisplayState::Standby, 20, DisplayCause::Timeout);
}

TEST_CASE("the clock becoming known lights standby; losing it darkens it") {
  DisplayPower d(0);
  d.configure(config(10), 0);
  d.set_has_standby_screen(true);
  CHECK_FALSE(d.take_change());  // active: nothing changes
  d.set_has_standby_screen(false);
  d.tick(10000);
  expect_change(d, DisplayState::Standby, 0, DisplayCause::Timeout);
  d.set_has_standby_screen(true);
  expect_change(d, DisplayState::Standby, 20, DisplayCause::Ha);
  d.set_has_standby_screen(true);
  CHECK_FALSE(d.take_change());
  d.set_has_standby_screen(false);
  expect_change(d, DisplayState::Standby, 0, DisplayCause::Ha);
}

TEST_CASE("motion wakes standby and keeps active awake, unless turned off") {
  DisplayPower d(0);
  d.configure(config(10), 0);
  d.motion(8000);
  d.tick(17999);
  CHECK(d.state() == DisplayState::Active);
  d.tick(18000);
  CHECK(d.state() == DisplayState::Standby);
  d.take_change();
  d.motion(19000);
  expect_change(d, DisplayState::Active, 80, DisplayCause::Motion);

  DisplayConfig no_motion = config(10);
  no_motion.wake_on_motion = false;
  d.configure(no_motion, 20000);
  d.tick(30000);
  d.take_change();
  d.motion(31000);
  CHECK(d.state() == DisplayState::Standby);
}

TEST_CASE("standby mode off: the timeout turns it off; motion doesn't wake it, input may") {
  DisplayPower d(0);
  d.configure(config(10, StandbyMode::Off), 0);
  d.tick(10000);
  expect_change(d, DisplayState::Off, 0, DisplayCause::Timeout);
  d.motion(11000);
  CHECK(d.state() == DisplayState::Off);

  CHECK_FALSE(d.input(12000));
  expect_change(d, DisplayState::Active, 80, DisplayCause::Input);

  DisplayConfig stay_off = config(10, StandbyMode::Off);
  stay_off.wake_on_input_when_off = false;
  d.configure(stay_off, 13000);
  d.tick(23000);
  d.take_change();
  CHECK_FALSE(d.input(24000));
  CHECK(d.state() == DisplayState::Off);
  CHECK_FALSE(d.take_change());
}

TEST_CASE("Home Assistant sets any state; active restarts the timeout") {
  DisplayPower d(0);
  d.configure(config(10), 0);
  d.set(DisplayState::Off, 1000);
  expect_change(d, DisplayState::Off, 0, DisplayCause::Ha);
  d.set(DisplayState::Off, 1500);
  CHECK_FALSE(d.take_change());  // no change, nothing to report
  d.set(DisplayState::Active, 9000);
  expect_change(d, DisplayState::Active, 80, DisplayCause::Ha);
  d.tick(18999);
  CHECK(d.state() == DisplayState::Active);
  d.set(DisplayState::Standby, 19000);
  expect_change(d, DisplayState::Standby, 0, DisplayCause::Ha);
}

TEST_CASE("timeout 0 never leaves active; a new brightness is reported at once") {
  DisplayPower d(0);
  d.configure(config(0), 0);
  d.tick(10'000'000);
  CHECK(d.state() == DisplayState::Active);

  DisplayConfig brighter = config(0);
  brighter.brightness = 100;
  d.configure(brighter, 10'000'001);
  expect_change(d, DisplayState::Active, 100, DisplayCause::Ha);
  d.configure(brighter, 10'000'002);
  CHECK_FALSE(d.take_change());
}

TEST_CASE("state names, both ways") {
  CHECK(std::string(nesto::room::name(DisplayState::Standby)) == "standby");
  CHECK(std::string(nesto::room::name(DisplayCause::Timeout)) == "timeout");
  CHECK(nesto::room::parse_display_state("off") == DisplayState::Off);
  CHECK_FALSE(nesto::room::parse_display_state("asleep"));
}

TEST_CASE("the backlight fades down on a curve and comes up at once") {
  using nesto::room::BacklightFade;
  BacklightFade fade(80);
  CHECK(fade.level(0) == 80);
  CHECK_FALSE(fade.fading(0));

  fade.go(0, 1000);
  CHECK(fade.level(1000) == 80);
  CHECK(fade.fading(1000));
  CHECK(fade.level(1400) == 20);  // halfway, a quarter: the square of the time left
  CHECK(fade.level(1799) == 1);   // still lit until the very end
  CHECK(fade.level(1800) == 0);
  CHECK_FALSE(fade.fading(1800));

  // Woken halfway down: back up at once.
  fade = BacklightFade(80);
  fade.go(0, 0);
  fade.go(80, 250);
  CHECK(fade.level(250) == 80);
  CHECK_FALSE(fade.fading(250));

  // A lower brightness while lit fades too, from wherever it is.
  fade.go(20, 1000);
  CHECK(fade.level(1400) == 35);
  CHECK(fade.level(2000) == 20);
  CHECK(fade.target() == 20);
}

TEST_CASE("a glide goes up or down in a straight line") {
  using nesto::room::BacklightFade;
  BacklightFade fade(80);
  fade.glide(20, 0);
  CHECK(fade.level(0) == 80);
  CHECK(fade.moving(0));
  CHECK_FALSE(fade.fading(0));  // not a change of state
  CHECK(fade.level(1000) == 50);
  CHECK(fade.level(1999) == 21);
  CHECK(fade.level(2000) == 20);
  CHECK_FALSE(fade.moving(2000));

  fade.glide(60, 3000);
  CHECK(fade.level(3000) == 20);
  CHECK(fade.level(4000) == 40);
  CHECK(fade.level(4999) == 59);
  CHECK(fade.level(5000) == 60);

  // A change of state after a glide fades as before.
  fade.go(0, 6000);
  CHECK(fade.fading(6000));
  CHECK(fade.level(6800) == 0);
}
