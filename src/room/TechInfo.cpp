#include "room/TechInfo.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace nesto::room {
namespace {

std::string fixed(double value, int decimals, const char* unit) {
  char text[32];
  std::snprintf(text, sizeof text, "%.*f%s", decimals, value, unit);
  return text;
}

std::string joined(const std::vector<std::string>& values) {
  std::string out;
  for (const auto& v : values) out += (out.empty() ? "" : "\n") + v;
  return out;
}

class Lines {
 public:
  void heading(const char* name) { out_.push_back({name, "", true}); }
  void add(const char* label, const std::string& value) {
    if (!value.empty()) out_.push_back({label, value, false});
  }
  std::vector<InfoLine> take() { return std::move(out_); }

 private:
  std::vector<InfoLine> out_;
};

std::string volts(const std::optional<double>& v) {
  return v ? fixed(*v, 2, " V") : "";
}

}  // namespace

std::string format_duration(Millis ms) {
  const Millis s = ms / 1000;
  const Millis d = s / 86400;
  const Millis h = s % 86400 / 3600;
  const Millis m = s % 3600 / 60;
  if (d > 0) return std::to_string(d) + " d " + std::to_string(h) + " h";
  if (h > 0) return std::to_string(h) + " h " + std::to_string(m) + " min";
  if (m > 0) return std::to_string(m) + " min";
  return std::to_string(s) + " s";
}

std::string format_utc_offset(int minutes) {
  if (minutes == 0) return "UTC";
  char text[16];
  const int a = std::abs(minutes);
  std::snprintf(text, sizeof text, "UTC%c%02d:%02d", minutes < 0 ? '-' : '+', a / 60, a % 60);
  return text;
}

std::vector<InfoLine> tech_info(const TechInfoInput& in) {
  Lines l;

  l.heading("Nesto");
  l.add("Version", in.version);
  l.add("Panel ID", in.panel_id);
  l.add("Port", in.port > 0 ? std::to_string(in.port) : "");
  l.add("Pairing", in.paired ? "Paired" : "Not paired");
  l.add("Running for", format_duration(in.uptime_ms));
  l.add("Free memory", in.free_ram_kb >= 0 ? std::to_string(in.free_ram_kb / 1024) + " MB" : "");

  l.heading("Home Assistant");
  l.add("Status", in.ha_status);
  if (in.ha_connected) {
    l.add("Version", in.ha_version);
    l.add("Integration", in.integration_version.empty() ? "" : "ha-nesto " + in.integration_version);
    l.add("Round trip", in.round_trip_ms ? std::to_string(*in.round_trip_ms) + " ms" : "");
  }

  const auto& n = in.network;
  l.heading("Network");
  l.add("Wi-Fi", n.ssid.empty() ? "Not connected" : n.ssid);
  l.add("Security", n.security);
  if (n.rssi_dbm || n.strength) {
    std::string signal = n.rssi_dbm ? std::to_string(*n.rssi_dbm) + " dBm" : "";
    if (n.strength) signal += (signal.empty() ? "" : ", ") + std::to_string(*n.strength) + "%";
    l.add("Signal", signal);
  }
  l.add("Frequency", n.freq_mhz ? fixed(*n.freq_mhz / 1000.0, 3, " GHz") : "");
  l.add("Interface", n.interface);
  l.add("IPv4 address", n.ipv4);
  l.add("Netmask", n.netmask);
  l.add("Router", n.ipv4_router);
  l.add("IPv6 address", joined(n.ipv6));
  l.add("IPv6 router", n.ipv6_router);
  l.add("Name servers", joined(n.nameservers));
  l.add("Search domains", joined(n.domains));
  l.add("MAC address", n.mac);

  const BaseReadings& b = in.base;
  if (in.has_base) {
    l.heading("Power");
    l.add("Battery", volts(b.battery_v));
    l.add("USB", volts(b.usb_v));
    l.add("Supply", volts(b.supply_v));

    l.heading("Sensors");
    l.add("Temperature", b.celsius ? fixed(*b.celsius, 1, " \xC2\xB0" "C") : "");
    l.add("Humidity", b.humidity ? fixed(*b.humidity, 0, "%") : "");
    l.add("Light", b.light_raw ? std::to_string(*b.light_raw) : "");
  }

  l.heading("Display");
  l.add("Model", in.model);
  l.add("Hardware", in.hardware);
  l.add("Serial number", in.serial);
  l.add("Time zone", in.utc_offset_min ? format_utc_offset(*in.utc_offset_min) : "");

  if (in.has_base) {
    l.heading("Base");
    l.add("Status", b.connected ? "Connected" : "Not connected");
    l.add("Software", b.firmware);
    l.add("Model", b.model);
    l.add("Serial number", b.serial);
  }
  return l.take();
}

}  // namespace nesto::room
