// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_players_open.h"

namespace CoopPlayersOpen {

void Run::Start(std::uint64_t now_ms, bool menu_up) {
    on_ = true;
    opened_ = menu_up;   // (a menu in front: Minus could shut it, so never pressed; a shut strip then ends the run)
    opened_late_ = false;
    presses_ = 0, taps_ = 0;
    started_ms_ = now_ms, press_ms_ = 0, next_ms_ = now_ms;
}

Step Run::Next(const Seen& seen, std::uint64_t now_ms) {
    if (!on_) return Step::kWait;
    const auto end = [this](Step s) {
        on_ = false;
        return s;
    };
    if (seen.blocked || now_ms - started_ms_ > kMostMs) return end(Step::kGiveUp);
    if (!seen.strip_open) {
        if (opened_) return end(Step::kGiveUp);   // (the player shut it, or another screen came: not ours to reopen)
        if (!seen.free_play) return end(Step::kNotFree);   // (an event, a load, another screen: Minus is not the - menu)
        if (presses_ > 0 && now_ms - press_ms_ < kPressMs) return Step::kPressMinus;   // (still held)
        if (presses_ >= kMostPresses) {
            return now_ms - press_ms_ > kOpenWaitMs ? end(Step::kGiveUp) : Step::kWait;
        }
        if (presses_ == 0 || now_ms - press_ms_ > kOpenWaitMs) {   // (the first press, or one the game did not take)
            ++presses_;
            press_ms_ = now_ms;
            return Step::kPressMinus;
        }
        return Step::kWait;
    }
    if (!opened_) {   // (just opened: let the strip settle a moment before the first tap)
        opened_ = true;
        opened_late_ = presses_ >= 2;
        next_ms_ = now_ms + kTapGapMs;
        return Step::kWait;
    }
    if (seen.our_tab == 0) return end(Step::kShowPlayers);
    if (now_ms < next_ms_) return Step::kWait;
    if (seen.tabs == Tabs::kNo) return end(Step::kNoTabs);   // (this opening's lookup ran and found none)
    if (seen.tabs == Tabs::kUnknown && seen.our_tab < 0) return Step::kWait;   // (its lookup not run yet: wait, kMostMs)
    if (taps_ >= kMostTaps) return end(Step::kGiveUp);
    ++taps_;
    next_ms_ = now_ms + kTapGapMs;
    // Our tabs are right of the game's Map, Multiplayer first: from Lobby or Mods, L; from a game tab, R.
    return seen.our_tab > 0 ? Step::kTapL : Step::kTapR;
}

}  // namespace CoopPlayersOpen
