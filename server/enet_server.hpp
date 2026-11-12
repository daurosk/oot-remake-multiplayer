#pragma once

// The thin ENet loop around a Session: turns ENet events into Session calls
// and the Session's actions into ENet sends and disconnects. All the rules
// are in session.hpp.
//
// The caller starts ENet (enet_initialize) before start() and stops it after
// the server is gone.

#include <chrono>
#include <cstdint>
#include <map>
#include <set>

#include <enet/enet.h>

#include "session.hpp"

namespace coop::server {

struct ServerOptions {
  std::uint16_t port = protocol::kDefaultPort;  // 0: any free port
  // More than 4 so a fifth player can connect and be told the server is full.
  std::size_t max_connections = 16;
  // Listen on this IPv4 address only (a number, first dotted part highest);
  // none: on every adapter.
  std::optional<std::uint32_t> bind;
  SessionConfig session;
};

// ENet flags for a packet on `channel`: 0 to 2 reliable, 3 and 4 unreliable
// and sequenced. One longer than a datagram on 3 or 4 goes as unreliable
// fragments, not reliably (ENet's default), so a lost fragment on a lossy
// VPN cannot hold the channel until the timeout (issue 143).
std::uint32_t packet_flags(std::uint8_t channel);

class EnetServer {
 public:
  explicit EnetServer(ServerOptions options);
  ~EnetServer();
  EnetServer(const EnetServer&) = delete;
  EnetServer& operator=(const EnetServer&) = delete;

  // Listens on all addresses at the port. False when ENet cannot bind it.
  bool start();

  // The port actually listened on (useful when the options asked for 0).
  std::uint16_t port() const;

  // One round: waits up to timeout_ms for network events, handles every one
  // that is ready, runs the session's tick and sends what it queued.
  void service(std::uint32_t timeout_ms);

  const Session& session() const { return session_; }

  // Bytes ENet had counted for players' lost fragments and we took off again
  // (coop/net/enet_waiting.hpp): above 0 means the line loses packets.
  std::uint64_t mended_bytes() const { return mended_bytes_; }

  // Players sent away to connect again with small datagrams because their
  // path lost full ones (coop/net/enet_path.hpp, issue 143), and the
  // addresses remembered for that.
  std::uint64_t small_path_drops() const { return small_path_drops_; }
  bool small_path(enet_uint32 host) const { return small_hosts_.count(host) != 0; }

 private:
  std::uint64_t now_us() const;
  void handle(const ENetEvent& event);
  void apply_actions();
  // Ends `peer`'s connection with kDisconnectSmallDatagrams (no DISCONNECT
  // event follows) and tells the session; `why` goes into its drop line.
  void send_small(ENetPeer* peer, const std::string& why);

  ServerOptions options_;
  Session session_;
  ENetHost* host_ = nullptr;
  std::map<ConnectionId, ENetPeer*> peers_;
  // Each peer's link as of the last round, for its drop line: ENet resets a
  // peer (round trip back to 500 ms, waiting to 0) before it reports the drop.
  struct LinkSeen {
    enet_uint32 round_trip_ms = 0;
    enet_uint32 loss_percent = 0;
    std::size_t waiting_bytes = 0;
  };
  std::map<ConnectionId, LinkSeen> links_;
  ConnectionId next_id_ = 1;
  std::uint64_t mended_bytes_ = 0;
  std::uint64_t small_path_drops_ = 0;
  std::set<enet_uint32> small_hosts_;  // ENet's form of the address
  std::chrono::steady_clock::time_point start_;
};

}  // namespace coop::server
