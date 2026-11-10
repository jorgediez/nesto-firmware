#pragma once

#include <memory>
#include <optional>
#include <string>

#include "core/Input.hpp"
#include "protocol/Bytes.hpp"

namespace nesto::protocol {

/// Where the pairing key lives between runs.
class KeyStore {
 public:
  virtual ~KeyStore() = default;
  virtual std::optional<Key> load() = 0;
  virtual bool save(const Key& key) = 0;
  virtual void erase() = 0;
};

/// Keeps the key in `<dir>/pairing.key`, readable only by its owner, and
/// replaces it atomically so a power cut can't leave half a key.
class FileKeyStore : public KeyStore {
 public:
  explicit FileKeyStore(std::string dir);
  std::optional<Key> load() override;
  bool save(const Key& key) override;
  void erase() override;

 private:
  std::string dir_;
  std::string path_;
};

/// Keeps the key in memory only, for tests.
class MemoryKeyStore : public KeyStore {
 public:
  std::optional<Key> load() override { return key_; }
  bool save(const Key& key) override {
    key_ = key;
    return true;
  }
  void erase() override { key_.reset(); }

 private:
  std::optional<Key> key_;
};

/// Pairing state shared by every connection: the key, whether pairing is
/// allowed (PROTOCOL.md section 6.1), the one pairing that may be in progress,
/// and the back-off after failed authentications (section 7.1).
class PairingState {
 public:
  static constexpr Millis kPairingWindow = 5 * 60 * 1000;
  static constexpr int kMaxAuthFailures = 3;
  static constexpr Millis kAuthLockout = 30 * 1000;

  explicit PairingState(std::unique_ptr<KeyStore> store);

  const std::optional<Key>& key() const { return key_; }
  bool paired() const { return key_.has_value(); }

  /// Pairing is open while unpaired, or for a while after the user asks for it
  /// on the panel (Settings, Pair with Home Assistant).
  bool pairing_open(Millis now) const;
  void open_pairing_window(Millis now);

  /// Only one pairing at a time: false if another connection is pairing.
  bool begin_pairing();
  void end_pairing();

  /// Stores a new key, replacing any old one. False if it can't be saved.
  bool save_key(const Key& key);
  void forget();

  bool auth_locked(Millis now) const;
  void auth_failed(Millis now);
  void auth_succeeded();

 private:
  std::unique_ptr<KeyStore> store_;
  std::optional<Key> key_;
  std::optional<Millis> window_until_;
  bool pairing_in_progress_ = false;
  int auth_failures_ = 0;
  Millis locked_until_ = 0;
};

}  // namespace nesto::protocol
