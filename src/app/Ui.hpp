#pragma once

#include <string>
#include <vector>

#include "lvgl.h"
#include "room/Controller.hpp"

namespace nesto {

/// The screens, drawn from the room controller's view. This first version is
/// plain: the home list with each item's state, and an item's control with its
/// value on the arc. Icons, colours and the Settings menu come next.
class Ui {
 public:
  Ui();
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
  void update_footer();
  static void hide_toast(lv_timer_t* timer);

  room::RoomView view_;
  std::string status_text_;
  std::vector<std::string> roller_names_;

  lv_obj_t* arc_ = nullptr;
  lv_obj_t* title_ = nullptr;
  lv_obj_t* roller_ = nullptr;
  lv_obj_t* centre_ = nullptr;   // a message when there's nothing to list
  lv_obj_t* control_ = nullptr;  // item screen: what the ring changes
  lv_obj_t* value_ = nullptr;    // item screen: its value
  lv_obj_t* footer_ = nullptr;   // status, the selected item's state, or the page
  lv_obj_t* toast_ = nullptr;
  lv_timer_t* toast_timer_ = nullptr;
  lv_obj_t* pairing_ = nullptr;
  lv_obj_t* pairing_code_ = nullptr;
};

}  // namespace nesto
