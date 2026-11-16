#pragma once

#include <string>

namespace nesto {

/// What the device says about itself, for Technical info.
struct DeviceIdentity {
  std::string hardware;  // "Nest J49, revision 020e"
  std::string serial;    // the Nest's serial number, empty if unknown
};

/// The hardware's name and revision from /proc/cpuinfo's text.
std::string hardware_from_cpuinfo(const std::string& text);

/// The serial number, if `hostname` is one: the stock software names the
/// thermostat after it, 16 hexadecimal digits such as 02AA01AC091503D1.
/// Empty otherwise.
std::string serial_from_hostname(const std::string& hostname);

/// Both, from this machine.
DeviceIdentity read_device_identity();

}  // namespace nesto
