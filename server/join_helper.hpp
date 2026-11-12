#pragma once
// coop_server --join: the friend's side of an invite code (Join.bat starts it
// in its own window; docs/play-online.md). The game connects to this helper
// on 127.0.0.1; the helper sends the game's datagrams to the host from its
// own UDP socket, the one it asked a STUN server about and punched the
// routers with, so the address in the answer code is the game's.
//
// 1. Tries the host directly for a few seconds (signed probes; the host's
//    server answers them only for someone with the join password).
// 2. No answer: makes an ANSWER CODE (this socket's internet address, a
//    nonce, the time, signed) for the friend to send to the host, and sends
//    signed punches to the host until the host's punches come through.
// 3. Then forwards the game's traffic both ways.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "nat_codes.hpp"
#include "punch.hpp"
#include "stun.hpp"

namespace coop::server::nat {

struct JoinConfig {
  Addr host;                       // from the invite code
  std::string password;            // from the invite code (signs punches)
  std::vector<Addr> stun_servers;
  std::uint16_t game_port = 27512; // the game connects to 127.0.0.1 here; 0: any
  std::uint64_t direct_ms = 3000;  // the direct try, before the answer code
  std::uint64_t punch_ms = 180'000;  // then waiting for the host this long before the help text
  bool probe = true;               // tests: false to go the punching way
  std::function<void(const std::string&)> log;
  std::function<void(const std::string&)> on_answer;  // the answer code, once
  std::function<std::uint64_t()> unix_now;            // empty: the system clock
};

enum class JoinState { kTrying, kWaitingForHost, kOpen };

class JoinHelper {
 public:
  explicit JoinHelper(JoinConfig config);
  ~JoinHelper();
  JoinHelper(const JoinHelper&) = delete;
  JoinHelper& operator=(const JoinHelper&) = delete;

  bool start(std::string& error);
  // Waits up to wait_ms for a datagram, then does what is due.
  void service(std::uint32_t wait_ms);
  void stop();

  JoinState state() const { return state_; }
  bool direct() const { return direct_; }
  std::uint16_t game_port() const;
  std::uint16_t public_socket_port() const;
  const std::string& answer_code() const { return answer_; }
  std::optional<Addr> public_address() const { return public_; }

  struct Stats {
    std::uint64_t to_host = 0;
    std::uint64_t to_game = 0;
    std::uint64_t rejected = 0;  // punch-looking datagrams that were not the host's, signed
    std::uint64_t dropped = 0;   // from anyone else, or before the way was open
  };
  const Stats& stats() const { return stats_; }

 private:
  struct StunAsk {
    Addr server;
    StunId id{};
    bool answered = false;
    Addr mapped;
  };
  std::uint64_t now_ms() const;
  std::uint64_t unix_now() const;
  void say(const std::string& text) const;
  void send_public(Addr to, std::span<const std::uint8_t> bytes);
  void send_punch(PunchType type);
  void on_public(Addr from, std::span<const std::uint8_t> data);
  void on_game(Addr from, std::span<const std::uint8_t> data);
  void tick(std::uint64_t now);
  void open(bool direct);

  JoinConfig config_;
  protocol::Sha256Digest key_{};
  Nonce nonce_{};
  ENetSocket public_socket_ = ENET_SOCKET_NULL;
  ENetSocket game_socket_ = ENET_SOCKET_NULL;
  std::optional<Addr> game_;
  std::vector<StunAsk> stun_;
  int stun_tries_ = 0;
  std::optional<Addr> public_;
  bool symmetric_ = false;
  JoinState state_ = JoinState::kTrying;
  bool direct_ = false;
  bool helped_ = false;
  std::string answer_;
  std::uint64_t start_ = 0;
  std::uint64_t waiting_since_ = 0;
  std::uint64_t next_probe_ = 0;
  std::uint64_t next_punch_ = 0;
  Stats stats_;
};

// What to do when no way works (both helpers' and the server's windows).
std::string no_way_help();

}  // namespace coop::server::nat
