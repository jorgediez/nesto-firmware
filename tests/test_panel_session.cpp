// Drives PanelSession the way Home Assistant would, with the same primitives,
// and checks every step of PROTOCOL.md sections 5 to 8 and 18.
#include <doctest/doctest.h>

#include <cstdlib>
#include <deque>
#include <set>
#include <string>
#include <unistd.h>

#include "protocol/PanelSession.hpp"

using namespace nesto;
using namespace nesto::protocol;

namespace {

const std::string kPanelId = "18b4300a1b2c";

struct Recorder : SessionEvents {
  std::vector<std::string> codes;
  std::vector<bool> pairing_results;
  int authenticated = 0;
  int ready = 0;
  int unpaired = 0;
  std::vector<std::string> messages;
  std::set<std::string> handled = {"time", "config", "layout", "states", "info"};

  void on_pairing_code(const std::string& code) override { codes.push_back(code); }
  void on_pairing_finished(bool paired) override { pairing_results.push_back(paired); }
  void on_authenticated(PanelSession&) override { ++authenticated; }
  void on_ready(PanelSession&) override { ++ready; }
  void on_unpaired() override { ++unpaired; }
  bool on_message(PanelSession& session, const Message& m) override {
    messages.push_back(m.type);
    if (m.type == "info" && m.is_request()) {
      Json r = result_ok(*m.id);
      r["uptime_s"] = 1;
      session.send(r);
    }
    return handled.count(m.type) > 0;
  }
};

/// Home Assistant's side of one connection.
class FakeHa {
 public:
  FakeHa(PairingState& pairing, Recorder& events, Millis now = 1000)
      : session(info(), pairing, events, now), now_(now) {
    hello = next();
  }

  static const PanelInfo& info() {
    static const PanelInfo i{kPanelId, "0.1.0", "nest-gen2", Json{{"input", {"ring", "press"}}}};
    return i;
  }

  void send_plain(const Json& m) { feed(encode_frame(serialize(m))); }

  void send(const Json& m) {
    auto frame = to_panel_->seal(serialize(m));
    REQUIRE(frame);
    feed(*frame);
  }

  void feed(const Bytes& bytes) { session.receive(bytes.data(), bytes.size(), now_); }

  /// The next message the panel sent; fails the test if there is none.
  Json next() {
    pull();
    REQUIRE_MESSAGE(!inbox_.empty(), "the panel sent nothing");
    Json m = inbox_.front();
    inbox_.pop_front();
    return m;
  }

  bool nothing_sent() {
    pull();
    return inbox_.empty();
  }

  void advance(Millis ms) {
    now_ += ms;
    session.tick(now_);
  }

  Millis now() const { return now_; }

  /// Pairs and returns the code the panel showed and the key both sides hold.
  std::pair<std::string, Key> pair() {
    const auto ha = generate_key_pair();
    REQUIRE(ha);
    send_plain(Json{{"t", "pair_begin"}, {"id", 1}, {"pub", base64_encode(ha->public_key)}});
    const Json begun = next();
    REQUIRE(begun["ok"] == true);
    const auto panel_pub = base64_decode_fixed<32>(begun["pub"].get<std::string>());
    REQUIRE(panel_pub);
    const auto shared = shared_secret(ha->secret, *panel_pub);
    REQUIRE(shared);
    const std::string code = pairing_code(ha->public_key, *panel_pub, *shared);
    const Key key = pairing_key(*shared, ha->public_key, *panel_pub);

    send_plain(Json{{"t", "pair_confirm"}, {"id", 2},
                    {"mac", base64_encode(confirm_mac(key, Role::Ha))}});
    const Json confirmed = next();
    REQUIRE(confirmed["ok"] == true);
    CHECK(confirmed["mac"] == base64_encode(confirm_mac(key, Role::Panel)));
    return {code, key};
  }

