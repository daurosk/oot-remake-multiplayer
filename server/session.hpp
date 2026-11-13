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
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop/protocol/messages.hpp"
#include "world.hpp"

namespace coop::server {

// The transport's own number for a connection, never reused while it lives.
using ConnectionId = std::uint32_t;

// An IPv4 address or a range of them ("25.1.2.3", "25.0.0.0/8"), as numbers
// with the first dotted part highest. For --allow.
struct AddressRange {
  std::uint32_t first = 0;
  std::uint8_t prefix = 32;  // 0 to 32 leading bits that must match
  bool contains(std::uint32_t ipv4) const;
  bool operator==(const AddressRange&) const = default;
};

// "a.b.c.d" or "a.b.c.d/N"; nothing for anything else (names, IPv6).
std::optional<AddressRange> parse_address_range(std::string_view text);

// ENet's address number (the four bytes as they came, in memory order) as a
// number with the first dotted part highest.
std::uint32_t ipv4_of(std::uint32_t enet_host);

// Players a server takes unless told otherwise (coop_server --max-players).
inline constexpr std::uint8_t kDefaultMaxPlayers = 16;

struct SessionConfig {
  // The game build every player must run. Empty: the first player to join
  // decides it, and it is forgotten again when the session empties.
  std::string game_build_id;
  std::uint16_t tick_rate = 60;
  std::uint64_t hello_timeout_us = 10'000'000;    // connect to HELLO
  // The join password (coop_server --password): a player must answer the
  // JOIN_CHALLENGE with HMAC-SHA256 of it over that connection's nonce and
  // both key shares; the traffic keys come from it too. Required: empty, every
  // HELLO is refused (there is no plain way in, docs/protocol.md).
  std::string join_password;
  // Most players at once (coop_server --max-players), 1 to protocol::kMaxPlayers;
  // one more is told the server is full. 4 until the game's side (puppets,
  // HUD, tables) takes 16; the protocol already does.
  std::uint8_t max_players = kDefaultMaxPlayers;
  // Interest (16 players, docs/protocol.md "Who gets which stream"): each
  // PLAYER_SAMPLE and PLAYER_POSE goes to a player by where the two are.
  // Same scene within near_distance (game units), or either place unknown:
  // all of them. Same scene, farther: every far_pose_every-th pose (samples
  // all: they are small). Another scene: every other_scene_sample_every-th
  // sample (the player list, markers) and no poses (nothing to draw).
  float near_distance = 2000.0f;
  std::uint8_t far_pose_every = 3;
  std::uint8_t other_scene_sample_every = 6;
  // Most PLAYER_POSE bytes a second the server sends everyone together (the
  // host's upload), shared evenly by the players; past a player's share its
  // poses wait for the next one. 0: no cap.
  std::uint32_t pose_upload_bytes_per_second = 0;
  // PvP (coop_server --pvp): players hurt each other. The host may change it
  // in the session (PVP_MODE); every player gets the server's value.
  bool pvp = false;
  // Guessing the join password: an address that sends max_wrong_passwords
  // wrong proofs within wrong_password_window_us is closed at connect for
  // wrong_password_block_us (counted in password_blocked). 0: no limit.
  std::uint32_t max_wrong_passwords = 5;
  std::uint64_t wrong_password_window_us = 60'000'000;
  std::uint64_t wrong_password_block_us = 60'000'000;
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
  // Most message bytes a player may send a second, both kinds together, with
  // bursts of up to twice that. The message counts alone let a player relay
  // 300 messages of up to 64 KB a second (19 MB/s) to every other player; the
  // protocol's own traffic is a few tens of KB a second. FLAG_SNAPSHOT is not
  // counted: it goes to the server only and max_flags bounds it. The
  // unreliable channels leave a quarter second of it for the reliable ones.
  std::uint32_t bytes_per_second = 1u << 20;
  // Only addresses in these ranges may connect (coop_server --allow); empty:
  // any address. A connection from elsewhere is closed at once and counted
  // in not_allowed.
  std::vector<AddressRange> allowed;
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
  // (rejoin after a drop or crash) only when its connection showed that
  // player's rejoin token (REJOIN_CLAIM), it comes from the same address and
  // the old player has sent nothing for this long; otherwise it is refused as
  // name taken. The server pings every 250 ms and a live build answers, so a
  // live player is never this quiet. Without the token nobody takes over, so
  // two players behind one address cannot push each other out by name.
  // 0: never take over.
  std::uint64_t rejoin_stale_us = 1'000'000;
  // Most shared objects (enemies, projectiles) the server keeps track of.
  // Objects past it are not owned by anyone and nothing about them is
  // relayed; each one turned away is counted in object_table_full.
  std::size_t max_objects = 4096;

