#include "room/StandbyClock.hpp"

#include <cstdio>

namespace nesto::room {
namespace {

/// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm), so the
/// local time needs neither the C library's time zone nor its locale.
void civil_from_days(std::int64_t days, int& year, int& month, int& day) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const std::int64_t doe = days - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
  return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
}

const char* const kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

}  // namespace

void WallClock::set_time(std::int64_t utc_ms, Millis now) {
  utc_at_set_ = utc_ms;
  set_at_ = now;
  time_known_ = true;
}

void WallClock::set_offset(int offset_min) {
  offset_min_ = offset_min;
  offset_known_ = true;
}

std::int64_t WallClock::utc_ms(Millis now) const {
  return utc_at_set_ + (static_cast<std::int64_t>(now) - static_cast<std::int64_t>(set_at_));
}

std::optional<LocalTime> WallClock::local(Millis now) const {
  if (!known()) return std::nullopt;
  const std::int64_t local_s =
      floor_div(utc_ms(now), 1000) + static_cast<std::int64_t>(offset_min_) * 60;
  const std::int64_t days = floor_div(local_s, 86400);
  const std::int64_t of_day = local_s - days * 86400;
  LocalTime t;
  civil_from_days(days, t.year, t.month, t.day);
  t.weekday = static_cast<int>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday
  t.hour = static_cast<int>(of_day / 3600);
  t.minute = static_cast<int>(of_day / 60 % 60);
  t.second = static_cast<int>(of_day % 60);
  return t;
}

ClockFace clock_face(const LocalTime& t, bool hour12, bool seconds) {
  ClockFace face;
  char text[16];
  int hour = t.hour;
  if (hour12) {
    face.suffix = t.hour < 12 ? "AM" : "PM";
    hour = t.hour % 12 == 0 ? 12 : t.hour % 12;
  }
  // Two digits for the hour on a 12-hour clock too: it never changes width.
  std::snprintf(text, sizeof text, "%02d:%02d", hour, t.minute);
  face.time = text;
  if (seconds) {
    std::snprintf(text, sizeof text, ":%02d", t.second);
    face.seconds = text;
  }
  std::snprintf(text, sizeof text, "%s %d %s", kWeekdays[t.weekday % 7], t.day,
                kMonths[(t.month + 11) % 12]);
  face.date = text;

  // The hands move on with the smaller ones, as a real clock's do, except the
  // minute hand without seconds, which then stays on the minute.
  face.second_angle = t.second * 60;  // 6 degrees a second
  face.minute_angle = t.minute * 60 + (seconds ? t.second : 0);
  face.hour_angle = (t.hour % 12) * 300 + t.minute * 5;
  return face;
}

}  // namespace nesto::room
