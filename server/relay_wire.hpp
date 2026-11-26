#pragma once

// The relay's own datagrams (docs/play-online.md, "Relay with join codes").
// They are not co-op messages: the relay never reads the game's traffic, it
// only forwards the host's ENet datagrams to the guests and back.
//
// Every relay datagram starts with kMagic. Its first byte, 'O' (0x4F), has
// ENet's "compressed" header bit set; we never turn ENet's compression on, so
// no ENet datagram starts like that and the relay can tell them apart on one
// port. All numbers are big-endian.
//
// Room names (issue 261) came later as three more kinds. A relay from before
// them decodes those as nothing and answers nothing, so a host or guest that
// gets no answer to them can say the relay is too old; hosts and guests that
// use join codes work with either relay.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace coop::server::relay {

inline constexpr std::uint16_t kDefaultRelayPort = 27530;
inline constexpr std::array<std::uint8_t, 4> kMagic = {'O', 'C', 'R', 1};
// ENet datagrams are at most protocol::kDatagramBytes (1100); a relayed one
// with our header is still under a 1200-byte path (issue 143).
inline constexpr std::size_t kMaxPayloadBytes = 1400;
inline constexpr std::size_t kCodeLength = 8;
inline constexpr std::size_t kMaxKeyBytes = 64;
// Room names: kMinRoomLength to kMaxRoomLength of A-Z, a-z, 0-9, '-' and '_',
// compared without case.
inline constexpr std::size_t kMinRoomLength = 3;
inline constexpr std::size_t kMaxRoomLength = 24;
// The room list (kListRoom, kListRooms, kRoomList): a listed room's host name
// is 1 to kMaxHostNameBytes bytes of UTF-8 without control characters. The
// relay lists at most kMaxListedRooms rooms, newest first. A request is
// padded with zeros to kListRequestBytes, so an answer (at most
// kMaxPayloadBytes) is never more than about three times what was sent: the
// relay cannot be used to flood a forged sender's address with much more than
// that sender sent.
inline constexpr std::size_t kMaxHostNameBytes = 32;
inline constexpr std::size_t kMaxListedRooms = 50;
inline constexpr std::size_t kListRequestBytes = 512;

enum class Kind : std::uint8_t {
  // Host link -> relay. All carry the session token the link chose.
  kRegister = 0x01,      // token, key length u8, key: "give me a join code"
  kKeepalive = 0x02,     // token (the relay answers kRegistered)
  kLinkData = 0x03,      // token, flow u16, one ENet datagram for that guest
  kLinkClose = 0x04,     // token, flow u16: that guest is gone on the host
  kBye = 0x05,           // token: the host stopped
  kRegisterName = 0x06,  // token, key length u8, key, name length u8, name:
                         // "give me a join code and this room name"
  kListRoom = 0x07,      // token, listed u8 (0 or 1), players u8, max u8,
                         // version u16, host name length u8, host name: "list
                         // my room (or not) with these numbers" (RoomEntry)
  kRoomProof = 0x08,     // token, time u64, signature (64 bytes): the listed
                         // room's proof (RoomProof; the official game's signed
                         // mark, docs/official-game.md). A relay before 0.11.8
                         // drops it; this one keeps it beside the listing and
                         // never checks it (the kits do).
  // Guest -> relay.
  kLookup = 0x10,      // the 8-letter code: lets this address join that host
  kLookupName = 0x11,  // name length u8, name: the same with a room name
  kListRooms = 0x12,   // start u8, zeros up to kListRequestBytes in all: "the
                       // listed rooms from this one on" (anyone may ask)
  kListProofs = 0x13,  // zeros up to kListRequestBytes in all: "the listed
                       // rooms' proofs" (a relay before 0.11.8 does not answer)
  // Relay -> host link.
  kRegistered = 0x81,      // token, code
  kRefused = 0x82,         // token, Refusal u8
  kRelayData = 0x83,       // flow u16, one ENet datagram from that guest
  kRelayClose = 0x84,      // flow u16: that guest went quiet
  kRegisteredName = 0x85,  // token, code, name length u8, name: the room is this host's
  // Relay -> guest.
  kFound = 0x90,     // LookupResult u8
  kRoomList = 0x91,  // total u8, start u8, count u8, then count entries: room
                     // length u8, room, host length u8, host, players u8,
                     // max u8, version u16, flags u8 (bit 0: a join password)
  kRoomProofs = 0x92,  // count u8, then count proofs: room length u8, room,
                       // time u64, signature (64 bytes); as many as fit
};

