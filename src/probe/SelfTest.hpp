#pragma once

#include <string>

namespace nesto::probe {

/// Runs the protocol's cryptography against the published test vectors
/// (tests/vectors/protocol-v1.json and RFC 8439): X25519, the pairing code,
/// HKDF and HMAC-SHA-512, and ChaCha20-Poly1305. On the thermostat this shows
/// the code computes the same results on its CPU as everywhere else. Empty if
/// everything matches, otherwise what didn't.
std::string crypto_self_test();

}  // namespace nesto::probe
