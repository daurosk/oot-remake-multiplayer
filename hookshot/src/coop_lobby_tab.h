// SPDX-License-Identifier: GPL-3.0-or-later
// The Lobby tab of Hookshot's menu (docs/hookshot.md "The Lobby tab",
// docs/play-online.md "Lobby"), right after Multiplayer: who is in the game
// (name, colour, the host, ping, where, up or downed, talking, muted), the
// invite code with Copy, the host's game (place, hearts) and the places
// left; on a player, every player's own mutes and Teleport to them, and the
// host's Mute for everyone, Kick and Ban. This file is the tab's logic only
// (no game, no network, no files), so the tests drive it: the module
// (coop_lobby_tab_module.cpp) gives it the session each refresh and carries
// out its requests; the server checks every host action itself.
// Not thread-safe: the module guards it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_pause_menu.h"
#include "hos_coop_menu.h"

namespace CoopLobbyTab {

using Input = CoopPauseMenu::Input;
using View = CoopPauseMenu::View;

struct Player {
    std::uint8_t number = 0;
    std::string name;
    bool self = false;
    bool host = false;
    std::string colour;                 // in words ("blue")
    std::optional<std::uint32_t> ms;    // ours: the round trip; theirs: how late their samples are
    std::string place;                  // where they are ("" not known)
    enum class Life : std::uint8_t { up, downed, out, dead };
    Life life = Life::up;
    bool talking = false;               // their voice plays here now (ours: we talk)
    std::uint8_t host_mutes = 0;        // proto::lobby_mute bits the host set
    std::uint8_t own_mutes = 0;         // ours of them
    bool operator==(const Player&) const = default;
};

struct Lobby {
    bool connected = false;
    bool hosting = false;               // we are the session's host
    int max_players = 0;                // 0: not known
    std::string invite;                 // the invite code (host), "" none yet
    std::vector<Player> players;        // by number
    std::string host_place;             // the host's game: where its Link is
    std::optional<std::uint16_t> host_life, host_max;   // quarter hearts
    bool operator==(const Lobby&) const = default;
};

struct Request {
    enum class Kind : std::uint8_t {
        kServer,      // `action` to the server (the host's)
        kOwnMute,     // our own mute of `player`: `what` (proto::lobby_mute bits) on or off
        kTeleport,    // /teleport to `name`
        kCopyInvite,  // the invite code onto the clipboard
    };
    Kind kind = Kind::kServer;
    coop::protocol::LobbyAction action{};
    std::uint8_t player = 0;
    std::uint8_t what = 0;
    bool on = false;
    std::string name;
};

class Tab {
public:
    // The tab came to show: the first page.
    void Show();
    void Hide();
    // The session now; a player page whose player left goes back to the list.
    void Set(Lobby lobby);

    // false when nothing took it (Back on the list: the menu closes).
    bool Press(Input input);
    void Select(std::size_t row);
    void Click(std::size_t row, int side);
    std::optional<Request> TakeRequest();

    View view() const;
    std::uint8_t shown_player() const { return player_; }   // 0: the list

private:
    enum class Act : std::uint8_t {
        kNone, kPlayer, kCopy, kOwnVoice, kOwnChat, kTeleport, kHostVoice, kHostChat, kKick, kBan
    };
    struct Line;
    std::vector<Line> Lines() const;
    const Player* Shown() const;
    void Move(int step);
    void Clamp();
    void Decide(const Line& line);
    void Change(const Line& line, int side);
    void Go(std::uint8_t player);

    Lobby lobby_;
    std::uint8_t player_ = 0;       // the player page shown; 0: the list
    std::size_t selected_ = 0;
    Act confirm_ = Act::kNone;      // Kick or Ban pressed once: the next press does it
    std::string note_;              // the answer to the last press, until the next one
    std::optional<Request> request_;
};

// The value cell of a player's row on the list: "42 ms  Kokiri Forest  downed  talking  muted".
std::string Summary(const Player& p);

// The module's part (coop_lobby_tab_module.cpp, only in hookshot_coop.dll): the one Tab, handed to Hookshot
// right after Multiplayer through coop_pause.cpp's exports (hos_coop_menu.h). Thread-safe.
const char* TabName();
bool Page(HosCoopMenuPage* out);
bool MenuInput(int event, int arg, int arg2);

}  // namespace CoopLobbyTab
