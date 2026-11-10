// Checks the panel's cryptography against the vectors the Python reference
// implementation generates (tools/make_vectors.py). Home Assistant's side is
// checked against the same file, so passing here means the two can talk.
#include <doctest/doctest.h>

#include <fstream>
#include <sstream>

#include "protocol/Crypto.hpp"
#include "protocol/Frame.hpp"
#include "protocol/Message.hpp"
#include "monocypher.h"

using namespace nesto::protocol;

namespace {

const Json& vectors() {
  static const Json v = [] {
    std::ifstream file(NESTO_VECTORS_FILE);
    REQUIRE_MESSAGE(file.good(), "can't open " NESTO_VECTORS_FILE);
    std::stringstream text;
    text << file.rdbuf();
    return Json::parse(text.str());
  }();
  return v;
}

Bytes bytes(const Json& hex) {
  auto out = hex_decode(hex.get<std::string>());
  REQUIRE(out);
  return *out;
}

template <std::size_t N>
std::array<std::uint8_t, N> fixed(const Json& hex) {
  const Bytes b = bytes(hex);
  REQUIRE(b.size() == N);
  std::array<std::uint8_t, N> out{};
  std::copy(b.begin(), b.end(), out.begin());
  return out;
}

Bytes text(const std::string& s) {
  return Bytes(s.begin(), s.end());
}

}  // namespace

TEST_CASE("the vectors are for protocol version 1") {
  CHECK(vectors()["protocol"] == 1);
}

TEST_CASE("pairing: key pairs, shared secret, code, key and confirmation MACs") {
  const Json& v = vectors()["pairing"];
  const KeyPair ha = key_pair_from_secret(fixed<32>(v["ha_private"]));
  const KeyPair panel = key_pair_from_secret(fixed<32>(v["panel_private"]));
  CHECK(hex_encode(ha.public_key) == v["ha_public"]);
  CHECK(hex_encode(panel.public_key) == v["panel_public"]);

  const auto from_ha = shared_secret(ha.secret, panel.public_key);
  const auto from_panel = shared_secret(panel.secret, ha.public_key);
  REQUIRE(from_ha);
  REQUIRE(from_panel);
  CHECK(*from_ha == *from_panel);
  CHECK(hex_encode(*from_ha) == v["shared_secret"]);

  CHECK(pairing_code(ha.public_key, panel.public_key, *from_ha) == v["code"]);

  const Key key = pairing_key(*from_ha, ha.public_key, panel.public_key);
  CHECK(hex_encode(key) == v["pairing_key"]);
  CHECK(hex_encode(confirm_mac(key, Role::Ha)) == v["confirm_mac_ha"]);
  CHECK(hex_encode(confirm_mac(key, Role::Panel)) == v["confirm_mac_panel"]);
}

TEST_CASE("pairing: a low-order public key is refused") {
  // The all-zero point makes every X25519 result zero.
  const KeyPair panel = key_pair_from_secret(fixed<32>(vectors()["pairing"]["panel_private"]));
  CHECK_FALSE(shared_secret(panel.secret, Key{}));
}

TEST_CASE("pairing: the code is always six digits") {
  const KeyPair a = key_pair_from_secret(fixed<32>(vectors()["pairing"]["ha_private"]));
  for (std::uint8_t i = 0; i < 50; ++i) {
    Key shared{};
    shared[0] = i;
    const std::string code = pairing_code(a.public_key, a.public_key, shared);
    CAPTURE(code);
    CHECK(code.size() == 6);
    CHECK(code.find_first_not_of("0123456789") == std::string::npos);
  }
}

TEST_CASE("generated key pairs are random and valid") {
  const auto a = generate_key_pair();
  const auto b = generate_key_pair();
  REQUIRE(a);
  REQUIRE(b);
  CHECK(a->secret != b->secret);
  CHECK(key_pair_from_secret(a->secret).public_key == a->public_key);
  CHECK(shared_secret(a->secret, b->public_key) == shared_secret(b->secret, a->public_key));
}

TEST_CASE("authentication: both MACs and the session keys") {
  const Json& v = vectors()["session"];
  const Key key = fixed<32>(v["pairing_key"]);
  const Nonce panel_nonce = fixed<16>(v["panel_nonce"]);
  const Nonce ha_nonce = fixed<16>(v["ha_nonce"]);
  const std::string panel_id = v["panel_id"];

  const Mac ha_mac = auth_mac(key, Role::Ha, panel_nonce, ha_nonce, panel_id);
  const Mac panel_mac = auth_mac(key, Role::Panel, panel_nonce, ha_nonce, panel_id);
  CHECK(hex_encode(ha_mac) == v["auth_mac_ha"]);
  CHECK(hex_encode(panel_mac) == v["auth_mac_panel"]);
  CHECK(macs_equal(ha_mac, fixed<64>(v["auth_mac_ha"])));
  CHECK_FALSE(macs_equal(ha_mac, panel_mac));

  const SessionKeys keys = session_keys(key, panel_nonce, ha_nonce);
  CHECK(hex_encode(keys.ha_to_panel) == v["key_h2p"]);
  CHECK(hex_encode(keys.panel_to_ha) == v["key_p2h"]);
}

