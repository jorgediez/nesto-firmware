#include "core/Backplate.hpp"

#include <algorithm>
#include <array>

namespace nesto::backplate {
namespace {

constexpr std::array<std::uint8_t, 3> kCommandPreamble = {0xd5, 0xaa, 0x96};
constexpr std::array<std::uint8_t, 4> kReplyPreamble = {0xd5, 0xd5, 0xaa, 0x96};
// Type, length, then the CRC after the payload.
constexpr std::size_t kHeader = 4;
constexpr std::size_t kTrailer = 2;

// What the session asks once the burst is over, and the reply to each. An
// optional question that goes unanswered is skipped: the replies' types are
// assumed from the others' pattern.
struct Question {
  std::uint16_t command;
  std::uint16_t reply;
  bool optional;
};
constexpr Question kQuestions[] = {
    {kGetTfeVersion, kTfeVersion, false},
    {kGetTfeBuildInfo, kTfeBuildInfo, false},
    {kGetBslId, kBslId, false},
    {kGetHardware, kHardware, true},
    {kGetSerial, kSerial, true},
};
constexpr std::size_t kQuestionCount = sizeof kQuestions / sizeof kQuestions[0];

std::uint16_t u16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xff));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

}  // namespace

std::uint16_t crc16(const std::uint8_t* data, std::size_t size, std::uint16_t crc) {
  for (std::size_t i = 0; i < size; ++i) {
    crc = static_cast<std::uint16_t>(crc ^ (data[i] << 8));
    for (int bit = 0; bit < 8; ++bit) {
      crc = static_cast<std::uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
  }
  return crc;
}

std::vector<std::uint8_t> encode(std::uint16_t type, const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> body;
  put_u16(body, type);
  put_u16(body, static_cast<std::uint16_t>(payload.size()));
  body.insert(body.end(), payload.begin(), payload.end());

  std::vector<std::uint8_t> frame(kCommandPreamble.begin(), kCommandPreamble.end());
  frame.insert(frame.end(), body.begin(), body.end());
  put_u16(frame, crc16(body.data(), body.size()));
  return frame;
}

void FrameReader::feed(const std::uint8_t* data, std::size_t size, std::vector<Frame>& out) {
  buffer_.insert(buffer_.end(), data, data + size);
  std::size_t at = 0;
  for (;;) {
    // The next preamble.
    const auto start = std::search(buffer_.begin() + static_cast<std::ptrdiff_t>(at), buffer_.end(),
                                   kReplyPreamble.begin(), kReplyPreamble.end());
    if (start == buffer_.end()) {
      // Keep a possible start of a preamble, drop the rest.
      const std::size_t keep = std::min<std::size_t>(buffer_.size() - at, kReplyPreamble.size() - 1);
      at = buffer_.size() - keep;
      break;
    }
    at = static_cast<std::size_t>(start - buffer_.begin());
    const std::size_t body = at + kReplyPreamble.size();
    if (buffer_.size() < body + kHeader) break;  // wait for the rest
    const std::uint16_t type = u16(&buffer_[body]);
    const std::size_t length = u16(&buffer_[body + 2]);
    if (length > kMaxPayload) {
      // Out of step: look for the next preamble after this one.
      ++bad_frames_;
      at += 1;
      continue;
    }
    const std::size_t end = body + kHeader + length + kTrailer;
    if (buffer_.size() < end) break;
    const std::uint16_t expected = u16(&buffer_[end - kTrailer]);
    if (crc16(&buffer_[body], kHeader + length) != expected) {
      ++bad_frames_;
      at += 1;
      continue;
    }
    Frame frame;
    frame.type = type;
    frame.payload.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(body + kHeader),
                         buffer_.begin() + static_cast<std::ptrdiff_t>(end - kTrailer));
    out.push_back(std::move(frame));
    at = end;
  }
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(at));
}

std::optional<Power> decode_power(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 14) return std::nullopt;
  Power power;
  // The stock software logs 8e; with Nesto they read ce. What 0x40 means
  // isn't known: the battery held with it set (docs/BACKPLATE.md).
  power.flags = payload[1];
  // Byte 6 matched the stock software's "Vusb" on a thermostat on a stand.
  power.usb = u16(&payload[6]) / 1000.0;
  power.input = u16(&payload[8]) / 100.0;
  power.operating = u16(&payload[10]) / 1000.0;
  power.battery = u16(&payload[12]) / 1000.0;
  return power;
}

std::optional<Climate> decode_climate(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 4) return std::nullopt;
  Climate climate;
  climate.celsius = static_cast<std::int16_t>(u16(&payload[0])) / 100.0;
  climate.humidity = u16(&payload[2]) / 10.0;
  return climate;
}

