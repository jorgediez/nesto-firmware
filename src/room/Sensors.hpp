#pragma once

#include "core/BaseReadings.hpp"
#include "core/Input.hpp"
#include "protocol/Message.hpp"

namespace nesto::room {

/// The base's sensors for Home Assistant (PROTOCOL.md section 17): keeps the
/// two motion sensors on or off, and says when a `sensors` message is due.
///
/// Motion turns on at once and off after a hold time without detection: the
/// proximity sensor reads about once a second, while the far-field sensor
/// sends frames only while someone moves, 10 to 23 a minute
/// (docs/DEVICE.md).
class Sensors {
 public:
  static constexpr Millis kNearHold = 5000;
  static constexpr Millis kFarHold = 30000;
  static constexpr Millis kInterval = 30000;

  /// Someone near (`far` false) or moving in the room (`far` true).
  void presence(bool far, Millis now);

  bool near(Millis now) const;
  bool far(Millis now) const;

  /// A message is due: none sent since start(), the interval has passed, or
  /// a motion sensor changed since the last one.
  bool due(Millis now) const;
  /// The `sensors` message for `base` at `now`, leaving out what it hasn't
  /// read; counts as sent.
  protocol::Json message(const BaseReadings& base, Millis now);
  /// Home Assistant (re)connected: a message is due at once.
  void start() { sent_ = false; }

 private:
  bool near_seen_ = false;
  bool far_seen_ = false;
  Millis near_at_ = 0;  // the last detection
  Millis far_at_ = 0;

  bool sent_ = false;
  Millis sent_at_ = 0;
  bool sent_near_ = false;
  bool sent_far_ = false;
};

}  // namespace nesto::room
