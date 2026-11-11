#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/Input.hpp"

/// The thermostat's backplate: the base on the wall, with its own
/// microcontroller, reached over a serial port (/dev/ttyO2). It charges the
/// battery, reads the temperature, humidity, proximity and light sensors, and
/// resets the thermostat when nothing has talked to it for about 30 minutes.
/// docs/BACKPLATE.md describes the protocol and who worked it out.
///
/// Nothing here touches hardware: frames go in and out as bytes, and the
/// session is driven by the time it's given, so it's all unit-tested.
namespace nesto::backplate {

/// Message types. Commands go to the backplate, the rest come from it.
enum Type : std::uint16_t {
  kAscii = 0x0001,          // text: versions, "BRK" at the end of the start-up burst
  kClimate = 0x0002,        // temperature and humidity
  kFetPresence = 0x0004,    // which terminals have wires
  kPirRaw = 0x0005,         // the far-field motion sensor, now and then (meaning not known yet)
  kProximity = 0x0007,      // how near someone is, about every second
  kState = 0x000b,          // voltages: input, operating, battery
  kTfeVersion = 0x0018,
  kTfeBuildInfo = 0x0019,
  kBslId = 0x001d,          // two bytes, not text
  kHardware = 0x001e,       // the backplate's name, for instance "Backplate-3.2" (assumed)
  kSerial = 0x001f,         // (assumed)
  kEndOfBuffers = 0x002f,

  kStatusRequest = 0x0083,  // the keep-alive
  kFetPresenceAck = 0x008f,
  kGetTfeVersion = 0x0098,
  kGetTfeBuildInfo = 0x0099,
  kGetBslId = 0x009d,
  kGetHardware = 0x009e,
  kGetSerial = 0x009f,
  kRequestBuffers = 0x00a2,
  kEndOfBuffersAck = 0x00a3,
  kSetPowerSteal = 0x00c0,
  kReset = 0x00ff,
};

/// Payloads are at most this long; a longer length means the stream is out of
/// step.
constexpr std::size_t kMaxPayload = 1024;

/// CRC-16/XMODEM (polynomial 0x1021, starting at 0, not reflected).
std::uint16_t crc16(const std::uint8_t* data, std::size_t size, std::uint16_t crc = 0);

/// A command for the backplate: d5 aa 96, the type and the payload's length
/// (both 16-bit little-endian), the payload, and the CRC of all but the
/// preamble, little-endian.
std::vector<std::uint8_t> encode(std::uint16_t type, const std::vector<std::uint8_t>& payload = {});

struct Frame {
  std::uint16_t type = 0;
  std::vector<std::uint8_t> payload;
};

/// Finds the backplate's frames in the bytes it sends: the same layout as a
/// command, after a four-byte preamble, d5 d5 aa 96. Bytes that aren't a
/// frame, and frames whose CRC is wrong, are skipped.
class FrameReader {
 public:
  /// Adds bytes; appends the frames they complete to `out`.
  void feed(const std::uint8_t* data, std::size_t size, std::vector<Frame>& out);
  void clear() { buffer_.clear(); }

  std::size_t bad_frames() const { return bad_frames_; }

