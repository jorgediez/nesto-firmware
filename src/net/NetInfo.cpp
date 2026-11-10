#include "net/NetInfo.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <algorithm>
#include <cctype>
#include <fstream>
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

std::string primary_ipv4() {
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return "";
  std::string ip;
  for (const ifaddrs* a = list; a && ip.empty(); a = a->ifa_next) {
    if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
    if ((a->ifa_flags & IFF_LOOPBACK) || !(a->ifa_flags & IFF_UP)) continue;
    char text[INET_ADDRSTRLEN];
    const auto* in = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
    if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof text)) ip = text;
  }
  freeifaddrs(list);
  return ip;
}

}  // namespace nesto::net
