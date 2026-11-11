#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "core/Backplate.hpp"

namespace bp = nesto::backplate;
using Bytes = std::vector<std::uint8_t>;

namespace {

/// A frame as the backplate sends it: d5 d5 aa 96, then as a command.
Bytes reply(std::uint16_t type, const Bytes& payload = {}) {
  Bytes frame = bp::encode(type, payload);
  frame.insert(frame.begin(), 0xd5);
  return frame;
}

Bytes ascii(const std::string& s) { return Bytes(s.begin(), s.end()); }

std::vector<bp::Frame> read(bp::FrameReader& reader, const Bytes& bytes) {
  std::vector<bp::Frame> out;
  reader.feed(bytes.data(), bytes.size(), out);
  return out;
}

/// The commands in some output, by type.
std::vector<std::uint16_t> commands(const Bytes& bytes) {
  std::vector<std::uint16_t> out;
  std::size_t i = 0;
  while (i + 7 <= bytes.size()) {
    REQUIRE(bytes[i] == 0xd5);
    REQUIRE(bytes[i + 1] == 0xaa);
    REQUIRE(bytes[i + 2] == 0x96);
    out.push_back(static_cast<std::uint16_t>(bytes[i + 3] | (bytes[i + 4] << 8)));
    const std::size_t length = bytes[i + 5] | (bytes[i + 6] << 8);
    i += 7 + length + 2;
  }
  CHECK(i == bytes.size());
  return out;
}

using Commands = std::vector<std::uint16_t>;

/// Gives the session a frame.
void say(bp::Session& s, nesto::Millis now, std::uint16_t type, const Bytes& payload = {}) {
  s.receive(now, bp::Frame{type, payload});
}

/// Takes a session through its handshake; returns the time it ends at.
nesto::Millis handshake(bp::Session& s, nesto::Millis now = 1000) {
  s.start(now);
  CHECK(s.take_break());
  s.tick(now + 250);
  CHECK(commands(s.take_output()) == Commands{bp::kReset});
  now += 300;
  say(s, now, bp::kAscii, ascii("4.2.8 2019-04-03 12:00:00 K"));
  say(s, now, bp::kFetPresence, {1, 2, 3});
  say(s, now, bp::kAscii, ascii("BRK"));
  CHECK(commands(s.take_output()) == Commands{bp::kFetPresenceAck, bp::kGetTfeVersion});
  say(s, now + 10, bp::kTfeVersion, ascii("4.2.8"));
  CHECK(commands(s.take_output()) == Commands{bp::kGetTfeBuildInfo});
  say(s, now + 20, bp::kTfeBuildInfo, ascii("2019-04-03 12:00:00"));
  CHECK(commands(s.take_output()) == Commands{bp::kGetModel});
  say(s, now + 30, bp::kModel, ascii("Backplate-3.2\r"));
  CHECK(s.running());
  CHECK(commands(s.take_output()) == Commands{bp::kStatusRequest, bp::kRequestBuffers});
  return now + 30;
}

}  // namespace

TEST_CASE("the CRC is CRC-16/XMODEM, as published for the backplate") {
  const Bytes reset = {0xff, 0x00, 0x00, 0x00};
  CHECK(bp::crc16(reset.data(), reset.size()) == 0x4ba3);
  const Bytes fet = {0x82, 0x00, 0x02, 0x00, 0x00, 0x00};
  CHECK(bp::crc16(fet.data(), fet.size()) == 0xb208);
  const Bytes check = ascii("123456789");
  CHECK(bp::crc16(check.data(), check.size()) == 0x31c3);  // the standard check value
}

TEST_CASE("a command is the preamble, type, length, payload and CRC, little-endian") {
  CHECK(bp::encode(bp::kReset) == Bytes{0xd5, 0xaa, 0x96, 0xff, 0x00, 0x00, 0x00, 0xa3, 0x4b});
  CHECK(bp::encode(0x0082, {0x00, 0x00}) ==
        Bytes{0xd5, 0xaa, 0x96, 0x82, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0xb2});
}

