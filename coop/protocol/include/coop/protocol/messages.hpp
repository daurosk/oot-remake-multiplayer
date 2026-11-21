#pragma once

// The messages of protocol version 0, stages 0 and 1 (docs/protocol.md).
// Plain structs only: no sockets, no ENet, no game. The codec in codec.hpp
// turns them into bytes and back.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "coop/protocol/difficulty.hpp"
#include "coop/protocol/join_password.hpp"
#include "coop/protocol/traffic_crypto.hpp"

namespace coop::protocol {

// 2: OBJECT_SPAWN of a placed id; 3: TIME_OF_DAY; 4: the messages
// (types 28 to 39) and the server sending HANDOVER_ASK; 9: the join password
// (JOIN_CHALLENGE, JOIN_PROOF, REFUSED bad_password).
// 12: DAMAGE's hit place (boss-intro, 0.11); 13: the host's approval of
// joiners (JOIN_KNOCK, JOIN_ALLOW, REFUSED not_let_in; issue 261).
// 14: the hit that happened (HitRecord) in DAMAGE and in the owner's samples (component 21), so every
// game's enemy takes the real hit through its own damage step (0.11.5, every hit shows on every screen).
// 15: PLATFORM_CLOCK keys in a mover's GroundControl node (regions 0xfffe / 0xfffd, #196, 0.11.6): a protocol 14
// reader refuses them as bad_value and drops the whole message, so a 0.11.5 game must be refused at HELLO.
inline constexpr std::uint16_t kProtocolVersion = 15;
// A public game (anyone joins without typing a password) lists itself on the relay's room list with this bit
// on top of kProtocolVersion; the relay repeats the version as given. A listing without it is a private game
// (or an older kit's): the joiner asks for the host's password and never tries the public one on it.
inline constexpr std::uint16_t kPublicListingBit = 0x8000;
constexpr std::uint16_t public_listing_version() { return kPublicListingBit | kProtocolVersion; }
inline constexpr std::uint16_t kDefaultPort = 27510;
inline constexpr std::uint8_t kMaxPlayers = 16;  // protocol 11; 4 before

// Size of the message header: u16 type, u16 length.
inline constexpr std::size_t kHeaderSize = 4;
// Size of a component header: u16 id, u16 length.
inline constexpr std::size_t kComponentHeaderSize = 4;

// Maximum lengths of string fields, in Unicode characters (code points).
// A string may also never be longer than 4 bytes per character, must be valid
// UTF-8 and holds no control characters (U+0000-U+001F, U+007F-U+009F).
inline constexpr std::size_t kMaxBuildIdChars = 64;

// A client's name for HELLO's build_id (docs/crossplay.md) cut to kMaxBuildIdChars characters, so a long version text
// (a long `git describe`, a launcher's own version) never keeps the HELLO from being encoded and the player from
// joining. Cut at a character's start, never inside a UTF-8 sequence.
inline std::string clamp_build_id(std::string_view id) {
  std::size_t chars = 0;
  for (std::size_t i = 0; i < id.size(); ++i) {
    if ((static_cast<unsigned char>(id[i]) & 0xC0) != 0x80 && chars++ == kMaxBuildIdChars) {
      return std::string(id.substr(0, i));
    }
  }
  return std::string(id);
}
// The co-op release a HELLO's build_id names (docs/crossplay.md): its first word that is a version, "v0.11.7" or
// "0.11.7" (at least two numbers with dots), without a `git describe` tail ("-86-gab12cd34", "-dirty"); "" when it names
// none (a launcher's own name, a commit alone, a test tool). Two games of different releases are not let play together.
inline std::string coop_release(std::string_view id) {
  std::size_t at = 0;
  while (at < id.size()) {
    const std::size_t end = (std::min)(id.find(' ', at), id.size());   // (parenthesised: windows.h's min macro)
    std::string_view word = id.substr(at, end - at);
    at = end + 1;
    if (word.starts_with('v') || word.starts_with('V')) word.remove_prefix(1);
    word = word.substr(0, word.find('-'));
    std::size_t dots = 0;
    bool ok = !word.empty() && word.front() != '.' && word.back() != '.';
    for (std::size_t i = 0; ok && i < word.size(); ++i) {
      if (word[i] == '.') {
        ok = word[i - 1] != '.';
        ++dots;
      } else {
        ok = word[i] >= '0' && word[i] <= '9';
      }
    }
    if (ok && dots >= 1) return "v" + std::string(word);
  }
  return {};
}
inline constexpr std::size_t kMaxGameBuildIdChars = 32;
inline constexpr std::size_t kMaxGameVersionChars = 16;
inline constexpr std::size_t kMaxNicknameChars = 16;
inline constexpr std::size_t kMaxChatChars = 200;
inline constexpr std::size_t kMaxSceneChars = 64;
inline constexpr std::size_t kMaxAnimationChars = 64;

// ENet channels (docs/protocol.md, "Transport").
inline constexpr std::uint8_t kChannelSession = 0;
inline constexpr std::uint8_t kChannelProgress = 1;
inline constexpr std::uint8_t kChannelOwnership = 2;
inline constexpr std::uint8_t kChannelPlayerSamples = 3;
inline constexpr std::uint8_t kChannelObjectSamples = 4;
// Proximity voice chat (VOICE_FRAME), unreliable: its own channel, so a late
// voice packet never holds up or drops a pose or object sample.
inline constexpr std::uint8_t kChannelVoice = 5;
inline constexpr std::uint8_t kChannelCount = 6;
// The largest UDP payload ENet puts in one datagram (its MTU), on the server
// and in every game; ENet takes the smaller of the two sides, so either side
// alone is enough. ENet's default of 1392 makes 1420-byte IP packets, more
// than a VPN adapter carries (Hamachi about 1404, Tailscale 1280): those were
// split or dropped, and a large reliable message (a snapshot) was sent again
// until the connection timed out (issue 143). A home line can be smaller
// still: a DS-Lite (IPv4 in IPv6) or PPPoE tunnel, one reporter's at 1200
// bytes, where 1200-byte datagrams (1228-byte packets) did not fit either.
// 1100 + 28 header bytes fits them all. Not part of the protocol version:
// ENet agrees on it at connect.
inline constexpr std::uint32_t kDatagramBytes = 1100;

// Message type numbers on the wire. Never renumber; add new ones at the end.
enum class MessageType : std::uint16_t {
  hello = 1,
  welcome = 2,
  refused = 3,
  player_joined = 4,
  player_left = 5,
  ping = 6,
  pong = 7,
  player_status = 8,
  chat = 9,
  player_sample = 10,
  test_payload = 11,
  join_partner = 12,
  flag_set = 13,
  flag_state = 14,
  flag_snapshot = 15,
  host = 16,
  player_equip = 17,
  object_present = 18,
  object_owner = 19,
  handover_ask = 20,
  handover_state = 21,
  object_spawn = 22,
  object_sample = 23,
  object_remove = 24,
  damage = 25,
  player_pose = 26,
  time_of_day = 27,
  grass_cut = 28,
  object_group = 29,  // 30, 31: kept for enemy ownership
  world_break = 32,
  world_move = 33,
  world_place = 34,
  world_hit = 35,
  drop_spawn = 36,
  drop_take = 37,
  drop_ask = 38,
  boss_room = 50,  // 50 to 54: boss rooms (docs/dungeons-boss.md)
  sign_cut = 80,   // 80 to 84: signs (docs/world.md, "Signs")
  flag_reset = 135,  // 135 to 139: the host's save load (docs/protocol.md, "Saves")
  ocarina_note = 140,  // 140 to 144: the other players' Ocarina (docs/protocol.md, "Ocarina")
  flag_origin = 150,  // 150 to 154: who made a shared change (docs/protocol.md, "Who did it")
  join_challenge = 191,  // 191 to 195: the join password (docs/protocol.md, "Join password")
  join_proof = 192,
  save_ask = 206,   // 206 to 215: the host's save sent to guests (docs/protocol.md, "Save transfer")
  save_part = 207,
  pvp_mode = 241,  // 241 to 250: player against player (docs/protocol.md, "PvP")
  pvp_hit = 242,
  pvp_hurt = 243,
  player_life = 251,  // 251 to 255: the co-op HUD (docs/protocol.md, "Hearts")
  fish_lure = 320,  // 320 to 324: the fishing pond (docs/protocol.md, "Fishing")
  rejoin_token = 345,  // 345 to 349: rejoin after a crash or drop (docs/protocol.md, "Rejoin")
  rejoin_claim = 346,
  switch_state = 356,  // room-objects: a room switch's own state (docs/protocol.md, "Switch states")
  dungeon_event = 355,  // 355 to 359: a room gimmick's event started (docs/protocol.md, "Gimmick events")
  player_ride = 385,  // 385 to 389: riding Epona (docs/protocol.md, "Riding")
  platform_clock = 400,  // 400 to 404: moving platforms' clocks (docs/protocol.md, "Moving platforms")
  platform_counter = 401,  // looping platforms' pause counters (docs/protocol.md, "Moving platforms")
  dbno_mode = 470,  // 470 to 479: down but not out (docs/protocol.md, "Down but not out")
  dbno_state = 471,
  dbno_revive = 472,
  dbno_threat = 473,
  difficulty = 480,  // 480 to 489: difficulty (docs/protocol.md, "Difficulty")
  voice_frame = 420,  // 420 to 424: proximity voice chat (docs/protocol.md, "Voice")
  mod_list = 490,  // 490 to 499: mods (docs/protocol.md, "Mods")
  mod_settings = 491,
  lobby_action = 510,  // 510 to 519: the Lobby's host moderation (docs/protocol.md, "Lobby")
  lobby_mutes = 511,
  lobby_result = 512,
  local_join = 460,  // 460 to 469: split screen's extra players (docs/protocol.md, "Local players")
  local_joined = 461,
  local_leave = 462,
  local_sample = 463,
  local_message = 464,
  player_look = 430,  // 430 to 434: a look of several parts (docs/protocol.md, "Looks")
  link_sound = 500,  // 500 to 509: the other Links' sounds (docs/protocol.md, "Link sounds")
  boss_scene = 520,  // 520 to 524: boss intro and death cutscenes (docs/protocol.md, "Boss cutscenes")
  boss_heart = 521,
  boss_alive = 522,
  join_knock = 525,  // 525 to 529: the host lets joiners in (docs/protocol.md, "Host approval")
  join_allow = 526,
  bomb_take = 530,  // 530 to 534: a bomb changes hands (docs/protocol.md, "Bomb hand-over")
  bomb_answer = 531,
  bomb_share = 532,
  lift_ask = 535,  // 535 to 539: one player lifts another (docs/protocol.md, "Player lift")
  lift_answer = 536,
  lift_place = 537,
  lift_share = 538,
  pvp_strike = 540,  // 540 to 544: PvP on real bodies (docs/protocol.md, "PvP on real bodies")
  pvp_result = 541,
  pvp_native = 542,
};

// PLAYER_SAMPLE component ids.
enum class SampleComponent : std::uint16_t {
  time = 1,
  position = 2,
  velocity = 3,
  rotation = 4,
  scene = 5,
  age = 6,
  animation = 7,
};

// PLAYER_EQUIP component ids. 1 to 8 are the game's equipped actor names
// (PlayerStatus.Equip*), 9 the drawn bits, 10 the actor name of the item in
// the hand slot (slingshot, bow; read from the piece itself).
enum class EquipComponent : std::uint16_t {
  weapon = 1,
  shield = 2,
  armor = 3,
  boots = 4,
  head = 5,
  hand = 6,
  ocarina = 7,
  arrow = 8,
  drawn = 9,
  held = 10,
  // The actor in each other hand slot of the game's EquipmentUser, by name,
  // as `held` is for slot 3 (docs/puppet.md, "The other hand slots").
  weapon_slot = 11,   // slot 0, Weapon: the sword, or a Deku stick in its place
  sub_weapon = 12,    // slot 1, SubWeapon
  third_weapon = 13,  // slot 2, ThirdWeapon
  tool = 14,          // slot 5, Tool
  sub_tool = 15,      // slot 6, SubTool
  // The player's colour (coop-options.ini `color`): 3 bytes, red, green,
  // blue. Others tint that player's puppet with it (docs/puppet.md, "Colour").
  color = 16,
  // Bit i set: the piece in equipment slot i burns (a lit Deku stick), as
  // the owner's game shows it. Others play the piece's own flame effect on
  // their puppet's piece, as a picture only (docs/puppet.md, "A lit Deku stick").
  lit = 17,
};

// Longest equipped actor name: the game keeps each in a String64 flag.
inline constexpr std::size_t kMaxEquipNameChars = 64;

// Player numbers are 1 to 4. In PLAYER_STATUS, CHAT and PLAYER_SAMPLE a
// client sends 0 ("me"); the server fills in the sender's number when it
// relays the message to the others.
inline constexpr std::uint8_t kPlayerSelf = 0;

// HELLO, client to server. The protocol version is always the first two bytes
// of the body, in every protocol version, so a server can refuse a client
// whose HELLO it cannot otherwise read (see hello_protocol_version).
struct Hello {
  std::uint16_t protocol_version = kProtocolVersion;
  std::string build_id;       // emulator or recomp, and its version
  std::string game_build_id;  // for example "e0f3ccfaf50065ae"
  std::string game_version;   // for example "1.1.1"
  std::string nickname;       // 1 to 16 characters
  // The game's X25519 public key for traffic encryption (traffic_crypto.hpp),
  // 32 bytes after the nickname. Sent by every game since protocol 10; a
  // server with a join password refuses a HELLO without it.
  std::optional<KeyShare> key_share{};
  // Since protocol 10 (this release's): the player number this game had in its last session
  // (1 to 4), one byte after the key share (or after the nickname without
  // one). The server gives it back when it is free, so a server restart
  // does not swap the players' numbers (and with them their colours and
  // every object's owner). Nothing on a game's first join.
  std::optional<std::uint8_t> previous_number{};
  bool operator==(const Hello&) const = default;
};

struct PlayerInfo {
  std::uint8_t number = 1;  // 1 to 4
  std::string nickname;
  bool operator==(const PlayerInfo&) const = default;
};

struct Welcome {
  std::uint8_t player_number = 1;  // 1 to 4
  std::uint64_t server_time_us = 0;
  std::uint16_t tick_rate = 0;
  std::vector<PlayerInfo> players;  // the players already in, at most kMaxPlayers - 1
  bool operator==(const Welcome&) const = default;
};

// Reasons are kept as raw numbers on decode, so a newer server's reason
// still reaches the client (which shows it as unknown).
enum class RefuseReason : std::uint8_t {
  wrong_protocol_version = 1,
  wrong_game_build = 2,
  server_full = 3,
  name_taken = 4,
  bad_name = 5,  // empty or nothing but spaces
  bad_password = 6,  // the server has a join password and the JOIN_PROOF did not match
  // Protocol 11, the Lobby (docs/protocol.md "Lobby"): the host took this
  // player out. Sent after WELCOME too, right before the server closes the
  // connection; the game then stops and does not join again by itself.
  kicked = 7,
  // The host banned this player for the rest of the session: as kicked, and
  // a HELLO with that name or from that address is refused with this.
  banned = 8,
  // Protocol 13, host approval (docs/protocol.md "Host approval"): the host
  // did not let this player in (it said no, did not answer in time, or left).
  not_let_in = 9,
  // The game runs another co-op release than the host's (coop_release of the HELLO's build_id against the session's
  // first player's; a build that names none is let in). Older games show it as an unknown reason.
  wrong_coop_version = 10,
  // Never sent: the game's own refusal of a server that did not encrypt
  // although the game has a join password (docs/protocol.md "Encrypted
  // traffic"), so nobody on the path can stand in for a no-password server.
  not_encrypted = 200,
};

struct Refused {
  RefuseReason reason = RefuseReason::wrong_protocol_version;
  bool operator==(const Refused&) const = default;
};

// JOIN_CHALLENGE, server to client, in answer to a HELLO when the server has
// a join password: a fresh random nonce for this connection only. The client
// answers with JOIN_PROOF; WELCOME or REFUSED follows.
struct JoinChallenge {
  JoinNonce nonce{};
  // The server's X25519 public key, 32 bytes after the nonce, when the
  // HELLO had one: from the JOIN_PROOF on, the connection is encrypted.
  std::optional<KeyShare> key_share{};
  bool operator==(const JoinChallenge&) const = default;
};

// JOIN_PROOF, client to server: join_proof(password, nonce) for the nonce of
// this connection's JOIN_CHALLENGE (an empty password when it has none).
struct JoinProof {
  JoinProofBytes proof{};
  // Protocol 11 (the Lobby): the player key, sealed (seal_player_key), 32
  // bytes after the proof.
  std::optional<SealedPlayerKey> player_key{};
  bool operator==(const JoinProof&) const = default;
};

// REJOIN_TOKEN, server to client, right after WELCOME: a fresh random token
// for this player's place. Only a connection that shows it (REJOIN_CLAIM)
// may take that place over while the old connection is quiet, so two players
// behind one address cannot push each other out by name.
using RejoinTokenBytes = JoinNonce;
struct RejoinToken {
  RejoinTokenBytes token{};
  bool operator==(const RejoinToken&) const = default;
};

// REJOIN_CLAIM, client to server, before its HELLO on a new connection: the
// token its last WELCOME came with. Without one no takeover happens.
struct RejoinClaim {
  RejoinTokenBytes token{};
  bool operator==(const RejoinClaim&) const = default;
};

struct PlayerJoined {
  std::uint8_t player_number = 1;  // 1 to 4
  std::string nickname;
  bool operator==(const PlayerJoined&) const = default;
};

enum class LeaveReason : std::uint8_t {
  left = 0,
  timed_out = 1,
  kicked = 2,
};

struct PlayerLeft {
  std::uint8_t player_number = 1;  // 1 to 4
  LeaveReason reason = LeaveReason::left;  // raw value kept on decode
  bool operator==(const PlayerLeft&) const = default;
};

struct Ping {
  std::uint64_t sender_time_us = 0;
  bool operator==(const Ping&) const = default;
};

struct Pong {
  std::uint64_t ping_sender_time_us = 0;  // echoed from the PING
  std::uint64_t receiver_time_us = 0;     // the PONG sender's clock
  bool operator==(const Pong&) const = default;
};

// PLAYER_STATUS flag bits. Unknown bits are kept, not refused.
namespace status {
inline constexpr std::uint8_t paused = 1u << 0;
inline constexpr std::uint8_t loading = 1u << 1;
inline constexpr std::uint8_t in_cutscene = 1u << 2;
inline constexpr std::uint8_t in_menu = 1u << 3;
inline constexpr std::uint8_t dead = 1u << 4;
// Not really here: a player who cannot fight or be fought now (PvP never hits them, and with the
// puppets perceived no enemy picks their puppet: coop_pvp.cpp Guarded, coop_dbno_body.h UnseenWhy).
inline constexpr std::uint8_t away = paused | loading | in_cutscene | in_menu | dead;
}  // namespace status

struct PlayerStatus {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  std::uint8_t flags = 0;
  bool operator==(const PlayerStatus&) const = default;
};

// PLAYER_LIFE (protocol 10): a player's hearts, in quarter hearts as the game
// keeps them (4 = one heart): life now and the most it can be. Sent reliably
// on every change and again when another player joins; the others draw it
// under their own hearts (docs/overlay.md). Same player field rules as
// PLAYER_STATUS. max_life 0: not known (the game's Link is not loaded).
struct PlayerLife {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  std::uint16_t life = 0;
  std::uint16_t max_life = 0;
  bool operator==(const PlayerLife&) const = default;
};

// The most quarter hearts PLAYER_LIFE carries: 20 hearts, with room for a
// game that allows more.
inline constexpr std::uint16_t kMaxLifeQuarters = 4 * 40;

struct Chat {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  std::string text;  // up to 200 characters
  bool operator==(const Chat&) const = default;
};

// The time of a sample is on the server clock, not the sender's: the server
// relays it unchanged, and a receiver can place it only on a clock everyone
// shares. The sender works it out from its own clock and its offset estimate
// (coop::net::Client::server_now_us or to_server_time).
struct SampleTime {
  std::uint64_t server_time_us = 0;
  std::uint32_t sequence = 0;
  bool operator==(const SampleTime&) const = default;
};

using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;  // x, y, z, w

enum class Age : std::uint8_t {
  child = 0,
  adult = 1,
};

struct Animation {
  std::string name;
  float frame = 0.0f;
  float rate = 0.0f;
  bool operator==(const Animation&) const = default;
};

// PLAYER_RIDE (protocol 10, issue 199): the horse a player rides, sent with
// its samples (channel 3) while it rides, and once with riding off when it
// gets down. The others draw a copy of the horse under the rider's puppet:
// at the puppet's place plus `offset` (the horse's place minus the rider's,
// metres), turned `rotation`, playing `animation` (the horse's AS slot 0).
// Same player field rules as PLAYER_STATUS. Floats must be finite.
inline constexpr std::uint8_t kRideRiding = 0x01;
inline constexpr float kMaxRideOffset = 10.0f;  // each axis
inline constexpr std::size_t kMaxRideHorseChars = 64;
struct PlayerRide {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  std::uint8_t flags = 0;                     // kRideRiding; other bits refused
  std::string horse;                          // the horse's actor name; empty when off
  Vec3 offset{};
  Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
  std::optional<Animation> animation;
  bool operator==(const PlayerRide&) const = default;
};

// PLAYER_SAMPLE. The time component is always sent; the others only when the
// sender has them. Floats must be finite.
struct PlayerSample {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  SampleTime time;
  std::optional<Vec3> position;
  std::optional<Vec3> velocity;  // metres a second
  std::optional<Quat> rotation;
  std::optional<std::string> scene;
  std::optional<Age> age;
  std::optional<Animation> animation;
  bool operator==(const PlayerSample&) const = default;
};

// TEST_PAYLOAD, used only by the stage 0 test tool. The filler is the rest of
// the body after the sequence number.
struct TestPayload {
  std::uint32_t sequence = 0;
  std::vector<std::uint8_t> filler;
  bool operator==(const TestPayload&) const = default;
};

// JOIN_PARTNER: a warp to another player's scene, in three steps.
//   1. The asker sends kind request, other = the player to join.
//   2. The server relays it to that player, with the asker in the player field.
//   3. That player answers with kind accept (where it stands) or refuse,
//      other = the asker; the server relays it back the same way.
// The server itself answers refuse / not_there when the player to join is
// not in the session. The request id is the asker's own; the answer echoes
// it, so a late answer to an older request can be told apart.
enum class JoinKind : std::uint8_t {
  request = 0,
  accept = 1,
  refuse = 2,
};

// Kept as a raw number on decode, so a newer build's reason still arrives.
enum class JoinRefuseReason : std::uint8_t {
  busy = 1,         // loading, in a cutscene, in a menu or dead
  declined = 2,     // the player said no
  cannot_warp = 3,  // a place nobody may warp into (a boss fight, for example)
  not_there = 4,    // sent by the server: that player is not in the session
};

// Where the answering player stands, for kind accept.
struct JoinTarget {
  std::string scene;  // the game's scene name, 1 to 64 characters
  Vec3 position{};
  Quat rotation{};  // x, y, z, w, as in PLAYER_SAMPLE
  bool operator==(const JoinTarget&) const = default;
};

struct JoinPartner {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  std::uint8_t other_player = 1;  // 1 to 4: who to join, or who asked
  std::uint16_t request_id = 0;
  JoinKind kind = JoinKind::request;
  std::optional<JoinTarget> target;  // set only for kind accept
  JoinRefuseReason reason = JoinRefuseReason::busy;  // read only for kind refuse
  bool operator==(const JoinPartner&) const = default;
};

// One shared world flag. Two kinds of key (docs/protocol.md):
// - named: the Murmur3 x86 32-bit hash, seed 0, of the flag's name in the
//   game (flag_hash in codec.hpp), and for an array flag the element in
//   index (0 for a plain flag).
// - key64: a 64-bit-key flag (the game's Bool64bitKey: chests, pots, rupees,
//   Gold Skulltula tokens, enemies), keyed by the placement's 64-bit hash.
//   table names the Bool64bitKey table by its hash; 0 is the game's one
//   table, the only one so far. Always a bool.
// The fields a kind does not use are 0. The value is 4 bytes on the wire,
// read by kind.
enum class FlagKeyKind : std::uint8_t {
  named = 0,
  key64 = 1,
};

struct FlagKey {
  FlagKeyKind kind = FlagKeyKind::named;
  std::uint32_t hash = 0;   // named only
  std::uint16_t index = 0;  // named only
  std::uint64_t key = 0;    // key64 only
  std::uint32_t table = 0;  // key64 only; 0 = the game's one table

