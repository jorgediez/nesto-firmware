#include "app/App.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
// The same while the display is in standby or off.
constexpr Millis kRestingWait = 100;

// Where the display settings are kept, in the state directory.
constexpr const char* kDisplayConfigFile = "display.json";
// Where the last UTC offset is kept, so the clock shows before Home Assistant
// connects.
constexpr const char* kTimeFile = "time.json";
// How far the system clock may be from Home Assistant's before it's set.
constexpr std::int64_t kWallClockSlack = 2000;

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

// Intents whose results are still awaited; older ones are forgotten.
constexpr std::size_t kMaxOpenIntents = 64;

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

App::App(Hal& hal, AppOptions options)
    : hal_(hal),
      options_(std::move(options)),
      controller_([this](const room::Intent& intent) { send_intent(intent); },
                  room::SettingsActions{[this] { open_pairing(); },
                                        [this] { forget_home_assistant(); },
                                        [this] { return about(); }}) {}

App::~App() {
  mdns_.reset();  // says goodbye on the network
  server_.reset();
  ui_.reset();
  fonts_.destroy();
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

  fonts_ = Fonts::load();
  ui_ = std::make_unique<Ui>(fonts_);
  // The display settings Home Assistant sent last, so the panel dims and
  // sleeps as it should before Home Assistant connects.
  load_display_config();
  load_time();
  hal_.set_screen_on(true);
  backlight_ = room::BacklightFade(display_power_.brightness());
  update_backlight();

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
    // While the display rests, the loop waits longer: less work, less power,
    // and waking takes at most that long. Not while it fades, which would
    // then go in steps.
    const bool resting = display_power_.state() != room::DisplayState::Active && !gestures_.busy() &&
                         !backlight_.fading(hal_.now());
    const Millis wait = std::min<Millis>(next_timer, resting ? kRestingWait : kMaxWait);

    // The network waits; the hardware is only checked, so neither delays
    // the other by more than one loop.
    server_->poll(wait);
    if (mdns_) mdns_->poll(hal_.now());
    inputs.clear();
    hal_.poll(inputs, 0);
    for (const TimedInput& input : inputs) handle(input);
    handle(gestures_.tick(hal_.now()));
    display_power_.tick(hal_.now());
    apply_display();
    update_backlight();
    update_clock();
    if (hal_.now() - status_checked_ >= kStatusInterval) update_status();

    controller_.tick(hal_.now());
    if (controller_.version() != rendered_version_) {
      rendered_version_ = controller_.version();
      ui_->render(controller_.view());
    }
    if (auto message = controller_.take_message()) ui_->show_toast(*message);
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
          // A turn that wakes the display only wakes it.
          if (display_power_.input(input.at)) controller_.rotate(event.ticks, input.at);
        } else if constexpr (std::is_same_v<T, ButtonEdge>) {
          // While active, a press is activity. While resting, it's the
          // gesture that decides: a single press or a hold wakes the display,
          // a double press doesn't (PROTOCOL.md section 13).
          if (display_power_.state() == room::DisplayState::Active) display_power_.input(input.at);
          handle(event.down ? gestures_.on_down(input.at, press_context()) : gestures_.on_up(input.at));
        } else if constexpr (std::is_same_v<T, Presence>) {
          const bool resting = display_power_.state() != room::DisplayState::Active;
          display_power_.motion(input.at);
          if (resting && display_power_.state() == room::DisplayState::Active) {
            log::info("display woken by the %s sensor", event.far ? "far-field motion" : "proximity");
          }
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
    if (g.context != PressContext::Ui) {
      // Pressed while resting: it only wakes the display, and a double press
      // not even that.
      if (g.gesture != Gesture::Double) display_power_.input(hal_.now());
      continue;
    }
    if (g.gesture == Gesture::Single) controller_.press(hal_.now());
    if (g.gesture == Gesture::Long) controller_.long_press(hal_.now());
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
  } else if (pairing_->paired() && pairing_->pairing_open(hal_.now())) {
    text = "Pairing open";
  } else if (pairing_->paired()) {
    text = "Waiting for Home Assistant";
  } else {
    text = ip.empty() ? "Not paired - no network" : "Not paired - " + ip;
  }
  if (text != status_) {
    status_ = text;
    ui_->set_status(text);
  }
  controller_.set_connected(session && session->ready());
}

