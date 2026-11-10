#pragma once

#include <cstddef>

// The panel's fonts, cut down and embedded at build time by
// tools/make_fonts.py (the source file is generated into the build directory).
namespace nesto::fonts {

extern const unsigned char text_ttf[];
extern const std::size_t text_ttf_size;
extern const unsigned char icons_ttf[];
extern const std::size_t icons_ttf_size;

/// The UTF-8 text that draws icon `name` in the icon font, or "" if there is
/// no such icon. Names are those of PROTOCOL.md appendix C, plus "settings",
/// "pair", "forget", "offline" and "unknown".
const char* icon(const char* name);

}  // namespace nesto::fonts