TEST_CASE("frames are read whole, split anywhere, and among other bytes") {
  const Bytes a = reply(bp::kAscii, ascii("BRK"));
  const Bytes b = reply(bp::kClimate, {0x9a, 0x08, 0xbb, 0x01});
  Bytes stream = {0x00, 0xd5, 0x13};
  stream.insert(stream.end(), a.begin(), a.end());
  stream.insert(stream.end(), {0xd5, 0xd5});  // a false start
  stream.insert(stream.end(), b.begin(), b.end());

  for (std::size_t split = 0; split <= stream.size(); ++split) {
    bp::FrameReader reader;
    std::vector<bp::Frame> frames = read(reader, Bytes(stream.begin(), stream.begin() + static_cast<long>(split)));
    const std::vector<bp::Frame> rest = read(reader, Bytes(stream.begin() + static_cast<long>(split), stream.end()));
    frames.insert(frames.end(), rest.begin(), rest.end());
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].type == bp::kAscii);
    CHECK(bp::text(frames[0]) == "BRK");
    CHECK(frames[1].type == bp::kClimate);
  }
}

TEST_CASE("a frame with a wrong CRC, or an impossible length, is skipped") {
  Bytes bad = reply(bp::kAscii, ascii("hello"));
  bad.back() ^= 0x01;
  Bytes huge = {0xd5, 0xd5, 0xaa, 0x96, 0x01, 0x00, 0xff, 0xff};
  const Bytes good = reply(bp::kAscii, ascii("ok"));
  Bytes stream = bad;
  stream.insert(stream.end(), huge.begin(), huge.end());
  stream.insert(stream.end(), good.begin(), good.end());

  bp::FrameReader reader;
  const std::vector<bp::Frame> frames = read(reader, stream);
  REQUIRE(frames.size() == 1);
  CHECK(bp::text(frames[0]) == "ok");
  CHECK(reader.bad_frames() == 2);
}

TEST_CASE("the state carries the voltages, the climate temperature and humidity") {
  // Input 12.00 V at 8, operating 3.300 V at 10, battery 3.952 V at 12.
  const Bytes state = {0, 0, 0, 0, 0, 0, 0, 0, 0xb0, 0x04, 0xe4, 0x0c, 0x70, 0x0f, 0, 0};
  const auto power = bp::decode_power(state);
  REQUIRE(power);
  CHECK(power->input == doctest::Approx(12.0));
  CHECK(power->operating == doctest::Approx(3.3));
  CHECK(power->battery == doctest::Approx(3.952));
  CHECK_FALSE(bp::decode_power(Bytes(13, 0)));

  const auto climate = bp::decode_climate({0x9a, 0x08, 0xbb, 0x01});  // 2202, 443
  REQUIRE(climate);
  CHECK(climate->celsius == doctest::Approx(22.02));
  CHECK(climate->humidity == doctest::Approx(44.3));
  const auto cold = bp::decode_climate({0x0c, 0xfe, 0, 0});  // -500
  CHECK(cold->celsius == doctest::Approx(-5.0));
}

TEST_CASE("the handshake: break, reset, burst to BRK, acknowledgement, three questions") {
  bp::Session s;
  CHECK(s.phase() == bp::Session::Phase::kStopped);
  handshake(s);
  CHECK(s.version() == "4.2.8");
  CHECK(s.build_info() == "2019-04-03 12:00:00");
  CHECK(s.model() == "Backplate-3.2");
  CHECK(s.handshakes() == 1);
  CHECK_FALSE(s.take_break());
}

TEST_CASE("the wire presence is acknowledged with its own payload") {
  bp::Session s;
  s.start(0);
  s.tick(250);
  s.take_output();
  say(s, 300, bp::kFetPresence, {7, 8, 9});
  say(s, 300, bp::kAscii, ascii("BRK"));
  CHECK(s.take_output() == [] {
    Bytes expected = bp::encode(bp::kFetPresenceAck, {7, 8, 9});
    const Bytes question = bp::encode(bp::kGetTfeVersion);
    expected.insert(expected.end(), question.begin(), question.end());
    return expected;
  }());
}

