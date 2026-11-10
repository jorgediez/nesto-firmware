#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/Input.hpp"

namespace nesto::net {

/// The DNS-SD service the panel announces (PROTOCOL.md section 2).
struct MdnsService {
  std::string instance;  // "Nesto Panel 3d4e5f", the name Home Assistant shows
  std::string host;      // "nesto-3d4e5f", announced as nesto-3d4e5f.local.
  std::uint16_t port = 0;
  std::vector<std::pair<std::string, std::string>> txt;

  bool operator==(const MdnsService& other) const {
    return instance == other.instance && host == other.host && port == other.port &&
           txt == other.txt;
  }
  bool operator!=(const MdnsService& other) const { return !(*this == other); }
};

constexpr const char* kServiceType = "_nesto._tcp.local.";

/// The service for a panel: names from the last six hex digits of its ID, and
/// the TXT keys of section 2.
MdnsService make_service(const std::string& panel_id, std::uint16_t port,
                         const std::string& firmware, const std::string& model, bool paired);

/// Answers mDNS queries for the panel's service over IPv4, and announces it
/// when it starts or changes, so Home Assistant's zeroconf discovery finds it
/// and notices when the address or pairing state changes. Non-blocking: call
/// poll() from the main loop.
class MdnsResponder {
 public:
  MdnsResponder();
  ~MdnsResponder();

  MdnsResponder(const MdnsResponder&) = delete;
  MdnsResponder& operator=(const MdnsResponder&) = delete;

  /// Opens the multicast socket. Logs the reason and returns false if it can't,
  /// for instance when another responder holds port 5353 exclusively.
  bool start();

  /// Sets what to announce and where. Announces again whenever either changes.
  /// Without an address nothing is announced or answered.
  void update(const MdnsService& service, const std::string& ipv4, Millis now);

  /// Answers pending queries and sends due announcements.
  void poll(Millis now);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace nesto::net
