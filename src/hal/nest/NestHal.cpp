#include "hal/nest/NestHal.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

#include "core/Log.hpp"

namespace nesto {
namespace {

constexpr const char* kFramebuffer = "/dev/fb0";
constexpr const char* kBacklights = "/sys/class/backlight";
// The input devices' names, as the kernel reports them (docs/DEVICE.md).
constexpr const char* kRingName = "avago-adbs-a330";
constexpr const char* kPressName = "twl4030_pwrbutton";
constexpr const char* kBackplate = "/dev/ttyO2";
// Blanking the framebuffer turns the display controller off too, as
// nest-gallery does while its display sleeps.
constexpr const char* kBlank = "/sys/class/graphics/fb0/blank";
// How often a run logs the backplate's readings.
constexpr Millis kBackplateReport = 60000;

/// A backplate message type's name, for the backplate test.
const char* type_name(std::uint16_t type) {
  switch (type) {
    case backplate::kAscii: return "text";
    case backplate::kClimate: return "temperature and humidity";
    case backplate::kFetPresence: return "wire presence";
    case backplate::kPirRaw: return "PIR, raw";
    case 0x0007: return "proximity";
    case 0x0009: return "wire presence (more)";
    case 0x000a: return "ambient light";
    case backplate::kState: return "state";
    case 0x000c: return "sensor ADC";
    case 0x0010: return "TFE id";
    case backplate::kTfeVersion: return "TFE version";
    case backplate::kTfeBuildInfo: return "TFE build";
    case backplate::kBslId: return "BSL id";
    case backplate::kHardware: return "name";
    case backplate::kSerial: return "serial";
    case 0x0022: return "buffered temperature";
    case 0x0023: return "PIR motion";
    case 0x0025: return "proximity event";
    case 0x0027: return "proximity, detailed";
    case backplate::kEndOfBuffers: return "end of buffers";
    default: return "?";
  }
}

volatile std::sig_atomic_t g_quit_signal = 0;

void on_quit_signal(int signal) { g_quit_signal = signal; }

void catch_quit_signals() {
  struct sigaction action {};
  action.sa_handler = &on_quit_signal;
  sigemptyset(&action.sa_mask);
  // No SA_RESTART: a signal ends a wait at once, and the loop sees it.
  for (int signal : {SIGINT, SIGTERM, SIGHUP}) sigaction(signal, &action, nullptr);
}

/// Opens the input device called `name`, or returns -1.
int open_input(const char* name) {
  for (int i = 0; i < 16; ++i) {
    char path[32];
    std::snprintf(path, sizeof path, "/dev/input/event%d", i);
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    char found[64] = "";
    ioctl(fd, EVIOCGNAME(sizeof found - 1), found);
    if (std::strcmp(found, name) == 0) {
      // Only Nesto gets its events from now on.
      if (ioctl(fd, EVIOCGRAB, 1) != 0) {
        log::warn("%s (%s): can't grab it: %s", path, name, std::strerror(errno));
      }
      log::info("%s is %s", path, name);
      return fd;
    }
    close(fd);
  }
  log::error("no input device called %s", name);
  return -1;
}

int read_int(const std::string& path) {
  std::ifstream file(path);
  int value = -1;
  file >> value;
  return file ? value : -1;
}

bool write_int(const std::string& path, int value) {
  std::ofstream file(path);
  file << value << '\n';
  file.flush();
  return static_cast<bool>(file);
}

}  // namespace

NestHal::NestHal(NestOptions options)
    : options_(options), ring_ticks_(options.ring), backplate_(options.session) {}

NestHal::~NestHal() {
  hand_back_backplate();
  // The stock software expects the screen as it found it: lit, not blanked.
  if (blanked_) write_int(kBlank, 0);
  if (!backlight_.empty() && backlight_before_ >= 0) write_int(backlight_, backlight_before_);
  if (fb_) munmap(fb_, fb_size_);
  for (int fd : {fb_fd_, ring_fd_, press_fd_, backplate_fd_}) {
    if (fd >= 0) close(fd);
  }
}

bool NestHal::init() {
  catch_quit_signals();
  if (!open_framebuffer() || !open_inputs()) return false;
  open_backlight();
  const RingTicks::Config& ring = ring_ticks_.config();
  log::info("the ring: %d counts per tick, clockwise %s", ring.counts_per_tick,
            ring.reverse ? "negative" : "positive");
  // Without the backplate Nesto still runs; the thermostat resets after
  // about 30 minutes.
  if (options_.backplate) {
    open_backplate();
  } else {
    log::warn("not talking to the backplate: it resets the thermostat after about 30 minutes");
  }
  return true;
}

bool NestHal::open_backplate() {
  backplate_fd_ = open(kBackplate, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (backplate_fd_ < 0) {
    log::error("%s: %s; the thermostat resets after about 30 minutes", kBackplate,
               std::strerror(errno));
    return false;
  }
  // 115200 baud, 8N1, raw: in particular no CR to NL, which would break
  // frames holding 0x0d.
  termios tty{};
  if (tcgetattr(backplate_fd_, &tty) != 0) {
    log::error("%s: %s", kBackplate, std::strerror(errno));
    close(backplate_fd_);
    backplate_fd_ = -1;
    return false;
  }
  cfmakeraw(&tty);
  cfsetispeed(&tty, B115200);
  cfsetospeed(&tty, B115200);
  tty.c_cflag |= CLOCAL | CREAD;
  tty.c_cflag &= ~static_cast<tcflag_t>(CSTOPB | CRTSCTS | PARENB);
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;
  if (tcsetattr(backplate_fd_, TCSANOW, &tty) != 0) {
    log::error("%s: can't set it up: %s", kBackplate, std::strerror(errno));
    close(backplate_fd_);
    backplate_fd_ = -1;
    return false;
  }
  log::info("%s is the backplate", kBackplate);
  backplate_.start(now());
  return true;
}

void NestHal::hand_back_backplate() {
  if (backplate_fd_ < 0 || !options_.backplate) return;
  // A few seconds at most, as the stock software does when it stops.
  backplate_.hand_back(now());
  while (backplate_.handing_back()) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(backplate_fd_, &readable);
    timeval tv{0, 50000};
    select(backplate_fd_ + 1, &readable, nullptr, nullptr, &tv);
    service_backplate(backplate_verbose_);
  }
  // Let the last acknowledgement leave before the port closes.
  tcdrain(backplate_fd_);
  if (backplate_.handed_back()) {
    log::info("backplate: handed back (reset, start-up burst, wires acknowledged)");
  } else {
    log::warn("backplate: %s", backplate_.problem().c_str());
  }
}

void NestHal::service_backplate(bool verbose, std::vector<TimedInput>* inputs) {
  if (backplate_fd_ < 0) return;
  const Millis t = now();

  std::uint8_t buffer[256];
  std::vector<backplate::Frame> frames;
  for (;;) {
    const ssize_t n = read(backplate_fd_, buffer, sizeof buffer);
    if (n <= 0) break;  // EAGAIN: nothing more for now
    backplate_reader_.feed(buffer, static_cast<std::size_t>(n), frames);
  }
  for (const backplate::Frame& frame : frames) {
    if (verbose) {
      std::printf("bp <- %04x %-26s %3zu bytes ", frame.type, type_name(frame.type),
                  frame.payload.size());
      const bool printable = frame.type == backplate::kAscii || frame.type == backplate::kTfeVersion ||
                             frame.type == backplate::kTfeBuildInfo || frame.type == backplate::kHardware ||
                             frame.type == backplate::kSerial;
      if (printable) {
        std::printf(" \"%s\"", backplate::text(frame).c_str());
      } else {
        for (std::size_t i = 0; i < frame.payload.size() && i < 24; ++i) std::printf(" %02x", frame.payload[i]);
        if (frame.payload.size() > 24) std::printf(" ...");
      }
      if (auto power = frame.type == backplate::kState ? backplate::decode_power(frame.payload) : std::nullopt) {
        std::printf("  [flags %02x, USB %.2f V, input %.2f V, operating %.3f V, battery %.3f V]",
                    power->flags, power->usb, power->input,
                    power->operating, power->battery);
      }
      if (auto climate =
              frame.type == backplate::kClimate ? backplate::decode_climate(frame.payload) : std::nullopt) {
        std::printf("  [%.2f C, %.1f%%]", climate->celsius, climate->humidity);
      }
      std::printf("\n");
    }
    backplate_.receive(t, frame);
    if (frame.type == backplate::kProximity) {
      const auto near = backplate::decode_proximity(frame.payload);
      if (near) {
        ++near_readings_;
        near_highest_ = std::max(near_highest_, *near);
        if (*near >= backplate::kNearThreshold) {
          ++near_count_;
          if (inputs) inputs->push_back({t, Presence{}});
        }
      }
    } else if (frame.type == backplate::kPirRaw) {
      ++far_frames_;
      const bool moving = std::any_of(frame.payload.begin(), frame.payload.end(),
                                      [](std::uint8_t b) { return b != 0; });
      if (moving) ++far_moving_;
    }
  }

  backplate_.tick(t);
  if (backplate_.take_break()) {
    // Start afresh: drop whatever is on the line, then the break.
    tcflush(backplate_fd_, TCIOFLUSH);
    backplate_reader_.clear();
    backplate_out_.clear();
    if (tcsendbreak(backplate_fd_, 0) != 0) log::warn("%s: break: %s", kBackplate, std::strerror(errno));
    if (verbose) std::printf("bp -> break\n");
  }
  const std::vector<std::uint8_t> out = backplate_.take_output();
  if (verbose && !out.empty()) {
    // The commands, by type.
    for (std::size_t i = 0; i + 7 <= out.size();) {
      const unsigned type = out[i + 3] | (out[i + 4] << 8);
      const std::size_t length = out[i + 5] | (out[i + 6] << 8);
      std::printf("bp -> %04x (%zu bytes)\n", type, length);
      i += 7 + length + 2;
    }
  }
  backplate_out_.insert(backplate_out_.end(), out.begin(), out.end());
  if (!backplate_out_.empty()) {
    const ssize_t n = write(backplate_fd_, backplate_out_.data(), backplate_out_.size());
    if (n > 0) backplate_out_.erase(backplate_out_.begin(), backplate_out_.begin() + n);
  }
  report_backplate(verbose);
  if (verbose) std::fflush(stdout);
}

void NestHal::report_backplate(bool verbose) {
  if (backplate_.handshakes() != reported_handshakes_) {
    reported_handshakes_ = backplate_.handshakes();
    log::info("backplate: talking to it (%s, serial %s, BSL id %s, firmware %s)",
              backplate_.model().empty() ? "no name" : backplate_.model().c_str(),
              backplate_.serial().empty() ? "?" : backplate_.serial().c_str(), backplate_.bsl_id().c_str(),
              backplate_.version().c_str());
  }
  if (backplate_.restarts() != reported_restarts_) {
    reported_restarts_ = backplate_.restarts();
    log::warn("backplate: %s; starting again", backplate_.problem().c_str());
  }
  const Millis t = now();
  if (!verbose && backplate_.running() && t - reported_readings_ >= kBackplateReport) {
    reported_readings_ = t;
    char power[96] = "no state yet";
    if (const auto& p = backplate_.power()) {
      std::snprintf(power, sizeof power, "battery %.3f V (flags %02x), USB %.2f V, input %.2f V",
                    p->battery, p->flags, p->usb, p->input);
    }
    char climate[64] = "";
    if (const auto& c = backplate_.climate()) {
      std::snprintf(climate, sizeof climate, ", %.1f C, %.0f%% humidity", c->celsius, c->humidity);
    }
    log::info("backplate: %s%s", power, climate);
    // What the motion sensors did over the minute, until we know enough to
    // wake on the far-field one too.
    log::info("backplate: near-field %d readings, highest %d, %d at %d or more; "
              "far-field %d frames, %d not zero",
              near_readings_, near_highest_, near_count_, backplate::kNearThreshold, far_frames_, far_moving_);
    near_readings_ = near_highest_ = near_count_ = far_frames_ = far_moving_ = 0;
  }
}

bool NestHal::open_inputs() {
  ring_fd_ = open_input(kRingName);
  press_fd_ = open_input(kPressName);
  return ring_fd_ >= 0 && press_fd_ >= 0;
}

bool NestHal::open_framebuffer() {
  fb_fd_ = open(kFramebuffer, O_RDWR | O_CLOEXEC);
  if (fb_fd_ < 0) {
    log::error("%s: %s", kFramebuffer, std::strerror(errno));
    return false;
  }

  fb_var_screeninfo var{};
  fb_fix_screeninfo fix{};
  if (ioctl(fb_fd_, FBIOGET_VSCREENINFO, &var) != 0 ||
      ioctl(fb_fd_, FBIOGET_FSCREENINFO, &fix) != 0) {
    log::error("%s: can't read the screen format: %s", kFramebuffer, std::strerror(errno));
    return false;
  }
  log::info("%s: %ux%u (%ux%u virtual, showing from %u,%u), %u bits per pixel, %u bytes per line",
            kFramebuffer, var.xres, var.yres, var.xres_virtual, var.yres_virtual, var.xoffset,
            var.yoffset, var.bits_per_pixel, fix.line_length);

  // A real thermostat has 320x320 in XRGB8888, which is what LVGL draws.
  if (var.bits_per_pixel != 32 || var.xres < static_cast<unsigned>(kScreenSize) ||
      var.yres < static_cast<unsigned>(kScreenSize) || var.red.offset != 16 ||
      var.green.offset != 8 || var.blue.offset != 0) {
    log::error("%s: expected at least %dx%d in XRGB8888", kFramebuffer, kScreenSize, kScreenSize);
    return false;
  }

  line_length_ = fix.line_length;
  fb_size_ = fix.smem_len;
  // Draw into the page on show; the stock software leaves the second one up.
  x_offset_ = static_cast<int>(var.xoffset);
  y_offset_ = static_cast<int>(var.yoffset);
  void* mapped = mmap(nullptr, fb_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd_, 0);
  if (mapped == MAP_FAILED) {
    log::error("%s: mmap: %s", kFramebuffer, std::strerror(errno));
    return false;
  }
  fb_ = static_cast<std::uint8_t*>(mapped);
  return true;
}

void NestHal::open_backlight() {
  // There is one, 3-0036; take whichever there is.
  DIR* dir = opendir(kBacklights);
  if (!dir) {
    log::warn("%s: %s; the backlight stays as it is", kBacklights, std::strerror(errno));
    return;
  }
  while (const dirent* entry = readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    const std::string base = std::string(kBacklights) + "/" + entry->d_name;
    const int max = read_int(base + "/max_brightness");
    if (max <= 0) continue;
    backlight_ = base + "/brightness";
    backlight_max_ = max;
    backlight_before_ = read_int(backlight_);
    log::info("backlight %s: %d of %d", entry->d_name, backlight_before_, max);
    break;
  }
  closedir(dir);
  if (backlight_.empty()) log::warn("no backlight in %s; it stays as it is", kBacklights);
}

Millis NestHal::now() const {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<Millis>(ts.tv_sec) * 1000 + static_cast<Millis>(ts.tv_nsec / 1000000);
}

void NestHal::read_inputs(std::vector<TimedInput>& out, bool verbose) {
  input_event events[32];
  for (int fd : {ring_fd_, press_fd_}) {
    if (fd < 0) continue;
    for (;;) {
      const ssize_t n = read(fd, events, sizeof events);
      if (n <= 0) break;  // EAGAIN: nothing more for now
      const Millis t = now();
      for (std::size_t i = 0; i < static_cast<std::size_t>(n) / sizeof events[0]; ++i) {
        const input_event& e = events[i];
        if (fd == ring_fd_ && e.type == EV_REL && e.code == REL_X) {
          const int ticks = ring_ticks_.add(t, e.value);
          ring_total_ += e.value;
          if (verbose) {
            std::printf("ring  %+5d counts   total %+7ld   ticks %+d\n", e.value, ring_total_,
                        ticks);
          }
          if (ticks != 0) out.push_back({t, RingTurn{ticks}});
        } else if (fd == press_fd_ && e.type == EV_KEY && e.code == KEY_POWER) {
          // 1 down, 0 up, 2 a key repeat, which a press doesn't need.
          if (verbose) std::printf("press %s\n", e.value == 1 ? "down" : e.value == 0 ? "up" : "repeat");
          if (e.value == 0 || e.value == 1) out.push_back({t, ButtonEdge{e.value == 1}});
        }
      }
    }
  }
  if (verbose) std::fflush(stdout);
}

void NestHal::poll(std::vector<TimedInput>& out, Millis timeout) {
  if (g_quit_signal == 0) {
    fd_set readable;
    FD_ZERO(&readable);
    int max_fd = -1;
    for (int fd : {ring_fd_, press_fd_, backplate_fd_}) {
      if (fd < 0) continue;
      FD_SET(fd, &readable);
      max_fd = std::max(max_fd, fd);
    }
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout % 1000) * 1000);
    select(max_fd + 1, &readable, nullptr, nullptr, &tv);  // EINTR: a signal, seen below
    read_inputs(out, false);
    service_backplate(false, &out);
  }
  if (g_quit_signal != 0) {
    log::info("signal %d: stopping", static_cast<int>(g_quit_signal));
    g_quit_signal = 0;
    out.push_back({now(), QuitRequest{0}});
  }
}