  constexpr FlagKey() = default;
  // A named flag.
  constexpr FlagKey(std::uint32_t name_hash, std::uint16_t element)
      : hash(name_hash), index(element) {}
  static constexpr FlagKey key64_flag(std::uint64_t key, std::uint32_t table = 0) {
    FlagKey k;
    k.kind = FlagKeyKind::key64;
    k.key = key;
    k.table = table;
    return k;
  }
  auto operator<=>(const FlagKey&) const = default;
};

enum class FlagKind : std::uint8_t {
  boolean = 0,   // value 0 or 1
  int32 = 1,     // two's complement
  float32 = 2,   // IEEE-754 bits, finite
  uint32 = 3,    // an enum flag's hash, or any other unsigned value
  // A change to a count (small keys, Gold Skulltula tokens, heart pieces),
  // two's complement: +1 got, -1 used. FLAG_SET only, named keys only; the
  // host adds it to its value and answers FLAG_STATE with the int32 total.
  count_delta = 4,
};

struct FlagEntry {
  FlagKey key;
  FlagKind kind = FlagKind::boolean;
  std::uint32_t value = 0;  // raw bits; see FlagKind

  constexpr FlagEntry() = default;
  constexpr FlagEntry(FlagKey k, FlagKind value_kind, std::uint32_t v)
      : key(k), kind(value_kind), value(v) {}
  // A named flag.
  constexpr FlagEntry(std::uint32_t hash, std::uint16_t index, FlagKind value_kind,
                      std::uint32_t v)
      : key(hash, index), kind(value_kind), value(v) {}
  bool operator==(const FlagEntry&) const = default;
};

// Bytes of one flag entry on the wire: u8 key kind, the key (named: u32
// hash, u16 index; key64: u64 key, u32 table), u8 kind, u32 value.
inline constexpr std::size_t kFlagEntrySize = 12;       // named
inline constexpr std::size_t kFlagEntryKey64Size = 18;  // key64
// Most entries one FLAG_SET, FLAG_STATE or FLAG_SNAPSHOT may carry. Keeps a
// message of key64 entries in the u16 body length (3600 x 18 + 5 bytes); a
// larger snapshot is split (split_snapshot).
inline constexpr std::size_t kMaxFlagEntries = 3600;

// FLAG_SET: a player's game changed shared flags. Goes to the server, which
// relays it to the host only. 1 to kMaxFlagEntries entries, each key at most
// once.
struct FlagSet {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  std::vector<FlagEntry> flags;
  bool operator==(const FlagSet&) const = default;
};

// FLAG_STATE: the host's answer, which every player applies, the host
// included. Sent by the host; the server keeps the values and relays it to
// everyone else. Same rules for the entries as FLAG_SET.
struct FlagState {
  std::uint8_t player_number = kPlayerSelf;  // 0 from the host, 1 to 4 relayed
  std::vector<FlagEntry> flags;
  bool operator==(const FlagState&) const = default;
};

// FLAG_RESET: the host loaded a save. The flags its load changed, as its
// game has them now, 0s included. Sent by the host only; the server keeps
// the values (replacing what it had) and relays it to everyone else, who
// write them exactly: no merging (max, OR) and no one-way rule, so the
// group's state follows the host's save (docs/flags.md, "Saves"). Same
// entries as FLAG_STATE (1 to kMaxFlagEntries, no count deltas); a longer
// reset is several messages in a row.
// FLAG_ORIGIN (server only): the player whose FLAG_SET the server just
// passed to the host. Sent to every other player but the host, before the
// host's FLAG_STATE answer, so a game can say who made the change it gets
// (the chat notices, docs/setup.md). The flags are the FLAG_SET's.
struct FlagOrigin {
  std::uint8_t player_number = kPlayerSelf;  // 1 to 4: who asked
  std::vector<FlagEntry> flags;
  bool operator==(const FlagOrigin&) const = default;
};

struct FlagReset {
  std::uint8_t player_number = kPlayerSelf;  // 0 from the host, 1 to 4 relayed
  std::vector<FlagEntry> flags;
  bool operator==(const FlagReset&) const = default;
};

// SAVE_ASK: a guest asks the host for its save (docs/protocol.md, "Save
// transfer"). The server passes it to the host only, with the asker's number.
struct SaveAsk {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a guest, 1 to 4 relayed
  bool operator==(const SaveAsk&) const = default;
};

// SAVE_PART: one part of the host's save (a packed save slot, see
// coop/host/save_transfer.hpp). Every part carries the whole transfer's
// header, so a receiver can check each one alone. Only the host sends it; the
// server passes it to `to_player`, or to every other player for 0.
inline constexpr std::uint32_t kMaxSaveBytes = 1u << 20;  // a slot is about 170 KB
inline constexpr std::size_t kMaxSavePart = 16384;
inline constexpr std::uint16_t kMaxSaveParts =
    static_cast<std::uint16_t>((kMaxSaveBytes + kMaxSavePart - 1) / kMaxSavePart);
enum class SaveReason : std::uint8_t {
  join = 1,       // a guest asked (it joined)
  host_load = 2,  // the host loaded a save: every guest reloads
};
struct SavePart {
  std::uint8_t player_number = kPlayerSelf;  // 0 from the host, 1 to 4 relayed
  std::uint8_t to_player = 0;                // 0: every guest; 1 to 4: that one
  std::uint32_t transfer_id = 0;
  SaveReason reason = SaveReason::join;
  std::uint32_t total_size = 0;  // 1 to kMaxSaveBytes
  std::uint32_t crc32 = 0;       // of all total_size bytes
  std::uint16_t index = 0;       // 0 to count - 1
  std::uint16_t count = 1;       // ceil(total_size / kMaxSavePart)
  std::vector<std::uint8_t> data;  // kMaxSavePart bytes, the last part the rest
  bool operator==(const SavePart&) const = default;
};
// The parts a save of `total_size` bytes has, and part `index`'s size; 0 for
// a size out of range.
std::uint16_t save_part_count(std::uint32_t total_size);
std::size_t save_part_size(std::uint32_t total_size, std::uint16_t index);

// FLAG_SNAPSHOT: every shared flag, in parts 0 to part_count - 1, sent in
// order on the progress channel. The server sends it to a player who joins;
// a new host sends it to the server to start the table. An empty table is
// one part with no entries. 0 to kMaxFlagEntries entries per part, each key
// at most once in a part.
struct FlagSnapshot {
  std::uint16_t part = 0;
  std::uint16_t part_count = 1;  // at least 1, more than part
  std::vector<FlagEntry> flags;
  bool operator==(const FlagSnapshot&) const = default;
};

// HOST: who hosts the session, the player whose game is the truth for
// shared flags. Sent by the server on the progress channel to a player who
// joins (before its FLAG_SNAPSHOT) and to everyone when the host changes. The
// first player in hosts; when the host leaves, the remaining player with the
// lowest number takes over.
struct Host {
  std::uint8_t player_number = 1;  // 1 to 4
  bool operator==(const Host&) const = default;
};

// The child's Goron's Bracelet, sent as `hand`: the game wears it as an
// accessory (kind 14), not as hand armor (docs/puppet.md).
inline constexpr std::string_view kChildBraceletPrefix = "Armor_Child_Gauntlet_";

// The only head pieces a puppet wears: the masks, `Armor_Child_Mask_*` (the
// game's ten; there are no adult masks, and the base `Armor_Mask_Base` is none).
// PlayerStatus.EquipHeadArmor also names the Lens of Truth once a player has
// it: `Armor_GlassTruth`, the age-less base actor whose model is the ADULT
// glasses with no animation or physics for their hanging lenses. The game
// never draws that name on Link (it wears Armor_Child_/Adult_GlassTruth only
// while the Lens is in use). Worn and shown on a puppet it drew the adult
// glasses' unposed mesh as orange spikes from the head and shoulders and a
// long streak along the ground on every puppet (row 197; 0.11.6 to 0.11.8).
constexpr bool is_drawn_head(std::string_view name) {
  constexpr std::string_view kPrefix = "Armor_Child_Mask_";
  return name.size() > kPrefix.size() && name.starts_with(kPrefix);
}

// What a player wears and holds (docs/puppet.md, "Equipment"). Each name is
// the game's actor name of that piece, for example "Weapon_Sword_Kokiri" or
// "Armor_Adult_Goron"; an empty name is nothing in that slot. A field that is
// not set is unknown: the sender could not read it, and the receiver keeps
// what it had.
struct Equipment {
  std::optional<std::string> weapon;   // sword
  std::optional<std::string> shield;
  std::optional<std::string> armor;    // tunic
  std::optional<std::string> boots;
  std::optional<std::string> head;     // mask (child), or head armor
  std::optional<std::string> hand;     // gauntlets, or the child's bracelet
  std::optional<std::string> ocarina;
  std::optional<std::string> arrow;    // arrow kind for the bow
  // Bit i set: the game's equipment slot i is drawn (in hand), not sheathed.
  // Slots 0 to 7; see docs/puppet.md for which slot is which.
  std::optional<std::uint8_t> drawn;
  // The item in the game's equipment slot 3 (slingshot, bow), by the name of
  // its actor; "" when the slot is empty. In hand when bit 3 of drawn is set.
  std::optional<std::string> held;
  // The same for the other hand slots: 0 (what is really in the sword hand),
  // 1, 2, 5 and 6. In hand when that slot's bit of drawn is set.
  std::optional<std::string> weapon_slot;
  std::optional<std::string> sub_weapon;
  std::optional<std::string> third_weapon;
  std::optional<std::string> tool;
  std::optional<std::string> sub_tool;
  // The player's colour, 0xRRGGBB. Unset: the player chose none (a receiver
  // uses its slot's colour, coop::protocol::slot_color).
  std::optional<std::uint32_t> color;
  // Bit i set: the piece in slot i burns (its ELink user plays Chemical_Fire,
  // the flame the game's chemistry starts on a lit Deku stick). 0 when none.
  std::optional<std::uint8_t> lit;
  bool operator==(const Equipment&) const = default;
};

// `now` with each field it does not carry taken from `before`: what a
// receiver keeps after a PLAYER_EQUIP, and what a sender sends after a read
// that failed for one frame.
inline Equipment merge_equipment(const Equipment& before, const Equipment& now) {
  Equipment e = now;
  auto keep = [](auto& field, const auto& old) {
    if (!field) field = old;
  };
  keep(e.weapon, before.weapon);
  keep(e.shield, before.shield);
  keep(e.armor, before.armor);
  keep(e.boots, before.boots);
  keep(e.head, before.head);
  keep(e.hand, before.hand);
  keep(e.ocarina, before.ocarina);
  keep(e.arrow, before.arrow);
  keep(e.drawn, before.drawn);
  keep(e.held, before.held);
  keep(e.weapon_slot, before.weapon_slot);
  keep(e.sub_weapon, before.sub_weapon);
  keep(e.third_weapon, before.third_weapon);
  keep(e.tool, before.tool);
  keep(e.sub_tool, before.sub_tool);
  keep(e.color, before.color);
  keep(e.lit, before.lit);
  return e;
}

// The colour of a player who chose none: one per player number (1 to 4),
// far apart on the colour wheel. 0 (and anything above 4) gets white.
inline constexpr std::uint32_t slot_color(std::uint8_t player_number) {
  switch (player_number) {
    case 1: return 0x3070FF;  // blue
    case 2: return 0xFF3030;  // red
    case 3: return 0x30D040;  // green
    case 4: return 0xFFC020;  // yellow
    // Protocol 11: up to 16 players, each its own colour.
    case 5: return 0xA040FF;   // purple
    case 6: return 0xFF7A20;   // orange
    case 7: return 0x20D0E0;   // cyan
    case 8: return 0xFF60B0;   // pink
    case 9: return 0xA0FF40;   // lime
    case 10: return 0x20A080;  // teal
    case 11: return 0x9A6030;  // brown
    case 12: return 0x2040A0;  // navy
    case 13: return 0xA02040;  // crimson
    case 14: return 0x909020;  // olive
    case 15: return 0xB0B0B0;  // silver
    case 16: return 0x404040;  // charcoal
    default: return 0xFFFFFF;
  }
}

// PLAYER_EQUIP: a player's equipment, sent reliably on every change and again
// when another player joins. Same player field rules as PLAYER_SAMPLE.
struct PlayerEquip {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  Equipment equipment;
  bool operator==(const PlayerEquip&) const = default;
};

// ---------------------------------------------------------------- objects
// Shared enemies and projectiles (docs/objects.md, docs/protocol.md "Objects").

enum class ObjectKind : std::uint8_t {
  placed = 0,  // value is the placement hash; maker is 0
  made = 1,    // value is the maker's own counter; maker is 1 to 4
};

// 10 bytes on the wire: u8 kind, u8 maker, u64 value.
struct ObjectId {
  ObjectKind kind = ObjectKind::placed;
  std::uint8_t maker = 0;
  std::uint64_t value = 0;
  auto operator<=>(const ObjectId&) const = default;
  bool operator==(const ObjectId&) const = default;
};

// OBJECT_SAMPLE component ids. 1 to 4 and 7 are laid out as in PLAYER_SAMPLE.
enum class ObjectComponent : std::uint16_t {
  time = 1,
  position = 2,
  velocity = 3,
  rotation = 4,
  animation = 7,
  life = 16,
  fuse = 17,
  state = 18,
  slots = 19,  // protocol 10, issue 150: a boss's other AS slots (below)
  bsa = 20,    // native boss sync: a boss's brain and action verbs (BossBsa below)
  hit = 21,    // protocol 14: the hit the owner's game took (ObjectHit below)
};

struct ObjectLife {
  std::int32_t life = 0;
  std::int32_t max = 0;
  bool operator==(const ObjectLife&) const = default;
};

namespace object_state {
inline constexpr std::uint8_t in_fight = 1u << 0;
inline constexpr std::uint8_t held = 1u << 1;
inline constexpr std::uint8_t asleep = 1u << 2;
// S2 (event-owner-0116): its owner's own Link's event binds it (a talk, a
// cutscene the owner's game plays with it). A copy of an actor with its own
// game logic (an Enemy_: a Deku Scrub) then keeps that logic; only the
// event's result reaches it (its flags). An older build ignores the bit.
inline constexpr std::uint8_t in_event = 1u << 3;
// Boss turns (0.11.8): the owner's enemy is in an attack, read from its game's own action verb attribute Attack
// (the attributes of its BSA's current action verbs, as the game's own method main+0x507be0 reads them). The server
// hands the enemy to the next player when it falls (server/boss_turns.hpp). An older build ignores the bit.
inline constexpr std::uint8_t attacking = 1u << 4;
// The sender reads attacks (the bit above means something): set on every sample of an enemy whose BSA it reads.
// Turns only for enemies whose owner sets it, so an older owner's boss keeps its one owner.
inline constexpr std::uint8_t attack_read = 1u << 5;
}  // namespace object_state

// Issue 150 (protocol 10): one more AS slot of the owner's actor than slot 0
// (Gohma's eye guard and eyelids play on their own slots). slot is 1 or more
// and below 32; no slot twice. An empty name: the slot plays nothing.
struct SlotAnimation {
  std::uint8_t slot = 1;
  Animation animation;
  bool operator==(const SlotAnimation&) const = default;
};
inline constexpr std::size_t kMaxObjectSlots = 8;
inline constexpr std::uint8_t kMaxSlotIndex = 31;

// Native boss sync (docs/dungeons-boss.md, "Native boss sync"): what the
// owner's game did to a boss's BSA component (the game's brain and action
// verbs), so the other games do the same to their copy through the game's own
// functions. An optional component: an older build skips it.
// - A change is one call of the owner's game: a brain verb set (verb, force)
//   or an action verb request. seq counts them for this boss, from 1.
// - recent holds the newest kMaxBossChanges changes, oldest first, in every
//   sample, so a lost sample loses none and a copy makes every change once,
//   in the owner's order.
// - brain_verb and action are the state now (the verb the owner's boss has,
//   the last action asked): where a copy starts (it joined, the owner
//   changed, or it missed more than recent holds), and what it is put back
//   to when it differs for long.
struct BossAction {
  std::int32_t verb = 0;
  std::int32_t slot = 0;
  std::uint16_t flags = 0;
  // The request's three places (target, look-at, a third), each optional
  // (the game marks a missing one NaN), and its pair of extra values.
  std::array<std::optional<Vec3>, 3> places{};
  std::optional<std::array<float, 2>> extra;
  bool changed = true;  // the game's own "the verb changed" argument
  bool operator==(const BossAction&) const = default;
};
struct BossBrain {
  std::int32_t verb = 0;
  bool force = false;
  bool operator==(const BossBrain&) const = default;
};
struct BossChange {
  std::uint32_t seq = 0;
  std::variant<BossBrain, BossAction> what;
  bool operator==(const BossChange&) const = default;
};
inline constexpr std::size_t kMaxBossChanges = 8;
struct BossBsa {
  std::uint32_t seq = 0;  // the owner's changes so far (the newest one's seq)
  std::int32_t brain_verb = 0;
  std::optional<BossAction> action;
  // At most kMaxBossChanges, seqs rising by one or more, none above seq.
  std::vector<BossChange> recent;
  bool operator==(const BossBsa&) const = default;
};

// Protocol 14 (every hit shows on every screen): the incoming hit an enemy's Life component took, as the
// game's own damage step read it (Life +0x240..+0x2d8, 38 words: attacker, hit id, present, attack kind and
// its parameters, the hit's place at +0x27c, the attacker's place at +0x2a0). The sender zeroes the attacker
// pointer (words 0 and 1); the receiver puts its own attacker, a fresh hit id and its present flag in, so its
// game judges the same hit (weapon, element, place) through its own damage step. Passed on unread otherwise.
inline constexpr std::size_t kHitRecordWords = 38;
using HitRecord = std::array<std::uint32_t, kHitRecordWords>;

// Component 21: the newest hit the owner's game took on this object, sent in its samples for a short
// while after (they may be lost); seq is the owner's own hit count for it, from 1, so each is replayed once.
struct ObjectHit {
  std::uint32_t seq = 0;
  HitRecord record{};
  bool operator==(const ObjectHit&) const = default;
};

// The components of OBJECT_SAMPLE, OBJECT_SPAWN and HANDOVER_STATE. time is
// always sent; the others when the sender has them. Floats must be finite.
struct ObjectState {
  SampleTime time;
  std::optional<Vec3> position;
  std::optional<Vec3> velocity;  // metres a second
  std::optional<Quat> rotation;
  std::optional<Animation> animation;
  std::optional<ObjectLife> life;
  std::optional<float> fuse;          // seconds left
  std::optional<std::uint8_t> state;  // object_state bits
  std::optional<std::vector<SlotAnimation>> slots;  // issue 150: bosses only
  std::optional<BossBsa> bsa;                       // native boss sync: bosses only
  std::optional<ObjectHit> hit;                     // protocol 14: the owner's newest hit taken
  bool operator==(const ObjectState&) const = default;
};

namespace object_presence {
inline constexpr std::uint8_t awake = 1u << 0;
inline constexpr std::uint8_t in_fight = 1u << 1;
inline constexpr std::uint8_t targeting = 1u << 2;  // targeting our player
// A boss whose intro this game has not played: its own boss is still in its
// pre-battle phase (Gohma on the ceiling), so it must not own the boss; its AI
// would hold every game's boss there.
inline constexpr std::uint8_t unready = 1u << 3;
}  // namespace object_presence

struct ObjectPresence {
  ObjectId id;
  std::uint8_t flags = 0;  // object_presence bits
  bool operator==(const ObjectPresence&) const = default;
};

// Most objects in one OBJECT_PRESENT.
inline constexpr std::size_t kMaxPresentObjects = 1024;
inline constexpr std::size_t kMaxActorNameChars = 64;

// OBJECT_PRESENT: the shared objects the sender has loaded in a scene. It
// replaces the sender's previous list for that scene. No id twice.
struct ObjectPresent {
  std::uint8_t player_number = kPlayerSelf;
  std::string scene;
  std::vector<ObjectPresence> objects;
  bool operator==(const ObjectPresent&) const = default;
};

// OBJECT_OWNER (server only): who owns an object now. owner 0: nobody.
struct ObjectOwner {
  ObjectId id;
  std::uint8_t owner = 0;
  std::uint16_t epoch = 0;
  std::uint32_t group = 0;  // 0: none
  bool operator==(const ObjectOwner&) const = default;
};

// HANDOVER_ASK (server only, to the owner): let go of it to `to_player`.
struct HandoverAsk {
  ObjectId id;
  std::uint16_t epoch = 0;
  std::uint8_t to_player = 1;
  bool operator==(const HandoverAsk&) const = default;
};

// HANDOVER_STATE: the old owner's last state, relayed to the new owner.
struct HandoverState {
  std::uint8_t player_number = kPlayerSelf;
  ObjectId id;
  std::uint16_t epoch = 0;
  ObjectState state;
  bool operator==(const HandoverState&) const = default;
};

namespace object_spawn {
inline constexpr std::uint8_t projectile = 1u << 0;
}  // namespace object_spawn

// OBJECT_SPAWN: the maker made it (kind made, maker = the sender's number).
// Owner is implied: the maker, epoch 0.
struct ObjectSpawn {
  std::uint8_t player_number = kPlayerSelf;
  ObjectId id;
  std::string actor;  // the game's actor name, up to 64 characters
  std::uint8_t flags = 0;  // object_spawn bits
  ObjectState state;
  bool operator==(const ObjectSpawn&) const = default;
};

// OBJECT_SAMPLE, on the object samples channel.
struct ObjectSample {
  std::uint8_t player_number = kPlayerSelf;
  ObjectId id;
  std::uint16_t epoch = 0;
  ObjectState state;
  bool operator==(const ObjectSample&) const = default;
};

enum class RemoveReason : std::uint8_t {
  gone = 0,
  dead = 1,
  exploded = 2,
  hit = 3,
  picked_up = 4,
};

struct ObjectRemove {
  std::uint8_t player_number = kPlayerSelf;
  ObjectId id;
  std::uint16_t epoch = 0;
  RemoveReason reason = RemoveReason::gone;
  Vec3 position{};
  bool operator==(const ObjectRemove&) const = default;
};

// Causes in DAMAGE. Other values are kept and passed on unchanged.
enum class DamageCause : std::uint8_t {
  unknown = 0,
  sword = 1,
  arrow = 2,
  fire_arrow = 3,
  ice_arrow = 4,
  light_arrow = 5,
  seed = 6,
  bomb = 7,
  nut = 8,
  boomerang = 9,
  hookshot = 10,
  hammer = 11,
  magic = 12,
  // The hitter's copy of an enemy whose life cannot be read died in its game (its own death
  // animation as it left): no life drop to send, so the kill itself goes (amount 1).
  killed = 13,
};

// DAMAGE: life lost on a copy, for the owner to take off. Amount 0 or more.
struct Damage {
  std::uint8_t player_number = kPlayerSelf;
  ObjectId id;
  std::uint16_t epoch = 0;  // the epoch the hitter knew
  std::int32_t amount = 0;
  std::uint8_t cause = 0;  // DamageCause
  Vec3 position{};
  // Where the hit landed, from the hitter's game's own hit record (a boss copy only; boss-intro): Gohma's weak
  // point is a headshot check against her Eyelid_Down bone, so the owner's replay lands there, not at her feet.
  // Optional, after position on the wire (protocol 12).
  std::optional<Vec3> hit_at{};
  // Protocol 14: the hit as the hitter's game took it on its copy (its newest in this DAMAGE), so the owner's
  // game takes the same hit (bomb, arrow, fire, hammer ...) instead of a stand-in sword hit. Optional, after
  // hit_at (or after position when there is no hit_at) on the wire.
  std::optional<HitRecord> hit_record{};
  bool operator==(const Damage&) const = default;
};

// PLAYER_POSE: the sender's Link skeleton after its animation, for the
// puppet to copy bone by bone (docs/puppet.md, "Pose"). Only the bones that
// move are listed, in increasing bone order, each with the parts it carries:
// the local rotation (quaternion x, y, z, w scaled by 32767), translation
// and scale (both scaled by kPoseUnit, so +-4). flags is the game's own bone
// flag byte, passed on unread. skeleton names the skeleton (bone count and a
// check of its layout); a puppet with another skeleton ignores the message.
// A piece in Link's hands (the slingshot, the bow) goes as a pose of its own
// skeleton, every bone, and is written onto the piece of that skeleton.
inline constexpr std::size_t kMaxPoseBones = 512;
inline constexpr float kPoseUnit = 8192.0f;

namespace pose_part {
inline constexpr std::uint8_t rotation = 1u << 0;
inline constexpr std::uint8_t translation = 1u << 1;
inline constexpr std::uint8_t scale = 1u << 2;
}  // namespace pose_part

struct PoseBone {
  std::uint16_t bone = 0;
  std::uint8_t flags = 0;
  std::optional<std::array<std::int16_t, 4>> rotation;
  std::optional<std::array<std::int16_t, 3>> translation;
  std::optional<std::array<std::int16_t, 3>> scale;
  bool operator==(const PoseBone&) const = default;
};

struct PlayerPose {
  std::uint8_t player_number = kPlayerSelf;
  SampleTime time;
  std::uint32_t skeleton = 0;
  std::vector<PoseBone> bones;
  bool operator==(const PlayerPose&) const = default;
};

// TIME_OF_DAY (docs/time.md): a game's clock, in seconds since midnight.
// The host sends it about once a second; any player whose clock jumped (the
// Sun's Song, a night's sleep, an event) sends it with `jump`, and every game
// takes a jump at once. Relayed to everyone else.
inline constexpr std::uint32_t kSecondsPerDay = 86'400;
namespace time_of_day {
inline constexpr std::uint8_t jump = 1u << 0;
}  // namespace time_of_day

// OCARINA_NOTE (issue 152): a note the sender's Link starts or stops on the
// Ocarina, relayed to everyone else, who play it on that player's puppet.
// `note` 0: the note held so far was let go; 1 to kOcarinaNotes: the game's
// own note table (OcarinaMgr + 0x40, the SLink assets Ocarina_LowB ...
// Ocarina_HighF in that order), plus one.
inline constexpr std::uint8_t kOcarinaNotes = 19;
struct OcarinaNote {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  std::uint8_t note = 0;                     // 0 to kOcarinaNotes
  bool operator==(const OcarinaNote&) const = default;
};

// LINK_SOUND (500, issues 131 and 152): a sound the game itself started on the
// sender's own Link (its SLink user), by the asset's name ("Pl_Voice_Attack",
// "Ocarina_D"), relayed to everyone else, who start the same asset on that
// player's Link with the game's own call. The name is the game's: letters,
// digits and '_', 1 to kMaxLinkSoundChars.
inline constexpr std::size_t kMaxLinkSoundChars = 55;
// PLAYER_LOOK (430): the colours a player chose for the parts of its Link
// (docs/protocol.md, "Looks"; hookshot/src/coop_look.h numbers the parts),
// sent when it changes and again when a player joins; relayed to everyone
// else. Only the set parts are listed, each once, by number (0 to
// kMaxLookParts - 1: a part a build does not know is passed over by it).
inline constexpr std::size_t kMaxLookParts = 64;
struct LookPart {
  std::uint8_t part = 0;
  std::uint32_t colour = 0;   // 0xRRGGBB
  bool operator==(const LookPart&) const = default;
};
struct PlayerLook {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  std::vector<LookPart> parts;
  bool operator==(const PlayerLook&) const = default;
};

struct LinkSound {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  std::string name;                          // the asset's name
  bool operator==(const LinkSound&) const = default;
};
inline bool valid_link_sound_name(std::string_view name) {
  if (name.empty() || name.size() > kMaxLinkSoundChars) return false;
  for (const char ch : name) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
    if (!ok) return false;
  }
  return true;
}

