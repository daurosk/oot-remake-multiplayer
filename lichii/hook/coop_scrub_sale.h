// SPDX-License-Identifier: GPL-3.0-or-later
//
// Business Scrubs' one-time sales (tracker row 100 follow-up, docs/items.md
// "One-time sales"): a sale bought in one game is bought in every game.
//
// Read from the romfs (build e0f3ccfaf50065ae, 1.1.1; GameDataList, Banc,
// Logic, Event/EventFlow/Enemy_Scrub_Business_Common):
// - A one-time sale is the shop flow's Onetime_Shop: after the buy it sets a
//   saved Bool (ResetTypeValue 8) named after the scrub, and the scrub's
//   prefab graph deletes the scrub from that Bool (HoldGameDataGetBoolSimple
//   -> PrefabAllDelete): Logic/Enemy_Scrub_Business_DekuNutsPouch_a676,
//   _DekuStickPouch_300c, _Heart_Piece_86ac and
//   DeathMountainCrater_Child_Prefab_0df2. The four Bools below are every
//   such flag in GameDataList (every saved Bool whose name holds
//   "_Enemy_Scrub_Business_"); nothing in the romfs sets one back to 0.
// - What the sale gives is a shared item: the nut bag and stick bag (max),
//   a heart piece (a count: each buy adds one for the whole group).
// - These Bools were not shared, so after one player's buy every other game
//   still had its scrub: a second bag step (the menu picks Item_NutBag_Large
//   once IsGet Medium is shared) or a second heart piece for the group.
// - The randomizer (OOTRR 0.6, setting shuffle_scrubs_b) turns the Lost Woods
//   grotto's rear scrub and Jabu-Jabu's scrub into one-time sales of a placed
//   item: its new flow entry asks a spare saved Bool and leaves at once when
//   it is set (crates/core/src/scrubs_b/flow.rs, data/scrubs_b.json). The same
//   rule, so those two Bools are here too. Nothing else sets them: their
//   names are flow entries no placement uses.
// Left out on purpose: the Boss Rush room's scrubs (DreamWorld_Scrub_Dream_*,
// ResetTypeValue 1: one run's own state, not the save), the menu scrubs that
// sell again and again (seeds, nuts, bombs, potions, arrows), and the Deku and
// Mad Scrubs (enemies; their drops are not saved).
//
// Shared as a pickup (FlagWatcher::share_pickup_named, as a chest's opened
// Bool): 0 to 1 goes out, a 1 never goes back to 0. Another player's 1 waits
// in our game (coop_hook's HoldFlagStore) while that flag's scrub may be in an
// event of ours, so the event never loses its scrub (issue 144's softlock,
// the same graph rule):
// - the scrub is an event member in our Link's event (Busy, from the objects
//   walk: CoopEventMember::InSameEvent, the game's own member test), at any
//   distance. The heart-piece and Jabu sellers' AttackedEventName starts their
//   talk when a deflected nut hits them, from up to the scrub's reach, and
//   event_running has not been seen during a scrub's event;
// - our Link is within kNearM of the scrub, in its scene (the walk's actor
//   place, and the Banc place when the walk has not seen it);
// - our Link is in an event (event_running, or coop_scrub_talk.h's Hold).
// Once none holds, it lands, and the scrub is gone as in the buyer's game.
// Known limit: two players in the same sale menu at the same moment both buy
// (the hold stops the softlock, not that race). Off with
// LICHII_COOP_SCRUB_SALE=0.
// Pure: tested in tests/lichii/scrub_sale_test.cpp.
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "coop/host/flags.hpp"

