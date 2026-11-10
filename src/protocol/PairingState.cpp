#include "protocol/PairingState.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include "core/Log.hpp"
#include "protocol/Crypto.hpp"

namespace nesto::protocol {

FileKeyStore::FileKeyStore(std::string dir) : dir_(std::move(dir)), path_(dir_ + "/pairing.key") {}

std::optional<Key> FileKeyStore::load() {
  const int fd = open(path_.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno != ENOENT) log::warn("%s: %s", path_.c_str(), std::strerror(errno));
    return std::nullopt;
  }
  Key key{};
  const ssize_t n = read(fd, key.data(), key.size());
  std::uint8_t extra = 0;
  const ssize_t more = read(fd, &extra, 1);
  close(fd);
  if (n != static_cast<ssize_t>(key.size()) || more != 0) {
    log::warn("%s: not a pairing key, ignoring it", path_.c_str());
    return std::nullopt;
  }
  return key;
}

bool FileKeyStore::save(const Key& key) {
  if (mkdir(dir_.c_str(), 0700) != 0 && errno != EEXIST) {
    log::error("%s: %s", dir_.c_str(), std::strerror(errno));
    return false;
  }
  const std::string tmp = path_ + ".new";
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    log::error("%s: %s", tmp.c_str(), std::strerror(errno));
    return false;
  }
  const bool written = write(fd, key.data(), key.size()) == static_cast<ssize_t>(key.size());
  const bool synced = fsync(fd) == 0;
  close(fd);
  if (!written || !synced || rename(tmp.c_str(), path_.c_str()) != 0) {
    log::error("%s: can't save the pairing key: %s", path_.c_str(), std::strerror(errno));
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

void FileKeyStore::erase() {
  if (unlink(path_.c_str()) != 0 && errno != ENOENT) {
    log::warn("%s: %s", path_.c_str(), std::strerror(errno));
  }
}

PairingState::PairingState(std::unique_ptr<KeyStore> store)
    : store_(std::move(store)), key_(store_->load()) {}

bool PairingState::pairing_open(Millis now) const {
  return !paired() || (window_until_ && now < *window_until_);
}

void PairingState::open_pairing_window(Millis now) {
  window_until_ = now + kPairingWindow;
}

bool PairingState::begin_pairing() {
  if (pairing_in_progress_) return false;
  pairing_in_progress_ = true;
  return true;
}

void PairingState::end_pairing() {
  pairing_in_progress_ = false;
}

bool PairingState::save_key(const Key& key) {
  if (!store_->save(key)) return false;
  if (key_) wipe(key_->data(), key_->size());
  key_ = key;
  window_until_.reset();  // a successful pairing closes the window
  auth_failures_ = 0;
  return true;
}

void PairingState::forget() {
  if (key_) wipe(key_->data(), key_->size());
  key_.reset();
  store_->erase();
}

bool PairingState::auth_locked(Millis now) const {
  return now < locked_until_;
}

void PairingState::auth_failed(Millis now) {
  if (++auth_failures_ >= kMaxAuthFailures) {
    locked_until_ = now + kAuthLockout;
    auth_failures_ = 0;
  }
}

void PairingState::auth_succeeded() {
  auth_failures_ = 0;
}

}  // namespace nesto::protocol
