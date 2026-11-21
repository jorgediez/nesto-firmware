#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/Input.hpp"
#include "monocypher-ed25519.h"
#include "protocol/Message.hpp"

namespace nesto::protocol {

/// Firmware updates from Home Assistant (PROTOCOL.md section 19). The panel
/// trusts a program only when a release key signed it: Home Assistant just
/// carries the bytes.

using OtaKey = std::array<std::uint8_t, 32>;        // Ed25519 public key
using OtaSignature = std::array<std::uint8_t, 64>;  // Ed25519 signature
using Sha512 = std::array<std::uint8_t, 64>;

/// What a release's signature covers: the model, the version, the size and
/// the program's SHA-512, one per line after a fixed first line, each ending
/// in a newline. tools/ota_sign.py signs the same text.
std::string ota_signed_text(std::string_view model, std::string_view version, std::uint64_t size,
                            const Sha512& sha512);

/// Whether one of `keys` signed this text.
bool ota_signature_valid(std::string_view text, const OtaSignature& signature,
                         const std::vector<OtaKey>& keys);

/// Compares dotted versions number by number: below zero if a is older, zero
/// if the same, above zero if newer. A part that isn't a number counts as 0.
int compare_versions(std::string_view a, std::string_view b);

/// The keys this build trusts, compiled in from keys/ (see CMakeLists.txt).
const std::vector<OtaKey>& ota_trusted_keys();

/// An answer to one of the update's requests: ok, or an error for the result.
struct OtaResult {
  bool ok = true;
  ErrorCode code = ErrorCode::Internal;
  std::string message;

  static OtaResult success() { return {}; }
  static OtaResult error(ErrorCode code, std::string message) { return {false, code, std::move(message)}; }
};

/// Receives a new program over the protocol and puts it in place.
///
/// The program goes to TARGET.new as it arrives, its SHA-512 computed on the
/// way, so it never sits in memory. Applying it keeps the running program as
/// TARGET.prev (a hard link, so the running one is untouched), leaves a
/// "pending" marker in the state directory, and renames TARGET.new over
/// TARGET. The boot script restarts Nesto, and puts TARGET.prev back if the
/// new one doesn't confirm itself (scripts/nesto-boot.sh).
class OtaReceiver {
 public:
  struct Config {
    std::string target;           // the program to replace, e.g. /nesto/nesto
    std::string state_dir;        // where the pending marker goes
    std::string current_version;  // the running program's
    std::string model;            // this panel's, which a release must name
    std::vector<OtaKey> keys;
    std::uint64_t max_size = 16 * 1024 * 1024;
  };

  /// Without a chunk for this long, the update is dropped.
  static constexpr Millis kStallTimeout = 60 * 1000;
  /// The most one chunk may carry, decoded.
  static constexpr std::size_t kMaxChunk = 64 * 1024;
  /// Free space kept on the target's file system beyond the program.
  static constexpr std::uint64_t kSpareSpace = 512 * 1024;

  enum class Stage { Idle, Receiving, Received };

  explicit OtaReceiver(Config config);
  ~OtaReceiver();
  OtaReceiver(const OtaReceiver&) = delete;
  OtaReceiver& operator=(const OtaReceiver&) = delete;

  /// `ota_begin`: checks the offer (model, version, size, signature, space)
  /// and starts TARGET.new. A new offer drops one in progress.
  OtaResult begin(const Json& body, Millis now);
  /// `ota_chunk`: the next piece, in order.
  OtaResult chunk(const Json& body, Millis now);
  /// `ota_end`: all of it arrived, and its SHA-512 matches.
  OtaResult end();
  /// `ota_apply`: puts it in place. Nesto should then exit for the boot
  /// script to start the new one.
  OtaResult apply();

  /// Drops a stalled update.
  void tick(Millis now);
  /// Drops the update in progress, deleting TARGET.new.
  void abort(const char* why);

  Stage stage() const { return stage_; }
  std::uint64_t received() const { return received_; }
  std::uint64_t size() const { return size_; }
  const std::string& version() const { return version_; }

  /// The marker an applied update leaves until the new program confirms it.
  static std::string pending_path(const std::string& state_dir);
  /// Called once Home Assistant is connected: the running program works, so
  /// the boot script keeps it. Returns the version the marker named, if any.
  static std::string confirm(const std::string& state_dir);

 private:
  std::string new_path() const { return config_.target + ".new"; }
  std::string prev_path() const { return config_.target + ".prev"; }
  void close_file();

  Config config_;
  Stage stage_ = Stage::Idle;
  int fd_ = -1;
  std::string version_;
  std::uint64_t size_ = 0;
  std::uint64_t received_ = 0;
  Sha512 expected_{};
  crypto_sha512_ctx hash_{};
  Millis last_ = 0;
};

}  // namespace nesto::protocol