enum class Refusal : std::uint8_t {
  kUnknownSession = 1,  // the relay does not know this token (it restarted)
  kFull = 2,            // the relay has as many hosts as it takes
  kWrongKey = 3,        // the relay wants a host key and this was not it
  kTooMany = 4,         // too many hosts from this address
  kSlowDown = 5,        // too many registrations from this address lately
  kNameTaken = 6,       // another host has this room name
  kBadName = 7,         // not a room name
};

enum class LookupResult : std::uint8_t {
  kOk = 0,
  kUnknown = 1,   // no host has this code (mistyped, or the host stopped)
  kSlowDown = 2,  // too many wrong codes from this address: wait 10 minutes
  kFull = 3,      // this host already has the most guest addresses it takes
};

// One listed room: what a host lists (kListRoom: room is not sent, the relay
// knows it) and what the list says (kRoomList). Never the join password.
struct RoomEntry {
  std::string room;        // the room name as its host holds it
  std::string host;        // the host's name, 1 to kMaxHostNameBytes bytes
  std::uint8_t players = 0;
  std::uint8_t max_players = 0;
  std::uint16_t version = 0;  // the host's co-op protocol version
  bool password = true;       // a join password is needed (every session has one)
  bool operator==(const RoomEntry&) const = default;
};

// Whether a host name can be listed: 1 to kMaxHostNameBytes bytes, none of
// them a control character (UTF-8 letters are fine).
bool listable_host_name(std::string_view name);
// A player's name as the list takes it: control characters dropped, cut to
// kMaxHostNameBytes before a UTF-8 letter's start; "Host" when nothing is left.
std::string listing_name(std::string_view name);

// A listed room's proof (kRoomProof, kRoomProofs): passed on as its host gave it; what it means is
// coop::protocol::official_check's.
inline constexpr std::size_t kProofSignatureBytes = 64;
struct RoomProof {
  std::string room;  // kRoomProofs: the room it was given for (kRoomProof: the relay knows it)
  std::uint64_t time_s = 0;
  std::array<std::uint8_t, kProofSignatureBytes> signature{};
  bool operator==(const RoomProof&) const = default;
};
// What a kRoomProofs proof adds to the datagram, in bytes.
std::size_t proof_bytes(const RoomProof& proof);

struct Message {
  Kind kind = Kind::kKeepalive;
  std::uint64_t token = 0;
  std::uint16_t flow = 0;
  std::uint8_t status = 0;  // Refusal or LookupResult
  std::string code;         // kRegistered, kRegisteredName, kLookup: kCodeLength letters
  std::string key;          // kRegister, kRegisterName
  std::string name;         // kRegisterName, kRegisteredName, kLookupName: at most kMaxRoomLength bytes
  std::vector<std::uint8_t> payload;
  bool listed = false;           // kListRoom: list this room (false: take it off the list)
  RoomEntry entry;               // kListRoom: its numbers and host name (room unused)
  std::uint8_t start = 0;        // kListRooms, kRoomList: the first room's place in the list
  std::uint8_t total = 0;        // kRoomList: how many rooms the list has
  std::vector<RoomEntry> rooms;  // kRoomList: the rooms from `start` on that fit
  RoomProof proof;                // kRoomProof (room unused)
  std::vector<RoomProof> proofs;  // kRoomProofs
};

bool has_magic(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode(const Message& message);
// Nothing for a datagram that is not a well-formed relay message.
std::optional<Message> decode(std::span<const std::uint8_t> bytes);
// What a kRoomList entry adds to the datagram, in bytes.
std::size_t entry_bytes(const RoomEntry& entry);

// Join codes: kCodeLength letters from 32 that cannot be mixed up when read
// aloud or typed (no I, O, 0, 1): 40 bits.
inline constexpr std::string_view kCodeLetters = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
// A code from 40 random bits.
std::string make_code(std::uint64_t random_bits);
// What a player typed ("abcd-efgh", " ABCD EFGH ") as the bare code, or
// nothing when it cannot be one.
std::optional<std::string> normalize_code(std::string_view typed);
// "ABCDEFGH" -> "ABCD-EFGH", the way it is shown.
std::string show_code(std::string_view code);

// What a player typed as a room name, without spaces or tabs at either end and
// with its letters' case kept; nothing when it is not one.
std::optional<std::string> normalize_room(std::string_view typed);
// The form room names are compared in: lower case ("Forest-Gang" and
// "forest-gang" are one room).
std::string room_key(std::string_view room);

std::string_view refusal_text(Refusal why);

}  // namespace coop::server::relay
