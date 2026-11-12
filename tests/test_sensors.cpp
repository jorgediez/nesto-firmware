#include <doctest/doctest.h>

#include "room/Sensors.hpp"

using nesto::BaseReadings;
using nesto::room::Sensors;

TEST_CASE("motion turns on at once and off after its hold time") {
  Sensors s;
  CHECK_FALSE(s.near(0));
  CHECK_FALSE(s.far(0));

  s.presence(false, 1000);
  CHECK(s.near(1000));
  CHECK(s.near(1000 + Sensors::kNearHold - 1));
  CHECK_FALSE(s.near(1000 + Sensors::kNearHold));
  CHECK_FALSE(s.far(1000));

  // The far-field sensor holds longer, from its last frame.
  s.presence(true, 2000);
  s.presence(true, 12000);
  CHECK(s.far(12000 + Sensors::kFarHold - 1));
  CHECK_FALSE(s.far(12000 + Sensors::kFarHold));
}

TEST_CASE("a message is due at the start, every 30 s and when motion changes") {
  Sensors s;
  CHECK(s.due(0));
  s.message({}, 0);
  CHECK_FALSE(s.due(1000));
  CHECK(s.due(Sensors::kInterval));

  s.message({}, 40000);
  s.presence(true, 41000);
  CHECK(s.due(41000));  // motion: at once
  auto m = s.message({}, 41000);
  CHECK(m["motion_far"] == true);
  CHECK_FALSE(s.due(42000));
  // And again when it ends.
  CHECK(s.due(41000 + Sensors::kFarHold));
  m = s.message({}, 41000 + Sensors::kFarHold);
  CHECK(m["motion_far"] == false);

  // A new connection: at once.
  s.start();
  CHECK(s.due(41000 + Sensors::kFarHold + 1));
}

TEST_CASE("the message rounds the readings and leaves out what's missing") {
  Sensors s;
  BaseReadings base;
  base.celsius = 22.6789;
  base.humidity = 45.66;
  base.light_raw = 1953;
  base.supply_v = 0.0;
  base.usb_v = 4.9412;
  base.battery_v = 3.9061;
  const auto m = s.message(base, 0);
  CHECK(m["t"] == "sensors");
  CHECK(m["temperature_c"].get<double>() == doctest::Approx(22.68));
  CHECK(m["humidity_pct"].get<double>() == doctest::Approx(45.7));
  CHECK(m["illuminance_raw"] == 1953);
  CHECK(m["supply_v"].get<double>() == doctest::Approx(0.0));
  CHECK(m["usb_v"].get<double>() == doctest::Approx(4.94));
  CHECK(m["battery_v"].get<double>() == doctest::Approx(3.91));
  CHECK(m["motion_near"] == false);

  // Before the base has answered: motion only.
  const auto empty = s.message({}, 1000);
  CHECK_FALSE(empty.contains("temperature_c"));
  CHECK_FALSE(empty.contains("usb_v"));
  CHECK(empty.contains("motion_near"));
  CHECK(empty.contains("motion_far"));
}
