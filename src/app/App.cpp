#include "app/App.hpp"

#include <algorithm>
#include <type_traits>
#include <variant>

#include "app/Ui.hpp"
#include "core/Log.hpp"

namespace nesto {
namespace {

// LVGL's tick callback takes no arguments, so it reaches the clock through
// this. There is only ever one App.
const Hal* g_clock = nullptr;

// Upper bound on how long the loop waits for input, so LVGL keeps animating
// and the gesture recogniser sees time pass.
constexpr Millis kMaxWait = 20;

const char* name(Gesture gesture) {
  switch (gesture) {
    case Gesture::Single: return "single";
    case Gesture::Double: return "double";
    case Gesture::Long: return "long";
  }
  return "?";
}

const char* name(PressContext context) {
  switch (context) {
    case PressContext::Ui: return "ui";
    case PressContext::Standby: return "standby";
    case PressContext::Off: return "off";
  }
  return "?";
}

void log_lvgl(lv_log_level_t level, const char* message) {
  (level >= LV_LOG_LEVEL_ERROR ? log::error : log::warn)("lvgl: %s", message);
}

}  // namespace

App::App(Hal& hal) : hal_(hal) {}

App::~App() {
  ui_.reset();
  if (display_) {
    lv_display_delete(display_);
    lv_deinit();
  }
  g_clock = nullptr;
}

bool App::init() {
  if (!hal_.init()) return false;

  g_clock = &hal_;
  lv_init();
  lv_log_register_print_cb(&log_lvgl);
  lv_tick_set_cb(&App::tick);

  // LVGL draws straight into one full-screen buffer (direct mode); the
  // hardware gets the whole frame once a refresh is complete.
  framebuffer_.assign(static_cast<std::size_t>(kScreenSize) * kScreenSize, 0);
  display_ = lv_display_create(kScreenSize, kScreenSize);
  lv_display_set_color_format(display_, LV_COLOR_FORMAT_XRGB8888);
  lv_display_set_buffers(display_, framebuffer_.data(), nullptr,
                         static_cast<std::uint32_t>(framebuffer_.size() * 4),
                         LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_user_data(display_, this);
  lv_display_set_flush_cb(display_, &App::flush);

  ui_ = std::make_unique<Ui>(NESTO_VERSION);
  hal_.set_backlight(100);
  log::info("Nesto %s started", NESTO_VERSION);
  return true;
}

int App::run() {
  std::vector<TimedInput> inputs;
  while (!quit_) {
    const std::uint32_t next_timer = lv_timer_handler();
    const Millis wait = std::min<Millis>(next_timer, kMaxWait);

    inputs.clear();
    hal_.poll(inputs, wait);
    for (const TimedInput& input : inputs) handle(input);
    handle(gestures_.tick(hal_.now()));
  }
  log::info("stopping");
  return exit_code_;
}

void App::flush(lv_display_t* display, const lv_area_t* /*area*/, std::uint8_t* /*pixels*/) {
  auto* self = static_cast<App*>(lv_display_get_user_data(display));
  if (lv_display_flush_is_last(display)) self->hal_.present(self->framebuffer_.data());
  lv_display_flush_ready(display);
}

std::uint32_t App::tick() {
  return g_clock ? static_cast<std::uint32_t>(g_clock->now()) : 0;
}

void App::handle(const TimedInput& input) {
  std::visit(
      [&](const auto& event) {
        using T = std::decay_t<decltype(event)>;
        if constexpr (std::is_same_v<T, RingTurn>) {
          ui_->rotate(event.steps);
        } else if constexpr (std::is_same_v<T, ButtonEdge>) {
          handle(event.down ? gestures_.on_down(input.at, press_context())
                            : gestures_.on_up(input.at));
        } else if constexpr (std::is_same_v<T, QuitRequest>) {
          quit_ = true;
          exit_code_ = event.exit_code;
        }
      },
      input.event);
}

void App::handle(const std::vector<GestureEvent>& gestures) {
  for (const GestureEvent& g : gestures) {
    // Reported to Home Assistant as an `input` event once the connection exists.
    log::info("gesture %s (%s)", name(g.gesture), name(g.context));
    if (g.context != PressContext::Ui) continue;
    if (g.gesture == Gesture::Single) ui_->select();
    if (g.gesture == Gesture::Long) ui_->back();
  }
}

PressContext App::press_context() const {
  // Standby and off arrive with display power control; until then the panel
  // is always showing its controls.
  return PressContext::Ui;
}

}  // namespace nesto