  // Enemy ownership (docs/objects.md "Enemies"). Distances in metres, between
  // a player's last PLAYER_SAMPLE position and the object's last relayed one.
  // Out of a fight an object moves to a player who is within handover_near_m
  // of it and at least handover_margin_m nearer than its owner, when the
  // owner is over handover_owner_far_m away or that player is within
  // handover_chase_m (the enemy chases the near one, and only its owner's
  // game moves it right), for handover_hold_us in a row and not within
  // handover_cooldown_us of the last change.
  float handover_near_m = 30.0f;
  float handover_margin_m = 10.0f;
  float handover_owner_far_m = 40.0f;
  float handover_chase_m = 20.0f;
  std::uint64_t handover_hold_us = 3'000'000;
  std::uint64_t handover_cooldown_us = 5'000'000;
  // Issue 101: a game that is not the owner says its copy chases its own
  // Link (OBJECT_PRESENT flag targeting). Its game is the only one whose
  // enemy moves right, so out of a fight the object goes to that player once
  // it has said so for handover_chased_us, wherever the owner's enemy
  // stands. The owner saying the same is a fight, which keeps it.
  std::uint64_t handover_chased_us = 500'000;
  // Issue 151 (fishing-3): a fish (an object some game listed in FISH_LURE)
  // goes to the game whose lure is nearest it, not by the distance rule and
  // never held by a fight lock. Its owner keeps it while the fish is hooked,
  // and while no other lure is fish_lure_margin_m nearer (or drawn when the
  // owner's is not, and is not fish_lure_margin_m farther). A better lure
  // must stay better for fish_lure_hold_us, and not within
  // fish_lure_cooldown_us of the last change. A list older than
  // fish_lure_stale_us counts as no lure; the owner's hooked fish is kept on
  // its list for fish_hooked_stale_us, and while its game stands still.
  // Only the owner's hooked counts.
  float fish_lure_margin_m = 1.0f;
  std::uint64_t fish_lure_hold_us = 300'000;
  std::uint64_t fish_lure_cooldown_us = 1'000'000;
  std::uint64_t fish_lure_stale_us = 2'500'000;
  std::uint64_t fish_hooked_stale_us = 10'000'000;
  // How long the owner has to answer HANDOVER_ASK with its state before the
  // server moves the object with the last sample it relayed.
  std::uint64_t handover_answer_us = 500'000;
  // An object is in a fight this long after its life changed, its owner
  // said it is fighting or targeting, or a DAMAGE for it was passed on.
  std::uint64_t fight_lock_us = 12'000'000;
  std::uint64_t boss_fight_lock_us = 60'000'000;
  // Issue 150: a DAMAGE on a boss (or a part of one) from a player who does
  // not own it hands the boss to that player, at most this often. The hit's
  // effect (Gohma's stun when the eye is hit) happens in the hitter's game,
  // and only the owner's game runs the boss right (the other copies follow
  // its pose), and the boss then turns on that player. 0: off.
  std::uint64_t boss_hitter_cooldown_us = 8'000'000;
  // gohma-sync (issue 150, Caleb's play-test 2026-11-12): only a stun (a
  // DAMAGE of 0: the stun is in the hitter's game alone) hands a boss over.
  // A hit that takes life is passed to the owner and the boss stays: every
  // hand-over ran the new owner's AI from a copy whose phase, eye guard and
  // height never matched the owner's, which is what the players saw. true:
  // a hit that takes life hands it over too, as before.
  bool boss_hit_handover = false;
  // ... and only when its owner has not hit it itself for this long (a life
  // drop in the owner's samples with no DAMAGE passed on just before), so
  // two players hitting in turn do not bounce it (each hand-over can jump a
  // phase: the state carries only place, turn, animation and life).
  std::uint64_t boss_owner_hit_hold_us = 6'000'000;
  // Issue 150: the boss's AI fights only its owner's Link. A fit player
  // within boss_near_m of a boss (its root) and boss_near_margin_m nearer it
  // than its owner, for boss_near_hold_us in a row, takes it while it is in
  // its fight (out of one the plain distance rule applies): then the boss
  // turns on the near one. Not while its owner hits it
  // (boss_owner_hit_hold_us) and not within boss_hitter_cooldown_us of the
  // last change. boss_near_hold_us 0: off, the default since gohma-sync
  // (issue 150: each move jumps the boss's phase; 4'000'000 was the old one).
  float boss_near_m = 15.0f;
  float boss_near_margin_m = 4.0f;
  std::uint64_t boss_near_hold_us = 0;
  // An owner paused or dead this long (or loading at all) loses its objects
  // to another player who has them, fight or not.
  std::uint64_t owner_paused_us = 6'000'000;
  std::uint64_t owner_dead_us = 10'000'000;
  // A player position older than this is not used for distances.
  std::uint64_t position_stale_us = 5'000'000;
  // How often tick runs the ownership rules.
  std::uint64_t ownership_check_us = 250'000;
  // Every this often a player's OBJECT_PRESENT is answered with OBJECT_OWNER
  // for all it lists, so a game that forgot its table (a reload of the same
  // scene) learns the owners again. 0: never.
  std::uint64_t owner_refresh_us = 10'000'000;
  // After a (re)start the games come back in whatever order they notice it,
  // and the first to list an object would own it: the others' live actors
  // that they owned before turn into copies all at once, and a game aborted
  // on that (stray, 2026-11-12). So objects first listed within this long
  // of the session's first listing get no owner yet; then each goes to the
  // lowest player number that lists it. With keep-number that gives player
  // 1 (who comes back as 1) back what it owned, which is what stray needed;
  // between other players the lowest number need not be the old owner.
  // 0: off (the first to list owns it at once).
  std::uint64_t owner_hold_us = 2'000'000;
  // A kill (OBJECT_REMOVE, dead) from the owner before the last change is
  // still taken this long after it: the kill was real.
  std::uint64_t late_remove_us = 2'000'000;
  // DAMAGE passed on to the old owner this long before a hand-over is taken
  // off the state the new owner gets, when the old owner's answer lacks it.
  std::uint64_t hit_window_us = 1'000'000;
  // A made enemy its owner no longer lists, with no OBJECT_REMOVE, is let
  // go of after this long.
  std::uint64_t orphan_us = 1'000'000;
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

// From now on, seal what goes to this connection and open what comes from
// it with these keys (docs/protocol.md "Encrypted traffic"). Comes right
// after the right JOIN_PROOF, before the answer to it.
struct EncryptAction {
  ConnectionId who = 0;
  protocol::TrafficKeys keys;
};

using Action = std::variant<SendAction, DisconnectAction, EncryptAction>;

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
  // PvP (docs/protocol.md, "PvP"). Relayed ones count in relayed too.
  std::uint64_t pvp_hits_relayed = 0;   // PVP_HIT passed on to its victim
  std::uint64_t pvp_hits_dropped = 0;   // PVP_HIT while PvP is off, or to nobody
  std::uint64_t pvp_mode_refused = 0;   // PVP_MODE from a player who does not host
  std::uint64_t difficulty_refused = 0;  // DIFFICULTY from a player who does not host
  // Connections refused at connect: their address already had
  // max_half_open_per_address waiting for HELLO. Not packets, so not in
  // dropped().
  std::uint64_t half_open_limited = 0;
  // Connections closed at connect: their address is not in config.allowed.
  // Not packets, so not in dropped().
  std::uint64_t not_allowed = 0;
  // HELLOs that took over a stale player of the same name (see
  // rejoin_stale_us).
  std::uint64_t rejoined = 0;
  // HELLOs for a quiet player's name from its address refused because the
  // connection showed no rejoin token, or a wrong one.
  std::uint64_t rejoin_no_token = 0;
  // REJOIN_CLAIMs after HELLO (a joined player's, or a second one): dropped.
  std::uint64_t stray_rejoin_claim = 0;
  // JOIN_PROOFs that did not match the join password (each one refused).
  std::uint64_t bad_password = 0;
  // JOIN_PROOFs nobody asked for: no challenge open on that connection.
  std::uint64_t stray_proof = 0;
  // A HELLO without a key share to a server with a join password (refused
  // as the wrong version: an older game, or someone stripping it).
  std::uint64_t no_key_share = 0;
  // Connections closed at connect: their address made too many wrong
  // guesses at the join password lately (max_wrong_passwords). Not packets,
  // so not in dropped().
  std::uint64_t password_blocked = 0;

