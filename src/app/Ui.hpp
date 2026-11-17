#pragma once

#include <array>
#include <string>
#include <vector>

#include "app/Fonts.hpp"
#include "lvgl.h"
#include "room/Controller.hpp"
#include "room/StandbyClock.hpp"

namespace nesto {

/// The screens, drawn from the room controller's view.
///
/// Home and Settings show one entry at a time, in the middle: its icon, name
/// and state, coloured by what it's doing, with the entries before and after it
/// dimmed above and below, and the arc showing where it is in the list. An
/// item's screen shows what the ring changes and its value, with the arc as a
/// gauge.
class Ui {
 public:
  explicit Ui(const Fonts& fonts);
  ~Ui();

  Ui(const Ui&) = delete;
  Ui& operator=(const Ui&) = delete;

  /// Redraws everything from `view`.
  void render(const room::RoomView& view);
  /// On a notification, how many lines its message can scroll: those that
  /// don't fit the box.
  int notice_scroll_limit() const { return notice_scroll_limit_; }
  /// On Technical info, how many entries it can scroll past.
  int info_scroll_limit() const { return info_scroll_limit_; }

  /// The connection state, shown at the bottom while not connected.
  void set_status(const std::string& text);

  /// A short message at the bottom, for a moment.
  void show_toast(const std::string& text, std::uint32_t ms = 0);  // 0: the usual time

  /// Full screen, over everything else, while Home Assistant pairs.
  void show_pairing_code(const std::string& code);
  void hide_pairing_code();

  /// The standby clock, over everything but the pairing code. Called again
  /// to move it on.
  void show_clock(const room::ClockFace& face, room::ClockStyle style, bool date);
  void hide_clock();
  bool clock_shown() const;

 private:
  using Hand = std::array<lv_point_precise_t, 2>;
  void build_clock(lv_obj_t* screen);
  lv_obj_t* make_hand(Hand& points, std::int32_t width, lv_color_t colour);
  void set_hand(lv_obj_t* line, Hand& points, int angle, int length);

  void render_list(const room::RoomView& view);
  void render_notice(const room::RoomView& view);
  void render_info(const room::RoomView& view);
  void render_keyboard(const room::RoomView& view);
  void update_footer();
  static void hide_toast(lv_timer_t* timer);

  const Fonts& fonts_;
  room::RoomView view_;
  std::string status_text_;

  lv_obj_t* arc_ = nullptr;
  lv_obj_t* title_ = nullptr;
  // Home and Settings
  lv_obj_t* previous_ = nullptr;
  lv_obj_t* icon_ = nullptr;
  lv_obj_t* name_ = nullptr;
  lv_obj_t* summary_ = nullptr;
  lv_obj_t* next_ = nullptr;
  // An item
  lv_obj_t* control_ = nullptr;
  lv_obj_t* value_ = nullptr;
  // Waiting, a question
  lv_obj_t* centre_ = nullptr;
  // Technical info: its entries in a column, moved up inside a box that
  // clips them as the ring scrolls.
  lv_obj_t* info_box_ = nullptr;
  lv_obj_t* info_list_ = nullptr;
  std::vector<room::InfoLine> info_drawn_;
  int info_scroll_limit_ = 0;
  // The ring keyboard: a label per key round the edge, and in the middle
  // what it's for and the text typed.
  lv_obj_t* keyboard_ = nullptr;
  lv_obj_t* keyboard_icon_ = nullptr;
  lv_obj_t* keyboard_prompt_ = nullptr;
  lv_obj_t* keyboard_field_ = nullptr;
  std::vector<lv_obj_t*> key_labels_;
  std::vector<std::string> keys_drawn_;
  lv_obj_t* footer_ = nullptr;
  lv_obj_t* toast_ = nullptr;
  lv_timer_t* toast_timer_ = nullptr;
  // The standby clock: analog or digital, with the date.
  lv_obj_t* clock_ = nullptr;
  lv_obj_t* analog_ = nullptr;
  std::array<Hand, 12> marks_{};
  Hand hour_points_{};
  Hand minute_points_{};
  Hand second_points_{};
  lv_obj_t* hour_hand_ = nullptr;
  lv_obj_t* minute_hand_ = nullptr;
  lv_obj_t* second_hand_ = nullptr;
  lv_obj_t* digital_ = nullptr;
  // The time and the seconds: a cell per character, "88:88" and ":88" at most.
  lv_obj_t* digital_time_ = nullptr;
  std::array<lv_obj_t*, 5> time_cells_{};
  lv_obj_t* digital_side_ = nullptr;  // AM/PM over the seconds
  lv_obj_t* digital_suffix_ = nullptr;
  lv_obj_t* digital_seconds_ = nullptr;
  std::array<lv_obj_t*, 3> seconds_cells_{};
  lv_obj_t* clock_date_ = nullptr;
  lv_obj_t* pairing_ = nullptr;
  lv_obj_t* pairing_code_ = nullptr;
  // A notification: its icon, title, message (in a box that scrolls) and
  // the choice the ring is on.
  lv_obj_t* notice_icon_ = nullptr;
  lv_obj_t* notice_title_ = nullptr;
  lv_obj_t* notice_box_ = nullptr;
  lv_obj_t* notice_message_ = nullptr;
  lv_obj_t* notice_choice_ = nullptr;
  int notice_scroll_limit_ = 0;
};

}  // namespace nesto
