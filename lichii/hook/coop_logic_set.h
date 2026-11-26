// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's own set-bool for one of an actor's logic plugs, called from the svc cave (jabu-lifts #65,
// deku-switches #176 #129).
//
// It is the AI node ExecuteLogicSetBool's execute (main+0x2727540, vtable main+0x49a9550; reports/dungeon-sync-b.md)
// on a node block of our own (one game malloc, kept by the caller). The execute reads
//   node+0x00 vtable (slot 0x138 must be main+0x181440, the base: it is), node+0x18 byte bit 1 (enabled),
//   node+0x20 u32 the plug's name key (Murmur3 of the name; the plug's own key is this xor 7),
//   node+0x38 -> the value byte, and the actor at [[[node+8]+8]+0x1f0], its logic at [[actor+0x270]+0x118].
// [actor+0x270] is read without a null check, so before each call the actor must be present and its logic must have
// a live bool plug of that key (BoolPlug).
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "coop_logic_probe.h"
#include "coop_puppet.h"

namespace CoopLogicSet {

namespace addr {
inline constexpr std::uint64_t kExecute = 0x2727540;  // ExecuteLogicSetBool's execute (vtable slot 0xf0)
inline constexpr std::uint64_t kVtable = 0x49a9550;   // ExecuteLogicSetBool's vtable
inline constexpr std::uint64_t kMalloc = 0x14c6c90;   // as coop_puppet.h
// Our node block.
inline constexpr std::uint64_t kNodeFlags = 0x18;     // byte; bit 1 on
inline constexpr std::uint64_t kNodeKey = 0x20;       // u32
inline constexpr std::uint64_t kNodeValue = 0x38;     // -> value byte
inline constexpr std::uint64_t kNodeOwner = 0x08;     // -> owner; [owner+8] -> holder; [holder+0x1f0] = actor
inline constexpr std::uint64_t kOwner = 0x40;         // in the block
inline constexpr std::uint64_t kHolder = 0x80;        // in the block
inline constexpr std::uint64_t kHolderActor = 0x1f0;
inline constexpr std::uint64_t kValueByte = 0x60;     // in the block
inline constexpr std::uint64_t kBlockSize = kHolder + kHolderActor + 0x10;
}  // namespace addr

// The type byte (plug + 0x38) of an INPUT plug: 0 (the probe lists the asker's EventRequestTrigger f446f53d as 0;
// outputs are CoopLogicProbe::kBool, 7, the only type the game's set-bool writes).
inline constexpr std::uint8_t kInputType = 0;

// Every plug of the actor's live logic, or nothing (not there, not live). Reads only.
inline std::optional<std::vector<CoopLogicProbe::Plug>> AllPlugs(CoopPuppet::Guest& guest, std::uint64_t actor) {
    return CoopLogicProbe::Read(
        [&guest](std::uint64_t at) -> std::optional<std::uint64_t> {
            std::uint64_t v = 0;
            if (!guest.Read(at, &v, 8)) return std::nullopt;
            return v;
        },
        actor);
}

// The actor's plug named by `key` (Murmur3 of the name) of type byte `type` in its live logic, or nothing (not
// there, not live, another type). Reads only.
inline std::optional<CoopLogicProbe::Plug> FindPlug(CoopPuppet::Guest& guest, std::uint64_t actor, std::uint32_t key,
                                                    std::uint8_t type) {
    const auto plugs = AllPlugs(guest, actor);
    if (!plugs) return std::nullopt;
    const auto plug = std::find_if(plugs->begin(), plugs->end(), [key, type](const CoopLogicProbe::Plug& p) {
        return p.key == (key ^ 7u) && p.type == type;
    });
    if (plug == plugs->end()) return std::nullopt;
    return *plug;
}

// The actor's bool output plug `key` (what the set-bool's execute writes), or nothing.
inline std::optional<CoopLogicProbe::Plug> BoolPlug(CoopPuppet::Guest& guest, std::uint64_t actor, std::uint32_t key) {
    return FindPlug(guest, actor, key, CoopLogicProbe::kBool);
}

// deku-ladder (A #39): the actor's input plug `key`, or nothing. The game's set-bool refuses an input (it wants type
// 7), so SetInput does the set-bool's own store (f_7102727540 after its checks: the value byte at [plug + 0x28] & ~3).
// It stays only on an UNWIRED input (its value is its own default): a wired input's value is its source's output,
// rewritten every frame by that source, so callers set only inputs the room graph leaves unwired
// (tools/puppet_pack/eye_switches.py --classify).
inline std::optional<CoopLogicProbe::Plug> InputPlug(CoopPuppet::Guest& guest, std::uint64_t actor,
                                                     std::uint32_t key) {
    return FindPlug(guest, actor, key, kInputType);
}

// Whether SetInput can store into `plug`: a value address, and no change mark (bit 1: the set-bool then also calls
// f_7103c41cf0/41d00, which we do not mirror).
inline bool CanSetInput(const CoopLogicProbe::Plug& plug) {
    return (plug.value_word & ~std::uint64_t{3}) != 0 && (plug.value_word & 2) == 0;
}

// review Round 8: nothing in the lifted set-bool says an UNWIRED input's value address is that plug's own; a graph
// may point unwired inputs at a shared default, and then a 1 there flips every input sharing it. So before each
// store the address is checked at run time on this instance, and the store is refused (the old way) unless:
// it is not in the game's static image (a shared constant would be: main .. main + kImageSpan), no other plug of
// the actor (`same_actor`, the plug itself excluded by its node) has it, no other instance of the switch has it for
// the same input (`others`: their value addresses), and it lies within kNearPlug of the plug node (the logic's own
// heap block). Returns why not, or nullptr when the address is the plug's own.
inline constexpr std::uint64_t kImageSpan = 0x8000000;  // main's image + data: the highest address used is ~main+0x4e85000
inline constexpr std::uint64_t kNearPlug = 0x100000;
inline const char* InputNotOwn(const CoopLogicProbe::Plug& plug, std::span<const CoopLogicProbe::Plug> same_actor,
                               std::span<const std::uint64_t> others, std::uint64_t main) {
    if (!CanSetInput(plug)) return "no value address, or the game marks its changes";
    const std::uint64_t at = plug.value_word & ~std::uint64_t{3};
    if (at >= main && at < main + kImageSpan) return "its value is in the game's static image (a shared default)";
    for (const auto& p : same_actor) {
        if (p.node != plug.node && (p.value_word & ~std::uint64_t{3}) == at) return "another plug of the switch shares it";
    }
    for (const std::uint64_t o : others) {
        if ((o & ~std::uint64_t{3}) == at) return "another switch of its kind shares it";
    }
    const std::uint64_t d = at > plug.node ? at - plug.node : plug.node - at;
    if (d > kNearPlug) return "it is not near its plug (not the logic's own block)";
    return nullptr;
}

// Stores `value` into the input `plug` (CanSetInput; callers check InputNotOwn first). False when it cannot.
inline bool StoreInput(CoopPuppet::Guest& guest, const CoopLogicProbe::Plug& plug, std::uint8_t value) {
    if (!CanSetInput(plug)) return false;
    return guest.Write(plug.value_word & ~std::uint64_t{3}, &value, 1);
}

// Stores 1 into the input `plug` (CanSetInput). False when it cannot.
inline bool SetInput(CoopPuppet::Guest& guest, const CoopLogicProbe::Plug& plug) { return StoreInput(guest, plug, 1); }

// Fills our node block so the execute sets `actor`'s plug `key` to 1. False when a write fails.
inline bool WriteNode(CoopPuppet::Guest& guest, std::uint64_t block, std::uint64_t main, std::uint64_t actor,
                      std::uint32_t key) {
    const std::uint64_t vtable = main + addr::kVtable;
    const std::uint64_t owner = block + addr::kOwner;
    const std::uint64_t holder = block + addr::kHolder;
    const std::uint64_t value_at = block + addr::kValueByte;
    const std::uint8_t flags = 2;
    const std::uint8_t one = 1;
    return guest.Write(block, &vtable, 8) && guest.Write(block + addr::kNodeOwner, &owner, 8) &&
           guest.Write(block + addr::kNodeFlags, &flags, 1) && guest.Write(block + addr::kNodeKey, &key, 4) &&
           guest.Write(block + addr::kNodeValue, &value_at, 8) && guest.Write(value_at, &one, 1) &&
           guest.Write(owner + 8, &holder, 8) && guest.Write(holder + addr::kHolderActor, &actor, 8);
}

inline CoopPuppet::Call Malloc(std::uint64_t main) {
    CoopPuppet::Call c;
    c.function = main + addr::kMalloc;
    c.x[0] = addr::kBlockSize;
    return c;
}

inline CoopPuppet::Call Execute(std::uint64_t main, std::uint64_t block) {
    CoopPuppet::Call c;
    c.function = main + addr::kExecute;
    c.x[0] = block;
    return c;
}

}  // namespace CoopLogicSet
