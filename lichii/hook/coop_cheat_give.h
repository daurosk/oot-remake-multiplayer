// SPDX-License-Identifier: GPL-3.0-or-later
//
// One test or cheat gift of an item flag, made at once into a flag store (docs/items.md, "Live check"; the
// play-test bridge's `give`, docs/playtest.md). No emulator code: tested in tests/lichii/cheat_give_test.cpp.
//
// The gift is what a pickup does to the flags: the item flag itself (a Bool or an owned item becomes 1, a count goes
// up by one, PlayerStatus.MaxLife by one heart), its IsGet flag, and the ammo and capacity the game gives with it
// (item_grants: the bow comes with the small quiver and 30 arrows, the bomb bag with 20 bombs).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "coop/host/flags.hpp"

namespace CoopCheatGive {

// The item flags a gift word stands for: `bombs`, `bow` and `hookshot` are the flags of each, any other word is a
// flag name as it is, except a bare item name (Item_Boomerang), which is its PouchData.<name>.StockNum flag, else its
// IsGet.<name> flag, when the game has one of them.
std::vector<std::string> Names(std::string_view word);

// A name in the list of item flags (shared or per player: the ammo counts are per player).
bool IsItemFlag(std::string_view name);

struct Result {
    bool ok = false;           // every flag written, or already at its value
    bool changed = false;      // at least one flag has a new value
    bool wrote = false;        // the item flag itself is written or already at its value (a later write may still fail)
    std::string text;          // what it did: "NAME was -> now; ..." (also the reason when not ok)
};

// Gives the item flag `name` into `game` now. Not ok: not an item flag, or a flag the store cannot read or write. Not ok
// with `wrote`: the item is in, but its IsGet or a grant failed (a partial gift; `text` says which).
Result Give(coop::host::FlagStore& game, std::string_view name);

}  // namespace CoopCheatGive
