// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_cheat_give.h"

#include <algorithm>
#include <cstdint>

#include "coop/host/game_items.hpp"

namespace CoopCheatGive {
namespace {

using coop::protocol::FlagKind;

const coop::host::ItemFlag* Find(std::string_view name) {
    for (const auto& item : coop::host::item_flags()) {
        if (item.name == name) return &item;
    }
    return nullptr;
}

std::string Add(const std::string& so_far, const std::string& what) {
    return so_far.empty() ? what : so_far + "; " + what;
}

// Writes `value` into the flag `hash` of `kind` (as bits) unless it already holds it. `text` gets "name was -> now"
// (or "name already N").
bool Put(coop::host::FlagStore& game, std::string_view name, std::uint32_t hash, FlagKind kind, std::int32_t value,
         Result& out) {
    const coop::host::FlagKey key(hash, 0);
    const auto now = game.read(key);
    if (!now || now->kind != kind) {
        out.text = Add(out.text, std::string(name) + ": not readable (no save loaded, or not this kind of flag)");
        return false;
    }
    const auto was = static_cast<std::int32_t>(now->value);
    if (was == value) {
        out.text = Add(out.text, std::string(name) + " already " + std::to_string(was));
        return true;
    }
    if (!game.write(coop::protocol::FlagEntry(key, kind, static_cast<std::uint32_t>(value)))) {
        out.text = Add(out.text, std::string(name) + ": write failed");
        return false;
    }
    out.changed = true;
    out.text = Add(out.text, std::string(name) + " " + std::to_string(was) + " -> " + std::to_string(value));
    return true;
}

// The value one gift of `item` leaves: an owned item or a Bool becomes 1 (a bigger value stays), a count goes up
// by one, max life by one heart (4 quarters), a floors bit field gets its first floor too.
std::int32_t Wanted(const coop::host::ItemFlag& item, std::int32_t was) {
    if (item.kind == FlagKind::int32) {
        if (item.rule == coop::host::ItemRule::count) return was + 1;
        if (item.name == "PlayerStatus.MaxLife") return was + 4;
        return std::max(was, 1);
    }
    if (item.kind == FlagKind::uint32) return static_cast<std::int32_t>(static_cast<std::uint32_t>(was) | 1u);
    return 1;
}

}  // namespace

bool IsItemFlag(std::string_view name) { return Find(name) != nullptr; }

std::vector<std::string> Names(std::string_view word) {
    if (word == "bombs") return {"PouchData.Item_BombBag_Small.StockNum"};
    if (word == "bow") return {"PouchData.Weapon_Range_Bow.StockNum"};
    if (word == "hookshot") return {"PouchData.Item_Hookshot.StockNum"};
    // A bare item name (Item_Boomerang, Weapon_Sword_Kokiri): the pouch count, which Give also marks as got; else its
    // IsGet flag. A name with a dot is a flag name and stays as it is.
    if (!word.empty() && word.find('.') == std::string_view::npos) {
        const std::string stock = "PouchData." + std::string(word) + ".StockNum";
        if (IsItemFlag(stock)) return {stock};
        const std::string got = "IsGet." + std::string(word);
        if (IsItemFlag(got)) return {got};
    }
    return {std::string(word)};
}

Result Give(coop::host::FlagStore& game, std::string_view name) {
    Result out;
    const auto* item = Find(name);
    if (!item) {
        out.text = std::string(name) + ": not an item flag";
        if (name.find('.') == std::string_view::npos) {
            out.text += " (a bare item name such as Item_Boomerang needs a PouchData." + std::string(name) +
                        ".StockNum or an IsGet." + std::string(name) + " flag, and this game has neither)";
        }
        return out;
    }
    const auto now = game.read(coop::host::FlagKey(item->hash, 0));
    if (!now) {
        out.text = std::string(name) + ": not readable (save not loaded?)";
        return out;
    }
    out.ok = Put(game, item->name, item->hash, item->kind, Wanted(*item, static_cast<std::int32_t>(now->value)), out);
    if (!out.ok) return out;
    out.wrote = true;
    // A pouch item also gets its IsGet flag, as a pickup sets both.
    constexpr std::string_view pouch = "PouchData.", stock = ".StockNum";
    if (name.starts_with(pouch) && name.ends_with(stock)) {
        const std::string get = "IsGet." + std::string(name.substr(pouch.size(), name.size() - pouch.size() - stock.size()));
        if (const auto* is_get = Find(get); is_get && is_get->kind == FlagKind::boolean) {
            out.ok = Put(game, is_get->name, is_get->hash, is_get->kind, 1, out) && out.ok;
        }
    }
    // What the game gives with the item (ammo, capacity): each only up to its floor, never lowered.
    for (const auto& grant : coop::host::item_grants(item->hash)) {
        const auto held = game.read(coop::host::FlagKey(grant.hash, 0));
        if (!held || held->kind != grant.kind) continue;  // (a flag this save does not have: left)
        if (static_cast<std::int32_t>(held->value) >= grant.value) continue;
        out.ok = Put(game, grant.name, grant.hash, grant.kind, grant.value, out) && out.ok;
    }
    return out;
}

}  // namespace CoopCheatGive
