#include "protocol/Message.hpp"

#include <limits>

namespace nesto::protocol {

const char* to_string(ErrorCode code) {
  switch (code) {
    case ErrorCode::BadRequest: return "bad_request";
    case ErrorCode::UnknownType: return "unknown_type";
    case ErrorCode::PairingClosed: return "pairing_closed";
    case ErrorCode::AuthFailed: return "auth_failed";
    case ErrorCode::NotReady: return "not_ready";
    case ErrorCode::UnknownKey: return "unknown_key";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::Unavailable: return "unavailable";
    case ErrorCode::ServiceFailed: return "service_failed";
    case ErrorCode::NoImages: return "no_images";
    case ErrorCode::TooLarge: return "too_large";
    case ErrorCode::Internal: return "internal";
  }
  return "internal";
}

std::optional<Message> parse_message(const Bytes& payload, std::string& error) {
  Json body = Json::parse(payload.begin(), payload.end(), nullptr, /*allow_exceptions=*/false);
  if (body.is_discarded()) {
    error = "not valid JSON";
    return std::nullopt;
  }
  if (!body.is_object()) {
    error = "not a JSON object";
    return std::nullopt;
  }

  const auto t = body.find("t");
  if (t == body.end() || !t->is_string() || t->get_ref<const std::string&>().empty()) {
    error = "missing message type `t`";
    return std::nullopt;
  }

  Message message;
  message.type = t->get<std::string>();

  const auto id = body.find("id");
  if (id != body.end()) {
    if (!id->is_number_unsigned() || id->get<std::uint64_t>() == 0 ||
        id->get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
      error = "`id` must be a positive 32-bit integer";
      return std::nullopt;
    }
    message.id = static_cast<std::uint32_t>(id->get<std::uint64_t>());
  }

  message.body = std::move(body);
  return message;
}

Bytes serialize(const Json& message) {
  // Invalid UTF-8 in a string becomes U+FFFD instead of throwing.
  const std::string text = message.dump(-1, ' ', false, Json::error_handler_t::replace);
  return Bytes(text.begin(), text.end());
}

Json result_ok(std::uint32_t re) {
  return Json{{"t", "result"}, {"re", re}, {"ok", true}};
}

Json result_error(std::uint32_t re, ErrorCode code, std::string_view message) {
  return Json{{"t", "result"},
              {"re", re},
              {"ok", false},
              {"error", {{"code", to_string(code)}, {"message", std::string(message)}}}};
}

}  // namespace nesto::protocol
