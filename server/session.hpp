#pragma once

// The relay server's rules, with no sockets in them. The ENet loop
// (enet_server.hpp) feeds a Session the connections, packets and clock, and
// carries out what it asks for (send these bytes, drop that connection).
// That keeps every rule here testable without a network.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop/protocol/messages.hpp"
#include "world.hpp"

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
  // Most keys (named and key64) the server's flag table holds, so a host cannot
  // grow it without end. Values past it are still relayed but not kept (and
  // so not in a joiner's snapshot); each such entry is counted in
  // flag_table_full. 262,144 is 73 snapshot parts, far past OoT's flags.
  // 0 keeps nothing; more than 65535 parts' worth counts as 65535 parts.
  std::size_t max_flags = 262'144;
  // A HELLO whose name a player already holds takes that player's place
  // (rejoin after a crash) only when it comes from the same address and the
  // old player has sent nothing for this long; otherwise it is refused as
  // name taken. The server pings every 250 ms and a live build answers, so a
  // live player is never this quiet. 0: never take over.
  std::uint64_t rejoin_stale_us = 1'000'000;
  // Most shared objects (enemies, projectiles) the server keeps track of.
  // Objects past it are not owned by anyone and nothing about them is
  // relayed; each one turned away is counted in object_table_full.
  std::size_t max_objects = 4096;
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
  // HELLOs that took over a stale player of the same name (see
  // rejoin_stale_us).
  std::uint64_t rejoined = 0;

  // JOIN_PARTNER (stage 1 step 3). Relayed ones count in relayed too.
  std::uint64_t join_relayed = 0;
  // Requests answered by the server itself with refuse / not_there.
  std::uint64_t join_not_there = 0;
  // Dropped: naming the sender itself, or an accept or refuse for a player
  // who is not in the session.
  std::uint64_t join_dropped = 0;
  // FLAG_SET and FLAG_STATE relayed (stage 1 step 4); in relayed too.
  std::uint64_t flag_relayed = 0;
  std::uint64_t flag_refused = 0;     // FLAG_STATE or FLAG_SNAPSHOT not from the host
  std::uint64_t snapshot_parts = 0;   // host snapshot parts merged in (not relayed)
  std::uint64_t flag_clashes = 0;     // host snapshot entries that differed (server kept)
  std::uint64_t flag_table_full = 0;  // entries not kept: max_flags reached
  // Objects (stage 5). Relayed ones count in relayed too.
  std::uint64_t object_relayed = 0;  // SPAWN, SAMPLE, REMOVE passed on
  // SPAWN, SAMPLE, REMOVE or HANDOVER_STATE not from the owner of the
  // object's current epoch, or for an object the server does not know.
  std::uint64_t object_refused = 0;
  std::uint64_t owner_changes = 0;      // owner set or moved (not first owner)
  std::uint64_t damage_relayed = 0;     // DAMAGE passed on to an owner
  std::uint64_t damage_dropped = 0;     // DAMAGE with no owner, or from it
  // Field grass (docs/world.md). Relayed ones count in relayed too.
  std::uint64_t grass_relayed = 0;   // GRASS_CUT kept and passed on to its scene
  std::uint64_t grass_repeated = 0;  // GRASS_CUT the scene already has: not passed on
  std::uint64_t grass_replayed = 0;  // kept cuts sent to a player who entered the scene
  // Shared world objects (docs/world.md). Relayed ones count in relayed too.
  std::uint64_t world_relayed = 0;   // WORLD_BREAK, WORLD_MOVE, WORLD_PLACE passed on
  std::uint64_t world_repeated = 0;  // WORLD_BREAK the scene already has: not passed on
  std::uint64_t world_replayed = 0;  // kept breaks and places sent to a player who entered
  std::uint64_t object_table_full = 0;  // objects turned away: max_objects

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
  // The host's player number; none while the session is empty.
  std::optional<std::uint8_t> host() const;
  // Flags in the server's table.
  std::size_t flag_count() const { return flags_.size(); }
  // Connections from `address` still waiting for HELLO.
  std::size_t half_open_count(std::uint32_t address) const;
  std::optional<std::uint8_t> player_number(ConnectionId id) const;
  // Shared objects the server keeps track of.
  std::size_t object_count() const { return objects_.size(); }
  // An object's owner (0: nobody) and epoch; none when it is not known.
  std::optional<std::pair<std::uint8_t, std::uint16_t>> object_owner(
      const protocol::ObjectId& id) const;

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
    // The scene the player is in, from its latest OBJECT_PRESENT or
    // PLAYER_SAMPLE scene; empty while not known.
    std::string scene;
    // The placed objects it has loaded in that scene. Its own made objects
    // are not listed here: they are found by owner.
    std::set<protocol::ObjectId> present;
  };

  // One shared object (docs/objects.md "Server").
  struct Object {
    std::string scene;
    std::set<ConnectionId> present;     // games that have it loaded
    std::optional<ConnectionId> owner;  // none: nobody, or removed
    std::uint16_t epoch = 0;
    bool made = false;     // made by a player (OBJECT_SPAWN): never handed over
    bool removed = false;  // its owner removed it; kept until nobody has it
    std::uint8_t last_owner = 0;  // the player who owned it before, 0: none
    // The last state relayed from its owner, for a hand-over with no answer.
    std::optional<protocol::ObjectState> last_state;
  };

  void on_hello(ConnectionId id, Connection& c, const protocol::Hello& hello,
                std::uint64_t now_us);
  void on_player_message(ConnectionId id, Connection& c, std::uint8_t channel,
                         protocol::Message& message, std::uint64_t now_us);
  void on_join_partner(ConnectionId id, Connection& c, protocol::JoinPartner& j);
  void on_flag_set(Connection& c, protocol::Message& message);
  void on_flag_state(ConnectionId id, Connection& c, protocol::Message& message);
  void on_flag_snapshot(ConnectionId id, Connection& c, const protocol::FlagSnapshot& part);
  // Keeps an entry in the table; false when it is new and the table is full.
  bool keep_flag(const protocol::FlagEntry& e);
  void send_snapshot(ConnectionId to);
  void on_object_present(ConnectionId id, Connection& c, const protocol::ObjectPresent& p);
  void on_object_message(ConnectionId id, Connection& c, std::uint8_t channel,
                         protocol::Message& message);
  void on_damage(ConnectionId id, Connection& c, protocol::Damage& d);
  void on_grass_cut(ConnectionId id, Connection& c, protocol::GrassCut& cut);
  // Forgets the cuts of a scene no joined player is in: the grass grows back
  // in a game that loads the scene again, so the cuts end with the last game
  // that still shows them.
  void forget_empty_grass(const std::string& scene);
  // WORLD_BREAK, WORLD_MOVE, WORLD_PLACE (world.hpp keeps what is kept).
  void on_world(ConnectionId id, Connection& c, std::uint8_t channel, protocol::Message& message);
  void forget_empty_world(const std::string& scene);
  // The player is in `scene` now; leaving another scene lets go of its objects.
  void set_scene(ConnectionId id, Connection& c, const std::string& scene);
  // The player no longer has the object loaded (unload, scene change, leave):
  // a made object of its own is removed, a placed one it owns handed over.
  void drop_presence(ConnectionId id, std::uint8_t number, const protocol::ObjectId& oid);
  // Lets go of everything the player has loaded or made.
  void drop_all_objects(ConnectionId id, std::uint8_t number,
                        const std::set<protocol::ObjectId>& present);
  // Gives the object to the next present player (lowest number), or nobody.
  void hand_over(const protocol::ObjectId& oid, Object& o, std::uint8_t old_owner);
  void announce_owner(const protocol::ObjectId& oid, const Object& o);
  // To every joined player in `scene` but `except`; empty scene: everyone.
  void send_to_scene(const std::string& scene, std::optional<ConnectionId> except,
                     std::uint8_t channel, const protocol::Message& m);
  std::optional<ConnectionId> connection_of(std::uint8_t number) const;
  bool within_rate(Connection& c, std::uint8_t channel, std::uint64_t now_us);
  // why: more for the server's log (which version is older), or empty.
  void refuse(ConnectionId id, Connection& c, protocol::RefuseReason reason,
              std::uint64_t now_us, const std::string& why = {});
  void remove_player(ConnectionId id, protocol::LeaveReason reason);
  std::optional<ConnectionId> holder_of(const std::string& nickname) const;
  std::uint8_t free_number(std::uint8_t preferred = 0) const;

  void send(ConnectionId to, std::uint8_t channel, const protocol::Message& m);
  void send_to_others(ConnectionId except, std::uint8_t channel,
                      const protocol::Message& m);
  void log(const std::string& line) const;

  SessionConfig config_;
  std::string session_game_build_;  // set while players are in
  std::map<ConnectionId, Connection> connections_;
  // The host's connection; set while players are in.
  std::optional<ConnectionId> host_;
  // Every shared flag value the host has sent, by key. Kept when
  // the host leaves; forgotten when the session empties.
  std::map<protocol::FlagKey, protocol::FlagEntry> flags_;
  // Shared objects, by id. Forgotten when no game has them loaded.
  std::map<protocol::ObjectId, Object> objects_;
  // The field grass cut in each scene (GRASS_CUT), oldest first, at most
  // kMaxGrassCuts a scene; sent to a player who enters the scene. Forgotten
  // when no player is in the scene.
  std::map<std::string, std::deque<protocol::GrassCut>> grass_;
  WorldStore world_;
  std::vector<Action> actions_;
  Stats stats_;
};

}  // namespace coop::server
