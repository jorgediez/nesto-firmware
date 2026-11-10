#pragma once

#include <string>
#include <vector>

#include "lvgl.h"

namespace nesto {

/// The screens. For now a single demo screen that proves the ring and press
/// reach the interface: a list of made-up items, chosen by turning the ring.
/// The real screens are built from the layout Home Assistant sends.
class Ui {
 public:
  explicit Ui(std::string version);
  ~Ui();

  Ui(const Ui&) = delete;
  Ui& operator=(const Ui&) = delete;

  void rotate(int steps);
  void select();  // single press
  void back();    // long press

 private:
  void show_toast(const std::string& text);
  static void hide_toast(lv_timer_t* timer);

  std::vector<std::string> items_;
  int selected_ = 0;
  lv_obj_t* arc_ = nullptr;
  lv_obj_t* roller_ = nullptr;
  lv_obj_t* status_ = nullptr;
  lv_obj_t* toast_ = nullptr;
  lv_timer_t* toast_timer_ = nullptr;
};

}  // namespace nesto
