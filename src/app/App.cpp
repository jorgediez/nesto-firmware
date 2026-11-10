#include "app/App.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <type_traits>
#include <utility>
#include <variant>

#include "app/Ui.hpp"
#include "core/Log.hpp"
#include "net/Mdns.hpp"
#include "net/NetInfo.hpp"
#include "net/Server.hpp"

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

// How often the status line is recomputed, which includes looking up the IP.
constexpr Millis kStatusInterval = 500;

/// Free memory in KiB from /proc/meminfo. Linux 2.6.37 has no MemAvailable, so
/// this adds free memory and the caches the kernel can drop.
long free_ram_kb() {
  std::ifstream file("/proc/meminfo");
  std::string line;
  long available = -1;
  long free_plus_caches = 0;
  while (std::getline(file, line)) {
    std::istringstream fields(line);
    std::string name;
    long kb = 0;
    fields >> name >> kb;
    if (name == "MemAvailable:") available = kb;
    if (name == "MemFree:" || name == "Buffers:" || name == "Cached:") free_plus_caches += kb;
  }
  return available >= 0 ? available : free_plus_caches;
}

}  // namespace

App::App(Hal& hal, AppOptions options) : hal_(hal), options_(std::move(options)) {}

App::~App() {
  mdns_.reset();  // says goodbye on the network
  server_.reset();
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

  info_.panel_id = options_.panel_id.empty() ? net::panel_id_from_mac() : options_.panel_id;
  if (info_.panel_id.empty()) {
    log::error("no network interface with a MAC address to use as the panel ID");
    return false;
  }
  info_.firmware = NESTO_VERSION;
  info_.model = "nest-gen2";
  // The base's sensors and firmware updates come later; until then the panel
  // reports none (PROTOCOL.md section 5).
  info_.caps = {{"display", {{"width", kScreenSize}, {"height", kScreenSize}, {"shape", "round"}}},
                {"input", {"ring", "press"}},
                {"sensors", protocol::Json::array()},
                {"beeper", false},
                {"image_formats", protocol::Json::array()},
                {"ota", false}};

  pairing_ = std::make_unique<protocol::PairingState>(
      std::make_unique<protocol::FileKeyStore>(options_.state_dir));
  protocol::SessionEvents& events = *this;  // a private base, so convert here
  server_ = std::make_unique<net::Server>(options_.port, info_, *pairing_, events,
                                          [this] { return hal_.now(); });
  if (!server_->start()) return false;

  if (options_.mdns) {
    mdns_ = std::make_unique<net::MdnsResponder>();
    // Without discovery the panel still works; Home Assistant needs its address.
    if (!mdns_->start()) mdns_.reset();
  }

  log::info("Nesto %s started: panel %s, %s, state in %s", NESTO_VERSION, info_.panel_id.c_str(),
            pairing_->paired() ? "paired" : "not paired", options_.state_dir.c_str());
  update_status();
  return true;
}

int App::run() {
  std::vector<TimedInput> inputs;
  while (!quit_) {
    const std::uint32_t next_timer = lv_timer_handler();
    const Millis wait = std::min<Millis>(next_timer, kMaxWait);

    // The network waits; the hardware is only checked, so neither delays
    // the other by more than one loop.
    server_->poll(wait);
    if (mdns_) mdns_->poll(hal_.now());
    inputs.clear();
    hal_.poll(inputs, 0);
    for (const TimedInput& input : inputs) handle(input);
    handle(gestures_.tick(hal_.now()));
    if (hal_.now() - status_checked_ >= kStatusInterval) update_status();
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

void App::update_status() {
  status_checked_ = hal_.now();
  const std::string ip = net::primary_ipv4();
  if (mdns_) {
    mdns_->update(net::make_service(info_.panel_id, options_.port, info_.firmware, info_.model,
                                    pairing_->paired()),
                  ip, hal_.now());
  }

  std::string text;
  const protocol::PanelSession* session = server_->active();
  if (session && session->ready()) {
    text = "Connected";
  } else if (session) {
    text = "Connecting";
  } else if (pairing_->paired()) {
    text = "Waiting for Home Assistant";
  } else {
    text = ip.empty() ? "Not paired - no network" : "Not paired - " + ip;
  }
  if (text != status_) {
    status_ = text;
    ui_->set_status(text);
  }
}

void App::on_pairing_code(const std::string& code) {
  // Logged too, so automated tests can check both sides agree. The log stays
  // on the device; the code only matters for the two minutes it's shown.
  log::info("pairing code %s", code.c_str());
  ui_->show_pairing_code(code);
}

void App::on_pairing_finished(bool paired) {
  ui_->hide_pairing_code();
  if (paired) ui_->show_toast("Paired with Home Assistant");
  update_status();
}

void App::on_authenticated(protocol::PanelSession& session) {
  log::info("Home Assistant authenticated");
  server_->close_others(session);
  update_status();
}

void App::on_ready(protocol::PanelSession& /*session*/) {
  log::info("synchronised with Home Assistant");
  update_status();
}

void App::on_unpaired() {
  ui_->show_toast("Removed from Home Assistant");
  update_status();
}

bool App::on_message(protocol::PanelSession& session, const protocol::Message& m) {
  if (m.type == "info" && m.is_request()) {
    protocol::Json result = protocol::result_ok(*m.id);
    result["uptime_s"] = hal_.now() / 1000;
    result["free_ram_kb"] = free_ram_kb();
    result["wifi_rssi"] = nullptr;
    result["ip"] = net::primary_ipv4();
    result["fw"] = NESTO_VERSION;
    result["base_connected"] = false;
    result["base_fw"] = nullptr;
    session.send(result);
    return true;
  }
  if (m.type == "identify" && m.is_request()) {
    ui_->show_toast("Hello from Home Assistant");
    session.send(protocol::result_ok(*m.id));
    return true;
  }
  if (m.type == "reboot" && m.is_request()) {
    session.send(protocol::result_error(*m.id, protocol::ErrorCode::Unsupported,
                                       "rebooting comes with the device support"));
    return true;
  }
  // Synchronisation messages are accepted; the screens built from them come
  // in a later version.
  return m.type == "time" || m.type == "config" || m.type == "layout" || m.type == "states";
}

PressContext App::press_context() const {
  // Standby and off arrive with display power control; until then the panel
  // is always showing its controls.
  return PressContext::Ui;
}

}  // namespace nesto