struct TimeOfDay {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  std::uint32_t seconds = 0;                 // 0 to kSecondsPerDay - 1
  std::uint8_t flags = 0;                    // time_of_day bits
  bool operator==(const TimeOfDay&) const = default;
};

// OBJECT_GROUP (client only, not relayed): which objects belong together, so
// the server keeps them with one owner (docs/objects.md "Groups"). Each entry
// names a member and its root: a boss part and the boss, a summoned enemy and
// its summoner. An entry whose root is the member itself takes the member
// out of any group; it is also how a lone object is marked a boss.
namespace object_group {
inline constexpr std::uint8_t boss = 1u << 0;  // the member is a boss: 60 s fight lock
}  // namespace object_group

struct ObjectGroupEntry {
  ObjectId member;
  ObjectId root;
  std::uint8_t flags = 0;  // object_group bits
  bool operator==(const ObjectGroupEntry&) const = default;
};

// FISH_LURE (client only, not relayed; issue 151, docs/objects.md "Fish and
// lures"): every fish the sender has loaded and how near the sender's own cast
// lure is to it. It replaces the sender's previous list. The server gives each
// fish to the game whose lure is nearest (a hooked fish stays with its owner),
// and a fish is never moved by the distance rule or held by a fight lock.
namespace fish_lure {
inline constexpr std::uint8_t hooked = 1u << 0;  // the sender owns it and it is on its lure
inline constexpr std::uint8_t drawn = 1u << 1;   // it swims at the sender's lure
}  // namespace fish_lure