void App::send_intent(const room::Intent& intent) {
  protocol::PanelSession* session = server_ ? server_->active() : nullptr;
  if (!session || !session->ready()) return;
  // Results come back with this id; only failures matter (section 11).
  const std::uint32_t id = next_request_id_++;
  if (next_request_id_ == 0) next_request_id_ = 1;
  intent_requests_[id] = intent.key;
  if (intent_requests_.size() > kMaxOpenIntents) intent_requests_.erase(intent_requests_.begin());
  session->send(protocol::Json{{"t", "intent"},
                               {"id", id},
                               {"key", intent.key},
                               {"action", intent.action},
                               {"args", intent.args}});
}

void App::open_pairing() {
  if (!pairing_->paired()) {
    ui_->show_toast("Ready to pair");
    return;
  }
  pairing_->open_pairing_window(hal_.now());
  log::info("pairing opened for five minutes");
  ui_->show_toast("Pairing open for 5 minutes");
  update_status();
}

void App::forget_home_assistant() {
  pairing_->forget();
  server_->close_all("the panel forgot Home Assistant");
  log::info("forgot Home Assistant");
  ui_->show_toast("Home Assistant forgotten");
  update_status();
}

std::vector<std::string> App::about() const {
  const std::string ip = net::primary_ipv4();
  return {std::string("Nesto ") + NESTO_VERSION,
          "Panel " + info_.panel_id,
          ip.empty() ? "No network" : ip + ":" + std::to_string(options_.port),
          pairing_->paired() ? "Paired" : "Not paired"};
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
  // Where the display is, for Home Assistant to start from (section 13); a
  // change from the synchronisation's config is part of it.
  apply_display(false);
  send_display_state(display_power_.state(), display_power_.brightness(), "sync");
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
  if (m.type == "layout") {
    std::string error;
    if (!controller_.apply_layout(m.body, error)) log::warn("ignoring a bad layout: %s", error.c_str());
    return true;
  }
  if (m.type == "states") {
    controller_.apply_states(m.body);
    return true;
  }
  if (m.type == "result") {
    const auto it = intent_requests_.find(m.body.value("re", 0u));
    if (it != intent_requests_.end()) {
      if (!m.body.value("ok", false)) {
        const auto& error = m.body.value("error", protocol::Json::object());
        log::warn("intent for %s failed: %s", it->second.c_str(),
                  error.value("message", std::string("unknown error")).c_str());
        controller_.intent_failed(it->second);
      }
      intent_requests_.erase(it);
    }
    return true;
  }
  if (m.type == "config") {
    // The whole object every time (PROTOCOL.md section 12); what's missing
    // takes the default.
    const room::DisplayConfig config = room::DisplayConfig::from_config(m.body);
    if (!(config == display_power_.config())) {
      display_power_.configure(config, hal_.now());
      store_display_config();
      log::info("display: %d%%, %us to standby, standby %s at %d%%", config.brightness, config.timeout_s,
                config.standby_mode == room::StandbyMode::Off ? "off" : "on", config.standby_brightness);
    }
    return true;
  }
  if (m.type == "display" && m.is_request()) {
    const auto state = room::parse_display_state(m.body.value("state", std::string()));
    if (!state) {
      session.send(protocol::result_error(*m.id, protocol::ErrorCode::BadRequest,
                                         "state must be active, standby or off"));
      return true;
    }
    display_power_.set(*state, hal_.now());
    apply_display();
    session.send(protocol::result_ok(*m.id));
    return true;
  }
  if (m.type == "time") {
    set_time(m.body);
    return true;
  }
  return false;
}

PressContext App::press_context() const {
  switch (display_power_.state()) {
    case room::DisplayState::Active: return PressContext::Ui;
    case room::DisplayState::Standby: return PressContext::Standby;
    case room::DisplayState::Off: return PressContext::Off;
  }
  return PressContext::Ui;
}

