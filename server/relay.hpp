#pragma once

// coop_server --relay: forwards a host's game to guests who cannot reach it
// directly (no VPN, no port forwarding, CGNAT). docs/play-online.md, "Relay
// with join codes".
//
// - The host's coop_server (--relay-via, relay_link.hpp) connects OUT to the
//   relay and gets a join code. Nothing on the host's router changes.
// - A guest's Join.bat sends the code once (kLookup). That lets the guest's
//   address send to that host for 30 minutes after its last datagram.
// - The guest's game then talks plain ENet to the relay's port; the relay
//   wraps each datagram for the host's link and unwraps the answers. It never
//   reads them: the join password and the server's own checks still apply.
//
// Relay is the rules without a network (tests drive it); RelayServer is the
// socket loop around it.

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "relay_wire.hpp"

namespace coop::server::relay {

struct Endpoint {
  std::uint32_t host = 0;  // as ENet keeps it (network byte order)
  std::uint16_t port = 0;
  auto operator<=>(const Endpoint&) const = default;
};

struct Outgoing {
  Endpoint to;
  std::vector<std::uint8_t> bytes;
};

struct RelayConfig {
  std::size_t max_sessions = 64;
  std::size_t max_sessions_per_address = 2;
  std::size_t max_flows_per_session = 32;
  std::size_t max_guest_addresses = 16;
  std::string host_key;  // non-empty: hosts must give it to register
  std::uint64_t session_timeout_ms = 30'000;  // no word from the host's link
  std::uint64_t flow_idle_ms = 60'000;        // no datagram from that guest port
  std::uint64_t guest_idle_ms = 30 * 60'000;  // an address let in by a code
  std::uint32_t lookups_per_window = 10;      // wrong codes from one address
  std::uint32_t registers_per_window = 6;     // new hosts from one address
  std::uint64_t window_ms = 10 * 60'000;
  std::function<void(const std::string&)> log;
  // Random numbers for join codes; std::random_device when empty.
  std::function<std::uint64_t()> random;
};

struct RelayStats {
  std::uint64_t to_host = 0;    // guest datagrams forwarded
  std::uint64_t to_guest = 0;   // host datagrams forwarded
  std::uint64_t not_let_in = 0; // datagrams from addresses without a code
  std::uint64_t lookups_ok = 0;
  std::uint64_t lookups_wrong = 0;
  std::uint64_t lookups_limited = 0;
  std::uint64_t hosts_registered = 0;
  std::uint64_t hosts_refused = 0;
};

class Relay {
 public:
  explicit Relay(RelayConfig config);

  std::vector<Outgoing> received(Endpoint from, std::span<const std::uint8_t> bytes,
                                 std::uint64_t now_ms);
  // Drops hosts, guests and flows that went quiet. Call about once a second.
  std::vector<Outgoing> tick(std::uint64_t now_ms);

  std::size_t sessions() const { return sessions_.size(); }
  std::size_t flows() const { return flows_by_guest_.size(); }
  const RelayStats& stats() const { return stats_; }

 private:
  struct Flow {
    Endpoint guest;
    std::uint64_t last_ms = 0;
  };
  struct Session {
    std::uint64_t token = 0;
    std::string code;
    Endpoint link;
    std::uint64_t last_ms = 0;
    std::map<std::uint16_t, Flow> flows;
    std::uint16_t next_flow = 1;
    std::map<std::uint32_t, std::uint64_t> guests;  // address -> last datagram
  };
  struct Tries {
    std::uint64_t window_start = 0;
    std::uint32_t count = 0;
  };

  void control(Endpoint from, const Message& m, std::uint64_t now_ms, std::vector<Outgoing>& out);
  void from_guest(Endpoint from, std::span<const std::uint8_t> bytes, std::uint64_t now_ms,
                  std::vector<Outgoing>& out);
  void remove_session(std::map<std::uint64_t, Session>::iterator it, const char* why);
  void remove_flow(Session& s, std::uint16_t flow);
  bool over_limit(std::map<std::uint32_t, Tries>& tries, std::uint32_t address, std::uint32_t limit,
                  std::uint64_t now_ms, bool count);
  std::string new_code();
  void say(const std::string& text) const;

  RelayConfig config_;
  RelayStats stats_;
  std::map<std::uint64_t, Session> sessions_;  // by token
  std::map<std::string, std::uint64_t> by_code_;
  std::map<Endpoint, std::pair<std::uint64_t, std::uint16_t>> flows_by_guest_;  // token, flow
  std::map<std::uint32_t, std::uint64_t> guest_session_;  // address -> token (last code wins)
  std::map<std::uint32_t, Tries> lookups_;
  std::map<std::uint32_t, Tries> registers_;
};

// The relay on a UDP port.
class RelayServer {
 public:
  RelayServer(std::uint16_t port, std::optional<std::uint32_t> bind, RelayConfig config);
  ~RelayServer();
  RelayServer(const RelayServer&) = delete;
  RelayServer& operator=(const RelayServer&) = delete;

  bool start();
  std::uint16_t port() const;
  // Waits up to timeout_ms for datagrams, handles every one that is ready.
  void service(std::uint32_t timeout_ms);
  const Relay& relay() const { return relay_; }

 private:
  void send(const Outgoing& out);
  std::uint64_t now_ms() const;

  std::uint16_t port_;
  std::optional<std::uint32_t> bind_;
  Relay relay_;
  ENetSocket socket_ = ENET_SOCKET_NULL;
  std::uint64_t last_tick_ms_ = 0;
  std::uint64_t start_ms_ = 0;
};

// "1.2.3.4" for an ENet host number.
std::string address_text(std::uint32_t enet_host);

}  // namespace coop::server::relay