  // JOIN_PARTNER (stage 1 step 3). Relayed ones count in relayed too.
  std::uint64_t join_relayed = 0;
  // Requests answered by the server itself with refuse / not_there.
  std::uint64_t join_not_there = 0;
  // Dropped: naming the sender itself, or an accept or refuse for a player
  // who is not in the session.
  std::uint64_t join_dropped = 0;
  // FLAG_SET and FLAG_STATE relayed (stage 1 step 4); in relayed too.
  std::uint64_t flag_relayed = 0;
  std::uint64_t host_resets = 0;   // FLAG_RESETs from the host (its save load)
  // The host's save (docs/protocol.md, "Save transfer"): SAVE_ASKs passed to
  // the host, SAVE_PARTs passed on (both in relayed too), and either refused
  // (an ask from the host or with no host, a part not from the host or for a
  // player not here).
  std::uint64_t save_asks = 0;
  std::uint64_t save_parts = 0;
  std::uint64_t save_refused = 0;
  std::uint64_t origins_sent = 0;  // FLAG_ORIGIN: who asked, to the others (chat notices)
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
  std::uint64_t boss_to_hitter = 0;     // a boss asked over to the player who hurt it (issue 150)
  std::uint64_t boss_to_near = 0;       // a boss asked over to the player near it (issue 150)
  std::uint64_t damage_dropped = 0;     // DAMAGE with no owner, or from it
  // Field grass (docs/world.md). Relayed ones count in relayed too.
  std::uint64_t grass_relayed = 0;   // GRASS_CUT kept and passed on to its scene
  std::uint64_t grass_repeated = 0;  // GRASS_CUT the scene already has: not passed on
  std::uint64_t grass_replayed = 0;  // kept cuts sent to a player who entered the scene
  // Shared world objects (docs/world.md). Relayed ones count in relayed too.
  std::uint64_t world_relayed = 0;   // WORLD_BREAK, WORLD_MOVE, WORLD_PLACE passed on
  std::uint64_t world_repeated = 0;  // WORLD_BREAK the scene already has: not passed on
  std::uint64_t world_replayed = 0;  // kept breaks and places sent to a player who entered
  std::uint64_t world_unkept = 0;    // GRASS_CUT or WORLD_* for a scene nobody is in: dropped
  std::uint64_t object_table_full = 0;  // objects turned away: max_objects
  std::uint64_t handover_asked = 0;       // HANDOVER_ASK sent to an owner
  std::uint64_t handover_unanswered = 0;  // moved with the last relayed sample
  std::uint64_t handover_chased = 0;      // asked because a copy chased its player
  std::uint64_t fish_to_lure = 0;         // a fish asked over to the nearest lure (issue 151)
  std::uint64_t spawns_replayed = 0;      // OBJECT_SPAWN sent again to a player entering
  std::uint64_t owners_refreshed = 0;     // OBJECT_OWNER repeated (owner_refresh_us)
  std::uint64_t owners_held = 0;          // objects given their first owner after owner_hold_us
  std::uint64_t stale_scenes = 0;
  // Interest (SessionConfig::near_distance): streamed messages not sent to a
  // player (far, another scene), poses held back by the upload cap, and the
  // stream bytes that were sent.
  std::uint64_t interest_skipped = 0;
  std::uint64_t upload_capped = 0;
  std::uint64_t stream_bytes = 0;         // a late message's old scene, not taken back
  std::uint64_t group_links = 0;          // OBJECT_GROUP entries taken
  std::uint64_t group_refused = 0;        // OBJECT_GROUP entries turned away
  std::uint64_t late_removes = 0;         // a kill from the owner before, taken
  std::uint64_t dead_told = 0;            // a placed enemy's kill told to a game that came later
  std::uint64_t dead_respawned = 0;       // a witness's game listed a killed enemy again
  std::uint64_t dead_unkept = 0;          // a kill not kept: the scene's list was full

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
  // `link` (the transport's view: round trip, loss) goes into the log line.
  void disconnected(ConnectionId id, std::uint64_t now_us, const std::string& link = {});

