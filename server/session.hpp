#pragma once

// The relay server's rules, with no sockets in them. The ENet loop
// (enet_server.hpp) feeds a Session the connections, packets and clock, and
// carries out what it asks for (send these bytes, drop that connection).
// That keeps every rule here testable without a network.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop/protocol/messages.hpp"

namespace coop::server {

// The transport's own number for a connection, never reused while it lives.
using ConnectionId = std::uint32_t;

struct SessionConfig {
  // The game build every player must run. Empty: the first player to join
  // decides it, and it is forgotten again when the session empties.
  std::string game_build_id;
  std::uint16_t tick_rate = 60;
  std::uint64_t hello_timeout_us = 10'000'000;    // connect to HELLO
  std::uint64_t silence_timeout_us = 10'000'000;  // nothing heard from a player
  std::uint64_t ping_interval_us = 250'000;
  // A refused connection the transport has not closed this long after the
  // REFUSED is dropped again, however much it still sends.
  std::uint64_t refused_grace_us = 10'000'000;
  // Most messages a player may send a second, with bursts of up to twice
  // that; the rest are dropped and counted, so one player cannot flood the
  // others through the relay. Reliable is channels 0 to 2 (PING included),
  // unreliable 3 and 4. The protocol's own rates (60 samples and a few other
  // messages a second) stay well under them.
  std::uint32_t reliable_per_second = 60;
  std::uint32_t unreliable_per_second = 240;
  // Most connections one address may hold that have not sent HELLO yet, so
  // one machine cannot fill every transport slot with silent connections.
  // One more is refused at once (see connected). 0: no limit.
  std::size_t max_half_open_per_address = 4;
  // Called with one line for each join, leave and refusal. May be empty.
  std::function<void(const std::string&)> log;
};

struct SendAction {
  ConnectionId to = 0;
  std::uint8_t channel = 0;
  std::vector<std::uint8_t> bytes;
};

// Close the connection once what was queued for it has gone out.
struct DisconnectAction {
  ConnectionId who = 0;
};

using Action = std::variant<SendAction, DisconnectAction>;

// What the session refused or ignored, so a bad client shows in the numbers
// instead of in a crash.
struct Stats {
  // Packets refused by decode or check_received, by error. unknown_type is
  // here too: such a packet is ignored, as the protocol says.
  std::array<std::uint64_t, static_cast<std::size_t>(protocol::Error::wrong_sender) + 1>
      by_error{};
  std::uint64_t before_welcome = 0;  // anything but HELLO before WELCOME
  std::uint64_t repeated_hello = 0;  // HELLO from a player already in
  std::uint64_t after_refused = 0;   // packets from a connection being closed
  std::uint64_t rate_limited = 0;    // over a player's message rate
  std::uint64_t relayed = 0;         // messages passed on (once per message)
  // Connections refused at connect: their address already had
  // max_half_open_per_address waiting for HELLO. Not packets, so not in
  // dropped().
  std::uint64_t half_open_limited = 0;

  std::uint64_t errors(protocol::Error e) const {
    return by_error[static_cast<std::size_t>(e)];
  }
  // Everything refused or ignored.
  std::uint64_t dropped() const;
};

class Session {
 public:
  explicit Session(SessionConfig config = {});

  // A new connection from `address` (the transport's own number for the
  // peer's IP; none: not counted against any address). It has
  // hello_timeout_us to send HELLO. False when the address already has
  // max_half_open_per_address connections waiting for HELLO: the session
  // then keeps nothing of it, queues nothing for it, and the transport must
  // close it at once and not report it again.
  bool connected(ConnectionId id, std::uint64_t now_us,
                 std::optional<std::uint32_t> address = std::nullopt);

  // One packet from a connection on a channel.
  void received(ConnectionId id, std::uint8_t channel,
                std::span<const std::uint8_t> bytes, std::uint64_t now_us);

  // The transport lost the connection. A player who was in is reported to
  // the others as left, or as timed out when nothing was heard from them for
  // half the silence timeout or more (ENet does not say which it was).
  void disconnected(ConnectionId id, std::uint64_t now_us);

  // Timeouts and the server's own PINGs. Call often (every few ms).
  void tick(std::uint64_t now_us);

  // What the transport must do, in order. Clears the list.
  std::vector<Action> take_actions();

  const Stats& stats() const { return stats_; }
  std::size_t player_count() const;
  // Connections from `address` still waiting for HELLO.
  std::size_t half_open_count(std::uint32_t address) const;
  std::optional<std::uint8_t> player_number(ConnectionId id) const;

 private:
  enum class State : std::uint8_t { awaiting_hello, joined, closing };

  struct Connection {
    State state = State::awaiting_hello;
    std::uint64_t connected_at = 0;
    std::uint64_t last_heard = 0;
    std::uint64_t last_ping = 0;
    std::uint64_t refused_at = 0;
    // Message budgets for the rate limits, in messages, refilled with time.
    double reliable_budget = 0;
    double unreliable_budget = 0;
    std::uint64_t budget_time = 0;
    std::optional<std::uint32_t> address;
    std::uint8_t number = 0;  // 1 to 4 once joined
    std::string nickname;
  };

  void on_hello(ConnectionId id, Connection& c, const protocol::Hello& hello,
                std::uint64_t now_us);
  void on_player_message(ConnectionId id, Connection& c, std::uint8_t channel,
                         protocol::Message& message, std::uint64_t now_us);
  bool within_rate(Connection& c, std::uint8_t channel, std::uint64_t now_us);
  void refuse(ConnectionId id, Connection& c, protocol::RefuseReason reason,
              std::uint64_t now_us);
  void remove_player(ConnectionId id, protocol::LeaveReason reason);
  bool name_in_use(const std::string& nickname) const;
  std::uint8_t free_number() const;

  void send(ConnectionId to, std::uint8_t channel, const protocol::Message& m);
  void send_to_others(ConnectionId except, std::uint8_t channel,
                      const protocol::Message& m);
  void log(const std::string& line) const;

  SessionConfig config_;
  std::string session_game_build_;  // set while players are in
  std::map<ConnectionId, Connection> connections_;
  std::vector<Action> actions_;
  Stats stats_;
};

}  // namespace coop::server
