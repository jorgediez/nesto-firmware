#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "core/Png.hpp"

using nesto::encode_png;

namespace {

std::uint32_t read_u32(const std::vector<std::uint8_t>& b, std::size_t at) {
  return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) |
         (std::uint32_t{b[at + 2]} << 8) | std::uint32_t{b[at + 3]};
}

}  // namespace

TEST_CASE("the PNG has a signature, a header with the size, and ends with IEND") {
  const std::vector<std::uint32_t> pixels(3 * 2, 0x00FF8000);
  const auto png = encode_png(pixels.data(), 3, 2);

  const std::vector<std::uint8_t> signature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  REQUIRE(png.size() > 33);
  CHECK(std::vector<std::uint8_t>(png.begin(), png.begin() + 8) == signature);

  CHECK(read_u32(png, 8) == 13);  // IHDR length
  CHECK(std::string(png.begin() + 12, png.begin() + 16) == "IHDR");
  CHECK(read_u32(png, 16) == 3);  // width
  CHECK(read_u32(png, 20) == 2);  // height
  CHECK(png[24] == 8);            // bit depth
  CHECK(png[25] == 2);            // RGB

  // IEND is empty and its CRC is fixed.
  const std::size_t iend = png.size() - 12;
  CHECK(read_u32(png, iend) == 0);
  CHECK(std::string(png.begin() + static_cast<std::ptrdiff_t>(iend) + 4,
                    png.begin() + static_cast<std::ptrdiff_t>(iend) + 8) == "IEND");
  CHECK(read_u32(png, iend + 8) == 0xAE426082);
}

TEST_CASE("pixels are stored as red, green, blue after each row's filter byte") {
  const std::vector<std::uint32_t> pixels = {0x00112233};
  const auto png = encode_png(pixels.data(), 1, 1);
  // IDAT data: zlib header (2), stored-block header (5), filter byte, RGB.
  const std::size_t idat = 8 + 25;  // after the signature and IHDR chunk
  REQUIRE(std::string(png.begin() + idat + 4, png.begin() + idat + 8) == "IDAT");
  const std::size_t data = idat + 8;
  CHECK(png[data + 7] == 0);  // filter: none
  CHECK(png[data + 8] == 0x11);
  CHECK(png[data + 9] == 0x22);
  CHECK(png[data + 10] == 0x33);
}

TEST_CASE("images larger than one stored block are split into several") {
  // 320x320 RGB is about 300 KB of raw data: more than four 64 KB blocks.
  const std::vector<std::uint32_t> pixels(320 * 320, 0x00FFFFFF);
  const auto png = encode_png(pixels.data(), 320, 320);
  const std::size_t raw = 320 * (1 + 320 * 3);
  CHECK(png.size() > raw);
  CHECK(png.size() < raw + 200);
}
