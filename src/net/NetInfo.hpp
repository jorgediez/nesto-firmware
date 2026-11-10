#pragma once

#include <string>

namespace nesto::net {

/// The panel ID of PROTOCOL.md section 2: the MAC address of the network
/// interface, lower-case hex without separators. Prefers wlan0 (the Nest's
/// Wi-Fi), then the first other interface with a real address. Empty if none.
std::string panel_id_from_mac();

/// The first non-loopback IPv4 address, for showing on screen. Empty if none.
std::string primary_ipv4();

}  // namespace nesto::net