  // Timeouts and the server's own PINGs. Call often (every few ms).
  void tick(std::uint64_t now_us);

  // What the transport must do, in order. Clears the list.
  std::vector<Action> take_actions();

  const Stats& stats() const { return stats_; }
  // Addresses with recent wrong join passwords (max_wrong_passwords).
  std::size_t wrong_guess_addresses() const { return wrong_guesses_.size(); }
  std::size_t player_count() const;
  // The host's player number; none while the session is empty.
  std::optional<std::uint8_t> host() const;
  // Whether players hurt each other now (config.pvp, then the host's PVP_MODE).
  bool pvp() const { return pvp_; }
  // The difficulty in force now, as every player has been told (DIFFICULTY).
  protocol::Difficulty difficulty() const;
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
  // The group number an OBJECT_OWNER for it carries (0: none); none when it
  // is not known.
  std::optional<std::uint32_t> object_group(const protocol::ObjectId& id) const;
  // Whether the object is in a fight at `now_us`; false when not known.
  bool object_in_fight(const protocol::ObjectId& id, std::uint64_t now_us) const;

 private:
  // awaiting_proof: HELLO came, JOIN_CHALLENGE sent, JOIN_PROOF not yet.
  enum class State : std::uint8_t { awaiting_hello, awaiting_proof, joined, closing };

