#include "net/Wireless.hpp"

#include <linux/wireless.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace nesto::net {
namespace {

// dbus-send's printed reply, as a tree. Only what GetServices returns is
// needed: strings, numbers, booleans, arrays, structs and dict entries;
// variants are unwrapped as they're read.
struct Value {
  enum class Kind { Text, Number, Boolean, Array, Struct, Entry };
  Kind kind = Kind::Text;
  std::string text;
  double number = 0;
  bool boolean = false;
  std::vector<Value> items;  // Array and Struct; an Entry's key and value
};

/// The reply's words, with each quoted string as one token (starting with
/// its quote). A string ends at the last quote on its line, so a network
/// name with a quote in it still reads whole.
std::vector<std::string> tokens(const std::string& reply) {
  std::vector<std::string> out;
  std::istringstream lines(reply);
  std::string line;
  bool header = true;
  while (std::getline(lines, line)) {
    if (header) {  // "method return time=... sender=..."
      header = false;
      if (line.rfind("method return", 0) == 0) continue;
    }
    std::size_t i = 0;
    while (i < line.size()) {
      if (std::isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
      } else if (line[i] == '"') {
        const std::size_t end = line.find_last_of('"');
        if (end == i) return {};  // an unterminated string
        out.push_back(line.substr(i, end - i));  // the opening quote marks it
        i = end + 1;
      } else {
        const std::size_t end = std::min(line.find_first_of(" \t", i), line.size());
        out.push_back(line.substr(i, end - i));
        i = end;
      }
    }
  }
  return out;
}

class Parser {
 public:
  explicit Parser(std::vector<std::string> tokens) : tokens_(std::move(tokens)) {}

  bool done() const { return at_ >= tokens_.size(); }

  bool value(Value& out, int depth = 0) {
    if (depth > 16 || done()) return false;
    const std::string word = tokens_[at_++];
    if (word == "variant") return value(out, depth + 1);
    if (word == "string") return text(out);
    if (word == "object") return expect("path") && text(out);
    if (word == "boolean") {
      if (done()) return false;
      out.kind = Value::Kind::Boolean;
      out.boolean = tokens_[at_++] == "true";
      return true;
    }
    if (word == "byte" || word == "int16" || word == "uint16" || word == "int32" || word == "uint32" ||
        word == "int64" || word == "uint64" || word == "double") {
      if (done()) return false;
      out.kind = Value::Kind::Number;
      out.number = std::strtod(tokens_[at_++].c_str(), nullptr);
      return true;
    }
    if (word == "array") return list(out, Value::Kind::Array, "[", "]", depth);
    if (word == "struct") return list(out, Value::Kind::Struct, "{", "}", depth);
    if (word == "dict") {
      if (!expect("entry(")) return false;
      out.kind = Value::Kind::Entry;
      out.items.resize(2);
      return value(out.items[0], depth + 1) && value(out.items[1], depth + 1) && expect(")");
    }
    return false;
  }

 private:
  bool expect(const char* word) {
    if (done() || tokens_[at_] != word) return false;
    ++at_;
    return true;
  }

  bool text(Value& out) {
    if (done() || tokens_[at_].empty() || tokens_[at_][0] != '"') return false;
    out.kind = Value::Kind::Text;
    out.text = tokens_[at_++].substr(1);
    return true;
  }

  bool list(Value& out, Value::Kind kind, const char* open, const char* close, int depth) {
    if (!expect(open)) return false;
    out.kind = kind;
    while (!done() && tokens_[at_] != close) {
      out.items.emplace_back();
      if (!value(out.items.back(), depth + 1)) return false;
    }
    return expect(close);
  }

  std::vector<std::string> tokens_;
  std::size_t at_ = 0;
};

/// A dictionary's entries by key, from an array of dict entries.
std::map<std::string, const Value*> entries(const Value& array) {
  std::map<std::string, const Value*> out;
  if (array.kind != Value::Kind::Array) return out;
  for (const Value& entry : array.items) {
    if (entry.kind == Value::Kind::Entry && entry.items[0].kind == Value::Kind::Text) {
      out[entry.items[0].text] = &entry.items[1];
    }
  }
  return out;
}

std::string text_of(const std::map<std::string, const Value*>& d, const char* key) {
  const auto it = d.find(key);
  return it != d.end() && it->second->kind == Value::Kind::Text ? it->second->text : "";
}

double number_of(const std::map<std::string, const Value*>& d, const char* key) {
  const auto it = d.find(key);
  return it != d.end() && it->second->kind == Value::Kind::Number ? it->second->number : 0;
}

std::vector<std::string> texts_of(const std::map<std::string, const Value*>& d, const char* key) {
  std::vector<std::string> out;
  const auto it = d.find(key);
  if (it == d.end() || it->second->kind != Value::Kind::Array) return out;
  for (const Value& v : it->second->items) {
    if (v.kind == Value::Kind::Text) out.push_back(v.text);
  }
  return out;
}

std::map<std::string, const Value*> dict_of(const std::map<std::string, const Value*>& d, const char* key) {
  const auto it = d.find(key);
  return it != d.end() ? entries(*it->second) : std::map<std::string, const Value*>{};
}

/// Runs a fixed command and returns what it printed. Nothing from outside
/// ever goes into the command.
std::string output_of(const char* command) {
  std::string out;
  FILE* pipe = popen(command, "r");
  if (!pipe) return out;
  char buffer[4096];
  std::size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) out.append(buffer, n);
  pclose(pipe);
  return out;
}

}  // namespace

