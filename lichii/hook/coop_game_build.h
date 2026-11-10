// SPDX-License-Identifier: GPL-3.0-or-later
// The game builds co-op knows, and where things are in each one's main.
// docs/game-builds.md says what every entry is and how to add a build.
//
// Today only the hook's build check reads this table (coop_hook.cpp). The
// readers still use their own constants (coop_puppet.h addr, coop_world.h,
// coop_fairy.h, coop_svc.h, coop_hold.h, coop/host/src/addresses.hpp and a
// few .cpp files); tests/lichii/game_build_test.cpp checks that this table
// and those constants agree, so moving a reader onto the table later is a
// plain swap. Struct offsets (fields inside the game's objects) are not here:
// they may well be the same in another build, which is not proven.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace CoopGameBuild {

// Offsets into main. Pointers are global slots that hold a pointer; calls are
// functions the cave or the hook calls; patches are instructions the loader
// rewrites (CoopSvc::PatchMain).
struct MainOffsets {
    // Global pointer slots.
    std::uint64_t actor_manager_ptr;   // CoopPuppet::addr::kActorManagerPtr
    std::uint64_t game_flags_ptr;      // coop_flags.cpp, game_flags.cpp kManagerPtr
    std::uint64_t game_ui_module_ptr;  // addresses.hpp kGameUiModulePtr
    std::uint64_t scene_module_ptr;    // addresses.hpp kSceneModulePtr, coop_pose.cpp
    std::uint64_t banc_module_ptr;     // addresses.hpp kBancModulePtr
    std::uint64_t event_manager_ptr;   // addresses.hpp kEventManagerPtr
    std::uint64_t world_mgr_ptr;       // coop_time.cpp kWorldMgrPtr
    std::uint64_t foliage_module_ptr;  // CoopWorld::kFoliageModule
    // Data.
    std::uint64_t actor_vtable;        // CoopPuppet::addr::kActorVtable
    std::uint64_t got_create_default;  // CoopPuppet::addr::kGotCreateDefault
    // Functions.
    std::uint64_t create_actor;        // kCreate
    std::uint64_t malloc;              // kMalloc
    std::uint64_t request_delete;      // kRequestDelete
    std::uint64_t take_created;        // kTakeCreated
    std::uint64_t drop_create;         // kDropCreate
    std::uint64_t play_as;             // kPlayAs
    std::uint64_t set_as_frame;        // kSetAsFrame
    std::uint64_t sync_as_slot;        // kSyncAsSlot
    std::uint64_t equip;               // kEquip
    std::uint64_t equip_change_state;  // kEquipChangeState
    std::uint64_t set_matrix;          // kSetMatrix
    std::uint64_t set_time;            // coop_time.cpp kSetTime
    std::uint64_t cut_foliage;         // CoopWorld::kCutFoliage
    // Code patches. The caves (coop_svc.s, coop_hold.s) are linked at these
    // addresses and branch to frame_step, so a new build needs them
    // re-linked, not only new numbers.
    std::uint64_t frame_step;          // the call the svc hooks replace
    std::uint64_t svc_hook_rare;       // CoopSvc::kHooks[0]
    std::uint64_t svc_hook_frame;      // CoopSvc::kHooks[1]
    std::uint64_t svc_cave;            // CoopSvc::kCaveOffset
    std::uint64_t cave_end;            // CoopSvc::kCaveEnd (rodata starts)
    std::uint64_t hold_hook;           // CoopHold::kHookOffset
    std::uint64_t hold_cave;           // CoopHold::kCaveOffset
    std::uint64_t hold_table;          // CoopHold::kTableOffset
};

struct Build {
    std::string_view id;       // first 8 bytes of the build id, lower-case hex
    std::string_view main_id;  // main's full build id, upper-case hex (loader)
    std::string_view version;  // the game's display version
    MainOffsets main;
};

inline constexpr std::array<Build, 1> kBuilds{{
    {"e0f3ccfaf50065ae", "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348", "1.1.1",
     MainOffsets{
         .actor_manager_ptr = 0x4e91888,
         .game_flags_ptr = 0x4e842a8,
         .game_ui_module_ptr = 0x4e7cfa8,
         .scene_module_ptr = 0x4e97648,
         .banc_module_ptr = 0x4e919d0,
         .event_manager_ptr = 0x4e94840,
         .world_mgr_ptr = 0x4e7eab0,
         .foliage_module_ptr = 0x4e22540,
         .actor_vtable = 0x4b25e18,
         .got_create_default = 0x4c7dad0,
         .create_actor = 0x372a70,
         .malloc = 0x14c6c90,
         .request_delete = 0xa6490,
         .take_created = 0x36fbd0,
         .drop_create = 0x36fde0,
         .play_as = 0x2022a0,
         .set_as_frame = 0x37b5d70,
         .sync_as_slot = 0x3abec10,
         .equip = 0x2df7b20,
         .equip_change_state = 0xe34c40,
         .set_matrix = 0x4c6580,
         .set_time = 0x34522d0,
         .cut_foliage = 0xb80800,
         .frame_step = 0x3b4e300,
         .svc_hook_rare = 0x6e6dd8,
         .svc_hook_frame = 0x6e70f8,
         .svc_cave = 0x3c46280,
         .cave_end = 0x3c47000,
         .hold_hook = 0x369df8,
         .hold_cave = 0x3c46300,
         .hold_table = 0x3c46400,
     }},
}};

// Only coop_hook.cpp reads the table; the readers still use the 1.1.1
// constants. Remove this when they take their offsets from the entry.
static_assert(kBuilds.size() == 1,
              "readers still use the 1.1.1 constants: docs/game-builds.md, one-time steps");

// The build co-op was made on; HELLO names it.
inline constexpr const Build& kPrimary = kBuilds[0];

// `build` as hex, either case: the 8-byte id or main's whole one. Null when
// co-op does not know that build.
inline const Build* Find(std::string_view build) {
    const auto same = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) ==
                          std::tolower(static_cast<unsigned char>(y));
               });
    };
    for (const auto& b : kBuilds) {
        if (same(build, b.id) || same(build, b.main_id)) {
            return &b;
        }
    }
    return nullptr;
}

// What co-op works with, for messages: "1.1.1 (build e0f3ccfaf50065ae)".
inline std::string Supported() {
    std::string out;
    for (const auto& b : kBuilds) {
        if (!out.empty()) {
            out += " or ";
        }
        out += std::string(b.version) + " (build " + std::string(b.id) + ")";
    }
    return out;
}

// The panel line when the running game is another build. Short: it is drawn
// over the game.
inline std::string PanelRefusal(std::string_view running) {
    return "Co-op off: game build " + std::string(running.substr(0, 16)) +
           " not supported, needs version " + std::string(kPrimary.version);
}

// The log line, once per game start: what runs, what co-op needs, what to do.
inline std::string LogRefusal(std::string_view running) {
    return "game build " + std::string(running.substr(0, 16)) +
           " is not supported; co-op needs version " + Supported() +
           ". Install that game update (docs/game-builds.md); nothing is read or sent until then";
}

} // namespace CoopGameBuild
