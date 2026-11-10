#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/RingSteps.hpp"
#include "hal/Hal.hpp"

namespace nesto {

struct NestOptions {
  RingSteps::Config ring;
};

/// The thermostat (docs/DEVICE.md): the screen is /dev/fb0, the ring and the
/// press are input devices found by name, and the backlight is under
/// /sys/class/backlight. They're only free once the stock software has been
/// stopped (scripts/nest-screen.sh).
///
/// SIGINT, SIGTERM and SIGHUP make poll() report a QuitRequest, so the
/// program stops cleanly. On the way out the backlight goes back to where it
/// was.
class NestHal : public Hal {
 public:
  explicit NestHal(NestOptions options = {});
  ~NestHal() override;

  NestHal(const NestHal&) = delete;
  NestHal& operator=(const NestHal&) = delete;

  bool init() override;
  Millis now() const override;
  void poll(std::vector<TimedInput>& out, Millis timeout) override;
  void present(const std::uint32_t* pixels) override;
  void set_backlight(int percent) override;

  /// Prints the ring's and the press's raw events, and the steps the ring's
  /// make, until interrupted: for measuring the ring. Leaves the screen and
  /// the backlight alone. Returns the exit code.
  int input_test();

 private:
  bool open_inputs();
  bool open_framebuffer();
  void open_backlight();
  /// Reads whatever the input devices have, without waiting. With `verbose`,
  /// prints each raw event too.
  void read_inputs(std::vector<TimedInput>& out, bool verbose);

  NestOptions options_;
  RingSteps ring_steps_;
  long ring_total_ = 0;  // raw counts since the start, for input_test()

  int fb_fd_ = -1;
  std::uint8_t* fb_ = nullptr;
  std::size_t fb_size_ = 0;
  std::size_t line_length_ = 0;  // bytes per framebuffer row
  int x_offset_ = 0;             // where the 320x320 image starts
  int y_offset_ = 0;

  int ring_fd_ = -1;
  int press_fd_ = -1;

  std::string backlight_;  // the brightness file; empty if there is none
  int backlight_max_ = 0;
  int backlight_before_ = -1;
};

}  // namespace nesto
