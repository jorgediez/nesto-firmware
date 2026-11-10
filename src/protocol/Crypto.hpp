#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "protocol/Bytes.hpp"

namespace nesto::protocol {

/// Who computes a MAC: some values differ by side (PROTOCOL.md sections 6 and 7).
enum class Role { Ha, Panel };

// Pairing, section 6.2 --------------------------------------------------------

struct KeyPair {
  Key secret{};
  Key public_key{};
};

/// The X25519 key pair for a given secret, which must be 32 random bytes.
KeyPair key_pair_from_secret(const Key& secret);

/// A fresh ephemeral key pair from the system's random source. Nothing if the
/// random source can't be read.
std::optional<KeyPair> generate_key_pair();

/// X25519. Nothing if the result is all zeros, which a peer sending a
/// low-order point would force; both sides must then abort.
std::optional<Key> shared_secret(const Key& secret, const Key& peer_public);

/// The six digits the panel shows and the user types into Home Assistant.
std::string pairing_code(const Key& ha_public, const Key& panel_public, const Key& shared);

Key pairing_key(const Key& shared, const Key& ha_public, const Key& panel_public);

Mac confirm_mac(const Key& pairing_key, Role role);

// Authentication and session keys, sections 7.1 and 7.2 -----------------------

Mac auth_mac(const Key& pairing_key, Role role, const Nonce& panel_nonce, const Nonce& ha_nonce,
             std::string_view panel_id);

struct SessionKeys {
  Key ha_to_panel{};
  Key panel_to_ha{};
};

SessionKeys session_keys(const Key& pairing_key, const Nonce& panel_nonce, const Nonce& ha_nonce);

/// Compares MACs in constant time.
bool macs_equal(const Mac& a, const Mac& b);

/// Overwrites secrets in memory once they're no longer needed.
void wipe(void* secret, std::size_t size);

// Encrypted frames, section 7.3 -----------------------------------------------

/// Encrypts or decrypts the frames of one direction of a session. Each frame
/// uses the next nonce counter, so frames must be handled in the order they
/// are sent or received.
class FrameCipher {
 public:
  explicit FrameCipher(const Key& key, std::uint64_t counter = 0);
  ~FrameCipher();

  FrameCipher(const FrameCipher&) = delete;
  FrameCipher& operator=(const FrameCipher&) = delete;

  /// A complete frame to send: length, ciphertext and tag. Nothing if the
  /// result would exceed the frame limit.
  std::optional<Bytes> seal(const Bytes& plaintext);

  /// Decrypts a received frame's payload (ciphertext and tag, without the
  /// length). Nothing if it fails authentication, in which case the
  /// connection must be closed.
  std::optional<Bytes> open(const Bytes& payload);

  std::uint64_t counter() const { return counter_; }

 private:
  Key key_;
  std::uint64_t counter_;
};

}  // namespace nesto::protocol
