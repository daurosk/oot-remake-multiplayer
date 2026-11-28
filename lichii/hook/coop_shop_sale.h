// SPDX-License-Identifier: GPL-3.0-or-later
//
// The randomizer's one-time shop shelves (shopsanity), bought once for the group (docs/items.md "One-time sales",
// tracker row 100's related case; the Business Scrubs' one-time sales are coop_scrub_sale.h).
//
// Read from OOTRR 0.7.3's source (randomizer/core/src/shelf/stage.rs, shelf/counters.rs, shelf/hook.rs, data/shops.json)
// and the romfs (GameDataList, the 12 shop keepers' ShopParam):
// - A shop shelf slot's stock is the slot's StockNum less its sold counter Shop_SoldNum.<keeper>.<slot> (a saved Int).
//   In the game every one of the 96 counters (12 keepers x 8 slots) resets daily (ResetTypeValue 2) and almost every
//   StockNum is -1 (no limit; the Kokiri Shop's short-stock list has a 1 for its nuts and sticks).
// - With shopsanity (the seed file's "shops"), OOTRR writes every seed shop slot: a one-time slot (a special deal, or any
//   slot of a shop marked "once") gets StockNum 1 and its counters are made saved (ResetTypeValue 8: shelf::counters
//   keep); every other slot gets StockNum -1. A tiered deal (a progressive family) adds up its family's counters to
//   pick the tier (shelf::hook PICK).
// - What a slot sells is shared (items, counts, tiers), its counter was not: after one player's buy every other game
//   still had the slot in stock, and could buy it again: one more heart piece or key for the group, the next tier of a
//   progressive item (the PICK sees its own counters at 0), a second bundle.
// So in a seed with shops the 96 counters are shared by the max rule (FlagStore::is_max): a buy anywhere sells the
// slot out everywhere; a drop (a daily reset in a game, on a slot with no limit) stays in that game. Only in such a
// seed: without shopsanity the counters are the game's daily ones and the short-stock slots must refill in each game.
// Another player's higher counter waits while our Link is in an event (the shop talk and its menu), so a shelf never
// sells out under an open menu; it lands once the talk ends. Known limit, as for the scrubs: two players in the same
// slot's menu at the same moment both buy. Off with LICHII_COOP_SHOP_SALE=0. Co-op idle (no session): nothing changes.
// Pure but for SetSeedHasShops (the module's randomizer says it at its start): tests/lichii/shop_sale_test.cpp.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop/host/flags.hpp"

namespace CoopShopSale {

// The shop keepers whose ShopParam OOTRR's shelves edit (shelf.rs KEEPER_PACKS), each with 8 slots (GoodsList).
inline constexpr std::array<std::string_view, 12> kKeepers{{
    "Npc_Kokiri_001_Shop_Child",
    "Npc_Kokiri_001_Shop_Child_ShortStock",
    "Npc_Kokiri_001_Shop_Adult",
    "Npc_Hylia_018_Shop_Child",
    "Npc_Hylia_018_Shop_Adult",
    "Npc_Hylia_020_Shop_Child",
    "Npc_Hylia_020_Shop_Adult",
    "Npc_Hylia_021_Shop_Child",
    "Npc_Goron_011_Shop_Child",
    "Npc_Goron_011_Shop_Adult",
    "Npc_Zora_011_Shop_Child",
    "Npc_Zora_011_Shop_Adult",
}};
inline constexpr std::uint32_t kSlots = 8;

inline std::string CounterName(std::string_view keeper, std::uint32_t slot) {
    return "Shop_SoldNum." + std::string(keeper) + "." + std::to_string(slot);
}

// The 96 counters' hashes, sorted.
inline const std::vector<std::uint32_t>& Counters() {
    static const std::vector<std::uint32_t> all = [] {
        std::vector<std::uint32_t> v;
        for (const std::string_view k : kKeepers) {
            for (std::uint32_t s = 0; s < kSlots; ++s) v.push_back(coop::host::flag_hash(CounterName(k, s)));
        }
        std::sort(v.begin(), v.end());
        return v;
    }();
    return all;
}

inline bool IsCounter(std::uint32_t hash) {
    const auto& v = Counters();
    return std::binary_search(v.begin(), v.end(), hash);
}

inline bool Plain(const coop::host::FlagKey& key) {
    return key.kind == coop::protocol::FlagKeyKind::named && key.index == 0;
}

// Whether this run of the game plays a randomizer seed with shops (set by the module's randomizer at its start;
// never in the emulator build).
inline std::atomic<bool>& SeedHasShopsFlag() {
    static std::atomic<bool> on{false};
    return on;
}
inline void SetSeedHasShops(bool on) { SeedHasShopsFlag().store(on, std::memory_order_relaxed); }
inline bool SeedHasShops() { return SeedHasShopsFlag().load(std::memory_order_relaxed); }
// Row 331: whether this run's seed places Gold Skulltula tokens off the Skulltulas (set by the module's randomizer at
// its start, CoopRando::SeedTokensOffSkulltulas; never in the emulator build). The hook hands it to the token guard.
inline std::atomic<bool>& SeedTokensElsewhereFlag() {
    static std::atomic<bool> on{false};
    return on;
}
inline void SetSeedTokensElsewhere(bool on) { SeedTokensElsewhereFlag().store(on, std::memory_order_relaxed); }
inline bool SeedTokensElsewhere() { return SeedTokensElsewhereFlag().load(std::memory_order_relaxed); }

// Adds the counters to the shared list; returns how many were new.
inline std::size_t Merge(std::optional<coop::host::SharedFlags>& shared) {
    std::vector<coop::host::FlagKey> keys;
    if (shared) keys = shared->keys();
    const std::size_t before = shared ? shared->size() : 0;
    for (const std::uint32_t h : Counters()) keys.push_back(coop::host::FlagKey(h, std::uint16_t{0}));
    shared = coop::host::SharedFlags(std::move(keys));  // sorts, drops repeats
    return shared->size() - before;
}

// Does another player's counter wait in our game now? While our Link is in an event (a shop talk and its menu).
inline bool Waits(std::uint32_t hash, bool in_event) {
    return in_event && IsCounter(hash);
}

// A store layer: the counters are merged by the highest value (a write lower than or equal to our game's value is
// not made), all else passes to `inner`. The host's save load and a load's write back stay exact (docs/flags.md
// "Saves": the host's save decides).
class Store final : public coop::host::FlagStore {
public:
    explicit Store(coop::host::FlagStore& inner) : inner_(inner) {}

