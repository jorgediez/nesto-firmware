#include "net/Mdns.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

#include "core/Log.hpp"
#include "mdns.h"

namespace nesto::net {
namespace {

constexpr const char* kDnsSdServices = "_services._dns-sd._udp.local.";
constexpr std::uint32_t kSharedTtl = 4500;  // PTR records, RFC 6762 section 10
constexpr std::uint32_t kUniqueTtl = 120;   // SRV, TXT and A records
constexpr int kAnnouncements = 2;           // RFC 6762 section 8.3: at least two
constexpr Millis kAnnouncementGap = 1000;
constexpr int kMaxPacketsPerPoll = 16;

mdns_string_t view(const std::string& s) {
  return {s.c_str(), s.size()};
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

}  // namespace

MdnsService make_service(const std::string& panel_id, std::uint16_t port,
                         const std::string& firmware, const std::string& model, bool paired) {
  const std::string tail = panel_id.size() > 6 ? panel_id.substr(panel_id.size() - 6) : panel_id;
  MdnsService s;
  s.instance = "Nesto Panel " + tail;
  s.host = "nesto-" + tail;
  s.port = port;
  s.txt = {{"id", panel_id},
           {"proto", "1"},
           {"fw", firmware},
           {"model", model},
           {"paired", paired ? "1" : "0"}};
  return s;
}

struct MdnsResponder::Impl {
  int sock = -1;
  MdnsService service;
  std::string ipv4;

  // Fully qualified names; the records point into these strings.
  std::string type = kServiceType;
  std::string dns_sd = kDnsSdServices;
  std::string instance_fqdn;
  std::string host_fqdn;

  mdns_record_t ptr{};
  mdns_record_t srv{};
  mdns_record_t a{};
  std::vector<mdns_record_t> txt;

  int announcements_left = 0;
  Millis next_announcement = 0;
  bool announced = false;

  alignas(4) std::uint8_t receive_buffer[2048];
  alignas(4) std::uint8_t send_buffer[2048];

  bool active() const { return sock >= 0 && !ipv4.empty(); }

  void rebuild() {
    instance_fqdn = service.instance + "." + type;
    host_fqdn = service.host + ".local.";

    ptr = mdns_record_t{};
    ptr.name = view(type);
    ptr.type = MDNS_RECORDTYPE_PTR;
    ptr.data.ptr.name = view(instance_fqdn);
    ptr.ttl = kSharedTtl;

    srv = mdns_record_t{};
    srv.name = view(instance_fqdn);
    srv.type = MDNS_RECORDTYPE_SRV;
    srv.data.srv.port = service.port;
    srv.data.srv.name = view(host_fqdn);
    srv.ttl = kUniqueTtl;

    a = mdns_record_t{};
    a.name = view(host_fqdn);
    a.type = MDNS_RECORDTYPE_A;
    a.data.a.addr.sin_family = AF_INET;
    inet_pton(AF_INET, ipv4.c_str(), &a.data.a.addr.sin_addr);
    a.ttl = kUniqueTtl;

    txt.clear();
    for (const auto& [key, value] : service.txt) {
      mdns_record_t r{};
      r.name = view(instance_fqdn);
      r.type = MDNS_RECORDTYPE_TXT;
      r.data.txt.key = view(key);
      r.data.txt.value = view(value);
      r.ttl = kUniqueTtl;
      txt.push_back(r);
    }
  }

  /// The records that go with an answer: the SRV record (if wanted), the
  /// address, and the TXT entries, which the library merges into one record.
  std::vector<mdns_record_t> extras(bool with_srv) const {
    std::vector<mdns_record_t> out;
    if (with_srv) out.push_back(srv);
    out.push_back(a);
    out.insert(out.end(), txt.begin(), txt.end());
    return out;
  }

  void announce() {
    const auto more = extras(true);
    if (mdns_announce_multicast(sock, send_buffer, sizeof send_buffer, ptr, nullptr, 0,
                                more.data(), more.size()) == 0) {
      announced = true;
    } else {
      log::warn("mDNS: can't announce: %s", std::strerror(errno));
    }
  }

  void goodbye() {
    if (!announced || sock < 0) return;
    const auto more = extras(true);
    mdns_goodbye_multicast(sock, send_buffer, sizeof send_buffer, ptr, nullptr, 0, more.data(),
                           more.size());
    announced = false;
  }

