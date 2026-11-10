#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include "protocol/Bytes.hpp"

namespace nesto::protocol {

/// Largest frame payload, PROTOCOL.md section 21.
constexpr std::size_t kMaxFrame = 512 * 1024;

/// A frame as sent: 4-byte big-endian length, then the payload. Empty or
/// oversized payloads aren't valid frames; the caller must not pass them.
Bytes encode_frame(const Bytes& payload);

/// Splits a TCP byte stream into frame payloads (section 3). Bytes can arrive
/// in any pieces; complete frames come out in order. A zero or oversized length
/// is a protocol error, after which the reader stays failed: the connection
/// must be closed.
class FrameReader {
 public:
  void feed(const std::uint8_t* data, std::size_t size);
  void feed(const Bytes& data) { feed(data.data(), data.size()); }

  /// The next complete payload, if there is one.
  std::optional<Bytes> next();

  bool failed() const { return !error_.empty(); }
  const std::string& error() const { return error_; }

 private:
  Bytes header_;  // up to 4 length bytes of the frame being read
  Bytes body_;
  std::size_t expected_ = 0;  // payload length, once the header is complete
  std::deque<Bytes> ready_;
  std::string error_;
};

}  // namespace nesto::protocol
