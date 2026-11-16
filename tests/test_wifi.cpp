// Joining a Wi-Fi network through connman, with dbus-send played by the test,
// and the provisioning file connman reads.
#include <doctest/doctest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "net/Wifi.hpp"

using namespace nesto;
using namespace nesto::net;

namespace {

/// Plays dbus-send: each command waits until the test answers it.
struct FakeRunner : CommandRunner {
  std::vector<std::vector<std::string>> started;
  bool busy = false;
  std::optional<CommandResult> answer;

  bool start(const std::vector<std::string>& argv, Millis, Millis) override {
    if (busy) return false;
    started.push_back(argv);
    busy = true;
    return true;
  }
  bool running() const override { return busy; }
  std::optional<CommandResult> poll(Millis) override {
    if (!busy || !answer) return std::nullopt;
    busy = false;
    auto r = std::move(*answer);
    answer.reset();
    return r;
  }
};

struct Service {
  std::string path, name, state, security;
  int strength = 50;
  bool favorite = false;
};

std::string reply(const std::vector<Service>& services) {
  std::string out = "method return time=1 sender=:1.4 -> destination=:1.9 serial=2 reply_serial=2\n   array [\n";
  for (const auto& s : services) {
    out += "      struct {\n         object path \"" + s.path + "\"\n         array [\n";
    const auto entry = [&](const std::string& key, const std::string& value) {
      out += "            dict entry(\n               string \"" + key + "\"\n               variant " + value +
             "\n            )\n";
    };
    entry("Type", "string \"wifi\"");
    entry("Security", "array [\n string \"" + s.security + "\"\n ]");
    entry("State", "string \"" + s.state + "\"");
    entry("Strength", "byte " + std::to_string(s.strength));
    entry("Favorite", s.favorite ? "boolean true" : "boolean false");
    entry("Name", "string \"" + s.name + "\"");
    out += "         ]\n      }\n";
  }
  return out + "   ]\n";
}

const Service kHome{"/net/connman/service/wifi_1_486f6d65_managed_psk", "Home", "online", "psk", 70, true};
const Service kCafe{"/net/connman/service/wifi_1_436166_managed_none", "Cafe", "idle", "none", 40, false};
const Service kNext{"/net/connman/service/wifi_1_4e657874_managed_psk", "Next door", "idle", "psk", 55, false};

struct Fixture {
  std::string dir;
  FakeRunner* runner = nullptr;
  std::unique_ptr<ConnmanWifi> wifi;
  Millis now = 1000;

  Fixture() {
    char pattern[] = "/tmp/nesto-wifi-XXXXXX";
    dir = mkdtemp(pattern);
    auto r = std::make_unique<FakeRunner>();
    runner = r.get();
    wifi = std::make_unique<ConnmanWifi>(std::move(r), dir);
  }
  ~Fixture() {
    if (std::system(("rm -rf " + dir).c_str()) != 0) MESSAGE("couldn't remove " << dir);
  }

  const std::vector<std::string>& last() const { return runner->started.back(); }
  std::string method() const { return last().back(); }
  std::string target() const { return last()[last().size() - 2]; }
  void answer(bool ok, const std::string& output = "") {
    runner->answer = CommandResult{ok, output};
    wifi->tick(now);
  }
  void after(Millis ms) {
    now += ms;
    wifi->tick(now);
  }
  void listed(const std::vector<Service>& services) {
    wifi->scan(now);
    REQUIRE(method() == "net.connman.Technology.Scan");
    answer(true);
    REQUIRE(method() == "net.connman.Manager.GetServices");
    answer(true, reply(services));
  }
  /// After the file: connman's list, checked before joining.
  void check() {
    REQUIRE(method() == "net.connman.Manager.GetServices");
    Service next = kNext;
    next.favorite = true;
    answer(true, reply({kHome, next}));
  }
  std::string file(const std::string& name) const {
    std::ifstream in(dir + "/" + connman_config_name(name));
    if (!in) return "";
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }
};

}  // namespace

TEST_CASE("scanning lists the networks, the connected one first") {
  Fixture f;
  CHECK_FALSE(f.wifi->listed());
  f.listed({kCafe, kNext, kHome});
  CHECK_FALSE(f.wifi->scanning());
  const auto& n = f.wifi->networks();
  REQUIRE(n.size() == 3);
  CHECK(n[0].name == "Home");
  CHECK(n[0].connected);
  CHECK(n[0].saved);
  CHECK(n[0].security == WifiSecurity::Wpa);
  CHECK(n[1].name == "Next door");
  CHECK(n[2].security == WifiSecurity::Open);
}

