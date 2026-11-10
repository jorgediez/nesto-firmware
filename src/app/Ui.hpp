#pragma once

#include <string>

#include "app/Fonts.hpp"
#include "lvgl.h"
#include "room/Controller.hpp"

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

  /// The connection state, shown at the bottom while not connected.
  void set_status(const std::string& text);

  /// A short message at the bottom, for a moment.
  void show_toast(const std::string& text);

  /// Full screen, over everything else, while Home Assistant pairs.
  void show_pairing_code(const std::string& code);
  void hide_pairing_code();

 private:
  void render_list(const room::RoomView& view);
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
  // Waiting, a question, About
  lv_obj_t* centre_ = nullptr;
  lv_obj_t* footer_ = nullptr;
  lv_obj_t* toast_ = nullptr;
  lv_timer_t* toast_timer_ = nullptr;
  lv_obj_t* pairing_ = nullptr;
  lv_obj_t* pairing_code_ = nullptr;
};

}  // namespace nesto