void NestHal::present(const std::uint32_t* pixels) {
  const std::size_t row_bytes = static_cast<std::size_t>(kScreenSize) * 4;
  for (int y = 0; y < kScreenSize; ++y) {
    const std::size_t at = static_cast<std::size_t>(y + y_offset_) * line_length_ +
                           static_cast<std::size_t>(x_offset_) * 4;
    if (at + row_bytes > fb_size_) return;
    std::memcpy(fb_ + at, pixels + static_cast<std::size_t>(y) * kScreenSize, row_bytes);
  }
}

void NestHal::set_backlight(int percent) {
  if (backlight_.empty()) return;
  percent = std::clamp(percent, 0, 100);
  // Round up, so any percentage above 0 still lights the screen.
  const int level = (percent * backlight_max_ + 99) / 100;
  if (!write_int(backlight_, level)) log::warn("%s: can't set it to %d", backlight_.c_str(), level);
}

void NestHal::set_screen_on(bool on) {
  if (on == !blanked_) return;
  if (!write_int(kBlank, on ? 0 : 1)) {
    log::warn("%s: can't %s the screen", kBlank, on ? "unblank" : "blank");
    return;
  }
  blanked_ = !on;
}

int NestHal::input_test() {
  catch_quit_signals();
  if (!open_inputs()) return 1;
  const RingTicks::Config& ring = ring_ticks_.config();
  std::printf(
      "Turn the ring and press the display; Ctrl-C stops.\n"
      "To measure the ring, turn it once round, clockwise, and read the total.\n"
      "Ticks at %d counts per tick, clockwise %s.\n",
      ring.counts_per_tick, ring.reverse ? "negative" : "positive");
  std::fflush(stdout);
  std::vector<TimedInput> ignored;
  while (g_quit_signal == 0) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(ring_fd_, &readable);
    FD_SET(press_fd_, &readable);
    select(std::max(ring_fd_, press_fd_) + 1, &readable, nullptr, nullptr, nullptr);
    ignored.clear();
    read_inputs(ignored, true);
  }
  std::printf("ring total: %+ld counts\n", ring_total_);
  return 0;
}

