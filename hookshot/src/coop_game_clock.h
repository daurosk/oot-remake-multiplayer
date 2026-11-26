// SPDX-License-Identifier: GPL-3.0-or-later
// The game's clock: a millisecond count that runs only while the game runs its frames. Co-op drives some of the game's
// own menus for the player (back to the title after a join: CoopPauseTab::ToTitle; the title's Continue and Load:
// CoopAutoPress; F6's - menu: CoopPlayersOpen; the Characters hop) by pressing its pad and waiting for the menu to
// answer, and each of those gives up after a time. The menu answers only as the game's frames run, so the wall clock was
// the wrong measure: a game the play-test bridge holds (it runs only the frames it is stepped, docs/playtest.md "Frame
// control"), a game stalled in a load, or the emulator's pause ate the waits with no frame run, and the steps came too
// fast for the game and gave up (test-bridge-c127-4: "back to the title: given up" on both games in every round).
//
// Hookshot calls the module at each of the game's calc frames (HOS_COOP_SITE_FRAME, the frame the bridge holds): each
// frame adds the wall time since the one before, but at most kMostPerFrameMs. The game runs 30 of them a second
// (lichii/hook/coop_frame_clock.h: 33 ms each), so in play the game clock keeps the wall clock's pace and every wait
// stays as long as it was; a gap longer than kMostPerFrameMs is a held, paused or stalled game and counts as one frame.
// Between two frames the clock runs on from the last one, up to kMostPerFrameMs, so a pad read between frames still sees
// it move. Until the first frame (an exe that never calls that site) it is the wall clock, as before.
//
// Logic only (tests/hookshot/game_clock_test.cpp); the module's one clock is CoopGameClock::NowMs().
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace CoopGameClock {

inline constexpr std::uint64_t kMostPerFrameMs = 100;   // (3 frames at 30 a second: under 10 fps the game is held)

class Clock {
public:
    // A game frame ran at wall `wall_ms`.
    void Frame(std::uint64_t wall_ms) {
        if (!seen_) {
            seen_ = true, game_ms_ = wall_ms;   // (on from the wall clock, so stamps taken before stay right)
        } else {
            game_ms_ += Gap(wall_ms);
        }
        wall_ms_ = std::max(wall_ms_, wall_ms);
    }
    // The game's ms at wall `wall_ms`: never less than an earlier answer.
    std::uint64_t Now(std::uint64_t wall_ms) const { return seen_ ? game_ms_ + Gap(wall_ms) : wall_ms; }
    bool Seen() const { return seen_; }

private:
    std::uint64_t Gap(std::uint64_t wall_ms) const {
        return wall_ms > wall_ms_ ? std::min(wall_ms - wall_ms_, kMostPerFrameMs) : 0;
    }
    bool seen_ = false;
    std::uint64_t game_ms_ = 0, wall_ms_ = 0;
};

// A wait that a player's event holds (ToTitle: the pause menu cannot open while an event runs, so a long talk or
// cutscene must not use the wait up): what it has counted at `now`, from `start`, less `held`, the time it was held.
inline std::uint64_t Counted(std::uint64_t start, std::uint64_t held, std::uint64_t now) {
    return now > start && now - start > held ? now - start - held : 0;
}

inline std::uint64_t WallMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// The module's clock (coop_hookshot.cpp Frame calls NoteFrame at HOS_COOP_SITE_FRAME, solo and in co-op). Any thread.
inline std::mutex g_mutex;
inline Clock g_clock;
inline void NoteFrame() {
    std::lock_guard lock(g_mutex);
    g_clock.Frame(WallMs());
}
inline std::uint64_t NowMs() {
    std::lock_guard lock(g_mutex);
    return g_clock.Now(WallMs());
}

}  // namespace CoopGameClock
