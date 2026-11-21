#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "protocol/Bytes.hpp"

namespace nesto::protocol {

using Json = nlohmann::json;

/// Error codes of PROTOCOL.md section 20.
enum class ErrorCode {
  BadRequest,
  UnknownType,
  PairingClosed,
  AuthFailed,
  NotReady,
  UnknownKey,
  Unsupported,
  Unavailable,
  ServiceFailed,
  NoImages,
  TooLarge,
  BadSignature,
  Downgrade,
  NoSpace,
  Internal,
};

const char* to_string(ErrorCode code);

/// A decoded message (section 4). `body` is the whole JSON object, with `t`
/// and `id` still in it, so handlers read their fields straight from it.
struct Message {
  std::string type;
  std::optional<std::uint32_t> id;  // present on requests only
  Json body;

  bool is_request() const { return id.has_value(); }
};

/// Decodes a frame payload. Fails, with a reason in `error`, unless it is a
/// JSON object with a non-empty string `t` and, if present, an `id` that is a
/// positive integer that fits in 32 bits.
std::optional<Message> parse_message(const Bytes& payload, std::string& error);

/// Compact UTF-8 JSON, ready to be framed.
Bytes serialize(const Json& message);

/// `{"t": "result", "re": re, "ok": true}`; add fields for answers that carry data.
Json result_ok(std::uint32_t re);

/// `{"t": "result", "re": re, "ok": false, "error": {"code": ..., "message": ...}}`.
Json result_error(std::uint32_t re, ErrorCode code, std::string_view message);

}  // namespace nesto::protocol