  struct Connection {
    State state = State::awaiting_hello;
    std::uint64_t connected_at = 0;
    std::uint64_t last_heard = 0;
    std::uint64_t last_ping = 0;
    std::uint64_t refused_at = 0;
    // Message budgets for the rate limits, in messages, refilled with time.
    double reliable_budget = 0;
    double unreliable_budget = 0;
    double byte_budget = 0;
    std::uint64_t budget_time = 0;
    std::optional<std::uint32_t> address;
    // While awaiting_proof: the HELLO to admit and the nonce it was sent.
    std::optional<protocol::Hello> pending_hello;
    protocol::JoinNonce nonce{};
    // While awaiting_proof: the HELLO and JOIN_CHALLENGE packets as on the
    // wire and our key pair, for the traffic keys.
    std::vector<std::uint8_t> hello_packet;
    std::vector<std::uint8_t> challenge_packet;
    std::shared_ptr<const protocol::KeyPair> key_pair;
    // Once joined: the token sent with its WELCOME (REJOIN_TOKEN). Before
    // HELLO: the token its REJOIN_CLAIM showed, if any.
    protocol::RejoinTokenBytes rejoin_token{};
    std::optional<protocol::RejoinTokenBytes> rejoin_claim;
    std::uint8_t number = 0;  // 1 to max_players once joined
    std::string nickname;
    // The scene the player is in, from its latest OBJECT_PRESENT or
    // PLAYER_SAMPLE scene; empty while not known.
    std::string scene;
    // The scene each of its two streams said last (see stale_scene).
    std::string sample_scene;
    std::string present_scene;
    // The objects it has loaded in that scene: placed ones, and made enemies
    // it listed. Projectiles are not listed here: they are found by owner.
    std::set<protocol::ObjectId> present;
    // Where its Link is, from PLAYER_SAMPLE; forgotten at a scene change.
    std::optional<protocol::Vec3> position;
    std::uint64_t position_at = 0;
    // As a receiver: how many samples and poses of each sender it was due
    // (interest counts every n-th), and its share of the pose upload cap.
    std::array<std::uint32_t, protocol::kMaxPlayers + 1> samples_due{};
    std::array<std::uint32_t, protocol::kMaxPlayers + 1> poses_due{};
    double upload_budget = 0;
    std::uint64_t upload_time = 0;
    // PLAYER_STATUS bits, and since when it is paused or dead (0: it is not).
    std::uint8_t status = 0;
    std::uint64_t paused_since = 0;
    std::uint64_t dead_since = 0;
    // It entered a scene and has not been sent that scene's spawns yet.
    bool replay_due = false;
    std::uint64_t owners_told_at = 0;
    // Its latest FISH_LURE (issue 151): each fish it has loaded and how near
    // its own lure is; lures_at 0: none yet.
    std::map<protocol::ObjectId, protocol::FishLureEntry> lures;
    std::uint64_t lures_at = 0;
  };

  // One shared object (docs/objects.md "Server").
  struct Object {
    std::string scene;
    std::set<ConnectionId> present;     // games that have it loaded
    std::optional<ConnectionId> owner;  // none: nobody, or removed
    std::uint16_t epoch = 0;
    bool made = false;        // made by a player (OBJECT_SPAWN)
    bool held = false;        // listed in the owner hold after a start: no owner yet (owner_hold_us)
    bool projectile = false;  // a made projectile: never handed over
    bool removed = false;     // its owner removed it; kept until nobody has it
    bool boss = false;        // OBJECT_GROUP said so: the long fight lock
    bool fish = false;        // FISH_LURE listed it: owned by the nearest lure
    std::uint8_t last_owner = 0;  // the player who owned it before, 0: none
    // The owner before the last change, for a kill that crossed it.
    std::optional<ConnectionId> previous_owner;
    std::uint64_t changed_at = 0;  // the last change of owner; 0: never
    // The last state relayed from its owner, for a hand-over with no answer
    // and for a player who enters the scene later.
    std::optional<protocol::ObjectState> last_state;
    // From OBJECT_SPAWN: the actor, to send the spawn again to a player who
    // enters the scene later. Empty for a placed object nobody announced.
    std::string actor;
    std::uint8_t spawn_flags = 0;
    std::uint32_t serial = 0;  // the group number when it is a group's root
    std::optional<protocol::ObjectId> root;  // the group it follows
    std::set<protocol::ObjectId> members;    // the objects that follow it
    std::uint64_t fight_until = 0;           // kept on the root for a group
    // The player the distance rule would give it to, and since when.
    std::optional<ConnectionId> candidate;
    std::uint64_t candidate_since = 0;
    // Games other than the owner whose copy chases their Link, and since when.
    std::map<ConnectionId, std::uint64_t> chased_by;
    // A HANDOVER_ASK the owner has not answered yet.
    struct Ask {
      ConnectionId to = 0;
      std::uint64_t at = 0;
    };
    std::optional<Ask> ask;
    // DAMAGE passed on lately: the life before the first, and their sum.
    std::optional<std::int32_t> hit_base;
    std::int64_t hit_sum = 0;
    std::uint64_t hit_at = 0;
    std::uint64_t owner_hit_at = 0;  // the owner's own hit (its samples' life fell); 0: none
    std::uint64_t orphan_since = 0;  // a made enemy its owner stopped listing
  };
  using Objects = std::map<protocol::ObjectId, Object>;