std::optional<int> parse_wireless_level(const std::string& text, const std::string& interface) {
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::string name;
    fields >> name;
    if (name != interface + ":") continue;
    std::string status, link, level;
    if (!(fields >> status >> link >> level)) return std::nullopt;
    char* end = nullptr;
    const double value = std::strtod(level.c_str(), &end);
    if (end == level.c_str()) return std::nullopt;
    int dbm = static_cast<int>(std::lround(value));
    // Some drivers give the level as an unsigned byte: 205 is -51 dBm.
    if (dbm > 0 && dbm < 256) dbm -= 256;
    // 0 is what an interface that isn't associated reports.
    if (dbm == 0 || dbm < -120) return std::nullopt;
    return dbm;
  }
  return std::nullopt;
}

std::optional<int> wireless_freq_mhz(long mantissa, int exponent) {
  if (mantissa <= 0 || exponent < 0) return std::nullopt;
  if (exponent == 0 && mantissa < 1000) {
    // A channel number.
    if (mantissa == 14) return 2484;
    if (mantissa < 14) return 2407 + 5 * static_cast<int>(mantissa);
    return 5000 + 5 * static_cast<int>(mantissa);
  }
  double hz = static_cast<double>(mantissa);
  for (int i = 0; i < exponent; ++i) hz *= 10;
  const auto mhz = static_cast<int>(std::lround(hz / 1e6));
  if (mhz <= 0) return std::nullopt;
  return mhz;
}

WifiLink read_wifi_link(const std::string& interface) {
  WifiLink link;
  {
    std::ifstream file("/proc/net/wireless");
    std::stringstream text;
    text << file.rdbuf();
    link.rssi_dbm = parse_wireless_level(text.str(), interface);
  }
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return link;
  iwreq request{};
  std::strncpy(request.ifr_name, interface.c_str(), IFNAMSIZ - 1);
  char essid[IW_ESSID_MAX_SIZE + 1] = {};
  request.u.essid.pointer = essid;
  request.u.essid.length = IW_ESSID_MAX_SIZE;
  if (ioctl(fd, SIOCGIWESSID, &request) == 0) {
    link.ssid.assign(essid, std::min<std::size_t>(request.u.essid.length, IW_ESSID_MAX_SIZE));
  }
  iwreq freq{};
  std::strncpy(freq.ifr_name, interface.c_str(), IFNAMSIZ - 1);
  if (ioctl(fd, SIOCGIWFREQ, &freq) == 0) link.freq_mhz = wireless_freq_mhz(freq.u.freq.m, freq.u.freq.e);
  close(fd);
  // Not associated: no network, so no frequency or signal to speak of.
  if (link.ssid.empty()) link = {};
  return link;
}

std::vector<NetService> parse_connman_services(const std::string& reply) {
  std::vector<NetService> out;
  Parser parser(tokens(reply));
  Value services;
  if (!parser.value(services) || services.kind != Value::Kind::Array) return out;
  for (const Value& s : services.items) {
    if (s.kind != Value::Kind::Struct || s.items.size() != 2 || s.items[0].kind != Value::Kind::Text) continue;
    const auto d = entries(s.items[1]);
    NetService service;
    service.type = text_of(d, "Type");
    if (service.type != "wifi" && service.type != "ethernet") continue;
    service.path = s.items[0].text;
    service.name = text_of(d, "Name");
    service.security = texts_of(d, "Security");
    service.state = text_of(d, "State");
    service.strength = std::clamp(static_cast<int>(number_of(d, "Strength")), 0, 100);
    const auto favorite = d.find("Favorite");
    service.favorite = favorite != d.end() && favorite->second->kind == Value::Kind::Boolean &&
                       favorite->second->boolean;
    const auto ethernet = dict_of(d, "Ethernet");
    service.interface = text_of(ethernet, "Interface");
    service.mac = text_of(ethernet, "Address");
    std::transform(service.mac.begin(), service.mac.end(), service.mac.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto ipv4 = dict_of(d, "IPv4");
    service.ipv4_method = text_of(ipv4, "Method");
    service.ipv4_address = text_of(ipv4, "Address");
    service.ipv4_netmask = text_of(ipv4, "Netmask");
    service.ipv4_gateway = text_of(ipv4, "Gateway");
    const auto ipv6 = dict_of(d, "IPv6");
    service.ipv6_method = text_of(ipv6, "Method");
    service.ipv6_address = text_of(ipv6, "Address");
    service.ipv6_prefix = static_cast<int>(number_of(ipv6, "PrefixLength"));
    service.ipv6_gateway = text_of(ipv6, "Gateway");
    service.nameservers = texts_of(d, "Nameservers");
    service.domains = texts_of(d, "Domains");
    out.push_back(std::move(service));
  }
  return out;
}

std::vector<NetService> connman_services() {
  return parse_connman_services(
      output_of("dbus-send --system --print-reply --reply-timeout=2000 --dest=net.connman / "
                "net.connman.Manager.GetServices 2> /dev/null"));
}

std::optional<NetService> current_service(const std::vector<NetService>& services) {
  for (const NetService& s : services) {
    if (s.connected()) return s;
  }
  return std::nullopt;
}

std::string security_name(const std::vector<std::string>& security) {
  const auto has = [&](const char* s) { return std::find(security.begin(), security.end(), s) != security.end(); };
  if (has("ieee8021x")) return "Enterprise";
  if (has("psk")) return "WPA Personal";
  if (has("wep")) return "WEP";
  if (has("none")) return "Open";
  return "";
}

}  // namespace nesto::net
