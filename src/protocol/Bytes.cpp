#include "protocol/Bytes.hpp"

namespace nesto::protocol {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string base64_encode(const std::uint8_t* data, std::size_t size) {
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < size; i += 3) {
    const std::uint32_t v = (std::uint32_t{data[i]} << 16) | (std::uint32_t{data[i + 1]} << 8) |
                            data[i + 2];
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  if (i + 1 == size) {
    const std::uint32_t v = std::uint32_t{data[i]} << 16;
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == size) {
    const std::uint32_t v = (std::uint32_t{data[i]} << 16) | (std::uint32_t{data[i + 1]} << 8);
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

std::optional<Bytes> base64_decode(std::string_view text) {
  if (text.size() % 4 != 0) return std::nullopt;
  Bytes out;
  out.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    const bool last = i + 4 == text.size();
    const std::size_t padding = !last ? 0 : text[i + 3] != '=' ? 0 : text[i + 2] != '=' ? 1 : 2;
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < 4; ++k) {
      const char c = text[i + k];
      int d = 0;
      if (k >= 4 - padding) {
        if (c != '=') return std::nullopt;
      } else {
        d = base64_value(c);
        if (d < 0) return std::nullopt;
      }
      v = (v << 6) | static_cast<std::uint32_t>(d);
    }
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    if (padding < 2) out.push_back(static_cast<std::uint8_t>(v >> 8));
    if (padding < 1) out.push_back(static_cast<std::uint8_t>(v));
    // Bits that padding hides must be zero, so each value has one encoding.
    if ((padding == 1 && (v & 0xFF) != 0) || (padding == 2 && (v & 0xFFFF) != 0)) {
      return std::nullopt;
    }
  }
  return out;
}

std::string hex_encode(const std::uint8_t* data, std::size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out += kDigits[data[i] >> 4];
    out += kDigits[data[i] & 15];
  }
  return out;
}

std::optional<Bytes> hex_decode(std::string_view text) {
  if (text.size() % 2 != 0) return std::nullopt;
  Bytes out;
  out.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int hi = hex_value(text[i]);
    const int lo = hex_value(text[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out.push_back(static_cast<std::uint8_t>(hi << 4 | lo));
  }
  return out;
}

}  // namespace nesto::protocol
