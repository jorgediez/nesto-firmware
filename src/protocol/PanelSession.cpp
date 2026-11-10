#include "protocol/PanelSession.hpp"

#include "core/Log.hpp"

namespace nesto::protocol {
namespace {

constexpr int kProtocolVersion = 1;

/// Time since `since`, never negative: callers may read the clock just before
/// an event that's stamped a moment later.
Millis elapsed(Millis now, Millis since) {
  return now > since ? now - since : 0;
}

std::optional<std::string> string_field(const Json& body, const char* name) {
  const auto it = body.find(name);
  if (it == body.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> binary_field(const Json& body, const char* name) {
  const auto text = string_field(body, name);
  if (!text) return std::nullopt;
  return base64_decode_fixed<N>(*text);
}

}  // namespace

PanelSession::PanelSession(const PanelInfo& info, PairingState& pairing, SessionEvents& events,
                           Millis now)
    : info_(info), pairing_(pairing), events_(events), started_(now), last_received_(now) {
  if (!random_bytes(panel_nonce_.data(), panel_nonce_.size())) {
    close("can't read the random source");
    return;
  }
  send_plain(Json{{"t", "hello"},
                  {"proto", kProtocolVersion},
                  {"panel_id", info_.panel_id},
                  {"fw", info_.firmware},
                  {"model", info_.model},
                  {"paired", pairing_.paired()},
                  {"pairing_open", pairing_.pairing_open(now)},
                  {"nonce", base64_encode(panel_nonce_)},
                  {"caps", info_.caps}});
}

PanelSession::~PanelSession() {
  finish_pairing(false);
}

void PanelSession::receive(const std::uint8_t* data, std::size_t size, Millis now) {
  if (closing_) return;
  reader_.feed(data, size);
  if (reader_.failed()) {
    close(reader_.error());
    return;
  }
  while (!closing_) {
    auto payload = reader_.next();
    if (!payload) break;
    last_received_ = now;
    if (state_ == State::Authenticated) {
      handle_encrypted(*payload, now);
    } else {
      handle_plain(*payload, now);
    }
  }
}

void PanelSession::tick(Millis now) {
  if (closing_) return;
  switch (state_) {
    case State::Handshake:
      if (elapsed(now, started_) > kHandshakeTimeout) close("no pairing or authentication in time");
      break;
    case State::Pairing:
      if (elapsed(now, started_) > kPairingTimeout) close("the pairing code wasn't confirmed in time");
      break;
    case State::Authenticated:
      if (elapsed(now, last_received_) > kIdleTimeout) close("nothing received for a minute");
      break;
    case State::Done:
      break;
  }
}

void PanelSession::send(const Json& message) {
  if (state_ != State::Authenticated || closing_) return;
  auto frame = to_ha_->seal(serialize(message));
  if (!frame) {
    log::error("message '%s' is too large to send", message.value("t", "?").c_str());
    return;
  }
  outgoing_.push_back(std::move(*frame));
}

std::vector<Bytes> PanelSession::take_outgoing() {
  std::vector<Bytes> out;
  out.swap(outgoing_);
  return out;
}

void PanelSession::close(const std::string& reason) {
  if (closing_) return;
  closing_ = true;
  close_reason_ = reason;
  state_ = State::Done;
  finish_pairing(false);
}

void PanelSession::handle_plain(const Bytes& payload, Millis now) {
  std::string error;
  const auto m = parse_message(payload, error);
  if (!m) {
    close("bad message: " + error);
    return;
  }

  const bool handshake = state_ == State::Handshake;
  const bool pairing = state_ == State::Pairing;
  if (m->type == "ping") {
    send_plain(Json{{"t", "pong"}, {"seq", m->body.value("seq", 0)}});
  } else if (m->type == "pair_begin" && handshake && m->is_request()) {
    handle_pair_begin(*m, now);
  } else if (m->type == "pair_confirm" && pairing && m->is_request()) {
    handle_pair_confirm(*m);
  } else if (m->type == "pair_cancel" && pairing) {
    close("Home Assistant cancelled the pairing");
  } else if (m->type == "auth" && handshake && m->is_request()) {
    handle_auth(*m, now);
  } else if (m->is_request()) {
    reply_error(*m, ErrorCode::NotReady, "pair or authenticate first");
  }
  // Other events before authentication are ignored.
}

void PanelSession::handle_pair_begin(const Message& m, Millis now) {
  if (!pairing_.pairing_open(now)) {
    reply_error(m, ErrorCode::PairingClosed, "open pairing on the panel first");
    close("pairing isn't open");
    return;
  }
  if (!pairing_.begin_pairing()) {
    reply_error(m, ErrorCode::PairingClosed, "another pairing is in progress");
    close("another pairing is in progress");
    return;
  }
  holds_pairing_ = true;

  const auto ha_public = binary_field<32>(m.body, "pub");
  if (!ha_public) {
    reply_error(m, ErrorCode::BadRequest, "`pub` must be a base64 X25519 public key");
    close("bad pair_begin");
    return;
  }
  auto keys = generate_key_pair();
  if (!keys) {
    reply_error(m, ErrorCode::Internal, "can't read the random source");
    close("can't read the random source");
    return;
  }
  auto shared = shared_secret(keys->secret, *ha_public);
  wipe(keys->secret.data(), keys->secret.size());
  if (!shared) {
    reply_error(m, ErrorCode::BadRequest, "low-order public key");
    close("low-order public key");
    return;
  }

  pending_key_ = pairing_key(*shared, *ha_public, keys->public_key);
  const std::string code = pairing_code(*ha_public, keys->public_key, *shared);
  wipe(shared->data(), shared->size());

  state_ = State::Pairing;
  started_ = now;
  events_.on_pairing_code(code);
  Json result = result_ok(*m.id);
  result["pub"] = base64_encode(keys->public_key);
  send_plain(result);
}

void PanelSession::handle_pair_confirm(const Message& m) {
  const auto mac = binary_field<64>(m.body, "mac");
  if (!mac || !macs_equal(*mac, confirm_mac(*pending_key_, Role::Ha))) {
    reply_error(m, ErrorCode::AuthFailed, "wrong confirmation");
    close("pairing confirmation failed");
    return;
  }
  if (!pairing_.save_key(*pending_key_)) {
    reply_error(m, ErrorCode::Internal, "can't save the pairing key");
    close("can't save the pairing key");
    return;
  }
  Json result = result_ok(*m.id);
  result["mac"] = base64_encode(confirm_mac(*pending_key_, Role::Panel));
  send_plain(result);
  log::info("paired with Home Assistant");
  finish_pairing(true);
  // Section 6.2: the panel closes, and HA reconnects to authenticate.
  close("paired");
}

void PanelSession::handle_auth(const Message& m, Millis now) {
  if (pairing_.auth_locked(now)) {
    reply_error(m, ErrorCode::AuthFailed, "too many failed attempts, try again later");
    close("authentication is locked out");
    return;
  }
  if (!pairing_.paired()) {
    reply_error(m, ErrorCode::AuthFailed, "not paired");
    close("authentication while not paired");
    return;
  }
  const auto ha_nonce = binary_field<16>(m.body, "nonce");
  const auto mac = binary_field<64>(m.body, "mac");
  if (!ha_nonce || !mac) {
    reply_error(m, ErrorCode::BadRequest, "`nonce` and `mac` must be base64");
    close("bad auth");
    return;
  }
  const Key& key = *pairing_.key();
  if (!macs_equal(*mac, auth_mac(key, Role::Ha, panel_nonce_, *ha_nonce, info_.panel_id))) {
    pairing_.auth_failed(now);
    reply_error(m, ErrorCode::AuthFailed, "wrong MAC");
    close("authentication failed");
    return;
  }
  pairing_.auth_succeeded();

  Json result = result_ok(*m.id);
  result["mac"] =
      base64_encode(auth_mac(key, Role::Panel, panel_nonce_, *ha_nonce, info_.panel_id));
  send_plain(result);

  // Everything after this result is encrypted (section 7.3).
  SessionKeys keys = session_keys(key, panel_nonce_, *ha_nonce);
  to_ha_ = std::make_unique<FrameCipher>(keys.panel_to_ha);
  from_ha_ = std::make_unique<FrameCipher>(keys.ha_to_panel);
  wipe(&keys, sizeof keys);
  state_ = State::Authenticated;
  last_received_ = now;
  events_.on_authenticated(*this);
}

void PanelSession::handle_encrypted(const Bytes& payload, Millis /*now*/) {
  const auto plaintext = from_ha_->open(payload);
  if (!plaintext) {
    close("a frame failed to decrypt");
    return;
  }
  std::string error;
  const auto m = parse_message(*plaintext, error);
  if (!m) {
    log::warn("ignoring a bad message: %s", error.c_str());
    return;
  }

  if (m->type == "ping") {
    send(Json{{"t", "pong"}, {"seq", m->body.value("seq", 0)}});
    return;
  }
  if (m->type == "unpair" && m->is_request()) {
    pairing_.forget();
    send(result_ok(*m->id));
    log::info("Home Assistant removed the pairing");
    events_.on_unpaired();
    close("unpaired");
    return;
  }

  // Before `ready`, only maintenance requests are answered (section 18).
  const bool maintenance = m->type == "info" || m->type == "identify" || m->type == "reboot";
  if (!ready_ && m->is_request() && !maintenance) {
    reply_error(*m, ErrorCode::NotReady, "synchronisation hasn't finished");
    return;
  }

  track_sync(*m);
  handle_app(*m);

  if (!ready_ && got_time_ && got_config_ && got_layout_ && got_states_) {
    ready_ = true;
    send(Json{{"t", "ready"}});
    events_.on_ready(*this);
  }
}

void PanelSession::handle_app(const Message& m) {
  const bool handled = events_.on_message(*this, m);
  if (!handled && m.is_request()) {
    reply_error(m, ErrorCode::UnknownType, "unknown request '" + m.type + "'");
  }
}

void PanelSession::track_sync(const Message& m) {
  if (m.type == "time") got_time_ = true;
  if (m.type == "config") got_config_ = true;
  if (m.type == "layout") got_layout_ = true;
  if (m.type == "states" && m.body.value("full", false)) got_states_ = true;
}

void PanelSession::send_plain(const Json& message) {
  outgoing_.push_back(encode_frame(serialize(message)));
}

void PanelSession::reply_error(const Message& m, ErrorCode code, const std::string& text) {
  if (!m.id) return;
  const Json reply = result_error(*m.id, code, text);
  if (state_ == State::Authenticated) {
    send(reply);
  } else {
    send_plain(reply);
  }
}

void PanelSession::finish_pairing(bool paired) {
  if (pending_key_) {
    wipe(pending_key_->data(), pending_key_->size());
    pending_key_.reset();
  }
  if (!holds_pairing_) return;
  holds_pairing_ = false;
  pairing_.end_pairing();
  events_.on_pairing_finished(paired);
}

}  // namespace nesto::protocol