int NestHal::backplate_test() {
  catch_quit_signals();
  backplate_verbose_ = true;
  if (!open_backplate()) return 1;
  // Dark, so the battery's reading isn't spent on a lit screen; put back on
  // the way out.
  open_backlight();
  set_backlight(0);
  std::printf(
      "Talking to the backplate, with the backlight off; every frame is printed. Ctrl-C stops.\n"
      "The handshake should end with a model and version, then a keep-alive every 15 s\n"
      "and buffered readings every 30 s.\n");
  std::fflush(stdout);
  Millis printed = now();
  while (g_quit_signal == 0) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(backplate_fd_, &readable);
    timeval tv{0, 50000};
    select(backplate_fd_ + 1, &readable, nullptr, nullptr, &tv);
    service_backplate(true);
    // Now and then, what it adds up to.
    if (now() - printed >= 30000) {
      printed = now();
      char battery[32] = "?";
      if (const auto& p = backplate_.power()) std::snprintf(battery, sizeof battery, "%.3f V", p->battery);
      char climate[48] = "?";
      if (const auto& c = backplate_.climate()) {
        std::snprintf(climate, sizeof climate, "%.2f C, %.1f%%", c->celsius, c->humidity);
      }
      std::printf("== %s; handshakes %d, restarts %d, bad frames %zu; battery %s; climate %s\n",
                  backplate_.running() ? "running" : "not running", backplate_.handshakes(),
                  backplate_.restarts(), backplate_reader_.bad_frames(), battery, climate);
      std::fflush(stdout);
    }
  }
  std::printf("stopped: handshakes %d, restarts %d, bad frames %zu\n", backplate_.handshakes(),
              backplate_.restarts(), backplate_reader_.bad_frames());
  return backplate_.handshakes() > 0 ? 0 : 1;
}

}  // namespace nesto
