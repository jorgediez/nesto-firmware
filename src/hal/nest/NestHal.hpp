#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/Backplate.hpp"
#include "core/RingTicks.hpp"
#include "hal/Hal.hpp"

namespace nesto {

struct NestOptions {
  // Measured on a real thermostat: about 7300 counts a turn, clockwise
  // negative. 49 counts make about 150 ticks a turn: 30 clicks, so 30 items
  // or choices a turn, and brightness from 0 to 100% in two thirds of one.
  RingTicks::Config ring{49, true};
  // Talk to the backplate, which otherwise resets the thermostat after about
  // 30 minutes (core/Backplate.hpp).
  bool backplate = true;
  backplate::Session::Config session;
};

/// The thermostat (docs/DEVICE.md): the screen is /dev/fb0, the ring and the
/// press are input devices found by name, the backlight is under
/// /sys/class/backlight, and the backplate is on /dev/ttyO2
/// (docs/BACKPLATE.md). They're only free once the stock software has been
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

  /// Prints the ring's and the press's raw events, and the ticks the ring's
  /// make, until interrupted: for measuring the ring. Leaves the screen and
  /// the backlight alone. Returns the exit code.
  int input_test();

  /// Talks to the backplate alone, printing every frame and what it means,
  /// until interrupted. Leaves the screen, ring and press alone. Returns the
  /// exit code.
  int backplate_test();

 private:
  bool open_inputs();
  bool open_framebuffer();
  void open_backlight();
  bool open_backplate();
  /// Reads what the backplate sent, moves the session on, and sends what it
  /// has to say. With `verbose`, prints each frame.
  void service_backplate(bool verbose);
  void report_backplate(bool verbose);
  /// Leaves the backplate as the stock software expects to find it.
  void hand_back_backplate();
  /// Reads whatever the input devices have, without waiting. With `verbose`,
  /// prints each raw event too.
  void read_inputs(std::vector<TimedInput>& out, bool verbose);

  NestOptions options_;
  RingTicks ring_ticks_;
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

  int backplate_fd_ = -1;
  backplate::FrameReader backplate_reader_;
  backplate::Session backplate_;
  std::vector<std::uint8_t> backplate_out_;  // written as the line takes it
  bool backplate_verbose_ = false;           // the backplate test prints every frame
  // What was last reported, to log only changes, and the readings now and then.
  int reported_handshakes_ = 0;
  int reported_restarts_ = 0;
  Millis reported_readings_ = 0;
};

}  // namespace nesto
