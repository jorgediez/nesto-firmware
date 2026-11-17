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
// Auto brightness waits for a change of this many points, so the screen
// doesn't keep shifting.
constexpr int kAutoStep = 5;
// With --hold-for-stock: how long after starting a hold counts, and how long
// the display must be held.
constexpr Millis kHoldWindow = 3000;
constexpr Millis kHoldTime = 1500;
// How long identify pulses the backlight; its beeps take as long.
constexpr Millis kIdentifyTime = 3000;
// A critical notification's alert repeats this often (PROTOCOL.md section 14).
constexpr Millis kAlertRepeat = 30000;

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

// Technical info, while it's open: the readings and a ping this often, and
// connman's view of the network (which takes a moment to ask) less often.
constexpr Millis kInfoInterval = 3000;
constexpr Millis kNetworkInterval = 30000;
// A ping unanswered for this long is given up on.
constexpr Millis kPingTimeout = 5000;
// The Wi-Fi interface on the thermostat.
constexpr const char* kWifiInterface = "wlan0";
// Home Assistant's versions are short; anything longer is cut.
constexpr std::size_t kMaxVersion = 32;

// Wi-Fi setup: where connman reads provisioning files, and how often the
// list is scanned again while it's open.
constexpr const char* kConnmanDir = "/var/lib/connman";
constexpr Millis kWifiRescan = 20000;

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
                                        [this] { open_info(); },
                                        [this] { wifi_.open(); },
                                        [this] { notifications_.open_list(); }}),
      notifications_([this](const protocol::Json& event) { send_event(event); }),
      wifi_(room::WifiSetup::Actions{[this] {
                                       last_wifi_scan_ = hal_.now();
                                       wifi_backend_->scan(hal_.now());
                                     },
                                     [this](const WifiConnect& request) {
                                       wifi_backend_->connect(request, hal_.now());
                                     },
                                     [this](const WifiNetwork& network) {
                                       wifi_backend_->forget(network, hal_.now());
                                     }}) {
  // The thermostat's Wi-Fi through connman, or a pretend one.
  if (options_.fake_wifi) {
    wifi_backend_ = std::make_unique<net::FakeWifi>();
  } else {
    wifi_backend_ = std::make_unique<net::ConnmanWifi>(std::make_unique<net::ProcessRunner>(), kConnmanDir);
  }
}

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
  // The base's sensors, with one; firmware updates come later (PROTOCOL.md
  // section 5).
  protocol::Json sensors = protocol::Json::array();
  if (hal_.has_base()) {
    sensors = {"temperature",  "humidity",       "illuminance", "motion_near",
               "motion_far",   "supply_voltage", "usb_voltage", "battery_voltage"};
  }
  info_.caps = {{"display", {{"width", kScreenSize}, {"height", kScreenSize}, {"shape", "round"}}},
                {"input", {"ring", "press"}},
                {"sensors", sensors},
                {"beeper", hal_.has_beeper()},
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
  if (options_.hold_for_stock) {
    hold_window_until_ = hal_.now() + kHoldWindow;
    ui_->show_toast("Hold to start Nest", static_cast<std::uint32_t>(kHoldWindow));
  }
  return true;
}

void App::check_hold() {
  if (held_since_ == 0 || hal_.now() - held_since_ < kHoldTime) return;
  log::info("the display was held as Nesto started: exiting for the stock software");
  ui_->show_toast("Starting Nest");
  lv_refr_now(display_);
  quit_ = true;
  exit_code_ = kHeldExit;
}

int App::run() {
  std::vector<TimedInput> inputs;
  while (!quit_) {
    const std::uint32_t next_timer = lv_timer_handler();
    // While the display rests, the loop waits longer: less work, less power,
    // and waking takes at most that long. Not while it fades, which would
    // then go in steps.
    const bool resting = display_power_.state() != room::DisplayState::Active && !gestures_.busy() &&
                         !backlight_.moving(hal_.now()) && !sound_.playing() &&
                         hal_.now() >= identify_until_;
    const Millis wait = std::min<Millis>(next_timer, resting ? kRestingWait : kMaxWait);

    // The network waits; the hardware is only checked, so neither delays
    // the other by more than one loop.
    server_->poll(wait);
    if (mdns_) mdns_->poll(hal_.now());
    inputs.clear();
    hal_.poll(inputs, 0);
    for (const TimedInput& input : inputs) handle(input);
    check_hold();
    handle(gestures_.tick(hal_.now()));
    display_power_.tick(hal_.now());
    apply_display();
    update_auto_brightness();
    update_backlight();
    update_clock();
    send_sensors();
    if (hal_.now() - status_checked_ >= kStatusInterval) update_status();

    controller_.tick(hal_.now());
    update_info();
    update_wifi();
    notifications_.tick(hal_.now());
    // A pop-up on its timer keeps the display up until it's had its time.
    if (notifications_.holds_display() && display_power_.state() == room::DisplayState::Active) {
      display_power_.input(hal_.now());
    }
    // So does joining a network, to show how it went.
    if (wifi_.joining() && display_power_.state() == room::DisplayState::Active) {
      display_power_.input(hal_.now());
    }
    controller_.set_notifications(notifications_.count(), notifications_.unread());
    update_sound();
    render();
    if (auto message = controller_.take_message()) ui_->show_toast(*message);
  }
  log::info("stopping");
  return exit_code_;
}

