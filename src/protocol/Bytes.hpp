#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nesto::protocol {

using Bytes = std::vector<std::uint8_t>;
using Key = std::array<std::uint8_t, 32>;    // X25519 keys, pairing and session keys
using Mac = std::array<std::uint8_t, 64>;    // HMAC-SHA-512
using Nonce = std::array<std::uint8_t, 16>;  // per-connection nonces in hello and auth

/// Standard base64 with padding, as the protocol's JSON carries binary fields.
std::string base64_encode(const std::uint8_t* data, std::size_t size);
std::optional<Bytes> base64_decode(std::string_view text);

/// Lower-case hex, for logs and tests.
std::string hex_encode(const std::uint8_t* data, std::size_t size);
std::optional<Bytes> hex_decode(std::string_view text);

template <typename Container>
std::string base64_encode(const Container& c) {
  return base64_encode(c.data(), c.size());
}

template <typename Container>
std::string hex_encode(const Container& c) {
  return hex_encode(c.data(), c.size());
}

/// Decodes into a fixed-size array; nothing if the text isn't exactly that long.
template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> base64_decode_fixed(std::string_view text) {
  auto bytes = base64_decode(text);
  if (!bytes || bytes->size() != N) return std::nullopt;
  std::array<std::uint8_t, N> out{};
  for (std::size_t i = 0; i < N; ++i) out[i] = (*bytes)[i];
  return out;
}

}  // namespace nesto::protocol
