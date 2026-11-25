#pragma once

// coop_server --relay: forwards a host's game to guests who cannot reach it
// directly (no VPN, no port forwarding, CGNAT). docs/play-online.md, "Relay
// with join codes".
//
// - The host's coop_server (--relay-via, relay_link.hpp) connects OUT to the
//   relay and gets a join code. Nothing on the host's router changes.
// - It may also hold a room name (--relay-room, kRegisterName): the name is
//   the session token's until that host stops or goes quiet, like its code.
// - A guest's Join.bat sends the code once (kLookup), or the room name
//   (kLookupName). That lets the guest's address send to that host for 30
//   minutes after its last datagram. Wrong codes and names count together.
// - The guest's game then talks plain ENet to the relay's port; the relay
//   wraps each datagram for the host's link and unwraps the answers. It never
//   reads them: the join password and the server's own checks still apply.
// - A host with a room name may list it (kListRoom, with each keepalive): its
//   host name, players, most players and protocol version, never the
//   password. Anyone may ask for the list (kListRooms, a page at a time,
//   newest first); a room leaves it when its host stops, goes quiet or
//   unlists it. One address gets 30 pages a minute, sends 30 LIST_ROOM a
//   minute and puts 6 rooms on the list in ten minutes; more are ignored.
// - The direct way (kDirect): a guest's answer code goes to the host's link
//   with a ticket; the host's kDirectReply (its address, only when the code
//   was made with the join password) goes back to that guest. One address
//   sends 20 kDirect in ten minutes (wrong rooms and codes count with the
//   lookups'); a host has at most 8 answers waiting, 10 seconds each.
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
  std::uint32_t lists_per_window = 30;        // room list pages asked from one address
  // LIST_ROOM from one address (a host's link sends one each 5 seconds: 24 a
  // minute for the 2 hosts an address may have); more are not taken.
  std::uint32_t listings_per_window = 30;
  std::uint64_t list_window_ms = 60'000;      // (for both)
  // Rooms put on the list from one address in window_ms, each listing that was
  // off the list counted (as registers_per_window counts new hosts): a room
  // taken off and listed again would go to the top of the list each time.
  std::uint32_t new_listings_per_window = 6;
  std::uint32_t directs_per_window = 20;      // kDirect from one address in window_ms
  std::uint64_t direct_wait_ms = 10'000;      // a passed answer code waits this long for its host's reply
  std::size_t max_directs_per_session = 8;    // answer codes waiting for one host
  std::function<void(const std::string&)> log;
  // Random numbers for join codes; std::random_device when empty.
  std::function<std::uint64_t()> random;
};

// --relay-max-per-address N (test relays; the default 2 stays for public ones):
// N sessions from one address, and the per-address rate limits for new hosts, new
// listings and LIST_ROOM raised to match so N hosts can register and list at once.
inline void allow_sessions_per_address(RelayConfig& c, std::size_t n) {
  c.max_sessions_per_address = n;
  if (c.max_sessions < n) c.max_sessions = n;
  const auto count = static_cast<std::uint32_t>(n);
  if (c.registers_per_window < count) c.registers_per_window = count;
  if (c.new_listings_per_window < count) c.new_listings_per_window = count;
  if (c.listings_per_window < count * 12) c.listings_per_window = count * 12;
  if (c.lists_per_window < count * 12) c.lists_per_window = count * 12;
}

struct RelayStats {
  std::uint64_t to_host = 0;    // guest datagrams forwarded
  std::uint64_t to_guest = 0;   // host datagrams forwarded
  std::uint64_t not_let_in = 0; // datagrams from addresses without a code
  std::uint64_t lookups_ok = 0;
  std::uint64_t lookups_wrong = 0;
  std::uint64_t lookups_limited = 0;
  std::uint64_t lookups_full = 0;   // a right code or room whose host had max_guest_addresses guests already
  std::uint64_t hosts_registered = 0;
  std::uint64_t hosts_refused = 0;
  std::uint64_t rooms_taken = 0;  // a room name another host already had
  std::uint64_t lists_answered = 0;  // room list pages sent
  std::uint64_t lists_limited = 0;   // room list asks over the limit (not answered)
  std::uint64_t proofs_kept = 0;     // kRoomProof taken for a held room
  std::uint64_t proofs_answered = 0; // kRoomProofs sent
  std::uint64_t listings_limited = 0;  // LIST_ROOM over a limit (not taken)
  std::uint64_t directs_passed = 0;    // answer codes passed to a host's link
  std::uint64_t directs_answered = 0;  // host replies passed back to the guest
  std::uint64_t directs_limited = 0;   // kDirect over a limit (not passed)
};

class Relay {
 public:
  explicit Relay(RelayConfig config);

