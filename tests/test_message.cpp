#include <doctest/doctest.h>

#include <string>

#include "protocol/Message.hpp"

using namespace nesto::protocol;

namespace {

std::optional<Message> parse(const std::string& text, std::string& error) {
  return parse_message(Bytes(text.begin(), text.end()), error);
}

}  // namespace

TEST_CASE("events and requests are told apart by id") {
  std::string error;
  const auto event = parse(R"({"t":"ping","seq":5})", error);
  REQUIRE(event);
  CHECK(event->type == "ping");
  CHECK_FALSE(event->is_request());
  CHECK(event->body["seq"] == 5);

  const auto request = parse(R"({"t":"display","id":30,"state":"off"})", error);
  REQUIRE(request);
  CHECK(request->is_request());
  CHECK(*request->id == 30);
  CHECK(request->body["state"] == "off");
}

TEST_CASE("unknown fields are kept for the handler to ignore") {
  std::string error;
  const auto m = parse(R"({"t":"ping","seq":1,"future":{"x":1}})", error);
  REQUIRE(m);
  CHECK(m->body.contains("future"));
}

TEST_CASE("malformed messages are rejected with a reason") {
  const char* bad[] = {
      "not json", "[1,2]", "\"text\"", R"({"seq":1})", R"({"t":""})", R"({"t":5})",
      R"({"t":"x","id":0})", R"({"t":"x","id":-3})", R"({"t":"x","id":1.5})",
      R"({"t":"x","id":"7"})", R"({"t":"x","id":4294967296})",
  };
  for (const char* text : bad) {
    CAPTURE(text);
    std::string error;
    CHECK_FALSE(parse(text, error));
    CHECK_FALSE(error.empty());
  }
}

TEST_CASE("the largest 32-bit id is accepted") {
  std::string error;
  const auto m = parse(R"({"t":"x","id":4294967295})", error);
  REQUIRE(m);
  CHECK(*m->id == 4294967295u);
}

TEST_CASE("invalid UTF-8 is rejected") {
  std::string error;
  CHECK_FALSE(parse(std::string("{\"t\":\"a\xff\"}"), error));
}

TEST_CASE("results follow section 4") {
  CHECK(serialize(result_ok(41)) == serialize(Json::parse(R"({"t":"result","re":41,"ok":true})")));

  const Json error = result_error(41, ErrorCode::UnknownKey, "No item l9");
  CHECK(error["ok"] == false);
  CHECK(error["error"]["code"] == "unknown_key");
  CHECK(error["error"]["message"] == "No item l9");
}

TEST_CASE("every error code has its protocol name") {
  CHECK(std::string(to_string(ErrorCode::BadRequest)) == "bad_request");
  CHECK(std::string(to_string(ErrorCode::PairingClosed)) == "pairing_closed");
  CHECK(std::string(to_string(ErrorCode::ServiceFailed)) == "service_failed");
  CHECK(std::string(to_string(ErrorCode::NoImages)) == "no_images");
  CHECK(std::string(to_string(ErrorCode::TooLarge)) == "too_large");
}

TEST_CASE("serializing replaces invalid UTF-8 instead of failing") {
  const Bytes out = serialize(Json{{"t", std::string("a\xff")}});
  CHECK(std::string(out.begin(), out.end()) == "{\"t\":\"a\xEF\xBF\xBD\"}");
}
