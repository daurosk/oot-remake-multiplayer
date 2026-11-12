#include "punch.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

namespace coop::server::nat {

namespace {

// Layout: magic 4, type 1, reserved 3 (zero), nonce 8, time 8 (Unix seconds,
// big-endian), from 6, to 6, reserved 4 (zero), then HMAC-SHA256 of those
// 40 bytes.
constexpr std::size_t kSignedBytes = 40;
static_assert(kSignedBytes + 32 == kPunchBytes);
constexpr std::size_t kMaxAnswers = 8;
constexpr std::size_t kMaxTrackedAddresses = 4096;

void put_addr(std::uint8_t* p, Addr a) {
  p[0] = static_cast<std::uint8_t>(a.ip >> 24);
  p[1] = static_cast<std::uint8_t>(a.ip >> 16);
  p[2] = static_cast<std::uint8_t>(a.ip >> 8);
  p[3] = static_cast<std::uint8_t>(a.ip);
  p[4] = static_cast<std::uint8_t>(a.port >> 8);
  p[5] = static_cast<std::uint8_t>(a.port);
}
Addr get_addr(const std::uint8_t* p) {
  Addr a;
  a.ip = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
  a.port = static_cast<std::uint16_t>((p[4] << 8) | p[5]);
  return a;
}

bool same_mac(const std::uint8_t* a, const std::uint8_t* b) {
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < 32; ++i) diff = static_cast<std::uint8_t>(diff | (a[i] ^ b[i]));
  return diff == 0;
}

}  // namespace

std::array<std::uint8_t, kPunchBytes> encode_punch(const Punch& punch, const protocol::Sha256Digest& key) {
  std::array<std::uint8_t, kPunchBytes> out{};
  std::copy(kPunchMagic.begin(), kPunchMagic.end(), out.begin());
  out[4] = static_cast<std::uint8_t>(punch.type);
  std::copy(punch.nonce.begin(), punch.nonce.end(), out.begin() + 8);
  for (std::size_t i = 0; i < 8; ++i) out[16 + i] = static_cast<std::uint8_t>(punch.time >> (56 - 8 * i));
  put_addr(&out[24], punch.from);
  put_addr(&out[30], punch.to);
  const auto mac = protocol::hmac_sha256(key, std::span<const std::uint8_t>(out.data(), kSignedBytes));
  std::copy(mac.begin(), mac.end(), out.begin() + kSignedBytes);
  return out;
}

bool looks_like_punch(std::span<const std::uint8_t> d) {
  return d.size() >= 3 && d[0] == kPunchMagic[0] && d[1] == kPunchMagic[1] && d[2] == kPunchMagic[2];
}

PunchRead read_punch(std::span<const std::uint8_t> d, const protocol::Sha256Digest& key, std::uint64_t unix_now) {
  PunchRead r;
  if (!looks_like_punch(d)) return r;
  r.check = PunchCheck::kBadSize;
  if (d.size() != kPunchBytes || d[3] != kPunchMagic[3]) return r;
  r.check = PunchCheck::kBadType;
  if (d[4] < 1 || d[4] > 4) return r;
  r.check = PunchCheck::kBadSignature;
  const auto mac = protocol::hmac_sha256(key, d.subspan(0, kSignedBytes));
  if (!same_mac(mac.data(), &d[kSignedBytes])) return r;
  r.punch.type = static_cast<PunchType>(d[4]);
  std::copy(d.begin() + 8, d.begin() + 16, r.punch.nonce.begin());
  std::uint64_t time = 0;
  for (std::size_t i = 0; i < 8; ++i) time = (time << 8) | d[16 + i];
  r.punch.time = time;
  r.punch.from = get_addr(&d[24]);
  r.punch.to = get_addr(&d[30]);
  const std::uint64_t age = unix_now > time ? unix_now - time : time - unix_now;
  r.check = age > kPunchMaxAgeSeconds ? PunchCheck::kStale : PunchCheck::kOk;
  return r;
}

bool RateLimit::allow(Addr from, std::uint64_t now_ms) {
  const std::uint64_t window = now_ms / 1000;
  if (window != window_) {
    window_ = window;
    count_ = 0;
    counts_.clear();
  }
  if (count_ >= total_) return false;
  auto it = counts_.find(from);
  if (it == counts_.end()) {
    if (counts_.size() >= kMaxTrackedAddresses) return false;
    it = counts_.emplace(from, 0u).first;
  }
  if (it->second >= per_address_) return false;
  ++it->second;
  ++count_;
  return true;
}

HostRendezvous::HostRendezvous(HostConfig config) : config_(std::move(config)) {
  key_ = punch_key(config_.password);
  if (!config_.random_id) {
    config_.random_id = [] {
      std::random_device device;
      StunId id{};
      for (auto& b : id) b = static_cast<std::uint8_t>(device());
      return id;
    };
  }
}

