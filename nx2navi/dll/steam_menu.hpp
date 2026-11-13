// SPDX-License-Identifier: GPL-3.0-or-later
// The Multiplayer menu's pages for co-op over Steam, as plain data (the rest
// of the menu is menu_model.hpp, whose State they share): the two entries of
// the first page, Host over Steam, Join over Steam, the list of friends to
// invite and the list of invitations received. menu_model.cpp calls in here;
// title_menu.cpp fills State::steam from steam_net.hpp and does what the
// actions ask. No Windows and no game code: tested on its own.
//
// Host over Steam needs no port, no password and no address: Start makes the
// session (the game starts again on the chosen multiplayer save, as over the
// local network) and a Steam room; the page then shows the room's id, which a
// press copies, and "Invite a friend". Join over Steam takes a room id, typed
// or pasted, or an invitation; then it is the local network's Join: the
// host's save, the restart, Continue.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "menu_model.hpp"

namespace CoopNx2navi::Menu::SteamPages {

bool Is(Page page);     // one of the four
bool List(Page page);   // the friends or the invitations

// The two entries of the first page: "Open" when Steam can be used, else
// "Unavailable" with the reason as the caption (and dimmed where State says so).
Row HostEntry(const State& state);
Row JoinEntry(const State& state);
// One of them was chosen: the page, when Steam can be used (Action::steam_open).
Action Open(State& state, bool host);

std::string Title(const State& state);
std::vector<Row> Rows(const State& state);  // kRows rows
Field FieldAt(Page page, std::size_t row);
// A button of a Steam page other than its Save row (menu_model.cpp's).
Action Activate(State& state, std::size_t row);

// The name the player has in a session over Steam: the one typed on a Steam
// page, else the Steam name, else the name of the other pages.
std::string PlayerName(const State& state);
// The clipboard's text for the Join page's room id (Action::paste_room).
void Pasted(State& state, std::string_view text);
// The owner did what Action::copy_room or Action::invite_friend asked.
void Copied(State& state, bool ok);
void Invited(State& state, bool ok);
// For menu_model.cpp's typing: the text a field's entry starts from, and the
// typed text taken when the field's rule holds (else State::message says it).
std::string EditStart(const State& state, Field field);
bool Commit(State& state, Field field, std::string_view text);
// What the owner knows of Steam now. True when a page shows something else
// for it (the page is to be drawn again).
bool Update(State& state, SteamAvail avail, std::string_view name, std::string_view hosted_room,
            std::vector<std::string> members, std::vector<SteamFriend> friends,
            std::vector<SteamInvite> invites);

// Sentences for the owner, in the page's language (two lines of at most 46 characters).
const char* WhyUnavailable(Lang lang, SteamAvail avail);
const char* WhyFailed(Lang lang, SteamFail fail);

}  // namespace CoopNx2navi::Menu::SteamPages
