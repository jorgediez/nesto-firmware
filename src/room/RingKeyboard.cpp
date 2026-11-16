#include "room/RingKeyboard.hpp"

#include <algorithm>

namespace nesto::room {
namespace {

// The characters of each page, one key each (UTF-8). The accents page has
// what Latin-1 and Latin Extended-A give the panel's font, the common ones.
const char* const kLower[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
                              "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
const char* const kDigits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "-", "/", ":",
                               ";", "(", ")", "$", "&", "@", "\"", ".", ",", "?", "!", "'"};
const char* const kSymbols[] = {"[", "]", "{", "}", "#", "%", "^", "*", "+", "=", "_",
                                "\\", "|", "~", "<", ">", "`", "\xE2\x82\xAC", "\xC2\xA3", "\xC2\xA5"};
const char* const kAccentsLower[] = {"\xC3\xA1", "\xC3\xA0", "\xC3\xA2", "\xC3\xA4", "\xC3\xA3", "\xC3\xA5",
                                     "\xC3\xA6", "\xC3\xA7", "\xC3\xA9", "\xC3\xA8", "\xC3\xAA", "\xC3\xAB",
                                     "\xC3\xAD", "\xC3\xAC", "\xC3\xAE", "\xC3\xAF", "\xC3\xB1", "\xC3\xB3",
                                     "\xC3\xB2", "\xC3\xB4", "\xC3\xB6", "\xC3\xB5", "\xC3\xB8", "\xC3\xBA",
                                     "\xC3\xB9", "\xC3\xBB", "\xC3\xBC", "\xC3\xBD", "\xC3\xBF", "\xC3\x9F"};
const char* const kAccentsUpper[] = {"\xC3\x81", "\xC3\x80", "\xC3\x82", "\xC3\x84", "\xC3\x83", "\xC3\x85",
                                     "\xC3\x86", "\xC3\x87", "\xC3\x89", "\xC3\x88", "\xC3\x8A", "\xC3\x8B",
                                     "\xC3\x8D", "\xC3\x8C", "\xC3\x8E", "\xC3\x8F", "\xC3\x91", "\xC3\x93",
                                     "\xC3\x92", "\xC3\x94", "\xC3\x96", "\xC3\x95", "\xC3\x98", "\xC3\x9A",
                                     "\xC3\x99", "\xC3\x9B", "\xC3\x9C", "\xC3\x9D", "\xC5\xB8", "\xC3\x9F"};

}  // namespace

void pop_utf8(std::string& text) {
  if (text.empty()) return;
  std::size_t i = text.size() - 1;
  // Back over continuation bytes (10xxxxxx) to the character's first.
  while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) --i;
  text.erase(i);
}

RingKeyboard::RingKeyboard(std::size_t max_bytes, std::string text)
    : max_bytes_(max_bytes), text_(std::move(text)) {
  build(false);
}

void RingKeyboard::build(bool keep_place) {
  keys_.clear();
  keys_.push_back({"OK", "check", Special::Done});
  keys_.push_back({"Delete", "backspace", Special::Delete});
  keys_.push_back({"Space", "space", Special::Space});
  if (page_ == Page::Letters || page_ == Page::Accents) keys_.push_back({"Shift", "shift", Special::Shift});
  if (page_ != Page::Letters) keys_.push_back({"abc", "", Special::ToLetters});
  if (page_ != Page::Digits) keys_.push_back({"123", "", Special::ToDigits});
  if (page_ != Page::Symbols) keys_.push_back({"#+=", "", Special::ToSymbols});
  if (page_ != Page::Accents) keys_.push_back({"\xC3\xA0\xC3\xA9", "", Special::ToAccents});
  const std::size_t first = keys_.size();
  const auto add = [&](const char* const* chars, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) keys_.push_back({chars[i], "", Special::None});
  };
  switch (page_) {
    case Page::Letters:
      for (const char* c : kLower) {
        std::string s = c;
        if (shift_) s[0] = static_cast<char>(s[0] - 'a' + 'A');
        keys_.push_back({s, "", Special::None});
      }
      break;
    case Page::Digits: add(kDigits, std::size(kDigits)); break;
    case Page::Symbols: add(kSymbols, std::size(kSymbols)); break;
    case Page::Accents:
      if (shift_) {
        add(kAccentsUpper, std::size(kAccentsUpper));
      } else {
        add(kAccentsLower, std::size(kAccentsLower));
      }
      break;
  }
  // Shifting keeps the highlight where it is; a new page starts on its
  // first character.
  if (keep_place) {
    selected_ = std::clamp(selected_, 0, static_cast<int>(keys_.size()) - 1);
  } else {
    selected_ = static_cast<int>(first);
  }
}

void RingKeyboard::rotate(int clicks) {
  const int n = static_cast<int>(keys_.size());
  selected_ = ((selected_ + clicks) % n + n) % n;
}

void RingKeyboard::type(const std::string& s) {
  if (text_.size() + s.size() <= max_bytes_) text_ += s;
}

bool RingKeyboard::press() {
  const Key key = keys_[static_cast<std::size_t>(selected_)];
  switch (key.special) {
    case Special::Done: return true;
    case Special::Delete: pop_utf8(text_); return false;
    case Special::Space: type(" "); return false;
    case Special::Shift:
      shift_ = !shift_;
      build(true);
      return false;
    case Special::ToLetters: page_ = Page::Letters; break;
    case Special::ToDigits: page_ = Page::Digits; break;
    case Special::ToSymbols: page_ = Page::Symbols; break;
    case Special::ToAccents: page_ = Page::Accents; break;
    case Special::None:
      type(key.text);
      // Shift is for one letter, as on a phone.
      if (shift_) {
        shift_ = false;
        build(true);
      }
      return false;
  }
  shift_ = false;
  build(false);
  return false;
}

}  // namespace nesto::room
