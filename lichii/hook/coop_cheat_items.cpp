// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_cheat_items.h"

#include <array>
#include <cstdint>
#include <optional>

namespace CoopCheatItems {
namespace {

using coop::host::GameFlags;

// PouchData.<item>.StockNum and IsGet.<item> hashes from docs/items.md, and
// the count each is kept at. Entries with is_get 0 are the ammo capacities
// (Int struct 0xf80f8077: Rupee, DekuSeed, Bomb, Arrow, DekuStick, DekuNut):
// without one the item is in the menu but the game gives none (bombs: Link
// lifted empty hands).
struct IntItem {
    const char* name;
    std::uint32_t stock;
    std::uint32_t is_get;
    std::int32_t count;
};
constexpr std::array<IntItem, 10> kIntItems{{
    {"Weapon_Range_Bow", 0x938d56bf, 0x69ec28be, 1},
    {"Item_Quiver_Small", 0xa8c3e1fb, 0x0a550531, 1},
    {"Item_Arrow_Normal", 0x518ec74b, 0x572701ea, 30},
    {"capacity Arrow", 0x418db791, 0, 30},
    {"Item_BombBag_Small", 0xb0a1bf51, 0xa63f1e2d, 1},
    {"Item_Bomb", 0xdb1bcd4f, 0x2bb00768, 20},
    {"capacity Bomb", 0xe753508b, 0, 20},
    {"Weapon_Range_SlingShot", 0x80d34ce9, 0x0ec22ee1, 1},
    {"Item_DekuSeed", 0x5fadf7fc, 0x29888e1e, 30},
    {"capacity DekuSeed", 0xae8cc742, 0, 30},
}};

// Bools a real bomb bag sets beside IsGet (coop_cheat_items.h; the children
// of the structs as GameDataList names them, member hash = Murmur3 of the
// member name).
struct BoolFlag {
    const char* name;
    std::uint32_t hash;
};
constexpr std::array<BoolFlag, 3> kBombBools{{
    {"0x541947d0.Bomb", 0x6e5e7d9a},
    {"0x57f52eb8.Bomb", 0xfced7932},
    {"0x41f3e410.Item_BombBag_Small", 0x4d345bbe},
}};

// The get order (UInt struct 0xf5ab6461): one UInt per assignable item and
// bottle. In the real saves each item got the next number when it was got
// (slingshot 34, bombs 35 in one save).
constexpr std::uint32_t kBombGetOrder = 0xc7ecb4a1;  // member Item_Bomb
constexpr std::uint32_t kStockBomb = 0xdb1bcd4f;     // read: are the flags up?
constexpr std::array<std::uint32_t, 42> kGetOrders{{
    0x117375f7, 0x22c20fa9, 0x3b9434e0, 0x3e70dbc1, 0x4301162b, 0x438c1f7b, 0x4ad60f93,
    0x549ee15c, 0x55f6470d, 0x56573d03, 0x58c854bc, 0x642ffd7f, 0x6a1e2459, 0x73666cdf,
    0x7c7c2880, 0x83b3eaa3, 0x956aed56, 0x961a120b, 0x99b98ae4, 0x9a5bef3e, 0xa6c38c34,
    0xaab858a4, 0xad3ff9c8, 0xaeeb7554, 0xaf5154ab, 0xc7025e0a, 0xc7ecb4a1, 0xc98ed673,
    0xcba88c4f, 0xcbab442d, 0xcfe1a634, 0xdb070fb9, 0xdd3baa0b, 0xe66b92b8, 0xe7ee66d6,
    0xe97874a7, 0xe9a087b5, 0xecffd2a6, 0xf038e577, 0xf2e7d892, 0xf2ffd542, 0xf78f00bc,
}};

std::string Note(const char* what, long long was, long long now, bool set) {
    return std::string("cheat ") + what + " " + std::to_string(was) + " -> " +
           std::to_string(now) + (set ? "" : " (queue busy, again next second)");
}

} // namespace

std::vector<std::string> TopUp(coop::host::WritableGameMemory& memory, State& state) {
    std::vector<std::string> out;
    GameFlags flags(memory);
    for (const IntItem& item : kIntItems) {
        const auto now = flags.get_int(item.stock);
        if (!now || *now >= item.count) {
            continue;
        }
        const bool set = GameFlags::set_int(memory, item.stock, item.count);
        if (item.is_get != 0 && flags.get_bool(item.is_get) == false) {
            GameFlags::set_bool(memory, item.is_get, true);
        }
        out.push_back(Note(item.name, *now, item.count, set));
    }
    for (const BoolFlag& b : kBombBools) {
        if (flags.get_bool(b.hash) == false) {
            out.push_back(Note(b.name, 0, 1, GameFlags::set_bool(memory, b.hash, true)));
        }
    }
    const auto order = flags.get_uint(kBombGetOrder);
    if (!order && !state.order_missing_noted && flags.get_int(kStockBomb)) {
        state.order_missing_noted = true;  // the flags are up, the UInt is not there
        out.push_back("cheat get order Item_Bomb not found");
    }
    if (order == 0u) {
        std::uint32_t highest = 0;
        for (const std::uint32_t h : kGetOrders) {
            if (const auto v = flags.get_uint(h); v && *v > highest) {
                highest = *v;
            }
        }
        out.push_back(Note("get order Item_Bomb", 0, highest + 1,
                           GameFlags::set_uint(memory, kBombGetOrder, highest + 1)));
    }
    return out;
}

} // namespace CoopCheatItems
