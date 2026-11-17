#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/WifiTypes.hpp"
#include "room/Controller.hpp"
#include "room/RingKeyboard.hpp"

namespace nesto::room {

/// Settings → Wi-Fi: the networks around, strongest first, with the one in
/// use at the top, and "Other network" for a hidden one. Choosing a network
/// asks for its password on the ring keyboard when it needs one, then joins
/// it; the app does the joining and reports back with set_state(). A saved
/// network offers Join and Forget instead.
///
/// The ring moves through the list, a press chooses, a long press goes back
/// a step: from the password to the list, from the list out of Wi-Fi.
class WifiSetup {
 public:
  struct Actions {
    std::function<void()> scan;
    std::function<void(const WifiConnect&)> connect;
    std::function<void(const WifiNetwork&)> forget;
  };

  static constexpr std::size_t kMaxName = 32;      // bytes, as Wi-Fi allows
  static constexpr std::size_t kMaxPassword = 64;  // 63 characters, or 64 hex digits

  explicit WifiSetup(Actions actions);

  void open();
  void close();
  bool active() const { return mode_ != Mode::None; }
  /// On the list, where it's worth scanning now and then.
  bool listing() const { return mode_ == Mode::List; }
  /// Joining a network: input waits until it's done.
  bool joining() const { return mode_ == Mode::Joining; }

  /// The networks found; `listed` once a scan has finished.
  void set_networks(const std::vector<WifiNetwork>& networks, bool listed);
  /// How joining went.
  void set_state(WifiState state);

  void rotate(int clicks);
  void press();
  void long_press();

  RoomView view() const;
  unsigned version() const { return version_; }
  /// A message to show briefly; taken once.
  std::optional<std::string> take_message();

 private:
  // Options: what to do with a saved network, Join (unless it's the one in
  // use) or Forget.
  enum class Mode { None, List, Options, Name, Security, Password, Joining };
  enum class Option { Join, Forget };

  void join(const WifiConnect& request);
  void changed() { ++version_; }
  int list_size() const { return static_cast<int>(networks_.size()) + 1; }  // and Other network

  Actions actions_;
  Mode mode_ = Mode::None;
  std::vector<WifiNetwork> networks_;
  bool listed_ = false;
  int selected_ = 0;
  int security_ = 0;  // for a hidden network: 0 WPA Personal, 1 Open
  WifiNetwork chosen_;  // the saved network the options are for
  std::vector<Option> options_;
  int option_ = 0;
  RingKeyboard keyboard_;

  // The network being set up.
  WifiConnect request_;
  std::optional<std::string> message_;
  unsigned version_ = 1;
};

}  // namespace nesto::room