// distance_cm when no lure of the sender's is in reach.
inline constexpr std::uint16_t kFishLureNone = 0xffff;
// Most fish in one FISH_LURE. 0 to this many, no fish twice.
inline constexpr std::size_t kMaxFishLures = 256;

struct FishLureEntry {
  ObjectId id;
  std::uint16_t distance_cm = kFishLureNone;  // fish to our nearest lure
  std::uint8_t flags = 0;                     // fish_lure bits
  bool operator==(const FishLureEntry&) const = default;
};

struct FishLure {
  std::uint8_t player_number = kPlayerSelf;
  std::vector<FishLureEntry> fish;
  bool operator==(const FishLure&) const = default;
};

// Most entries in one OBJECT_GROUP. 1 to this many, no member twice.
inline constexpr std::size_t kMaxGroupEntries = 256;

struct ObjectGroup {
  std::uint8_t player_number = kPlayerSelf;
  std::vector<ObjectGroupEntry> entries;
  bool operator==(const ObjectGroup&) const = default;
};

// GRASS_CUT (docs/world.md, "Field grass"): one cut of the field grass the
// game's foliage module draws (no actor), as the game queued it: a sphere at
// position with radius, and for a sword, three points of the swing (NaN when
// the cut is a sphere only). flags is the game's byte, passed on unread.
// Every game in the same scene makes the same cut. Relayed to everyone else.
inline constexpr float kMaxGrassCutRadius = 100.0f;
// Cuts the server keeps for one scene, for a player who enters it later.
inline constexpr std::size_t kMaxGrassCuts = 512;

