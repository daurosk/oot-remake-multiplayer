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
#include <variant>
#include <vector>

namespace coop::protocol {

inline constexpr std::uint16_t kProtocolVersion = 0;
inline constexpr std::uint16_t kDefaultPort = 27510;
inline constexpr std::uint8_t kMaxPlayers = 4;

// Size of the message header: u16 type, u16 length.
inline constexpr std::size_t kHeaderSize = 4;
// Size of a component header: u16 id, u16 length.
inline constexpr std::size_t kComponentHeaderSize = 4;

// Maximum lengths of string fields, in Unicode characters (code points).
// A string may also never be longer than 4 bytes per character, must be valid
// UTF-8 and holds no control characters (U+0000-U+001F, U+007F-U+009F).
inline constexpr std::size_t kMaxBuildIdChars = 64;
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
inline constexpr std::uint8_t kChannelCount = 5;

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
// (PlayerStatus.Equip*), 9 the drawn bits.
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
  std::vector<PlayerInfo> players;  // the players already in, at most 4
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
};

struct Refused {
  RefuseReason reason = RefuseReason::wrong_protocol_version;
  bool operator==(const Refused&) const = default;
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
}  // namespace status

struct PlayerStatus {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 4 relayed
  std::uint8_t flags = 0;
  bool operator==(const PlayerStatus&) const = default;
};

struct Chat {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 4 relayed
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

// PLAYER_SAMPLE. The time component is always sent; the others only when the
// sender has them. Floats must be finite.
struct PlayerSample {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 4 relayed
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
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 4 relayed
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
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 4 relayed
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
  std::optional<std::string> hand;     // gauntlets
  std::optional<std::string> ocarina;
  std::optional<std::string> arrow;    // arrow kind for the bow
  // Bit i set: the game's equipment slot i is drawn (in hand), not sheathed.
  // Slots 0 to 7; see docs/puppet.md for which slot is which.
  std::optional<std::uint8_t> drawn;
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
  return e;
}

// PLAYER_EQUIP: a player's equipment, sent reliably on every change and again
// when another player joins. Same player field rules as PLAYER_SAMPLE.
struct PlayerEquip {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 4 relayed
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
}  // namespace object_state

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
  bool operator==(const ObjectState&) const = default;
};

namespace object_presence {
inline constexpr std::uint8_t awake = 1u << 0;
inline constexpr std::uint8_t in_fight = 1u << 1;
inline constexpr std::uint8_t targeting = 1u << 2;  // targeting our player
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
};

// DAMAGE: life lost on a copy, for the owner to take off. Amount 0 or more.
struct Damage {
  std::uint8_t player_number = kPlayerSelf;
  ObjectId id;
  std::uint16_t epoch = 0;  // the epoch the hitter knew
  std::int32_t amount = 0;
  std::uint8_t cause = 0;  // DamageCause
  Vec3 position{};
  bool operator==(const Damage&) const = default;
};

// PLAYER_POSE: the sender's Link skeleton after its animation, for the
// puppet to copy bone by bone (docs/puppet.md, "Pose"). Only the bones that
// move are listed, in increasing bone order, each with the parts it carries:
// the local rotation (quaternion x, y, z, w scaled by 32767), translation
// and scale (both scaled by kPoseUnit, so +-4). flags is the game's own bone
// flag byte, passed on unread. skeleton names the skeleton (bone count and a
// check of its layout); a puppet with another skeleton ignores the message.
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

struct TimeOfDay {
  std::uint8_t player_number = kPlayerSelf;  // 0 from a client, 1 to 4 relayed
  std::uint32_t seconds = 0;                 // 0 to kSecondsPerDay - 1
  std::uint8_t flags = 0;                    // time_of_day bits
  bool operator==(const TimeOfDay&) const = default;
};

using Message = std::variant<Hello, Welcome, Refused, PlayerJoined, PlayerLeft,
                             Ping, Pong, PlayerStatus, Chat, PlayerSample,
                             TestPayload, JoinPartner, FlagSet, FlagState,
                             FlagSnapshot, Host, PlayerEquip, ObjectPresent,
                             ObjectOwner, HandoverAsk, HandoverState, ObjectSpawn,
                             ObjectSample, ObjectRemove, Damage, PlayerPose,
                             TimeOfDay>;

MessageType type_of(const Message& message);

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