namespace CoopScrubSale {

struct SaleFlag {
    std::uint32_t hash;  // Murmur3 x86 32, seed 0, of the name
    std::string_view name;
    std::string_view what;  // for the log
};

inline constexpr std::array<SaleFlag, 6> kSaleFlags{{
    {0x5ef24799, "Lostwood_Enemy_Scrub_Business_DekuNutsPouch_01", "the Lost Woods grotto's nut bag scrub"},
    {0x8c98cf5a, "LostWood_Enemy_Scrub_Business_DekuStickPouch_01", "the stick bag scrub (Lost Woods, Kokiri Forest)"},
    {0xe41e1597, "HyruleField_Enemy_Scrub_Business_HeartPiece_01", "Hyrule Field's heart piece scrub"},
    {0x6b8fc751, "DeathMountain_Enemy_Scrub_Business_HeartPiece_01", "Death Mountain Crater's heart piece scrub"},
    // OOTRR's one-time sales (shuffle_scrubs_b), spare flags it reuses:
    {0x1ef2f533, "HavePlayedEvent.Enemy_Scrub_Business_DekuStick_01", "the randomizer's Lost Woods grotto rear scrub"},
    {0x25359a9e, "HavePlayedEvent.Enemy_Scrub_Business_RedPotion_01", "the randomizer's Jabu-Jabu scrub"},
}};

inline const SaleFlag* Find(std::uint32_t hash) {
    for (const SaleFlag& f : kSaleFlags) {
        if (f.hash == hash) return &f;
    }
    return nullptr;
}

inline bool IsSaleFlag(std::uint32_t hash) {
    return Find(hash) != nullptr;
}

// Where each flag's scrub stands: the Banc placement (Translate, metres, as
// Link's position), by the scene name the game reports (Banc file name).
struct Seller {
    std::uint32_t flag;
    std::string_view scene;
    float x, y, z;
    std::uint64_t placement;  // the Banc placement hash (the objects walk's hash)
};

inline constexpr std::array<Seller, 12> kSellers{{
    {0x5ef24799, "LostWood_Child", 7.3f, 82.9f, -85.6f, 0x3c12c790a6a89781ULL},
    {0x5ef24799, "LostWood_Adult", 7.3f, 82.9f, -85.6f, 0x4fff7b57ec78ef4bULL},
    {0x8c98cf5a, "LostWood_Child", -45.2f, 95.2f, 72.6f, 0x4b08aaeb6b969caaULL},
    {0x8c98cf5a, "LostWood_Adult", -45.2f, 95.2f, 72.6f, 0x9b531635ddafa170ULL},
    {0x8c98cf5a, "KokiriForest_Child", -128.0f, 114.6f, -15.3f, 0x0286b51989353c02ULL},
    {0x8c98cf5a, "KokiriForest_Adult", -128.0f, 114.6f, -15.3f, 0x67ec5015aa8a81c0ULL},
    {0xe41e1597, "HyruleField_Child", 25.0f, 129.9f, 22.1f, 0x8fd5fcd13a29513fULL},
    {0xe41e1597, "HyruleField_Adult", 25.0f, 129.9f, 22.1f, 0x1a338ca8b7b910d2ULL},
    {0x6b8fc751, "DeathMountainCrater_Child", -23.0f, 119.1f, 33.0f, 0x22f23970504a8bcfULL},
    // OOTRR's: the grotto's rear scrub (the child seed and adult arrow
    // scrubs' spot) and Jabu-Jabu's scrub.
    {0x1ef2f533, "LostWood_Child", 8.8f, 82.9f, -89.4f, 0x1eba012e68c1bfceULL},
    {0x1ef2f533, "LostWood_Adult", 8.8f, 82.9f, -89.4f, 0xcae41233352d9551ULL},
    {0x25359a9e, "InsideJabuJabuBelly", -27.7f, 74.1f, -42.5f, 0x83c9e5108c604316ULL},
}};

// The scrub's own reach, from its PerceptionReceiverParam
// (Pack/Actor/Enemy_Scrub_Business): Discover 5/7 m, Track 9 m, Alert 10 m
// across. Within it the scrub fights (spits nuts at Link), and a deflected nut
// starts the heart-piece and Jabu sellers' attacked event (AttackedEventName).
// Alert plus a 1 m margin. (A talk itself starts within about 5.3 m:
// coop_scrub_talk.h.) An event further away is held by Busy (in_event).
inline constexpr float kAlertM = 10.0f;
inline constexpr float kNearM = kAlertM + 1.0f;

inline const Seller* FindSeller(std::uint64_t placement) {
    for (const Seller& s : kSellers) {
        if (s.placement == placement) return &s;
    }
    return nullptr;
}

// The index of a sale flag in kSaleFlags (Busy's bit); -1 when none.
inline int IndexOf(std::uint32_t hash) {
    for (std::size_t i = 0; i < kSaleFlags.size(); ++i) {
        if (kSaleFlags[i].hash == hash) return static_cast<int>(i);
    }
    return -1;
}

// Written by the objects walk, read by the flag hold (another thread): the
// last time each sale flag's scrub was seen bound into an event of ours
// (CoopEventMember::InSameEvent) or within kNearM of our Link.
class Busy {
public:
    static constexpr std::uint64_t kFreshUs = 1'500'000;  // a walk is ~4 a second; it may stop

