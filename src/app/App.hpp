#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/Fonts.hpp"
#include "core/PressGestures.hpp"
#include "hal/Hal.hpp"
#include "lvgl.h"
#include "protocol/PanelSession.hpp"
#include "room/AutoBrightness.hpp"
#include "room/Controller.hpp"
#include "room/DisplayPower.hpp"
#include "room/Notifications.hpp"
#include "room/Sensors.hpp"
#include "room/StandbyClock.hpp"

namespace nesto {

namespace net {
class MdnsResponder;
class Server;
}

class Ui;

struct AppOptions {
  std::uint16_t port = 6170;  // PROTOCOL.md section 2
  std::string state_dir;      // where the pairing key is kept
  std::string panel_id;       // empty: the network interface's MAC address
  bool mdns = true;           // announce the panel for Home Assistant's discovery
};

/// Ties the hardware, LVGL, the screens and the connection to Home Assistant
/// together, and runs the main loop.
class App : private protocol::SessionEvents {
 public:
  App(Hal& hal, AppOptions options);
  ~App() override;

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  /// Starts the hardware, LVGL and the server. Returns false if any can't start.
  bool init();

  /// Runs until asked to quit. Returns the process exit code.
  int run();

 private:
  static void flush(lv_display_t* display, const lv_area_t* area, std::uint8_t* pixels);
  static std::uint32_t tick();

  void handle(const TimedInput& input);
  void handle(const std::vector<GestureEvent>& gestures);
  PressContext press_context() const;
  /// Applies the display's state changes to the hardware and, with `report`,
  /// tells Home Assistant.
  void apply_display(bool report = true);
  /// Moves the backlight along its fade, and darkens the screen once it's dark.
  void update_backlight();
  /// `brightness` as shown: scaled by the room's light with auto_brightness.
  int shown_brightness(int brightness) const;
  /// Glides the backlight to follow the room's light, between changes of state.
  void update_auto_brightness();
  /// Shows the standby clock once standby is lit and the fade is over, moves
  /// it on, and hides it otherwise.
  void update_clock();
  /// Home Assistant's `time` message (PROTOCOL.md section 16).
  void set_time(const protocol::Json& body);
  void load_time();
  void store_time() const;
  void send_display_state(room::DisplayState state, int brightness, const char* cause);
  /// Draws what changed: a notification screen while one is open, else the
  /// room's.
  void render();
  /// Home Assistant's `notify` request (PROTOCOL.md section 14).
  void notify(protocol::PanelSession& session, const protocol::Message& m);
  /// Sends an event to Home Assistant, if it's connected and synchronised.
  void send_event(const protocol::Json& event);
  /// Sends the base's readings when they're due (PROTOCOL.md section 17).
  void send_sensors();
  void load_display_config();
  void store_display_config() const;
  void update_status();
  void send_intent(const room::Intent& intent);
  void open_pairing();
  void forget_home_assistant();
  std::vector<std::string> about() const;

  // protocol::SessionEvents
  void on_pairing_code(const std::string& code) override;
  void on_pairing_finished(bool paired) override;
  void on_authenticated(protocol::PanelSession& session) override;
  void on_ready(protocol::PanelSession& session) override;
  void on_unpaired() override;
  bool on_message(protocol::PanelSession& session, const protocol::Message& message) override;

  Hal& hal_;
  AppOptions options_;
  std::vector<std::uint32_t> framebuffer_;
  lv_display_t* display_ = nullptr;
  PressGestures gestures_;
  Fonts fonts_;
  std::unique_ptr<Ui> ui_;

  protocol::PanelInfo info_;
  std::unique_ptr<protocol::PairingState> pairing_;
  std::unique_ptr<net::Server> server_;
  std::unique_ptr<net::MdnsResponder> mdns_;
  std::string status_;
  Millis status_checked_ = 0;

  room::Controller controller_;
  room::Notifications notifications_;
  int notice_ticks_ = 0;  // ring ticks towards a click, on a notification screen
  unsigned rendered_notices_ = 0;  // notifications_.version() last drawn
  bool notices_drawn_ = false;     // whether the screen shows a notification screen
  room::DisplayPower display_power_{0};
  room::BacklightFade backlight_;
  room::AutoBrightness auto_brightness_;
  room::Sensors sensors_;
  int backlight_level_ = -1;  // what the hardware was last set to
  // How the last fade went, logged at its end: steps written, the longest
  // gap between them and the longest write.
  struct FadeStats {
    bool active = false;
    Millis started = 0;
    Millis last_step = 0;
    int steps = 0;
    Millis longest_gap = 0;
    Millis longest_write = 0;
  } fade_stats_;
  room::WallClock wall_clock_;
  std::string clock_drawn_;  // what the clock shows, to redraw only on a change
  unsigned rendered_version_ = 0;
  std::map<std::uint32_t, std::string> intent_requests_;  // request id to item key
  std::uint32_t next_request_id_ = 1;

  bool quit_ = false;
  int exit_code_ = 0;
};

}  // namespace nesto
