#include "protocol/Ota.hpp"

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "core/Log.hpp"
#include "protocol/Bytes.hpp"

namespace nesto::protocol {
namespace {

std::string dir_of(const std::string& path) {
  const auto slash = path.rfind('/');
  if (slash == std::string::npos) return ".";
  return slash == 0 ? "/" : path.substr(0, slash);
}

/// Writes all of it, retrying short writes.
bool write_all(int fd, const std::uint8_t* data, std::size_t size) {
  while (size > 0) {
    const ssize_t n = ::write(fd, data, size);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data += n;
    size -= static_cast<std::size_t>(n);
  }
  return true;
}

/// A small file, written and flushed to the flash before it's renamed in.
bool write_file(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  const bool ok = write_all(fd, reinterpret_cast<const std::uint8_t*>(text.data()), text.size()) &&
                  ::fsync(fd) == 0;
  ::close(fd);
  return ok && ::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string error_text(const char* what, const std::string& path) {
  return std::string(what) + " " + path + ": " + std::strerror(errno);
}

}  // namespace

std::string ota_signed_text(std::string_view model, std::string_view version, std::uint64_t size,
                            const Sha512& sha512) {
  std::string text = "nesto-ota-v1\n";
  text += "model=" + std::string(model) + "\n";
  text += "version=" + std::string(version) + "\n";
  text += "size=" + std::to_string(size) + "\n";
  text += "sha512=" + hex_encode(sha512) + "\n";
  return text;
}

bool ota_signature_valid(std::string_view text, const OtaSignature& signature,
                         const std::vector<OtaKey>& keys) {
  for (const OtaKey& key : keys) {
    if (crypto_ed25519_check(signature.data(), key.data(),
                             reinterpret_cast<const std::uint8_t*>(text.data()), text.size()) == 0) {
      return true;
    }
  }
  return false;
}

int compare_versions(std::string_view a, std::string_view b) {
  auto next = [](std::string_view& s) {
    long value = 0;
    std::size_t i = 0;
    while (i < s.size() && s[i] != '.') {
      if (s[i] >= '0' && s[i] <= '9') value = value * 10 + (s[i] - '0');
      ++i;
    }
    s = i < s.size() ? s.substr(i + 1) : std::string_view{};
    return value;
  };
  while (!a.empty() || !b.empty()) {
    const long x = next(a);
    const long y = next(b);
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

OtaReceiver::OtaReceiver(Config config) : config_(std::move(config)) {}

OtaReceiver::~OtaReceiver() {
  if (stage_ != Stage::Idle) abort("the panel stopped");
}

OtaResult OtaReceiver::begin(const Json& body, Millis now) {
  if (stage_ != Stage::Idle) abort("a new update started");

  const auto& model = body.find("model");
  const auto& version = body.find("version");
  const auto& size = body.find("size");
  const auto& sha512 = body.find("sha512");
  const auto& signature = body.find("signature");
  if (model == body.end() || !model->is_string() || version == body.end() || !version->is_string() ||
      size == body.end() || !size->is_number_integer() || size->get<long long>() < 0 || sha512 == body.end() ||
      !sha512->is_string() || signature == body.end() || !signature->is_string()) {
    return OtaResult::error(ErrorCode::BadRequest,
                            "ota_begin needs model, version, size, sha512 and signature");
  }
  const auto hash = hex_decode(sha512->get<std::string>());
  const auto sig = base64_decode_fixed<64>(signature->get<std::string>());
  if (!hash || hash->size() != 64 || !sig) {
    return OtaResult::error(ErrorCode::BadRequest, "sha512 is 128 hex digits, signature 64 bytes");
  }
  Sha512 expected{};
  std::copy(hash->begin(), hash->end(), expected.begin());
  const std::string offered_model = model->get<std::string>();
  const std::string offered_version = version->get<std::string>();
  const std::uint64_t offered_size = size->get<std::uint64_t>();

  if (offered_model != config_.model) {
    return OtaResult::error(ErrorCode::Unsupported,
                            "a release for " + offered_model + ", not " + config_.model);
  }
  if (offered_size == 0 || offered_size > config_.max_size) {
    return OtaResult::error(ErrorCode::TooLarge, "a program of " + std::to_string(offered_size) +
                                                     " bytes; at most " +
                                                     std::to_string(config_.max_size));
  }
  if (compare_versions(offered_version, config_.current_version) < 0) {
    return OtaResult::error(ErrorCode::Downgrade, offered_version + " is older than " +
                                                      config_.current_version + ", which runs");
  }
  const std::string text = ota_signed_text(offered_model, offered_version, offered_size, expected);
  if (!ota_signature_valid(text, *sig, config_.keys)) {
    return OtaResult::error(ErrorCode::BadSignature, "no release key this panel trusts signed it");
  }
  struct statvfs fs {};
  if (::statvfs(dir_of(config_.target).c_str(), &fs) == 0) {
    const std::uint64_t free_bytes = static_cast<std::uint64_t>(fs.f_bavail) * fs.f_frsize;
    if (free_bytes < offered_size + kSpareSpace) {
      return OtaResult::error(ErrorCode::NoSpace, std::to_string(free_bytes / 1024) + " KiB free");
    }
  }

  fd_ = ::open(new_path().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (fd_ < 0) return OtaResult::error(ErrorCode::Internal, error_text("can't create", new_path()));
  crypto_sha512_init(&hash_);
  version_ = offered_version;
  size_ = offered_size;
  received_ = 0;
  expected_ = expected;
  last_ = now;
  stage_ = Stage::Receiving;
  log::info("update to Nesto %s: receiving %llu bytes", version_.c_str(),
            static_cast<unsigned long long>(size_));
  return OtaResult::success();
}

OtaResult OtaReceiver::chunk(const Json& body, Millis now) {
  if (stage_ != Stage::Receiving) return OtaResult::error(ErrorCode::BadRequest, "no update started");
  const auto& offset = body.find("offset");
  const auto& data = body.find("data");
  if (offset == body.end() || !offset->is_number_integer() || offset->get<long long>() < 0 || data == body.end() || !data->is_string()) {
    return OtaResult::error(ErrorCode::BadRequest, "ota_chunk needs offset and data");
  }
  if (offset->get<std::uint64_t>() != received_) {
    return OtaResult::error(ErrorCode::BadRequest, "the next offset is " + std::to_string(received_));
  }
  const auto bytes = base64_decode(data->get<std::string>());
  if (!bytes) return OtaResult::error(ErrorCode::BadRequest, "data isn't base64");
  if (bytes->empty() || bytes->size() > kMaxChunk) {
    return OtaResult::error(ErrorCode::TooLarge, "a chunk is 1 byte to 64 KiB");
  }
  if (received_ + bytes->size() > size_) {
    return OtaResult::error(ErrorCode::BadRequest, "more than the size given");
  }
  if (!write_all(fd_, bytes->data(), bytes->size())) {
    const std::string why = error_text("can't write", new_path());
    abort("writing failed");
    return OtaResult::error(ErrorCode::NoSpace, why);
  }
  crypto_sha512_update(&hash_, bytes->data(), bytes->size());
  received_ += bytes->size();
  last_ = now;
  return OtaResult::success();
}

OtaResult OtaReceiver::end() {
  if (stage_ != Stage::Receiving) return OtaResult::error(ErrorCode::BadRequest, "no update started");
  if (received_ != size_) {
    return OtaResult::error(ErrorCode::BadRequest, std::to_string(received_) + " of " +
                                                       std::to_string(size_) + " bytes arrived");
  }
  Sha512 got{};
  crypto_sha512_final(&hash_, got.data());
  if (got != expected_) {
    abort("its SHA-512 didn't match");
    return OtaResult::error(ErrorCode::BadSignature, "the program doesn't match its SHA-512");
  }
  if (::fsync(fd_) != 0) {
    const std::string why = error_text("can't flush", new_path());
    abort("flushing failed");
    return OtaResult::error(ErrorCode::Internal, why);
  }
  close_file();
  stage_ = Stage::Received;
  log::info("update to Nesto %s: received and checked", version_.c_str());
  return OtaResult::success();
}

OtaResult OtaReceiver::apply() {
  if (stage_ != Stage::Received) {
    return OtaResult::error(ErrorCode::BadRequest, "no checked update to apply");
  }
  // The marker first: if anything below fails half-way, the program that
  // starts next confirms or is rolled back, either way a working one. The
  // boot script's count of starts begins afresh.
  ::unlink((pending_path(config_.state_dir) + ".starts").c_str());
  if (!write_file(pending_path(config_.state_dir), version_ + "\n")) {
    return OtaResult::error(ErrorCode::Internal,
                            error_text("can't write", pending_path(config_.state_dir)));
  }
  ::unlink(prev_path().c_str());
  if (::access(config_.target.c_str(), F_OK) == 0 &&
      ::link(config_.target.c_str(), prev_path().c_str()) != 0) {
    const std::string why = error_text("can't keep", config_.target);
    ::unlink(pending_path(config_.state_dir).c_str());
    return OtaResult::error(ErrorCode::Internal, why);
  }
  if (::rename(new_path().c_str(), config_.target.c_str()) != 0) {
    const std::string why = error_text("can't replace", config_.target);
    ::unlink(pending_path(config_.state_dir).c_str());
    return OtaResult::error(ErrorCode::Internal, why);
  }
  ::sync();
  stage_ = Stage::Idle;
  log::info("update to Nesto %s: in place; the previous one kept as %s", version_.c_str(),
            prev_path().c_str());
  return OtaResult::success();
}

void OtaReceiver::tick(Millis now) {
  if (stage_ == Stage::Receiving && now - last_ >= kStallTimeout) abort("nothing arrived for a minute");
}

void OtaReceiver::abort(const char* why) {
  if (stage_ == Stage::Idle) return;
  close_file();
  ::unlink(new_path().c_str());
  log::warn("update to Nesto %s dropped: %s", version_.c_str(), why);
  stage_ = Stage::Idle;
  received_ = 0;
}

void OtaReceiver::close_file() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

std::string OtaReceiver::pending_path(const std::string& state_dir) {
  return state_dir + "/ota-pending";
}

std::string OtaReceiver::confirm(const std::string& state_dir) {
  const std::string path = pending_path(state_dir);
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return "";
  char line[64] = {};
  if (!std::fgets(line, sizeof line, f)) line[0] = '\0';
  std::fclose(f);
  ::unlink(path.c_str());
  ::unlink((path + ".starts").c_str());
  std::string version(line);
  while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) version.pop_back();
  return version;
}

}  // namespace nesto::protocol