  /// Authenticates with `key`; returns the panel's answer.
  Json auth(const Key& key, bool correct = true) {
    Nonce ha_nonce{};
    REQUIRE(random_bytes(ha_nonce.data(), ha_nonce.size()));
    const auto panel_nonce = base64_decode_fixed<16>(hello["nonce"].get<std::string>());
    REQUIRE(panel_nonce);
    Mac mac = auth_mac(key, Role::Ha, *panel_nonce, ha_nonce, kPanelId);
    if (!correct) mac[0] ^= 1;
    send_plain(Json{{"t", "auth"}, {"id", 1}, {"nonce", base64_encode(ha_nonce)},
                    {"mac", base64_encode(mac)}});
    const Json answer = next();
    if (answer["ok"] == true) {
      CHECK(answer["mac"] ==
            base64_encode(auth_mac(key, Role::Panel, *panel_nonce, ha_nonce, kPanelId)));
      const SessionKeys keys = session_keys(key, *panel_nonce, ha_nonce);
      to_panel_ = std::make_unique<FrameCipher>(keys.ha_to_panel);
      from_panel_ = std::make_unique<FrameCipher>(keys.panel_to_ha);
    }
    return answer;
  }

  void sync() {
    send(Json{{"t", "time"}, {"utc_ms", 1789000000000}, {"utc_offset_min", 60}});
    send(Json{{"t", "config"}});
    send(Json{{"t", "layout"}, {"rev", 1}, {"title", "Room"}, {"items", Json::array()}});
    send(Json{{"t", "states"}, {"full", true}, {"items", Json::object()}});
  }

  PanelSession session;
  Json hello;

 private:
  void pull() {
    for (const Bytes& frame : session.take_outgoing()) reader_.feed(frame);
    while (auto payload = reader_.next()) {
      Bytes plain = *payload;
      if (from_panel_) {
        auto opened = from_panel_->open(*payload);
        // The auth result is the last plaintext frame; everything after is encrypted.
        if (opened) plain = *opened;
      }
      std::string error;
      auto m = parse_message(plain, error);
      REQUIRE_MESSAGE(m, error);
      inbox_.push_back(m->body);
    }
  }

  Millis now_;
  FrameReader reader_;
  std::deque<Json> inbox_;
  std::unique_ptr<FrameCipher> to_panel_;
  std::unique_ptr<FrameCipher> from_panel_;
};

PairingState unpaired() {
  return PairingState(std::make_unique<MemoryKeyStore>());
}

PairingState paired_with(const Key& key) {
  auto store = std::make_unique<MemoryKeyStore>();
  store->save(key);
  return PairingState(std::move(store));
}

Key some_key() {
  Key k{};
  for (std::size_t i = 0; i < k.size(); ++i) k[i] = static_cast<std::uint8_t>(i * 3 + 1);
  return k;
}

}  // namespace

TEST_CASE("hello says who the panel is and whether it can pair") {
  auto pairing = unpaired();
  Recorder events;
  FakeHa ha(pairing, events);
  const Json& h = ha.hello;
  CHECK(h["t"] == "hello");
  CHECK(h["proto"] == 1);
  CHECK(h["panel_id"] == kPanelId);
  CHECK(h["fw"] == "0.1.0");
  CHECK(h["model"] == "nest-gen2");
  CHECK(h["paired"] == false);
  CHECK(h["pairing_open"] == true);
  CHECK(base64_decode_fixed<16>(h["nonce"].get<std::string>()));
  CHECK(h["caps"]["input"][0] == "ring");

  FakeHa again(pairing, events);
  CHECK(again.hello["nonce"] != h["nonce"]);  // fresh for every connection
}

TEST_CASE("pairing: the panel shows the same code HA computes, and keeps the key") {
  auto pairing = unpaired();
  Recorder events;
  FakeHa ha(pairing, events);
  const auto [code, key] = ha.pair();

  REQUIRE(events.codes.size() == 1);
  CHECK(events.codes[0] == code);
  CHECK(events.pairing_results == std::vector<bool>{true});
  REQUIRE(pairing.key());
  CHECK(*pairing.key() == key);
  CHECK(ha.session.closing());  // HA reconnects to authenticate
  CHECK(ha.session.close_reason() == "paired");
}

