#include "protocol/Frame.hpp"

#include <algorithm>

namespace nesto::protocol {

Bytes encode_frame(const Bytes& payload) {
  const auto n = static_cast<std::uint32_t>(payload.size());
  Bytes out = {static_cast<std::uint8_t>(n >> 24), static_cast<std::uint8_t>(n >> 16),
               static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n)};
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

void FrameReader::feed(const std::uint8_t* data, std::size_t size) {
  std::size_t at = 0;
  while (at < size && !failed()) {
    if (header_.size() < 4) {
      header_.push_back(data[at++]);
      if (header_.size() < 4) continue;
      expected_ = (std::size_t{header_[0]} << 24) | (std::size_t{header_[1]} << 16) |
                  (std::size_t{header_[2]} << 8) | header_[3];
      if (expected_ == 0 || expected_ > kMaxFrame) {
        error_ = "frame length " + std::to_string(expected_) + " is out of range";
        return;
      }
      body_.clear();
      body_.reserve(expected_);
      continue;
    }
    const std::size_t take = std::min(size - at, expected_ - body_.size());
    body_.insert(body_.end(), data + at, data + at + take);
    at += take;
    if (body_.size() == expected_) {
      ready_.push_back(std::move(body_));
      body_ = Bytes();
      header_.clear();
    }
  }
}

std::optional<Bytes> FrameReader::next() {
  if (ready_.empty()) return std::nullopt;
  Bytes out = std::move(ready_.front());
  ready_.pop_front();
  return out;
}

}  // namespace nesto::protocol
