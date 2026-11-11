#pragma once

#include <optional>
#include <string>

#include "core/Input.hpp"
#include "protocol/Message.hpp"

namespace nesto::room {

/// The display's three states (PROTOCOL.md section 13).
enum class DisplayState { Active, Standby, Off };

/// Why the state changed, as display_state reports it.
enum class DisplayCause { Input, Motion, Timeout, Ha, Notification };

enum class StandbyMode { Clock, Carousel, Off };

/// The display settings of a `config` message (PROTOCOL.md section 12). The
/// defaults are the protocol's example, used until Home Assistant sends its
/// own.
struct DisplayConfig {
  int brightness = 80;          // active, 0 to 100
  bool auto_brightness = true;  // not used yet: needs the light sensor
  unsigned timeout_s = 30;      // 0: never leave active
  bool wake_on_motion = true;
  bool wake_on_input_when_off = true;
  StandbyMode standby_mode = StandbyMode::Clock;
  int standby_brightness = 20;

  /// From a whole `config` message; anything missing or out of range takes
  /// the default. Unknown fields are ignored.
  static DisplayConfig from_config(const protocol::Json& config);
  /// The display and standby parts of a `config` message, to store.
  protocol::Json to_config() const;

  bool operator==(const DisplayConfig& other) const;
};

const char* name(DisplayState state);
const char* name(DisplayCause cause);
std::optional<DisplayState> parse_display_state(const std::string& text);

/// When the screen is lit and how brightly:
///
/// - Active shows the controls at `brightness`. After `timeout_s` without
///   input (or motion, with wake_on_motion) it goes to standby, or to off if
///   the standby mode is "off".
/// - Standby shows the clock or carousel at `standby_brightness`; until
///   there's a standby screen, it's dark (brightness 0). Input, or motion
///   with wake_on_motion, makes it active.
/// - Off has the backlight off. Input makes it active only with
///   wake_on_input_when_off; motion doesn't.
///
/// Input that wakes the display only wakes it: input() says whether it goes
/// on to the controls. Home Assistant can set any state; active restarts the
/// timeout. Each change is kept for take_change(), to apply and report.
class DisplayPower {
 public:
  struct Change {
    DisplayState state;
    int brightness;
    DisplayCause cause;
  };

  /// Starts active. `has_standby_screen`: false until there is a clock to
  /// show, and standby is then dark.
  DisplayPower(Millis now, bool has_standby_screen = false);

  void configure(const DisplayConfig& config, Millis now);
  const DisplayConfig& config() const { return config_; }

  /// The ring turned or the display was pressed. Returns true if the display
  /// was active, so the input is meant for the controls.
  bool input(Millis now);

  /// Someone is near (the base's proximity sensor).
  void motion(Millis now);

  /// Home Assistant's `display` request.
  void set(DisplayState state, Millis now);

  /// Leaves active once the timeout passes.
  void tick(Millis now);

  DisplayState state() const { return state_; }
  int brightness() const;

  /// The last change not taken yet, if any.
  std::optional<Change> take_change();

 private:
  /// Where the timeout leads: standby, or off if the standby mode is "off".
  DisplayState resting_state() const;
  void enter(DisplayState state, DisplayCause cause, Millis now);

  DisplayConfig config_;
  bool has_standby_screen_;
  DisplayState state_ = DisplayState::Active;
  Millis last_activity_;
  std::optional<Change> change_;
};

/// The backlight's way to a new brightness. Going down it fades, so resting
/// looks calm, and the screen turns black only once it's dark. Going up it's
/// at once, so waking is quick. The fade follows a curve, faster at first: the eye sees a straight
/// line in brightness as staying bright and then dropping at the end.
class BacklightFade {
 public:
  static constexpr Millis kDuration = 800;

  explicit BacklightFade(int level = 0) : from_(level), to_(level) {}

  /// Heads for `target` from where the backlight is at `now`.
  void go(int target, Millis now);

  /// The backlight's level at `now`.
  int level(Millis now) const;
  int target() const { return to_; }
  /// Still on the way down at `now`.
  bool fading(Millis now) const { return level(now) != to_; }

 private:
  int from_;
  int to_;
  Millis start_ = 0;
};

}  // namespace nesto::room
