#include "probe/SelfTest.hpp"

#include <cstring>

#include "monocypher.h"
#include "protocol/Bytes.hpp"
#include "protocol/Crypto.hpp"

namespace nesto::probe {
namespace {

using protocol::Bytes;
using protocol::Key;
using protocol::Mac;

// From tests/vectors/protocol-v1.json ("pairing" and "rfc8439"). The unit
// tests run this self-test too, so a typo here fails the build's tests.
constexpr const char* kHaPrivate = "a9b0b7bec5ccd3dae1e8eff6fd040b121920272e353c434a51585f666d747b82";
constexpr const char* kPanelPrivate =
    "f0f7fe050c131a21282f363d444b525960676e757c838a91989fa6adb4bbc2c9";
constexpr const char* kHaPublic = "1b3defe6a281c0a049fcc14628e9f7aae71ca49a49ca50cb1f13bcc1ec21c275";
constexpr const char* kPanelPublic =
    "f4fc3c0041e99aa60c6077194b6b23b76dbf01ff9919207c4eca2120d933b810";
constexpr const char* kShared = "b8e23719e27dd2e199fc9750288117dd12aaec3451643ce30f88b24e9e432400";
constexpr const char* kCode = "243690";
constexpr const char* kPairingKey =
    "f54b52b7908df4d2638582d64ab4b12fc99ed9b5589227b6ddf17110feb01167";
constexpr const char* kConfirmMacPanel =
    "f5569020e50507c12b68e4e42af17823e457ebed04d7102088eef41790fcdf207d3e75bcc94ce96968ed2beffc376ac3"
    "50b58156308e1cceb2e55fd9603293d4";

constexpr const char* kAeadKey = "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f";
constexpr const char* kAeadNonce = "070000004041424344454647";
constexpr const char* kAeadAad = "50515253c0c1c2c3c4c5c6c7";
constexpr const char* kAeadPlaintext =
    "4c616469657320616e642047656e746c656d656e206f662074686520636c617373206f66202739393a204966204920"
    "636f756c64206f6666657220796f75206f6e6c79206f6e652074697020666f7220746865206675747572652c207375"
    "6e73637265656e20776f756c642062652069742e";
constexpr const char* kAeadCiphertextAndTag =
    "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da9272"
    "8b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831"
    "d7bc3ff4def08e4b7a9de576d26586cec64b61161ae10b594f09e26a7e902ecbd0600691";

Bytes hex(const char* text) {
  return protocol::hex_decode(text).value_or(Bytes{});
}

Key key(const char* text) {
  Key k{};
  const Bytes b = hex(text);
  if (b.size() == k.size()) std::memcpy(k.data(), b.data(), k.size());
  return k;
}

template <typename A>
bool same(const A& actual, const char* expected_hex) {
  return protocol::hex_encode(actual) == expected_hex;
}

}  // namespace

std::string crypto_self_test() {
  std::string failed;
  auto fail = [&failed](const char* what) {
    if (!failed.empty()) failed += ", ";
    failed += what;
  };

  const protocol::KeyPair ha = protocol::key_pair_from_secret(key(kHaPrivate));
  const protocol::KeyPair panel = protocol::key_pair_from_secret(key(kPanelPrivate));
  if (!same(ha.public_key, kHaPublic) || !same(panel.public_key, kPanelPublic)) {
    fail("X25519 public keys");
  }

  const auto shared = protocol::shared_secret(panel.secret, ha.public_key);
  if (!shared || !same(*shared, kShared)) {
    fail("X25519 shared secret");
  } else {
    if (protocol::pairing_code(ha.public_key, panel.public_key, *shared) != kCode) {
      fail("pairing code (SHA-512)");
    }
    const Key pairing = protocol::pairing_key(*shared, ha.public_key, panel.public_key);
    if (!same(pairing, kPairingKey)) {
      fail("pairing key (HKDF-SHA-512)");
    } else if (!same(protocol::confirm_mac(pairing, protocol::Role::Panel), kConfirmMacPanel)) {
      fail("confirm MAC (HMAC-SHA-512)");
    }
  }

  // ChaCha20-Poly1305 with RFC 8439's own nonce, so straight through
  // Monocypher rather than FrameCipher, whose nonces are counters.
  const Bytes aead_key = hex(kAeadKey);
  const Bytes nonce = hex(kAeadNonce);
  const Bytes aad = hex(kAeadAad);
  const Bytes plaintext = hex(kAeadPlaintext);
  Bytes sealed(plaintext.size() + 16);
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, aead_key.data(), nonce.data());
  crypto_aead_write(&ctx, sealed.data(), sealed.data() + plaintext.size(), aad.data(), aad.size(),
                    plaintext.data(), plaintext.size());
  crypto_wipe(&ctx, sizeof ctx);
  if (!same(sealed, kAeadCiphertextAndTag)) fail("ChaCha20-Poly1305");

  // And a frame through the session's own cipher, sealed and opened again.
  const Key session_key = key(kPairingKey);
  protocol::FrameCipher sender(session_key);
  protocol::FrameCipher receiver(session_key);
  const Bytes message = {'n', 'e', 's', 't', 'o'};
  const auto frame = sender.seal(message);
  const auto opened =
      frame && frame->size() > 4 ? receiver.open(Bytes(frame->begin() + 4, frame->end()))
                                 : std::nullopt;
  if (!opened || *opened != message) fail("session frame round trip");

  return failed;
}

}  // namespace nesto::probe
