#include "core/InputScript.hpp"

#include <charconv>

namespace nesto {
namespace {

constexpr Millis kTurnTime = 50;
constexpr Millis kPressDown = 80;
constexpr Millis kHoldDown = 900;
constexpr Millis kAfterRelease = 120;

bool parse_number(std::string_view text, long& value) {
  if (text.empty()) return false;
  const char* end = text.data() + text.size();
  auto [ptr, ec] = std::from_chars(text.data(), end, value);
  return ec == std::errc() && ptr == end;
}

}  // namespace

std::optional<InputScript> parse_input_script(std::string_view script, std::string& error) {
  InputScript out;
  Millis& t = out.duration;

  std::size_t start = 0;
  while (start < script.size()) {
    const std::size_t end = script.find_first_of(" ,\t\n", start);
    const std::size_t stop = end == std::string_view::npos ? script.size() : end;
    const std::string_view step = script.substr(start, stop - start);
    start = stop + 1;
    if (step.empty()) continue;

    const char kind = step.front();
    const std::string_view arg = step.substr(1);
    long value = 0;

    if (kind == 'r' && arg.size() >= 2 && (arg.front() == '+' || arg.front() == '-') &&
        parse_number(arg.substr(1), value) && value > 0) {
      const int steps = static_cast<int>(arg.front() == '-' ? -value : value);
      out.events.push_back({t, RingTurn{steps * kTicksPerClick}});
      t += kTurnTime;
    } else if ((kind == 'p' || kind == 'h') && arg.empty()) {
      out.events.push_back({t, ButtonEdge{true}});
      t += kind == 'p' ? kPressDown : kHoldDown;
      out.events.push_back({t, ButtonEdge{false}});
      t += kAfterRelease;
    } else if (kind == 'm' && arg.empty()) {
      out.events.push_back({t, Presence{}});
      t += kTurnTime;
    } else if (kind == 'l' && parse_number(arg, value) && value >= 0) {
      out.events.push_back({t, Light{static_cast<int>(value)}});
    } else if (kind == 'w' && parse_number(arg, value) && value >= 0) {
      t += static_cast<Millis>(value);
    } else {
      error = "unknown step '" + std::string(step) + "'";
      return std::nullopt;
    }
  }
  return out;
}

}  // namespace nesto