  void on_hello(ConnectionId id, Connection& c, const protocol::Hello& hello,
                std::uint64_t now_us);
  void on_join_proof(ConnectionId id, Connection& c, const protocol::JoinProof& proof,
                     std::uint64_t now_us);
  // The checks after the password, then WELCOME.
  void admit(ConnectionId id, Connection& c, const protocol::Hello& hello,
             std::uint64_t now_us);
  void on_player_message(ConnectionId id, Connection& c, std::uint8_t channel,
                         protocol::Message& message, std::size_t bytes, std::uint64_t now_us);
  void on_join_partner(ConnectionId id, Connection& c, protocol::JoinPartner& j);
  void on_flag_set(Connection& c, protocol::Message& message);
  void on_flag_state(ConnectionId id, Connection& c, protocol::Message& message);
  void on_flag_snapshot(ConnectionId id, Connection& c, const protocol::FlagSnapshot& part);
  // The host loaded a save (its FLAG_RESET; docs/flags.md, "Saves"). Every
  // session-wide state kept outside the flag table that the host's save
  // undoes is dropped here: add each owner's clear (locked doors' kept
  // breaks, the kills-late dead list, ...). Called once per FLAG_RESET.
  void on_host_reset();
  void on_save(ConnectionId id, Connection& c, protocol::Message& message);
  // Keeps an entry in the table; false when it is new and the table is full.
  bool keep_flag(const protocol::FlagEntry& e);
  void send_snapshot(ConnectionId to);
  void on_object_present(ConnectionId id, Connection& c, const protocol::ObjectPresent& p,
                         std::uint64_t now_us);
  void on_object_message(ConnectionId id, Connection& c, std::uint8_t channel,
                         protocol::Message& message, std::uint64_t now_us);
  void on_damage(ConnectionId id, Connection& c, protocol::Damage& d, std::uint64_t now_us);
  void on_pvp_mode(ConnectionId id, Connection& c, const protocol::PvpMode& m);
  void on_difficulty(ConnectionId id, Connection& c, const protocol::Difficulty& m);
  // After a join or a leave: the automatic scaler's new values, to everyone
  // but `except` (the joiner, told already; the leaver). Nothing when the
  // values in force are the ones told before, as with the host's own choice.
  void tell_difficulty_change(ConnectionId except);
  void on_pvp_hit(ConnectionId id, Connection& c, protocol::Message& message);
  void on_object_group(ConnectionId id, Connection& c, const protocol::ObjectGroup& g);
  void on_fish_lure(Connection& c, const protocol::FishLure& m, std::uint64_t now_us);
  // A fish's owner by the lures (issue 151): asks it over when another game's
  // lure is clearly better than its owner's.
  // unfit: its owner's game stands still (unfit_owner).
  void fish_owner(const protocol::ObjectId& oid, Object& o, ConnectionId owner, bool unfit,
                  std::uint64_t now_us);
  void on_status(Connection& c, std::uint8_t flags, std::uint64_t now_us);
  void on_grass_cut(ConnectionId id, Connection& c, protocol::GrassCut& cut,
                    std::uint64_t now_us);
  // Forgets the cuts of a scene no joined player is in: the grass grows back
  // in a game that loads the scene again, so the cuts end with the last game
  // that still shows them.
  void forget_empty_grass(const std::string& scene);
  // WORLD_BREAK, WORLD_MOVE, WORLD_PLACE, WORLD_HIT (world.hpp keeps what is kept).
  void on_world(ConnectionId id, Connection& c, std::uint8_t channel, protocol::Message& message,
                std::uint64_t now_us);
  void forget_empty_world(const std::string& scene);
  void forget_empty_dead(const std::string& scene);
  // A placed enemy listed by game `id`: true when it is dead for that game
  // (told now, or still its dying copy); false when it is alive for it.
  bool dead_for(ConnectionId id, Connection& c, const std::string& scene,
                const protocol::ObjectId& oid);
  // Whether a joined player is in this scene now.
  bool scene_has_player(const std::string& scene) const;
  // The player is in `scene` now; leaving another scene lets go of its
  // objects. Always true.
  // (Callers first ask stale_scene whether the message is a late one.)
  bool stale_scene(std::string& said, const Connection& c, const std::string& scene);
  bool set_scene(ConnectionId id, Connection& c, const std::string& scene,
                 std::uint64_t now_us);
  // Sends a player who entered a scene the OBJECT_SPAWNs still alive in it.
  void replay_spawns(ConnectionId id, const Connection& c);
  // The player no longer has the object loaded (unload, scene change, leave):
  // what it owns goes to another player who has it; a made object nobody
  // else has is removed, a placed one forgotten.
  void drop_presence(ConnectionId id, std::uint8_t number, const protocol::ObjectId& oid,
                     std::uint64_t now_us);
  // Lets go of everything the player has loaded or made.
  void drop_all_objects(ConnectionId id, std::uint8_t number,
                        const std::set<protocol::ObjectId>& present, std::uint64_t now_us);
  // The rules that run on the clock: answers that never came, groups,
  // owners that cannot play, the distance rule.
  void run_ownership(std::uint64_t now_us);
  // The end of the owner hold: each held object to the lowest player number
  // that lists it, announced to its scene.
  void end_owner_hold(std::uint64_t now_us);
  // Asks the owner to let go of it to `to`; finish_handover follows.
  void ask_handover(const protocol::ObjectId& oid, Object& o, ConnectionId to,
                    std::uint64_t now_us);
  // The owner answered (or never did: answer null) and the object moves.
  void finish_handover(const protocol::ObjectId& oid, Object& o,
                       const protocol::ObjectState* answer, std::uint64_t now_us);
  // Gives the object to `next` (or nobody) with epoch + 1: HANDOVER_STATE to
  // the new owner, then OBJECT_OWNER to the scene.
  void change_owner(const protocol::ObjectId& oid, Object& o, std::optional<ConnectionId> next,
                    std::uint8_t old_owner, std::uint64_t now_us);
  // The best other player who has it loaded: the nearest when places are
  // known, then the lowest number. fit_only: none who is loading, paused or
  // dead.
  std::optional<ConnectionId> best_other(const Object& o, std::optional<ConnectionId> except,
                                         bool fit_only, std::uint64_t now_us) const;
  // Joined, in the object's scene and with it loaded.
  bool has_loaded(ConnectionId id, const Object& o) const;
  // Metres from the player's Link to the object; none when either place is
  // not known or the player's is stale.
  std::optional<double> distance_to(const Connection& c, const Object& o,
                                    std::uint64_t now_us) const;
  bool unfit_owner(const Connection& c, std::uint64_t now_us) const;
  void mark_fight(Object& o, std::uint64_t now_us);
  bool in_fight(const Object& o, std::uint64_t now_us) const;
  // The owner hit it itself within boss_owner_hit_hold_us (issue 150).
  bool owner_hit_lately(const Object& o, std::uint64_t now_us) const;
  // The game that has said longest (at least handover_chased_us) that its
  // copy of `o`, or of one of its members, chases its Link; not the owner.
  std::optional<ConnectionId> chased_by(const Object& o, ConnectionId owner,
                                        std::uint64_t now_us) const;
  std::uint32_t group_of(const Object& o) const;
  protocol::ObjectOwner owner_message(const protocol::ObjectId& oid, const Object& o) const;
  // Takes it out of its group (its own members go free) and forgets it.
  void forget(Objects::iterator it);
  void unlink(const protocol::ObjectId& oid, Object& o);
  void announce_owner(const protocol::ObjectId& oid, const Object& o);
  // To every joined player in `scene` but `except`; empty scene: everyone.
  void send_to_scene(const std::string& scene, std::optional<ConnectionId> except,
                     std::uint8_t channel, const protocol::Message& m);
  std::optional<ConnectionId> connection_of(std::uint8_t number) const;
  bool within_rate(Connection& c, std::uint8_t channel, std::size_t bytes, std::uint64_t now_us);
  // why: more for the server's log (which version is older), or empty.
  void refuse(ConnectionId id, Connection& c, protocol::RefuseReason reason,
              std::uint64_t now_us, const std::string& why = {});
  // `why` goes into the log line: every drop says its reason.
  void remove_player(ConnectionId id, protocol::LeaveReason reason, std::uint64_t now_us,
                     const std::string& why = {});
  std::optional<ConnectionId> holder_of(const std::string& nickname) const;
  std::uint8_t free_number(std::uint8_t preferred = 0) const;
  std::uint8_t max_players() const;  // config_.max_players, 1 to kMaxPlayers

