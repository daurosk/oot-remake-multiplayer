// SPDX-License-Identifier: GPL-3.0-or-later
// F6 opens the game's - menu on co-op's Multiplayer tab, Players view (hookshot/src/coop_players_open.h).
#include <doctest/doctest.h>

#include "coop_players_open.h"

using CoopPlayersOpen::Run;
using CoopPlayersOpen::Seen;
using CoopPlayersOpen::Step;
using CoopPlayersOpen::Tabs;

namespace {
Seen Shut(bool free_play = true) {
    Seen s;
    s.free_play = free_play;
    return s;
}
Seen Open(int tab, Tabs tabs = Tabs::kYes) {
    Seen s;
    s.strip_open = true;
    s.our_tab = tab;
    s.tabs = tabs;
    s.free_play = true;
    return s;
}
}  // namespace

TEST_CASE("F6 players: Minus opens the - menu, R taps to Multiplayer, then the Players view") {
    Run r;
    CHECK(r.Next(Shut(), 0) == Step::kWait);   // (not started: nothing)
    r.Start(1000, false);
    CHECK(r.On());
    CHECK(r.Next(Shut(), 1000) == Step::kPressMinus);
    CHECK(r.Next(Shut(), 1050) == Step::kPressMinus);   // held for kPressMs
    CHECK(r.Next(Shut(), 1000 + CoopPlayersOpen::kPressMs) == Step::kWait);   // let go
    CHECK(r.Next(Open(-1), 1300) == Step::kWait);   // the strip opened on a game tab: settles first
    CHECK(r.Next(Open(-1), 1300 + CoopPlayersOpen::kTapGapMs) == Step::kTapR);
    CHECK(r.Next(Open(-1), 1300 + CoopPlayersOpen::kTapGapMs + 10) == Step::kWait);   // one read only, then the gap
    CHECK(r.Next(Open(-1), 1300 + 2 * CoopPlayersOpen::kTapGapMs) == Step::kTapR);
    CHECK(r.Next(Open(0), 1300 + 2 * CoopPlayersOpen::kTapGapMs + 50) == Step::kShowPlayers);
    CHECK_FALSE(r.On());
    CHECK(r.taps() == 2);
    CHECK(r.presses() == 1);
}

TEST_CASE("F6 players: from Lobby or Mods it taps L, already on Multiplayer it shows at once") {
    Run r;
    r.Start(0, true);   // the - menu is up already: no Minus (it would shut it)
    CHECK(r.Next(Open(2), 0) == Step::kTapL);
    CHECK(r.Next(Open(1), CoopPlayersOpen::kTapGapMs) == Step::kTapL);
    CHECK(r.Next(Open(0), 2 * CoopPlayersOpen::kTapGapMs) == Step::kShowPlayers);
    Run at;
    at.Start(0, true);
    CHECK(at.Next(Open(0), 0) == Step::kShowPlayers);
    CHECK(at.presses() == 0);
}

TEST_CASE("F6 players: gives up, never presses into another screen") {
    // Another menu in front (the + menu: not the - strip): no Minus, given up.
    Run plus;
    plus.Start(0, true);
    CHECK(plus.Next(Shut(), 0) == Step::kGiveUp);
    // The game never opens the - menu (a cutscene, a load): two presses, then given up.
    Run never;
    never.Start(0, false);
    CHECK(never.Next(Shut(), 0) == Step::kPressMinus);
    CHECK(never.Next(Shut(), 500) == Step::kWait);
    CHECK(never.Next(Shut(), CoopPlayersOpen::kOpenWaitMs + 1) == Step::kPressMinus);
    CHECK(never.Next(Shut(), CoopPlayersOpen::kOpenWaitMs + 200) == Step::kWait);
    CHECK(never.Next(Shut(), 2 * CoopPlayersOpen::kOpenWaitMs + 2) == Step::kGiveUp);
    CHECK(never.presses() == CoopPlayersOpen::kMostPresses);
    // The player shuts the menu on the way: not opened again.
    Run shut;
    shut.Start(0, false);
    CHECK(shut.Next(Shut(), 0) == Step::kPressMinus);
    CHECK(shut.Next(Open(-1), 300) == Step::kWait);
    CHECK(shut.Next(Shut(), 900) == Step::kGiveUp);
    // Typing, the chat box, a load: at once.
    Run typing;
    typing.Start(0, false);
    Seen t = Shut();
    t.blocked = true;
    CHECK(typing.Next(t, 0) == Step::kGiveUp);
    // A strip that never reaches our tab (taps stop) and the whole run's limit.
    Run far;
    far.Start(0, true);
    std::uint64_t now = 0;
    int taps = 0;
    for (; taps < 20; ++taps, now += CoopPlayersOpen::kTapGapMs) {
        const Step s = far.Next(Open(-1), now);
        if (s != Step::kTapR) {
            CHECK(s == Step::kGiveUp);
            break;
        }
    }
    CHECK(taps == CoopPlayersOpen::kMostTaps);
    Run slow;
    slow.Start(0, true);
    CHECK(slow.Next(Open(-1), CoopPlayersOpen::kMostMs + 1) == Step::kGiveUp);
}

