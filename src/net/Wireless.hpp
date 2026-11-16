#pragma once

#include <optional>
#include <string>
#include <vector>

namespace nesto::net {

/// The Wi-Fi link as the kernel reports it now.
struct WifiLink {
  std::string ssid;               // empty when not associated
  std::optional<int> rssi_dbm;    // signal level
  std::optional<int> freq_mhz;
};

/// The signal level of `interface` in /proc/net/wireless's text, in dBm.
/// Empty if the interface isn't listed or reports no level.
std::optional<int> parse_wireless_level(const std::string& text, const std::string& interface);

/// A frequency as the wireless extensions give it (mantissa and exponent, in
/// Hz, or a channel number when the exponent is 0 and the mantissa small),
/// in MHz.
std::optional<int> wireless_freq_mhz(long mantissa, int exponent);

/// The link of `interface`: /proc/net/wireless for the signal, the wireless
/// extensions' ioctls for the network name and frequency, as iwconfig does.
/// Empty fields where there's no Wi-Fi, as in the simulator.
WifiLink read_wifi_link(const std::string& interface = "wlan0");

/// One of connman's services, from its D-Bus API (net.connman.Manager's
/// GetServices). Only Wi-Fi and Ethernet are kept: the thermostat's Thread
/// radio is a connman service too, and connman hands out its network key
/// with it, which is never read.
struct NetService {
  std::string path;  // the D-Bus object path, which names the service
  std::string type;  // "wifi" or "ethernet"
  std::string name;  // the network's name
  std::vector<std::string> security;  // "none", "wep", "psk", "ieee8021x", "wps"
  std::string state;  // idle, failure, association, configuration, ready, disconnect, online
  int strength = 0;   // 0 to 100
  bool favorite = false;  // saved
  std::string interface;
  std::string mac;
  std::string ipv4_method;
  std::string ipv4_address;
  std::string ipv4_netmask;
  std::string ipv4_gateway;
  std::string ipv6_method;
  std::string ipv6_address;
  int ipv6_prefix = 0;
  std::string ipv6_gateway;
  std::vector<std::string> nameservers;
  std::vector<std::string> domains;

  bool connected() const { return state == "ready" || state == "online"; }
};

/// Reads `dbus-send --print-reply`'s text for GetServices. Anything it can't
/// read gives an empty list.
std::vector<NetService> parse_connman_services(const std::string& reply);

/// Asks connman for its services, through dbus-send, waiting at most two
/// seconds. Empty without connman, as in the simulator.
std::vector<NetService> connman_services();

/// The service in use: the first connected one (connman lists it first).
std::optional<NetService> current_service(const std::vector<NetService>& services);

/// How a network's security reads on screen: "Open", "WEP", "WPA Personal",
/// "Enterprise". WPS alone says nothing about the key, so it's ignored.
std::string security_name(const std::vector<std::string>& security);

}  // namespace nesto::net
