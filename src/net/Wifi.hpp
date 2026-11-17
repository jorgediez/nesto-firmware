#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Input.hpp"
#include "core/WifiTypes.hpp"
#include "net/Command.hpp"
#include "net/Wireless.hpp"

namespace nesto::net {

/// The Wi-Fi as Settings sees it: scan, list, join. Each call returns at
/// once; tick() moves things on.
class WifiBackend {
 public:
  virtual ~WifiBackend() = default;
  /// Looks for networks, unless already looking or joining one.
  virtual void scan(Millis now) = 0;
  virtual bool scanning() const = 0;
  /// Joins a network, going back to the one before if it doesn't work
  /// within kJoinTime.
  virtual void connect(const WifiConnect& request, Millis now) = 0;
  /// Forgets a saved network: its password, and Nesto's file for it. The
  /// one in use disconnects, and connman joins another saved one if it can.
  virtual void forget(const WifiNetwork& network, Millis now) = 0;
  virtual void tick(Millis now) = 0;

  /// The networks found, the connected one first, then the strongest.
  const std::vector<WifiNetwork>& networks() const { return networks_; }
  /// Whether a scan has finished yet.
  bool listed() const { return listed_; }
  WifiState state() const { return state_; }
  /// Increases with every change to the above.
  unsigned version() const { return version_; }

  static constexpr Millis kJoinTime = 60000;

 protected:
  void set_networks(std::vector<WifiNetwork> networks);
  void set_state(WifiState state);

  std::vector<WifiNetwork> networks_;
  bool listed_ = false;
  WifiState state_ = WifiState::Idle;
  unsigned version_ = 1;
};

/// connman's services as Wi-Fi networks: named ones only, sorted.
std::vector<WifiNetwork> wifi_networks(const std::vector<NetService>& services);

/// connman's provisioning file for a network (its config files' format):
/// the SSID in hex so any name works, the passphrase escaped for GLib's key
/// files.
std::string connman_config(const std::string& name, const std::string& passphrase, bool hidden);

/// The file's name in connman's directory: one per network.
std::string connman_config_name(const std::string& name);

/// The thermostat's Wi-Fi, through connman: scans and lists with dbus-send,
/// joins by writing a provisioning file with the passphrase (connman reads
/// those at once) and asking connman to connect. If the new network doesn't
/// come up, the file goes (or its earlier version comes back) and the
/// network before is asked for again.
class ConnmanWifi : public WifiBackend {
 public:
  ConnmanWifi(std::unique_ptr<CommandRunner> runner, std::string config_dir);

  void scan(Millis now) override;
  bool scanning() const override { return step_ == Step::Scan || step_ == Step::List; }
  void connect(const WifiConnect& request, Millis now) override;
  void forget(const WifiNetwork& network, Millis now) override;
  void tick(Millis now) override;

 private:
  enum class Step {
    Forget,       // Service.Remove
    None,
    Scan,         // Technology.Scan
    List,         // Manager.GetServices
    Settle,       // a moment for connman to read a new file
    Check,        // whether it has: the network saved
    FindScan,     // a hidden network: scanning for it
    FindList,
    Connect,      // Service.Connect
    Watch,        // listing until it's connected
    Restore,      // back to the network before
  };

  void run(Step step, const std::vector<std::string>& argv, Millis timeout, Millis now);
  void finished(const CommandResult& result, Millis now);
  void list(Millis now);
  void start_connect(Millis now);
  bool write_config();
  void fail(Millis now);
  void succeed();

  std::unique_ptr<CommandRunner> runner_;
  std::string config_dir_;
  Step step_ = Step::None;
  Millis step_at_ = 0;

  WifiConnect request_;
  Millis started_ = 0;
  std::string previous_;          // the service connected before
  bool was_saved_ = false;        // the network had a password in connman already
  Millis written_at_ = 0;         // when the provisioning file was written
  std::string config_path_;       // the file written, if one was
  std::optional<std::string> config_before_;  // its contents before, if any
  std::optional<WifiNetwork> forget_;  // waiting for the command before it
};

/// A pretend Wi-Fi for the simulator: a few networks, and joining works
/// after two seconds unless the password starts with "wrong".
class FakeWifi : public WifiBackend {
 public:
  FakeWifi();
  void scan(Millis now) override;
  bool scanning() const override { return scan_until_ != 0; }
  void connect(const WifiConnect& request, Millis now) override;
  void forget(const WifiNetwork& network, Millis now) override;
  void tick(Millis now) override;

 private:
  std::vector<WifiNetwork> all_;
  Millis scan_until_ = 0;
  Millis join_at_ = 0;
  WifiConnect request_;
};

}  // namespace nesto::net
