// SPDX-License-Identifier: GPL-3.0-or-later
// When a reading one thread left for another is too old to use (hud-fresh-0121).
//
// The hook's frame (markers, map view, our own scene) and the party HUD's blocks are written once a present and read
// on the game's own thread. They used to be dropped once a second old by the wall clock alone. A game that runs one
// frame a second or slower (a slow PC, a long hitch, the play-test bridge stepping frames: testkit-0120's kit-in-play
// shots, about 1.5 s a frame, had no party HUD and no name tags) then read every frame's reading as stale and hid them,
// although nothing newer could exist. Now a reading is stale only when it is old by the wall clock AND that many
// presents went by without a new one: a load or a menu (presents go on, the reading stops) still drops it as before,
// a slow game keeps its last one. Header only, standard C++ only, so the tests build it.
#pragma once

#include <atomic>
#include <cstdint>

namespace CoopFresh {

// Presents so far: ticked once a present by the hook's Frame (CoopHook, every host).
inline std::atomic<std::uint64_t> g_presents{0};
inline void Present() { g_presents.fetch_add(1, std::memory_order_relaxed); }
inline std::uint64_t Presents() { return g_presents.load(std::memory_order_relaxed); }

// A reading written once a present: two presents without a new one is a present that left it out.
inline constexpr std::uint64_t kStalePresents = 2;

// A reading `age_ms` old by the wall clock, written at present `made_at`, read at present `now`: stale only when it
// is at least `max_ms` old AND at least `max_presents` presents older than now.
inline constexpr bool Stale(std::uint64_t age_ms, std::uint64_t max_ms, std::uint64_t made_at, std::uint64_t now,
                            std::uint64_t max_presents = kStalePresents) {
    return age_ms >= max_ms && now > made_at && now - made_at >= max_presents;
}

}  // namespace CoopFresh