TEST_CASE("pairing: a wrong confirmation keeps the old state") {
  auto pairing = unpaired();
  Recorder events;
  FakeHa ha(pairing, events);
  const auto keys = generate_key_pair();
  ha.send_plain(Json{{"t", "pair_begin"}, {"id", 1}, {"pub", base64_encode(keys->public_key)}});
  REQUIRE(ha.next()["ok"] == true);
  ha.send_plain(Json{{"t", "pair_confirm"}, {"id", 2}, {"mac", base64_encode(Mac{})}});
  const Json answer = ha.next();
  CHECK(answer["ok"] == false);
  CHECK(answer["error"]["code"] == "auth_failed");
  CHECK_FALSE(pairing.paired());
  CHECK(events.pairing_results == std::vector<bool>{false});
  CHECK(ha.session.closing());
}

TEST_CASE("pairing: HA can cancel, and the code disappears") {
  auto pairing = unpaired();
  Recorder events;
  FakeHa ha(pairing, events);
  const auto keys = generate_key_pair();
  ha.send_plain(Json{{"t", "pair_begin"}, {"id", 1}, {"pub", base64_encode(keys->public_key)}});
  REQUIRE(ha.next()["ok"] == true);
  ha.send_plain(Json{{"t", "pair_cancel"}});
  CHECK(events.pairing_results == std::vector<bool>{false});
  CHECK(ha.session.closing());
}

TEST_CASE("pairing: a paired panel refuses unless pairing was opened on it") {
  auto pairing = paired_with(some_key());
  Recorder events;
  {
    FakeHa ha(pairing, events);
    CHECK(ha.hello["paired"] == true);
    CHECK(ha.hello["pairing_open"] == false);
    const auto keys = generate_key_pair();
    ha.send_plain(Json{{"t", "pair_begin"}, {"id", 1}, {"pub", base64_encode(keys->public_key)}});
    CHECK(ha.next()["error"]["code"] == "pairing_closed");
    CHECK(ha.session.closing());
  }
  pairing.open_pairing_window(1000);
  FakeHa ha(pairing, events);
  CHECK(ha.hello["pairing_open"] == true);
  const auto [code, key] = ha.pair();
  CHECK(*pairing.key() == key);  // the old key is replaced
  CHECK_FALSE(pairing.pairing_open(ha.now()));  // and the window closes
}

TEST_CASE("pairing: the window closes after five minutes") {
  auto pairing = paired_with(some_key());
  pairing.open_pairing_window(1000);
  CHECK(pairing.pairing_open(1000 + PairingState::kPairingWindow - 1));
  CHECK_FALSE(pairing.pairing_open(1000 + PairingState::kPairingWindow));
}

TEST_CASE("pairing: only one at a time") {
  auto pairing = unpaired();
  Recorder events;
  FakeHa first(pairing, events);
  const auto keys = generate_key_pair();
  first.send_plain(Json{{"t", "pair_begin"}, {"id", 1}, {"pub", base64_encode(keys->public_key)}});
  REQUIRE(first.next()["ok"] == true);

  FakeHa second(pairing, events);
  second.send_plain(Json{{"t", "pair_begin"}, {"id", 1}, {"pub", base64_encode(keys->public_key)}});
  CHECK(second.next()["error"]["code"] == "pairing_closed");
  CHECK(events.codes.size() == 1);
}

TEST_CASE("pairing: a low-order public key is refused") {
  auto pairing = unpaired();
  Recorder events;
  FakeHa ha(pairing, events);
  ha.send_plain(Json{{"t", "pair_begin"}, {"id", 1}, {"pub", base64_encode(Key{})}});
  CHECK(ha.next()["error"]["code"] == "bad_request");
  CHECK(ha.session.closing());
  CHECK(events.codes.empty());
}

TEST_CASE("pairing: the code expires after two minutes") {
  auto pairing = unpaired();
  Recorder events;
  FakeHa ha(pairing, events);
  const auto keys = generate_key_pair();
  ha.send_plain(Json{{"t", "pair_begin"}, {"id", 1}, {"pub", base64_encode(keys->public_key)}});
  REQUIRE(ha.next()["ok"] == true);
  ha.advance(PanelSession::kPairingTimeout);
  CHECK_FALSE(ha.session.closing());
  ha.advance(1);
  CHECK(ha.session.closing());
  CHECK(events.pairing_results == std::vector<bool>{false});
}

