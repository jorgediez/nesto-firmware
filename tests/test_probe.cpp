// nesto-probe's cryptography self-test, which the thermostat runs: here it
// must pass, which also proves its copied test vectors are right.
#include <doctest/doctest.h>

#include "probe/SelfTest.hpp"

TEST_CASE("the probe's cryptography self-test passes") {
  CHECK(nesto::probe::crypto_self_test() == "");
}