 private:
  std::vector<std::uint8_t> buffer_;
  std::size_t bad_frames_ = 0;
};

/// A kState payload, in volts.
struct Power {
  std::uint8_t flags = 0;  // byte 1, the stock software's "flags"; 0x40 isn't "charger off" (docs/BACKPLATE.md)
  double usb = 0;        // from a USB supply, as on a stand
  double input = 0;      // from the heating's wires
  double operating = 0;
  double battery = 0;
};
std::optional<Power> decode_power(const std::vector<std::uint8_t>& payload);

/// A kClimate payload.
struct Climate {
  double celsius = 0;
  double humidity = 0;  // percent
};
std::optional<Climate> decode_climate(const std::vector<std::uint8_t>& payload);

/// A kProximity payload: higher is nearer. The stock software logs it as
/// "near pir msg N, threshold 15", so 15 or more means someone is near.
std::optional<int> decode_proximity(const std::vector<std::uint8_t>& payload);
constexpr int kNearThreshold = 15;

/// A frame's payload as text, for kAscii and the version replies.
std::string text(const Frame& frame);

/// Keeps a session with the backplate going:
///
///  1. a break on the line, a quarter of a second's pause, and a reset;
///  2. the backplate's start-up burst, up to "BRK", which must include which
///     terminals have wires; that's acknowledged with the same payload;
///  3. its versions and identity, asked one at a time (the name and serial
///     number are optional: unanswered, they're skipped);
///  4. then, for as long as it runs, a keep-alive every 15 s and a request
///     for its buffered readings every 30 s, whose end is acknowledged.
///
/// If a step times out, or the backplate goes quiet, it starts again after a
/// pause.
class Session {
 public:
  struct Config {
    Millis break_pause = 250;
    Millis burst_timeout = 5000;
    Millis reply_timeout = 1000;
    Millis keepalive = 15000;
    Millis buffers = 30000;
    Millis silence = 120000;   // nothing heard for this long: start again
    Millis retry = 10000;      // pause before starting again
    bool power_steal = false;  // send "set power steal mode" before running
    Millis hand_back_timeout = 3000;
  };

  enum class Phase { kStopped, kBreak, kBurst, kInfo, kRunning, kRetry, kHandingBack };

  Session() = default;
  explicit Session(Config config) : config_(config) {}

  /// Starts (or restarts) the session.
  void start(Millis now);

  /// Leaves the backplate as the stock software does when it stops: a reset,
  /// the start-up burst, and its wire presence acknowledged. The stock
  /// software sometimes fails to start after a session left mid-way, and
  /// monit then reboots the thermostat. Done once handing_back() is false;
  /// gives up after hand_back_timeout.
  void hand_back(Millis now);
  bool handing_back() const { return phase_ == Phase::kHandingBack; }
  /// Whether the last hand_back() saw the burst through and acknowledged it.
  bool handed_back() const { return handed_back_; }

  /// Advances timers. Call often: every loop is fine.
  void tick(Millis now);

  /// Handles a frame from the backplate.
  void receive(Millis now, const Frame& frame);

  /// Whether the line wants a break now; true once per request. Input read
  /// before the break should be dropped.
  bool take_break();

  /// The bytes to send, in order; empties the queue.
  std::vector<std::uint8_t> take_output();

  Phase phase() const { return phase_; }
  bool running() const { return phase_ == Phase::kRunning; }

  /// What went wrong last, if the session is starting again.
  const std::string& problem() const { return problem_; }

  // What the backplate said about itself.
  const std::string& version() const { return version_; }
  const std::string& build_info() const { return build_info_; }
  const std::string& bsl_id() const { return bsl_id_; }  // in hex
  const std::string& model() const { return model_; }
  const std::string& serial() const { return serial_; }

  // Its latest readings, if any came yet.
  const std::optional<Power>& power() const { return power_; }
  const std::optional<Climate>& climate() const { return climate_; }

  int handshakes() const { return handshakes_; }
  int restarts() const { return restarts_; }

 private:
  void store(std::size_t question, const Frame& frame);
  void send(std::uint16_t type, const std::vector<std::uint8_t>& payload = {});
  void ask(Millis now);
  void enter_running(Millis now);
  void fail(Millis now, const std::string& problem);

  Config config_;
  Phase phase_ = Phase::kStopped;
  Millis since_ = 0;      // when the phase, or the current question, began
  Millis heard_ = 0;      // when the backplate last said anything
  Millis keepalive_ = 0;  // when the last keep-alive went
  Millis buffers_ = 0;    // when the last buffers request went
  bool break_wanted_ = false;
  std::vector<std::uint8_t> output_;
  std::optional<std::vector<std::uint8_t>> fet_presence_;
  std::size_t query_ = 0;
  std::string problem_;

  std::string version_;
  std::string build_info_;
  std::string bsl_id_;
  std::string model_;
  std::string serial_;
  std::optional<Power> power_;
  std::optional<Climate> climate_;
  int handshakes_ = 0;
  int restarts_ = 0;
  bool handed_back_ = false;
};

}  // namespace nesto::backplate
