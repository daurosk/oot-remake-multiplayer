// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op over Steam, the DLL's side: NX2navi's plugin interface (version 7)
// gives a mod Steam rooms (friends-only lobbies with a room id), invitations
// and datagrams between the members of a room; this carries the session over
// them. Nothing of the session changes: the host's game runs the co-op server
// on 127.0.0.1, every game's client speaks the shared protocol over ENet, and
// here each ENet datagram is taken from a UDP socket on 127.0.0.1 and sent to
// the other game through Steam, and back (steam_model.hpp has the few bytes
// added around it).
//
//   the host's game                               a joiner's game
//   server 127.0.0.1:P  <-- UDP -->  one socket    client --> 127.0.0.1:Q (this
//   client -> 127.0.0.1:P            per member    DLL's socket) <-- Steam -->
//                                    <-- Steam -->
//
// The host makes the join password itself, anew for each room (16 random
// bytes), and gives it to each member that asks, in a reliable datagram Steam
// encrypts and signs: nobody types one, and it is in no file and no log.
// The server listens on 127.0.0.1 only, so the room's members are the only
// ones who can reach it.
//
// No Steam code is here and none of NX2navi's: the game does the Steam part
// (it loads Steam's library from beside its executable) and this file only
// calls the interface (third_party/plugin_api.h).
//
// Threads: Start, Host, Join and the rest are called from the DLL's thread
// and the game's (the menu); Steam's events come on the game's Steam thread;
// one thread of this file's own moves the UDP datagrams. One mutex.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "menu_model.hpp"

struct Nx2PluginApi;

namespace CoopNx2navi::SteamNet {

using Avail = Menu::SteamAvail;
using Fail = Menu::SteamFail;

struct Member {
    std::uint64_t id = 0;
    std::string name;
    bool self = false;
    bool owner = false;
};
struct Invitation {
    std::string from;  // the friend's name, "" unknown
    std::string room;  // the room's id
    bool asked = false;  // accepted in Steam's own window: to be joined at once
};
// Where the way to the session is.
enum class Link : std::uint8_t {
    none,
    entering,  // the room is being made or entered
    asking,    // a joiner asks the host for the session's secret
    ready,     // the datagrams go through: `port` is where this game's client connects
    failed,    // `fail` says why
};
struct View {
    Avail avail = Avail::old_game;
    bool started = false;  // signed in, or on the way
    bool local = false;    // the game's stand-in for Steam (two games on one PC)
    std::string name;      // this player's Steam name
    bool owner = false;    // this game made the room
    std::string room;      // the room's id, "" in none
    std::vector<Member> members;
    std::vector<Invitation> invites;  // the newest first
    std::size_t friends = 0;
    Link link = Link::none;
    Fail fail = Fail::none;
    std::uint16_t port = 0;
    std::uint64_t serial = 0;  // counts the changes
};

// Once the game's interface is known. `log` gets this file's lines.
void Init(const Nx2PluginApi* api, void (*log)(const char* line));
// Whether Steam can probably be used, without signing in: the interface has
// it, Steam's library is beside the game, Steam runs. Start gives the last word.
Avail Precheck();
// Signs in (the player then shows as playing "Spacewar" on Steam). False when
// the game refused at once; View().avail says why either way.
bool Start();
// Signs out unless a room is entered or on its way: for the menu page that
// closes without a session.
void StopIfIdle();
// Leaves the room and signs out. Any thread but the game's Steam thread.
void Shutdown();

View Get();
std::vector<Menu::SteamFriend> Friends();
bool Invite(std::uint64_t friend_id);
// The invitation the player accepted in Steam's own window, once ("" none).
std::string TakeAsked();

// The host: makes the room and carries each member's datagrams to and from
// the server on 127.0.0.1:`server_port`, whose join password is `secret`.
bool Host(std::uint16_t server_port, std::string secret);
// A joiner: enters the room, asks its host for the secret and opens the UDP
// port this game's client connects to.
bool Join(const std::string& room);
void Leave();
// The session's join password, once the link is ready.
std::string Secret();

// For the DLL's thread: both wait (with `sleep(ms)`, true when the DLL is
// asked to stop) until the link is ready or failed.
struct Session {
    bool ok = false;
    Fail fail = Fail::none;
    std::uint16_t port = 0;   // where this game's client connects, on 127.0.0.1
    std::string secret;       // with this join password
};
// `start_server(port, secret)` starts the co-op server on 127.0.0.1:port
// (true when it runs).
Session HostSession(const std::function<bool(std::uint16_t, const std::string&)>& start_server,
                    const std::function<bool(unsigned)>& sleep);
Session JoinSession(const std::string& room, const std::function<bool(unsigned)>& sleep);

}  // namespace CoopNx2navi::SteamNet
