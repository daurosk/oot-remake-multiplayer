// SPDX-License-Identifier: GPL-3.0-or-later
// Game screens known by what is in them, looked at again whenever a screen may be another one at the same addresses
// (row 262). Logic only (no game): the callers do the game's pane lookups; tests/hookshot/pause_tab_test.cpp drives it.
//
// Row 262 (0.11.11, the owner's game): Character Select's Customize screen was known by its panes once per (layout,
// screen) address pair, and the verdict stayed for that pair for good. The game frees a menu screen with its layout and
// makes the next one at the same addresses (lane 061445: Saria's 4th opening came at the same screen and layout as the
// three before), so in a long session his screen came at addresses already taken as another screen's: never found,
// co-op's rows never on it (his log: no "Customize screen found" in two sessions; a fresh lane boot found it at once).
// The other way round (review-01112-e): a screen taken as his, freed, and another made at the same two addresses was
// taken as his too (lane 053539's symptom: every one of his panes failed at each update, its keys eaten).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>

namespace CoopScreen {

inline constexpr std::uint64_t kScreenGapMs = 200, kScreenLookMs = 1000;

// Whether an update of a screen is a new one of it (an "instance"): another screen or layout address, a gap of more than
// kScreenGapMs in its updates (freed and made again at the same place), or, when `open` is given, the screen open after
// an update that read closed. A clock that went back counts as a gap.
struct ScreenInstance {
    std::uint64_t screen = 0, layout = 0, seen_ms = 0;
    bool open = false, any = false;
    bool Next(std::uint64_t screen_now, std::uint64_t layout_now, bool open_now, std::uint64_t now_ms) {
        const bool gap = now_ms < seen_ms || now_ms - seen_ms > kScreenGapMs;
        const bool fresh = !any || screen_now != screen || layout_now != layout || gap || (open_now && !open);
        screen = screen_now, layout = layout_now, seen_ms = now_ms, open = open_now, any = true;
        return fresh;
    }
};

// Which screens are ours by their panes (his Customize screen's, SnailScreen; co-op's own CoopMenu_00, CoopMenuScreen):
//  - a verdict is made only while the screen reads open (1, 2): an update while closed (0, 3) keeps the verdict of its
//    opening (so its close is seen) and looks at nothing;
//  - EACH opening (ScreenInstance: open after a closed update, another screen at the layout, or a gap) forgets the
//    verdict, either way, and looks again: a screen at addresses that were another's is never taken for it;
//  - in an opening it is looked at each update until its panes are there: for kScreenLookMs, or for the whole opening
//    when the opening before at those addresses was ours (his panes may come late in a reopening, lane 061445); then the
//    verdict holds until the next opening.
class ScreenPanes {
public:
    struct Seen {
        ScreenInstance at;
        bool ours = false, looked = false, was_ours = false;
        std::uint64_t opened_ms = 0;
    };
    // One update of `screen` (its layout `layout`; open: its state 1 or 2): whether it is ours. `look()` (the game calls
    // that find its panes) runs only when its panes must be looked at.
    template <class Look>
    bool Update(std::uint64_t layout, std::uint64_t screen, bool open, std::uint64_t now_ms, Look&& look) {
        auto it = seen_.find(layout);
        if (it == seen_.end()) {
            if (seen_.size() >= 512) seen_.clear();   // (a bound: freed screens' layouts)
            it = seen_.emplace(layout, Seen{}).first;
        }
        Seen& e = it->second;
        const std::uint64_t had_screen = e.at.screen;
        const bool had_any = e.at.any;
        if (e.at.Next(screen, layout, open, now_ms) && open) {
            const bool was = had_any && had_screen == screen && e.ours;
            e.ours = false, e.looked = false, e.was_ours = was, e.opened_ms = now_ms;
        } else if (!had_any || had_screen != screen) {
            e.ours = false, e.looked = false, e.was_ours = false, e.opened_ms = now_ms;   // (another screen, closed)
        }
        if (open && !e.ours && (!e.looked || e.was_ours || now_ms - e.opened_ms <= kScreenLookMs)) {
            e.ours = look();
            e.looked = true;
        }
        return e.ours;
    }
    std::size_t size() const { return seen_.size(); }

private:
    std::map<std::uint64_t, Seen> seen_;   // by layout
};

}  // namespace CoopScreen
