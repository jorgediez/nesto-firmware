#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/PressGestures.hpp"
#include "hal/Hal.hpp"
#include "lvgl.h"

namespace nesto {

class Ui;

/// Ties the hardware, LVGL and the screens together and runs the main loop.
class App {
 public:
  explicit App(Hal& hal);
  ~App();

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  /// Starts the hardware and LVGL. Returns false if the hardware can't start.
  bool init();

  /// Runs until asked to quit. Returns the process exit code.
  int run();

 private:
  static void flush(lv_display_t* display, const lv_area_t* area, std::uint8_t* pixels);
  static std::uint32_t tick();

  void handle(const TimedInput& input);
  void handle(const std::vector<GestureEvent>& gestures);
  PressContext press_context() const;

  Hal& hal_;
  std::vector<std::uint32_t> framebuffer_;
  lv_display_t* display_ = nullptr;
  PressGestures gestures_;
  std::unique_ptr<Ui> ui_;
  bool quit_ = false;
  int exit_code_ = 0;
};

}  // namespace nesto
