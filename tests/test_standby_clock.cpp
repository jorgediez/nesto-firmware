#include <doctest/doctest.h>

#include "room/StandbyClock.hpp"

using nesto::room::clock_face;
using nesto::room::LocalTime;
using nesto::room::WallClock;

namespace {

// 2026-11-11 14:05:09 UTC, a Wednesday.
constexpr std::int64_t kWednesday = 1794405909000;

LocalTime at(int hour, int minute, int second = 0) {
  LocalTime t;
  t.year = 2026;
  t.month = 11;
  t.day = 11;
  t.weekday = 3;
  t.hour = hour;
  t.minute = minute;
  t.second = second;
  return t;
}

}  // namespace

TEST_CASE("the clock needs both the time and the offset") {
  WallClock clock;
  CHECK_FALSE(clock.known());
  CHECK_FALSE(clock.local(0));
  clock.set_time(kWednesday, 1000);
  CHECK_FALSE(clock.known());
  clock.set_offset(60);
  REQUIRE(clock.known());
  CHECK(clock.offset_min() == 60);
}

TEST_CASE("the local time follows the monotonic clock and the offset") {
  WallClock clock;
  clock.set_time(kWednesday, 5000);
  clock.set_offset(60);
  auto t = clock.local(5000);
  REQUIRE(t);
  CHECK(t->year == 2026);
  CHECK(t->month == 11);
  CHECK(t->day == 11);
  CHECK(t->weekday == 3);
  CHECK(t->hour == 15);
  CHECK(t->minute == 5);
  CHECK(t->second == 9);

  // 51 seconds on: the next minute.
  t = clock.local(5000 + 51000);
  REQUIRE(t);
  CHECK(t->minute == 6);
  CHECK(t->second == 0);
  CHECK(clock.utc_ms(5000 + 51000) == kWednesday + 51000);

  // Across midnight with a negative offset: the day before, a Tuesday.
  clock.set_offset(-15 * 60);
  t = clock.local(5000);
  REQUIRE(t);
  CHECK(t->day == 10);
  CHECK(t->weekday == 2);
  CHECK(t->hour == 23);

  // A leap day.
  clock.set_time(1709208000000, 0);  // 2024-02-29 12:00 UTC
  clock.set_offset(0);
  t = clock.local(0);
  REQUIRE(t);
  CHECK(t->month == 2);
  CHECK(t->day == 29);
  CHECK(t->weekday == 4);
}

TEST_CASE("the face's text: 24 and 12 hours, seconds, the date") {
  auto face = clock_face(at(14, 5, 9), false, false);
  CHECK(face.time == "14:05");
  CHECK(face.suffix.empty());
  CHECK(face.date == "Wed 11 Nov");
  CHECK(face.seconds.empty());
  CHECK(clock_face(at(14, 5, 9), false, true).time == "14:05");
  CHECK(clock_face(at(14, 5, 9), false, true).seconds == ":09");
  CHECK(clock_face(at(7, 0), false, false).time == "07:00");

  face = clock_face(at(14, 5, 9), true, false);
  CHECK(face.time == "2:05");
  CHECK(face.suffix == "PM");
  CHECK(clock_face(at(0, 30), true, false).time == "12:30");
  CHECK(clock_face(at(0, 30), true, false).suffix == "AM");
  CHECK(clock_face(at(12, 0), true, false).suffix == "PM");
}

TEST_CASE("the hands, in tenths of a degree") {
  auto face = clock_face(at(3, 0), false, false);
  CHECK(face.hour_angle == 900);
  CHECK(face.minute_angle == 0);

  face = clock_face(at(15, 30, 30), false, false);
  CHECK(face.hour_angle == 1050);   // half way from 3 to 4
  CHECK(face.minute_angle == 1800);  // on the minute without seconds
  CHECK(face.second_angle == 1800);
  CHECK(clock_face(at(15, 30, 30), false, true).minute_angle == 1830);
  CHECK(clock_face(at(23, 59, 59), false, true).hour_angle == 3595);
}