std::string text(const Frame& frame) {
  std::string out;
  for (std::uint8_t c : frame.payload) {
    if (c == 0) break;
    out += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : ' ';
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

void Session::start(Millis now) {
  phase_ = Phase::kBreak;
  since_ = now;
  heard_ = now;
  break_wanted_ = true;
  output_.clear();
  fet_presence_.reset();
  query_ = 0;
}

void Session::hand_back(Millis now) {
  // As the stock software's log shows it stopping: "sending reset, waiting
  // for wire state response", the burst, "sending wire ACK".
  output_.clear();
  fet_presence_.reset();
  handed_back_ = false;
  phase_ = Phase::kHandingBack;
  since_ = now;
  send(kReset);
}

void Session::send(std::uint16_t type, const std::vector<std::uint8_t>& payload) {
  const std::vector<std::uint8_t> frame = encode(type, payload);
  output_.insert(output_.end(), frame.begin(), frame.end());
}

void Session::store(std::size_t question, const Frame& frame) {
  switch (question) {
    case 0: version_ = text(frame); break;
    case 1: build_info_ = text(frame); break;
    case 2: {
      static const char kHex[] = "0123456789abcdef";
      bsl_id_.clear();
      for (std::uint8_t b : frame.payload) {
        bsl_id_ += kHex[b >> 4];
        bsl_id_ += kHex[b & 0x0f];
      }
      break;
    }
    case 3: model_ = text(frame); break;
    default: serial_ = text(frame); break;
  }
}

void Session::ask(Millis now) {
  if (query_ >= kQuestionCount) {
    enter_running(now);
    return;
  }
  send(kQuestions[query_].command);
  since_ = now;
}

void Session::enter_running(Millis now) {
  if (config_.power_steal) send(kSetPowerSteal, {0, 0, 0, 0});
  phase_ = Phase::kRunning;
  since_ = now;
  ++handshakes_;
  problem_.clear();
  // Both at once, to start with.
  send(kStatusRequest);
  send(kRequestBuffers);
  keepalive_ = now;
  buffers_ = now;
}

void Session::fail(Millis now, const std::string& problem) {
  problem_ = problem;
  phase_ = Phase::kRetry;
  since_ = now;
  ++restarts_;
}

void Session::tick(Millis now) {
  const Millis in_phase = now - since_;
  switch (phase_) {
    case Phase::kStopped:
      break;
    case Phase::kBreak:
      if (in_phase >= config_.break_pause) {
        send(kReset);
        phase_ = Phase::kBurst;
        since_ = now;
      }
      break;
    case Phase::kBurst:
      if (in_phase >= config_.burst_timeout) fail(now, "no start-up burst ending in BRK");
      break;
    case Phase::kInfo:
      if (in_phase >= config_.reply_timeout) {
        if (query_ < kQuestionCount && kQuestions[query_].optional) {
          ++query_;
          ask(now);
        } else {
          fail(now, "no reply to a version question");
        }
      }
      break;
    case Phase::kRunning:
      if (now - heard_ >= config_.silence) {
        fail(now, "the backplate went quiet");
        break;
      }
      if (now - keepalive_ >= config_.keepalive) {
        send(kStatusRequest);
        keepalive_ = now;
      }
      if (now - buffers_ >= config_.buffers) {
        send(kRequestBuffers);
        buffers_ = now;
      }
      break;
    case Phase::kRetry:
      if (in_phase >= config_.retry) start(now);
      break;
    case Phase::kHandingBack:
      if (in_phase >= config_.hand_back_timeout) {
        problem_ = "no start-up burst while handing back";
        phase_ = Phase::kStopped;
      }
      break;
  }
}

void Session::receive(Millis now, const Frame& frame) {
  heard_ = now;
  switch (frame.type) {
    case kState:
      if (auto power = decode_power(frame.payload)) power_ = power;
      break;
    case kClimate:
      if (auto climate = decode_climate(frame.payload)) climate_ = climate;
      break;
    default:
      break;
  }

  switch (phase_) {
    case Phase::kBurst:
      if (frame.type == kFetPresence) {
        fet_presence_ = frame.payload;
      } else if (frame.type == kAscii && text(frame) == "BRK") {
        if (!fet_presence_) {
          fail(now, "the start-up burst had no wire presence");
          return;
        }
        send(kFetPresenceAck, *fet_presence_);
        phase_ = Phase::kInfo;
        query_ = 0;
        ask(now);
      } else if (frame.type == kAscii && version_.empty()) {
        // The burst opens with the firmware's version and build time.
        version_ = text(frame);
      }
      break;
    case Phase::kInfo:
      if (query_ < kQuestionCount && frame.type == kQuestions[query_].reply) {
        store(query_, frame);
        ++query_;
        ask(now);
      }
      break;
    case Phase::kRunning:
      if (frame.type == kEndOfBuffers) send(kEndOfBuffersAck);
      break;
    case Phase::kHandingBack:
      if (frame.type == kFetPresence && !fet_presence_) {
        // The stock software acknowledges the wire state as it comes.
        fet_presence_ = frame.payload;
        send(kFetPresenceAck, frame.payload);
      } else if (frame.type == kAscii && text(frame) == "BRK" && fet_presence_) {
        handed_back_ = true;
        phase_ = Phase::kStopped;
      }
      break;
    default:
      break;
  }
}

bool Session::take_break() {
  const bool wanted = break_wanted_;
  break_wanted_ = false;
  return wanted;
}

std::vector<std::uint8_t> Session::take_output() {
  std::vector<std::uint8_t> out;
  out.swap(output_);
  return out;
}

}  // namespace nesto::backplate
