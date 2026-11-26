// SPDX-License-Identifier: GPL-3.0-or-later
//
// Row 238 (hearts-238-01112-2; on unless LICHII_COOP_LIFE_GAUGE=0): the hearts and the magic bar on our HUD show what
// co-op wrote into life, max life or magic, as they do for the game's own changes.
//
// The heart gauge (and the magic gauge beside it) keep their own copy of Life and MaxLife: +1876 life, +1880 the shown
// max (float), +1884 the max. They take PlayerStatus.Life / MaxLife in only when the game's own refresh runs
// (main+0x33bc100 for the hearts, main+0x33be680 for magic). The game's refresh action (main+0x28625d0, a virtual)
// finds the HUD by its hash (main+0x837240 on the registry at main+0x4c7e098, hash 0x5c7f1c3c) and calls both. The
// game's own life methods on the game data object (main+0x33a84f0 / 0x33a8630 / 0x33a8760: a set of life, a heart
// container, a refill) send the change to Link's Life component and mark the gauges dirty (+0x3b49 grow, +0x3b4a at
// once) for that refresh. A value co-op writes (the join snapshot, another player's container, a lowering hold, a save
// load window's revert, a reset) goes through the flag setter or Link's component and skips both, so the HUD kept the
// old count (run hs-join-hearts-238 20261123-051052: B's MaxLife and Link at 16, B's HUD still 5 containers).
//
// So after such a write this asks the game's own refresh once, a moment later (after the setter's queue and Link's
// component write-back have landed), on the game's thread through the callers (CoopCallerOrders::kLifeGauge), in play
// only. It writes nothing in the gauges itself. It logs the gauge's own max before and after (the proof's check).
// No emulator or Hookshot code here: standard C++ only (tests/lichii/life_gauge_test.cpp).
#pragma once

#include <cstdint>
#include <mutex>

namespace CoopLifeGauge {

namespace addr {  // main offsets (dump addresses minus 0x7100000000), and the HUD's own fields
inline constexpr std::uint64_t kHudRegistry = 0x4c7e098;   // pointer to the registry object (*it is the lookup's x0)
inline constexpr std::uint64_t kFindByHash = 0x837240;     // (registry, hash) -> the HUD object or 0
inline constexpr std::uint32_t kHudHash = 0x5c7f1c3c;      // the HUD that holds both gauges
inline constexpr std::uint64_t kHudHearts = 792;           // the heart gauge in it (0: none)
inline constexpr std::uint64_t kHudMagic = 800;            // the magic gauge in it (0: none)
inline constexpr std::uint64_t kHeartsRefresh = 0x33bc100; // (gauge): takes Life / MaxLife in
inline constexpr std::uint64_t kMagicRefresh = 0x33be680;  // (gauge): takes MP / MPMax in
inline constexpr std::uint64_t kGaugeLife = 1876;          // s32
inline constexpr std::uint64_t kGaugeShownMax = 1880;      // float
inline constexpr std::uint64_t kGaugeMax = 1884;           // s32
}  // namespace addr

// The flags whose change the gauges show: PlayerStatus.MaxLife, Life, MPMax, MP.
inline constexpr std::uint32_t kMaxLife = 0xfbe01da1u, kLife = 0x31ab5580u, kMPMax = 0x12d63a97u,
                               kMP = 0x76b18200u;
inline constexpr bool Shown(std::uint32_t hash) {
    return hash == kMaxLife || hash == kLife || hash == kMPMax || hash == kMP;
}

// The setter's queue and Link's component write-back take a frame or more: the refresh waits this long after the
// last write.
inline constexpr std::uint64_t kDelayMs = 300;
// While MaxLife and our Link's own max still differ (one has not caught up), it waits at most this long after the
// first write it is asked for; writes coming without end never hold it off longer either.
inline constexpr std::uint64_t kMostWaitMs = 2000;

// When to ask the refresh. Any thread.
class Refresher {
public:
    // A co-op write of life, max life or magic (the flag or Link's component).
    void Want(std::uint64_t now_ms) {
        std::lock_guard lock(mutex_);
        if (due_ms_ == 0) first_ms_ = now_ms;
        due_ms_ = now_ms + kDelayMs;
        if (due_ms_ > first_ms_ + kMostWaitMs) due_ms_ = first_ms_ + kMostWaitMs;
    }
    // Link is in play (a scene loaded, our Link there). Off play the refresh waits.
    void SetInPlay(bool in_play) {
        std::lock_guard lock(mutex_);
        in_play_ = in_play;
    }
    // Whether MaxLife and our Link's own max agree (or one is not known): the session's frame says.
    void SetSettled(bool settled) {
        std::lock_guard lock(mutex_);
        settled_ = settled;
    }
    // True once when the refresh is to be asked now.
    bool Due(std::uint64_t now_ms) {
        std::lock_guard lock(mutex_);
        if (!in_play_ || due_ms_ == 0 || now_ms < due_ms_) return false;
        if (!settled_ && now_ms < first_ms_ + kMostWaitMs) return false;
        due_ms_ = 0;
        return true;
    }

private:
    std::mutex mutex_;
    std::uint64_t due_ms_ = 0;    // 0: nothing wanted
    std::uint64_t first_ms_ = 0;  // the first write since the last refresh
    bool in_play_ = false;
    bool settled_ = true;
};

inline Refresher& TheRefresher() {
    static Refresher refresher;
    return refresher;
}

}  // namespace CoopLifeGauge
