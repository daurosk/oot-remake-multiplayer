// SPDX-License-Identifier: GPL-3.0-or-later
// Games on this network (the pause menu's Multiplayer page, "Games on this network"): a hosting game says it is there
// on the local network every two seconds (a UDP datagram to each network's broadcast address, port kPort), and a game
// looking for one lists the ones it heard in the last few seconds; A on one joins it at its address and port. The
// announcement names the game (the host's name, its port, how many play and fit, its room name, the protocol version)
// and never carries the join password: a guest types it on the Join view, as for a room. docs/protocol.md, "Games on
// this network". This file: the datagram and the list (no sockets: coop_lan_net.h).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopLan {

inline constexpr std::uint16_t kPort = 27508;             // where the announcements go (the game's own is 27510)
inline constexpr std::uint64_t kEveryMs = 2000;           // a host announces itself this often
inline constexpr std::uint64_t kGoneMs = 7000;            // a game not heard this long is off the list
inline constexpr std::size_t kMaxDatagram = 512;
inline constexpr std::size_t kMaxName = 32;               // bytes of a host's name kept (cut at a UTF-8 letter)
inline constexpr std::size_t kMaxGames = 32;              // the most the list keeps

struct Game {
    std::string name;          // the host's name (coop.ini name)
    std::uint16_t port = 0;    // its game port
    int players = 0;           // in the game now, the host included
    int max = 0;               // the most it takes
    std::string room;          // its relay room name, "" none
    int protocol = 0;          // its protocol version (coop::protocol::kProtocolVersion): another one cannot join
    std::string id;            // a random token the game picks once (NewId): the same game heard from several of its
                               // networks (Wi-Fi, a VPN, a virtual switch) is one row. "" none (then address and port)
};

inline constexpr std::size_t kIdLength = 16;   // hex digits

// A new id: kIdLength hex digits from the system's random source.
std::string NewId();

// The datagram: "SQUADALA LAN 1" then "key=value" lines (name, port, players, max, room, protocol, id).
std::string Encode(const Game& g);
// A datagram heard: the game it names, or nothing for anything else (not ours, another format, out of range).
std::optional<Game> Decode(std::string_view datagram);

// A game heard, where from.
struct Heard {
    std::string address;   // its IPv4 address, as the datagram came from it ("192.168.1.20")
    Game game;
    std::uint64_t at_ms = 0;
};

// The games heard, newest word kept per game: per id, or per address and port for a game without one.
class List {
public:
    // A game with an id heard from another address keeps the address it was first heard at (a steady row), except that
    // a network address takes the place of 127.0.0.1.
    void Add(const std::string& address, const Game& g, std::uint64_t now_ms);
    // The ones heard in the last kGoneMs, by name then address (a steady order for the page's rows).
    std::vector<Heard> Now(std::uint64_t now_ms) const;
    void Clear() { heard_.clear(); }

private:
    std::vector<Heard> heard_;
};

// What a game's row says: "Saria's game (2/4)", "... (full)", "... (other version)".
std::string RowText(const Heard& h, int my_protocol);
// Whether it can be joined from here (same protocol, room for one more).
bool Joinable(const Heard& h, int my_protocol);
// Nucleus's "Join a game on this network" (coop.ini join_lan): the game to join among those heard. `want` "first" (or
// empty): the first that can be joined, in the list's order; else the one whose host has that name (any case). None:
// nothing.
std::optional<Heard> PickToJoin(const std::vector<Heard>& heard, const std::string& want, int my_protocol);

}  // namespace CoopLan
