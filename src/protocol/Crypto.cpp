#include "protocol/Crypto.hpp"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>

#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "protocol/Frame.hpp"

namespace nesto::protocol {
namespace {

constexpr std::size_t kTagSize = 16;

Bytes concat(std::initializer_list<std::pair<const std::uint8_t*, std::size_t>> parts) {
  Bytes out;
  for (const auto& [data, size] : parts) out.insert(out.end(), data, data + size);
  return out;
}

Bytes text(std::string_view s) {
  return Bytes(s.begin(), s.end());
}

Key hkdf(const Key& ikm, const Bytes& salt, std::string_view info) {
  Key out{};
  crypto_sha512_hkdf(out.data(), out.size(), ikm.data(), ikm.size(), salt.data(), salt.size(),
                     reinterpret_cast<const std::uint8_t*>(info.data()), info.size());
  return out;
}

Mac hmac(const Key& key, const Bytes& message) {
  Mac out{};
  crypto_sha512_hmac(out.data(), key.data(), key.size(), message.data(), message.size());
  return out;
}

const char* role_name(Role role) {
  return role == Role::Ha ? "ha" : "panel";
}

std::array<std::uint8_t, 12> frame_nonce(std::uint64_t counter) {
  std::array<std::uint8_t, 12> nonce{};
  for (int i = 0; i < 8; ++i) nonce[4 + i] = static_cast<std::uint8_t>(counter >> (8 * i));
  return nonce;
}

std::array<std::uint8_t, 4> length_field(std::size_t n) {
  return {static_cast<std::uint8_t>(n >> 24), static_cast<std::uint8_t>(n >> 16),
          static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n)};
}

}  // namespace

KeyPair key_pair_from_secret(const Key& secret) {
  KeyPair pair;
  pair.secret = secret;
  crypto_x25519_public_key(pair.public_key.data(), pair.secret.data());
  return pair;
}

std::optional<KeyPair> generate_key_pair() {
  Key secret{};
  std::FILE* f = std::fopen("/dev/urandom", "rb");
  if (!f) return std::nullopt;
  const bool ok = std::fread(secret.data(), 1, secret.size(), f) == secret.size();
  std::fclose(f);
  if (!ok) return std::nullopt;
  KeyPair pair = key_pair_from_secret(secret);
  wipe(secret.data(), secret.size());
  return pair;
}

std::optional<Key> shared_secret(const Key& secret, const Key& peer_public) {
  Key shared{};
  crypto_x25519(shared.data(), secret.data(), peer_public.data());
  const Key zero{};
  if (crypto_verify32(shared.data(), zero.data()) == 0) return std::nullopt;
  return shared;
}

std::string pairing_code(const Key& ha_public, const Key& panel_public, const Key& shared) {
  const Bytes label = text("nesto-sas-v1");
  const Bytes input = concat({{label.data(), label.size()},
                              {ha_public.data(), ha_public.size()},
                              {panel_public.data(), panel_public.size()},
                              {shared.data(), shared.size()}});
  std::uint8_t digest[64];
  crypto_sha512(digest, input.data(), input.size());
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value = (value << 8) | digest[i];
  char code[7];
  std::snprintf(code, sizeof code, "%06u", static_cast<unsigned>(value % 1000000));
  return code;
}

Key pairing_key(const Key& shared, const Key& ha_public, const Key& panel_public) {
  const Bytes salt = concat({{ha_public.data(), ha_public.size()},
                             {panel_public.data(), panel_public.size()}});
  return hkdf(shared, salt, "nesto-pair-key-v1");
}

Mac confirm_mac(const Key& pairing_key, Role role) {
  return hmac(pairing_key, text(std::string("nesto-confirm-") + role_name(role) + "-v1"));
}

Mac auth_mac(const Key& pairing_key, Role role, const Nonce& panel_nonce, const Nonce& ha_nonce,
             std::string_view panel_id) {
  // Each side's MAC starts with the other side's nonce, so neither can be
  // replayed as the other.
  const Nonce& first = role == Role::Ha ? panel_nonce : ha_nonce;
  const Nonce& second = role == Role::Ha ? ha_nonce : panel_nonce;
  const Bytes label = text(std::string("nesto-auth-") + role_name(role) + "-v1");
  const Bytes message = concat({{label.data(), label.size()},
                                {first.data(), first.size()},
                                {second.data(), second.size()},
                                {reinterpret_cast<const std::uint8_t*>(panel_id.data()),
                                 panel_id.size()}});
  return hmac(pairing_key, message);
}

SessionKeys session_keys(const Key& pairing_key, const Nonce& panel_nonce, const Nonce& ha_nonce) {
  const Bytes salt = concat({{panel_nonce.data(), panel_nonce.size()},
                             {ha_nonce.data(), ha_nonce.size()}});
  return {hkdf(pairing_key, salt, "nesto-session-h2p-v1"),
          hkdf(pairing_key, salt, "nesto-session-p2h-v1")};
}

bool macs_equal(const Mac& a, const Mac& b) {
  return crypto_verify64(a.data(), b.data()) == 0;
}

void wipe(void* secret, std::size_t size) {
  crypto_wipe(secret, size);
}

FrameCipher::FrameCipher(const Key& key, std::uint64_t counter) : key_(key), counter_(counter) {}

FrameCipher::~FrameCipher() {
  wipe(key_.data(), key_.size());
}

std::optional<Bytes> FrameCipher::seal(const Bytes& plaintext) {
  const std::size_t length = plaintext.size() + kTagSize;
  if (plaintext.empty() || length > kMaxFrame) return std::nullopt;

  const auto header = length_field(length);
  Bytes frame(4 + length);
  std::memcpy(frame.data(), header.data(), 4);
  std::uint8_t* cipher = frame.data() + 4;
  std::uint8_t* tag = cipher + plaintext.size();

  // A fresh context per frame: Monocypher's incremental AEAD re-keys itself
  // after each message, which only matches RFC 8439 for a context's first one.
  const auto nonce = frame_nonce(counter_);
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key_.data(), nonce.data());
  crypto_aead_write(&ctx, cipher, tag, header.data(), header.size(), plaintext.data(),
                    plaintext.size());
  crypto_wipe(&ctx, sizeof ctx);
  ++counter_;
  return frame;
}

std::optional<Bytes> FrameCipher::open(const Bytes& payload) {
  if (payload.size() <= kTagSize || payload.size() > kMaxFrame) return std::nullopt;
  const std::size_t text_size = payload.size() - kTagSize;
  const auto header = length_field(payload.size());
  const auto nonce = frame_nonce(counter_);

  Bytes plaintext(text_size);
  crypto_aead_ctx ctx;
  crypto_aead_init_ietf(&ctx, key_.data(), nonce.data());
  const int failed = crypto_aead_read(&ctx, plaintext.data(), payload.data() + text_size,
                                      header.data(), header.size(), payload.data(), text_size);
  crypto_wipe(&ctx, sizeof ctx);
  if (failed) return std::nullopt;
  ++counter_;
  return plaintext;
}

}  // namespace nesto::protocol
