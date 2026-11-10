#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nesto {

/// Encodes 32-bit XRGB pixels (LVGL's XRGB8888, as stored in memory: blue,
/// green, red, unused) as an RGB PNG. The image data is stored uncompressed:
/// these files are screenshots for development, so simplicity wins over size.
std::vector<std::uint8_t> encode_png(const std::uint32_t* pixels, int width, int height);

/// Writes encode_png's output to a file. Returns false if it can't.
bool write_png(const std::string& path, const std::uint32_t* pixels, int width, int height);

}  // namespace nesto
