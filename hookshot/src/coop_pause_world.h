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
// - manager +0x30..+0x3c: four u32 the UI's own code tests before it acts (main+0x2cf094..0x2cf0c4): read for the probe.
//
// Nothing here runs unless the play-test bridge asks (its `state` member `pause_world`, and the test step
// `menu hold release N`): solo and co-op idle never reach it.
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
// Every menu screen's update (hos_coop_game_screen): only a pending test release does anything.
void OnScreen(void* ctx) noexcept;

}  // namespace CoopPauseWorld
