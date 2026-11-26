// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The hit replay (docs/objects.md, "Hit replay"): a hit to give an enemy copy, queued on the present thread and
// written into its Life component on the game thread before its Life step (CoopObjects' Mover runs it from its
// Step). Pure over CoopPuppet::Guest, so the tests run the same code with a fake guest.
//
// Locks (boss-intro run 080505: B's game ended with resource_deadlock_would_occur): the Mover's Step holds its
// own mutex while it runs the replay, so nothing here ever takes the Mover's lock. The queue and the replayed
// ids each have their own mutex, taken only around their own containers, and never while writing the guest.

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "coop_puppet.h"

namespace CoopHitReplay {

// The Life component (offsets as in coop_objects.cpp's notes, live dump 2026-11-09).
inline constexpr std::uint64_t kActorComponents = 0x270;
inline constexpr std::uint64_t kComponentLife = 0x2b8;
inline constexpr std::uint64_t kHitHeader = 0x1f8;
inline constexpr std::uint64_t kHitAmount = 0x224;
inline constexpr std::uint64_t kHitRecord = 0x240;
inline constexpr std::uint64_t kLastHitId = 0x1b18;
inline constexpr std::uint64_t kHitLateMs = 200;  // under Sync::kHurtWaitUs: a hit that waited longer is dropped

// The Kokiri Sword hit a ReDead took (live dump, 2026-11-09): the incoming hit at +0x240..+0x2d8 (attacker
// actor +0x240, hit id +0x248, present +0x24c, attack kind +0x250, the hit's place +0x27c, the attacker's place
// +0x2a0). The list links from +0x2d8 on are left alone. Our own player stands in as the attacker.
inline constexpr std::array<std::uint32_t, 0x98 / 4> kHitTemplate{
    0x472d7288, 0x00000021, 0x000000b9, 0x00000001,  // +0x240 attacker, id, present
    0x00060001, 0x40040000, 0x00000014, 0x00000000,  // +0x250
    0x00220000, 0x00001000, 0x0000000a, 0x00000000,  // +0x260
    0x00000200, 0x00000000, 0x00000000, 0xc21085f6,  // +0x270, hit place from +0x27c
    0x42c63e88, 0x40501269, 0x3e76ed80, 0xb3c00000,  // +0x280
    0xbf787210, 0x40588db0, 0xc1129418, 0xc0cb90a0,  // +0x290
    0xc210600c, 0x42c45791, 0x409352f5, 0xbdcc4eb0,  // +0x2a0 attacker place
    0x3ecc555c, 0xc0d481b8, 0x000343ff, 0x00040000,  // +0x2b0
    0x00000004, 0x000000be, 0x00000002, 0x03000000,  // +0x2c0 (+0x2c4 id again)
    0x03010102, 0x00030000,                          // +0x2d0
};

using Record = std::array<std::uint32_t, 0x98 / 4>;  // the incoming hit, +0x240..+0x2d8 (protocol 14's HitRecord)

// Indexes into a Record.
inline constexpr std::size_t kRecPresent = 3;
inline constexpr std::size_t kRecKind = 0x250 / 4 - kHitRecord / 4;   // attack kind, two words (the header's too)
inline constexpr std::size_t kRecPlace = 0x27c / 4 - kHitRecord / 4;  // the hit's place, 3 floats
inline constexpr std::size_t kRecFrom = 0x2a0 / 4 - kHitRecord / 4;   // the attacker's place, 3 floats
inline constexpr std::size_t kRecIdAgain = 0x2c4 / 4 - kHitRecord / 4;
// The words taken from another game's record (review-pr-d crash guard): only its hit kind, +0x250 up to the
// hit's place (+0x27c): attack kind and its parameters, plain values. Every other word (pointers, list
// links, ids of the other process) stays kHitTemplate's; attacker, id, present and places are ours.
inline constexpr std::size_t kRecKindEnd = kRecPlace;

// kHitTemplate with another game's hit kind in it (words kRecKind up to kRecKindEnd), nothing else of it.
inline Record WithHitKind(const Record& other) {
    Record rec = kHitTemplate;
    for (std::size_t i = kRecKind; i < kRecKindEnd; ++i) {
        rec[i] = other[i];
    }
    return rec;
}

struct Hit {
    std::uint64_t actor = 0;      // the copy
    std::uint64_t attacker = 0;   // our player, or the hitter's puppet (ReplayAttacker)
    std::int32_t amount = 0;
    std::array<float, 3> at{};    // the copy's place
    std::array<float, 3> from{};  // the hitter's place (ours when not known)
    std::uint64_t asked_ms = 0;   // set by Add
    bool exact = false;           // `at` is where the hit landed, not the actor's foot
    // Protocol 14 (every hit shows on every screen): the hit that happened in the other game. Only its hit kind
    // (weapon, element: WithHitKind) goes into this game's damage step; the rest is kHitTemplate's and ours.
    std::optional<Record> record;
};

// Row 177 (every boss, every enemy): the actor a replayed hit names as its attacker (the record's +0x240). The
// game's own battle AI draws its target from that hit (EnemyCommon.BattleCommon's ExecuteMemoryPushLastAttacker):
// with our Link standing in for every other player's hit, the owner's enemy turned on the owner's Link after each
// of them (Phantom Ganon's light balls always at player 1). The hitter's puppet when it is one enemies see here
// (Director::LiveSeenActor) and they can fight now (not unseen: downed, spectating, away); else our Link, as before.
// A killing hit (lethal: it takes the last of the life) keeps our Link: the game's death consequences (room clear
// counters, drops, kill flags of Gold Skulltulas, Big Poes, mid-bosses) may check that the killer is the Player,
// and a dead enemy's target memory no longer matters.
// A boss (row 178, `boss`: CoopBossNative::Table::KeepsLinkAsAttacker) keeps our Link too: its attacks wait on the
// game's attack permission for its target (target info isForbidAttackPermission), given to the Player only, so a
// puppet in its target memory held Phantom Ganon's phase 2 Fly with no throw (~22 s, until our stall check). Its
// own perception still picks every player's puppet it sees (Gohma's turn to player 2, issue 150).
inline std::uint64_t ReplayAttacker(std::uint64_t our_link, std::uint8_t hitter, std::uint64_t hitter_puppet,
                                    bool hitter_unseen, bool lethal = false, bool boss = false) {
    return !lethal && !boss && hitter != 0 && hitter_puppet != 0 && !hitter_unseen ? hitter_puppet : our_link;
}

// The replayed hit takes the last of the life: a hurt that is at least our life now (when read), or the owner's
// life after it is 0 or less (the hitter's game killed it).
inline bool LethalHit(std::int64_t life_now, std::int64_t hurt, std::int64_t life_after) {
    return hurt > 0 && ((life_now >= 0 && hurt >= life_now) || life_after <= 0);
}

// One hit written: for the caller's log and the difficulty's pass.
struct Written {
    std::uint64_t actor = 0;
    std::uint64_t life = 0;  // its Life component
    std::int32_t amount = 0;
    std::uint32_t id = 0;
    bool exact = false;
    bool real = false;  // the other game's own hit record (protocol 14), not the stand-in
};

class Replayer {
public:
    static constexpr std::size_t kQueued = 64;

