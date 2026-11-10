#include <doctest/doctest.h>

#include "net/Mdns.hpp"

using nesto::net::make_service;

TEST_CASE("the service is named after the panel ID's last six digits") {
  const auto s = make_service("18b4300a1b2c", 6170, "0.1.0", "nest-gen2", false);
  CHECK(s.instance == "Nesto Panel 0a1b2c");
  CHECK(s.host == "nesto-0a1b2c");
  CHECK(s.port == 6170);
}

TEST_CASE("the TXT record carries the keys of PROTOCOL.md section 2") {
  const auto s = make_service("18b4300a1b2c", 6170, "0.1.0", "nest-gen2", true);
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"id", "18b4300a1b2c"}, {"proto", "1"}, {"fw", "0.1.0"},
      {"model", "nest-gen2"}, {"paired", "1"},
  };
  CHECK(s.txt == expected);
  CHECK(make_service("18b4300a1b2c", 6170, "0.1.0", "nest-gen2", false).txt.back().second == "0");
}

TEST_CASE("a change in pairing changes the service, so it's announced again") {
  const auto unpaired = make_service("18b4300a1b2c", 6170, "0.1.0", "nest-gen2", false);
  const auto paired = make_service("18b4300a1b2c", 6170, "0.1.0", "nest-gen2", true);
  CHECK(unpaired != paired);
  CHECK(unpaired == make_service("18b4300a1b2c", 6170, "0.1.0", "nest-gen2", false));
}

TEST_CASE("a short panel ID is used whole") {
  CHECK(make_service("abc", 1, "", "", false).host == "nesto-abc");
}
