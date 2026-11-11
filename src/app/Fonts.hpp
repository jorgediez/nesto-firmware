#pragma once

#include "lvgl.h"

namespace nesto {

/// The panel's fonts at the sizes the screens use, drawn from the embedded
/// TrueType fonts (app/FontData.hpp). Create after lv_init() and destroy before
/// lv_deinit(). If a size can't be created, LVGL's built-in font stands in.
struct Fonts {
  const lv_font_t* small = nullptr;      // 14 px: hints, status, summaries
  const lv_font_t* body = nullptr;       // 20 px: titles, messages
  const lv_font_t* large = nullptr;      // 28 px: the selected item's name
  const lv_font_t* huge = nullptr;       // 48 px: values, the pairing code
  const lv_font_t* clock = nullptr;      // 80 px: the digital standby clock
  const lv_font_t* icon_small = nullptr; // 28 px icons
  const lv_font_t* icon_large = nullptr; // 56 px icons

  static Fonts load();
  void destroy();
};

}  // namespace nesto
