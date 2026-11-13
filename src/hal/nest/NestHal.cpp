#include "hal/nest/NestHal.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
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
constexpr const char* kBeeperName = "pwm-beeper";
constexpr const char* kBackplate = "/dev/ttyO2";
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
    case backplate::kLight: return "ambient light";
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
  if (beeper_fd_ >= 0) {
    tone(0);  // never left sounding
    close(beeper_fd_);
  }
  hand_back_backplate();
  if (!backlight_.empty() && backlight_before_ >= 0) write_int(backlight_, backlight_before_);
  if (fb_) munmap(fb_, fb_size_);
  release_console();
  for (int fd : {fb_fd_, ring_fd_, press_fd_, backplate_fd_}) {
    if (fd >= 0) close(fd);
  }
}

bool NestHal::init() {
  catch_quit_signals();
  if (!open_framebuffer() || !open_inputs()) return false;
  claim_console();
  // If the console had blanked it already, as after 10 minutes in text mode.
  if (ioctl(fb_fd_, FBIOBLANK, FB_BLANK_UNBLANK) != 0) {
    log::warn("%s: can't unblank: %s", kFramebuffer, std::strerror(errno));
  }
  open_backlight();
  open_beeper();
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
    if (frame.type == backplate::kLight) {
      if (const auto light = backplate::decode_light(frame.payload)) {
        light_ = light;
        if (inputs) inputs->push_back({t, Light{light->value}});
      }
    } else if (frame.type == backplate::kProximity) {
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
      if (moving) {
        ++far_moving_;
        // Not all zero: someone moved in the room (docs/DEVICE.md).
        if (inputs) inputs->push_back({t, Presence{true}});
      }
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

void NestHal::open_beeper() {
  for (int i = 0; i < 16 && beeper_fd_ < 0; ++i) {
    char path[32];
    std::snprintf(path, sizeof path, "/dev/input/event%d", i);
    const int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    char found[64] = "";
    ioctl(fd, EVIOCGNAME(sizeof found - 1), found);
    if (std::strcmp(found, kBeeperName) == 0) {
      log::info("%s is %s", path, kBeeperName);
      beeper_fd_ = fd;
    } else {
      close(fd);
    }
  }
  if (beeper_fd_ < 0) log::warn("no %s: no sound", kBeeperName);
}

void NestHal::tone(int hz) {
  if (beeper_fd_ < 0) return;
  input_event events[2]{};
  events[0].type = EV_SND;
  events[0].code = SND_TONE;
  events[0].value = std::max(0, hz);
  events[1].type = EV_SYN;
  events[1].code = SYN_REPORT;
  if (write(beeper_fd_, events, sizeof events) != static_cast<ssize_t>(sizeof events)) {
    log::warn("%s: %s", kBeeperName, std::strerror(errno));
  }
}

void NestHal::click(int hz, int us) {
  if (beeper_fd_ < 0) return;
  tone(hz);
  usleep(static_cast<useconds_t>(std::clamp(us, 0, 10000)));
  tone(0);
}

int NestHal::sound_test() {
  open_beeper();
  if (beeper_fd_ < 0) return 1;
  auto say = [](const char* text) {
    std::printf("%s\n", text);
    std::fflush(stdout);
  };
  auto pause = [](int ms) { usleep(static_cast<useconds_t>(ms) * 1000); };
  // Clicks, ten in a row as a turn of the ring makes them.
  struct Click {
    const char* name;
    int hz;
    int us;
  };
  const Click clicks[] = {{"click A: 4000 Hz, 1 ms", 4000, 1000}, {"click B: 4000 Hz, 3 ms", 4000, 3000},
                          {"click C: 2000 Hz, 2 ms", 2000, 2000}, {"click D: 6000 Hz, 1 ms", 6000, 1000},
                          {"click E: 3000 Hz, 5 ms", 3000, 5000}};
  for (const Click& c : clicks) {
    say(c.name);
    for (int i = 0; i < 10; ++i) {
      click(c.hz, c.us);
      pause(70);
    }
    pause(1500);
  }
  // Beeps: on for so long, then off for so long, in turn.
  struct Beep {
    const char* name;
    std::vector<int> steps;  // hz, ms, hz, ms...; 0 Hz is silence
  };
  const Beep beeps[] = {{"short A: 2000 Hz, 100 ms", {2000, 100}},
                        {"short B: 2500 Hz, 150 ms", {2500, 150}},
                        {"alert A: 2700 Hz, 3 x 150 ms", {2700, 150, 0, 100, 2700, 150, 0, 100, 2700, 150}},
                        {"alert B: 3200/2400 Hz, 3 x 150 ms",
                         {3200, 150, 2400, 150, 3200, 150, 2400, 150, 3200, 150}}};
  for (const Beep& b : beeps) {
    say(b.name);
    for (std::size_t i = 0; i + 1 < b.steps.size(); i += 2) {
      tone(b.steps[i]);
      pause(b.steps[i + 1]);
    }
    tone(0);
    pause(2000);
  }
  say("done");
  return 0;
}

bool NestHal::open_inputs() {
  ring_fd_ = open_input(kRingName);
  press_fd_ = open_input(kPressName);
  return ring_fd_ >= 0 && press_fd_ >= 0;
}

void NestHal::claim_console() {
  // The kernel's text console shares the screen: without this it blanks it
  // after 10 minutes without console input (consoleblank=600), which this
  // LCD shows as a white flash and then black, whatever Nesto draws. In
  // graphics mode it neither blanks nor draws, as for the stock software
  // ("arm display: VT Mode: 1").
  for (const char* path : {"/dev/tty0", "/dev/tty1"}) {
    tty_fd_ = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (tty_fd_ >= 0) break;
  }
  if (tty_fd_ < 0) {
    log::warn("no console to put in graphics mode: %s", std::strerror(errno));
    return;
  }
  int mode = KD_TEXT;
  if (ioctl(tty_fd_, KDGETMODE, &mode) == 0) tty_mode_before_ = mode;
  if (ioctl(tty_fd_, KDSETMODE, KD_GRAPHICS) != 0) {
    log::warn("console: can't set graphics mode: %s", std::strerror(errno));
    // Blanking off, at least: setterm -blank 0.
    const char off[] = "\033[9;0]";
    if (write(tty_fd_, off, sizeof off - 1) < 0) log::warn("console: %s", std::strerror(errno));
  } else {
    log::info("console in graphics mode (was %s)", tty_mode_before_ == KD_TEXT ? "text" : "graphics");
  }
}

void NestHal::release_console() {
  if (tty_fd_ < 0) return;
  // As it was found, for the stock software, which sets it again itself.
  if (tty_mode_before_ >= 0) ioctl(tty_fd_, KDSETMODE, tty_mode_before_);
  close(tty_fd_);
  tty_fd_ = -1;
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

bool NestHal::set_wall_clock(std::int64_t utc_ms) {
  timespec ts{};
  ts.tv_sec = static_cast<time_t>(utc_ms / 1000);
  ts.tv_nsec = static_cast<long>(utc_ms % 1000) * 1000000;
  if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
    log::warn("can't set the wall clock: %s", std::strerror(errno));
    return false;
  }
  return true;
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
  last_frame_ = pixels;
  if (!dark_) copy_to_screen(pixels);
}

void NestHal::copy_to_screen(const std::uint32_t* pixels) {
  const std::size_t row_bytes = static_cast<std::size_t>(kScreenSize) * 4;
  for (int y = 0; y < kScreenSize; ++y) {
    const std::size_t at = static_cast<std::size_t>(y + y_offset_) * line_length_ +
                           static_cast<std::size_t>(x_offset_) * 4;
    if (at + row_bytes > fb_size_) return;
    if (pixels) {
      std::memcpy(fb_ + at, pixels + static_cast<std::size_t>(y) * kScreenSize, row_bytes);
    } else {
      std::memset(fb_ + at, 0, row_bytes);
    }
  }
}

void NestHal::set_backlight(int percent) {
  if (backlight_.empty()) return;
  percent = std::clamp(percent, 0, 100);
  // Round up, so any percentage above 0 still lights the screen.
  const int level = (percent * backlight_max_ + 99) / 100;
  if (!write_int(backlight_, level)) log::warn("%s: can't set it to %d", backlight_.c_str(), level);
}

BaseReadings NestHal::base() const {
  BaseReadings out;
  if (!options_.backplate) return out;
  out.connected = backplate_.running();
  out.firmware = backplate_.version();
  if (const auto& c = backplate_.climate()) {
    out.celsius = c->celsius;
    out.humidity = c->humidity;
  }
  if (const auto& p = backplate_.power()) {
    out.supply_v = p->input;
    out.usb_v = p->usb;
    out.battery_v = p->battery;
  }
  if (light_) out.light_raw = light_->value;
  return out;
}

void NestHal::set_screen_on(bool on) {
  if (on == !dark_ || !fb_) return;
  dark_ = !on;
  // Black while dark, rather than a blanked framebuffer: blanked, the LCD
  // turns white for most of a second before it goes dark (docs/DEVICE.md).
  copy_to_screen(on ? last_frame_ : nullptr);
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

int NestHal::light_test() {
  catch_quit_signals();
  if (!open_backplate()) return 1;
  open_backlight();
  std::printf(
      "Light readings, about one a second; the backlight switches between 0%% and 100%%\n"
      "every 10 s, to see whether it reaches the sensor. Try the room's lights on and off,\n"
      "and a torch near the screen. Ctrl-C stops.\n");
  std::fflush(stdout);
  constexpr Millis kPhase = 10000;
  const Millis start = now();
  int level = 0;
  int phase = -1;
  long sum_value = 0, sum_extra = 0;
  int count = 0, low = 0, high = 0;
  auto summary = [&] {
    if (count == 0) return;
    std::printf("== backlight %3d%%: %d readings, value mean %ld (%d to %d), extra mean %ld\n", level, count,
                sum_value / count, low, high, sum_extra / count);
    std::fflush(stdout);
  };
  while (g_quit_signal == 0) {
    const int now_phase = static_cast<int>((now() - start) / kPhase);
    if (now_phase != phase) {
      summary();
      phase = now_phase;
      level = phase % 2 == 0 ? 0 : 100;
      set_backlight(level);
      sum_value = sum_extra = 0;
      count = 0;
    }
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(backplate_fd_, &readable);
    timeval tv{0, 50000};
    select(backplate_fd_ + 1, &readable, nullptr, nullptr, &tv);
    light_.reset();
    service_backplate(false);
    if (light_) {
      // The first reading after a switch may be from before it.
      std::printf("%7.1f s  backlight %3d%%  value %5d  extra %3d\n", static_cast<double>(now() - start) / 1000.0, level,
                  light_->value, light_->extra);
      std::fflush(stdout);
      if (count == 0 || light_->value < low) low = light_->value;
      if (count == 0 || light_->value > high) high = light_->value;
      sum_value += light_->value;
      sum_extra += light_->extra;
      ++count;
    }
  }
  summary();
  return backplate_.handshakes() > 0 ? 0 : 1;
}

}  // namespace nesto