void App::apply_display(bool report) {
  const auto change = display_power_.take_change();
  if (!change) return;
  // Lit: the screen first, then the backlight, at once. Dark: the backlight
  // fades, and update_backlight() darkens the screen at the end, as a dark
  // standby is as good as off for the screen itself.
  backlight_.go(change->brightness, hal_.now());
  // What the new state shows is drawn before the backlight rises.
  update_clock();
  if (change->brightness > 0) hal_.set_screen_on(true);
  if (backlight_.fading(hal_.now())) fade_stats_ = {true, hal_.now(), hal_.now(), 0, 0, 0};
  update_backlight();
  log::info("display %s at %d%% (%s)", room::name(change->state), change->brightness,
            room::name(change->cause));
  if (report) send_display_state(change->state, change->brightness, room::name(change->cause));
}

void App::update_backlight() {
  const Millis t = hal_.now();
  const int level = backlight_.level(t);
  if (level != backlight_level_) {
    hal_.set_backlight(level);
    backlight_level_ = level;
    if (fade_stats_.active) {
      ++fade_stats_.steps;
      fade_stats_.longest_gap = std::max(fade_stats_.longest_gap, t - fade_stats_.last_step);
      fade_stats_.longest_write = std::max(fade_stats_.longest_write, hal_.now() - t);
      fade_stats_.last_step = t;
    }
  }
  if (level == 0) hal_.set_screen_on(false);
  if (fade_stats_.active && !backlight_.fading(t)) {
    fade_stats_.active = false;
    log::info("backlight: faded to %d%% in %d steps over %llu ms; "
              "longest gap %llu ms, longest write %llu ms",
              level, fade_stats_.steps, static_cast<unsigned long long>(hal_.now() - fade_stats_.started),
              static_cast<unsigned long long>(fade_stats_.longest_gap),
              static_cast<unsigned long long>(fade_stats_.longest_write));
  }
}

void App::update_clock() {
  const Millis now = hal_.now();
  const auto local = wall_clock_.local(now);
  const bool wanted = display_power_.state() == room::DisplayState::Standby && local &&
                      backlight_.target() > 0 && !backlight_.fading(now);
  if (!wanted) {
    if (ui_->clock_shown()) {
      ui_->hide_clock();
      clock_drawn_.clear();
      lv_refr_now(display_);  // gone before the backlight rises for the controls
    }
    return;
  }
  const room::DisplayConfig& c = display_power_.config();
  const room::ClockFace face = room::clock_face(*local, c.hour12, c.clock_seconds);
  const bool analog = c.clock_style == room::ClockStyle::Analog;
  const std::string drawn = face.time + face.seconds + face.suffix + (c.clock_date ? face.date : "") +
                            (analog ? "a" : "d");
  if (drawn == clock_drawn_) return;
  const bool appearing = !ui_->clock_shown();
  ui_->show_clock(face, c.clock_style, c.clock_date);
  clock_drawn_ = drawn;
  if (appearing) {
    lv_refr_now(display_);
    log::info("standby clock shown, %s: %s%s%s%s", analog ? "analog" : "digital", face.time.c_str(),
              face.seconds.c_str(), face.suffix.empty() ? "" : " ", face.suffix.c_str());
  }
}

void App::set_time(const protocol::Json& body) {
  const auto utc = body.find("utc_ms");
  const auto offset = body.find("utc_offset_min");
  if (utc == body.end() || !utc->is_number() || offset == body.end() || !offset->is_number_integer() ||
      std::abs(offset->get<long long>()) > 18 * 60) {
    log::warn("ignoring a time message without utc_ms and a utc_offset_min");
    return;
  }
  const Millis now = hal_.now();
  const auto utc_ms = static_cast<std::int64_t>(utc->get<double>());
  const int offset_min = static_cast<int>(offset->get<long long>());
  const bool first = !wall_clock_.known();
  wall_clock_.set_time(utc_ms, now);
  // The system clock too, so logs and anything else on the device agree.
  const std::int64_t off_by = hal_.wall_clock_ms() - utc_ms;
  const bool set = std::llabs(off_by) > kWallClockSlack && hal_.set_wall_clock(utc_ms);
  if (!wall_clock_.has_offset() || wall_clock_.offset_min() != offset_min) {
    wall_clock_.set_offset(offset_min);
    store_time();
  }
  const room::LocalTime t = *wall_clock_.local(now);
  log::info("time from Home Assistant: %04d-%02d-%02d %02d:%02d:%02d, UTC%+d min%s%s", t.year, t.month,
            t.day, t.hour, t.minute, t.second, offset_min, set ? "; the system clock was off by " : "",
            set ? (std::to_string(off_by) + " ms and is set").c_str() : "");
  if (first) {
    display_power_.set_has_standby_screen(true);
    apply_display();
  }
}

