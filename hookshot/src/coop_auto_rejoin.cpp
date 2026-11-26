// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_auto_rejoin.h"

namespace CoopAutoRejoin {

bool Planner::Tick(const Facts& f, std::uint64_t now_ms) {
    if (first_ms_ == 0) {
        first_ms_ = now_ms == 0 ? 1 : now_ms;
    }
    // The player's own Host, Join or Leave: nothing planned stays (and the start's join is not made after it).
    // So too when the session turned this game away (the host's Kick or ban): joining again by itself would undo the
    // host's choice; the player's own Join is the next one.
    if (f.turned_away) turned_away_ = true;    // (kept until the player's own Host or Join: the next drop is not one)
    if (f.player_acted) turned_away_ = false;
    if (f.player_left || f.player_acted || f.turned_away) {
        start_done_ = true;
        was_in_ = false;
        tries_ = 0, next_ms_ = 0;
    }
    if (f.guest_in) {   // (in: a drop later is noticed; the start's join is done)
        was_in_ = true;
        start_done_ = true;
        tries_ = 0, next_ms_ = 0;
        return false;
    }
    const bool wanted = f.enabled && f.code && !f.launcher;
    if (was_in_ && f.idle) {   // (out again, and not by the player's Leave: the connection dropped)
        was_in_ = false;
        if (wanted && !f.player_left && !turned_away_) {
            tries_ = 0;
            next_ms_ = now_ms + kFirstRetryMs;
        }
    }
    if (!wanted) {
        next_ms_ = 0;
        // (off at the start's moment: that join is spent. Turned on later in the game, it waits for a drop: a run
        // showed the switch joining the last game at once otherwise)
        if (!start_done_ && now_ms - first_ms_ >= kStartDelayMs) start_done_ = true;
        return false;
    }
    if (!start_done_) {
        if (!f.idle) {
            start_done_ = true;   // (co-op is on already: hosting, or joining by itself)
            return false;
        }
        if (now_ms - first_ms_ < kStartDelayMs) return false;
        start_done_ = true;
        why_ = "at the game's start";
        return true;
    }
    if (next_ms_ != 0 && f.idle && now_ms >= next_ms_) {
        if (tries_ >= kTries) {
            next_ms_ = 0;
            why_ = "gave up after " + std::to_string(kTries) + " tries";
            return false;
        }
        ++tries_;
        next_ms_ = now_ms + kRetryMs;
        why_ = "the connection dropped (try " + std::to_string(tries_) + " of " + std::to_string(kTries) + ")";
        return true;
    }
    return false;
}

}  // namespace CoopAutoRejoin