struct GrassCut {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  std::string scene;                         // the sender's scene, not empty
  Vec3 position{};
  float radius = 0.0f;                       // above 0, at most kMaxGrassCutRadius
  std::uint8_t flags = 0;
  std::array<Vec3, 3> sweep{};               // each value finite or NaN
  bool operator==(const GrassCut& o) const;  // NaN equals NaN here
};

// WORLD_BREAK (docs/world.md, "Broken for everyone"): the placed breakable
// with this placement hash was broken in the sender's scene (a pot, a crate,
// a rock, a grass tuft, a cracked wall). The server keeps each scene's
// breaks while anyone is in it and sends them to a player who enters, so a
// pot broken before a player came is gone for that player too. With
// `restored` the game put the object back (a pot that respawns): the break
// is forgotten. `kept` a break that lasts (a small-key door's lock, which the
// opener's game saves): the server keeps it for the session, also while
// nobody is in the scene, and passes on a second player's break of it too
// (both opened it: one key is given back, docs/dungeon.md "Locked doors").
namespace world_break {
inline constexpr std::uint8_t restored = 1u << 0;
inline constexpr std::uint8_t kept = 1u << 1;
}  // namespace world_break
inline constexpr std::size_t kMaxWorldKept = 256;        // kept breaks per scene
inline constexpr std::size_t kMaxWorldKeptTotal = 2048;  // and in all (the game has 98 locks)
inline constexpr std::size_t kMaxWorldBreaks = 1024;  // kept per scene

struct WorldBreak {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  std::string scene;                         // the sender's scene, not empty
  std::uint64_t hash = 0;                    // placement hash, not 0 and not ~0
  Vec3 position{};                           // where it was (finite)
  std::uint8_t flags = 0;                    // world_break bits
  bool operator==(const WorldBreak&) const = default;
};

// WORLD_MOVE: where the sender's game has a placed object it is moving (a
// push block, a pot or rock in Link's hands or in flight), about 20 a
// second while it moves. Unreliable, relayed to the sender's scene, not kept.
namespace world_move {
inline constexpr std::uint8_t carried = 1u << 0;  // in the mover's hands
}  // namespace world_move

struct WorldMove {
  std::uint8_t player_number = kPlayerSelf;
  std::string scene;
  std::uint64_t hash = 0;
  std::uint64_t time_us = 0;  // server time of the reading
  Vec3 position{};
  Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};  // unit
  std::uint8_t flags = 0;                  // world_move bits
  bool operator==(const WorldMove&) const = default;
};

// WORLD_PLACE: the object has come to rest here after a move. Reliable; the
// server keeps the latest one of each object per scene (while anyone is in
// the scene) for a player who enters later.
inline constexpr std::size_t kMaxWorldPlaces = 256;  // kept per scene

struct WorldPlace {
  std::uint8_t player_number = kPlayerSelf;
  std::string scene;
  std::uint64_t hash = 0;
  Vec3 position{};
  Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
  bool operator==(const WorldPlace&) const = default;
};


// WORLD_HIT (docs/world.md, "Signs"): the sender's sword swing passed the
// placed object with this hash (a sign: its life never drops, the cut is
// made by the game's destruction of the pieces the sword met). Each other
// game gives its own copy the same hit, at the same place, through the
// game's own damage call. The server keeps the last kMaxWorldHitsPerObject
// of each object per scene for a player who enters later.
inline constexpr std::size_t kMaxWorldHitsPerObject = 4;
inline constexpr std::size_t kMaxWorldHitObjects = 128;  // per scene

struct WorldHit {
  std::uint8_t player_number = kPlayerSelf;
  std::string scene;
  std::uint64_t hash = 0;
  Vec3 at{};    // where the sword was (finite)
  Vec3 from{};  // where the player who swung stood (finite)
  bool operator==(const WorldHit&) const = default;
};

// SIGN_CUT (docs/world.md, "Signs: the cut words"): the words of a sign's
// components that the sender's own sword cut changed (the sign probe saw
// them), with their new values. A replayed hit gives the others' sign the
// hit reaction but not the cut, which the blade's contact makes: so the
// cut itself is written. A key is (2 + component table entry) << 32 | byte
// offset in that component (as SignProbe::Where), an entry below
// kSignCutEntries and an offset below kSignCutBytes, a multiple of 8. The
// server keeps the latest of each sign per scene for a player who enters
// later.
inline constexpr std::size_t kMaxSignCutWords = 48;
inline constexpr std::uint64_t kSignCutEntries = 0x60;  // table +0..+0x300
inline constexpr std::uint64_t kSignCutBytes = 0x200;   // of each component

struct SignWord {
  std::uint64_t key = 0;
  std::uint64_t value = 0;
  bool operator==(const SignWord&) const = default;
};

struct SignCut {
  std::uint8_t player_number = kPlayerSelf;
  std::string scene;
  std::uint64_t hash = 0;
  std::vector<SignWord> words;  // 1 to kMaxSignCutWords, keys as above
  bool operator==(const SignCut&) const = default;
};

// Shared drops (docs/objects.md, "Drops"): the rupees, hearts and ammo an
// enemy, a pot, grass or a rock leaves. Every game announces the drops its
// own game rolls with DROP_SPAWN; where two games rolled for the same thing
// (near in place and time) the one from the owner of what dropped it stays,
// then the one from the lower player number. Relayed to everyone else.
namespace drop_spawn {
inline constexpr std::uint8_t owned = 1u << 0;  // the sender owns what dropped it
inline constexpr std::uint8_t again = 1u << 1;  // told again for a player who came later
}  // namespace drop_spawn

struct DropSpawn {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 16 relayed
  ObjectId id;             // kind made: the game that rolled it and its counter
  std::string scene;       // 1 to 64 characters
  std::string actor;       // the game's actor name, 1 to 64 characters
  Vec3 position{};
  std::uint64_t time_us = 0;  // server time when its maker first saw it
  std::uint8_t flags = 0;     // drop_spawn bits
  bool operator==(const DropSpawn&) const = default;
};

