#include "app/Fonts.hpp"

#include "app/FontData.hpp"
#include "core/Log.hpp"

namespace nesto {
namespace {

const lv_font_t* make(const unsigned char* data, std::size_t size, int pixels) {
  lv_font_t* font = lv_tiny_ttf_create_data(data, size, pixels);
  if (!font) {
    log::error("can't create a %d px font; using the built-in one", pixels);
    return &lv_font_montserrat_14;
  }
  return font;
}

void release(const lv_font_t*& font) {
  if (font && font != &lv_font_montserrat_14) {
    lv_tiny_ttf_destroy(const_cast<lv_font_t*>(font));
  }
  font = nullptr;
}

}  // namespace

Fonts Fonts::load() {
  Fonts f;
  f.small = make(fonts::text_ttf, fonts::text_ttf_size, 14);
  f.body = make(fonts::text_ttf, fonts::text_ttf_size, 20);
  f.large = make(fonts::text_ttf, fonts::text_ttf_size, 28);
  f.huge = make(fonts::text_ttf, fonts::text_ttf_size, 48);
  f.clock = make(fonts::text_ttf, fonts::text_ttf_size, 80);
  f.icon_small = make(fonts::icons_ttf, fonts::icons_ttf_size, 28);
  f.icon_large = make(fonts::icons_ttf, fonts::icons_ttf_size, 56);
  return f;
}

void Fonts::destroy() {
  release(small);
  release(body);
  release(large);
  release(huge);
  release(clock);
  release(icon_small);
  release(icon_large);
}

}  // namespace nesto
