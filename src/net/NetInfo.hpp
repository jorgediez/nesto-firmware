#pragma once

#include <optional>
#include <string>

namespace nesto::net {

/// The panel ID of PROTOCOL.md section 2: the MAC address of the network
/// interface, lower-case hex without separators. Prefers wlan0 (the Nest's
/// Wi-Fi), then the first other interface with a real address. Empty if none.
std::string panel_id_from_mac();

/// The first non-loopback IPv4 address that's up, and its interface.
struct Ipv4 {
  std::string interface;
  std::string address;
  std::string netmask;
};
std::optional<Ipv4> primary_ipv4_info();

/// The first non-loopback IPv4 address, for showing on screen. Empty if none.
std::string primary_ipv4();

/// An interface's MAC address, "18:b4:30:19:90:84". Empty if it has none.
std::string mac_address(const std::string& interface);

/// The IPv4 default route's gateway in /proc/net/route's text, as
/// "192.168.1.1". Empty if there's none.
std::string parse_default_route(const std::string& text);

/// The IPv4 router, from /proc/net/route. Empty if there's none.
std::string default_ipv4_router();

}  // namespace nesto::net