TEST_CASE("joining with a password: a provisioning file, Connect, then watching") {
  Fixture f;
  f.listed({kHome, kNext});
  f.wifi->connect({kNext.path, "Next door", WifiSecurity::Wpa, "secret password", false}, f.now);
  CHECK(f.wifi->state() == WifiState::Connecting);
  // The file first, only root may read it, and connman is given a moment.
  const std::string config = f.file("Next door");
  CHECK(config.find("SSID = 4e65787420646f6f72") != std::string::npos);
  CHECK(config.find("Passphrase = secret password") != std::string::npos);
  struct stat st {};
  REQUIRE(stat((f.dir + "/" + connman_config_name("Next door")).c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  const auto commands = f.runner->started.size();
  f.after(500);
  CHECK(f.runner->started.size() == commands);
  f.after(600);
  f.check();
  CHECK(f.method() == "net.connman.Service.Connect");
  CHECK(f.target() == kNext.path);
  f.answer(true);
  f.after(0);
  CHECK(f.method() == "net.connman.Manager.GetServices");
  Service joined = kNext;
  joined.state = "online";
  Service left = kHome;
  left.state = "idle";
  f.answer(true, reply({left, joined}));
  CHECK(f.wifi->state() == WifiState::Connected);
  CHECK(f.wifi->networks()[0].name == "Next door");
  CHECK_FALSE(f.file("Next door").empty());  // kept: connman forgets the network without it
}

TEST_CASE("a wrong password: the file goes and the network before comes back") {
  Fixture f;
  f.listed({kHome, kNext});
  f.wifi->connect({kNext.path, "Next door", WifiSecurity::Wpa, "wrong password", false}, f.now);
  f.after(1100);
  f.check();
  REQUIRE(f.method() == "net.connman.Service.Connect");
  f.answer(false);  // connman's error
  CHECK(f.wifi->state() == WifiState::Failed);
  CHECK(f.file("Next door").empty());
  CHECK(f.method() == "net.connman.Service.Connect");
  CHECK(f.target() == kHome.path);
}

TEST_CASE("a password changed for a network with a file: the old file comes back") {
  Fixture f;
  f.listed({kHome, kNext});
  const std::string before = connman_config("Next door", "old password", false);
  std::ofstream(f.dir + "/" + connman_config_name("Next door")) << before;
  f.wifi->connect({kNext.path, "Next door", WifiSecurity::Wpa, "new password", false}, f.now);
  f.after(1100);
  f.check();
  f.answer(false);
  CHECK(f.file("Next door") == before);
}

TEST_CASE("no connection within a minute: going back") {
  Fixture f;
  f.listed({kHome, kNext});
  f.wifi->connect({kNext.path, "Next door", WifiSecurity::Wpa, "secret password", false}, f.now);
  f.after(1100);
  f.check();
  f.answer(true);
  for (int i = 0; i < 40 && f.wifi->state() == WifiState::Connecting; ++i) {
    f.after(2000);
    if (f.runner->running() && f.method() == "net.connman.Manager.GetServices") {
      f.answer(true, reply({kHome, kNext}));  // still idle
    }
  }
  CHECK(f.wifi->state() == WifiState::Failed);
  CHECK(f.file("Next door").empty());
  CHECK(f.target() == kHome.path);
}

TEST_CASE("joining waits until connman has the password") {
  Fixture f;
  f.listed({kHome, kNext});
  f.wifi->connect({kNext.path, "Next door", WifiSecurity::Wpa, "secret password", false}, f.now);
  f.after(1100);
  REQUIRE(f.method() == "net.connman.Manager.GetServices");
  f.answer(true, reply({kHome, kNext}));  // not saved yet
  const auto commands = f.runner->started.size();
  f.after(400);
  CHECK(f.runner->started.size() == commands);
  f.after(100);
  f.check();  // saved now
  CHECK(f.method() == "net.connman.Service.Connect");
}

TEST_CASE("a new password for a saved network gets a few seconds") {
  Fixture f;
  Service next = kNext;
  next.favorite = true;
  f.listed({kHome, next});
  f.wifi->connect({kNext.path, "Next door", WifiSecurity::Wpa, "new password", false}, f.now);
  f.after(1100);
  f.check();  // saved, but from before
  CHECK(f.method() == "net.connman.Manager.GetServices");
  for (int i = 0; i < 6 && f.method() == "net.connman.Manager.GetServices"; ++i) {
    f.after(500);
    if (f.runner->running() && f.method() == "net.connman.Manager.GetServices") f.check();
  }
  CHECK(f.method() == "net.connman.Service.Connect");
  CHECK(f.now - 1000 >= 3000);
}

TEST_CASE("forgetting: Nesto's file goes, then connman's Remove") {
  Fixture f;
  f.listed({kHome, kNext});
  std::ofstream(f.dir + "/" + connman_config_name("Home")) << connman_config("Home", "a password", false);
  f.wifi->forget(f.wifi->networks()[0], f.now);
  CHECK(f.file("Home").empty());
  CHECK(f.method() == "net.connman.Service.Remove");
  CHECK(f.target() == kHome.path);
  f.answer(true);
  CHECK(f.method() == "net.connman.Manager.GetServices");
}

TEST_CASE("a saved or open network is just asked for, without a file") {
  Fixture f;
  f.listed({kHome, kCafe});
  f.wifi->connect({kCafe.path, "Cafe", WifiSecurity::Open, "", false}, f.now);
  CHECK(f.method() == "net.connman.Service.Connect");
  CHECK(f.target() == kCafe.path);
  CHECK(f.file("Cafe").empty());
}

TEST_CASE("a hidden network: a file that says so, a scan, then found by name") {
  Fixture f;
  f.listed({kHome});
  f.wifi->connect({"", "Attic", WifiSecurity::Wpa, "attic password", true}, f.now);
  CHECK(f.file("Attic").find("Hidden = true") != std::string::npos);
  f.after(1100);
  CHECK(f.method() == "net.connman.Technology.Scan");
  f.answer(true);
  CHECK(f.method() == "net.connman.Manager.GetServices");
  const Service attic{"/net/connman/service/wifi_1_4174746963_managed_psk", "Attic", "idle", "psk", 30, false};
  f.answer(true, reply({kHome, attic}));
  CHECK(f.method() == "net.connman.Service.Connect");
  CHECK(f.target() == attic.path);
}

TEST_CASE("a hidden network that isn't there fails") {
  Fixture f;
  f.listed({kHome});
  f.wifi->connect({"", "Nowhere", WifiSecurity::Wpa, "a password", true}, f.now);
  f.after(1100);
  f.answer(true);
  f.answer(true, reply({kHome}));
  CHECK(f.wifi->state() == WifiState::Failed);
  CHECK(f.file("Nowhere").empty());
}

TEST_CASE("joining while a scan runs waits for it") {
  Fixture f;
  f.listed({kHome, kCafe});
  f.wifi->scan(f.now);
  f.wifi->connect({kCafe.path, "Cafe", WifiSecurity::Open, "", false}, f.now);
  CHECK(f.method() == "net.connman.Technology.Scan");
  f.answer(true);  // the scan ends; no listing, joining goes first
  f.after(0);
  CHECK(f.method() == "net.connman.Service.Connect");
}

TEST_CASE("the provisioning file escapes what GLib would lose") {
  const std::string c = connman_config("Caf\xC3\xA9", " pass\\word ", false);
  CHECK(c.find("[service_nesto_436166c3a9]") != std::string::npos);
  CHECK(c.find("SSID = 436166c3a9\n") != std::string::npos);
  CHECK(c.find("Security = psk\n") != std::string::npos);
  CHECK(c.find("Passphrase = \\spass\\\\word\\s\n") != std::string::npos);
  CHECK(c.find("Hidden") == std::string::npos);
  const std::string open = connman_config("Attic", "", true);
  CHECK(open.find("Security = none\n") != std::string::npos);
  CHECK(open.find("Passphrase") == std::string::npos);
  CHECK(open.find("Hidden = true\n") != std::string::npos);
  CHECK(connman_config_name("Home") == "nesto-486f6d65.config");
}

TEST_CASE("the pretend Wi-Fi joins, unless the password starts with 'wrong'") {
  FakeWifi w;
  w.scan(0);
  w.tick(2000);
  REQUIRE(w.listed());
  CHECK(w.networks()[0].connected);
  w.connect({"/fake/Neighbour", "Neighbour", WifiSecurity::Wpa, "wrong one", false}, 3000);
  w.tick(6000);
  CHECK(w.state() == WifiState::Failed);
  w.connect({"/fake/Neighbour", "Neighbour", WifiSecurity::Wpa, "right one", false}, 7000);
  w.tick(10000);
  CHECK(w.state() == WifiState::Connected);
  w.tick(10001);
  CHECK(w.networks()[0].name == "Neighbour");
}
