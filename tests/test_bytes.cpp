#include <doctest/doctest.h>

#include <string>

#include "protocol/Bytes.hpp"

using namespace nesto::protocol;

namespace {

Bytes b(const std::string& s) {
  return Bytes(s.begin(), s.end());
}

}  // namespace

TEST_CASE("base64 matches RFC 4648's test vectors") {
  const std::pair<const char*, const char*> cases[] = {
      {"", ""},         {"f", "Zg=="},         {"fo", "Zm8="},        {"foo", "Zm9v"},
      {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
  };
  for (const auto& [plain, encoded] : cases) {
    CAPTURE(plain);
    CHECK(base64_encode(b(plain)) == encoded);
    const auto decoded = base64_decode(encoded);
    REQUIRE(decoded);
    CHECK(*decoded == b(plain));
  }
}

TEST_CASE("base64 round-trips every byte value") {
  Bytes all(256);
  for (int i = 0; i < 256; ++i) all[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
  const auto decoded = base64_decode(base64_encode(all));
  REQUIRE(decoded);
  CHECK(*decoded == all);
}

TEST_CASE("base64 rejects malformed input") {
  for (const char* bad : {"Zg=", "Zg", "Z===", "Zm9v!A==", "Zh==", "Zm9=", "=Zm9", "Zg==Zg=="}) {
    CAPTURE(bad);
    CHECK_FALSE(base64_decode(bad));
  }
}

TEST_CASE("fixed-size base64 decoding checks the length") {
  CHECK(base64_decode_fixed<3>("Zm9v"));
  CHECK_FALSE(base64_decode_fixed<4>("Zm9v"));
  CHECK_FALSE(base64_decode_fixed<2>("Zm9v"));
}

TEST_CASE("hex round-trips and rejects malformed input") {
  const Bytes data = {0x00, 0x7f, 0x80, 0xff};
  CHECK(hex_encode(data) == "007f80ff");
  CHECK(hex_decode("007F80ff") == data);
  CHECK_FALSE(hex_decode("abc"));
  CHECK_FALSE(hex_decode("zz"));
}
