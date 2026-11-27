// SPDX-License-Identifier: GPL-3.0-or-later
//
// One test or cheat gift of an item flag, made at once into a flag store (docs/items.md, "Live check"; the
// play-test bridge's `give`, docs/playtest.md). No emulator code: tested in tests/lichii/cheat_give_test.cpp.
//
// The gift is what a pickup does to the flags: the item flag itself (a Bool or an owned item becomes 1, a count goes
// up by one, PlayerStatus.MaxLife by one heart), its IsGet flag, and the ammo and capacity the game gives with it
// (item_grants: the bow comes with the small quiver and 30 arrows, the bomb bag with 20 bombs).
#pragma once

#include <cstdint>
#include <optional>
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

// Values the game keeps in our Link's own Life component, not in a flag: the PlayerStatus.Life and
// PlayerStatus.MaxLife flags are only its mirror, which the game writes from the component every frame (issue 88:
// a raised MaxLife flag alone never reaches the hearts; volvagia-rocks-234-2 2026-11-26: a give of MaxLife read 16
// again at every give, the next frame put 16 back). Any write of one goes into the component first.
// The emulator side (coop_testinput.cpp) finds our Link by a fresh walk of the game's actor list
// (CoopObjects::FindOurLinkLife); tested with a fake in tests/lichii/cheat_give_test.cpp.
class LinkLife {
public:
    virtual ~LinkLife() = default;
    // Writes the component's word: now (`life`) or the most (`max`), in quarter hearts. False: no Link.
    virtual bool set_life(std::int32_t quarters) = 0;
    virtual bool set_max(std::int32_t quarters) = 0;
};

enum class LinkWord { none, life, max };
// Which word of our Link's Life component the flag `hash` mirrors (none: a flag the game keeps itself).
LinkWord LinkBacked(std::uint32_t hash);

// A flag store whose writes of a component-backed flag (LinkBacked) go into our Link's component first, then into
// the flag (its mirror, so a read in the same held call already sees the new value). No Link (`link` null, or its
// write fails): the write fails and nothing is written, so the caller says NOT given / NOT set at once. Every other
// flag goes to `flags` as it is.
class LinkBackedFlags final : public coop::host::FlagStore {
public:
    LinkBackedFlags(coop::host::FlagStore& flags, LinkLife* link) : flags_(flags), link_(link) {}
    std::optional<coop::protocol::FlagEntry> read(coop::host::FlagKey key) override { return flags_.read(key); }
    bool write(const coop::protocol::FlagEntry& entry) override;
    bool is_count(coop::host::FlagKey key) override { return flags_.is_count(key); }

private:
    coop::host::FlagStore& flags_;
    LinkLife* link_;
};

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