  void send(ConnectionId to, std::uint8_t channel, const protocol::Message& m);
  void send_to_others(ConnectionId except, std::uint8_t channel,
                      const protocol::Message& m);
  // PLAYER_SAMPLE and PLAYER_POSE: to each other player by interest.
  void send_streamed(ConnectionId from, const Connection& sender, std::uint8_t channel,
                     const protocol::Message& m, std::uint64_t now_us);
  void log(const std::string& line) const;

  SessionConfig config_;
  std::string session_game_build_;  // set while players are in
  std::map<ConnectionId, Connection> connections_;
  // The host's connection; set while players are in.
  std::optional<ConnectionId> host_;
  // PvP now: config_.pvp until the host changes it; back to config_.pvp when
  // the session empties. `pvp_set_by_` is PVP_MODE's set_by (0: the server).
  bool pvp_ = false;
  std::uint8_t pvp_set_by_ = 0;
  // Difficulty (protocol/difficulty.hpp): automatic, by the number of
  // players, until the host chooses; the host's choice then stays whoever
  // joins or leaves, a new host included. Automatic again when the session
  // empties. `difficulty_set_by_` is DIFFICULTY's set_by; `difficulty_told_`
  // the values the players in the session were last told.
  protocol::DifficultyChoice difficulty_;
  std::uint8_t difficulty_set_by_ = 0;
  protocol::DifficultyValues difficulty_told_;
  // Every shared flag value the host has sent, by key. Kept when
  // the host leaves; forgotten when the session empties.
  std::map<protocol::FlagKey, protocol::FlagEntry> flags_;
  // Shared objects, by id. Forgotten when no game has them loaded.
  Objects objects_;
  std::uint32_t next_serial_ = 1;
  std::uint64_t ownership_checked_at_ = 0;
  // The owner hold (owner_hold_us): until when, from the session's first
  // object listing (0: not started), and whether its objects got owners.
  std::uint64_t owner_hold_until_ = 0;
  bool owner_hold_done_ = false;
  // The field grass cut in each scene (GRASS_CUT), oldest first, at most
  // kMaxGrassCuts a scene; sent to a player who enters the scene. Forgotten
  // when no player is in the scene.
  std::map<std::string, std::deque<protocol::GrassCut>> grass_;
  // Placed enemies killed in a scene (issue 133, docs/objects.md "Kills for
  // late entrants"): who saw it die (the games that had it loaded, and
  // whether each still lists its dying copy) and where. A game that did not
  // see it die gets OBJECT_REMOVE dead when it lists it. A witness that lists
  // it again after it left its list respawned it (a room loaded again): the
  // record goes. Forgotten when no player is in the scene.
  struct Dead {
    std::map<ConnectionId, bool> witnesses;  // true: still lists it
    protocol::Vec3 position{};
    std::uint8_t by = 1;  // the player whose game said it died (the sender field)
  };
  std::map<std::string, std::map<protocol::ObjectId, Dead>> dead_;
  static constexpr std::size_t kMaxDeadPerScene = 1024;
  WorldStore world_;
  std::vector<Action> actions_;
  Stats stats_;
  // Wrong join passwords by address (max_wrong_passwords). An entry goes
  // when its window and its block are both over (tick sweeps them), so the
  // map holds only addresses that guessed wrong in the last minute or two.
  struct WrongGuesses {
    std::uint32_t count = 0;
    std::uint64_t first_at = 0;       // the first wrong guess of this window
    std::uint64_t blocked_until = 0;  // 0: not blocked
  };
  std::map<std::uint32_t, WrongGuesses> wrong_guesses_;
  static constexpr std::size_t kMaxWrongGuessAddresses = 4096;
  // Counts a wrong proof from `address`; blocks it at the limit.
  void note_wrong_password(std::optional<std::uint32_t> address, std::uint64_t now_us);
};

}  // namespace coop::server