TEST_CASE("authentication: the right key opens an encrypted session") {
  const Key key = some_key();
  auto pairing = paired_with(key);
  Recorder events;
  FakeHa ha(pairing, events);
  CHECK(ha.auth(key)["ok"] == true);
  CHECK(ha.session.authenticated());
  CHECK(events.authenticated == 1);

  ha.send(Json{{"t", "ping"}, {"seq", 5}});
  const Json pong = ha.next();
  CHECK(pong["t"] == "pong");
  CHECK(pong["seq"] == 5);
}

TEST_CASE("authentication: a wrong MAC closes, and three in a row lock it out") {
  const Key key = some_key();
  auto pairing = paired_with(key);
  Recorder events;
  for (int i = 0; i < PairingState::kMaxAuthFailures; ++i) {
    FakeHa ha(pairing, events);
    CHECK(ha.auth(key, false)["error"]["code"] == "auth_failed");
    CHECK(ha.session.closing());
  }
  {
    FakeHa ha(pairing, events, 2000);
    const Json refused = ha.auth(key);  // even the right key, while locked out
    CHECK(refused["error"]["code"] == "auth_failed");
  }
  FakeHa later(pairing, events, 1000 + PairingState::kAuthLockout);
  CHECK(later.auth(key)["ok"] == true);
  CHECK(events.authenticated == 1);
}

TEST_CASE("authentication: refused while not paired") {
  auto pairing = unpaired();
  Recorder events;
  FakeHa ha(pairing, events);
  CHECK(ha.auth(some_key())["error"]["code"] == "auth_failed");
}

TEST_CASE("before authentication, requests are refused and nothing reaches the app") {
  auto pairing = paired_with(some_key());
  Recorder events;
  FakeHa ha(pairing, events);
  ha.send_plain(Json{{"t", "info"}, {"id", 3}});
  CHECK(ha.next()["error"]["code"] == "not_ready");
  ha.send_plain(Json{{"t", "layout"}, {"rev", 1}});
  CHECK(ha.nothing_sent());
  CHECK(events.messages.empty());
  CHECK_FALSE(ha.session.closing());
}

TEST_CASE("the handshake must finish within ten seconds") {
  auto pairing = paired_with(some_key());
  Recorder events;
  FakeHa ha(pairing, events);
  ha.advance(PanelSession::kHandshakeTimeout);
  CHECK_FALSE(ha.session.closing());
  ha.advance(1);
  CHECK(ha.session.closing());
}

TEST_CASE("synchronisation: ready after time, config, layout and full states") {
  const Key key = some_key();
  auto pairing = paired_with(key);
  Recorder events;
  FakeHa ha(pairing, events);
  REQUIRE(ha.auth(key)["ok"] == true);

  // An application request before ready is refused; maintenance ones aren't.
  ha.send(Json{{"t", "display"}, {"id", 4}, {"state", "off"}});
  CHECK(ha.next()["error"]["code"] == "not_ready");
  ha.send(Json{{"t", "info"}, {"id", 5}});
  CHECK(ha.next()["uptime_s"] == 1);

  ha.send(Json{{"t", "states"}, {"full", false}, {"items", Json::object()}});
  ha.send(Json{{"t", "time"}, {"utc_ms", 1}});
  ha.send(Json{{"t", "config"}});
  ha.send(Json{{"t", "layout"}, {"rev", 1}});
  CHECK(ha.nothing_sent());  // partial states don't count
  CHECK_FALSE(ha.session.ready());

  ha.send(Json{{"t", "states"}, {"full", true}, {"items", Json::object()}});
  CHECK(ha.next()["t"] == "ready");
  CHECK(ha.session.ready());
  CHECK(events.ready == 1);
  CHECK(events.messages ==
        std::vector<std::string>{"info", "states", "time", "config", "layout", "states"});

  ha.sync();  // a second round doesn't send ready again
  CHECK(ha.nothing_sent());
  CHECK(events.ready == 1);
}

