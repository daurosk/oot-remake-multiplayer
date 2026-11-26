// SPDX-License-Identifier: GPL-3.0-or-later
// F6: the player list in the game's own UI (ui-audit-01112 B5, A #84's plan). A press of F6 in play opens the game's
// - menu on co-op's Multiplayer tab, Players view (coop_pause_tab.h View::kPlayers: everyone, in the game's own
// panes), instead of drawing a list over the picture. Co-op drives the game's own buttons for it, as the hop to the
// Characters tab does (CharactersHop): a Minus press opens the - menu, R or L taps move the strip to our first tab
// (Multiplayer, right of the game's Map), then the Players view shows.
// This file is the steps only (no game, no pad): the pause tab module (coop_pause_tab_module.cpp) asks Next at each
// pad read and ORs in the button it says. Not thread-safe: the module guards it.
#pragma once

#include <cstdint>

namespace CoopPlayersOpen {

// Whether this opening of the - menu has tabs of ours: not looked up yet (the strip's lookup runs as it opens, a
// stall can make it late), found, or looked up and none (an old pack).
enum class Tabs { kUnknown, kYes, kNo };

// What the module sees at a pad read.
struct Seen {
    bool strip_open = false;   // the - menu is the open screen (Page::MinusStripOpen)
    int our_tab = -1;          // our - tab shown (Page::MinusTab: 0 Multiplayer, 1 Lobby, 2 Mods), -1 a game's tab
    Tabs tabs = Tabs::kUnknown;   // this opening's lookup of our tabs
    // Free play: our Link in a scene with no event (cutscene, talk, item get), no load, no other menu or screen in
    // front (the module's own reading: CoopHook::CurrentMapView and the game's event manager). Minus is pressed only
    // in free play; not in free play with the - menu shut: given up, never pressed into another screen.
    bool free_play = false;
    bool blocked = false;      // typing (a field of ours, the chat box), the Characters hop: stop at once
};

enum class Step {
    kWait,          // nothing to press now
    kPressMinus,    // hold Minus at this read (the - menu opens on the game's own Minus)
    kTapL,          // L at this read only
    kTapR,          // R at this read only
    kShowPlayers,   // our Multiplayer tab is up: show its Players view (once), then done
    kDone,
    kNoTabs,        // the - menu is open but has no tabs of ours: stop (the drawn list is F6's then)
    kNotFree,       // not free play while the - menu is shut: nothing pressed, stopped
    kGiveUp,
};

inline constexpr std::uint64_t kPressMs = 100;      // Minus held this long (the game reads its pad once a frame)
inline constexpr std::uint64_t kOpenWaitMs = 1200;  // the - menu's strip up within this after a press, else press again
inline constexpr int kMostPresses = 2;
inline constexpr std::uint64_t kTapGapMs = 400;     // between taps (the strip's slide)
inline constexpr int kMostTaps = 8;
inline constexpr std::uint64_t kMostMs = 8000;

class Run {
public:
    // F6 pressed: start. `menu_up`: a game menu is in front already (the - menu included): then only the hop, no press.
    void Start(std::uint64_t now_ms, bool menu_up);
    bool On() const { return on_; }
    void Stop() { on_ = false; }
    Step Next(const Seen& seen, std::uint64_t now_ms);

    int presses() const { return presses_; }
    // The strip opened after the second Minus (a stall): that press may have been the one the game took or the one
    // that shut it again; the module logs it.
    bool opened_after_second() const { return opened_late_; }
    int taps() const { return taps_; }

private:
    bool on_ = false;
    bool opened_ = false;      // the strip has been seen open (from then on: it shutting is the player's, give up)
    bool opened_late_ = false;
    int presses_ = 0, taps_ = 0;
    std::uint64_t started_ms_ = 0, press_ms_ = 0, next_ms_ = 0;
};

}  // namespace CoopPlayersOpen
