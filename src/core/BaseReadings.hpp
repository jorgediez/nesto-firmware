#pragma once

#include <optional>
#include <string>

namespace nesto {

/// What the thermostat's base reports (PROTOCOL.md section 17). Each reading
/// is empty until the base has sent it.
struct BaseReadings {
  bool connected = false;  // the base's session is running
  std::string firmware;    // the base's firmware version, empty if unknown
  std::optional<double> celsius;
  std::optional<double> humidity;   // percent
  std::optional<int> light_raw;     // higher is brighter (docs/BACKPLATE.md)
  std::optional<double> supply_v;   // from the heating's wires
  std::optional<double> usb_v;      // from a USB supply, as on a stand
  std::optional<double> battery_v;
};

}  // namespace nesto