TEST_CASE("encrypted frames match the reference byte for byte, both ways") {
  const Json& v = vectors()["session"];
  for (const Json& f : v["frames"]) {
    const std::string direction = f["direction"];
    const Key key = fixed<32>(direction == "h2p" ? v["key_h2p"] : v["key_p2h"]);
    const std::uint64_t counter = f["counter"];
    const Bytes plaintext = text(f["plaintext"]);
    const Bytes expected = bytes(f["frame"]);
    CAPTURE(direction);
    CAPTURE(counter);

    FrameCipher sender(key, counter);
    const auto sealed = sender.seal(plaintext);
    REQUIRE(sealed);
    CHECK(hex_encode(*sealed) == f["frame"]);
    CHECK(sender.counter() == counter + 1);

    // The receiver gets the payload without the length, as FrameReader gives it.
    FrameReader reader;
    reader.feed(expected);
    const auto payload = reader.next();
    REQUIRE(payload);
    FrameCipher receiver(key, counter);
    const auto opened = receiver.open(*payload);
    REQUIRE(opened);
    CHECK(*opened == plaintext);
  }
}

TEST_CASE("a frame fails to open with the wrong counter, key or a changed byte") {
  const Json& v = vectors()["session"];
  const Json& f = v["frames"][0];
  const Key key = fixed<32>(v["key_h2p"]);
  const Bytes frame = bytes(f["frame"]);
  const Bytes payload(frame.begin() + 4, frame.end());

  FrameCipher wrong_counter(key, 1);
  CHECK_FALSE(wrong_counter.open(payload));
  CHECK(wrong_counter.counter() == 1);  // a failure doesn't advance it

  FrameCipher wrong_key(fixed<32>(v["key_p2h"]), 0);
  CHECK_FALSE(wrong_key.open(payload));

  for (std::size_t i = 0; i < payload.size(); i += 7) {
    Bytes changed = payload;
    changed[i] ^= 0x01;
    FrameCipher receiver(key, 0);
    CAPTURE(i);
    CHECK_FALSE(receiver.open(changed));
  }

  // Dropping the last byte changes the length, which is authenticated too.
  FrameCipher receiver(key, 0);
  CHECK_FALSE(receiver.open(Bytes(payload.begin(), payload.end() - 1)));
}

TEST_CASE("frames in a session use consecutive counters") {
  const Key key = fixed<32>(vectors()["session"]["key_h2p"]);
  FrameCipher sender(key);
  FrameCipher receiver(key);
  for (int i = 0; i < 5; ++i) {
    const Bytes message = text("message " + std::to_string(i));
    const auto frame = sender.seal(message);
    REQUIRE(frame);
    const auto opened = receiver.open(Bytes(frame->begin() + 4, frame->end()));
    REQUIRE(opened);
    CHECK(*opened == message);
  }
  CHECK(sender.counter() == 5);
  CHECK(receiver.counter() == 5);
}

TEST_CASE("sealing refuses empty and oversized messages") {
  FrameCipher sender(Key{});
  CHECK_FALSE(sender.seal({}));
  CHECK_FALSE(sender.seal(Bytes(kMaxFrame - 15, 'x')));
  CHECK(sender.seal(Bytes(kMaxFrame - 16, 'x')));
}

TEST_CASE("Monocypher's IETF AEAD matches RFC 8439 section 2.8.2") {
  const Json& v = vectors()["rfc8439"];
  const Key key = fixed<32>(v["key"]);
  const auto nonce = fixed<12>(v["nonce"]);
  const Bytes aad = bytes(v["aad"]);
  const Bytes plaintext = bytes(v["plaintext"]);
  const Bytes expected = bytes(v["ciphertext_and_tag"]);

  Bytes out(plaintext.size() + 16);
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key.data(), nonce.data());
  crypto_aead_write(&ctx, out.data(), out.data() + plaintext.size(), aad.data(), aad.size(),
                    plaintext.data(), plaintext.size());
  CHECK(out == expected);
  CHECK(hex_encode(Bytes(out.end() - 16, out.end())) == "1ae10b594f09e26a7e902ecbd0600691");
}
