#include "core/DeviceIdentity.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace nesto {
namespace {

std::string trim(const std::string& s) {
  const auto start = s.find_first_not_of(" \t");
  if (start == std::string::npos) return "";
  return s.substr(start, s.find_last_not_of(" \t\r") - start + 1);
}

}  // namespace

std::string hardware_from_cpuinfo(const std::string& text) {
  std::istringstream lines(text);
  std::string line;
  std::string hardware;
  std::string revision;
  while (std::getline(lines, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = trim(line.substr(0, colon));
    const std::string value = trim(line.substr(colon + 1));
    if (key == "Hardware") hardware = value;
    if (key == "Revision") revision = value;
  }
  if (hardware.empty()) return "";
  // A revision of zeros says nothing.
  if (revision.empty() || revision.find_first_not_of('0') == std::string::npos) return hardware;
  return hardware + ", revision " + revision;
}

std::string serial_from_hostname(const std::string& hostname) {
  if (hostname.size() != 16) return "";
  const bool hex = std::all_of(hostname.begin(), hostname.end(), [](unsigned char c) {
    return std::isdigit(c) || (c >= 'A' && c <= 'F');
  });
  return hex ? hostname : "";
}

DeviceIdentity read_device_identity() {
  DeviceIdentity identity;
  std::ifstream file("/proc/cpuinfo");
  std::stringstream text;
  text << file.rdbuf();
  identity.hardware = hardware_from_cpuinfo(text.str());
  char name[256] = {};
  if (gethostname(name, sizeof name - 1) == 0) identity.serial = serial_from_hostname(name);
  return identity;
}

}  // namespace nesto