// DROP_TAKE: a drop is gone from the sender's game; every game removes it.
// The id is a made drop's, or (protocol 10) a placed one's: kind placed and
// the placement hash, for the loot a bush or grass tuft has in every game.
enum class DropTakeReason : std::uint8_t {
  picked_up = 0,  // the sender's Link collected it
  gone = 1,       // it blinked out
};

struct DropTake {
  std::uint8_t player_number = kPlayerSelf;
  ObjectId id;
  DropTakeReason reason = DropTakeReason::picked_up;
  bool operator==(const DropTake&) const = default;
};

// DROP_ASK: the sender just came into this scene; every game there tells
// the drops it still has again (DROP_SPAWN with `again`).
struct DropAsk {
  std::uint8_t player_number = kPlayerSelf;
  std::string scene;  // 1 to 64 characters
  bool operator==(const DropAsk&) const = default;
};

// BOSS_ROOM (docs/dungeons-boss.md): where the sender stands with a dungeon's
// boss. `in_room` is sent on entering and every 2 s while there, so a game
// in the same dungeon (one that joined or respawned too) moves its player in;
// the intro's start and end let the others leave the intro together; `left`
// and `defeated` end the pull. Relayed to everyone else. `defeated` is no
// proof of a death (also sent on a guess): G2 takes BOSS_SCENE's outro.
enum class BossState : std::uint8_t {
  in_room = 1,
  intro_start = 2,
  intro_end = 3,
  left = 4,
  defeated = 5,
};
inline constexpr std::uint8_t kMaxBossRoom = 15;  // the room index in coop_boss.h's table

struct BossRoom {
  std::uint8_t player_number = kPlayerSelf;
  std::string scene;  // the dungeon, 1 to 64 characters
  std::uint8_t room = 0;  // 0 to kMaxBossRoom
  BossState state = BossState::in_room;
  bool operator==(const BossRoom&) const = default;
};

// PvP (protocol 10; docs/protocol.md, "PvP"). Off unless the host turns it
// on; the server's value (set by the host) is the whole session's.

// Most life one PvP hit may take, and most life a PVP_HURT may report:
// quarter hearts (20 hearts).
inline constexpr std::int32_t kMaxPvpLife = 80;

// PVP_MODE: whether players hurt each other in this session. From the server
// to each player when it joins and to everyone on a change: `set_by` is 0
// when the server's start decided it (coop_server --pvp), else the player
// (the host) who changed it. From a player: the host asks for a change
// (`set_by` 0; the server ignores it from anyone but the host).
struct PvpMode {
  std::uint8_t set_by = 0;  // 0 the server, 1 to 4 a player
  bool on = false;
  bool operator==(const PvpMode&) const = default;
};

// DIFFICULTY (protocol 11): how hard the enemies are in this session
// (difficulty.hpp). From the server to each player when it joins and to
// everyone whenever the values in force change: `values` are in force, and
// `set_by` is 0 while nobody has chosen (the automatic scaler), else the
// host who chose. From a player: the host's choice (`set_by` 0; `values` are
// the sliders, read only with the custom preset); the server ignores it from
// anyone but the host and answers with what is in force.
struct Difficulty {
  std::uint8_t set_by = 0;  // 0 nobody, else a player
  bool auto_scale = true;   // the values follow the number of players
  DifficultyPreset preset = DifficultyPreset::normal;  // when not automatic
  DifficultyValues values;  // each within its slider's ends
  bool operator==(const Difficulty&) const = default;
};

// PVP_HIT: the sender's attack reached another player's Link on the sender's
// screen. The server passes it to the victim only, and only while PvP is on;
// the victim's game decides whether it hurts (PVP_HURT).
struct PvpHit {
  std::uint8_t player_number = kPlayerSelf;  // the attacker
  std::uint8_t victim = 0;                   // 1 to 4, not the attacker
  std::uint8_t cause = 0;                    // DamageCause; others kept
  std::int32_t amount = 0;                   // quarter hearts, 1 to kMaxPvpLife
  Vec3 at{};    // where the blow landed
  Vec3 from{};  // where the attacker stood
  bool operator==(const PvpHit&) const = default;
};

// PVP_HURT flags.
namespace pvp_hurt {
inline constexpr std::uint8_t died = 1;     // the hit took the last of the life
inline constexpr std::uint8_t guarded = 2;  // not taken (amount 0): a cutscene, a menu, ...
inline constexpr std::uint8_t known = died | guarded;
}  // namespace pvp_hurt

// PVP_HURT: what a PVP_HIT did in the victim's game (the victim decides).
// Relayed to everyone else, so the attacker's screen shows it.
struct PvpHurt {
  std::uint8_t player_number = kPlayerSelf;  // the victim
  std::uint8_t attacker = 0;                 // 1 to 4, not the victim
  std::int32_t amount = 0;                   // taken: 0 to kMaxPvpLife
  std::int32_t life = 0;                     // left after it: 0 to kMaxPvpLife
  std::uint8_t flags = 0;                    // pvp_hurt; other bits refused
  bool operator==(const PvpHurt&) const = default;
};

// DUNGEON_EVENT (protocol 10, deku-dungeon-2, issues 176 and 196): a room
// gimmick's event (a switch's cutscene, as InsideTheDekuTree_Gimmick_RoomD_PlatformUp)
// started in the sender's game. The others in that scene start the same
// event at once, so its result (platforms up, a ladder down) runs at the same
// time in every game, not only after the sender's cutscene ended and its
// played flag came (docs/dungeon.md, "Live events"). Relayed to the scene,
// never kept.
inline constexpr std::size_t kMaxEventNameChars = 96;

// PLATFORM_CLOCK (protocol 10, branch platforms-b, issues 196 and 194): the
// clock word of each moving platform's driver the sender owns, read at
// `server_time_us` (server time). The others put value + rate x (their
// server time now - server_time_us) into their own word, looped by `cycle`
// when it is over 0 (docs/objects.md, "Moving platforms"). On channel 4 like
// WORLD_MOVE, unreliable: the next one comes a second later. Relayed to the
// sender's scene, never kept.
inline constexpr std::size_t kMaxPlatformTicks = 16;
inline constexpr std::uint64_t kPlatformAsFrameRegion = 0xffff;  // key region: the AS slot 0 frame
// #196: key regions of a mover's GroundControl node (lichii/hook/coop_platforms.h, GroundNode): the wave
// controller's and the rotate controller's, offset kPlatformNodeFrom .. kPlatformNodeTo in the node.
inline constexpr std::uint64_t kPlatformWaveNodeRegion = 0xfffe;
inline constexpr std::uint64_t kPlatformRotateNodeRegion = 0xfffd;
inline constexpr std::uint64_t kPlatformNodeFrom = 0xc0;
inline constexpr std::uint64_t kPlatformNodeTo = 0x140;
inline constexpr std::uint64_t kPlatformActorBytes = 0x600;
inline constexpr std::uint64_t kPlatformComponentBytes = 0x200;
inline constexpr std::uint64_t kPlatformComponents = 0x300 / 8;  // table entries
inline constexpr float kMaxPlatformRate = 10000.0f;
inline constexpr float kMaxPlatformValue = 1.0e7f;

struct PlatformTick {
  std::uint64_t hash = 0;  // the driver's placement hash
  // Where the word is: region << 32 | byte offset (a multiple of 4).
  // Region 0 the actor (offset under 0x600), 2 + n the component in table
  // entry n (under 0x200), kPlatformAsFrameRegion the AS slot 0 frame (offset 0), kPlatformWaveNodeRegion /
  // kPlatformRotateNodeRegion a word of the mover's GroundControl node (kPlatformNodeFrom .. kPlatformNodeTo).
  std::uint64_t key = 0;
  float value = 0.0f;
  float rate = 0.0f;   // per second, not 0
  float cycle = 0.0f;  // where it loops back; 0 when not seen
  bool operator==(const PlatformTick&) const = default;
};

struct PlatformClock {
  std::uint8_t player_number = kPlayerSelf;
  std::string scene;
  std::uint64_t server_time_us = 0;
  std::vector<PlatformTick> ticks;  // 1 to kMaxPlatformTicks, no hash twice
  bool operator==(const PlatformClock&) const = default;
};

// PLATFORM_COUNTER (protocol 10, branch platforms-b2): the looping pause
// counters (AI ExecuteGenericCounter nodes) of the platforms near the
// sender, each with the time since its last pause began, read at
// `server_time_us`. A game that has seen fewer starts of the same key
// shifts its own pause once to be in phase (docs/objects.md, "Moving
// platforms: the pause counters"). Channel 4, unreliable, every 3 s;
// relayed to the sender's scene, never kept.
inline constexpr std::size_t kMaxPlatformCounters = 32;
inline constexpr std::uint32_t kMinCounterPeriodUs = 100'000;

struct CounterTick {
  std::uint64_t key = 0;        // hash of the actor's placement, the pause's length and its place in the graph
  std::uint32_t period_us = 0;  // between two starts; at least kMinCounterPeriodUs
  std::uint32_t since_us = 0;   // since the last start
  std::uint32_t starts = 0;     // starts the sender has seen, at least 1
  bool operator==(const CounterTick&) const = default;
};

struct PlatformCounter {
  std::uint8_t player_number = kPlayerSelf;
  std::string scene;
  std::uint64_t server_time_us = 0;
  std::vector<CounterTick> ticks;  // 1 to kMaxPlatformCounters, no key twice
  bool operator==(const PlatformCounter&) const = default;
};

struct DungeonEvent {
  std::uint8_t player_number = kPlayerSelf;
  std::string scene;
  std::string name;  // the event's name, 1 to kMaxEventNameChars, no "HavePlayedEvent."
  bool operator==(const DungeonEvent&) const = default;
};

// SWITCH_STATE (room-objects; docs/protocol.md, "Switch states"): one of the sender's room switches (a timed step, eye
// or hit switch whose output a room object reads) turned an output plug on or off by its own player's press. A game in
// the same scene holds the same plug on its own switch while the sender's is on, so its own timers, gates and movers
// run (lichii/hook/coop_switch_share.h). Reliable, relayed to the sender's scene, never kept; sent again every 2 s
// while on, so a player who comes in late gets it.
inline constexpr std::size_t kMaxSwitchNameChars = 64;

struct SwitchState {
  std::uint8_t player_number = kPlayerSelf;
  std::string scene;
  std::string name;        // the switch's actor name, 1 to kMaxSwitchNameChars
  Vec3 place{};            // its placement (the romfs's), finite
  std::uint32_t plug = 0;  // Murmur3 of the output plug's name (OnState, OffState, FirstOn, ...), not 0
  bool on = false;
  bool operator==(const SwitchState&) const = default;
};

// Down but not out (protocol 11; docs/protocol.md, "Down but not out"): a
// player whose hearts run out is downed, not dead, and a teammate can revive
// them. On unless the host turns it off.

// The host's rules, as DBNO_MODE carries them. The limits are the codec's.
inline constexpr std::uint8_t kDbnoMinBleedS = 10;       // bleed-out time, seconds
inline constexpr std::uint8_t kDbnoMaxBleedS = 240;
inline constexpr std::uint8_t kDbnoMinReviveTenths = 5;  // how long a revive is held, tenths of a second
inline constexpr std::uint8_t kDbnoMaxReviveTenths = 100;
inline constexpr std::uint8_t kDbnoMinReviveLife = 1;    // life a revived player gets, quarter hearts
inline constexpr std::uint8_t kDbnoMaxReviveLife = 80;

// DBNO_MODE: the session's rules. From the server to each player when it
// joins and to everyone on a change: `set_by` is 0 when the server's start
// decided them, else the player (the host) who changed them. From a player:
// the host asks for a change (`set_by` 0; the server takes it from nobody
// but the host).
struct DbnoMode {
  std::uint8_t set_by = 0;         // 0 the server, 1 to kMaxPlayers a player
  bool on = true;
  std::uint8_t bleed_s = 45;       // kDbnoMinBleedS to kDbnoMaxBleedS
  std::uint8_t revive_tenths = 50; // kDbnoMinReviveTenths to kDbnoMaxReviveTenths
  std::uint8_t revive_life = 12;   // kDbnoMinReviveLife to kDbnoMaxReviveLife
  bool operator==(const DbnoMode&) const = default;
};

// DBNO_STATE states.
enum class DbnoState : std::uint8_t {
  up = 0,
  downed = 1,  // crawling, bleeding out, can be revived
  out = 2,     // nobody was left standing: the game's own death (Game Over) runs
  // Not revived in time: watches a player still up until no enemy is near
  // the players still up, then comes back beside the one watched.
  spectating = 3,
};

