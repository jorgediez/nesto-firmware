#include "net/Wifi.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "core/Log.hpp"

namespace nesto::net {
namespace {

// How long connman may take: a scan, a listing, and joining a network.
constexpr Millis kScanTimeout = 20000;
constexpr Millis kListTimeout = 5000;
constexpr Millis kConnectTimeout = 50000;
// A new provisioning file is read soon but not at once: a moment first,
// then connman is asked every half second whether the network has its
// password, for up to 10 s. A network saved before, whose password is being
// changed, gets 3 s, as connman shows nothing new for it.
constexpr Millis kSettle = 1000;
constexpr Millis kCheckEvery = 500;
constexpr Millis kTakeTime = 10000;
constexpr Millis kResettle = 3000;
// While joining, how often connman is asked how it's going.
constexpr Millis kWatchEvery = 2000;

std::vector<std::string> dbus(const std::string& path, const std::string& method, Millis reply_timeout) {
  return {"dbus-send",
          "--system",
          "--print-reply",
          "--reply-timeout=" + std::to_string(reply_timeout),
          "--dest=net.connman",
          path,
          method};
}

/// A failed command's first line, for the log: dbus-send's error, such as
/// "Error net.connman.Error.InvalidArguments: Invalid arguments".
std::string first_line(const std::string& output) {
  const std::string line = output.substr(0, output.find('\n'));
  return line.empty() ? "no output" : line.substr(0, 200);
}

std::string hex(const std::string& text) {
  static const char* const digits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : text) {
    out += digits[c >> 4];
    out += digits[c & 0xF];
  }
  return out;
}

/// A value for a GLib key file, which connman's config files are: what
/// would otherwise be lost (backslashes, spaces at either end, tabs) escaped.
std::string key_file_value(const std::string& value) {
  std::string out;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c == ' ' && (i == 0 || i + 1 == value.size())) {
      out += "\\s";
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

void WifiBackend::set_networks(std::vector<WifiNetwork> networks) {
  networks_ = std::move(networks);
  listed_ = true;
  ++version_;
}

void WifiBackend::set_state(WifiState state) {
  if (state == state_) return;
  state_ = state;
  ++version_;
}

std::vector<WifiNetwork> wifi_networks(const std::vector<NetService>& services) {
  std::vector<WifiNetwork> out;
  for (const NetService& s : services) {
    if (s.type != "wifi" || s.name.empty()) continue;  // hidden ones have no name
    WifiNetwork n;
    n.path = s.path;
    n.name = s.name;
    n.security_name = security_name(s.security);
    if (n.security_name == "Open") {
      n.security = WifiSecurity::Open;
    } else if (n.security_name == "WPA Personal") {
      n.security = WifiSecurity::Wpa;
    } else {
      n.security = WifiSecurity::Unsupported;
    }
    n.strength = s.strength;
    n.saved = s.favorite;
    n.connected = s.connected();
    n.provisioned = s.immutable;
    out.push_back(std::move(n));
  }
  std::stable_sort(out.begin(), out.end(), [](const WifiNetwork& a, const WifiNetwork& b) {
    if (a.connected != b.connected) return a.connected;
    return a.strength > b.strength;
  });
  return out;
}

std::string connman_config_name(const std::string& name) {
  return "nesto-" + hex(name) + ".config";
}

std::string connman_config(const std::string& name, const std::string& passphrase, bool hidden) {
  std::string out = "# Written by Nesto's Wi-Fi setup.\n";
  out += "[service_nesto_" + hex(name) + "]\n";
  out += "Type = wifi\n";
  out += "SSID = " + hex(name) + "\n";
  if (passphrase.empty()) {
    out += "Security = none\n";
  } else {
    out += "Security = psk\n";
    out += "Passphrase = " + key_file_value(passphrase) + "\n";
  }
  if (hidden) out += "Hidden = true\n";
  return out;
}

// ConnmanWifi ---------------------------------------------------------------------

ConnmanWifi::ConnmanWifi(std::unique_ptr<CommandRunner> runner, std::string config_dir)
    : runner_(std::move(runner)), config_dir_(std::move(config_dir)) {}

void ConnmanWifi::run(Step step, const std::vector<std::string>& argv, Millis timeout, Millis now) {
  step_ = step;
  if (!runner_->start(argv, timeout, now)) {
    // dbus-send missing, or something else: as if it had failed.
    finished(CommandResult{false, ""}, now);
  }
}

void ConnmanWifi::scan(Millis now) {
  if (step_ != Step::None || state_ == WifiState::Connecting) return;
  run(Step::Scan, dbus("/net/connman/technology/wifi", "net.connman.Technology.Scan", kScanTimeout - 1000),
      kScanTimeout, now);
}

void ConnmanWifi::list(Millis now) {
  run(Step::List, dbus("/", "net.connman.Manager.GetServices", kListTimeout - 1000), kListTimeout, now);
}

void ConnmanWifi::connect(const WifiConnect& request, Millis now) {
  if (state_ == WifiState::Connecting) return;
  request_ = request;
  started_ = now;
  previous_.clear();
  was_saved_ = false;
  for (const auto& n : networks_) {
    if (n.connected) previous_ = n.path;
    if (n.path == request.path && n.saved) was_saved_ = true;
  }
  config_path_.clear();
  config_before_.reset();
  set_state(WifiState::Connecting);
  log::info("wifi: joining %s%s", request_.name.c_str(), request_.hidden ? " (hidden)" : "");
  // A scan running finishes first; tick() carries on from there.
  if (!runner_->running()) {
    step_ = Step::None;
    tick(now);
  }
}

void ConnmanWifi::forget(const WifiNetwork& network, Millis now) {
  forget_ = network;
  if (!runner_->running() && step_ == Step::None && state_ != WifiState::Connecting) tick(now);
}

bool ConnmanWifi::write_config() {
  config_path_ = config_dir_ + "/" + connman_config_name(request_.name);
  {
    std::ifstream before(config_path_);
    if (before) {
      std::stringstream text;
      text << before.rdbuf();
      config_before_ = text.str();
    }
  }
  // Only root may read it: it holds the password. Written aside and renamed,
  // so connman never reads half a file.
  const std::string temporary = config_dir_ + "/.nesto-wifi.tmp";
  {
    const mode_t old = umask(077);
    std::ofstream file(temporary, std::ios::trunc);
    umask(old);
    file << connman_config(request_.name, request_.passphrase, request_.hidden);
    if (!file.flush()) {
      log::warn("wifi: can't write %s", temporary.c_str());
      std::remove(temporary.c_str());
      config_path_.clear();
      return false;
    }
  }
  chmod(temporary.c_str(), 0600);
  if (std::rename(temporary.c_str(), config_path_.c_str()) != 0) {
    log::warn("wifi: can't write %s", config_path_.c_str());
    std::remove(temporary.c_str());
    config_path_.clear();
    return false;
  }
  return true;
}

void ConnmanWifi::start_connect(Millis now) {
  if (request_.path.empty()) {
    fail(now);
    return;
  }
  run(Step::Connect, dbus(request_.path, "net.connman.Service.Connect", kConnectTimeout - 5000), kConnectTimeout,
      now);
}

void ConnmanWifi::tick(Millis now) {
  if (runner_->running()) {
    if (auto result = runner_->poll(now)) finished(*result, now);
    return;
  }
  if (state_ == WifiState::Connecting && now - started_ > kJoinTime && step_ != Step::Restore) {
    fail(now);
    return;
  }
  switch (step_) {
    case Step::None:
      if (forget_ && state_ != WifiState::Connecting) {
        // Nesto's file first: with it there, connman would set the
        // network up again.
        const WifiNetwork n = *forget_;
        forget_.reset();
        std::remove((config_dir_ + "/" + connman_config_name(n.name)).c_str());
        log::info("wifi: forgetting %s", n.name.c_str());
        run(Step::Forget, dbus(n.path, "net.connman.Service.Remove", kListTimeout - 1000), kListTimeout, now);
        return;
      }
      if (state_ == WifiState::Connecting) {
        // Starting: a provisioning file when there's a password to give or
        // a hidden network to find; a saved or open one is just asked for.
        if (!request_.passphrase.empty() || request_.hidden) {
          if (!write_config()) {
            fail(now);
            return;
          }
          step_ = Step::Settle;
          step_at_ = now + kSettle;
          written_at_ = now;
        } else {
          start_connect(now);
        }
      }
      return;
    case Step::Settle:
      if (now < step_at_) return;
      if (request_.hidden) {
        run(Step::FindScan,
            dbus("/net/connman/technology/wifi", "net.connman.Technology.Scan", kScanTimeout - 1000), kScanTimeout,
            now);
      } else {
        // Whether connman took the file, for the log, then joining.
        run(Step::Check, dbus("/", "net.connman.Manager.GetServices", kListTimeout - 1000), kListTimeout, now);
      }
      return;
    case Step::Watch:
      if (now >= step_at_) {
        run(Step::Watch, dbus("/", "net.connman.Manager.GetServices", kListTimeout - 1000), kListTimeout, now);
      }
      return;
    default:
      return;
  }
}

void ConnmanWifi::finished(const CommandResult& result, Millis now) {
  const Step step = step_;
  step_ = Step::None;
  switch (step) {
    case Step::Scan:
      // Even a refused scan (one already running) leaves a list to read.
      if (state_ == WifiState::Connecting) return;  // joining goes first
      list(now);
      return;
    case Step::List:
      if (result.ok) set_networks(wifi_networks(parse_connman_services(result.output)));
      return;
    case Step::Check: {
      // connman takes the file in its own time: joining before it has,
      // it has no password to give and refuses at once. A network saved
      // before (a new password) gets a while longer instead, as there's no
      // telling.
      if (result.ok) set_networks(wifi_networks(parse_connman_services(result.output)));
      const auto it = std::find_if(networks_.begin(), networks_.end(),
                                   [&](const WifiNetwork& n) { return n.path == request_.path; });
      const bool taken = it != networks_.end() && (it->saved || it->provisioned) &&
                         (!was_saved_ || now - written_at_ >= kResettle);
      if (!taken && now - written_at_ < kTakeTime) {
        step_ = Step::Settle;
        step_at_ = now + kCheckEvery;
        return;
      }
      log::info("wifi: %s %s after %llu ms", request_.name.c_str(),
                taken ? "has its password in connman" : "still isn't saved in connman; joining anyway",
                static_cast<unsigned long long>(now - written_at_));
      start_connect(now);
      return;
    }
    case Step::FindScan:
      run(Step::FindList, dbus("/", "net.connman.Manager.GetServices", kListTimeout - 1000), kListTimeout, now);
      return;
    case Step::FindList: {
      if (result.ok) set_networks(wifi_networks(parse_connman_services(result.output)));
      const auto it = std::find_if(networks_.begin(), networks_.end(),
                                   [&](const WifiNetwork& n) { return n.name == request_.name; });
      if (it == networks_.end()) {
        log::warn("wifi: %s wasn't found", request_.name.c_str());
        fail(now);
        return;
      }
      request_.path = it->path;
      start_connect(now);
      return;
    }
    case Step::Connect:
      if (!result.ok) {
        log::warn("wifi: connman couldn't join %s: %s", request_.name.c_str(), first_line(result.output).c_str());
        fail(now);
        return;
      }
      step_ = Step::Watch;
      step_at_ = now;
      return;
    case Step::Watch: {
      if (result.ok) set_networks(wifi_networks(parse_connman_services(result.output)));
      const auto it = std::find_if(networks_.begin(), networks_.end(),
                                   [&](const WifiNetwork& n) { return n.path == request_.path; });
      if (it != networks_.end() && it->connected) {
        succeed();
        return;
      }
      if (it == networks_.end()) log::info("wifi: %s isn't listed now", request_.name.c_str());
      step_ = Step::Watch;
      step_at_ = now + kWatchEvery;
      return;
    }
    case Step::Restore:
      list(now);
      return;
    case Step::Forget:
      if (!result.ok) log::warn("wifi: connman couldn't forget it: %s", first_line(result.output).c_str());
      list(now);
      return;
    case Step::None:
    case Step::Settle:
      return;
  }
}

void ConnmanWifi::fail(Millis now) {
  log::warn("wifi: %s didn't come up; going back", request_.name.c_str());
  if (!config_path_.empty()) {
    // The file goes, or what was there comes back: connman then forgets
    // the password just tried.
    if (config_before_) {
      std::ofstream(config_path_, std::ios::trunc) << *config_before_;
    } else {
      std::remove(config_path_.c_str());
    }
    config_path_.clear();
  }
  set_state(WifiState::Failed);
  step_ = Step::None;
  if (runner_->running()) return;  // a timed-out command is killed by its own timeout
  if (!previous_.empty() && previous_ != request_.path) {
    run(Step::Restore, dbus(previous_, "net.connman.Service.Connect", kConnectTimeout - 5000), kConnectTimeout,
        now);
  } else {
    list(now);
  }
}

void ConnmanWifi::succeed() {
  log::info("wifi: joined %s", request_.name.c_str());
  step_ = Step::None;
  set_state(WifiState::Connected);
}

// FakeWifi ------------------------------------------------------------------------

FakeWifi::FakeWifi() {
  const auto net = [](const char* name, WifiSecurity s, const char* sn, int strength, bool saved, bool on) {
    return WifiNetwork{std::string("/fake/") + name, name, s, sn, strength, saved, on};
  };
  all_ = {net("Home", WifiSecurity::Wpa, "WPA Personal", 82, true, true),
          net("Neighbour", WifiSecurity::Wpa, "WPA Personal", 48, false, false),
          net("Caf\xC3\xA9", WifiSecurity::Open, "Open", 35, false, false),
          net("Office", WifiSecurity::Unsupported, "Enterprise", 60, false, false)};
}

void FakeWifi::scan(Millis now) {
  if (scan_until_ != 0 || state_ == WifiState::Connecting) return;
  scan_until_ = now + 1500;
}

void FakeWifi::connect(const WifiConnect& request, Millis now) {
  if (state_ == WifiState::Connecting) return;
  request_ = request;
  join_at_ = now + 2000;
  set_state(WifiState::Connecting);
}

void FakeWifi::forget(const WifiNetwork& network, Millis now) {
  for (auto& n : all_) {
    if (n.name != network.name) continue;
    const bool was_connected = n.connected;
    n.saved = false;
    n.connected = false;
    // As connman would: another saved network, if there is one.
    if (was_connected) {
      for (auto& other : all_) {
        if (other.saved) {
          other.connected = true;
          break;
        }
      }
    }
  }
  scan_until_ = now;
}

void FakeWifi::tick(Millis now) {
  if (scan_until_ != 0 && now >= scan_until_) {
    scan_until_ = 0;
    std::vector<WifiNetwork> sorted = all_;
    std::stable_sort(sorted.begin(), sorted.end(), [](const WifiNetwork& a, const WifiNetwork& b) {
      if (a.connected != b.connected) return a.connected;
      return a.strength > b.strength;
    });
    set_networks(sorted);
  }
  if (state_ != WifiState::Connecting || now < join_at_) return;
  if (request_.passphrase.rfind("wrong", 0) == 0) {
    set_state(WifiState::Failed);
    return;
  }
  auto it = std::find_if(all_.begin(), all_.end(), [&](const WifiNetwork& n) { return n.name == request_.name; });
  if (it == all_.end()) {
    all_.push_back({"/fake/hidden", request_.name, request_.security, request_.passphrase.empty() ? "Open" : "WPA Personal",
                    70, false, false});
    it = all_.end() - 1;
  }
  for (auto& n : all_) n.connected = false;
  it->connected = true;
  it->saved = true;
  set_state(WifiState::Connected);
  scan_until_ = now;  // the list again, with the new network first
}

}  // namespace nesto::net
