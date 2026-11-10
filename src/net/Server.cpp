#include "net/Server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "core/Log.hpp"

namespace nesto::net {
namespace {

bool set_non_blocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

Server::Server(std::uint16_t port, const protocol::PanelInfo& info,
               protocol::PairingState& pairing, protocol::SessionEvents& events,
               std::function<Millis()> clock)
    : port_(port), info_(info), pairing_(pairing), events_(events), clock_(std::move(clock)) {}

Server::~Server() {
  for (auto& c : connections_) {
    if (c->fd >= 0) close(c->fd);
  }
  if (listen_fd_ >= 0) close(listen_fd_);
}

bool Server::start() {
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    log::error("socket: %s", std::strerror(errno));
    return false;
  }
  const int yes = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port_);
  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
      listen(listen_fd_, 4) != 0 || !set_non_blocking(listen_fd_)) {
    log::error("can't listen on port %u: %s", port_, std::strerror(errno));
    return false;
  }
  log::info("listening on port %u", port_);
  return true;
}

void Server::poll(Millis timeout) {
  fd_set readable;
  fd_set writable;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  int max_fd = listen_fd_;
  FD_SET(listen_fd_, &readable);
  for (const auto& c : connections_) {
    FD_SET(c->fd, &readable);
    if (!c->pending.empty()) FD_SET(c->fd, &writable);
    max_fd = std::max(max_fd, c->fd);
  }

  timeval tv{};
  tv.tv_sec = static_cast<time_t>(timeout / 1000);
  tv.tv_usec = static_cast<suseconds_t>((timeout % 1000) * 1000);
  const int ready = select(max_fd + 1, &readable, &writable, nullptr, &tv);
  if (ready < 0 && errno != EINTR) log::warn("select: %s", std::strerror(errno));

  if (ready > 0 && FD_ISSET(listen_fd_, &readable)) accept_new();

  const Millis now = clock_();
  for (auto& c : connections_) {
    if (c->fd < 0) continue;
    if (ready > 0 && FD_ISSET(c->fd, &readable)) read_from(*c);
    if (c->fd < 0) continue;
    c->session->tick(now);
  }
  // Sessions may have queued output while others were handled (close_others,
  // for instance), so collect and write in a second pass.
  for (auto& c : connections_) {
    if (c->fd < 0) continue;
    for (const auto& frame : c->session->take_outgoing()) {
      c->pending.insert(c->pending.end(), frame.begin(), frame.end());
    }
    if (c->pending.size() > kMaxPendingOutput) {
      drop(*c, "it isn't reading what the panel sends");
      continue;
    }
    write_to(*c);
    if (c->fd >= 0 && c->session->closing() && c->pending.empty()) {
      drop(*c, c->session->close_reason());
    }
  }

  connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                    [](const auto& c) { return c->fd < 0; }),
                     connections_.end());
}

void Server::accept_new() {
  for (;;) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    const int fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    if (fd < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        log::warn("accept: %s", std::strerror(errno));
      }
      return;
    }
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
    const std::string peer = std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));

    if (connections_.size() >= kMaxConnections || !set_non_blocking(fd)) {
      log::warn("refusing a connection from %s: too many connections", peer.c_str());
      close(fd);
      continue;
    }
    auto c = std::make_unique<Connection>();
    c->fd = fd;
    c->peer = peer;
    c->session = std::make_unique<protocol::PanelSession>(info_, pairing_, events_, clock_());
    log::info("connection from %s", peer.c_str());
    connections_.push_back(std::move(c));
  }
}

void Server::read_from(Connection& c) {
  std::uint8_t buffer[4096];
  for (;;) {
    const ssize_t n = recv(c.fd, buffer, sizeof buffer, 0);
    if (n > 0) {
      c.session->receive(buffer, static_cast<std::size_t>(n), clock_());
      if (c.session->closing()) return;
      continue;
    }
    if (n == 0) {
      drop(c, "the other side closed the connection");
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      drop(c, std::strerror(errno));
    }
    return;
  }
}

void Server::write_to(Connection& c) {
  std::size_t written = 0;
  while (written < c.pending.size()) {
    const ssize_t n =
        send(c.fd, c.pending.data() + written, c.pending.size() - written, MSG_NOSIGNAL);
    if (n > 0) {
      written += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
    drop(c, std::strerror(errno));
    return;
  }
  c.pending.erase(c.pending.begin(), c.pending.begin() + static_cast<std::ptrdiff_t>(written));
}

void Server::drop(Connection& c, const std::string& reason) {
  if (c.fd < 0) return;
  log::info("closing the connection from %s: %s", c.peer.c_str(), reason.c_str());
  close(c.fd);
  c.fd = -1;
  c.session->close(reason);
}

void Server::close_others(const protocol::PanelSession& keep) {
  for (auto& c : connections_) {
    if (c->fd >= 0 && c->session.get() != &keep) {
      c->session->close("replaced by a newer connection");
    }
  }
}

void Server::close_all(const std::string& reason) {
  for (auto& c : connections_) {
    if (c->fd >= 0) c->session->close(reason);
  }
}

protocol::PanelSession* Server::active() const {
  for (const auto& c : connections_) {
    if (c->fd >= 0 && c->session->authenticated()) return c->session.get();
  }
  return nullptr;
}

}  // namespace nesto::net
