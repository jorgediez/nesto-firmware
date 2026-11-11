#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include "core/Input.hpp"

namespace nesto {

/// The Nest's screen: 320x320 pixels, of which only the inscribed circle shows.
constexpr int kScreenSize = 320;

/// Everything Nesto needs from the hardware. The simulator and the thermostat
/// each implement it; the rest of the program doesn't know which one it runs on.
class Hal {
 public:
  virtual ~Hal() = default;

  /// Opens the screen and inputs. Logs the reason and returns false on failure.
  virtual bool init() = 0;

  /// Milliseconds on a monotonic clock.
  virtual Millis now() const = 0;

  /// Waits up to `timeout` ms for input and appends what arrived to `out`.
  /// Returns earlier as soon as there is something.
  virtual void poll(std::vector<TimedInput>& out, Millis timeout) = 0;

  /// Shows a full frame: kScreenSize x kScreenSize pixels in XRGB8888.
  virtual void present(const std::uint32_t* pixels) = 0;

  /// Sets the backlight, 0 (off) to 100.
  virtual void set_backlight(int percent) = 0;

  /// Turns the screen itself dark or back on, beyond the backlight, where the
  /// hardware needs it: the thermostat shows black while dark, as a dark LCD
  /// still shows its picture in room light, and puts the last frame back.
  virtual void set_screen_on(bool /*on*/) {}

  /// UTC in milliseconds from the system's wall clock, which may never have
  /// been set: see WallClock::kEarliestUtcMs.
  virtual std::int64_t wall_clock_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  /// Sets the system's wall clock where Nesto owns it: the thermostat's, not
  /// the simulator's host. Returns false if it couldn't.
  virtual bool set_wall_clock(std::int64_t /*utc_ms*/) { return true; }
};

}  // namespace nesto