TEST_CASE("after ready, unknown requests get unknown_type and unknown events are ignored") {
  const Key key = some_key();
  auto pairing = paired_with(key);
  Recorder events;
  FakeHa ha(pairing, events);
  REQUIRE(ha.auth(key)["ok"] == true);
  ha.sync();
  REQUIRE(ha.next()["t"] == "ready");

  ha.send(Json{{"t", "teleport"}, {"id", 9}});
  const Json answer = ha.next();
  CHECK(answer["re"] == 9);
  CHECK(answer["error"]["code"] == "unknown_type");
  ha.send(Json{{"t", "future_event"}});
  CHECK(ha.nothing_sent());
  CHECK_FALSE(ha.session.closing());
}

TEST_CASE("unpair forgets the key and closes") {
  const Key key = some_key();
  auto pairing = paired_with(key);
  Recorder events;
  FakeHa ha(pairing, events);
  REQUIRE(ha.auth(key)["ok"] == true);
  ha.send(Json{{"t", "unpair"}, {"id", 7}});
  CHECK(ha.next()["ok"] == true);
  CHECK_FALSE(pairing.paired());
  CHECK(events.unpaired == 1);
  CHECK(ha.session.closing());
}

TEST_CASE("an authenticated session closes after a minute of silence") {
  const Key key = some_key();
  auto pairing = paired_with(key);
  Recorder events;
  FakeHa ha(pairing, events);
  REQUIRE(ha.auth(key)["ok"] == true);
  ha.advance(PanelSession::kIdleTimeout - 1);
  ha.send(Json{{"t", "ping"}, {"seq", 1}});  // any frame resets the timer
  ha.advance(PanelSession::kIdleTimeout - 1);
  CHECK_FALSE(ha.session.closing());
  ha.advance(2);
  CHECK(ha.session.closing());
}

TEST_CASE("a tick stamped before the last frame doesn't count as silence") {
  // The server reads the clock, then reads the socket; frames received there
  // carry a slightly later time than the tick that follows.
  const Key key = some_key();
  auto pairing = paired_with(key);
  Recorder events;
  FakeHa ha(pairing, events);
  REQUIRE(ha.auth(key)["ok"] == true);
  ha.advance(500);
  ha.send(Json{{"t", "ping"}, {"seq", 1}});
  ha.session.tick(ha.now() - 5);
  CHECK_FALSE(ha.session.closing());
}

TEST_CASE("a tampered encrypted frame closes the session") {
  const Key key = some_key();
  auto pairing = paired_with(key);
  Recorder events;
  FakeHa ha(pairing, events);
  REQUIRE(ha.auth(key)["ok"] == true);
  // Plaintext where encryption is required fails to decrypt.
  ha.send_plain(Json{{"t", "ping"}, {"seq", 1}});
  CHECK(ha.session.closing());
  CHECK(ha.session.close_reason() == "a frame failed to decrypt");
}

TEST_CASE("a frame with a bad length closes the session") {
  auto pairing = unpaired();
  Recorder events;
  FakeHa ha(pairing, events);
  ha.feed(Bytes{0, 0, 0, 0});
  CHECK(ha.session.closing());
}

TEST_CASE("a closed session sends nothing more") {
  const Key key = some_key();
  auto pairing = paired_with(key);
  Recorder events;
  FakeHa ha(pairing, events);
  REQUIRE(ha.auth(key)["ok"] == true);
  ha.session.close("test");
  ha.session.send(Json{{"t", "ping"}});
  CHECK(ha.nothing_sent());
  CHECK_FALSE(ha.session.authenticated());
}

TEST_CASE("the file key store saves, loads and erases the key") {
  char dir_template[] = "/tmp/nesto-test-XXXXXX";
  REQUIRE(mkdtemp(dir_template));
  const std::string dir = std::string(dir_template) + "/state";  // created on first save

  FileKeyStore store(dir);
  CHECK_FALSE(store.load());
  CHECK(store.save(some_key()));
  CHECK(store.load() == some_key());

  // A file of the wrong size is ignored rather than trusted.
  {
    std::FILE* f = std::fopen((dir + "/pairing.key").c_str(), "ab");
    REQUIRE(f);
    std::fputc('x', f);
    std::fclose(f);
  }
  CHECK_FALSE(store.load());

  store.erase();
  CHECK_FALSE(store.load());
  rmdir(dir.c_str());
  rmdir(dir_template);
}
