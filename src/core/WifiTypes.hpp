#pragma once

#include <string>

namespace nesto {

/// What the panel can join: open networks and WPA Personal ones. Others
/// (WEP, Enterprise) are listed but not offered.
enum class WifiSecurity { Open, Wpa, Unsupported };

struct WifiNetwork {
  std::string path;  // connman's service, to connect to
  std::string name;
  WifiSecurity security = WifiSecurity::Open;
  std::string security_name;  // "Open", "WPA Personal", "Enterprise", ...
  int strength = 0;           // 0 to 100
  bool saved = false;         // connman knows its password
  bool connected = false;
  bool provisioned = false;   // connman took a provisioning file for it
};

/// What to join. A hidden network has no path: it's found by name once
/// connman knows to look for it.
struct WifiConnect {
  std::string path;
  std::string name;
  WifiSecurity security = WifiSecurity::Wpa;
  std::string passphrase;  // empty: open, or already saved
  bool hidden = false;
};

enum class WifiState { Idle, Connecting, Connected, Failed };

}  // namespace nesto
