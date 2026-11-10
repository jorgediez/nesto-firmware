#include "app/Ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace nesto {
namespace {

const lv_color_t kBackground = lv_color_hex(0x000000);
const lv_color_t kText = lv_color_hex(0xFFFFFF);
const lv_color_t kMuted = lv_color_hex(0x7A7A7A);
const lv_color_t kTrack = lv_color_hex(0x2A2A2A);
const lv_color_t kAccent = lv_color_hex(0xFF8A2A);

constexpr std::uint32_t kToastTime = 1500;
constexpr std::int32_t kArcScale = 1000;

/// The built-in fonts only cover ASCII; until the localised fonts arrive, drop
/// the degree sign rather than show a missing-glyph box.
std::string for_font(std::string text) {
  for (std::size_t at; (at = text.find("\xC2\xB0")) != std::string::npos;) text.erase(at, 2);
  return text;
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t colour) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, colour, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  return label;
}

}  // namespace

Ui::Ui() {
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
  lv_obj_set_style_arc_color(arc_, kAccent, LV_PART_INDICATOR);

  title_ = make_label(screen, &lv_font_montserrat_20, kMuted);
  lv_obj_set_width(title_, 200);
  lv_label_set_long_mode(title_, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 58);

  roller_ = lv_roller_create(screen);
  lv_roller_set_visible_row_count(roller_, 3);
  lv_obj_set_width(roller_, 240);
  lv_obj_center(roller_);
  lv_obj_set_style_bg_opa(roller_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(roller_, 0, LV_PART_MAIN);
  lv_obj_set_style_text_font(roller_, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(roller_, kMuted, LV_PART_MAIN);
  lv_obj_set_style_text_align(roller_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(roller_, LV_OPA_TRANSP, LV_PART_SELECTED);
  lv_obj_set_style_text_color(roller_, kText, LV_PART_SELECTED);
  lv_obj_set_clickable(roller_, false);

  centre_ = make_label(screen, &lv_font_montserrat_20, kText);
  lv_obj_center(centre_);

  control_ = make_label(screen, &lv_font_montserrat_14, kMuted);
  lv_obj_align(control_, LV_ALIGN_CENTER, 0, -44);

  value_ = make_label(screen, &lv_font_montserrat_48, kText);
  lv_obj_set_width(value_, 230);
  lv_label_set_long_mode(value_, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_align(value_, LV_ALIGN_CENTER, 0, 6);

  footer_ = make_label(screen, &lv_font_montserrat_14, kMuted);
  lv_obj_set_width(footer_, 210);
  lv_label_set_long_mode(footer_, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_align(footer_, LV_ALIGN_BOTTOM_MID, 0, -58);

  toast_ = make_label(screen, &lv_font_montserrat_14, kText);
  lv_obj_set_style_bg_color(toast_, kTrack, 0);
  lv_obj_set_style_bg_opa(toast_, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(toast_, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(toast_, 14, 0);
  lv_obj_set_style_pad_ver(toast_, 6, 0);
  lv_obj_align(toast_, LV_ALIGN_BOTTOM_MID, 0, -52);
  lv_obj_set_hidden(toast_, true);

  // The pairing screen covers everything while it's shown.
  pairing_ = lv_obj_create(screen);
  lv_obj_remove_style_all(pairing_);
  lv_obj_set_size(pairing_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(pairing_, kBackground, 0);
  lv_obj_set_style_bg_opa(pairing_, LV_OPA_COVER, 0);
  lv_obj_t* heading = make_label(pairing_, &lv_font_montserrat_20, kMuted);
  lv_label_set_text(heading, "Pairing code");
  lv_obj_align(heading, LV_ALIGN_CENTER, 0, -58);
  pairing_code_ = make_label(pairing_, &lv_font_montserrat_48, kAccent);
  lv_obj_align(pairing_code_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_t* hint = make_label(pairing_, &lv_font_montserrat_14, kMuted);
  lv_label_set_text(hint, "Enter it in\nHome Assistant");
  lv_obj_align(hint, LV_ALIGN_CENTER, 0, 62);
  lv_obj_set_hidden(pairing_, true);

  render(view_);
}

Ui::~Ui() {
  if (toast_timer_) lv_timer_delete(toast_timer_);
}

void Ui::render(const room::RoomView& view) {
  using Screen = room::RoomView::Screen;
  const bool same_screen = view.screen == view_.screen;
  view_ = view;
  const bool home = view.screen == Screen::Home;
  const bool item = view.screen == Screen::Item;
  const bool menu = view.screen == Screen::Settings;
  const bool listed = (home || menu) && !view.entries.empty();

  // Title
  std::string title = "Nesto";
  if (home) title = view.title.empty() ? "Home Assistant" : view.title;
  if (item) title = view.item_name;
  if (menu || view.screen == Screen::Confirm || view.screen == Screen::About) title = view.title;
  lv_label_set_text(title_, for_font(title).c_str());

  // Home and Settings: the list
  lv_obj_set_hidden(roller_, !listed);
  if (listed) {
    std::vector<std::string> names;
    for (const auto& e : view.entries) names.push_back(for_font(e.name));
    if (names != roller_names_) {
      std::string options;
      for (const auto& n : names) options += (options.empty() ? "" : "\n") + n;
      lv_roller_set_options(roller_, options.c_str(), LV_ROLLER_MODE_NORMAL);
      roller_names_ = std::move(names);
    }
    lv_roller_set_selected(roller_, static_cast<std::uint32_t>(view.selected),
                           same_screen ? LV_ANIM_ON : LV_ANIM_OFF);
    const auto& selected = view.entries[static_cast<std::size_t>(view.selected)];
    lv_obj_set_style_text_color(roller_, selected.available && !view.offline ? kText : kMuted,
                                LV_PART_SELECTED);
  }

  // A message in the middle: waiting, a question, or About's lines.
  std::string centre;
  if (view.screen == Screen::Empty) centre = "Waiting for\nHome Assistant";
  if (view.screen == Screen::Confirm) centre = view.item_name;
  if (view.screen == Screen::About) {
    for (const auto& line : view.lines) centre += (centre.empty() ? "" : "\n") + line;
  }
  lv_obj_set_hidden(centre_, centre.empty());
  lv_obj_set_style_text_font(centre_, view.screen == Screen::About ? &lv_font_montserrat_14
                                                                    : &lv_font_montserrat_20,
                             0);
  lv_label_set_text(centre_, for_font(centre).c_str());

  // Item: control and value
  lv_obj_set_hidden(control_, !item);
  lv_obj_set_hidden(value_, !item);
  if (item) {
    lv_label_set_text(control_, view.control.c_str());
    lv_label_set_text(value_, for_font(view.value.empty() ? "-" : view.value).c_str());
    lv_obj_set_style_text_color(value_, view.available && !view.offline ? kText : kMuted, 0);
  }

  // The arc: the position in the list, or the item's value as a gauge.
  bool arc_shown = false;
  if (listed && view.entries.size() > 1) {
    lv_arc_set_range(arc_, 0, static_cast<std::int32_t>(view.entries.size()) - 1);
    lv_arc_set_value(arc_, view.selected);
    arc_shown = true;
  } else if (item && view.fraction >= 0) {
    lv_arc_set_range(arc_, 0, kArcScale);
    lv_arc_set_value(arc_, static_cast<std::int32_t>(std::lround(view.fraction * kArcScale)));
    arc_shown = true;
  }
  lv_obj_set_style_arc_opa(arc_, arc_shown ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc_, view.offline ? kMuted : kAccent, LV_PART_INDICATOR);

  update_footer();
}

void Ui::update_footer() {
  using Screen = room::RoomView::Screen;
  std::string text = status_text_;
  if (view_.screen == Screen::Settings && !view_.entries.empty()) {
    text = view_.entries[static_cast<std::size_t>(view_.selected)].summary;
  } else if (view_.screen == Screen::About) {
    text = "Hold to go back";
  } else if (view_.screen == Screen::Confirm) {
    text = view_.prompt;
  } else if (!view_.offline && view_.screen == Screen::Home && !view_.entries.empty()) {
    text = view_.entries[static_cast<std::size_t>(view_.selected)].summary;
  } else if (!view_.offline && view_.screen == Screen::Item && view_.control_count > 1) {
    text = std::to_string(view_.control_index + 1) + " / " + std::to_string(view_.control_count);
  } else if (!view_.offline && view_.screen == Screen::Item) {
    text.clear();
  }
  lv_label_set_text(footer_, for_font(text).c_str());
}

void Ui::set_status(const std::string& text) {
  status_text_ = text;
  update_footer();
}

void Ui::show_pairing_code(const std::string& code) {
  // Two groups of three digits read more easily than six in a row.
  const std::string spaced = code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
  lv_label_set_text(pairing_code_, spaced.c_str());
  lv_obj_set_hidden(pairing_, false);
}

void Ui::hide_pairing_code() {
  lv_obj_set_hidden(pairing_, true);
}

void Ui::show_toast(const std::string& text) {
  lv_label_set_text(toast_, for_font(text).c_str());
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