    // One scrub of the walk: its placement hash, its distance to our Link
    // (not finite: not known) and whether it is an event member in our Link's
    // event (nullopt: not read).
    void See(std::uint64_t placement, float distance_m, std::uint64_t now_us, std::optional<bool> in_event) {
        const Seller* s = FindSeller(placement);
        if (s == nullptr) return;
        const bool near = std::isfinite(distance_m) && distance_m <= kNearM;
        if (!near && !in_event.value_or(false)) return;
        const int i = IndexOf(s->flag);
        if (i >= 0) last_us_[static_cast<std::size_t>(i)].store(now_us, std::memory_order_relaxed);
    }
    // The sale flags held now, a bit each (bit = index in kSaleFlags).
    std::uint32_t Mask(std::uint64_t now_us) const {
        std::uint32_t m = 0;
        for (std::size_t i = 0; i < kSaleFlags.size(); ++i) {
            const std::uint64_t at = last_us_[i].load(std::memory_order_relaxed);
            if (at != 0 && now_us >= at && now_us < at + kFreshUs) m |= 1u << i;
        }
        return m;
    }
    void Reset() {
        for (auto& a : last_us_) a.store(0, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<std::uint64_t>, kSaleFlags.size()> last_us_{};
};

// The one the hook and the objects walk share.
inline Busy& TheBusy() {
    static Busy busy;
    return busy;
}

// Is our Link within kNearM of a scrub that `flag` deletes, in its scene?
inline bool Near(std::uint32_t flag, std::string_view scene, float x, float y, float z) {
    for (const Seller& s : kSellers) {
        if (s.flag != flag || s.scene != scene) continue;
        const float dx = x - s.x, dy = y - s.y, dz = z - s.z;
        if (dx * dx + dy * dy + dz * dz <= kNearM * kNearM) return true;
    }
    return false;
}

// What our game is doing this frame, as the flag hold sees it.
struct Here {
    bool in_event = false;   // our Link is in an event (event_running, or coop_scrub_talk.h's Hold)
    std::uint32_t busy = 0;  // TheBusy().Mask(): from the objects walk
    bool known = false;      // a reading this frame (scene and position)
    std::string_view scene;
    float x = 0, y = 0, z = 0;
};

// Does another player's write of `hash` wait in our game now?
inline bool Waits(std::uint32_t hash, const Here& here) {
    const int i = IndexOf(hash);
    if (i < 0) return false;
    if (here.in_event || (here.busy & (1u << i)) != 0) return true;
    return here.known && Near(hash, here.scene, here.x, here.y, here.z);
}

// Adds the sale flags to the shared list; returns how many were new.
inline std::size_t Merge(std::optional<coop::host::SharedFlags>& shared) {
    std::vector<coop::host::FlagKey> keys;
    if (shared) keys = shared->keys();
    const std::size_t before = shared ? shared->size() : 0;
    for (const SaleFlag& f : kSaleFlags) keys.push_back(coop::host::FlagKey(f.hash, std::uint16_t{0}));
    shared = coop::host::SharedFlags(std::move(keys));  // sorts, drops repeats
    return shared->size() - before;
}

} // namespace CoopScrubSale
