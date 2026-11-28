// Row 299 (owner 2026-11-27): the world keeps running while a player's pause menu is open. Design and evidence:
// docs/pause-world.md. Phase 0 is this probe: reads only, test only.
//
// The game's pause is the PauseMgr's named holds (read from main 1.1.1, build e0f3ccfaf50065ae, 2026-11-27):
// - the manager: main+kPauseMgrSlot is a GOT slot holding the address of the global that holds the manager
//   (SeqOneShotRequestPause's run, main+0xd9aec0: `adrp x9; ldr x9, [x9, #0x6d8]; ldr x0, [x9]`, page 0x84cda000);
// - manager +0: u32 the number of holds, +8: -> the holds, kHoldSize bytes each, sorted by key (the run's binary search);
// - a hold: +8 u32 its key (the hash of its name, main+0x1739e0(name, 0); e.g. Game Over's "GameOverPause",
//   main+0x30310e0), +0x1c u32 how many hold it now (Request: ldadd +1; Release, main+0xdad2c0: cas -1, never below 0);
// - after the count moved, both call the manager's sub-object at +0x48 through its vtable's slot 0: (sub, key, 1 or 0);
// - manager +0x30..+0x3c: a 128-bit set, one bit per hold in force; a screen tests one hold's bit (its key looked up in
//   the manager's +0x40 table), e.g. main+0x2cf094..0x2cf0c4 tests 0x7bf055fc and 0x0778e566, never the menu's keys.
//
// Phase 1 (the host's "World runs in menus", coop_pause_world_mode.h; docs/pause-world.md "Phase 1"): while it is in
// force, each hold in kMenuHolds is let go with the game's own release at the next screen update. Off (the default,
// solo, co-op idle): nothing is released. The probe's parts (the bridge's `state` member `pause_world`, the test step
// `menu hold release N`) stay, test only.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace CoopPauseWorld {

inline constexpr std::uint64_t kPauseMgrSlot = 0x4cba6d8;   // main+: GOT slot -> the global -> the manager
inline constexpr std::uint64_t kHolds = 0x8;                 // manager +: -> the holds
inline constexpr std::uint64_t kHoldSize = 0x20;
inline constexpr std::uint64_t kHoldKey = 0x8;               // hold +: u32 key
inline constexpr std::uint64_t kHoldCount = 0x1c;            // hold +: u32 how many hold it
inline constexpr std::uint64_t kNotify = 0x48;               // manager +: the sub-object called after a change
inline constexpr std::uint64_t kWords = 0x30;                // manager +: four u32 (the UI's test)
inline constexpr std::uint32_t kMostHolds = 512;             // a count above it is not a manager (a wrong read)

// The pause menu's own holds: one screen object keeps one of them by its state (+0x384), from its table at
// main+0x3f16804 {1: opening, 2: open, 3: shutting}; each key is asked and let go only there (main+0x33eb440,
// main+0x2d6880, main+0x33ec974, main+0x2d6840: an immediate scan of main's text finds no other use, and no bit test).
// 0xba597383 is the one phase 0's run saw while the + menu was open.
inline constexpr std::array<std::uint32_t, 3> kMenuHolds{0xb584333eu, 0xba597383u, 0x90ddf88bu};
constexpr bool IsMenuHold(std::uint32_t key) {
    for (const std::uint32_t k : kMenuHolds) {
        if (k == key) return true;
    }
    return false;
}

struct Hold {
    std::uint32_t index = 0;
    std::uint32_t key = 0;
    std::uint32_t count = 0;
    std::array<std::uint64_t, 4> raw{};   // the hold's 32 bytes as read (the probe: which word names it)
};

struct Snapshot {
    bool ok = false;
    std::string why;              // when not ok
    std::uint64_t manager = 0;
    std::uint32_t holds = 0;      // how many the manager has
    std::array<std::uint32_t, 4> words{};
    std::vector<Hold> held;       // the holds with a count above 0
};

// (address, out, size) -> read; the module gives CoopRuntime::Read, the unit test a map.
using ReadFn = std::function<bool(std::uint64_t, void*, std::size_t)>;

Snapshot Take(const ReadFn& read, std::uint64_t main_base);
// The first held hold that is the menu's own (kMenuHolds), if any: the one to let go now.
const Hold* MenuHeld(const Snapshot& s);
// Nothing holds the world (no hold with a count above 0): the menu's hold let go and no event's, Game Over's or other
// menu's in force.
inline bool WorldFree(const Snapshot& s) { return s.ok && s.held.empty(); }
std::string Json(const Snapshot& s);
// Every hold (held or not) as one log line each, for the probe's first look: which keys exist.
std::vector<std::string> AllHoldLines(const ReadFn& read, std::uint64_t main_base);

// ---- the module (coop_pause_world_module.cpp; not in the unit test) ----
void Init(const void* api);   // (const HosCoopApi*)
// The bridge's `state` member: the snapshot now (and, once per boot, every hold to the log).
std::string StateJson();
// Test step `menu hold release N`: the game's own release (SeqOneShotReleasePause's) of hold N, done at the next menu
// screen update (OnScreen, the game's thread). false: N is not a number.
bool TestRelease(const std::string& index);
// Every menu screen's update (hos_coop_game_screen), the game's thread: a pending test release, and, while the host's
// "World runs in menus" is in force, the menu's own holds let go. `menu_up`: a game menu is in front (row 199's test).
void OnScreen(void* ctx, bool menu_up) noexcept;
// A game menu is in front and the world runs under it (this menu's hold let go, nothing else holds the world): our
// Link's input is held off (the input-off wrap) and PLAYER_STATUS sends neither in_menu nor paused. Any thread.
bool WorldRuns() noexcept;

}  // namespace CoopPauseWorld
