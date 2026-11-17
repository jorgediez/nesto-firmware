#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/BaseReadings.hpp"
#include "core/Input.hpp"

namespace nesto::room {

/// One entry on the Technical info screen: a section's heading, or a label
/// with its value below (several lines for several addresses).
struct InfoLine {
  std::string label;
  std::string value;
  bool heading = false;

  bool operator==(const InfoLine& o) const {
    return label == o.label && value == o.value && heading == o.heading;
  }
};

/// Everything Technical info shows, gathered by the app. Empty strings and
/// empty optionals are left off the screen.
struct TechInfoInput {
  // Nesto
  std::string version;
  std::string panel_id;
  int port = 0;
  bool paired = false;
  Millis uptime_ms = 0;
  long free_ram_kb = -1;

  // Home Assistant
  std::string ha_status;  // "Connected", "Waiting for Home Assistant", ...
  bool ha_connected = false;
  std::string ha_version;
  std::string integration_version;
  std::optional<Millis> round_trip_ms;

  // Network
  struct Network {
    std::string ssid;
    std::string security;  // as net::security_name() puts it
    std::optional<int> rssi_dbm;
    std::optional<int> strength;  // connman's 0 to 100
    std::optional<int> freq_mhz;
    std::string interface;
    std::string ipv4;
    std::string netmask;
    std::string ipv4_router;
    std::vector<std::string> ipv6;
    std::string ipv6_router;
    std::vector<std::string> nameservers;
    std::vector<std::string> domains;
    std::string mac;
  } network;

  // The thermostat's base: power, sensors and its own details.
  bool has_base = false;
  BaseReadings base;

  // The display: the thermostat's head unit.
  std::string model;
  std::string hardware;
  std::string serial;
  std::optional<int> utc_offset_min;
};

/// The screen's entries, section by section: Nesto, Home Assistant,
/// Network, Power, Sensors, Display and Base. Power, Sensors and Base only
/// with a base.
std::vector<InfoLine> tech_info(const TechInfoInput& in);

/// "3 d 4 h", "4 h 12 min", "12 min", "40 s".
std::string format_duration(Millis ms);

/// "UTC+01:00", "UTC-03:30", "UTC".
std::string format_utc_offset(int minutes);

}  // namespace nesto::room
