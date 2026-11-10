#include "app/Ui.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace nesto {
namespace {

const lv_color_t kBackground = lv_color_hex(0x000000);
const lv_color_t kText = lv_color_hex(0xFFFFFF);
const lv_color_t kMuted = lv_color_hex(0x7A7A7A);
const lv_color_t kTrack = lv_color_hex(0x2A2A2A);
const lv_color_t kAccent = lv_color_hex(0xFF8A2A);

constexpr std::uint32_t kToastTime = 1200;

}  // namespace

Ui::Ui(std::string version)
    : items_{"Ceiling light", "Floor lamp", "Radiator", "Window blind", "Film night"} {
  lv_obj_t* screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, kBackground, 0);
  lv_obj_set_scrollable(screen, false);

  // Around the edge, an arc showing where the selection is in the list.
  arc_ = lv_arc_create(screen);
  lv_obj_set_size(arc_, 300, 300);
  lv_obj_center(arc_);
  lv_arc_set_bg_angles(arc_, 135, 45);
  lv_arc_set_range(arc_, 0, static_cast<std::int32_t>(items_.size()) - 1);
  lv_obj_remove_style(arc_, nullptr, LV_PART_KNOB);
  lv_obj_set_clickable(arc_, false);
  lv_obj_set_style_arc_width(arc_, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc_, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc_, kTrack, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc_, kAccent, LV_PART_INDICATOR);

  lv_obj_t* title = lv_label_create(screen);
  lv_label_set_text(title, "Demo room");
  lv_obj_set_style_text_color(title, kMuted, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 62);

  std::string options;
  for (const auto& item : items_) options += (options.empty() ? "" : "\n") + item;
  roller_ = lv_roller_create(screen);
  lv_roller_set_options(roller_, options.c_str(), LV_ROLLER_MODE_NORMAL);
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

  status_ = lv_label_create(screen);
  lv_label_set_text(status_, ("Not paired - v" + version).c_str());
  lv_obj_set_style_text_font(status_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(status_, kMuted, 0);
  lv_obj_align(status_, LV_ALIGN_BOTTOM_MID, 0, -58);

  toast_ = lv_label_create(screen);
  lv_obj_set_style_bg_color(toast_, kTrack, 0);
  lv_obj_set_style_bg_opa(toast_, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(toast_, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(toast_, 14, 0);
  lv_obj_set_style_pad_ver(toast_, 6, 0);
  lv_obj_set_style_text_color(toast_, kText, 0);
  lv_obj_set_style_text_font(toast_, &lv_font_montserrat_14, 0);
  // In the status line's place, which is hidden while the message shows.
  lv_obj_align(toast_, LV_ALIGN_BOTTOM_MID, 0, -52);
  lv_obj_set_hidden(toast_, true);

  rotate(0);
}

Ui::~Ui() {
  if (toast_timer_) lv_timer_delete(toast_timer_);
}

void Ui::rotate(int steps) {
  const int last = static_cast<int>(items_.size()) - 1;
  selected_ = std::clamp(selected_ + steps, 0, last);
  lv_roller_set_selected(roller_, static_cast<std::uint32_t>(selected_), LV_ANIM_ON);
  lv_arc_set_value(arc_, selected_);
}

void Ui::select() {
  show_toast("Selected " + items_[static_cast<std::size_t>(selected_)]);
}

void Ui::back() {
  show_toast("Back");
}

void Ui::show_toast(const std::string& text) {
  lv_label_set_text(toast_, text.c_str());
  lv_obj_set_hidden(toast_, false);
  lv_obj_set_hidden(status_, true);
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
  lv_obj_set_hidden(self->status_, false);
  self->toast_timer_ = nullptr;  // a one-shot timer deletes itself
}

}  // namespace nesto
