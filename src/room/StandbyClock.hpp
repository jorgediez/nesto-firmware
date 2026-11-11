#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/Input.hpp"

namespace nesto::room {

/// A local time, broken down.
struct LocalTime {
  int year = 1970;
  int month = 1;    // 1 to 12
  int day = 1;      // 1 to 31
  int weekday = 4;  // 0 Sunday to 6 Saturday
  int hour = 0;     // 0 to 23
  int minute = 0;
  int second = 0;
};

/// The time of day (PROTOCOL.md section 16): a UTC time and the UTC offset,
/// kept from the monotonic clock between settings, so a wall clock changed
/// under it doesn't matter. No time zone database: Home Assistant sends the
/// offset again whenever it changes.
class WallClock {
 public:
  /// Before this, a system clock was never set: 2025-01-01 UTC.
  static constexpr std::int64_t kEarliestUtcMs = 1735689600000;

  /// The UTC time is `utc_ms` at `now`.
  void set_time(std::int64_t utc_ms, Millis now);
  /// The UTC offset, in minutes east of Greenwich.
  void set_offset(int offset_min);

  /// Both the time and the offset are known.
  bool known() const { return time_known_ && offset_known_; }
  bool has_offset() const { return offset_known_; }
  int offset_min() const { return offset_min_; }

  /// UTC at `now`; only meaningful once the time is set.
  std::int64_t utc_ms(Millis now) const;
  /// The local time at `now`, once known().
  std::optional<LocalTime> local(Millis now) const;

 private:
  bool time_known_ = false;
  bool offset_known_ = false;
  std::int64_t utc_at_set_ = 0;
  Millis set_at_ = 0;
  int offset_min_ = 0;
};

/// How the standby clock looks (PROTOCOL.md section 12).
enum class ClockStyle { Analog, Digital };

/// The clock face's text and hands for one local time.
struct ClockFace {
  std::string time;     // "14:05", or "2:05" on a 12-hour clock
  std::string seconds;  // ":09" with seconds, else empty
  std::string suffix;   // "AM" or "PM" on a 12-hour clock, else empty
  std::string date;     // "Wed 11 Nov"
  // Hands, in tenths of a degree clockwise from 12 o'clock.
  int hour_angle = 0;
  int minute_angle = 0;
  int second_angle = 0;
};

ClockFace clock_face(const LocalTime& t, bool hour12, bool seconds);

}  // namespace nesto::room
