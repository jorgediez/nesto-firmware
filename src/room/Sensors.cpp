#include "room/Sensors.hpp"

#include <cmath>

namespace nesto::room {
namespace {

double rounded(double value, double step) { return std::round(value / step) * step; }

}  // namespace

void Sensors::presence(bool far, Millis now) {
  if (far) {
    far_seen_ = true;
    far_at_ = now;
  } else {
    near_seen_ = true;
    near_at_ = now;
  }
}

bool Sensors::near(Millis now) const { return near_seen_ && now - near_at_ < kNearHold; }

bool Sensors::far(Millis now) const { return far_seen_ && now - far_at_ < kFarHold; }

bool Sensors::due(Millis now) const {
  return !sent_ || now - sent_at_ >= kInterval || near(now) != sent_near_ || far(now) != sent_far_;
}

protocol::Json Sensors::message(const BaseReadings& base, Millis now) {
  protocol::Json out{{"t", "sensors"}};
  if (base.celsius) out["temperature_c"] = rounded(*base.celsius, 0.01);
  if (base.humidity) out["humidity_pct"] = rounded(*base.humidity, 0.1);
  if (base.light_raw) out["illuminance_raw"] = *base.light_raw;
  out["motion_near"] = near(now);
  out["motion_far"] = far(now);
  if (base.supply_v) out["supply_v"] = rounded(*base.supply_v, 0.01);
  if (base.usb_v) out["usb_v"] = rounded(*base.usb_v, 0.01);
  if (base.battery_v) out["battery_v"] = rounded(*base.battery_v, 0.01);

  sent_ = true;
  sent_at_ = now;
  sent_near_ = near(now);
  sent_far_ = far(now);
  return out;
}

}  // namespace nesto::room