// Most bleed-out time a DBNO_STATE may carry, tenths of a second.
inline constexpr std::uint16_t kDbnoMaxLeftTenths = 10 * kDbnoMaxBleedS;

// DBNO_STATE: the sender's own state, as its game decided it. Relayed to
// everyone else; the server keeps the state for its own rules (a downed
// player's enemies go to a player still up) and nothing else.
struct DbnoStatus {
  std::uint8_t player_number = kPlayerSelf;
  DbnoState state = DbnoState::up;
  std::uint16_t left_tenths = 0;  // downed: bleed-out time left; else 0
  std::uint8_t reviver = 0;       // downed: who is reviving (1 to kMaxPlayers, not the player); 0 nobody
  std::uint8_t progress = 0;      // downed with a reviver: 0 to 255 of the revive; else 0
  bool operator==(const DbnoStatus&) const = default;
};

// DBNO_REVIVE: the sender holds (or let go of) the revive button beside a
// downed player on the sender's screen. The server passes it to that player
// only, and only while the mode is on; the downed player's game decides.
struct DbnoRevive {
  std::uint8_t player_number = kPlayerSelf;  // the reviver
  std::uint8_t target = 0;                   // 1 to kMaxPlayers, not the reviver
  bool holding = false;
  bool operator==(const DbnoRevive&) const = default;
};

// DBNO_THREAT: whether an enemy is near the sender's Link, as the sender's
// own game sees its enemies. Sent by a player who is up, when it changes and
// again every few seconds; relayed to everyone else. A spectating player
// comes back when every player still up says "none near" (or the one it
// watches does, when the others are elsewhere).
struct DbnoThreat {
  std::uint8_t player_number = kPlayerSelf;
  bool enemy_near = false;
  bool operator==(const DbnoThreat&) const = default;
};

// BOSS_SCENE (boss-cutscenes, docs/dungeons-boss.md "Boss cutscenes"): the
// sender's game started or ended a boss's intro or death cutscene. Relayed to
// everyone else; each game decides whether it is in the boss's area (the
// boss room and its antechamber) and plays it too, or only says so in chat.
enum class BossSceneKind : std::uint8_t {
  intro_start = 1,  // the sender's game took the boss's intro event (it is its starter)
  intro_end = 2,    // the starter's intro ended (played out or skipped): every intro ends
  outro_start = 3,  // the sender's game took the boss's death event
  outro_end = 4,
};
inline constexpr std::uint8_t kMaxSceneBoss = 15;  // CoopBossScenes::Bosses() index

struct BossScene {
  std::uint8_t player_number = kPlayerSelf;
  std::string scene;      // the sender's scene, 1 to 64 characters
  std::uint8_t boss = 0;  // 0 to kMaxSceneBoss
  BossSceneKind kind = BossSceneKind::intro_start;
  Vec3 position{};        // the sender's Link when it sent (finite)
  bool operator==(const BossScene&) const = default;
};

// BOSS_HEART: one shared heart container per boss. A client sends a claim
// (given false) when its own game's maximum life rose in a beaten boss's
// area; `max_life` is that new maximum. The server takes the first claim of
// each boss as the winner and sends given true, player_number the winner,
// to everyone (the sender too); a later claim of that boss is answered to
// its sender alone with the same winner and value. Every game raises its
// maximum to max_life (never lowers it), so the container gives one +1 to all.
inline constexpr std::int32_t kMaxBossHeartLife = 80;  // 20 hearts of 4 quarters

struct BossHeart {
  std::uint8_t player_number = kPlayerSelf;  // claim: the claimer; given: the winner
  std::uint8_t boss = 0;                     // 0 to kMaxSceneBoss
  bool given = false;                        // false: a claim (client); true: the server's answer
  std::int32_t max_life = 0;                 // 1 to kMaxBossHeartLife
  bool operator==(const BossHeart&) const = default;
};

// BOSS_ALIVE (row 159): the host's save load said which bosses it holds alive. The host sends it once after
// its FLAG_RESET, on the same channel (so it comes after it), when its game reads the bosses' quests again. Bit
// N of `known` is boss index N (CoopBossBeaten::Sets(): 0 to 7 the dungeon bosses, 8 Ganondorf, 9 Ganon) whose
// quest the host read; bit N of `alive` (a subset of `known`): that quest is before the boss's death in the
// host's save. A guest rewinds a boss whose quest the host does not set for it (each game's own) and that its
// own game holds just beaten. The server takes it from the host only. Older games drop the unknown type.
inline constexpr std::uint16_t kBossAliveMask = 0x03ff;  // ten bosses

struct BossAlive {
  std::uint8_t player_number = kPlayerSelf;  // the host
  std::uint16_t known = 0;                   // within kBossAliveMask
  std::uint16_t alive = 0;                   // within known
  bool operator==(const BossAlive&) const = default;
};

// VOICE_FRAME (protocol 11, branch proximity-voip): 20 ms of the sender's
// microphone as one Opus packet (48 kHz mono), or an empty packet with
// voice::end that closes a talk spurt (the gate closed). Channel 5,
// unreliable. The server writes the sender's number and relays it to the
// players in the sender's scene whose Link is within the voice range of the
// sender's (docs/protocol.md, "Voice"); never kept.
// A ceiling, not the size: at 24 kbit/s a frame is about 60 bytes.
inline constexpr std::size_t kMaxVoiceBytes = 400;
namespace voice {
inline constexpr std::uint8_t end = 1u << 0;  // the talk spurt ends here; no audio
}  // namespace voice

struct VoiceFrame {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, the sender relayed
  std::uint16_t sequence = 0;                // +1 per 20 ms frame, wraps
  std::uint8_t flags = 0;                    // voice::end
  std::vector<std::uint8_t> opus;            // 1 to kMaxVoiceBytes; empty with voice::end
  bool operator==(const VoiceFrame&) const = default;
};

// Mods (protocol 11; docs/protocol.md "Mods", docs/mods.md). Every game
// tells the others which mods it runs, so a player with other mods than the
// host's is warned; the host's "shared" mod settings (a randomizer's seed)
// are everyone's.
inline constexpr std::size_t kMaxModEntries = 32;
inline constexpr std::size_t kMaxModIdChars = 48;
inline constexpr std::size_t kMaxModVersionChars = 32;
inline constexpr std::size_t kMaxModSettingEntries = 32;
inline constexpr std::size_t kMaxModSettingKeyChars = 32;
inline constexpr std::size_t kMaxModSettingValueChars = 256;
using ModHash = std::array<std::uint8_t, 16>;

struct ModEntry {
  std::string id;       // 1 to kMaxModIdChars, [a-z0-9._-]
  std::string version;  // 0 to kMaxModVersionChars
  ModHash hash{};       // the mod's game files and code (all zero: not known yet)
  bool operator==(const ModEntry&) const = default;
};

// MOD_LIST: the mods a game runs that every player needs alike (mod.json
// "coop" not "own"), in its load order. Sent once per connection, when the
// game has hashed its mods; the server writes the sender's number, keeps the
// last one of each player and gives a joiner everyone else's.
struct ModList {
  std::uint8_t player_number = kPlayerSelf;  // 0 from the sender, 1 to 16 relayed
  std::vector<ModEntry> mods;                // 0 to kMaxModEntries, ids unique
  bool operator==(const ModList&) const = default;
};

struct ModSetting {
  std::string mod_id;  // as ModEntry::id
  std::string key;     // 1 to kMaxModSettingKeyChars, [a-z0-9_]
  std::string value;   // 0 to kMaxModSettingValueChars
  bool operator==(const ModSetting&) const = default;
};

// MOD_SETTINGS: the host's values of its mods' "shared" settings, all of them
// each time. Taken from the host only (anyone else's is dropped); the server
// keeps the last one, relays it to the others and gives it to each joiner.
// A new host's own values count from its first MOD_SETTINGS on.
struct ModSettings {
  std::uint8_t player_number = kPlayerSelf;  // 0 from the host, its number relayed
  std::vector<ModSetting> settings;          // 0 to kMaxModSettingEntries
  bool operator==(const ModSettings&) const = default;
};

// The Lobby (protocol 11, docs/protocol.md "Lobby"): the host's moderation,
// all of it carried out by the server, which takes a LOBBY_ACTION from the
// host only. A player's own (local) mutes never reach the wire.
enum class LobbyActionKind : std::uint8_t {
  kick = 1,          // out now; may join again
  ban = 2,           // out, and the name and address refused until the session ends
  mute_voice = 3,    // the server stops relaying the player's VOICE_FRAME
  unmute_voice = 4,
  mute_chat = 5,     // the server drops the player's CHAT
  unmute_chat = 6,
};
inline constexpr std::uint8_t kMaxLobbyAction = 6;

// LOBBY_ACTION, the host to the server: do `action` to player `target`.
struct LobbyAction {
  LobbyActionKind action = LobbyActionKind::kick;
  std::uint8_t target = 1;  // 1 to kMaxPlayers
  bool operator==(const LobbyAction&) const = default;
};

// LOBBY_MUTES bits.
namespace lobby_mute {
inline constexpr std::uint8_t voice = 1u << 0;
inline constexpr std::uint8_t chat = 1u << 1;
}  // namespace lobby_mute

// LOBBY_MUTES, the server to everyone: what the host has muted of a player
// now (the Lobby shows it). Sent to all on each change and, for each muted
// player, to a joiner after WELCOME. Unknown bits are kept.
struct LobbyMutes {
  std::uint8_t player = 1;  // 1 to kMaxPlayers
  std::uint8_t flags = 0;   // lobby_mute bits
  bool operator==(const LobbyMutes&) const = default;
};

// What came of a LOBBY_ACTION. Raw value kept on decode.
enum class LobbyResultCode : std::uint8_t {
  done = 0,
  not_host = 1,    // only the host may do it
  no_player = 2,   // nobody has that number
  is_host = 3,     // the host cannot do it to itself
  unchanged = 4,   // it was so already (muted twice)
};

// LOBBY_RESULT, the server to the player who sent a LOBBY_ACTION.
struct LobbyResult {
  LobbyActionKind action = LobbyActionKind::kick;
  std::uint8_t target = 1;
  LobbyResultCode result = LobbyResultCode::done;
  bool operator==(const LobbyResult&) const = default;
};

// Host approval (protocol 13, docs/protocol.md "Host approval"): with the
// host's setting on, the server holds a joiner who gave the right join
// password until the host lets it in or says no, or its time runs out (60 s
// by default). A JOIN_KNOCK's wait is at most this.
inline constexpr std::uint16_t kMaxKnockWaitSeconds = 600;

// JOIN_KNOCK flag bits. Unknown bits are kept.
namespace join_knock {
// The host let this player key in before in this session.
inline constexpr std::uint8_t known = 1u << 0;
}  // namespace join_knock

// JOIN_KNOCK, server only. To the host: `nickname` asks to join; answer with
// JOIN_ALLOW and `knock` within `wait_s` seconds. To the held joiner itself
// (before WELCOME, its own name): its join waits for the host for up to
// `wait_s` seconds. `wait_s` 0, to the host only: that knock is over (the
// joiner left, or the time ran out).
struct JoinKnock {
  std::uint32_t knock = 1;          // not 0
  std::uint16_t wait_s = 60;        // 0 to kMaxKnockWaitSeconds
  std::uint8_t flags = 0;           // join_knock bits
  std::string nickname;             // 1 to kMaxNicknameChars, as HELLO's
  bool operator==(const JoinKnock&) const = default;
};

// JOIN_ALLOW, the host to the server: its answer to the knock `knock`.
// remember: allowed, that player key gets in without asking from now on;
// not allowed, it is turned away without asking (as a ban). Both until the
// session ends.
struct JoinAllow {
  std::uint32_t knock = 1;  // not 0
  bool allow = false;
  bool remember = false;
  bool operator==(const JoinAllow&) const = default;
};

// Native puppets phase 2 (docs/native-puppets.md, "Lift a bomb, and stash
// it"; docs/protocol.md, "Bomb hand-over"). BOMB_TAKE (client only): our Link
// lifted the copy of another player's made bomb; give it to us. The server
// asks its owner (HANDOVER_ASK); a yes reaches us as HANDOVER_STATE (the fuse
// left in it), a no as BOMB_ANSWER.
struct BombTake {
  ObjectId id;
  std::uint16_t epoch = 0;  // the owner's epoch as we last had it
  bool operator==(const BombTake&) const = default;
};

