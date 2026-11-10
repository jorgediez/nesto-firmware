/*
 * LVGL configuration for Nesto. Anything not set here takes LVGL's default
 * from lv_conf_internal.h.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* The Nest's framebuffer is 32 bits per pixel (BGRA in memory), so LVGL
 * renders straight into that layout. */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_XRGB8888

/* Use the C library's allocator: the device runs Linux, so there's no need for
 * LVGL's own fixed-size heap. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE

/* About 30 frames per second is plenty for a 320x320 software renderer on a
 * Cortex-A8. */
#define LV_DEF_REFR_PERIOD 33

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 0

/* Text and icons are TrueType fonts drawn at runtime (see app/Fonts.cpp), so
 * any size and every letter the embedded fonts carry are available. Each size
 * caches the glyphs it has drawn. */
#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_FILE_SUPPORT 0
#define LV_TINY_TTF_CACHE_GLYPH_CNT 256

/* One small built-in font, only as a last resort if the embedded ones fail. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* No drivers from LVGL: Nesto's own hardware layer feeds LVGL. */
#define LV_USE_SDL 0
#define LV_USE_LINUX_FBDEV 0
#define LV_USE_EVDEV 0

#endif /* LV_CONF_H */