  void handle_question(const sockaddr* from, std::size_t addrlen, std::uint16_t query_id,
                       std::uint16_t rtype, std::uint16_t rclass, const mdns_string_t& raw_name) {
    if (!active()) return;
    const std::string name = lower(std::string(raw_name.str, raw_name.length));
    const bool any = rtype == MDNS_RECORDTYPE_ANY;

    mdns_record_t answer{};
    std::vector<mdns_record_t> more;
    if (name == dns_sd && (rtype == MDNS_RECORDTYPE_PTR || any)) {
      // Service enumeration: which service types are here.
      answer.name = view(dns_sd);
      answer.type = MDNS_RECORDTYPE_PTR;
      answer.data.ptr.name = view(type);
      answer.ttl = kSharedTtl;
    } else if (name == type && (rtype == MDNS_RECORDTYPE_PTR || any)) {
      answer = ptr;
      more = extras(true);
    } else if (name == lower(instance_fqdn) &&
               (rtype == MDNS_RECORDTYPE_SRV || rtype == MDNS_RECORDTYPE_TXT || any)) {
      // The library can't send TXT as the answer itself, so a TXT question gets
      // the SRV record with the TXT record alongside; resolvers keep both.
      answer = srv;
      more = extras(false);
    } else if (name == lower(host_fqdn) && (rtype == MDNS_RECORDTYPE_A || any)) {
      answer = a;
    } else {
      return;
    }

    // A unicast reply when asked for one, or when the question didn't come
    // from the mDNS port (a one-shot resolver, RFC 6762 section 6.7).
    const auto* in = reinterpret_cast<const sockaddr_in*>(from);
    const bool legacy = from->sa_family == AF_INET && ntohs(in->sin_port) != MDNS_PORT;
    if ((rclass & MDNS_UNICAST_RESPONSE) || legacy) {
      mdns_query_answer_unicast(sock, from, addrlen, send_buffer, sizeof send_buffer, query_id,
                                static_cast<mdns_record_type_t>(rtype), raw_name.str,
                                raw_name.length, answer, nullptr, 0, more.data(), more.size());
    } else {
      mdns_query_answer_multicast(sock, send_buffer, sizeof send_buffer, answer, nullptr, 0,
                                  more.data(), more.size());
    }
  }

  static int on_record(int /*sock*/, const sockaddr* from, std::size_t addrlen,
                       mdns_entry_type_t entry, std::uint16_t query_id, std::uint16_t rtype,
                       std::uint16_t rclass, std::uint32_t /*ttl*/, const void* data,
                       std::size_t size, std::size_t name_offset, std::size_t /*name_length*/,
                       std::size_t /*record_offset*/, std::size_t /*record_length*/,
                       void* user_data) {
    if (entry != MDNS_ENTRYTYPE_QUESTION) return 0;
    auto* self = static_cast<Impl*>(user_data);
    char buffer[256];
    std::size_t offset = name_offset;
    const mdns_string_t name = mdns_string_extract(data, size, &offset, buffer, sizeof buffer);
    self->handle_question(from, addrlen, query_id, rtype, rclass, name);
    return 0;
  }
};

MdnsResponder::MdnsResponder() : impl_(std::make_unique<Impl>()) {}

MdnsResponder::~MdnsResponder() {
  impl_->goodbye();
  if (impl_->sock >= 0) mdns_socket_close(impl_->sock);
}

bool MdnsResponder::start() {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(MDNS_PORT);
  impl_->sock = mdns_socket_open_ipv4(&addr);
  if (impl_->sock < 0) {
    log::warn("mDNS: can't use port %d: %s; Home Assistant must be given the address by hand",
              MDNS_PORT, std::strerror(errno));
    return false;
  }
  const int flags = fcntl(impl_->sock, F_GETFL, 0);
  fcntl(impl_->sock, F_SETFL, flags | O_NONBLOCK);
  log::info("mDNS: answering for %s", kServiceType);
  return true;
}

void MdnsResponder::update(const MdnsService& service, const std::string& ipv4, Millis now) {
  if (service == impl_->service && ipv4 == impl_->ipv4) return;
  if (service.instance != impl_->service.instance) impl_->goodbye();
  impl_->service = service;
  impl_->ipv4 = ipv4;
  impl_->rebuild();
  if (impl_->active()) {
    impl_->announcements_left = kAnnouncements;
    impl_->next_announcement = now;
  }
}

void MdnsResponder::poll(Millis now) {
  if (impl_->sock < 0) return;
  for (int i = 0; i < kMaxPacketsPerPoll; ++i) {
    if (mdns_socket_listen(impl_->sock, impl_->receive_buffer, sizeof impl_->receive_buffer,
                           &Impl::on_record, impl_.get()) == 0) {
      break;
    }
  }
  if (impl_->active() && impl_->announcements_left > 0 && now >= impl_->next_announcement) {
    impl_->announce();
    --impl_->announcements_left;
    impl_->next_announcement = now + kAnnouncementGap;
  }
}

}  // namespace nesto::net