// BOMB_ANSWER (server only): why a BOMB_TAKE was refused. Sent only to the
// asker.
enum class BombRefusal : std::uint8_t {
  busy = 1,        // another player's take (or a hand-over) is in flight
  not_a_bomb = 2,  // unknown, not a made Item_Bomb, another scene, or ours already
  stale = 3,       // the epoch is not the owner's now
  kept = 4,        // the owner did not answer (its game keeps the bomb)
};
inline constexpr std::uint8_t kBombRefusalMax = 4;
// BOMB_SHARE (client only): our game takes part in the bomb hand-over (its
// switch is on). Sent after WELCOME and every few seconds; the server asks an
// owner for its bomb, and hands on a bomb whose owner left, only with games
// that said so.
struct BombShare {
  bool on = true;
  bool operator==(const BombShare&) const = default;
};

struct BombAnswer {
  ObjectId id;
  std::uint16_t epoch = 0;  // the owner's epoch now
  BombRefusal refusal = BombRefusal::busy;
  bool operator==(const BombAnswer&) const = default;
};

// Native puppets phase 3 (docs/native-puppets.md, "Lifting another player";
// docs/protocol.md, "Player lift"). The lifted player's game decides; the
// server only passes each message to the one player it names, and only
// between games that sent LIFT_SHARE.
//
// LIFT_ASK: our Link lifted `lifted`'s copy (the game's own lift). To the
// lifted player only; it answers with LIFT_ANSWER.
struct LiftAsk {
  std::uint8_t player_number = kPlayerSelf;  // the lifter
  std::uint8_t lifted = 0;                   // 1 to 4, not the lifter
  bool operator==(const LiftAsk&) const = default;
};

// Why a lift was refused or ended (LIFT_ANSWER).
enum class LiftRefusal : std::uint8_t {
  none = 0,         // taken: the lifter's game drives our place from now on
  busy = 1,         // already lifted, or lifting someone ourselves
  event = 2,        // our Link is in an event (a talk, a cutscene), a load or a menu
  down = 3,         // dead, or down but not out
  not_sharing = 4,  // (the server) the lifted game did not say LIFT_SHARE
  freed = 5,        // (after a yes) the lifted game took its place back
  too_far = 6,      // the lifter's Link is not near ours
  // Owner rule: only a young (child) Link can be lifted. Our Link is adult (or its age not known); after a yes,
  // our Link grew up while lifted (freed the same way). The server says it too, from the last PLAYER_SAMPLE age.
  not_young = 7,
};
inline constexpr std::uint8_t kLiftRefusalMax = 7;

// LIFT_ANSWER: the lifted game's word on a LIFT_ASK (refusal none = yes), or
// `freed` when a lift it took ends on its side. To the lifter only.
// `player_number` is the lifted player (the server writes it, or names the
// asked player in its own not_sharing answer).
struct LiftAnswer {
  std::uint8_t player_number = kPlayerSelf;  // the lifted
  std::uint8_t lifter = 0;                   // 1 to 4, not the lifted
  LiftRefusal refusal = LiftRefusal::none;
  bool operator==(const LiftAnswer&) const = default;
};

// Where the lifter's game has the lifted player (LIFT_PLACE's phase).
enum class LiftPhase : std::uint8_t {
  held = 1,    // in the lifter's hands
  flying = 2,  // thrown (or set down), not at rest yet
  landed = 3,  // at rest: the lifted game owns its place again (a lift's last place)
};
inline constexpr std::uint8_t kLiftPhaseMax = 3;

// LIFT_PLACE: the lifted player's place as the lifter's game shows it, each
// frame of a taken lift; to the lifted player only.
struct LiftPlace {
  std::uint8_t player_number = kPlayerSelf;  // the lifter
  std::uint8_t lifted = 0;                   // 1 to 4, not the lifter
  LiftPhase phase = LiftPhase::held;
  Vec3 position{};
  bool operator==(const LiftPlace&) const = default;
};

// LIFT_SHARE (client only): our game takes part in player lifts (its switch
// is on). Sent after WELCOME and every few seconds, as BOMB_SHARE.
struct LiftShare {
  bool on = true;
  bool operator==(const LiftShare&) const = default;
};

// PvP on real bodies (native puppets phase 4; docs/native-puppets.md section 5,
// docs/protocol.md "PvP on real bodies"). Sent only by games with
// LICHII_COOP_PVP_NATIVE=1 (off by default). The victim's game decides.
//
// The words of a hit's kind as the game keeps them in its incoming hit record
// (Life +0x250 up to +0x27c, CoopHitReplay::kRecKind..kRecKindEnd): plain
// values, never pointers.
inline constexpr std::size_t kPvpKindWords = 11;
using PvpKind = std::array<std::uint32_t, kPvpKindWords>;

// What dealt a strike (PVP_STRIKE's source).
enum class PvpSource : std::uint8_t {
  sword = 1,  // a sword, a stick, the hammer, the body
  shot = 2,   // an arrow, a seed, the hookshot, the boomerang, a nut
  blast = 3,  // a bomb's or bombchu's blast, Din's Fire
  other = 4,
};
inline constexpr std::uint8_t kPvpSourceMax = 4;

// PVP_STRIKE: the sender's game took a hit of its own Link (or of a shot or
// blast it made) on `victim`'s copy, by the game's own collision. To the
// victim only, and only while PvP is on between two PVP_NATIVE games.
struct PvpStrike {
  std::uint8_t player_number = kPlayerSelf;  // the attacker
  std::uint8_t victim = 0;                   // 1 to 16, not the attacker
  std::uint32_t seq = 0;                     // the attacker's own count
  std::uint64_t seen_us = 0;                 // server time of the victim's sample the copy showed
  PvpKind kind{};
  std::int32_t amount = 0;                   // quarter hearts the attacker's game counted (compared, never used)
  PvpSource source = PvpSource::sword;
  ObjectId object{};  // a shot or blast: the attacker's made object; else all zero
  Vec3 at{};      // where the hit landed
  Vec3 copy_at{}; // where the attacker's game showed the victim
  Vec3 from{};    // where the attacker stood
  bool operator==(const PvpStrike&) const = default;
};

// The victim's word on a strike (PVP_RESULT's result).
enum class PvpOutcome : std::uint8_t {
  taken = 0,
  guarded = 1,
  guard_broken = 2,
  invincible = 3,     // the game's invincible frames refused it
  late = 4,           // seen_us too old
  out_of_reach = 5,   // not where the victim was at seen_us
  kept_off = 6,       // an event, a menu, a load, another scene, down or dead
};
inline constexpr std::uint8_t kPvpOutcomeMax = 6;

// PVP_RESULT: the victim's game's word on one strike. To every game in the
// victim's scene: the attacker plays a guard's recoil, the others replay the
// kind on their copy of the victim.
struct PvpResult {
  std::uint8_t player_number = kPlayerSelf;  // the victim
  std::uint8_t attacker = 0;                 // 1 to 16, not the victim
  std::uint32_t seq = 0;                     // the strike's
  PvpOutcome outcome = PvpOutcome::taken;
  std::int32_t amount = 0;                   // taken, 0 to kMaxPvpLife
  std::int32_t life = 0;                     // left, 0 to kMaxPvpLife
  PvpKind kind{};
  Vec3 at{};
  bool operator==(const PvpResult&) const = default;
};

// PVP_NATIVE: the sender's game runs PvP on real bodies. Sent after WELCOME
// and every few seconds; relayed to everyone.
struct PvpNative {
  std::uint8_t player_number = kPlayerSelf;
  bool on = true;
  bool operator==(const PvpNative&) const = default;
};

// Split screen (docs/protocol.md, "Local players"): the extra players of one
// game, seats 1 to kMaxLocalSeats (seat 0 is the connection's own player).
inline constexpr std::uint8_t kMaxLocalSeats = 3;

// LOCAL_JOIN: the game asks for a player number for an extra local player.
struct LocalJoin {
  std::uint8_t seat = 1;
  std::string nickname;  // 1 to kMaxNicknameChars, as HELLO's
  bool operator==(const LocalJoin&) const = default;
};

// LOCAL_JOINED: the answer, to that game only: the seat's number, or 0 and
// why not (server_full, name_taken, bad_name).
struct LocalJoined {
  std::uint8_t seat = 1;
  std::uint8_t player_number = 0;
  RefuseReason reason = RefuseReason::server_full;  // when player_number is 0
  bool operator==(const LocalJoined&) const = default;
};

// LOCAL_LEAVE: the extra local player leaves (the others get PLAYER_LEFT).
struct LocalLeave {
  std::uint8_t seat = 1;
  bool operator==(const LocalLeave&) const = default;
};

// LOCAL_SAMPLE and LOCAL_MESSAGE: one of an extra local player's own
// messages, encoded whole, in an envelope naming its seat. The server takes
// it as that player's (writing its number in the player field). A sample
// carries PLAYER_SAMPLE, PLAYER_POSE or PLAYER_RIDE (on the samples channel);
// a message PLAYER_STATUS, PLAYER_EQUIP, PLAYER_LIFE or OCARINA_NOTE (on the
// session channel). Decoding checks the inner message.
struct LocalSample {
  std::uint8_t seat = 1;
  std::vector<std::uint8_t> inner;
  bool operator==(const LocalSample&) const = default;
};
struct LocalMessage {
  std::uint8_t seat = 1;
  std::vector<std::uint8_t> inner;
  bool operator==(const LocalMessage&) const = default;
};

using Message = std::variant<Hello, Welcome, Refused, PlayerJoined, PlayerLeft,
                             Ping, Pong, PlayerStatus, Chat, PlayerSample,
                             TestPayload, JoinPartner, FlagSet, FlagState,
                             FlagSnapshot, Host, PlayerEquip, ObjectPresent,
                             ObjectOwner, HandoverAsk, HandoverState, ObjectSpawn,
                             ObjectSample, ObjectRemove, Damage, PlayerPose,
                             TimeOfDay, GrassCut, ObjectGroup, WorldBreak, WorldMove,
                             WorldPlace, WorldHit, DropSpawn, DropTake,
                             DropAsk, BossRoom, SignCut, FlagReset, FlagOrigin,
                             JoinChallenge, JoinProof, SaveAsk, SavePart, PlayerLife,
                             PvpMode, PvpHit, PvpHurt, FishLure, RejoinToken, RejoinClaim, DungeonEvent, PlayerRide,
                             OcarinaNote, PlatformClock, PlatformCounter,
                             DbnoMode, DbnoStatus, DbnoRevive, DbnoThreat, Difficulty, VoiceFrame,
                             ModList, ModSettings, LobbyAction, LobbyMutes, LobbyResult,
                             LocalJoin, LocalJoined, LocalLeave, LocalSample, LocalMessage, LinkSound,
                             BossScene, BossHeart, PlayerLook, SwitchState, JoinKnock, JoinAllow,
                             BombTake, BombAnswer, BombShare, LiftAsk, LiftAnswer, LiftPlace,
                             LiftShare, PvpStrike, PvpResult, PvpNative, BossAlive>;

MessageType type_of(const Message& message);

// What an envelope may carry (LOCAL_SAMPLE, LOCAL_MESSAGE).
bool local_sample_carries(MessageType type);
bool local_message_carries(MessageType type);
// An extra local player's message in its envelope (LOCAL_SAMPLE or
// LOCAL_MESSAGE by the inner type); nullopt when no envelope carries it.
std::optional<Message> local_envelope(std::uint8_t seat, const Message& inner);
// The message inside an envelope; nullopt when it is not a LOCAL_SAMPLE or
// LOCAL_MESSAGE, or what it holds does not decode.
std::optional<Message> local_inner(const Message& envelope);

// The channel a message is sent on. TEST_PAYLOAD may go on 0 or 3; this
// returns 3 and the test tool picks for itself.
std::uint8_t channel_of(const Message& message);
std::uint8_t channel_of(MessageType type);

// Whether a message of this type may arrive on this channel. False for a type
// this build does not know.
bool allowed_on_channel(MessageType type, std::uint8_t channel);

// The two ends of a connection.
enum class Sender : std::uint8_t {
  client,
  server,
};

// Whether this side may send this message type. HELLO is client only;
// WELCOME, REFUSED, PLAYER_JOINED, PLAYER_LEFT and HOST are server only; the rest go
// both ways. False for a type this build does not know.
bool may_send(Sender from, MessageType type);

}  // namespace coop::protocol
