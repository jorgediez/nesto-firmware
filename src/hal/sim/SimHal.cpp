#include "hal/sim/SimHal.hpp"

#include <algorithm>
#include <thread>
#include <utility>

#include "core/Log.hpp"
#include "core/Png.hpp"

#ifdef NESTO_HAVE_SDL
#include <SDL.h>
#endif

namespace nesto {
namespace {

// Outside the visible circle the real device shows its bezel; a dark grey
// stands in for it.
constexpr std::uint32_t kBezel = 0x00202020;

// How long to let animations finish before taking the screenshot.
constexpr Millis kSettleTime = 600;

constexpr std::size_t kPixels = static_cast<std::size_t>(kScreenSize) * kScreenSize;

std::uint32_t dim(std::uint32_t pixel, int percent) {
  const auto channel = [&](int shift) {
    const std::uint32_t c = (pixel >> shift) & 0xFF;
    return (c * static_cast<std::uint32_t>(percent) / 100) << shift;
  };
  return channel(16) | channel(8) | channel(0);
}

}  // namespace

SimHal::SimHal(SimOptions options) : options_(std::move(options)) {}

SimHal::~SimHal() {
#ifdef NESTO_HAVE_SDL
  if (texture_) SDL_DestroyTexture(texture_);
  if (renderer_) SDL_DestroyRenderer(renderer_);
  if (window_) SDL_DestroyWindow(window_);
  if (window_ || renderer_ || texture_) SDL_Quit();
#endif
}

bool SimHal::init() {
  start_ = std::chrono::steady_clock::now();

  std::string error;
  auto script = parse_input_script(options_.script, error);
  if (!script) {
    log::error("input script: %s", error.c_str());
    return false;
  }
  script_ = std::move(*script);

  frame_.assign(kPixels, kBezel);
  inside_circle_.resize(kPixels);
  const double r = kScreenSize / 2.0;
  for (int y = 0; y < kScreenSize; ++y) {
    for (int x = 0; x < kScreenSize; ++x) {
      const double dx = x + 0.5 - r;
      const double dy = y + 0.5 - r;
      inside_circle_[static_cast<std::size_t>(y) * kScreenSize + x] = dx * dx + dy * dy <= r * r;
    }
  }

  if (options_.headless) {
    log::info("simulator running headless");
    return true;
  }
#ifdef NESTO_HAVE_SDL
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    log::error("SDL: %s", SDL_GetError());
    return false;
  }
  const int size = kScreenSize * std::clamp(options_.scale, 1, 4);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  window_ = SDL_CreateWindow("Nesto Panel (simulator)", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, size, size, 0);
  renderer_ = window_ ? SDL_CreateRenderer(window_, -1, 0) : nullptr;
  texture_ = renderer_ ? SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING, kScreenSize, kScreenSize)
                       : nullptr;
  if (!texture_) {
    log::error("SDL: %s", SDL_GetError());
    return false;
  }
  show_frame();
  return true;
#else
  log::error("built without SDL2: use --headless");
  return false;
#endif
}

Millis SimHal::now() const {
  return static_cast<Millis>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start_)
                                 .count());
}

void SimHal::poll(std::vector<TimedInput>& out, Millis timeout) {
  const Millis deadline = now() + timeout;
  for (;;) {
    play_script(out);
    poll_window(out);
    const Millis t = now();
    if (!out.empty() || t >= deadline) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min<Millis>(5, deadline - t)));
  }
}

void SimHal::play_script(std::vector<TimedInput>& out) {
  const Millis t = now();
  while (next_step_ < script_.events.size() && script_.events[next_step_].offset <= t) {
    out.push_back({t, script_.events[next_step_].event});
    ++next_step_;
  }

  if (options_.screenshot.empty() || screenshot_taken_) return;
  if (next_step_ < script_.events.size() || t < script_.duration + kSettleTime) return;

  screenshot_taken_ = true;
  const bool saved = write_png(options_.screenshot, frame_.data(), kScreenSize, kScreenSize);
  if (saved) {
    log::info("screenshot saved to %s", options_.screenshot.c_str());
  } else {
    log::error("can't write %s", options_.screenshot.c_str());
  }
  out.push_back({t, QuitRequest{saved ? 0 : 1}});
}

void SimHal::poll_window(std::vector<TimedInput>& out) {
#ifdef NESTO_HAVE_SDL
  if (!window_) return;
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    const Millis t = now();
    switch (e.type) {
      case SDL_QUIT:
        out.push_back({t, QuitRequest{}});
        break;
      case SDL_MOUSEWHEEL: {
        // Scrolling down turns the ring clockwise.
        int steps = -e.wheel.y;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) steps = -steps;
        if (steps != 0) out.push_back({t, RingTurn{steps * kTicksPerClick}});
        break;
      }
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
        if (e.button.button == SDL_BUTTON_LEFT) {
          out.push_back({t, ButtonEdge{e.type == SDL_MOUSEBUTTONDOWN}});
        }
        break;
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        const bool down = e.type == SDL_KEYDOWN;
        switch (e.key.keysym.sym) {
          case SDLK_RIGHT:
          case SDLK_DOWN:
            if (down) out.push_back({t, RingTurn{kTicksPerClick}});
            break;
          case SDLK_LEFT:
          case SDLK_UP:
            if (down) out.push_back({t, RingTurn{-kTicksPerClick}});
            break;
          case SDLK_SPACE:
          case SDLK_RETURN:
            if (!e.key.repeat) out.push_back({t, ButtonEdge{down}});
            break;
          case SDLK_m:  // someone comes near
            if (down) out.push_back({t, Presence{}});
            break;
          case SDLK_ESCAPE:
            if (down) out.push_back({t, QuitRequest{}});
            break;
          default:
            break;
        }
        break;
      }
      case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_EXPOSED) show_frame();
        break;
      default:
        break;
    }
  }
#else
  (void)out;
#endif
}

void SimHal::present(const std::uint32_t* pixels) {
  raw_.assign(pixels, pixels + kPixels);
  compose();
}

void SimHal::set_backlight(int percent) {
  backlight_ = std::clamp(percent, 0, 100);
  if (!raw_.empty()) compose();
}

void SimHal::compose() {
  for (std::size_t i = 0; i < kPixels; ++i) {
    frame_[i] = inside_circle_[i] ? dim(raw_[i], backlight_) : kBezel;
  }
  show_frame();
}

void SimHal::show_frame() {
#ifdef NESTO_HAVE_SDL
  if (!texture_) return;
  SDL_UpdateTexture(texture_, nullptr, frame_.data(), kScreenSize * 4);
  SDL_RenderClear(renderer_);
  SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
  SDL_RenderPresent(renderer_);
#endif
}

}  // namespace nesto
