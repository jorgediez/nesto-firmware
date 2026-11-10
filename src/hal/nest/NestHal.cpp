#include "hal/nest/NestHal.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
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

NestHal::NestHal(NestOptions options) : options_(options), ring_ticks_(options.ring) {}

NestHal::~NestHal() {
  if (!backlight_.empty() && backlight_before_ >= 0) write_int(backlight_, backlight_before_);
  if (fb_) munmap(fb_, fb_size_);
  for (int fd : {fb_fd_, ring_fd_, press_fd_}) {
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
  return true;
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
    for (int fd : {ring_fd_, press_fd_}) {
      if (fd < 0) continue;
      FD_SET(fd, &readable);
      max_fd = std::max(max_fd, fd);
    }
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout % 1000) * 1000);
    select(max_fd + 1, &readable, nullptr, nullptr, &tv);  // EINTR: a signal, seen below
    read_inputs(out, false);
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

}  // namespace nesto
