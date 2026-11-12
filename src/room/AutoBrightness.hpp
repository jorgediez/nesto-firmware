#pragma once

#include "core/Input.hpp"

namespace nesto::room {

/// The display's brightness from the light sensor (PROTOCOL.md section 12):
/// the configured brightness in a lit room, less in a darker one.
///
/// Measured on a Nest Gen 2 (docs/BACKPLATE.md): about 100 in a dark room,
/// 2000 with the room's lights on, nearly 20000 with a torch at the glass,
/// and the backlight doesn't reach the sensor. The scale is logarithmic, as
/// the eye sees light, from kDarkRaw to kBrightRaw: a little below a lit
/// room, so one reaches the full brightness soon, not only in the limit.
///
/// The readings are smoothed: towards dark slowly, so a hand or a passing
/// shadow doesn't dim the screen; towards light quickly, so switching the
/// lights on brightens it soon.
class AutoBrightness {
 public:
  static constexpr int kDarkRaw = 100;
  static constexpr int kBrightRaw = 1500;
  static constexpr Millis kDarkerTime = 10000;  // time constants of the smoothing
  static constexpr Millis kLighterTime = 2000;
  /// Readings older than this no longer count: no sensor, no scaling.
  static constexpr Millis kStale = 10000;
  /// In the dark: this percentage of the configured brightness, but at least
  /// kMinimum (or the configured brightness, if that's less).
  static constexpr int kDarkPercent = 25;
  static constexpr int kMinimum = 5;

  /// A light sensor reading at `now`.
  void reading(int raw, Millis now);

  /// There is a recent reading.
  bool known(Millis now) const;
  /// The smoothed light, 0 dark to 1 a lit room or brighter; only meaningful
  /// once known().
  double light() const;
  /// The smoothed reading, for logs.
  int raw() const;

  /// `brightness` (0 to 100) for the light now. Unchanged without a recent
  /// reading; 0 stays 0.
  int scale(int brightness, Millis now) const;

 private:
  bool any_ = false;
  double log_raw_ = 0;  // smoothed, natural log
  Millis last_ = 0;
};

}  // namespace nesto::room
