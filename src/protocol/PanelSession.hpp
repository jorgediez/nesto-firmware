#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Input.hpp"
#include "protocol/Crypto.hpp"
#include "protocol/Frame.hpp"
#include "protocol/Message.hpp"
#include "protocol/PairingState.hpp"

namespace nesto::protocol {

/// What the panel says about itself in `hello` (PROTOCOL.md section 5).
struct PanelInfo {
  std::string panel_id;  // Wi-Fi MAC address, lower-case hex
  std::string firmware;
  std::string model;
  Json caps = Json::object();
};

class PanelSession;

/// The panel's side of the application: what a session reports and what it
/// can't handle on its own.
class SessionEvents {
 public:
  virtual ~SessionEvents() = default;

  /// Show this pairing code on screen (section 6.2).
  virtual void on_pairing_code(const std::string& code) = 0;
  /// Stop showing it: pairing finished, with or without a new key.
  virtual void on_pairing_finished(bool paired) = 0;
  /// A connection authenticated; any older authenticated one should close.
  virtual void on_authenticated(PanelSession& session) = 0;
  /// Synchronisation finished and the panel sent `ready` (section 8).
  virtual void on_ready(PanelSession& session) = 0;
  /// Home Assistant removed the panel (section 6.3).
  virtual void on_unpaired() = 0;
  /// A message for the application, after authentication. Return false if it
  /// isn't handled, so the session answers a request with `unknown_type`.
  /// Requests that are handled must be answered through session.send().
  virtual bool on_message(PanelSession& session, const Message& message) = 0;
};

/// One connection from Home Assistant, from `hello` to closing: pairing,
/// authentication, encryption and synchronisation (sections 5 to 8). It does
/// no input or output itself: the server feeds it received bytes and the
/// time, and sends the frames it queues.
class PanelSession {
 public:
  static constexpr Millis kHandshakeTimeout = 10 * 1000;
  static constexpr Millis kPairingTimeout = 120 * 1000;
  static constexpr Millis kIdleTimeout = 60 * 1000;

  /// Sends `hello` straight away. If the random source fails, the session
  /// starts closed.
  PanelSession(const PanelInfo& info, PairingState& pairing, SessionEvents& events, Millis now);
  ~PanelSession();

  PanelSession(const PanelSession&) = delete;
  PanelSession& operator=(const PanelSession&) = delete;

  void receive(const std::uint8_t* data, std::size_t size, Millis now);
  void tick(Millis now);

  /// Sends an application message. Only after authentication; ignored before.
  void send(const Json& message);

  /// Frames waiting to be written, in order. Taking them empties the queue.
  std::vector<Bytes> take_outgoing();

  /// Closes once everything queued has been written.
  void close(const std::string& reason);

  bool closing() const { return closing_; }
  const std::string& close_reason() const { return close_reason_; }
  bool authenticated() const { return state_ == State::Authenticated; }
  bool ready() const { return ready_; }

 private:
  enum class State { Handshake, Pairing, Authenticated, Done };

  void handle_plain(const Bytes& payload, Millis now);
  void handle_encrypted(const Bytes& payload, Millis now);
  void handle_pair_begin(const Message& m, Millis now);
  void handle_pair_confirm(const Message& m);
  void handle_auth(const Message& m, Millis now);
  void handle_app(const Message& m);
  void track_sync(const Message& m);

  void send_plain(const Json& message);
  void reply_error(const Message& m, ErrorCode code, const std::string& text);
  void finish_pairing(bool paired);

  const PanelInfo& info_;
  PairingState& pairing_;
  SessionEvents& events_;

  State state_ = State::Handshake;
  FrameReader reader_;
  std::vector<Bytes> outgoing_;
  bool closing_ = false;
  std::string close_reason_;

  Nonce panel_nonce_{};
  Millis started_ = 0;
  Millis last_received_ = 0;

  bool holds_pairing_ = false;  // this session owns PairingState's one pairing
  std::optional<Key> pending_key_;  // between pair_begin and pair_confirm

  std::unique_ptr<FrameCipher> to_ha_;
  std::unique_ptr<FrameCipher> from_ha_;

  bool got_time_ = false;
  bool got_config_ = false;
  bool got_layout_ = false;
  bool got_states_ = false;
  bool ready_ = false;
};

}  // namespace nesto::protocol