void App::load_time() {
  std::ifstream file(options_.state_dir + "/" + kTimeFile);
  const protocol::Json stored = file ? protocol::Json::parse(file, nullptr, false) : protocol::Json();
  const auto offset = stored.is_object() ? stored.find("utc_offset_min") : stored.end();
  const std::int64_t system = hal_.wall_clock_ms();
  if (!stored.is_object() || offset == stored.end() || !offset->is_number_integer()) {
    log::info("no stored UTC offset: no clock until Home Assistant sends the time");
    return;
  }
  if (system < room::WallClock::kEarliestUtcMs) {
    log::info("the system clock was never set: no clock until Home Assistant sends the time");
    return;
  }
  // The system clock kept time (the hardware clock, or the stock firmware's
  // network time): the clock shows before Home Assistant connects.
  wall_clock_.set_offset(static_cast<int>(offset->get<long long>()));
  wall_clock_.set_time(system, hal_.now());
  const room::LocalTime t = *wall_clock_.local(hal_.now());
  log::info("time from the system clock: %04d-%02d-%02d %02d:%02d, UTC%+d min stored", t.year, t.month,
            t.day, t.hour, t.minute, wall_clock_.offset_min());
  display_power_.set_has_standby_screen(true);
  display_power_.take_change();  // still active: nothing to apply
}

void App::store_time() const {
  const std::string path = options_.state_dir + "/" + kTimeFile;
  const std::string temporary = path + ".new";
  {
    std::ofstream file(temporary, std::ios::trunc);
    file << protocol::Json{{"utc_offset_min", wall_clock_.offset_min()}}.dump() << '\n';
    if (!file.flush()) {
      log::warn("can't store the UTC offset in %s", temporary.c_str());
      return;
    }
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    log::warn("can't store the UTC offset in %s", path.c_str());
  }
}

void App::send_display_state(room::DisplayState state, int brightness, const char* cause) {
  protocol::PanelSession* session = server_ ? server_->active() : nullptr;
  if (!session || !session->ready()) return;
  session->send(protocol::Json{
      {"t", "display_state"}, {"state", room::name(state)}, {"brightness", brightness}, {"cause", cause}});
}

void App::load_display_config() {
  room::DisplayConfig config;
  std::ifstream file(options_.state_dir + "/" + kDisplayConfigFile);
  if (file) {
    const protocol::Json stored = protocol::Json::parse(file, nullptr, false);
    if (stored.is_discarded()) {
      log::warn("%s/%s isn't JSON; using the default display settings", options_.state_dir.c_str(),
                kDisplayConfigFile);
    } else {
      config = room::DisplayConfig::from_config(stored);
    }
  }
  display_power_.configure(config, hal_.now());
  display_power_.take_change();  // the starting state, applied by the caller
}

void App::store_display_config() const {
  // Written aside and renamed, so a power cut never leaves half a file.
  const std::string path = options_.state_dir + "/" + kDisplayConfigFile;
  const std::string temporary = path + ".new";
  {
    std::ofstream file(temporary, std::ios::trunc);
    file << display_power_.config().to_config().dump() << '\n';
    if (!file.flush()) {
      log::warn("can't store the display settings in %s", temporary.c_str());
      return;
    }
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    log::warn("can't store the display settings in %s", path.c_str());
  }
}

}  // namespace nesto
