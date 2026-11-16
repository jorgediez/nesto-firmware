// The ring keyboard: pages, shift, typing, deleting and the limits.
#include <doctest/doctest.h>

#include <algorithm>

#include "room/RingKeyboard.hpp"

using namespace nesto::room;
using Special = RingKeyboard::Special;

namespace {

/// Moves the highlight to the key showing `text`, or with special `s`.
void go_to(RingKeyboard& k, const std::string& text, Special s = Special::None) {
  const auto& keys = k.keys();
  const auto it = std::find_if(keys.begin(), keys.end(), [&](const RingKeyboard::Key& key) {
    return s != Special::None ? key.special == s : key.text == text && key.special == Special::None;
  });
  REQUIRE(it != keys.end());
  k.rotate(static_cast<int>(it - keys.begin()) - k.selected());
}

void type(RingKeyboard& k, const std::string& text) {
  go_to(k, text);
  CHECK_FALSE(k.press());
}

bool tap(RingKeyboard& k, Special s) {
  go_to(k, "", s);
  return k.press();
}

}  // namespace

TEST_CASE("the letters page: the special keys first, the highlight on 'a'") {
  RingKeyboard k;
  const auto& keys = k.keys();
  CHECK(keys[0].special == Special::Done);
  CHECK(keys[1].special == Special::Delete);
  CHECK(keys[2].special == Special::Space);
  CHECK(keys[3].special == Special::Shift);
  CHECK(keys[static_cast<std::size_t>(k.selected())].text == "a");
  CHECK(keys.back().text == "z");
  CHECK(std::none_of(keys.begin(), keys.end(), [](const auto& key) { return key.special == Special::ToLetters; }));
}

TEST_CASE("the ring wraps round both ways") {
  RingKeyboard k;
  const int n = static_cast<int>(k.keys().size());
  k.rotate(-k.selected());
  CHECK(k.selected() == 0);
  k.rotate(-1);
  CHECK(k.selected() == n - 1);
  k.rotate(1);
  CHECK(k.selected() == 0);
  k.rotate(3 * n + 2);
  CHECK(k.selected() == 2);
}

TEST_CASE("typing, shift for one letter, space and delete") {
  RingKeyboard k;
  type(k, "h");
  tap(k, Special::Shift);
  CHECK(k.shifted());
  type(k, "I");
  CHECK_FALSE(k.shifted());  // back to lower case
  tap(k, Special::Space);
  CHECK(k.text() == "hI ");
  tap(k, Special::Delete);
  tap(k, Special::Delete);
  CHECK(k.text() == "h");
  CHECK(tap(k, Special::Done));
}

TEST_CASE("the other pages: digits, symbols and accents, which take two bytes") {
  RingKeyboard k;
  tap(k, Special::ToDigits);
  CHECK(k.page() == RingKeyboard::Page::Digits);
  CHECK(k.keys()[static_cast<std::size_t>(k.selected())].text == "0");
  type(k, "7");
  tap(k, Special::ToSymbols);
  type(k, "#");
  tap(k, Special::ToAccents);
  type(k, "\xC3\xB1");  // ñ
  tap(k, Special::Shift);
  type(k, "\xC3\x89");  // É
  CHECK(k.text() == "7#\xC3\xB1\xC3\x89");
  tap(k, Special::Delete);
  CHECK(k.text() == "7#\xC3\xB1");  // the whole character
  tap(k, Special::ToLetters);
  CHECK(k.page() == RingKeyboard::Page::Letters);
}

TEST_CASE("the text stops at its limit, in bytes") {
  RingKeyboard k(3, "ab");
  type(k, "c");
  type(k, "d");
  CHECK(k.text() == "abc");
  RingKeyboard accents(3, "ab");
  tap(accents, Special::ToAccents);
  type(accents, "\xC3\xA1");  // two bytes don't fit in one
  CHECK(accents.text() == "ab");
}

TEST_CASE("deleting UTF-8 a character at a time") {
  std::string s = "a\xE2\x82\xAC";  // a€
  pop_utf8(s);
  CHECK(s == "a");
  pop_utf8(s);
  CHECK(s.empty());
  pop_utf8(s);
  CHECK(s.empty());
}
