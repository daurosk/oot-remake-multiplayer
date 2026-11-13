// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op over Steam, the part that needs neither Windows nor the game: what the
// two games of a Steam room say to each other before and around the session's
// own traffic. The session itself is unchanged: the host's game runs the co-op
// server on 127.0.0.1 and every game's client speaks the shared protocol over
// ENet, as on the local network; steam_net.cpp carries each ENet datagram
// through NX2navi's plugin interface (version 7: Steam rooms and datagrams
// between their members) instead of over the network. Tested on its own
// (tests/nx2navi_test.cpp).
//
// A datagram between two members of the room is one kind byte and a body:
//   hello   u8 version                 a member to the room's owner, every second
//                                      until the secret comes
//   secret  u8 version, the secret     the owner's answer: the session's join
//                                      password, made anew by the host for each
//                                      room, so that nobody types one
//   data    an ENet datagram, whole
//   part    u16 number, u8 index, u8 count, bytes
//                                      an ENet datagram too long for one Steam
//                                      datagram, in up to kMaxParts parts
// hello and secret travel reliably; data and part do not (ENet does its own
// resending). The secret is sent to members only, by a datagram Steam encrypts
// and whose sender Steam vouches for: it is never written into the room's
// public data, a file or a log.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace CoopNx2navi::SteamModel {

inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kSteamDatagram = 1200;  // NX2_STEAM_MESSAGE_MAX
inline constexpr std::size_t kMaxParts = 4;          // ENet's largest datagram is 4096 bytes
inline constexpr std::size_t kSecretChars = 32;
// The tag of this mod's rooms (the plugin interface leaves a lobby with another).
inline constexpr const char* kRoomTag = "oot-coop.1";

enum class Kind : std::uint8_t { hello = 1, secret = 2, data = 3, part = 4 };

std::vector<std::uint8_t> Hello();
std::vector<std::uint8_t> Secret(std::string_view secret);

// What one datagram from a member is.
struct Parsed {
    Kind kind = Kind::data;
    std::uint8_t version = 0;               // hello, secret
    std::string secret;                     // secret
    std::span<const std::uint8_t> bytes;    // data: the ENet datagram; part: its bytes
    std::uint16_t number = 0;               // part
    std::uint8_t index = 0, count = 0;      // part
};
// Nothing for bytes that are none of the four (too short, an unknown kind, a
// secret of another length or with other characters, a part out of range).
std::optional<Parsed> Parse(std::span<const std::uint8_t> datagram);

// An ENet datagram as the Steam datagrams that carry it: one `data`, or
// `part`s numbered `number` (the caller counts it up for each datagram it
// splits). Empty for a datagram that is empty or longer than kMaxParts parts.
std::vector<std::vector<std::uint8_t>> Wrap(std::span<const std::uint8_t> enet, std::uint16_t number);

// The parts of one sender put together again. Parts of another number than
// the one being collected start anew (the older datagram is lost, as any
// datagram may be); a part that comes twice changes nothing.
class Joiner {
public:
    // The whole ENet datagram when `part` completed it.
    std::optional<std::vector<std::uint8_t>> Add(const Parsed& part);

private:
    std::uint16_t number_ = 0;
    std::uint8_t count_ = 0;
    std::uint8_t have_ = 0;  // a bit per part
    std::vector<std::uint8_t> parts_[kMaxParts];
};

// The join password of a Steam session from 16 random bytes: 32 characters of
// 0-9 and a-f.
std::string SecretFrom(std::span<const std::uint8_t, 16> random);
bool ValidSecret(std::string_view secret);

// A Steam name as a co-op player's name: control characters dropped, blanks
// at the ends cut, at most 16 code points. "" when nothing is left.
std::string PlayerName(std::string_view steam_name);

// A room id as the player typed or pasted it, made plain: blanks and dashes
// dropped, capitals, and the dash put back ("abcd 2345" is "ABCD-2345"). ""
// when it is not eight of the letters and digits a room id is made of (no 0,
// 1, I or O). Whether such a room exists, and the id's own check, is the
// game's to say.
std::string RoomId(std::string_view typed);

}  // namespace CoopNx2navi::SteamModel
