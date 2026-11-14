#include "app/Ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "app/FontData.hpp"
#include "hal/Hal.hpp"

namespace nesto {
namespace {

using room::Tone;
using Screen = room::RoomView::Screen;

const lv_color_t kBackground = lv_color_hex(0x000000);
const lv_color_t kText = lv_color_hex(0xFFFFFF);
const lv_color_t kMuted = lv_color_hex(0x7A7A7A);
const lv_color_t kDim = lv_color_hex(0x4A4A4A);
const lv_color_t kTrack = lv_color_hex(0x262626);
const lv_color_t kAccent = lv_color_hex(0xFF8A2A);
// The standby clock is a little softer than white, so it reads at standby
// brightness without glowing in a dark room.
const lv_color_t kClock = lv_color_hex(0xC8C8C8);

constexpr int kCentre = kScreenSize / 2;

constexpr std::uint32_t kToastTime = 1500;
constexpr std::int32_t kArcScale = 1000;
// A notification's message: four lines this wide fit inside the round screen.
constexpr std::int32_t kNoticeWidth = 236;
constexpr std::int32_t kNoticeLines = 4;

lv_color_t colour(Tone tone) {
  switch (tone) {
    case Tone::Off: return lv_color_hex(0x6E6E6E);
    case Tone::Neutral: return lv_color_hex(0xE6E6E6);
    case Tone::Light: return lv_color_hex(0xFFC24B);
    case Tone::Heat: return lv_color_hex(0xFF7A33);
    case Tone::Cool: return lv_color_hex(0x4AA8FF);
    case Tone::Active: return lv_color_hex(0x4CD18A);
  }
  return kText;
}

/// The arc's colour: a neutral tone would be white, which reads as a highlight,
/// so menus and idle things get the panel's accent instead.
lv_color_t arc_colour(Tone tone) {
  return tone == Tone::Neutral ? kAccent : colour(tone);
}

std::string icon_text(const std::string& name, const std::string& fallback) {
  const char* glyph = fonts::icon(name.c_str());
  if (!*glyph) glyph = fonts::icon(fallback.c_str());
  if (!*glyph) glyph = fonts::icon("unknown");
  return glyph;
}

/// Keeps a label to one line of `font`: longer text ends in an ellipsis.
void one_line(lv_obj_t* label, const lv_font_t* font) {
  lv_obj_set_height(label, lv_font_get_line_height(font));
}

/// A centred label. With a width and `lines` above 0, text that doesn't fit in
/// that many lines ends in an ellipsis; with `lines` 0 it wraps freely.
lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t colour,
                     std::int32_t width = 0, int lines = 1) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, colour, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  if (width > 0) {
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    if (lines > 0) lv_obj_set_height(label, lv_font_get_line_height(font) * lines);
  }
  return label;
}

void set_text(lv_obj_t* label, const std::string& text) {
  lv_label_set_text(label, text.c_str());
}

/// The widest of the font's digits.
std::int32_t widest_digit(const lv_font_t* font) {
  std::int32_t digit = 0;
  for (char d = '0'; d <= '9'; ++d) {
    digit = std::max<std::int32_t>(digit, lv_font_get_glyph_width(font, static_cast<std::uint32_t>(d), 0));
  }
  return digit;
}

/// The width of `text` in `font`, with 2 px for kerning's slack.
std::int32_t text_width(const lv_font_t* font, const std::string& text) {
  std::int32_t width = 2;
  for (char c : text) width += lv_font_get_glyph_width(font, static_cast<unsigned char>(c), 0);
  return width;
}

/// A row of one-character labels for a clock's numbers: tabular figures.
template <std::size_t N>
lv_obj_t* make_cells(lv_obj_t* parent, const lv_font_t* font, lv_color_t colour,
                     std::array<lv_obj_t*, N>& cells) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  for (lv_obj_t*& cell : cells) {
    cell = make_label(row, font, colour);
    lv_label_set_long_mode(cell, LV_LABEL_LONG_MODE_CLIP);
  }
  return row;
}

