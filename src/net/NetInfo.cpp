#include "net/NetInfo.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace nesto::net {
namespace {

std::string mac_of(const std::string& interface) {
  std::ifstream file("/sys/class/net/" + interface + "/address");
  std::string mac;
  std::getline(file, mac);
  mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
  if (mac.size() != 12 || mac == "000000000000") return "";
  std::transform(mac.begin(), mac.end(), mac.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return mac;
}

std::string ipv4_text(const sockaddr* address) {
  char text[INET_ADDRSTRLEN];
  const auto* in = reinterpret_cast<const sockaddr_in*>(address);
  return inet_ntop(AF_INET, &in->sin_addr, text, sizeof text) ? text : "";
}

}  // namespace

std::string panel_id_from_mac() {
  std::vector<std::string> names;
  if (DIR* dir = opendir("/sys/class/net")) {
    while (const dirent* entry = readdir(dir)) {
      const std::string name = entry->d_name;
      if (name != "." && name != ".." && name != "lo") names.push_back(name);
    }
    closedir(dir);
  }
  std::sort(names.begin(), names.end());
  const auto wlan = std::find(names.begin(), names.end(), "wlan0");
  if (wlan != names.end()) std::rotate(names.begin(), wlan, wlan + 1);
  for (const auto& name : names) {
    const std::string mac = mac_of(name);
    if (!mac.empty()) return mac;
  }
  return "";
}

std::optional<Ipv4> primary_ipv4_info() {
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return std::nullopt;
  std::optional<Ipv4> found;
  for (const ifaddrs* a = list; a && !found; a = a->ifa_next) {
    if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
    if ((a->ifa_flags & IFF_LOOPBACK) || !(a->ifa_flags & IFF_UP)) continue;
    Ipv4 ip{a->ifa_name ? a->ifa_name : "", ipv4_text(a->ifa_addr),
            a->ifa_netmask ? ipv4_text(a->ifa_netmask) : ""};
    if (!ip.address.empty()) found = ip;
  }
  freeifaddrs(list);
  return found;
}

std::string primary_ipv4() {
  const auto ip = primary_ipv4_info();
  return ip ? ip->address : "";
}

std::string mac_address(const std::string& interface) {
  const std::string mac = mac_of(interface);
  if (mac.empty()) return "";
  std::string out;
  for (std::size_t i = 0; i < mac.size(); i += 2) {
    if (i) out += ':';
    out += mac.substr(i, 2);
  }
  return out;
}

std::string parse_default_route(const std::string& text) {
  std::istringstream lines(text);
  std::string line;
  std::getline(lines, line);  // the header
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::string interface, destination, gateway, flags;
    if (!(fields >> interface >> destination >> gateway >> flags)) continue;
    // The default route: destination 0, with a gateway (RTF_GATEWAY).
    const unsigned long route_flags = std::strtoul(flags.c_str(), nullptr, 16);
    if (destination != "00000000" || !(route_flags & 0x2)) continue;
    // The kernel writes the address as it sits in memory, read as a number:
    // on the little-endian machines this runs on, the first octet is the
    // last pair of digits.
    const auto value = static_cast<std::uint32_t>(std::strtoul(gateway.c_str(), nullptr, 16));
    char out[INET_ADDRSTRLEN];
    std::snprintf(out, sizeof out, "%u.%u.%u.%u", value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF,
                  value >> 24);
    return out;
  }
  return "";
}

std::string default_ipv4_router() {
  std::ifstream file("/proc/net/route");
  std::stringstream text;
  text << file.rdbuf();
  return parse_default_route(text.str());
}

}  // namespace nesto::net
