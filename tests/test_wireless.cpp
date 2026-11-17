// Reading the network and the device: the kernel's files, connman's D-Bus
// reply and the stock software's names, as the thermostat shows them
// (device-reports/wifi-check*.txt, with made-up names and addresses).
#include <doctest/doctest.h>

#include "core/DeviceIdentity.hpp"
#include "net/NetInfo.hpp"
#include "net/Wireless.hpp"

using namespace nesto;
using namespace nesto::net;

namespace {

const char* const kWireless =
    "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n"
    " face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n"
    " wlan0: 0000   59.  -51.  -256        0      0      0     11     99        0\n";

// GetServices, as dbus-send prints it: the Wi-Fi in use, the Thread radio
// (with its key, which must never be read), and a network nearby.
const char* const kServices = R"(method return time=1794832347.479491 sender=:1.4 -> destination=:1.9 serial=55721 reply_serial=2
   array [
      struct {
         object path "/net/connman/service/wifi_18b430000001_486f6d65_managed_psk"
         array [
            dict entry(
               string "Type"
               variant                   string "wifi"
            )
            dict entry(
               string "Security"
               variant                   array [
                     string "psk"
                     string "wps"
                  ]
            )
            dict entry(
               string "State"
               variant                   string "online"
            )
            dict entry(
               string "Strength"
               variant                   byte 67
            )
            dict entry(
               string "Favorite"
               variant                   boolean true
            )
            dict entry(
               string "Name"
               variant                   string "Home "Wi-Fi""
            )
            dict entry(
               string "Ethernet"
               variant                   array [
                     dict entry(
                        string "Method"
                        variant                            string "auto"
                     )
                     dict entry(
                        string "Interface"
                        variant                            string "wlan0"
                     )
                     dict entry(
                        string "Address"
                        variant                            string "18:B4:30:00:00:01"
                     )
                     dict entry(
                        string "MTU"
                        variant                            uint16 1500
                     )
                  ]
            )
            dict entry(
               string "IPv4"
               variant                   array [
                     dict entry(
                        string "Method"
                        variant                            string "dhcp"
                     )
                     dict entry(
                        string "Address"
                        variant                            string "192.168.1.50"
                     )
                     dict entry(
                        string "Netmask"
                        variant                            string "255.255.255.0"
                     )
                     dict entry(
                        string "Gateway"
                        variant                            string "192.168.1.1"
                     )
                  ]
            )
            dict entry(
               string "IPv6"
               variant                   array [
                     dict entry(
                        string "Method"
                        variant                            string "auto"
                     )
                     dict entry(
                        string "Address"
                        variant                            string "2001:db8::50"
                     )
                     dict entry(
                        string "PrefixLength"
                        variant                            byte 64
                     )
                     dict entry(
                        string "Gateway"
                        variant                            string "fe80::1"
                     )
                  ]
            )
            dict entry(
               string "Nameservers"
               variant                   array [
                     string "192.168.1.1"
                     string "2001:db8::1"
                  ]
            )
            dict entry(
               string "Domains"
               variant                   array [
                  ]
            )
         ]
      }
      struct {
         object path "/net/connman/service/lowpan_wpan0_4E455354_x35"
         array [
            dict entry(
               string "Type"
               variant                   string "lowpan"
            )
            dict entry(
               string "Passphrase"
               variant                   string "SECRET-THREAD-KEY"
            )
            dict entry(
               string "Name"
               variant                   string "NEST-PAN"
            )
         ]
      }
      struct {
         object path "/net/connman/service/wifi_18b430000001_4e6f_managed_none"
         array [
            dict entry(
               string "Type"
               variant                   string "wifi"
            )
            dict entry(
               string "Security"
               variant                   array [
                     string "none"
                  ]
            )
            dict entry(
               string "State"
               variant                   string "idle"
            )
            dict entry(
               string "Strength"
               variant                   byte 38
            )
            dict entry(
               string "Name"
               variant                   string "Café"
            )
         ]
      }
   ]
)";

}  // namespace

TEST_CASE("the signal level from /proc/net/wireless") {
  CHECK(parse_wireless_level(kWireless, "wlan0") == -51);
  CHECK_FALSE(parse_wireless_level(kWireless, "wlan1"));
  CHECK_FALSE(parse_wireless_level("", "wlan0"));
  // A driver giving the level as an unsigned byte.
  CHECK(parse_wireless_level(" wlan0: 0000   59.  205.  0  0 0 0 0 0 0\n", "wlan0") == -51);
  // Not associated.
  CHECK_FALSE(parse_wireless_level(" wlan0: 0000   0.  0.  0  0 0 0 0 0 0\n", "wlan0"));
}