std::string App::ring_view() const {
  const room::RoomView v = notifications_.active() ? notifications_.view()
                           : wifi_.active()        ? wifi_.view()
                                                   : controller_.view();
  return std::to_string(static_cast<int>(v.screen)) + "|" + std::to_string(v.selected) + "|" + v.value +
         "|" + std::to_string(v.choice) + "|" + std::to_string(v.scroll);
}

void App::click(Millis now) {
  if (now - last_click_ < room::sounds::kClickGap && last_click_ != 0) return;
  last_click_ = now;
  hal_.click(room::sounds::kClickHz, room::sounds::kClickUs);
}

void App::update_sound() {
  const Millis now = hal_.now();
  // A critical notification's alert, again every 30 s until it's answered.
  if (notifications_.critical_waiting() && now - last_alert_ >= kAlertRepeat) {
    sound_.play(room::sounds::alert(), now);
    last_alert_ = now;
  }
  if (const auto hz = sound_.tick(now)) hal_.tone(*hz);
}

void App::render() {
  // A notification screen over everything; then Wi-Fi's, over Settings; or
  // the room's.
  const Drawn source = notifications_.active() ? Drawn::Notices : wifi_.active() ? Drawn::Wifi : Drawn::Room;
  const bool fresh = source != drawn_;
  drawn_ = source;
  switch (source) {
    case Drawn::Notices:
      if (fresh || notifications_.version() != rendered_notices_) {
        rendered_notices_ = notifications_.version();
        ui_->render(notifications_.view());
        notifications_.set_scroll_limit(ui_->notice_scroll_limit());
      }
      return;
    case Drawn::Wifi:
      if (fresh || wifi_.version() != rendered_wifi_) {
        rendered_wifi_ = wifi_.version();
        ui_->render(wifi_.view());
      }
      return;
    case Drawn::Room:
      if (fresh || controller_.version() != rendered_version_) {
        rendered_version_ = controller_.version();
        ui_->render(controller_.view());
        controller_.set_info_scroll_limit(ui_->info_scroll_limit());
      }
      return;
  }
}

