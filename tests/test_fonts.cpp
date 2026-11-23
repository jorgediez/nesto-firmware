// The embedded fonts: real TrueType data, and every icon the panel can ask for.
#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "app/FontData.hpp"
#include "room/Controller.hpp"

using namespace nesto;

namespace {

bool is_truetype(const unsigned char* data, std::size_t size) {
  return size > 12 && data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x00;
}

/// A glyph from the icon font: one 4-byte UTF-8 sequence in Unicode's private
/// use planes, where Material Design Icons lives.
bool is_icon(const char* utf8) {
  const auto* b = reinterpret_cast<const unsigned char*>(utf8);
  return std::strlen(utf8) == 4 && b[0] == 0xF3 && (b[1] & 0xC0) == 0x80;
}

}  // namespace

TEST_CASE("both fonts are TrueType and cut down to a small size") {
  CHECK(is_truetype(fonts::text_ttf, fonts::text_ttf_size));
  CHECK(is_truetype(fonts::icons_ttf, fonts::icons_ttf_size));
  CHECK(fonts::text_ttf_size < 120 * 1024);
  CHECK(fonts::icons_ttf_size < 40 * 1024);
}

TEST_CASE("every icon of PROTOCOL.md appendix C is in the icon font") {
  for (const char* name :
       {"lightbulb", "lightbulb-group", "ceiling-light", "lamp", "led-strip", "switch",
        "power-socket", "fan", "scene", "script", "thermostat", "radiator", "air-conditioner",
        "blind", "shutter", "curtains", "garage", "window", "door", "sensor", "thermometer",
        "water", "tv", "speaker", "bell", "alert", "info", "lock", "lock-open"}) {
    CAPTURE(name);
    CHECK(is_icon(fonts::icon(name)));
  }
}

TEST_CASE("the panel's own icons and every kind's default are in the icon font") {
  for (const char* name : {"settings", "pair", "forget", "offline", "unknown"}) {
    CAPTURE(name);
    CHECK(is_icon(fonts::icon(name)));
  }
  for (room::Kind kind : {room::Kind::Light, room::Kind::Switch, room::Kind::Scene,
                          room::Kind::Climate, room::Kind::Cover, room::Kind::Sensor,
                          room::Kind::Fan, room::Kind::Lock}) {
    CHECK(is_icon(fonts::icon(room::default_icon(kind))));
  }
}

TEST_CASE("an unknown icon name gives nothing, so the screen falls back") {
  CHECK(std::string(fonts::icon("no-such-icon")).empty());
  CHECK(std::string(fonts::icon("")).empty());
}
