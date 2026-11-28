// SPDX-License-Identifier: GPL-3.0-or-later
//
// Row 316 probes (reports/navi-sync-316.md, P2/P3): what our own Navi (the game's Npc_Navi actor) and each co-op fairy
// are doing, read only, for the play-test bridge's `state` member `navi`. Nothing here writes game memory.
//
// Read: the fairy cave caller (coop_fairy.cpp, the calc thread) at the start of a frame, at most every kEveryMs.
// Json: the bridge thread (coop_playtest.cpp StateMembers): the last sample, as JSON.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "coop_puppet.h"

namespace CoopNaviProbe {

inline constexpr std::uint64_t kEveryMs = 100;
inline constexpr std::int32_t kMostSlots = 16;      // Npc_Navi's ASInfo: 6 skeletal + 2 material slots
inline constexpr std::uint64_t kSlotStride = 0x48;  // CoopPuppet::addr kSlots: 0x48 bytes each, the first 8 the slot

struct Slot {
    std::string name;      // the slot's command (slot +0x10), "" none
    float frame = 0.0f;    // slot +0xe8
};

struct Actor {
    std::uint64_t actor = 0;
    std::uint8_t number = 0;              // a fairy's player number (0: our own Navi)
    std::uint32_t state = 0;              // +0x2c (CoopPuppet::addr::kActorState, 4 present)
    CoopPuppet::Vec3 position{};          // +0x31c
    std::array<float, 9> rotation{};      // +0x328, rows of 3
    std::int32_t slot_count = -1;         // the AS controller's slot count; -1 no controller read
    std::vector<Slot> slots;              // every slot (up to kMostSlots), in slot order
    bool read = false;                    // the actor's own fields read
};

struct Sample {
    std::uint64_t ms = 0;                 // steady ms of the read
    std::uint64_t frames = 0;             // reads made, all told
    Actor navi;                           // actor 0: our Npc_Navi not found
    std::vector<Actor> fairies;           // the live fairies (number + actor)
};

// One actor's fields and every AS slot. Failures leave the fields at their defaults.
Actor ReadActor(CoopPuppet::Guest& guest, std::uint64_t actor, std::uint8_t number = 0);

// The same as JSON (one object). Pure.
std::string Json(const Sample& s);

// Kept for the bridge: the calc thread stores, the bridge thread reads. Json() of the last; "null" before any.
// Nothing is read until a bridge `state` has asked StateJson once (Wanted): a game with no play-test bridge never reads.
void Store(Sample s);
std::string StateJson();
bool Wanted();

}  // namespace CoopNaviProbe
