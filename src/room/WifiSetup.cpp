#include "room/WifiSetup.hpp"

#include <algorithm>
#include <cctype>

namespace nesto::room {
namespace {

const char* bars(int strength) {
  if (strength >= 75) return "wifi-4";
  if (strength >= 50) return "wifi-3";
  if (strength >= 25) return "wifi-2";
  return "wifi-1";
}

/// What WPA takes: 8 to 63 characters, or 64 hexadecimal digits (the key
/// itself).
bool valid_passphrase(const std::string& p) {
  if (p.size() == 64) {
    return std::all_of(p.begin(), p.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
  }
  return p.size() >= 8 && p.size() <= 63;
}

}  // namespace

WifiSetup::WifiSetup(Actions actions) : actions_(std::move(actions)) {}

void WifiSetup::open() {
  mode_ = Mode::List;
  selected_ = 0;
  changed();
  if (actions_.scan) actions_.scan();
}

void WifiSetup::close() {
  if (mode_ == Mode::None) return;
  mode_ = Mode::None;
  changed();
}

void WifiSetup::set_networks(const std::vector<WifiNetwork>& networks, bool listed) {
  // The highlight stays on the same network as the list changes.
  const std::string current =
      selected_ < static_cast<int>(networks_.size()) ? networks_[static_cast<std::size_t>(selected_)].name : "";
  const bool on_other = !networks_.empty() && selected_ >= static_cast<int>(networks_.size());
  networks_ = networks;
  listed_ = listed;
  if (on_other) {
    selected_ = static_cast<int>(networks_.size());
  } else {
    const auto it =
        std::find_if(networks_.begin(), networks_.end(), [&](const WifiNetwork& n) { return n.name == current; });
    selected_ = it != networks_.end() ? static_cast<int>(it - networks_.begin()) : 0;
  }
  changed();
}

void WifiSetup::set_state(WifiState state) {
  if (mode_ != Mode::Joining) return;
  if (state == WifiState::Connected) {
    message_ = "Connected to " + request_.name;
    mode_ = Mode::List;
    selected_ = 0;
    changed();
  } else if (state == WifiState::Failed) {
    message_ = "Couldn't join " + request_.name;
    // A password can be corrected, or given anew for a saved network whose
    // password changed; an open network goes back to the list.
    if (request_.security == WifiSecurity::Wpa) {
      keyboard_ = RingKeyboard(kMaxPassword, request_.passphrase);
      mode_ = Mode::Password;
    } else {
      mode_ = Mode::List;
    }
    changed();
  }
}

void WifiSetup::join(const WifiConnect& request) {
  request_ = request;
  mode_ = Mode::Joining;
  changed();
  if (actions_.connect) actions_.connect(request_);
}

void WifiSetup::rotate(int clicks) {
  if (clicks == 0) return;
  switch (mode_) {
    case Mode::List: {
      const int next = std::clamp(selected_ + clicks, 0, list_size() - 1);
      if (next == selected_) return;
      selected_ = next;
      break;
    }
    case Mode::Security: {
      const int next = std::clamp(security_ + clicks, 0, 1);
      if (next == security_) return;
      security_ = next;
      break;
    }
    case Mode::Options: {
      const int next = std::clamp(option_ + clicks, 0, static_cast<int>(options_.size()) - 1);
      if (next == option_) return;
      option_ = next;
      break;
    }
    case Mode::Name:
    case Mode::Password:
      keyboard_.rotate(clicks);
      break;
    case Mode::None:
    case Mode::Joining:
      return;
  }
  changed();
}

void WifiSetup::press() {
  switch (mode_) {
    case Mode::List: {
      if (selected_ >= static_cast<int>(networks_.size())) {
        // Other network: its name first.
        request_ = WifiConnect{};
        request_.hidden = true;
        keyboard_ = RingKeyboard(kMaxName);
        mode_ = Mode::Name;
        break;
      }
      const WifiNetwork& n = networks_[static_cast<std::size_t>(selected_)];
      if (n.saved || n.connected) {
        // Join it, or forget it.
        chosen_ = n;
        options_.clear();
        if (!n.connected && n.security != WifiSecurity::Unsupported) options_.push_back(Option::Join);
        options_.push_back(Option::Forget);
        option_ = 0;
        mode_ = Mode::Options;
        break;
      }
      if (n.security == WifiSecurity::Unsupported) {
        message_ = n.security_name + " isn't supported";
        break;
      }
      WifiConnect request{n.path, n.name, n.security, "", false};
      if (n.security == WifiSecurity::Open) {
        join(request);
        return;
      }
      request_ = request;
      keyboard_ = RingKeyboard(kMaxPassword);
      mode_ = Mode::Password;
      break;
    }
    case Mode::Options:
      if (options_[static_cast<std::size_t>(option_)] == Option::Join) {
        join(WifiConnect{chosen_.path, chosen_.name, chosen_.security, "", false});
        return;
      }
      if (actions_.forget) actions_.forget(chosen_);
      message_ = "Forgot " + chosen_.name;
      mode_ = Mode::List;
      break;
    case Mode::Name:
      if (!keyboard_.press()) break;
      if (keyboard_.text().empty()) {
        message_ = "Type the network's name";
        break;
      }
      request_.name = keyboard_.text();
      security_ = 0;
      mode_ = Mode::Security;
      break;
    case Mode::Security:
      if (security_ == 1) {
        request_.security = WifiSecurity::Open;
        request_.passphrase.clear();
        join(request_);
        return;
      }
      request_.security = WifiSecurity::Wpa;
      keyboard_ = RingKeyboard(kMaxPassword);
      mode_ = Mode::Password;
      break;
    case Mode::Password:
      if (!keyboard_.press()) break;
      if (!valid_passphrase(keyboard_.text())) {
        message_ = "Passwords have 8 to 63 characters";
        break;
      }
      request_.passphrase = keyboard_.text();
      join(request_);
      return;
    case Mode::None:
    case Mode::Joining:
      return;
  }
  changed();
}

void WifiSetup::long_press() {
  switch (mode_) {
    case Mode::List:
      mode_ = Mode::None;
      break;
    case Mode::Name:
    case Mode::Options:
      mode_ = Mode::List;
      break;
    case Mode::Security:
      keyboard_ = RingKeyboard(kMaxName, request_.name);
      mode_ = Mode::Name;
      break;
    case Mode::Password:
      mode_ = request_.hidden ? Mode::Security : Mode::List;
      break;
    case Mode::None:
    case Mode::Joining:
      return;
  }
  changed();
}

std::optional<std::string> WifiSetup::take_message() {
  auto m = std::move(message_);
  message_.reset();
  return m;
}

RoomView WifiSetup::view() const {
  RoomView v;
  v.offline = false;  // works without Home Assistant
  switch (mode_) {
    case Mode::List:
      v.screen = RoomView::Screen::WifiList;
      v.title = "Wi-Fi";
      if (!listed_) v.entries.push_back({"Searching\xE2\x80\xA6", "", true, "wifi", "wifi", Tone::Neutral});
      for (const WifiNetwork& n : networks_) {
        const bool unsupported = n.security == WifiSecurity::Unsupported;
        std::string summary = n.connected ? "Connected" : n.saved ? "Saved" : n.security_name;
        if (unsupported) summary = n.security_name + ", not supported";
        v.entries.push_back({n.name, summary, !unsupported, bars(n.strength), "wifi",
                             n.connected ? Tone::Active : Tone::Neutral});
      }
      v.entries.push_back({"Other network", "A hidden one", true, "wifi-plus", "wifi", Tone::Neutral});
      // "Searching" is only a placeholder: the selection skips it.
      v.selected = selected_ + (listed_ ? 0 : 1);
      return v;
    case Mode::Options:
      v.screen = RoomView::Screen::WifiList;
      v.title = chosen_.name;
      for (const Option o : options_) {
        if (o == Option::Join) {
          v.entries.push_back({"Join", "Use this network", true, bars(chosen_.strength), "wifi", Tone::Neutral});
        } else {
          v.entries.push_back({"Forget", chosen_.connected ? "Disconnects it" : "Its password goes",
                               true, "forget", "wifi", Tone::Neutral});
        }
      }
      v.selected = option_;
      return v;
    case Mode::Security:
      v.screen = RoomView::Screen::WifiList;
      v.title = "Security";
      v.entries.push_back({"WPA Personal", "With a password", true, "lock", "wifi", Tone::Neutral});
      v.entries.push_back({"Open", "No password", true, "wifi", "wifi", Tone::Neutral});
      v.selected = security_;
      return v;
    case Mode::Name:
    case Mode::Password:
      v.screen = RoomView::Screen::Keyboard;
      v.title = mode_ == Mode::Name ? "Network name" : "Password";
      v.item_name = mode_ == Mode::Name ? "" : request_.name;
      v.icon = mode_ == Mode::Name ? "wifi" : "lock";
      v.value = keyboard_.text();
      v.keys = keyboard_.keys();
      v.selected = keyboard_.selected();
      return v;
    case Mode::Joining:
      v.screen = RoomView::Screen::WifiBusy;
      v.title = "Wi-Fi";
      v.item_name = "Joining\n" + request_.name;
      v.prompt = "Up to a minute";
      return v;
    case Mode::None:
      break;
  }
  return v;
}

}  // namespace nesto::room
