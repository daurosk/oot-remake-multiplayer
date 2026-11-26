#pragma once

// coop_server --relay-via HOST[:PORT]: the host's side of a relay
// (relay.hpp). It connects out to the relay, gets a join code and keeps it
// with a keepalive every 5 seconds (which also keeps the host's router
// letting the relay's answers in). Each guest the relay forwards gets its own
// UDP socket on 127.0.0.1, so to this PC's server every guest is a separate
// player at 127.0.0.1.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <enet/enet.h>

#include "relay_wire.hpp"

namespace coop::server::relay {

struct RelayLinkConfig {
  std::string relay_host;  // a name or an IPv4 address
  std::uint16_t relay_port = kDefaultRelayPort;
  std::uint16_t server_port = 0;  // this PC's coop_server
  std::string key;                // the relay's host key, if it wants one
  // A room name to hold besides the join code (3 to 24 letters, digits, '-'
  // and '_'); empty for a join code only.
  std::string room;
  std::size_t max_flows = 32;
  // The one IPv4 address of this PC (host order) the link's sockets bind to; none: all addresses (tests: 127.0.0.1).
  std::optional<std::uint32_t> bind;
  std::function<void(const std::string&)> log;
  // Called when the relay gives a join code (again after a relay restart).
  std::function<void(const std::string& code)> on_code;
  // Called when the relay says the room name is this host's (again after a
  // relay restart).
  std::function<void(const std::string& room)> on_room;
  // Set: the room goes on the relay's public room list (kListRoom, with each
  // keepalive) with what this gives (host name, players, most players,
  // protocol version; its room field is not used). Nothing from it takes the
  // room off the list. Needs a room name; never sends the join password.
  std::function<std::optional<RoomEntry>()> listing;
  // Set (the official game, --official-key-file): a proof for the room as listed, sent right after each listing
  // (kRoomProof). Given the room name and the listing as sent.
  std::function<std::optional<RoomProof>(const std::string& room, const RoomEntry& listed)> proof;
  // The session token; std::random_device when empty.
  std::function<std::uint64_t()> random;
};

// "relay.example.net:27530" or "1.2.3.4" (default port) as host and port;
// false when it is not one.
bool parse_relay_address(const std::string& text, std::string& host, std::uint16_t& port);

// A join through a relay as a player types it: "ABCD-EFGH@relay[:port]" (a
// join code) or "forest-gang@relay[:port]" (a room name, issue 261). A text
// that can be both (8 code letters, with or without a dash: "lake-gang") has
// both, and the code is tried first. The same rules as Join.bat's
// relay-join.ps1.
struct RelayJoin {
  std::string code;  // kCodeLength letters, or empty
  std::string room;  // as typed, or empty
  std::string relay_host;
  std::uint16_t relay_port = kDefaultRelayPort;
};
std::optional<RelayJoin> parse_relay_join(std::string_view text);

// Asks the relay at `relay` (ENet's form) to let this PC's internet address in
// for the host with that code or room: LOOKUP for the code, then LOOKUP_NAME
// for the room when the code was unknown. Three tries a second apart for each,
// from a socket of its own (the relay lets in the address, not the port).
// Nothing when the relay never answered (not running, wrong address, or a
// relay from before room names asked for a room). `stop` returning true ends
// the wait early.
// `bind`: the one IPv4 address of this PC (host order) the socket binds to; none: all addresses (tests: 127.0.0.1).
std::optional<LookupResult> lookup_on_relay(const ENetAddress& relay, const RelayJoin& join,
                                            const std::function<bool()>& stop = {},
                                            std::optional<std::uint32_t> bind = std::nullopt);

// What to tell the player for a lookup's result, in plain English.
std::string lookup_text(std::optional<LookupResult> result, const RelayJoin& join);

// The relay's public room list (kListRooms): the rooms its hosts listed,
// newest first, never a password. `total` is how many the relay has; `rooms`
// may hold fewer when a page did not come.
struct RoomList {
  std::vector<RoomEntry> rooms;
  std::size_t total = 0;
  // The listed rooms' proofs (kListProofs, asked once after the pages): empty from a relay before 0.11.8, which
  // does not answer (a short wait, then the list as it is).
  std::vector<RoomProof> proofs;
  bool proofs_answered = false;
};
// Asks the relay at `relay` (ENet's form) for its room list, a page at a time
// (about 20 rooms a page; three tries a second apart for each, from a socket
// of its own). Nothing when the relay never answered the first page (not
// running, wrong address, a relay from before the list, or this address asked
// more than 30 pages in the last minute). `stop` returning true ends the wait
// early. Blocks for up to about 3 s a page: call it off the game's threads.
// `ask_proofs`: also ask the listed rooms' proofs (kListProofs) after the pages, two tries 500 ms apart;
// proofs_answered says whether the relay answered (a relay before 0.11.8 never does: the caller need not ask it again).
// `bind`: as for lookup_on_relay.
std::optional<RoomList> list_rooms_on_relay(const ENetAddress& relay, const std::function<bool()>& stop = {},
                                            bool ask_proofs = false, std::optional<std::uint32_t> bind = std::nullopt);

class RelayLink {
 public:
  explicit RelayLink(RelayLinkConfig config);
  ~RelayLink();
  RelayLink(const RelayLink&) = delete;
  RelayLink& operator=(const RelayLink&) = delete;

  // Looks the relay up and opens the socket. False (and why) when it cannot.
  bool start(std::string& error);
  // Handles what arrived (does not wait) and sends what is due.
  void service();
  // Tells the relay the host stopped, so the code ends at once.
  void stop();

  // The join code while the relay has given one; empty before that.
  const std::string& code() const { return code_; }
  // The room name while the relay holds it for this host; empty before that.
  const std::string& room() const { return room_; }
  std::size_t flows() const { return flows_.size(); }

 private:
  struct Flow {
    ENetSocket socket = ENET_SOCKET_NULL;
    std::uint64_t last_ms = 0;
  };

  std::uint64_t now_ms() const;
  void send_relay(const Message& m);
  void handle(const Message& m, std::uint64_t now);
  void close_flow(std::uint16_t flow, bool tell_relay);
  void say(const std::string& text) const;

  RelayLinkConfig config_;
  ENetSocket socket_ = ENET_SOCKET_NULL;
  ENetAddress relay_{};
  ENetAddress server_{};
  std::uint64_t token_ = 0;
  std::string code_;
  std::string room_;
  bool registered_ = false;
  bool refused_hard_ = false;
  std::uint64_t started_ms_ = 0;
  std::uint64_t last_sent_ms_ = 0;
  std::uint64_t last_register_ms_ = 0;
  std::uint64_t last_keepalive_ms_ = 0;
  std::uint64_t last_heard_ms_ = 0;
  std::uint64_t first_try_ms_ = 0;
  std::uint64_t last_waiting_note_ms_ = 0;
  std::uint64_t last_list_ms_ = 0;  // the last kListRoom (0: send one at the next service)
  std::map<std::uint16_t, Flow> flows_;
};

}  // namespace coop::server::relay