void App::update_wifi() {
  const Millis now = hal_.now();
  wifi_backend_->tick(now);
  if (wifi_backend_->version() != wifi_seen_) {
    wifi_seen_ = wifi_backend_->version();
    wifi_.set_networks(wifi_backend_->networks(), wifi_backend_->listed());
    wifi_.set_state(wifi_backend_->state());
  }
  // The list kept fresh while it's open.
  if (wifi_.listing() && !wifi_backend_->scanning() && now - last_wifi_scan_ >= kWifiRescan) {
    last_wifi_scan_ = now;
    wifi_backend_->scan(now);
  }
  if (auto message = wifi_.take_message()) ui_->show_toast(*message);
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
          // A turn that wakes the display only wakes it, without a click.
          if (!display_power_.input(input.at)) return;
          // A click only when the turn changes what the screen shows, so
          // it's felt as much as heard: none at either end of a list or a
          // value.
          const std::string before = ring_view();
          if (!notifications_.active() && !wifi_.active()) {
            notice_ticks_ = 0;
            controller_.rotate(event.ticks, input.at);
          } else {
            // A notification's choices, or Wi-Fi's lists and keys, a click
            // at a time.
            if ((notice_ticks_ > 0 && event.ticks < 0) || (notice_ticks_ < 0 && event.ticks > 0)) {
              notice_ticks_ = 0;
            }
            notice_ticks_ += event.ticks;
            const int clicks = notice_ticks_ / kTicksPerClick;
            notice_ticks_ -= clicks * kTicksPerClick;
            if (notifications_.active()) {
              notifications_.rotate(clicks);
            } else {
              wifi_.rotate(clicks);
            }
          }
          if (display_power_.config().ring_click && ring_view() != before) click(input.at);
        } else if constexpr (std::is_same_v<T, ButtonEdge>) {
          // Just started, with --hold-for-stock: a press is only a hold
          // that may mean the stock software.
          if (input.at < hold_window_until_ || held_since_ != 0) {
            held_since_ = event.down && input.at < hold_window_until_ ? input.at : 0;
            return;
          }
          // While active, a press is activity. While resting, it's the
          // gesture that decides: a single press or a hold wakes the display,
          // a double press doesn't (PROTOCOL.md section 13).
          if (display_power_.state() == room::DisplayState::Active) {
            display_power_.input(input.at);
            if (event.down && display_power_.config().press_click) click(input.at);
          }
          handle(event.down ? gestures_.on_down(input.at, press_context()) : gestures_.on_up(input.at));
        } else if constexpr (std::is_same_v<T, Presence>) {
          const bool resting = display_power_.state() != room::DisplayState::Active;
          sensors_.presence(event.far, input.at);
          display_power_.motion(input.at);
          if (resting && display_power_.state() == room::DisplayState::Active) {
            log::info("display woken by the %s sensor", event.far ? "far-field motion" : "proximity");
          }
        } else if constexpr (std::is_same_v<T, Light>) {
          auto_brightness_.reading(event.raw, input.at);
        } else if constexpr (std::is_same_v<T, QuitRequest>) {
          quit_ = true;
          exit_code_ = event.exit_code;
        }
      },
      input.event);
}

