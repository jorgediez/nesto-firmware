#include <doctest/doctest.h>

#include "protocol/Frame.hpp"

using namespace nesto::protocol;

TEST_CASE("a frame is a big-endian length followed by the payload") {
  const Bytes frame = encode_frame({'a', 'b', 'c'});
  CHECK(frame == Bytes{0, 0, 0, 3, 'a', 'b', 'c'});
}

TEST_CASE("the reader returns frames however the bytes are split") {
  Bytes stream = encode_frame({'h', 'i'});
  const Bytes second = encode_frame(Bytes(300, 'x'));
  stream.insert(stream.end(), second.begin(), second.end());

  for (std::size_t piece : {std::size_t{1}, std::size_t{3}, std::size_t{5}, stream.size()}) {
    CAPTURE(piece);
    FrameReader reader;
    std::vector<Bytes> got;
    for (std::size_t at = 0; at < stream.size(); at += piece) {
      reader.feed(stream.data() + at, std::min(piece, stream.size() - at));
      while (auto f = reader.next()) got.push_back(*f);
    }
    CHECK_FALSE(reader.failed());
    REQUIRE(got.size() == 2);
    CHECK(got[0] == Bytes{'h', 'i'});
    CHECK(got[1] == Bytes(300, 'x'));
  }
}

TEST_CASE("an incomplete frame isn't returned until the rest arrives") {
  FrameReader reader;
  reader.feed(Bytes{0, 0, 0, 4, 'a', 'b'});
  CHECK_FALSE(reader.next());
  reader.feed(Bytes{'c', 'd'});
  CHECK(reader.next() == Bytes{'a', 'b', 'c', 'd'});
}

TEST_CASE("a zero length is a protocol error, and the reader stays failed") {
  FrameReader reader;
  reader.feed(Bytes{0, 0, 0, 0});
  CHECK(reader.failed());
  reader.feed(encode_frame({'o', 'k'}));
  CHECK_FALSE(reader.next());
}

TEST_CASE("frames up to the limit are accepted and larger ones refused") {
  FrameReader ok;
  const Bytes largest(kMaxFrame, 1);
  ok.feed(encode_frame(largest));
  CHECK_FALSE(ok.failed());
  CHECK(ok.next() == largest);

  FrameReader too_big;
  const std::size_t n = kMaxFrame + 1;
  too_big.feed(Bytes{static_cast<std::uint8_t>(n >> 24), static_cast<std::uint8_t>(n >> 16),
                     static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n)});
  CHECK(too_big.failed());
  CHECK(too_big.error().find("out of range") != std::string::npos);
}
