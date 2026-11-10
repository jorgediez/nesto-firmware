#pragma once

#include <cstddef>
#include <cstdint>

#include "hal/Hal.hpp"

namespace nesto {

/// The thermostat. For now it only draws: the input devices, backlight and
/// base are added once their details have been checked on a real device
/// (Phase 2 of the plan). Until then poll() just waits and reports nothing.
class NestHal : public Hal {
 public:
  NestHal() = default;
  ~NestHal() override;

  NestHal(const NestHal&) = delete;
  NestHal& operator=(const NestHal&) = delete;

  bool init() override;
  Millis now() const override;
  void poll(std::vector<TimedInput>& out, Millis timeout) override;
  void present(const std::uint32_t* pixels) override;
  void set_backlight(int percent) override;

 private:
  int fb_fd_ = -1;
  std::uint8_t* fb_ = nullptr;
  std::size_t fb_size_ = 0;
  std::size_t line_length_ = 0;  // bytes per framebuffer row
  int x_offset_ = 0;             // where the 320x320 image starts
  int y_offset_ = 0;
};

}  // namespace nesto
