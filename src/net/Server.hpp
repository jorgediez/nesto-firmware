#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "protocol/PanelSession.hpp"

namespace nesto::net {

/// The panel's TCP server (PROTOCOL.md section 3): accepts Home Assistant's
/// connections and runs a PanelSession for each. Non-blocking, driven from the
/// main loop through poll(). Only one session stays authenticated: when a new
/// one authenticates, the application calls close_others().
class Server {
 public:
  static constexpr std::size_t kMaxConnections = 4;
  /// A client that doesn't read what the panel sends gets disconnected.
  static constexpr std::size_t kMaxPendingOutput = 2 * 1024 * 1024;

  Server(std::uint16_t port, const protocol::PanelInfo& info, protocol::PairingState& pairing,
         protocol::SessionEvents& events, std::function<Millis()> clock);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  /// Starts listening. Logs the reason and returns false if it can't.
  bool start();

  /// Waits up to `timeout` ms for network activity, then handles it: accepts,
  /// reads, runs session timers, writes and closes.
  void poll(Millis timeout);

  /// Closes every session except `keep`.
  void close_others(const protocol::PanelSession& keep);

  /// Closes every session.
  void close_all(const std::string& reason);

  /// The authenticated session, if there is one.
  protocol::PanelSession* active() const;

  std::uint16_t port() const { return port_; }

 private:
  struct Connection {
    int fd = -1;
    std::string peer;
    std::unique_ptr<protocol::PanelSession> session;
    protocol::Bytes pending;  // queued output not yet written
  };

  void accept_new();
  void read_from(Connection& c);
  void write_to(Connection& c);
  void drop(Connection& c, const std::string& reason);

  std::uint16_t port_;
  const protocol::PanelInfo& info_;
  protocol::PairingState& pairing_;
  protocol::SessionEvents& events_;
  std::function<Millis()> clock_;
  int listen_fd_ = -1;
  std::vector<std::unique_ptr<Connection>> connections_;
};

}  // namespace nesto::net
