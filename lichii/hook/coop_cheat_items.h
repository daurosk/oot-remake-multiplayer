// SPDX-License-Identifier: GPL-3.0-or-later
//
// LICHII_COOP_OBJECTS_CHEAT=1 (test only, for the live checks): keeps the bow,
// the small quiver with 30 arrows, the small bomb bag and 20 bombs, and the
// slingshot with 30 seeds in the local player's pouch, through the game's own
// flag write queues (coop/host/game_flags). coop_objects.cpp calls TopUp once
// a second.
//
// Bombs need more than the pouch (issue 125): with only the pouch, the count
// and the capacity, every ZR made an Item_Bomb that was gone 0.1 s later,
// nothing in hand and the count still 20. In the saves of a player who got
// the bomb bag in the game (docs/items.md, "Bombs from the cheat") these are
// also set, and they were 0 in the cheat's save:
// - the bomb's get order (UInt struct 0xf5ab6461, member Item_Bomb), the
//   number after the highest of the 42 items';
// - the "Bomb" member of the two 53-member Bool structs 0x541947d0 and
//   0x57f52eb8 (most likely the "Bomb" help box seen at the first ZR; the
//   slingshot's are set in every save that has the slingshot);
// - Item_BombBag_Small in the 199-member Bool struct 0x41f3e410 (set with
//   IsGet in every save that has the bag).
// It touches nothing else.
//
// No emulator code: tested in tests/lichii/cheat_items_test.cpp.
#pragma once

#include <string>
#include <vector>

#include "coop/host/game_flags.hpp"

namespace CoopCheatItems {

// What TopUp has already said, kept by the caller for the session.
struct State {
    bool order_missing_noted = false;
};

// One pass: tops up what is below its value. Returns one log line per write
// ("cheat <what> <was> -> <now>"). A flag that cannot be read (the flag
// manager is not up yet, at the title screen) is left for the next pass; a
// write the queue refused says so and is tried again on the next pass. When
// the flags are up but the bomb's get order is not in the UInt table (it may
// be in the game's fallback table, which is not read), that is said once:
// "cheat get order Item_Bomb not found".
std::vector<std::string> TopUp(coop::host::WritableGameMemory& memory, State& state);

} // namespace CoopCheatItems
