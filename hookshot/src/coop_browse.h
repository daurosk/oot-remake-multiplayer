// SPDX-License-Identifier: GPL-3.0-or-later
// The game browser's search, filter and sort (the pause menu's Multiplayer page, Find a game; the title's Join page,
// Find a game): which of the games heard are listed and in what order. The games on this network (coop_lan.h) and
// the relay's public rooms (coop_room_list.h) both come in as entries; the lists keep their own indices so a pick
// reaches the game it showed.
#pragma once

#include <string>
#include <vector>

namespace CoopBrowse {

// What the list keeps: every game; only the ones this game can join (its version, room for one more); every game of
// this version (full ones too).
enum Filter : int { kAll = 0, kOpen = 1, kSameVersion = 2 };
inline constexpr int kFilters = 3;
// The order: the most players first, the fewest first, by name (A to Z, any case). Ties go by name, then as heard.
enum Sort : int { kMostPlayers = 0, kFewestPlayers = 1, kByName = 2 };
inline constexpr int kSorts = 3;

const char* FilterWords(int filter);   // "All games", "Open games", "This version"
const char* SortWords(int sort);       // "Most players", "Fewest players", "Name"

struct Entry {
    std::string name;   // the game's or the room's name as listed
    std::string host;   // its host's name ("" when the name says it)
    int players = 0, max = 0;   // max 0: not known
    bool same_version = true;
};

// Whether this game can join it: the same version, and room for one more.
bool Joinable(const Entry& e);
// The entries the search (in the name or the host's, any case; "" all) and the filter keep, in the sort's order: their
// indices in `all`.
std::vector<int> Order(const std::vector<Entry>& all, const std::string& search, int filter, int sort);

}  // namespace CoopBrowse