TEST_CASE("frequencies from the wireless extensions") {
  CHECK(wireless_freq_mhz(2442000000L / 1000, 3) == 2442);  // 2.442 GHz
  CHECK(wireless_freq_mhz(5180, 6) == 5180);
  CHECK(wireless_freq_mhz(6, 0) == 2437);  // channel 6
  CHECK(wireless_freq_mhz(14, 0) == 2484);
  CHECK(wireless_freq_mhz(36, 0) == 5180);
  CHECK_FALSE(wireless_freq_mhz(0, 0));
}

TEST_CASE("connman's services, Wi-Fi only, never the Thread radio's key") {
  const auto services = parse_connman_services(kServices);
  REQUIRE(services.size() == 2);

  const NetService& home = services[0];
  CHECK(home.type == "wifi");
  CHECK(home.name == "Home \"Wi-Fi\"");  // a quote in a name reads whole
  CHECK(home.security == std::vector<std::string>{"psk", "wps"});
  CHECK(home.state == "online");
  CHECK(home.connected());
  CHECK(home.strength == 67);
  CHECK(home.favorite);
  CHECK(home.interface == "wlan0");
  CHECK(home.mac == "18:b4:30:00:00:01");
  CHECK(home.ipv4_address == "192.168.1.50");
  CHECK(home.ipv4_netmask == "255.255.255.0");
  CHECK(home.ipv4_gateway == "192.168.1.1");
  CHECK(home.ipv6_address == "2001:db8::50");
  CHECK(home.ipv6_prefix == 64);
  CHECK(home.ipv6_gateway == "fe80::1");
  CHECK(home.nameservers == std::vector<std::string>{"192.168.1.1", "2001:db8::1"});
  CHECK(home.domains.empty());

  const NetService& cafe = services[1];
  CHECK(cafe.name == "Café");
  CHECK_FALSE(cafe.connected());
  CHECK_FALSE(cafe.favorite);
  CHECK(security_name(cafe.security) == "Open");

  for (const auto& s : services) {
    CHECK(s.name.find("SECRET") == std::string::npos);
    CHECK(s.path.find("lowpan") == std::string::npos);
  }
  REQUIRE(current_service(services));
  CHECK(current_service(services)->name == home.name);
}

TEST_CASE("a reply that isn't one gives no services") {
  CHECK(parse_connman_services("").empty());
  CHECK(parse_connman_services("Error org.freedesktop.DBus.Error.ServiceUnknown: no connman\n").empty());
  CHECK(parse_connman_services("method return time=1\n   array [\n      struct {\n").empty());
  CHECK_FALSE(current_service({}));
}

TEST_CASE("security as it reads on screen") {
  CHECK(security_name({"psk", "wps"}) == "WPA Personal");
  CHECK(security_name({"none"}) == "Open");
  CHECK(security_name({"wep"}) == "WEP");
  CHECK(security_name({"ieee8021x"}) == "Enterprise");
  CHECK(security_name({"wps"}).empty());
}

TEST_CASE("the IPv4 router from /proc/net/route") {
  const char* const route =
      "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
      "wlan0\t0101A8C0\t00000000\t0005\t0\t0\t0\tFFFFFFFF\t0\t0\t0\n"
      "wlan0\t0001A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n"
      "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\n";
  CHECK(parse_default_route(route) == "192.168.1.1");
  CHECK(parse_default_route("Iface\tDestination\n").empty());
}

TEST_CASE("the thermostat's hardware, serial number and model") {
  const char* const cpuinfo =
      "Processor\t: ARMv7 Processor rev 2 (v7l)\n"
      "Hardware\t: Nest J49\n"
      "Revision\t: 020e\n"
      "Serial\t\t: 0000000000000000\n";
  CHECK(hardware_from_cpuinfo(cpuinfo) == "Nest J49, revision 020e");
  CHECK(hardware_from_cpuinfo("Hardware\t: Board\nRevision\t: 0000\n") == "Board");
  CHECK(hardware_from_cpuinfo("model name\t: x86\n").empty());
  CHECK(serial_from_hostname("02AA01AC00000001") == "02AA01AC00000001");
  CHECK(serial_from_hostname("j49").empty());
  CHECK(serial_from_hostname("02aa01ac00000001").empty());
  // The thermostat's own command line.
  CHECK(model_from_cmdline("root=/dev/mtdblock7 rootfstype=jffs2 console= nlmodel=Display-2.14 brightness=48 "
                           "panic=1\n") == "Display-2.14");
  CHECK(model_from_cmdline("console=ttyS0 root=/dev/sda1\n").empty());
}
