#include <doctest/doctest.h>

#include "room/DisplayPower.hpp"

using nesto::room::DisplayCause;
using nesto::room::DisplayConfig;
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

  const DisplayConfig c = DisplayConfig::from_config(Json::parse(R"({
    "t": "config",
    "display": {"brightness": 55, "timeout_s": 12, "wake_on_motion": false,
                "wake_on_input_when_off": false, "auto_brightness": false},
    "standby": {"mode": "off", "brightness": 5}
  })"));
  CHECK(c.brightness == 55);
  CHECK(c.timeout_s == 12);
  CHECK_FALSE(c.wake_on_motion);
  CHECK_FALSE(c.wake_on_input_when_off);
  CHECK_FALSE(c.auto_brightness);
  CHECK(c.standby_mode == StandbyMode::Off);
  CHECK(c.standby_brightness == 5);
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