TEST_CASE("F6 players: a pack without our - tabs stops once the strip settled (F6's drawn list then)") {
    Run r;
    r.Start(0, false);
    CHECK(r.Next(Shut(), 0) == Step::kPressMinus);
    CHECK(r.Next(Open(-1, Tabs::kNo), 200) == Step::kWait);   // (just opened: settles first)
    CHECK(r.Next(Open(-1, Tabs::kNo), 200 + CoopPlayersOpen::kTapGapMs) == Step::kNoTabs);
    CHECK_FALSE(r.On());
    // This opening's lookup not run yet (a stall): no verdict, no tap, it waits; then the tabs are found and it taps.
    Run late;
    late.Start(0, true);
    CHECK(late.Next(Open(-1, Tabs::kUnknown), 0) == Step::kWait);
    CHECK(late.Next(Open(-1, Tabs::kUnknown), 3000) == Step::kWait);
    CHECK(late.taps() == 0);
    CHECK(late.Next(Open(-1, Tabs::kYes), 3100) == Step::kTapR);
    // Never looked up within the run's limit: given up, not "no tabs" (the next F6 tries again).
    Run never;
    never.Start(0, true);
    CHECK(never.Next(Open(-1, Tabs::kUnknown), 0) == Step::kWait);
    CHECK(never.Next(Open(-1, Tabs::kUnknown), CoopPlayersOpen::kMostMs + 1) == Step::kGiveUp);
}

TEST_CASE("F6 players: Minus only in free play (no event, load or other screen)") {
    // An event, a load or another screen with the - menu shut: nothing pressed, stopped.
    Run cutscene;
    cutscene.Start(0, false);
    CHECK(cutscene.Next(Shut(false), 0) == Step::kNotFree);
    CHECK(cutscene.presses() == 0);
    CHECK_FALSE(cutscene.On());
    // Free play at the first press, an event before the second: no second press.
    Run talk;
    talk.Start(0, false);
    CHECK(talk.Next(Shut(), 0) == Step::kPressMinus);
    CHECK(talk.Next(Shut(false), CoopPlayersOpen::kOpenWaitMs + 1) == Step::kNotFree);
    CHECK(talk.presses() == 1);
    // The strip opened after the second press (a stall): said, so the log can tell.
    Run slow;
    slow.Start(0, false);
    CHECK(slow.Next(Shut(), 0) == Step::kPressMinus);
    CHECK(slow.Next(Shut(), CoopPlayersOpen::kOpenWaitMs + 1) == Step::kPressMinus);
    CHECK(slow.Next(Open(-1), CoopPlayersOpen::kOpenWaitMs + 900) == Step::kWait);
    CHECK(slow.opened_after_second());
    // In the - menu already (not free play there): the taps still go.
    Run in_menu;
    in_menu.Start(0, true);
    Seen open = Open(2);
    open.free_play = false;
    CHECK(in_menu.Next(open, 0) == Step::kTapL);
}
