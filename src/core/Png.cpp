#include "core/Png.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>

namespace nesto {
namespace {

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t n = 0; n < 256; ++n) {
      std::uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v >> 24));
  out.push_back(static_cast<std::uint8_t>(v >> 16));
  out.push_back(static_cast<std::uint8_t>(v >> 8));
  out.push_back(static_cast<std::uint8_t>(v));
}

void put_chunk(std::vector<std::uint8_t>& out, const char type[4],
               const std::vector<std::uint8_t>& data) {
  put_u32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t type_at = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  put_u32(out, crc32(out.data() + type_at, 4 + data.size()));
}

}  // namespace

std::vector<std::uint8_t> encode_png(const std::uint32_t* pixels, int width, int height) {
  // Raw scanlines: a filter byte (0, none) then RGB triples.
  std::vector<std::uint8_t> raw;
  raw.reserve(static_cast<std::size_t>(height) * (1 + 3 * static_cast<std::size_t>(width)));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    for (int x = 0; x < width; ++x) {
      const std::uint32_t p = pixels[static_cast<std::size_t>(y) * width + x];
      raw.push_back(static_cast<std::uint8_t>(p >> 16));  // red
      raw.push_back(static_cast<std::uint8_t>(p >> 8));   // green
      raw.push_back(static_cast<std::uint8_t>(p));        // blue
    }
  }

  // zlib stream made of stored (uncompressed) deflate blocks.
  std::vector<std::uint8_t> z = {0x78, 0x01};
  std::size_t pos = 0;
  do {
    const std::size_t len = std::min<std::size_t>(65535, raw.size() - pos);
    const bool last = pos + len == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<std::uint8_t>(len));
    z.push_back(static_cast<std::uint8_t>(len >> 8));
    z.push_back(static_cast<std::uint8_t>(~len));
    z.push_back(static_cast<std::uint8_t>(~len >> 8));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
             raw.begin() + static_cast<std::ptrdiff_t>(pos + len));
    pos += len;
  } while (pos < raw.size());
  std::uint32_t a = 1, b = 0;  // Adler-32 of the uncompressed data
  for (std::uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  put_u32(z, (b << 16) | a);

  std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<std::uint8_t> ihdr;
  put_u32(ihdr, static_cast<std::uint32_t>(width));
  put_u32(ihdr, static_cast<std::uint32_t>(height));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit, RGB, deflate, no filter, no interlace
  put_chunk(png, "IHDR", ihdr);
  put_chunk(png, "IDAT", z);
  put_chunk(png, "IEND", {});
  return png;
}

bool write_png(const std::string& path, const std::uint32_t* pixels, int width, int height) {
  const std::vector<std::uint8_t> png = encode_png(pixels, width, height);
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
  return std::fclose(f) == 0 && ok;
}

}  // namespace nesto
