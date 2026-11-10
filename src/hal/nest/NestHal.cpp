#include "hal/nest/NestHal.hpp"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <thread>

#include "core/Log.hpp"

namespace nesto {
namespace {

constexpr const char* kFramebuffer = "/dev/fb0";

}  // namespace

NestHal::~NestHal() {
  if (fb_) munmap(fb_, fb_size_);
  if (fb_fd_ >= 0) close(fb_fd_);
}

bool NestHal::init() {
  fb_fd_ = open(kFramebuffer, O_RDWR);
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
  log::info("%s: %ux%u, %u bits per pixel, %u bytes per line", kFramebuffer, var.xres, var.yres,
            var.bits_per_pixel, fix.line_length);

  // Reported as 320x320 at 32 bits per pixel by the community's research;
  // anything else needs a conversion this code doesn't have yet.
  if (var.bits_per_pixel != 32 || var.xres < static_cast<unsigned>(kScreenSize) ||
      var.yres < static_cast<unsigned>(kScreenSize)) {
    log::error("%s: expected at least %dx%d at 32 bits per pixel", kFramebuffer, kScreenSize,
               kScreenSize);
    return false;
  }

  line_length_ = fix.line_length;
  fb_size_ = fix.smem_len;
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

Millis NestHal::now() const {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<Millis>(ts.tv_sec) * 1000 + static_cast<Millis>(ts.tv_nsec / 1000000);
}

void NestHal::poll(std::vector<TimedInput>& out, Millis timeout) {
  (void)out;  // ring and press: read from evdev once their devices are known
  std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
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
  (void)percent;  // the backlight's control file is confirmed in Phase 2
}

}  // namespace nesto
