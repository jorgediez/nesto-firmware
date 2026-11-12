#include "room/AutoBrightness.hpp"

#include <algorithm>
#include <cmath>

namespace nesto::room {

void AutoBrightness::reading(int raw, Millis now) {
  const double value = std::log(static_cast<double>(std::max(raw, 1)));
  if (!any_ || !known(now)) {
    log_raw_ = value;
  } else {
    const Millis tau = value < log_raw_ ? kDarkerTime : kLighterTime;
    const double dt = static_cast<double>(now > last_ ? now - last_ : 0);
    log_raw_ += (value - log_raw_) * (1.0 - std::exp(-dt / static_cast<double>(tau)));
  }
  any_ = true;
  last_ = now;
}

bool AutoBrightness::known(Millis now) const { return any_ && now - last_ <= kStale; }

double AutoBrightness::light() const {
  const double dark = std::log(static_cast<double>(kDarkRaw));
  const double bright = std::log(static_cast<double>(kBrightRaw));
  return std::clamp((log_raw_ - dark) / (bright - dark), 0.0, 1.0);
}

int AutoBrightness::raw() const { return static_cast<int>(std::lround(std::exp(log_raw_))); }

int AutoBrightness::scale(int brightness, Millis now) const {
  if (brightness <= 0 || !known(now)) return brightness;
  const int floor = std::min(brightness, std::max(kMinimum, (brightness * kDarkPercent + 50) / 100));
  return floor + static_cast<int>(std::lround((brightness - floor) * light()));
}

}  // namespace nesto::room
