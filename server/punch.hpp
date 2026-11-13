#pragma once
// NAT hole punching without a rendezvous server (docs/play-online.md,
// "When the invite code does not connect"; issue 165).
//
// The host's game server and the friend's join helper (join_helper.hpp) send
// each other small signed datagrams ("punches") on the game's UDP port until
// both routers let the other side's packets in; then ENet connects over the
// same ports. Every punch is signed (HMAC-SHA256 with a key made from the
// join password, nat_codes.hpp punch_key) over the type, nonce, time and both
// addresses, and is only taken within 60 s of its time. The server answers
// nothing to a datagram that is not signed with its password, and only sends
// punches toward an address named in a signed answer code that the host
// pasted, so a stranger who scans the port learns nothing new and cannot
// make it send packets elsewhere. No answer is bigger than its question.
//
// Host side (HostRendezvous) is pure logic: the server hands it datagrams that
// reach the game port before ENet sees them and gives it a send function.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nat_codes.hpp"
#include "stun.hpp"

namespace coop::server::nat {

// --- Punch datagrams ---
enum class PunchType : std::uint8_t {
  kProbe = 1,       // helper -> server: "do you hear me directly?"
  kProbeAck = 2,    // server -> helper: "yes"
  kGuestPunch = 3,  // helper -> server, after the answer code
  kHostPunch = 4,   // server -> helper's answer address
};
struct Punch {
  PunchType type = PunchType::kProbe;
  Nonce nonce{};           // the helper's
  std::uint64_t time = 0;  // Unix seconds of the sender
  Addr from;               // the sender's internet address, if known
  Addr to;                 // where it was sent
};
// "OCP" and a version: ENet's first byte is never 'O' with this server's
// peer count, so these never reach ENet.
inline constexpr std::array<std::uint8_t, 4> kPunchMagic = {'O', 'C', 'P', 1};
inline constexpr std::size_t kPunchBytes = 72;  // 40 signed bytes and the HMAC
inline constexpr std::uint64_t kPunchMaxAgeSeconds = 60;

std::array<std::uint8_t, kPunchBytes> encode_punch(const Punch& punch, const protocol::Sha256Digest& key);
bool looks_like_punch(std::span<const std::uint8_t> datagram);
enum class PunchCheck { kOk, kNotPunch, kBadSize, kBadType, kBadSignature, kStale };
struct PunchRead {
  PunchCheck check = PunchCheck::kNotPunch;
  Punch punch;
};
PunchRead read_punch(std::span<const std::uint8_t> datagram, const protocol::Sha256Digest& key,
                     std::uint64_t unix_now);

// At most `per_address` datagrams a second from one address and `total` from
// all of them; checked before any signature is computed.
class RateLimit {
 public:
  RateLimit(std::uint32_t per_address, std::uint32_t total) : per_address_(per_address), total_(total) {}
  bool allow(Addr from, std::uint64_t now_ms);

 private:
  std::uint32_t per_address_;
  std::uint32_t total_;
  std::uint64_t window_ = ~0ull;
  std::uint32_t count_ = 0;
  std::map<Addr, std::uint32_t> counts_;
};

// --- The game server's side ---
struct HostConfig {
  std::string password;           // must not be empty
  std::vector<Addr> stun_servers; // asked once at the start
  std::function<void(Addr, std::span<const std::uint8_t>)> send;
  std::function<void(const std::string&)> log;
  std::function<std::uint64_t()> unix_now;  // empty: the system clock
  std::function<StunId()> random_id;        // empty: random_device
  bool allow_private = false;               // tests on 127.0.0.1 only
  std::uint64_t stun_wait_ms = 3000;
  std::uint64_t punch_ms = 10'000;          // how long the server punches after a paste
  std::uint64_t answer_life_ms = 600'000;   // how long it answers that friend's punches
};

class HostRendezvous {
 public:
  explicit HostRendezvous(HostConfig config);

  // Sends the STUN requests.
  void start(std::uint64_t now_ms);
  // A datagram that reached the game port: true when it was ours (STUN from
  // a server we asked, or anything that looks like a punch), so ENet does not
  // see it.
  bool on_datagram(Addr from, std::span<const std::uint8_t> data, std::uint64_t now_ms);
  // STUN retries and punches; call every loop.
  void service(std::uint64_t now_ms);

  bool stun_done() const { return stun_done_; }
  std::optional<Addr> public_address() const { return public_; }
  // Two STUN servers saw different ports: the router gives each destination
  // its own port, and punching will most likely not work.
  bool symmetric() const { return symmetric_; }

  // An answer code the host pasted.
  CodeError add_answer(std::string_view text, std::uint64_t now_ms);
  std::size_t answers() const { return answers_.size(); }

  struct Stats {
    std::uint64_t probes_answered = 0;
    std::uint64_t punches_sent = 0;
    std::uint64_t guest_punches = 0;  // signed ones for a pasted answer
    std::uint64_t rejected = 0;       // unsigned, stale, unknown nonce
    std::uint64_t rate_limited = 0;
    std::uint64_t stun_ignored = 0;   // from a STUN server we asked, but not our answer
  };
  const Stats& stats() const { return stats_; }

 private:
  struct StunAsk {
    Addr server;
    StunId id{};
    bool answered = false;
    Addr mapped;
  };
  struct Pending {
    Answer answer;
    std::uint64_t punch_until = 0;
    std::uint64_t expire = 0;
    std::uint64_t next_send = 0;
    bool heard = false;
  };
  std::uint64_t unix_now() const;
  void say(const std::string& text) const;
  void send_punch(PunchType type, const Nonce& nonce, Addr to);
  void send_stun(StunAsk& ask);

  HostConfig config_;
  protocol::Sha256Digest key_{};
  std::vector<StunAsk> stun_;
  std::uint64_t stun_start_ = 0;
  int stun_tries_ = 0;
  bool stun_done_ = false;
  std::optional<Addr> public_;
  bool symmetric_ = false;
  std::vector<Pending> answers_;
  RateLimit limit_{10, 100};
  Stats stats_;
};

}  // namespace coop::server::nat