    std::optional<coop::protocol::FlagEntry> read(coop::host::FlagKey key) override { return inner_.read(key); }
    bool write(const coop::protocol::FlagEntry& entry) override {
        if (Plain(entry.key) && IsCounter(entry.key.hash) && entry.kind == coop::protocol::FlagKind::int32) {
            const auto now = inner_.read(entry.key);
            if (now && static_cast<std::int32_t>(now->value) >= static_cast<std::int32_t>(entry.value)) return true;
        }
        return inner_.write(entry);
    }
    bool is_count(coop::host::FlagKey key) override { return inner_.is_count(key); }
    bool is_max(coop::host::FlagKey key) override {
        return (Plain(key) && IsCounter(key.hash)) || inner_.is_max(key);
    }
    bool is_one_way(coop::host::FlagKey key) override { return inner_.is_one_way(key); }
    bool holds(coop::host::FlagKey key) override { return inner_.holds(key); }
    bool busy(coop::host::FlagKey key) override { return inner_.busy(key); }
    bool write_exact(const coop::protocol::FlagEntry& entry) override { return inner_.write_exact(entry); }
    bool write_load_back(const coop::protocol::FlagEntry& entry) override { return inner_.write_load_back(entry); }

private:
    coop::host::FlagStore& inner_;
};

// One-time gifts whose "already given" Bool no list shared, so each player got the gift again (row 237, "OOTRR
// items repeat"; rows 171/179, #244). Each is shared by the pickup rule (0 to 1 goes out, a 1 never back to 0), as
// the scrubs' sales; another player's 1 waits while our Link is in an event (the gift's own scene), so a running
// gift is never cut. Shared in every session: what they hold back is a gift of the group's.
//
// The randomizer's gates (read from OOTRR 0.8.2's data/flow_extra.json; the same rows since 0.7.x): a returning
// visit or a second talk no longer asks IsGet / the pouch for the vanilla item (a shuffled gift never sets it); it asks
// a spare saved Bool (an unused row, ResetTypeValue 8) that OOTRR's flow sets right after the gift. No vanilla flow
// sets them, so nothing changes without a seed that shuffles these spots.
// - The six Great Fairy rewards (commit fdb29c1, "<fountain> Great Fairy Reward"): HavePlayedEvent.Dm_Title_*.
//   The fountain's first visit (HavePlayedEvent.Dm_FairyFountain_*) is shared as story; with these not shared,
//   every other game took the next visit as a returning one with the gift not given and gave it again.
// - Cowsanity's eight cows ("... Cow", kind once_grant): the milk entry asks HavePlayedEvent.Dm_StaffCredit_0023..
//   0030 and, while clear, sets it and hands out the seed's item; milk is repeatable, so each player's cow paid.
// - Medigoron ("GC Medigoron"): his stock check asks the dead talk flag
//   HaveTalked_Special.Npc_Goron_Medigoron_Adult_Talk_Event26 instead of the Giant's Knife; the shop sets it after
//   the hand over: a second player bought the seed's item again.
// The frogs (Zora's River, MiniGame_FrogTenors.bfevfl, read from the romfs): TeachNewMusic pays each song once,
// asked by its own Bool (MiniGame_FrogTenors_<song>, set as the song is taught); the bug game (MushiToriGame) pays
// the Heart Piece while MiniGame_FrogTenors_InsectRepellent is 0 (event 100; the first win sets it, event 63) and a
// Blue Rupee after. The Song of Storms pays a Heart Piece too: event 22 (MiniGame_FrogTenors_StormSong clear) ->
// 167 -> 184 -> fork 141 (146 -> 23 sets the Bool) -> 145 -> 247 GetInEvent Obj_Heart_Piece; the scene's
// HeartPieceAnchor names the same Bool. Under OOTRR ("ZR Frogs <song>", "ZR Frogs Ocarina Game" = event 247, in every
// seed's pool, "ZR Frogs Bug Game") each payout is the seed's item, so every player got all seven again; in the base
// game each player got the song rupees and the group two Heart Pieces more. All six song Bools are shared, so the
// frogs' own talk sets MiniGame_FrogTenors_SongClear (events 13-19) and opens the bug game in every game. The Storms
// Bool is also the only input of Zora's River's water group (coop_room_objects.cpp kGroupFlags): shared, it moves
// that water in every game, as the dungeon groups do. The payout tier (MiniGame_FrogTenors_GetCount) and the bug
// game's win count stay each game's own.
struct GiftFlag {
    std::uint32_t hash;  // Murmur3 x86 32, seed 0, of the name
    std::string_view name;
    std::string_view what;  // for the log
};

inline constexpr std::array<GiftFlag, 22> kOnceGifts{{
    {0xbc9cec9b, "HavePlayedEvent.Dm_Title_0080", "the Death Mountain Trail Great Fairy's gift (Din)"},
    {0x93ada401, "HavePlayedEvent.Dm_Title_0096", "the Hyrule Castle Great Fairy's gift (Power)"},
    {0x0f39b5f4, "HavePlayedEvent.Dm_Title_0110", "the Zora's Fountain Great Fairy's gift (Farore)"},
    {0x573cbeb9, "HavePlayedEvent.Dm_Title_0120", "the Desert Colossus Great Fairy's gift (Nayru)"},
    {0x75241a60, "HavePlayedEvent.Dm_Title_0056", "the outside Ganon's Castle Great Fairy's gift (Courage)"},
    {0x881fb42a, "HavePlayedEvent.Dm_Title_0100", "the Death Mountain Crater Great Fairy's gift (Wisdom)"},
    {0xdd17d3eb, "HavePlayedEvent.Dm_StaffCredit_0023", "the randomizer's Gerudo Fortress cow"},
    {0x88e4cc5a, "HavePlayedEvent.Dm_StaffCredit_0024", "the randomizer's Hyrule Field grotto cow"},
    {0xf0746d5f, "HavePlayedEvent.Dm_StaffCredit_0025", "the randomizer's Lon Lon stable left cow"},
    {0xf064742b, "HavePlayedEvent.Dm_StaffCredit_0026", "the randomizer's Lon Lon stable right cow"},
    {0x4f4c1315, "HavePlayedEvent.Dm_StaffCredit_0027", "the randomizer's Link's house cow"},
    {0xce98af96, "HavePlayedEvent.Dm_StaffCredit_0028", "the randomizer's Gerudo Valley cow"},
    {0xa643c625, "HavePlayedEvent.Dm_StaffCredit_0029", "the randomizer's Death Mountain grotto cow"},
    {0xe099da54, "HavePlayedEvent.Dm_StaffCredit_0030", "the randomizer's Impa's house cow"},
    {0x0e3dad8a, "HaveTalked_Special.Npc_Goron_Medigoron_Adult_Talk_Event26", "the randomizer's Medigoron sale"},
    {0x6e3948e7, "MiniGame_FrogTenors_ZeldaLullaby", "the frogs' Zelda's Lullaby payout"},
    {0x8d068302, "MiniGame_FrogTenors_EponaSong", "the frogs' Epona's Song payout"},
    {0xf675a0f2, "MiniGame_FrogTenors_SariaSong", "the frogs' Saria's Song payout"},
    {0x61305ed4, "MiniGame_FrogTenors_SunSong", "the frogs' Sun's Song payout"},
    {0xcc4216b9, "MiniGame_FrogTenors_TimeSong", "the frogs' Song of Time payout"},
    {0xd578df95, "MiniGame_FrogTenors_StormSong", "the frogs' Song of Storms Heart Piece"},
    {0x8ffec767, "MiniGame_FrogTenors_InsectRepellent", "the frogs' bug game Heart Piece"},
}};

inline const GiftFlag* FindGift(std::uint32_t hash) {
    for (const GiftFlag& f : kOnceGifts) {
        if (f.hash == hash) return &f;
    }
    return nullptr;
}

inline bool IsGift(std::uint32_t hash) { return FindGift(hash) != nullptr; }

// Adds the gift flags to the shared list; returns how many were new.
inline std::size_t MergeGifts(std::optional<coop::host::SharedFlags>& shared) {
    std::vector<coop::host::FlagKey> keys;
    if (shared) keys = shared->keys();
    const std::size_t before = shared ? shared->size() : 0;
    for (const GiftFlag& f : kOnceGifts) keys.push_back(coop::host::FlagKey(f.hash, std::uint16_t{0}));
    shared = coop::host::SharedFlags(std::move(keys));
    return shared->size() - before;
}

// Does another player's gift flag (or shelf counter, in a seed with shops) wait in our game now?
inline bool GiftWaits(std::uint32_t hash, bool in_event) {
    return in_event && IsGift(hash);
}

} // namespace CoopShopSale