void App::handle(const std::vector<GestureEvent>& gestures) {
  for (const GestureEvent& g : gestures) {
    log::info("gesture %s (%s)", name(g.gesture), name(g.context));
    // For automations (PROTOCOL.md section 17), whatever it does here.
    send_event({{"t", "input"}, {"gesture", name(g.gesture)}, {"context", name(g.context)}});
    if (g.context != PressContext::Ui) {
      // Pressed while resting: it only wakes the display, and a double press
      // not even that.
      if (g.gesture != Gesture::Double) display_power_.input(hal_.now());
      continue;
    }
    notice_ticks_ = 0;
    if (notifications_.active()) {
      if (g.gesture == Gesture::Single) notifications_.press();
      if (g.gesture == Gesture::Long) notifications_.long_press();
      continue;
    }
    if (wifi_.active()) {
      if (g.gesture == Gesture::Single) wifi_.press();
      if (g.gesture == Gesture::Long) wifi_.long_press();
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

void App::open_info() {
  gather_info(true);
  ping_pending_ = false;
  ping(hal_.now());
}

void App::ping(Millis now) {
  protocol::PanelSession* session = server_ ? server_->active() : nullptr;
  if (!session || !session->ready()) return;
  // Home Assistant answers pings too (PROTOCOL.md section 18).
  ping_sent_ = now;
  ping_pending_ = true;
  session->send(protocol::Json{{"t", "ping"}, {"seq", ++ping_seq_}});
}

void App::update_info() {
  if (!controller_.info_open()) return;
  const Millis now = hal_.now();
  if (ping_pending_ && now - ping_sent_ >= kPingTimeout) ping_pending_ = false;
  if (!ping_pending_ && now - ping_sent_ >= kInfoInterval) ping(now);
  if (now - info_gathered_ >= kInfoInterval) gather_info(now - network_gathered_ >= kNetworkInterval);
}

void App::gather_info(bool network) {
  const Millis now = hal_.now();
  info_gathered_ = now;
  if (network) {
    services_ = net::connman_services();
    network_gathered_ = now;
  }
  const protocol::PanelSession* session = server_ ? server_->active() : nullptr;
  const bool connected = session && session->ready();

  room::TechInfoInput in;
  in.version = NESTO_VERSION;
  in.panel_id = info_.panel_id;
  in.port = options_.port;
  in.paired = pairing_->paired();
  in.uptime_ms = now;
  in.free_ram_kb = free_ram_kb();
  in.ha_status = status_;
  in.ha_connected = connected;
  in.ha_version = ha_version_;
  in.integration_version = integration_version_;
  in.round_trip_ms = round_trip_;

  room::TechInfoInput::Network& n = in.network;
  const net::WifiLink link = net::read_wifi_link(kWifiInterface);
  n.ssid = link.ssid;
  n.rssi_dbm = link.rssi_dbm;
  n.freq_mhz = link.freq_mhz;
  if (const auto s = net::current_service(services_)) {
    // connman knows it all, Wi-Fi or Ethernet.
    if (n.ssid.empty() && s->type == "wifi") n.ssid = s->name;
    n.security = s->type == "wifi" ? net::security_name(s->security) : "";
    if (s->type == "wifi") n.strength = s->strength;
    n.interface = s->interface;
    n.ipv4 = s->ipv4_address;
    n.netmask = s->ipv4_netmask;
    n.ipv4_router = s->ipv4_gateway;
    if (!s->ipv6_address.empty()) n.ipv6 = {s->ipv6_address + "/" + std::to_string(s->ipv6_prefix)};
    n.ipv6_router = s->ipv6_gateway;
    n.nameservers = s->nameservers;
    n.domains = s->domains;
    n.mac = s->mac;
  } else if (const auto ip = net::primary_ipv4_info()) {
    // No connman, as in the simulator: what the kernel says.
    n.interface = ip->interface;
    n.ipv4 = ip->address;
    n.netmask = ip->netmask;
    n.ipv4_router = net::default_ipv4_router();
    n.mac = net::mac_address(ip->interface);
  }

  in.has_base = hal_.has_base();
  in.base = hal_.base();
  const DeviceIdentity identity = hal_.identity();
  in.model = identity.model;
  in.hardware = identity.hardware;
  in.serial = identity.serial;
  if (wall_clock_.has_offset()) in.utc_offset_min = wall_clock_.offset_min();
  controller_.set_info(room::tech_info(in));
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
  // What the last Home Assistant said no longer holds.
  ha_version_.clear();
  integration_version_.clear();
  round_trip_.reset();
  ping_pending_ = false;
  update_status();
}

void App::on_ready(protocol::PanelSession& /*session*/) {
  log::info("synchronised with Home Assistant");
  update_status();
  // Where the display is, for Home Assistant to start from (section 13); a
  // change from the synchronisation's config is part of it.
  apply_display(false);
  send_display_state(display_power_.state(), backlight_.target(), "sync");
  sensors_.start();  // the readings, at once
  send_sensors();
}

void App::notify(protocol::PanelSession& session, const protocol::Message& m) {
  room::Notification n;
  std::string error;
  if (!room::Notification::parse(m.body, n, error)) {
    session.send(protocol::result_error(*m.id, protocol::ErrorCode::BadRequest, error));
    return;
  }
  session.send(protocol::result_ok(*m.id));
  log::info("notification %s (%s)", n.nid.c_str(),
            n.priority == room::Priority::Low      ? "low"
            : n.priority == room::Priority::Normal ? "normal"
            : n.priority == room::Priority::High   ? "high"
                                                   : "critical");
  // Waking first (PROTOCOL.md section 14), so it can pop up.
  if (n.priority != room::Priority::Low) {
    display_power_.notification(n.priority != room::Priority::Normal, hal_.now());
    apply_display();
  }
  // Its beep, unless notifications are silenced; a critical one always.
  const bool critical = n.priority == room::Priority::Critical;
  if (n.beep != room::Notification::Beep::None && (display_power_.config().notification_sound || critical)) {
    sound_.play(n.beep == room::Notification::Beep::Alert ? room::sounds::alert() : room::sounds::short_beep(),
                hal_.now());
    if (critical) last_alert_ = hal_.now();
  }
  notifications_.add(std::move(n), display_power_.state() == room::DisplayState::Active, hal_.now());
}

void App::send_event(const protocol::Json& event) {
  protocol::PanelSession* session = server_ ? server_->active() : nullptr;
  if (session && session->ready()) session->send(event);
}

void App::send_sensors() {
  if (!hal_.has_base()) return;
  protocol::PanelSession* session = server_ ? server_->active() : nullptr;
  if (!session || !session->ready() || !sensors_.due(hal_.now())) return;
  session->send(sensors_.message(hal_.base(), hal_.now()));
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
    const net::WifiLink link = net::read_wifi_link(kWifiInterface);
    result["wifi_rssi"] = link.rssi_dbm ? protocol::Json(*link.rssi_dbm) : protocol::Json(nullptr);
    result["wifi_ssid"] = link.ssid.empty() ? protocol::Json(nullptr) : protocol::Json(link.ssid);
    result["wifi_freq_mhz"] = link.freq_mhz ? protocol::Json(*link.freq_mhz) : protocol::Json(nullptr);
    result["ip"] = net::primary_ipv4();
    result["fw"] = NESTO_VERSION;
    const BaseReadings base = hal_.base();
    result["base_connected"] = base.connected;
    result["base_fw"] = base.firmware.empty() ? protocol::Json(nullptr) : protocol::Json(base.firmware);
    session.send(result);
    return true;
  }
  if (m.type == "notify" && m.is_request()) {
    notify(session, m);
    return true;
  }
  if (m.type == "notify_clear") {
    if (m.body.value("all", false)) {
      notifications_.clear_all();
    } else if (const auto it = m.body.find("nid"); it != m.body.end() && it->is_string()) {
      notifications_.clear(it->get<std::string>());
    }
    return true;
  }
  if (m.type == "identify" && m.is_request()) {
    // Lit, pulsing and beeping for a few seconds, to find which panel it is.
    display_power_.set(room::DisplayState::Active, hal_.now());
    apply_display();
    ui_->show_toast("Hello from Home Assistant");
    identify_until_ = hal_.now() + kIdentifyTime;
    sound_.play(room::sounds::identify(), hal_.now());
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
  if (m.type == "ha_info") {
    // For Technical info (PROTOCOL.md section 18).
    const auto text = [&](const char* key) {
      const auto it = m.body.find(key);
      return it != m.body.end() && it->is_string() ? it->get<std::string>().substr(0, kMaxVersion) : "";
    };
    ha_version_ = text("ha_version");
    integration_version_ = text("integration_version");
    log::info("Home Assistant %s, ha-nesto %s", ha_version_.c_str(), integration_version_.c_str());
    return true;
  }
  if (m.type == "pong") {
    // The answer to Technical info's ping.
    if (ping_pending_ && m.body.value("seq", 0u) == ping_seq_) {
      round_trip_ = hal_.now() - ping_sent_;
      ping_pending_ = false;
      if (controller_.info_open()) gather_info(false);
    }
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
  // Woken from rest: the top of the home screen, drawn before the backlight
  // rises.
  const bool active = change->state == room::DisplayState::Active;
  if (active && !display_active_) {
    controller_.go_home();
    // Wi-Fi's screens too, unless a network is being joined.
    if (!wifi_.joining()) wifi_.close();
    render();
  }
  display_active_ = active;
  // Lit: the screen first, then the backlight, at once. Dark: the backlight
  // fades, and update_backlight() darkens the screen at the end, as a dark
  // standby is as good as off for the screen itself.
  const int brightness = shown_brightness(change->brightness);
  backlight_.go(brightness, hal_.now());
  // What the new state shows is drawn before the backlight rises.
  update_clock();
  if (brightness > 0) hal_.set_screen_on(true);
  if (backlight_.fading(hal_.now())) fade_stats_ = {true, hal_.now(), hal_.now(), 0, 0, 0};
  update_backlight();
  if (brightness != change->brightness) {
    log::info("display %s at %d%%, %d%% for the room's light (%s)", room::name(change->state),
              change->brightness, brightness, room::name(change->cause));
  } else {
    log::info("display %s at %d%% (%s)", room::name(change->state), brightness, room::name(change->cause));
  }
  if (report) send_display_state(change->state, brightness, room::name(change->cause));
  // A notification waiting for an answer comes back on waking; a pop-up on a
  // timer doesn't outlast the display.
  if (change->state == room::DisplayState::Active) {
    notifications_.display_woke(hal_.now());
  } else {
    notifications_.display_rested();
  }
}

int App::shown_brightness(int brightness) const {
  if (!display_power_.config().auto_brightness) return brightness;
  return auto_brightness_.scale(brightness, hal_.now());
}

void App::update_auto_brightness() {
  const Millis now = hal_.now();
  if (backlight_.fading(now)) return;  // a change of state goes first
  const int configured = display_power_.brightness();
  const int wanted = shown_brightness(configured);
  const int target = backlight_.target();
  if (wanted == target) return;
  // Back to the configured brightness at once (auto brightness turned off,
  // the sensor gone, a lit room); a darker or lighter room waits for a
  // change worth seeing.
  if (wanted != configured && std::abs(wanted - target) < kAutoStep) return;
  backlight_.glide(wanted, now);
  log::info("auto brightness: %d%% of %d%% (light %d)", wanted, configured, auto_brightness_.raw());
}

void App::update_backlight() {
  const Millis t = hal_.now();
  int level = backlight_.level(t);
  // Identifying: between full and a third, four times a second.
  if (t < identify_until_ && level > 0 && (t / 250) % 2 == 1) level = std::max(1, level / 3);
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
