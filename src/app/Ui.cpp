#include "app/Ui.hpp"

#include <cmath>
#include <cstdint>

#include "app/FontData.hpp"

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

constexpr std::uint32_t kToastTime = 1500;
constexpr std::int32_t kArcScale = 1000;

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
  const bool list = (view.screen == Screen::Home || view.screen == Screen::Settings) &&
                    !view.entries.empty();
  const bool item = view.screen == Screen::Item;

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
  }
  lv_obj_set_style_arc_opa(arc_, arc_shown ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc_, view.offline ? kMuted : arc_colour(arc_tone),
                             LV_PART_INDICATOR);

  update_footer();
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
      text = view_.prompt;
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

void Ui::show_toast(const std::string& text) {
  set_text(toast_, text);
  lv_obj_set_hidden(toast_, false);
  lv_obj_set_hidden(footer_, true);
  if (toast_timer_) {
    lv_timer_reset(toast_timer_);
    return;
  }
  toast_timer_ = lv_timer_create(&Ui::hide_toast, kToastTime, this);
  lv_timer_set_repeat_count(toast_timer_, 1);
}

void Ui::hide_toast(lv_timer_t* timer) {
  auto* self = static_cast<Ui*>(lv_timer_get_user_data(timer));
  lv_obj_set_hidden(self->toast_, true);
  lv_obj_set_hidden(self->footer_, false);
  self->toast_timer_ = nullptr;  // a one-shot timer deletes itself
}

}  // namespace nesto
