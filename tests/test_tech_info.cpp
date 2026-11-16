// Technical info's entries: the sections, and what's left out.
#include <doctest/doctest.h>

#include <algorithm>

#include "room/TechInfo.hpp"

using namespace nesto;
using namespace nesto::room;

namespace {

std::vector<std::string> headings(const std::vector<InfoLine>& lines) {
  std::vector<std::string> out;
  for (const auto& l : lines) {
    if (l.heading) out.push_back(l.label);
  }
  return out;
}

/// The value of `label` in `section`, or "(none)".
std::string value(const std::vector<InfoLine>& lines, const std::string& section, const std::string& label) {
  bool in = false;
  for (const auto& l : lines) {
    if (l.heading) in = l.label == section;
    else if (in && l.label == label) return l.value;
  }
  return "(none)";
}

TechInfoInput thermostat() {
  TechInfoInput in;
  in.version = "0.1.0";
  in.panel_id = "18b430000001";
  in.port = 6170;
  in.paired = true;
  in.uptime_ms = (2 * 86400 + 3 * 3600 + 5) * 1000LL;
  in.free_ram_kb = 20480;
  in.ha_status = "Connected";
  in.ha_connected = true;
  in.ha_version = "2026.11.1";
  in.integration_version = "0.1.0";
  in.round_trip_ms = 12;
  auto& n = in.network;
  n.ssid = "Home";
  n.security = "WPA Personal";
  n.rssi_dbm = -51;
  n.strength = 67;
  n.freq_mhz = 2442;
  n.interface = "wlan0";
  n.ipv4 = "192.168.1.50";
  n.netmask = "255.255.255.0";
  n.ipv4_router = "192.168.1.1";
  n.ipv6 = {"2001:db8::50/64"};
  n.ipv6_router = "fe80::1";
  n.nameservers = {"192.168.1.1", "2001:db8::1"};
  n.mac = "18:b4:30:00:00:01";
  in.has_base = true;
  in.base.connected = true;
  in.base.firmware = "1.2";
  in.base.model = "Backplate-3.2";
  in.base.serial = "0123";
  in.base.celsius = 21.46;
  in.base.humidity = 44.6;
  in.base.light_raw = 300;
  in.base.battery_v = 3.912;
  in.base.usb_v = 4.94;
  in.base.supply_v = 0.0;
  in.hardware = "Nest J49, revision 020e";
  in.serial = "02AA01AC00000001";
  in.utc_offset_min = 60;
  return in;
}

}  // namespace

TEST_CASE("the thermostat's Technical info, section by section") {
  const auto lines = tech_info(thermostat());
  CHECK(headings(lines) ==
        std::vector<std::string>{"Nesto", "Home Assistant", "Network", "Power", "Sensors", "Display", "Base"});
  CHECK(value(lines, "Nesto", "Version") == "0.1.0");
  CHECK(value(lines, "Nesto", "Port") == "6170");
  CHECK(value(lines, "Nesto", "Pairing") == "Paired");
  CHECK(value(lines, "Nesto", "Running for") == "2 d 3 h");
  CHECK(value(lines, "Nesto", "Free memory") == "20 MB");
  CHECK(value(lines, "Home Assistant", "Version") == "2026.11.1");
  CHECK(value(lines, "Home Assistant", "Integration") == "ha-nesto 0.1.0");
  CHECK(value(lines, "Home Assistant", "Round trip") == "12 ms");
  CHECK(value(lines, "Network", "Wi-Fi") == "Home");
  CHECK(value(lines, "Network", "Signal") == "-51 dBm, 67%");
  CHECK(value(lines, "Network", "Frequency") == "2.442 GHz");
  CHECK(value(lines, "Network", "Name servers") == "192.168.1.1\n2001:db8::1");
  CHECK(value(lines, "Network", "Search domains") == "(none)");  // none to show
  CHECK(value(lines, "Power", "Battery") == "3.91 V");
  CHECK(value(lines, "Power", "Supply") == "0.00 V");
  CHECK(value(lines, "Sensors", "Temperature") == "21.5 \xC2\xB0" "C");
  CHECK(value(lines, "Sensors", "Humidity") == "45%");
  CHECK(value(lines, "Display", "Serial number") == "02AA01AC00000001");
  CHECK(value(lines, "Display", "Time zone") == "UTC+01:00");
  CHECK(value(lines, "Base", "Model") == "Backplate-3.2");
}

TEST_CASE("without a base or Home Assistant, less to show") {
  TechInfoInput in = thermostat();
  in.has_base = false;
  in.ha_connected = false;
  in.ha_status = "Waiting for Home Assistant";
  in.network = {};
  const auto lines = tech_info(in);
  CHECK(headings(lines) == std::vector<std::string>{"Nesto", "Home Assistant", "Network", "Display"});
  CHECK(value(lines, "Home Assistant", "Status") == "Waiting for Home Assistant");
  CHECK(value(lines, "Home Assistant", "Version") == "(none)");
  CHECK(value(lines, "Network", "Wi-Fi") == "Not connected");
  CHECK(value(lines, "Network", "Signal") == "(none)");
}

TEST_CASE("durations and time zones") {
  CHECK(format_duration(40 * 1000) == "40 s");
  CHECK(format_duration(12 * 60 * 1000) == "12 min");
  CHECK(format_duration((4 * 3600 + 12 * 60) * 1000LL) == "4 h 12 min");
  CHECK(format_utc_offset(0) == "UTC");
  CHECK(format_utc_offset(-210) == "UTC-03:30");
  CHECK(format_utc_offset(330) == "UTC+05:30");
}
