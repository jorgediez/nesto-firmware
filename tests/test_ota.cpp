// Firmware updates: what a release's signature covers, and receiving a new
// program into place, or refusing it (PROTOCOL.md section 19).
#include <doctest/doctest.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "monocypher-ed25519.h"
#include "protocol/Bytes.hpp"
#include "protocol/Ota.hpp"

using namespace nesto;
using namespace nesto::protocol;

namespace {

/// A key pair from a fixed seed, as a release key would be.
struct Signer {
  std::array<std::uint8_t, 64> secret{};
  OtaKey public_key{};
  explicit Signer(std::uint8_t fill) {
    std::array<std::uint8_t, 32> seed{};
    seed.fill(fill);
    crypto_ed25519_key_pair(secret.data(), public_key.data(), seed.data());
  }
  std::string sign(const std::string& text) const {
    OtaSignature sig{};
    crypto_ed25519_sign(sig.data(), secret.data(), reinterpret_cast<const std::uint8_t*>(text.data()),
                        text.size());
    return base64_encode(sig);
  }
};

Sha512 sha512_of(const std::string& data) {
  Sha512 out{};
  crypto_sha512(out.data(), reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
  return out;
}

std::string read(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

bool exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

struct Fixture {
  std::string dir;
  std::string target;
  Signer release{7};
  std::unique_ptr<OtaReceiver> ota;
  Millis now = 1000;

  Fixture() {
    char pattern[] = "/tmp/nesto-ota-XXXXXX";
    dir = mkdtemp(pattern);
    target = dir + "/nesto";
    std::ofstream(target) << "the old program";
    ota = std::make_unique<OtaReceiver>(
        OtaReceiver::Config{target, dir, "0.2.0", "nest-gen2", {release.public_key}});
  }
  ~Fixture() {
    ota.reset();
    if (std::system(("rm -rf " + dir).c_str()) != 0) MESSAGE("couldn't remove " << dir);
  }

  Json offer(const std::string& program, const std::string& version = "0.3.0",
             const Signer* signer = nullptr, const std::string& model = "nest-gen2") const {
    const Sha512 hash = sha512_of(program);
    const std::string text = ota_signed_text(model, version, program.size(), hash);
    return Json{{"t", "ota_begin"},  {"model", model}, {"version", version},
                {"size", program.size()}, {"sha512", hex_encode(hash)},
                {"signature", (signer ? *signer : release).sign(text)}};
  }
  OtaResult send(const std::string& data, std::uint64_t offset) {
    return ota->chunk(Json{{"offset", offset},
                           {"data", base64_encode(reinterpret_cast<const std::uint8_t*>(data.data()),
                                                  data.size())}},
                      now);
  }
};

}  // namespace

TEST_CASE("versions compare number by number") {
  CHECK(compare_versions("0.2.0", "0.1.9") > 0);
  CHECK(compare_versions("0.10.0", "0.9.0") > 0);
  CHECK(compare_versions("1.0", "1.0.0") == 0);
  CHECK(compare_versions("0.1.0", "0.1.1") < 0);
}

TEST_CASE("what the signature covers, line by line") {
  Sha512 hash{};
  hash.fill(0xab);
  std::string ab;
  for (int i = 0; i < 64; ++i) ab += "ab";
  CHECK(ota_signed_text("nest-gen2", "0.2.0", 1234, hash) ==
        "nesto-ota-v1\nmodel=nest-gen2\nversion=0.2.0\nsize=1234\nsha512=" + ab + "\n");
}

TEST_CASE("a signed program arrives in pieces and goes into place") {
  Fixture f;
  const std::string program = "a new program, " + std::string(100, 'x');
  REQUIRE(f.ota->begin(f.offer(program), f.now).ok);
  CHECK(f.ota->stage() == OtaReceiver::Stage::Receiving);
  REQUIRE(f.send(program.substr(0, 50), 0).ok);
  CHECK(f.ota->received() == 50);
  REQUIRE(f.send(program.substr(50), 50).ok);
  REQUIRE(f.ota->end().ok);
  CHECK(read(f.target) == "the old program");  // nothing replaced until applied
  REQUIRE(f.ota->apply().ok);

  CHECK(read(f.target) == program);
  CHECK(read(f.target + ".prev") == "the old program");
  CHECK_FALSE(exists(f.target + ".new"));
  CHECK(read(OtaReceiver::pending_path(f.dir)) == "0.3.0\n");
  // The new program, once Home Assistant is back, confirms itself.
  CHECK(OtaReceiver::confirm(f.dir) == "0.3.0");
  CHECK_FALSE(exists(OtaReceiver::pending_path(f.dir)));
  CHECK(OtaReceiver::confirm(f.dir).empty());
}

TEST_CASE("the same version again is allowed, an older one isn't") {
  Fixture f;
  CHECK(f.ota->begin(f.offer("same", "0.2.0"), f.now).ok);
  const OtaResult older = f.ota->begin(f.offer("old", "0.1.9"), f.now);
  CHECK(older.code == ErrorCode::Downgrade);
}

TEST_CASE("offers it refuses") {
  Fixture f;
  const Signer stranger{9};
  CHECK(f.ota->begin(f.offer("p", "0.3.0", &stranger), f.now).code == ErrorCode::BadSignature);
  CHECK(f.ota->begin(f.offer("p", "0.3.0", nullptr, "nest-gen3"), f.now).code ==
        ErrorCode::Unsupported);

  Json changed = f.offer("p");
  changed["version"] = "9.9.9";  // signed for 0.3.0
  CHECK(f.ota->begin(changed, f.now).code == ErrorCode::BadSignature);

  Json huge = f.offer("p");
  huge["size"] = 64 * 1024 * 1024;
  CHECK(f.ota->begin(huge, f.now).code == ErrorCode::TooLarge);

  Json missing = f.offer("p");
  missing.erase("signature");
  CHECK(f.ota->begin(missing, f.now).code == ErrorCode::BadRequest);
  CHECK(f.ota->stage() == OtaReceiver::Stage::Idle);
  CHECK_FALSE(exists(f.target + ".new"));
}

TEST_CASE("pieces out of order, too many bytes, or a program that isn't the one signed") {
  Fixture f;
  const std::string program = "0123456789";
  REQUIRE(f.ota->begin(f.offer(program), f.now).ok);
  CHECK(f.send("456", 4).code == ErrorCode::BadRequest);
  CHECK(f.ota->end().code == ErrorCode::BadRequest);  // not all of it yet
  CHECK(f.send("0123456789!", 0).code == ErrorCode::BadRequest);
  REQUIRE(f.send("0123456789", 0).ok);

  // Same size, other bytes: the SHA-512 tells.
  Fixture g;
  REQUIRE(g.ota->begin(g.offer(program), g.now).ok);
  REQUIRE(g.send("9876543210", 0).ok);
  CHECK(g.ota->end().code == ErrorCode::BadSignature);
  CHECK_FALSE(exists(g.target + ".new"));
  CHECK(g.ota->apply().code == ErrorCode::BadRequest);
  CHECK(read(g.target) == "the old program");
}

TEST_CASE("a stalled update is dropped, and a new offer replaces one in progress") {
  Fixture f;
  REQUIRE(f.ota->begin(f.offer("abcdef"), f.now).ok);
  REQUIRE(f.send("abc", 0).ok);
  f.ota->tick(f.now + OtaReceiver::kStallTimeout - 1);
  CHECK(f.ota->stage() == OtaReceiver::Stage::Receiving);
  f.ota->tick(f.now + OtaReceiver::kStallTimeout);
  CHECK(f.ota->stage() == OtaReceiver::Stage::Idle);
  CHECK_FALSE(exists(f.target + ".new"));

  REQUIRE(f.ota->begin(f.offer("abcdef"), f.now).ok);
  REQUIRE(f.send("abc", 0).ok);
  REQUIRE(f.ota->begin(f.offer("xyz", "0.4.0"), f.now).ok);
  CHECK(f.ota->received() == 0);
  CHECK(f.ota->version() == "0.4.0");
}