    // Present thread: a hit to replay at the next game frame (at most kQueued wait).
    void Add(Hit hit, std::uint64_t now_ms) {
        std::lock_guard lock(hits_mutex_);
        if (hits_.size() < kQueued) {
            hit.asked_ms = now_ms;
            hits_.push_back(hit);
        }
    }

    // Game thread (the Mover's Step, under the Mover's own lock): writes every queued hit not older than
    // kHitLateMs into its copy's Life component. `alive(actor)` says whether the actor is still one to hit.
    std::vector<Written> Run(CoopPuppet::Guest& guest, std::uint64_t now_ms,
                             const std::function<bool(std::uint64_t)>& alive) {
        std::vector<Hit> hits;
        {
            std::lock_guard lock(hits_mutex_);
            hits.swap(hits_);
        }
        std::vector<Written> out;
        for (const Hit& h : hits) {
            // A hit that waited too long (no game frame ran) is dropped.
            if (now_ms >= h.asked_ms + kHitLateMs || !alive(h.actor)) {
                continue;
            }
            if (const auto w = Write(guest, h)) {
                out.push_back(*w);
            }
        }
        return out;
    }

    // Any thread: the hit id we last wrote into this Life component (0xffffffff: none).
    std::uint32_t ReplayedId(std::uint64_t life) {
        std::lock_guard lock(ids_mutex_);
        const auto it = ids_.find(life);
        return it == ids_.end() ? 0xffffffffu : it->second;
    }

    // A new scene: its Life components may sit where the old ones were.
    void Forget() {
        std::lock_guard lock(ids_mutex_);
        ids_.clear();
    }

    std::size_t Waiting() {
        std::lock_guard lock(hits_mutex_);
        return hits_.size();
    }

private:
    std::optional<Written> Write(CoopPuppet::Guest& guest, const Hit& h) {
        std::uint64_t table = 0;
        std::uint64_t life = 0;
        if (!guest.Read(h.actor + kActorComponents, &table, 8) || table == 0 ||
            !guest.Read(table + kComponentLife, &life, 8) || life == 0) {
            return std::nullopt;
        }
        std::uint32_t last = 0;
        guest.Read(life + kLastHitId, &last, 4);
        const std::uint32_t id = (last == 0xffffffffu ? 0x1000u : last + 2u) & 0x7fffffffu;
        const bool real = h.record.has_value();
        Record rec = real ? WithHitKind(*h.record) : kHitTemplate;
        std::memcpy(&rec[0], &h.attacker, 8);
        rec[2] = id;
        rec[kRecIdAgain] = id;
        const std::array<float, 3> at{h.at[0], h.at[1] + (h.exact ? 0.0f : 1.0f), h.at[2]};
        std::memcpy(&rec[kRecPlace], at.data(), 12);
        std::memcpy(&rec[kRecFrom], h.from.data(), 12);
        rec[kRecPresent] = 0;  // present last, below
        // The attack header names the same attack kind as the record (the template's: a sword).
        const std::array<std::uint32_t, 4> header{rec[kRecKind], rec[kRecKind + 1], id, 2};
        const std::array<std::int32_t, 2> amount{4, h.amount};
        {
            std::lock_guard lock(ids_mutex_);
            ids_[life] = id;
        }
        guest.Write(life + kHitHeader, header.data(), sizeof(header));
        guest.Write(life + kHitAmount, amount.data(), sizeof(amount));
        guest.Write(life + kHitRecord, rec.data(), sizeof(rec));
        const std::uint32_t present = 1;
        guest.Write(life + kHitRecord + 0xc, &present, 4);
        return Written{h.actor, life, h.amount, id, h.exact, real};
    }

    std::mutex hits_mutex_;
    std::vector<Hit> hits_;  // under hits_mutex_
    std::mutex ids_mutex_;
    std::unordered_map<std::uint64_t, std::uint32_t> ids_;  // Life component: the hit id we wrote last
};

} // namespace CoopHitReplay
