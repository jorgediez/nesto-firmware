#include "room/Notifications.hpp"

#include <algorithm>

namespace nesto::room {
namespace {

constexpr std::size_t kMaxNid = 64;
constexpr std::size_t kMaxMessage = 256;
constexpr std::size_t kMaxActions = 3;
constexpr unsigned kMaxDuration = 86400;

/// Characters, not bytes: UTF-8 continuation bytes don't count.
std::size_t characters(const std::string& text) {
  return static_cast<std::size_t>(
      std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

bool optional_string(const protocol::Json& body, const char* key, std::string& out, std::string& error) {
  const auto it = body.find(key);
  if (it == body.end() || it->is_null()) return true;
  if (!it->is_string()) {
    error = std::string(key) + " must be text";
    return false;
  }
  out = it->get<std::string>();
  return true;
}

bool untimed(const Notification& n) { return n.priority == Priority::Critical || n.duration_s == 0; }

}  // namespace

bool Notification::parse(const protocol::Json& body, Notification& out, std::string& error) {
  out = Notification();
  if (!body.is_object()) {
    error = "not an object";
    return false;
  }
  if (!optional_string(body, "nid", out.nid, error) || !optional_string(body, "title", out.title, error) ||
      !optional_string(body, "message", out.message, error) || !optional_string(body, "icon", out.icon, error)) {
    return false;
  }
  if (out.nid.empty() || characters(out.nid) > kMaxNid) {
    error = "nid must be 1 to 64 characters";
    return false;
  }
  if (out.message.empty() || characters(out.message) > kMaxMessage) {
    error = "message must be 1 to 256 characters";
    return false;
  }
  if (const auto it = body.find("priority"); it != body.end() && !it->is_null()) {
    const std::string p = it->is_string() ? it->get<std::string>() : "";
    if (p == "low") {
      out.priority = Priority::Low;
    } else if (p == "normal") {
      out.priority = Priority::Normal;
    } else if (p == "high") {
      out.priority = Priority::High;
    } else if (p == "critical") {
      out.priority = Priority::Critical;
    } else {
      error = "priority must be low, normal, high or critical";
      return false;
    }
  }
  if (const auto it = body.find("duration_s"); it != body.end() && !it->is_null()) {
    if (!it->is_number() || it->get<double>() < 0) {
      error = "duration_s must be 0 or more";
      return false;
    }
    out.duration_s = static_cast<unsigned>(std::min(it->get<double>(), static_cast<double>(kMaxDuration)));
  }
  if (const auto it = body.find("actions"); it != body.end() && !it->is_null()) {
    if (!it->is_array() || it->size() > kMaxActions) {
      error = "actions must be a list of at most 3";
      return false;
    }
    for (const auto& a : *it) {
      Action action;
      if (a.is_object()) {
        action.id = a.value("id", std::string());
        action.label = a.value("label", std::string());
      }
      if (action.id.empty() || action.label.empty()) {
        error = "each action needs an id and a label";
        return false;
      }
      out.actions.push_back(std::move(action));
    }
  }
  // `beep` is accepted and ignored: the panel doesn't beep yet.
  return true;
}

Notifications::Kept* Notifications::find(const std::string& nid) {
  for (Kept& k : kept_) {
    if (k.n.nid == nid) return &k;
  }
  return nullptr;
}

Notifications::Kept* Notifications::current() { return current_.empty() ? nullptr : find(current_); }

int Notifications::unread() const {
  return static_cast<int>(std::count_if(kept_.begin(), kept_.end(), [](const Kept& k) { return !k.read; }));
}

void Notifications::add(Notification n, bool lit, Millis now) {
  now_ = now;
  const std::string nid = n.nid;
  const bool is_new = find(nid) == nullptr;
  if (!is_new) {
    kept_.erase(std::find_if(kept_.begin(), kept_.end(), [&](const Kept& k) { return k.n.nid == nid; }));
  }
  kept_.insert(kept_.begin(), Kept{std::move(n), false, false});
  if (kept_.size() > kKept) {
    // The oldest that isn't critical goes, or the oldest of all.
    auto drop = std::find_if(kept_.rbegin(), kept_.rend(),
                             [](const Kept& k) { return k.n.priority != Priority::Critical; });
    const std::string dropped = drop != kept_.rend() ? drop->n.nid : kept_.back().n.nid;
    if (dropped == current_) close_current();
    remove(dropped);
  }
  if (mode_ == Mode::List && is_new) selected_ = std::min(selected_ + 1, count() - 1);

  const Kept& k = kept_.front();
  if (k.n.priority != Priority::Low && lit) {
    // A pop-up waiting for an answer isn't pushed aside by a lesser one.
    Kept* shown = mode_ == Mode::Popup ? current() : nullptr;
    const bool keep_shown = shown && shown->n.nid != nid && shown->n.priority == Priority::Critical &&
                            k.n.priority != Priority::Critical;
    // One opened from the list isn't interrupted, except by a critical one.
    const bool reading = mode_ == Mode::Open && current_ != nid;
    if (!keep_shown && (!reading || k.n.priority == Priority::Critical)) pop_up(nid, now);
  }
  if (current_ == nid) {
    // Replaced while on screen: the new one, from the start.
    choice_ = 0;
    scroll_ = 0;
    shown_at_ = now;
    mark_shown(kept_.front());
  }
  changed();
}

void Notifications::pop_up(const std::string& nid, Millis now) {
  if (mode_ == Mode::Popup && current_ != nid) {
    // Pushed aside without an answer: a timed one has had its time.
    if (Kept* old = current(); old && !untimed(old->n)) event(old->n.nid, "expired");
  }
  // Over the list, or one opened from it: back to the list afterwards.
  if (mode_ == Mode::List || mode_ == Mode::Open) back_to_list_ = true;
  mode_ = Mode::Popup;
  current_ = nid;
  shown_at_ = now;
  choice_ = 0;
  scroll_ = 0;
  scroll_limit_ = 0;
  if (Kept* k = current()) mark_shown(*k);
  changed();
}

void Notifications::mark_shown(Kept& k) {
  if (k.read) return;
  k.read = true;
  event(k.n.nid, "shown");
}

void Notifications::close_current() {
  mode_ = Mode::None;
  current_.clear();
  changed();
}

void Notifications::after_popup(Millis now) {
  close_current();
  for (const Kept& k : kept_) {
    if (untimed(k.n) && !k.answered && k.n.priority != Priority::Low) {
      pop_up(k.n.nid, now);
      return;
    }
  }
  if (back_to_list_ && !kept_.empty()) {
    mode_ = Mode::List;
    selected_ = std::clamp(selected_, 0, count() - 1);
  }
  back_to_list_ = false;
}

void Notifications::remove(const std::string& nid) {
  kept_.erase(std::remove_if(kept_.begin(), kept_.end(), [&](const Kept& k) { return k.n.nid == nid; }),
              kept_.end());
  selected_ = std::clamp(selected_, 0, std::max(0, count() - 1));
  changed();
}

void Notifications::clear(const std::string& nid) {
  if (!find(nid)) return;
  const bool on_screen = nid == current_;
  const Mode mode = mode_;
  remove(nid);
  if (!on_screen) return;
  if (mode == Mode::Popup) {
    after_popup(now_);
  } else {
    current_.clear();
    mode_ = kept_.empty() ? Mode::None : Mode::List;
    changed();
  }
}

void Notifications::clear_all() {
  kept_.clear();
  current_.clear();
  mode_ = Mode::None;
  selected_ = 0;
  changed();
}

void Notifications::open_list() {
  if (kept_.empty()) return;
  mode_ = Mode::List;
  current_.clear();
  selected_ = 0;
  changed();
}

int Notifications::choice_count() const {
  const auto it = std::find_if(kept_.begin(), kept_.end(), [&](const Kept& k) { return k.n.nid == current_; });
  return it == kept_.end() ? 1 : static_cast<int>(it->n.actions.size()) + 1;  // and Dismiss
}

void Notifications::rotate(int clicks) {
  if (clicks == 0) return;
  if (mode_ == Mode::List) {
    const int next = std::clamp(selected_ + clicks, 0, count() - 1);
    if (next != selected_) {
      selected_ = next;
      changed();
    }
  } else if (mode_ == Mode::Popup || mode_ == Mode::Open) {
    if (choice_count() > 1) {
      const int next = std::clamp(choice_ + clicks, 0, choice_count() - 1);
      if (next != choice_) {
        choice_ = next;
        changed();
      }
    } else {
      // Only Dismiss to choose: the ring scrolls the message instead.
      const int next = std::clamp(scroll_ + clicks, 0, scroll_limit_);
      if (next != scroll_) {
        scroll_ = next;
        changed();
      }
    }
  }
}

void Notifications::press() {
  if (mode_ == Mode::List) {
    if (kept_.empty()) return;
    current_ = kept_[static_cast<std::size_t>(selected_)].n.nid;
    mode_ = Mode::Open;
    choice_ = 0;
    scroll_ = 0;
    scroll_limit_ = 0;
    mark_shown(kept_[static_cast<std::size_t>(selected_)]);
    changed();
    return;
  }
  if (mode_ != Mode::Popup && mode_ != Mode::Open) return;
  Kept* k = current();
  if (!k) return;
  const std::string nid = k->n.nid;
  if (choice_ < static_cast<int>(k->n.actions.size())) {
    event(nid, "action", k->n.actions[static_cast<std::size_t>(choice_)].id);
  } else {
    event(nid, "dismissed");
  }
  const Mode mode = mode_;
  remove(nid);
  if (mode == Mode::Popup) {
    after_popup(now_);
  } else {
    current_.clear();
    mode_ = kept_.empty() ? Mode::None : Mode::List;
    changed();
  }
}

void Notifications::long_press() {
  switch (mode_) {
    case Mode::Popup:
      if (Kept* k = current()) {
        if (k->n.priority == Priority::Critical) return;  // answer it, or dismiss it
        k->answered = true;  // to the list: it doesn't pop up again
      }
      after_popup(now_);
      return;
    case Mode::Open:
      current_.clear();
      mode_ = Mode::List;
      changed();
      return;
    case Mode::List:
      mode_ = Mode::None;
      changed();
      return;
    case Mode::None:
      return;
  }
}

void Notifications::tick(Millis now) {
  now_ = now;
  if (mode_ != Mode::Popup) return;
  Kept* k = current();
  if (!k || untimed(k->n)) return;
  if (now - shown_at_ >= static_cast<Millis>(k->n.duration_s) * 1000) {
    event(k->n.nid, "expired");
    k->answered = true;
    after_popup(now);
  }
}

void Notifications::display_rested() {
  if (mode_ == Mode::Popup) {
    if (Kept* k = current(); k && !untimed(k->n)) {
      event(k->n.nid, "expired");
      k->answered = true;
    }
  }
  if (mode_ != Mode::None) close_current();
  back_to_list_ = false;
}

void Notifications::display_woke(Millis now) {
  now_ = now;
  if (mode_ != Mode::None) return;
  after_popup(now);
}

bool Notifications::holds_display() const {
  if (mode_ != Mode::Popup) return false;
  const auto it = std::find_if(kept_.begin(), kept_.end(), [&](const Kept& k) { return k.n.nid == current_; });
  return it != kept_.end() && !untimed(it->n);
}

void Notifications::set_scroll_limit(int lines) {
  lines = std::max(0, lines);
  if (lines == scroll_limit_) return;
  scroll_limit_ = lines;
  scroll_ = std::min(scroll_, scroll_limit_);
  changed();  // the prompt may change
}

void Notifications::event(const std::string& nid, const char* what, const std::string& action) {
  protocol::Json e{{"t", "notify_event"}, {"nid", nid}, {"event", what}};
  if (!action.empty()) e["action"] = action;
  if (send_) send_(e);
}

RoomView Notifications::view() const {
  RoomView v;
  v.offline = false;
  auto tone = [](const Notification& n) {
    if (n.priority == Priority::Critical) return Tone::Heat;
    if (n.priority == Priority::High) return Tone::Light;
    return Tone::Neutral;
  };
  if (mode_ == Mode::List) {
    v.screen = RoomView::Screen::Notifications;
    v.title = "Notifications";
    for (const Kept& k : kept_) {
      const bool titled = !k.n.title.empty();
      RoomView::Entry e;
      e.name = titled ? k.n.title : k.n.message;
      e.summary = titled ? k.n.message : "";
      e.icon = k.n.icon.empty() ? "bell" : k.n.icon;
      e.default_icon = "bell";
      // Unread ones in the accent's colour, until opened.
      e.tone = k.read ? tone(k.n) : Tone::Heat;
      v.entries.push_back(std::move(e));
    }
    v.selected = selected_;
    return v;
  }
  const auto it = std::find_if(kept_.begin(), kept_.end(), [&](const Kept& k) { return k.n.nid == current_; });
  if (it == kept_.end()) return v;
  const Notification& n = it->n;
  v.screen = RoomView::Screen::Notice;
  v.title = mode_ == Mode::Open ? "Notifications" : "";
  v.item_name = n.title;
  v.icon = n.icon.empty() ? "bell" : n.icon;
  v.default_icon = "bell";
  v.tone = tone(n);
  v.message = n.message;
  for (const auto& a : n.actions) v.choices.push_back(a.label);
  v.choices.push_back("Dismiss");
  v.choice = choice_;
  v.scroll = scroll_;
  if (n.actions.empty() && scroll_ < scroll_limit_) {
    v.prompt = "Turn to read on";
  } else if (n.priority == Priority::Critical) {
    v.prompt = "Press to answer";
  } else {
    v.prompt = mode_ == Mode::Open ? "Hold to go back" : "Hold to keep for later";
  }
  return v;
}

}  // namespace nesto::room