TEST_CASE("running: a keep-alive every 15 s, buffers every 30 s, their end acknowledged") {
  bp::Session s;
  nesto::Millis now = handshake(s);
  s.tick(now + 14999);
  CHECK(s.take_output().empty());
  s.tick(now + 15000);
  CHECK(commands(s.take_output()) == Commands{bp::kStatusRequest});
  say(s, now + 15100, bp::kState);
  s.tick(now + 30000);
  CHECK(commands(s.take_output()) == Commands{bp::kStatusRequest, bp::kRequestBuffers});
  say(s, now + 30100, bp::kClimate, {0x9a, 0x08, 0xbb, 0x01});
  say(s, now + 30100, bp::kEndOfBuffers);
  CHECK(commands(s.take_output()) == Commands{bp::kEndOfBuffersAck});
  REQUIRE(s.climate());
  CHECK(s.climate()->celsius == doctest::Approx(22.02));
}

TEST_CASE("power steal mode is only set when asked for") {
  bp::Session::Config config;
  config.power_steal = true;
  bp::Session s(config);
  s.start(0);
  s.tick(250);
  s.take_output();
  say(s, 300, bp::kFetPresence, {1});
  say(s, 300, bp::kAscii, ascii("BRK"));
  say(s, 310, bp::kTfeVersion);
  say(s, 320, bp::kTfeBuildInfo);
  s.take_output();
  say(s, 330, bp::kModel);
  CHECK(commands(s.take_output()) ==
        Commands{bp::kSetPowerSteal, bp::kStatusRequest, bp::kRequestBuffers});
}

TEST_CASE("no BRK, no wire presence or no reply: start again after a pause") {
  SUBCASE("no burst") {
    bp::Session s;
    s.start(0);
    CHECK(s.take_break());
    s.tick(250);
    s.tick(5249);
    CHECK(s.phase() == bp::Session::Phase::kBurst);
    s.tick(5250);
    CHECK(s.phase() == bp::Session::Phase::kRetry);
    CHECK(s.problem() == "no start-up burst ending in BRK");
    s.tick(15249);
    CHECK_FALSE(s.take_break());
    s.tick(15250);
    CHECK(s.phase() == bp::Session::Phase::kBreak);
    CHECK(s.take_break());
    CHECK(s.restarts() == 1);
  }
  SUBCASE("BRK without wire presence") {
    bp::Session s;
    s.start(0);
    s.tick(250);
    say(s, 300, bp::kAscii, ascii("BRK"));
    CHECK(s.phase() == bp::Session::Phase::kRetry);
  }
  SUBCASE("a question unanswered") {
    bp::Session s;
    s.start(0);
    s.tick(250);
    say(s, 300, bp::kFetPresence, {1});
    say(s, 300, bp::kAscii, ascii("BRK"));
    say(s, 400, bp::kTfeVersion);
    s.tick(1399);
    CHECK(s.phase() == bp::Session::Phase::kInfo);
    s.tick(1400);
    CHECK(s.phase() == bp::Session::Phase::kRetry);
  }
}

TEST_CASE("a backplate that goes quiet for two minutes is started again") {
  bp::Session s;
  const nesto::Millis now = handshake(s);
  say(s, now + 60000, bp::kState);
  s.tick(now + 179999);
  CHECK(s.running());
  s.tick(now + 180000);
  CHECK(s.phase() == bp::Session::Phase::kRetry);
  CHECK(s.problem() == "the backplate went quiet");
  s.tick(now + 190000);
  CHECK(s.take_break());
  CHECK(s.handshakes() == 1);
}

TEST_CASE("readings are kept whatever the phase") {
  bp::Session s;
  s.start(0);
  const Bytes state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x70, 0x0f};
  say(s, 10, bp::kState, state);
  REQUIRE(s.power());
  CHECK(s.power()->battery == doctest::Approx(3.952));
}
