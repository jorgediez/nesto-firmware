#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nesto::room {

/// Typing with the ring, like the stock software's: the keys sit around the
/// edge, the ring moves the highlight round (and round again), a press types
/// the highlighted one. The first keys do the rest: done, delete, space,
/// shift and the other pages. A long press is for the screen to handle
/// (going back).
class RingKeyboard {
 public:
  enum class Page { Letters, Digits, Symbols, Accents };
  enum class Special { None, Done, Delete, Space, Shift, ToLetters, ToDigits, ToSymbols, ToAccents };

  struct Key {
    std::string text;  // what's shown: the character, or the key's name
    std::string icon;  // the panel's icon name for a special key, if it has one
    Special special = Special::None;
  };

  /// `max_bytes`: the longest text, in UTF-8 bytes (a Wi-Fi name has 32, a
  /// password 63, or 64 hexadecimal digits).
  explicit RingKeyboard(std::size_t max_bytes = 64, std::string text = "");

  /// Moves the highlight, wrapping round.
  void rotate(int clicks);
  /// Presses the highlighted key. Returns true for Done.
  bool press();

  const std::vector<Key>& keys() const { return keys_; }
  int selected() const { return selected_; }
  const std::string& text() const { return text_; }
  Page page() const { return page_; }
  bool shifted() const { return shift_; }

 private:
  void build(bool keep_place);
  void type(const std::string& s);

  std::size_t max_bytes_;
  std::string text_;
  Page page_ = Page::Letters;
  bool shift_ = false;
  std::vector<Key> keys_;
  int selected_ = 0;
};

/// Removes the last character of UTF-8 `text`, however many bytes it takes.
void pop_utf8(std::string& text);

}  // namespace nesto::room