/// Shows `text` in the cells: each digit centred in a cell as wide as the
/// widest digit, other characters at their own width. The font's digits
/// differ in width ("1" is narrow), so with this nothing moves as the time
/// changes.
template <std::size_t N>
void set_cells(std::array<lv_obj_t*, N>& cells, const lv_font_t* font, const std::string& text) {
  const std::int32_t digit = widest_digit(font);
  for (std::size_t i = 0; i < N; ++i) {
    lv_obj_t* cell = cells[i];
    lv_obj_set_hidden(cell, i >= text.size());
    if (i >= text.size()) continue;
    const char c = text[i];
    set_text(cell, std::string(1, c));
    lv_obj_set_width(cell, c >= '0' && c <= '9' ? digit : LV_SIZE_CONTENT);
  }
}

}  // namespace

Ui::Ui(const Fonts& fonts) : fonts_(fonts) {
  lv_obj_t* screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, kBackground, 0);
  lv_obj_set_scrollable(screen, false);

  // Around the edge: where the selection is in the list, or the item's value.
  arc_ = lv_arc_create(screen);
  lv_obj_set_size(arc_, 300, 300);
  lv_obj_center(arc_);
  lv_arc_set_bg_angles(arc_, 135, 45);
  lv_obj_remove_style(arc_, nullptr, LV_PART_KNOB);
  lv_obj_set_clickable(arc_, false);
  lv_obj_set_style_arc_width(arc_, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc_, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc_, kTrack, LV_PART_MAIN);

  title_ = make_label(screen, fonts_.small, kMuted, 180);
  lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 34);

  previous_ = make_label(screen, fonts_.small, kDim, 160);
  lv_obj_align(previous_, LV_ALIGN_CENTER, 0, -84);
  icon_ = make_label(screen, fonts_.icon_large, kText);
  lv_obj_align(icon_, LV_ALIGN_CENTER, 0, -40);
  name_ = make_label(screen, fonts_.large, kText, 236);
  lv_obj_align(name_, LV_ALIGN_CENTER, 0, 16);
  summary_ = make_label(screen, fonts_.small, kMuted, 220);
  lv_obj_align(summary_, LV_ALIGN_CENTER, 0, 48);
  next_ = make_label(screen, fonts_.small, kDim, 160);
  lv_obj_align(next_, LV_ALIGN_CENTER, 0, 82);

  control_ = make_label(screen, fonts_.small, kMuted, 200);
  lv_obj_align(control_, LV_ALIGN_CENTER, 0, -46);
  value_ = make_label(screen, fonts_.huge, kText, 236);
  lv_obj_align(value_, LV_ALIGN_CENTER, 0, 4);

  centre_ = make_label(screen, fonts_.body, kText);
  lv_obj_center(centre_);

  // A notification: icon and title above, the message in a box of four lines
  // that the ring scrolls, and the choice below.
  notice_icon_ = make_label(screen, fonts_.icon_small, kText);
  lv_obj_align(notice_icon_, LV_ALIGN_CENTER, 0, -86);
  notice_title_ = make_label(screen, fonts_.body, kText, 220);
  lv_obj_align(notice_title_, LV_ALIGN_CENTER, 0, -52);
  const std::int32_t line = lv_font_get_line_height(fonts_.small);
  notice_box_ = lv_obj_create(screen);
  lv_obj_remove_style_all(notice_box_);
  lv_obj_set_size(notice_box_, kNoticeWidth, line * kNoticeLines);
  lv_obj_set_scrollable(notice_box_, false);
  lv_obj_align(notice_box_, LV_ALIGN_CENTER, 0, 6);
  notice_message_ = lv_label_create(notice_box_);
  lv_obj_set_style_text_font(notice_message_, fonts_.small, 0);
  lv_obj_set_style_text_color(notice_message_, kText, 0);
  lv_obj_set_style_text_align(notice_message_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(notice_message_, kNoticeWidth);
  lv_label_set_long_mode(notice_message_, LV_LABEL_LONG_MODE_WRAP);
  notice_choice_ = make_label(screen, fonts_.body, kAccent, 200);
  lv_obj_align(notice_choice_, LV_ALIGN_CENTER, 0, 72);

  footer_ = make_label(screen, fonts_.small, kMuted, 200, 0);  // Confirm's prompt takes two lines
  lv_obj_align(footer_, LV_ALIGN_BOTTOM_MID, 0, -40);

  toast_ = make_label(screen, fonts_.small, kText);
  lv_obj_set_style_bg_color(toast_, kTrack, 0);
  lv_obj_set_style_bg_opa(toast_, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(toast_, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(toast_, 14, 0);
  lv_obj_set_style_pad_ver(toast_, 6, 0);
  lv_obj_align(toast_, LV_ALIGN_BOTTOM_MID, 0, -36);
  lv_obj_set_hidden(toast_, true);

  build_clock(screen);

  // The pairing screen covers everything while it's shown.
  pairing_ = lv_obj_create(screen);
  lv_obj_remove_style_all(pairing_);
  lv_obj_set_size(pairing_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(pairing_, kBackground, 0);
  lv_obj_set_style_bg_opa(pairing_, LV_OPA_COVER, 0);
  lv_obj_t* pair_icon = make_label(pairing_, fonts_.icon_small, kAccent);
  set_text(pair_icon, fonts::icon("pair"));
  lv_obj_align(pair_icon, LV_ALIGN_CENTER, 0, -86);
  lv_obj_t* heading = make_label(pairing_, fonts_.body, kMuted);
  set_text(heading, "Pairing code");
  lv_obj_align(heading, LV_ALIGN_CENTER, 0, -50);
  pairing_code_ = make_label(pairing_, fonts_.huge, kAccent);
  lv_obj_align(pairing_code_, LV_ALIGN_CENTER, 0, 4);
  lv_obj_t* hint = make_label(pairing_, fonts_.small, kMuted);
  set_text(hint, "Enter it in\nHome Assistant");
  lv_obj_align(hint, LV_ALIGN_CENTER, 0, 64);
  lv_obj_set_hidden(pairing_, true);

  render(view_);
}

Ui::~Ui() {
  if (toast_timer_) lv_timer_delete(toast_timer_);
}

void Ui::render(const room::RoomView& view) {
  view_ = view;
  const bool list = (view.screen == Screen::Home || view.screen == Screen::Settings ||
                     view.screen == Screen::Notifications) &&
                    !view.entries.empty();
  const bool item = view.screen == Screen::Item;
  const bool notice = view.screen == Screen::Notice;

  // Title: the room, the item, or the menu.
  std::string title = "Nesto";
  if (view.screen == Screen::Home) title = view.title.empty() ? "Home Assistant" : view.title;
  if (view.screen != Screen::Home && view.screen != Screen::Empty) {
    title = item ? view.item_name : view.title;
  }
  set_text(title_, title);
  lv_obj_set_style_text_font(title_, item ? fonts_.body : fonts_.small, 0);
  one_line(title_, item ? fonts_.body : fonts_.small);
  lv_obj_set_style_text_color(title_, item ? kText : kMuted, 0);
  lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, item ? 48 : 34);

  for (lv_obj_t* o : {previous_, icon_, name_, summary_, next_}) lv_obj_set_hidden(o, !list);
  if (list) render_list(view);

  lv_obj_set_hidden(control_, !item);
  lv_obj_set_hidden(value_, !item);
  if (item) {
    set_text(control_, view.control);
    set_text(value_, view.value.empty() ? "-" : view.value);
    const bool live = view.available && !view.offline && view.tone != Tone::Off;
    lv_obj_set_style_text_color(value_, live ? kText : kMuted, 0);
  }

  for (lv_obj_t* o : {notice_icon_, notice_title_, notice_box_, notice_choice_}) {
    lv_obj_set_hidden(o, !notice);
  }
  notice_scroll_limit_ = 0;
  if (notice) render_notice(view);

  // A message in the middle: waiting, a question, or About's lines.
  std::string centre;
  if (view.screen == Screen::Empty) centre = "Waiting for\nHome Assistant";
  if (view.screen == Screen::Confirm) centre = view.item_name;
  if (view.screen == Screen::About) {
    for (const auto& line : view.lines) centre += (centre.empty() ? "" : "\n") + line;
  }
  lv_obj_set_hidden(centre_, centre.empty());
  lv_obj_set_style_text_font(centre_, view.screen == Screen::About ? fonts_.small : fonts_.body,
                             0);
  set_text(centre_, centre);

  // The arc: the position in the list, or the item's value as a gauge.
  bool arc_shown = false;
  Tone arc_tone = Tone::Neutral;
  if (list && view.entries.size() > 1) {
    lv_arc_set_range(arc_, 0, static_cast<std::int32_t>(view.entries.size()) - 1);
    lv_arc_set_value(arc_, view.selected);
    arc_tone = view.entries[static_cast<std::size_t>(view.selected)].tone;
    arc_shown = true;
  } else if (item && view.fraction >= 0) {
    lv_arc_set_range(arc_, 0, kArcScale);
    lv_arc_set_value(arc_, static_cast<std::int32_t>(std::lround(view.fraction * kArcScale)));
    arc_tone = view.tone;
    arc_shown = true;
  } else if (notice && view.choices.size() > 1) {
    lv_arc_set_range(arc_, 0, static_cast<std::int32_t>(view.choices.size()) - 1);
    lv_arc_set_value(arc_, view.choice);
    arc_tone = view.tone;
    arc_shown = true;
  }
  lv_obj_set_style_arc_opa(arc_, arc_shown ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc_, view.offline ? kMuted : arc_colour(arc_tone),
                             LV_PART_INDICATOR);

  update_footer();
}

void Ui::render_notice(const room::RoomView& view) {
  set_text(notice_icon_, icon_text(view.icon, view.default_icon));
  lv_obj_set_style_text_color(notice_icon_, colour(view.tone), 0);
  set_text(notice_title_, view.item_name);
  lv_obj_set_hidden(notice_title_, view.item_name.empty());

  // The message scrolls a line at a time inside its box.
  set_text(notice_message_, view.message);
  lv_obj_update_layout(notice_message_);
  const std::int32_t line = lv_font_get_line_height(fonts_.small);
  const std::int32_t lines = (lv_obj_get_height(notice_message_) + line - 1) / line;
  notice_scroll_limit_ = std::max<std::int32_t>(0, lines - kNoticeLines);
  const int scroll = std::min(view.scroll, notice_scroll_limit_);
  // A short message sits in the middle of the box; a long one from its top.
  const std::int32_t free = std::max<std::int32_t>(0, (kNoticeLines - lines) * line);
  lv_obj_set_pos(notice_message_, 0, free / 2 - scroll * line);

  const auto at = static_cast<std::size_t>(std::clamp<int>(view.choice, 0, static_cast<int>(view.choices.size()) - 1));
  set_text(notice_choice_, view.choices.empty() ? "" : view.choices[at]);
}

void Ui::render_list(const room::RoomView& view) {
  const auto at = static_cast<std::size_t>(view.selected);
  const auto& entry = view.entries[at];
  const bool live = entry.available && !view.offline;

  set_text(previous_, at > 0 ? view.entries[at - 1].name : "");
  set_text(next_, at + 1 < view.entries.size() ? view.entries[at + 1].name : "");
  set_text(icon_, icon_text(entry.icon, entry.default_icon));
  set_text(name_, entry.name);
  set_text(summary_, entry.summary);
  lv_obj_set_style_text_color(icon_, live ? colour(entry.tone) : kDim, 0);
  lv_obj_set_style_text_color(name_, live ? kText : kMuted, 0);
  const bool coloured = live && entry.tone != Tone::Neutral && entry.tone != Tone::Off;
  lv_obj_set_style_text_color(summary_, coloured ? colour(entry.tone) : kMuted, 0);
}

void Ui::update_footer() {
  std::string text;
  switch (view_.screen) {
    case Screen::Empty:
      text = status_text_;
      break;
    case Screen::Home:
      if (view_.offline) text = status_text_;
      break;
    case Screen::Item:
      if (view_.offline) {
        text = status_text_;
      } else if (view_.control_count > 1) {
        text = std::to_string(view_.control_index + 1) + " / " + std::to_string(view_.control_count);
      }
      break;
    case Screen::Settings:
      break;  // each entry's description is already below it
    case Screen::About:
      text = "Hold to go back";
      break;
    case Screen::Confirm:
    case Screen::Notice:
      text = view_.prompt;
      break;
    case Screen::Notifications:
      text = "Hold to go back";
      break;
  }
  set_text(footer_, text);
}

void Ui::set_status(const std::string& text) {
  status_text_ = text;
  update_footer();
}

void Ui::show_pairing_code(const std::string& code) {
  // Two groups of three digits read more easily than six in a row.
  const std::string spaced = code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
  set_text(pairing_code_, spaced);
  lv_obj_set_hidden(pairing_, false);
}

void Ui::hide_pairing_code() {
  lv_obj_set_hidden(pairing_, true);
}

void Ui::build_clock(lv_obj_t* screen) {
  clock_ = lv_obj_create(screen);
  lv_obj_remove_style_all(clock_);
  lv_obj_set_size(clock_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(clock_, kBackground, 0);
  lv_obj_set_style_bg_opa(clock_, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(clock_, false);

  // The date first, so the hands pass over it.
  clock_date_ = make_label(clock_, fonts_.body, kMuted);

  // Analog: twelve marks round the edge, longer at the quarters, and the hands.
  analog_ = lv_obj_create(clock_);
  lv_obj_remove_style_all(analog_);
  lv_obj_set_size(analog_, LV_PCT(100), LV_PCT(100));
  for (std::size_t i = 0; i < marks_.size(); ++i) {
    const bool quarter = i % 3 == 0;
    const double a = static_cast<double>(i) * 3.14159265358979 / 6;
    const double outer = 146;
    const double inner = quarter ? 126 : 134;
    marks_[i][0] = {static_cast<lv_value_precise_t>(std::lround(kCentre + inner * std::sin(a))),
                    static_cast<lv_value_precise_t>(std::lround(kCentre - inner * std::cos(a)))};
    marks_[i][1] = {static_cast<lv_value_precise_t>(std::lround(kCentre + outer * std::sin(a))),
                    static_cast<lv_value_precise_t>(std::lround(kCentre - outer * std::cos(a)))};
    lv_obj_t* mark = lv_line_create(analog_);
    lv_line_set_points(mark, marks_[i].data(), 2);
    lv_obj_set_style_line_width(mark, quarter ? 5 : 3, 0);
    lv_obj_set_style_line_color(mark, quarter ? kMuted : kDim, 0);
    lv_obj_set_style_line_rounded(mark, true, 0);
  }
  hour_hand_ = make_hand(hour_points_, 9, kClock);
  minute_hand_ = make_hand(minute_points_, 6, kClock);
  second_hand_ = make_hand(second_points_, 2, kAccent);
  lv_obj_t* hub = lv_obj_create(analog_);
  lv_obj_remove_style_all(hub);
  lv_obj_set_size(hub, 14, 14);
  lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(hub, kClock, 0);
  lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
  lv_obj_center(hub);

  // Digital: the time, with the seconds or AM/PM smaller beside it.
  digital_ = lv_obj_create(clock_);
  lv_obj_remove_style_all(digital_);
  lv_obj_set_size(digital_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(digital_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(digital_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(digital_, 4, 0);
  lv_obj_align(digital_, LV_ALIGN_CENTER, 0, -10);
  digital_time_ = make_cells(digital_, fonts_.clock, kClock, time_cells_);
  digital_side_ = lv_obj_create(digital_);
  lv_obj_remove_style_all(digital_side_);
  lv_obj_set_size(digital_side_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(digital_side_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(digital_side_, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  digital_suffix_ = make_label(digital_side_, fonts_.body, kMuted);
  digital_seconds_ = make_cells(digital_side_, fonts_.large, kMuted, seconds_cells_);
  // AM/PM in a box as wide as the wider of the two, so it doesn't move.
  lv_label_set_long_mode(digital_suffix_, LV_LABEL_LONG_MODE_CLIP);
  lv_obj_set_style_text_align(digital_suffix_, LV_TEXT_ALIGN_LEFT, 0);
  // Lifts the small text onto the large one's baseline.
  lv_obj_set_style_pad_bottom(digital_side_, 10, 0);

  lv_obj_set_hidden(clock_, true);
}

lv_obj_t* Ui::make_hand(Hand& points, std::int32_t width, lv_color_t colour) {
  points[0] = points[1] = {kCentre, kCentre};
  lv_obj_t* line = lv_line_create(analog_);
  lv_line_set_points(line, points.data(), 2);
  lv_obj_set_style_line_width(line, width, 0);
  lv_obj_set_style_line_color(line, colour, 0);
  lv_obj_set_style_line_rounded(line, true, 0);
  return line;
}

void Ui::set_hand(lv_obj_t* line, Hand& points, int angle, int length) {
  // A short tail past the centre, as a real hand has.
  const double a = angle * 3.14159265358979 / 1800;
  const double tail = length / 7.0;
  points[0] = {static_cast<lv_value_precise_t>(std::lround(kCentre - tail * std::sin(a))),
               static_cast<lv_value_precise_t>(std::lround(kCentre + tail * std::cos(a)))};
  points[1] = {static_cast<lv_value_precise_t>(std::lround(kCentre + length * std::sin(a))),
               static_cast<lv_value_precise_t>(std::lround(kCentre - length * std::cos(a)))};
  lv_line_set_points(line, points.data(), 2);  // takes the new points and redraws
}

void Ui::show_clock(const room::ClockFace& face, room::ClockStyle style, bool date) {
  const bool analog = style == room::ClockStyle::Analog;
  lv_obj_set_hidden(analog_, !analog);
  lv_obj_set_hidden(digital_, analog);
  if (analog) {
    set_hand(hour_hand_, hour_points_, face.hour_angle, 72);
    set_hand(minute_hand_, minute_points_, face.minute_angle, 112);
    lv_obj_set_hidden(second_hand_, face.seconds.empty());
    if (!face.seconds.empty()) set_hand(second_hand_, second_points_, face.second_angle, 120);
  } else {
    set_cells(time_cells_, fonts_.clock, face.time);
    set_text(digital_suffix_, face.suffix);
    lv_obj_set_width(digital_suffix_, std::max(text_width(fonts_.body, "AM"),
                                               text_width(fonts_.body, "PM")));
    lv_obj_set_hidden(digital_suffix_, face.suffix.empty());
    set_cells(seconds_cells_, fonts_.large, face.seconds);
    lv_obj_set_hidden(digital_seconds_, face.seconds.empty());
    lv_obj_set_hidden(digital_side_, face.suffix.empty() && face.seconds.empty());
  }
  // The date: under the hands' hub on the analog face, under the time on the
  // digital one.
  lv_obj_set_hidden(clock_date_, !date);
  set_text(clock_date_, face.date);
  lv_obj_align(clock_date_, LV_ALIGN_CENTER, 0, analog ? 62 : 52);
  lv_obj_set_hidden(clock_, false);
}

void Ui::hide_clock() {
  lv_obj_set_hidden(clock_, true);
}

bool Ui::clock_shown() const {
  return !lv_obj_is_hidden(clock_);
}

void Ui::show_toast(const std::string& text, std::uint32_t ms) {
  if (ms == 0) ms = kToastTime;
  set_text(toast_, text);
  lv_obj_set_hidden(toast_, false);
  lv_obj_set_hidden(footer_, true);
  if (toast_timer_) {
    lv_timer_set_period(toast_timer_, ms);
    lv_timer_reset(toast_timer_);
    return;
  }
  toast_timer_ = lv_timer_create(&Ui::hide_toast, ms, this);
  lv_timer_set_repeat_count(toast_timer_, 1);
}

void Ui::hide_toast(lv_timer_t* timer) {
  auto* self = static_cast<Ui*>(lv_timer_get_user_data(timer));
  lv_obj_set_hidden(self->toast_, true);
  lv_obj_set_hidden(self->footer_, false);
  self->toast_timer_ = nullptr;  // a one-shot timer deletes itself
}

}  // namespace nesto