std::uint64_t HostRendezvous::unix_now() const {
  if (config_.unix_now) return config_.unix_now();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

void HostRendezvous::say(const std::string& text) const {
  if (config_.log) config_.log(text);
}

void HostRendezvous::send_stun(StunAsk& ask) {
  const auto request = stun_request(ask.id);
  if (config_.send) config_.send(ask.server, request);
}

void HostRendezvous::send_punch(PunchType type, const Nonce& nonce, Addr to) {
  Punch p;
  p.type = type;
  p.nonce = nonce;
  p.time = unix_now();
  p.from = public_.value_or(Addr{});
  p.to = to;
  const auto bytes = encode_punch(p, key_);
  if (config_.send) config_.send(to, bytes);
  ++stats_.punches_sent;
}

void HostRendezvous::start(std::uint64_t now_ms) {
  stun_start_ = now_ms;
  stun_tries_ = 1;
  for (const Addr& server : config_.stun_servers) {
    StunAsk ask;
    ask.server = server;
    ask.id = config_.random_id();
    stun_.push_back(ask);
  }
  for (auto& ask : stun_) send_stun(ask);
  if (stun_.empty()) stun_done_ = true;
}

void HostRendezvous::service(std::uint64_t now_ms) {
  if (!stun_done_) {
    const std::uint64_t elapsed = now_ms - stun_start_;
    const bool all = std::all_of(stun_.begin(), stun_.end(), [](const StunAsk& a) { return a.answered; });
    if (all || elapsed >= config_.stun_wait_ms) {
      stun_done_ = true;
    } else if ((stun_tries_ == 1 && elapsed >= 700) || (stun_tries_ == 2 && elapsed >= 1600)) {
      ++stun_tries_;
      for (auto& ask : stun_) {
        if (!ask.answered) send_stun(ask);
      }
    }
  }
  for (auto it = answers_.begin(); it != answers_.end();) {
    if (now_ms >= it->expire) {
      say("punch: stopped answering " + show(it->answer.guest) + " (10 minutes after the answer code)");
      it = answers_.erase(it);
      continue;
    }
    if (now_ms < it->punch_until && now_ms >= it->next_send) {
      send_punch(PunchType::kHostPunch, it->answer.nonce, it->answer.guest);
      it->next_send = now_ms + 200;
    }
    ++it;
  }
}

bool HostRendezvous::on_datagram(Addr from, std::span<const std::uint8_t> data, std::uint64_t now_ms) {
  for (auto& ask : stun_) {
    if (ask.server != from) continue;
    const StunAnswer answer = read_stun(data, ask.id);
    if (answer.result == StunResult::kNotStun) return false;
    if (answer.result != StunResult::kOk || ask.answered || stun_done_) {
      ++stats_.stun_ignored;
      return true;
    }
    ask.answered = true;
    ask.mapped = answer.mapped;
    if (!public_) {
      public_ = answer.mapped;
    } else if (*public_ != answer.mapped) {
      symmetric_ = true;
    }
    return true;
  }
  if (!looks_like_punch(data)) return false;
  if (!limit_.allow(from, now_ms)) {
    ++stats_.rate_limited;
    return true;
  }
  const PunchRead read = read_punch(data, key_, unix_now());
  if (read.check != PunchCheck::kOk) {
    ++stats_.rejected;
    return true;
  }
  switch (read.punch.type) {
    case PunchType::kProbe:
      // Someone with the password hears us directly; the answer is no bigger.
      ++stats_.probes_answered;
      send_punch(PunchType::kProbeAck, read.punch.nonce, from);
      return true;
    case PunchType::kGuestPunch: {
      auto it = std::find_if(answers_.begin(), answers_.end(),
                             [&](const Pending& p) { return p.answer.nonce == read.punch.nonce; });
      if (it == answers_.end()) {
        ++stats_.rejected;
        return true;
      }
      ++stats_.guest_punches;
      if (!it->heard) {
        it->heard = true;
        say("punch: the friend's helper got through from " + show(from) +
            (from == it->answer.guest ? std::string()
                                      : std::string(" (not the answer code's address: their router changes ports)")));
      }
      // Only toward the answer code's address, never toward the sender.
      send_punch(PunchType::kHostPunch, it->answer.nonce, it->answer.guest);
      return true;
    }
    default:
      ++stats_.rejected;
      return true;
  }
}

CodeError HostRendezvous::add_answer(std::string_view text, std::uint64_t now_ms) {
  const AnswerRead read = read_answer(text, config_.password, unix_now());
  if (read.error != CodeError::kOk) return read.error;
  if (!config_.allow_private && !is_public(read.answer.guest.ip)) return CodeError::kNotPublic;
  auto it = std::find_if(answers_.begin(), answers_.end(),
                         [&](const Pending& p) { return p.answer.nonce == read.answer.nonce; });
  if (it == answers_.end()) {
    if (answers_.size() >= kMaxAnswers) answers_.erase(answers_.begin());
    answers_.push_back(Pending{});
    it = answers_.end() - 1;
    it->answer = read.answer;
    it->expire = now_ms + config_.answer_life_ms;
  }
  it->punch_until = now_ms + config_.punch_ms;
  it->next_send = now_ms;
  say("punch: answer code accepted; sending punches to " + show(read.answer.guest) +
      " for 10 seconds. Your friend's Join.bat window says when the game connects.");
  return CodeError::kOk;
}

}  // namespace coop::server::nat