  std::vector<Outgoing> received(Endpoint from, std::span<const std::uint8_t> bytes,
                                 std::uint64_t now_ms);
  // Drops hosts, guests and flows that went quiet. Call about once a second.
  std::vector<Outgoing> tick(std::uint64_t now_ms);

  std::size_t sessions() const { return sessions_.size(); }
  std::size_t rooms() const { return by_room_.size(); }
  // The rooms on the list now (listed by their host, with a room name).
  std::size_t listed() const;
  // The list as kListRooms pages it: newest listed first, at most kMaxListedRooms.
  std::vector<RoomEntry> room_list() const;
  std::size_t flows() const { return flows_by_guest_.size(); }
  std::size_t directs_waiting() const { return directs_.size(); }
  const RelayStats& stats() const { return stats_; }

 private:
  struct Flow {
    Endpoint guest;
    std::uint64_t last_ms = 0;
  };
  struct Session {
    std::uint64_t token = 0;
    std::string code;
    std::string room;  // the room name as the host gave it; empty: a join code only
    bool listed = false;             // its host lists the room (kListRoom)
    RoomEntry listing;               // what it lists (room: filled from `room`)
    std::uint64_t listed_since = 0;  // when it was first listed (newest first)
    std::optional<RoomProof> proof;  // its host's latest kRoomProof for `room` (passed on, never checked here)
    Endpoint link;
    std::uint64_t last_ms = 0;
    std::map<std::uint16_t, Flow> flows;
    std::uint16_t next_flow = 1;
    std::map<std::uint32_t, std::uint64_t> guests;  // address -> last datagram
  };
  struct Direct {  // an answer code passed to a host, waiting for its kDirectReply
    Endpoint guest;
    std::uint64_t token = 0;
    std::uint64_t since_ms = 0;
  };
  struct Tries {
    std::uint64_t window_start = 0;
    std::uint32_t count = 0;
    bool said = false;   // (lookups: "over the limit" said once this window)
  };

  void control(Endpoint from, const Message& m, std::uint64_t now_ms, std::vector<Outgoing>& out);
  // This token's session (a new one when it has none yet), at this address and
  // time; sessions_.end() when the host is refused (the refusal is in out).
  std::map<std::uint64_t, Session>::iterator open_session(Endpoint from, const Message& m, std::uint64_t now_ms,
                                                          std::vector<Outgoing>& out);
  // kLookup and kLookupName: lets the guest's address in for the session
  // (token), or says why not.
  LookupResult let_in(Endpoint from, std::optional<std::uint64_t> token, const std::string& what,
                      std::uint64_t now_ms);
  void from_guest(Endpoint from, std::span<const std::uint8_t> bytes, std::uint64_t now_ms,
                  std::vector<Outgoing>& out);
  void remove_session(std::map<std::uint64_t, Session>::iterator it, const char* why);
  void remove_flow(Session& s, std::uint16_t flow);
  bool over_limit(std::map<std::uint32_t, Tries>& tries, std::uint32_t address, std::uint32_t limit,
                  std::uint64_t now_ms, bool count, std::uint64_t window_ms = 0);
  // kLookup, kLookupName and kDirect: the session `m` names (its code, or its room name), and what it was in words.
  std::optional<std::uint64_t> target(const Message& m, std::string& what) const;
  // kDirect: passes the answer code to the host's link, or answers the guest why not.
  void direct(Endpoint from, const Message& m, std::uint64_t now_ms, std::vector<Outgoing>& out);
  // kListRooms: the page from `start` that fits a datagram.
  Message list_page(std::uint8_t start) const;
  // kRoomProofs: the listed rooms' proofs, newest listing first, as many as fit in a datagram.
  Message proofs_page() const;
  std::string new_code();
  void say(const std::string& text) const;

  RelayConfig config_;
  RelayStats stats_;
  std::map<std::uint64_t, Session> sessions_;  // by token
  std::map<std::string, std::uint64_t> by_code_;
  std::map<std::string, std::uint64_t> by_room_;  // room_key -> token
  std::map<Endpoint, std::pair<std::uint64_t, std::uint16_t>> flows_by_guest_;  // token, flow
  std::map<std::uint32_t, std::uint64_t> guest_session_;  // address -> token (last code wins)
  std::map<std::uint32_t, Tries> lookups_;
  std::map<std::uint32_t, Tries> registers_;
  std::map<std::uint32_t, Tries> lists_;
  std::map<std::uint32_t, Tries> listings_;
  std::map<std::uint32_t, Tries> new_listings_;
  std::map<std::uint32_t, Tries> directs_by_address_;
  std::map<std::uint32_t, Direct> directs_;  // by ticket
  std::uint32_t next_ticket_ = 0;
  std::uint64_t list_order_ = 0;  // a number that goes up with each new listing (newest first)
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
