#include <doctest/doctest.h>

#include "core/PressGestures.hpp"

using nesto::Gesture;
using nesto::GestureEvent;
using nesto::PressContext;
using nesto::PressGestures;
using Events = std::vector<GestureEvent>;

namespace nesto {
// Lets doctest print gestures when a check fails.
doctest::String toString(const GestureEvent& e) {
  const char* g = e.gesture == Gesture::Single ? "single" : e.gesture == Gesture::Double ? "double" : "long";
  const char* c = e.context == PressContext::Ui ? "ui" : e.context == PressContext::Standby ? "standby" : "off";
  return doctest::String(g) + " (" + c + ")";
}
}  // namespace nesto

TEST_CASE("in the controls a short press is a single press at once") {
  PressGestures g;
  CHECK(g.on_down(1000, PressContext::Ui).empty());
  CHECK(g.tick(1100).empty());
  CHECK(g.on_up(1120) == Events{{Gesture::Single, PressContext::Ui}});
  CHECK_FALSE(g.busy());
}

TEST_CASE("in the controls two quick presses are two single presses, never a double") {
  PressGestures g;
  g.on_down(0, PressContext::Ui);
  CHECK(g.on_up(80) == Events{{Gesture::Single, PressContext::Ui}});
  CHECK(g.on_down(150, PressContext::Ui).empty());
  CHECK(g.on_up(230) == Events{{Gesture::Single, PressContext::Ui}});
}

TEST_CASE("a long press fires while still held, and its release adds nothing") {
  PressGestures g;
  g.on_down(0, PressContext::Ui);
  CHECK(g.tick(799).empty());
  CHECK(g.tick(800) == Events{{Gesture::Long, PressContext::Ui}});
  CHECK(g.tick(900).empty());
  CHECK(g.on_up(1500).empty());
  CHECK_FALSE(g.busy());
}

TEST_CASE("a long press is reported even if tick() was late, before the release") {
  PressGestures g;
  g.on_down(0, PressContext::Standby);
  CHECK(g.on_up(2000) == Events{{Gesture::Long, PressContext::Standby}});
  CHECK_FALSE(g.busy());
}

TEST_CASE("in standby a single press waits for the double-press window") {
  PressGestures g;
  g.on_down(0, PressContext::Standby);
  CHECK(g.on_up(80).empty());
  CHECK(g.busy());
  CHECK(g.tick(80 + 350).empty());
  CHECK(g.tick(80 + 351) == Events{{Gesture::Single, PressContext::Standby}});
  CHECK_FALSE(g.busy());
}

TEST_CASE("in standby a second press inside the window is a double, at once") {
  PressGestures g;
  g.on_down(0, PressContext::Standby);
  g.on_up(80);
  CHECK(g.on_down(80 + 350, PressContext::Standby) ==
        Events{{Gesture::Double, PressContext::Standby}});
  CHECK(g.on_up(500).empty());
  CHECK(g.tick(2000).empty());
  CHECK_FALSE(g.busy());
}

TEST_CASE("a double press that is held long stays a double") {
  PressGestures g;
  g.on_down(0, PressContext::Off);
  g.on_up(80);
  CHECK(g.on_down(200, PressContext::Off) == Events{{Gesture::Double, PressContext::Off}});
  CHECK(g.tick(1500).empty());
  CHECK(g.on_up(1600).empty());
}

TEST_CASE("a press after the window has passed is a single, then a new gesture") {
  PressGestures g;
  g.on_down(0, PressContext::Off);
  g.on_up(80);
  // tick() wasn't called in time; the late press still reports the single.
  CHECK(g.on_down(1000, PressContext::Off) == Events{{Gesture::Single, PressContext::Off}});
  CHECK(g.busy());
  CHECK(g.on_up(1080).empty());
  CHECK(g.tick(1500) == Events{{Gesture::Single, PressContext::Off}});
}

TEST_CASE("the context is the one at the first press") {
  PressGestures g;
  g.on_down(0, PressContext::Standby);
  g.on_up(80);
  // The panel woke between the presses; the gesture still belongs to standby.
  CHECK(g.on_down(200, PressContext::Ui) == Events{{Gesture::Double, PressContext::Standby}});
}

TEST_CASE("stray edges are ignored") {
  PressGestures g;
  CHECK(g.on_up(10).empty());
  g.on_down(20, PressContext::Ui);
  CHECK(g.on_down(30, PressContext::Ui).empty());
  CHECK(g.on_up(90) == Events{{Gesture::Single, PressContext::Ui}});
}

TEST_CASE("timing can be changed") {
  PressGestures g({/*long_press=*/500, /*double_window=*/200});
  g.on_down(0, PressContext::Ui);
  CHECK(g.tick(500) == Events{{Gesture::Long, PressContext::Ui}});
}
