#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "core/InputScript.hpp"
#include "hal/Hal.hpp"

#ifdef NESTO_HAVE_SDL
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
#endif

namespace nesto {

struct SimOptions {
  bool headless = false;   // no window, even if SDL2 is available
  int scale = 2;           // window pixels per screen pixel
  std::string script;      // input steps to play, see InputScript.hpp
  std::string screenshot;  // PNG to write once the script has played, then quit
};

/// The desktop simulator. With SDL2 it opens a window showing the round screen:
///
///   mouse wheel, arrow keys      turn the ring
///   left button, Space, Enter    press the display
///   Escape, closing the window   quit
///
/// Headless, or with a script, it plays scripted input and can save the screen
/// as a PNG, which is how screens are checked without a person watching.
class SimHal : public Hal {
 public:
  explicit SimHal(SimOptions options);
  ~SimHal() override;

  SimHal(const SimHal&) = delete;
  SimHal& operator=(const SimHal&) = delete;

  bool init() override;
  Millis now() const override;
  void poll(std::vector<TimedInput>& out, Millis timeout) override;
  void present(const std::uint32_t* pixels) override;
  void set_backlight(int percent) override;

 private:
  void play_script(std::vector<TimedInput>& out);
  void poll_window(std::vector<TimedInput>& out);
  void compose();  // raw_ + backlight + round mask -> frame_, then show it
  void show_frame();

  SimOptions options_;
  std::chrono::steady_clock::time_point start_;

  InputScript script_;
  std::size_t next_step_ = 0;
  int light_step_ = 1;  // the L key: starts as a lit room
  bool screenshot_taken_ = false;

  std::vector<std::uint32_t> raw_;    // the last frame as LVGL drew it
  std::vector<std::uint32_t> frame_;  // the same, as the user sees it
  std::vector<bool> inside_circle_;
  int backlight_ = 100;

#ifdef NESTO_HAVE_SDL
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
#endif
};

}  // namespace nesto
