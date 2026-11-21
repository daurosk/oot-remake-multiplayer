// SPDX-License-Identifier: GPL-3.0-or-later
// The pause menu's Multiplayer tab (coop_pause_tab.h): when its page shows, what
// the module does to the game, what the pad lets through, and the probe's lines.
#include <doctest/doctest.h>

#include <array>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "coop_pause_tab.h"
#include "hos_coop_host.h"

namespace {

using namespace CoopPauseTab;

constexpr int kMinusMode = 0;   // (the strip's mode in the - menu: any mode but kPlusMode; one for every case)

Facts Strip(std::uint64_t screen, int tabs, int lit, int mode = kPlusMode, int state = 2) {
    Facts f;
    f.kind = Kind::kStrip;
    f.screen = screen;
    f.tabs = tabs, f.lit = lit, f.mode = mode, f.state = state;
    return f;
}

Facts Pages(std::uint64_t screen, int page, int asked, int state = 2) {
    Facts f;
    f.kind = Kind::kPages;
    f.screen = screen;
    f.page = page, f.asked = asked, f.state = state;
    return f;
}

Facts Blocker(int state) {
    Facts f;
    f.kind = Kind::kBlocker;
    f.screen = 0xb000;
    f.state = state;
    return f;
}

bool Has(const std::vector<std::string>& lines, const std::string& part) {
    for (const auto& l : lines) {
        if (l.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// A page whose panes and game calls were found (the module's SetUsable), as in a game with the pack and module.
Page UsablePage() {
    Page p;
    p.SetUsable(true);
    return p;
}

// The menu open on System, the strip with `tabs` tabs (the page needs 4 or more), at `now`.
void OnSystem(Page& p, std::uint64_t now, int tabs = 5) {
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
    p.OnScreen(Strip(0x9000, tabs, kSystemPage), now);
}

// A on one of System's buttons of ours (which: 0 the Multiplayer button, 1 Longshot Settings'), as the module tells
// the page while A is held: the buttons the game gets.
std::uint64_t Open(Page& p, std::uint64_t now, int which = 0) {
    p.SetSystemButton(which);
    const std::uint64_t got = p.OnPad(kPadA, now);
    p.SetSystemButton(-1);
    return got;
}

}  // namespace

TEST_CASE("pause tab: the screens are known by their class's vtable") {
    CHECK(KindOf(kPagesVtable) == Kind::kPages);
    CHECK(KindOf(kStripVtable) == Kind::kStrip);
    for (const std::uint64_t v : kBlockerVtables) {
        CHECK(KindOf(v) == Kind::kBlocker);
    }
    CHECK(KindOf(0x4bf06e8) == Kind::kOther);   // the title (UITitleScreen)
    CHECK(KindOf(0x4be8300) == Kind::kOther);   // the HUD (UIMapMenuScreen): updates every frame in play
    // Guest addresses, main at 0x80020000 (docs/game-ui.md and the game's names list).
    CHECK(0x80020000 + kPagesVtable == 0x84c0baa8);
    CHECK(0x80020000 + kStripVtable == 0x84c08aa0);
    CHECK(0x80020000 + kMenuManager == 0x84e9cfa8);
    CHECK(0x80020000 + kBlockerVtables[0] == 0x84c0fbf8);   // UISystemWindowScreen
    CHECK(0x80020000 + kBlockerVtables[7] == 0x84c07068);   // UIGameOverScreen
    CHECK(0x80020000 + kSetVisible == 0x8022aea0);
    CHECK(0x80020000 + kTabFill == 0x80a8ab50);
    CHECK(0x80020000 + kTabLight == 0x802ecf60);
    CHECK(0x80020000 + kPaneTable == 0x8045c040);
    CHECK(0x80020000 + kPaneAlpha == 0x802f8ac0);
    CHECK(0x80020000 + kTabOpen == 0x80228350);
    CHECK(0x80020000 + kPaneMoved == 0x8154af50);
    CHECK(0x80020000 + kScreenSound == 0x8045e400);
    CHECK(0x80020000 + kPathFind == 0x8045c4a0);
    CHECK(0x80020000 + kPaneSetMsg == 0x80459520);
    CHECK(0x80020000 + kPaneByName == 0x8045e300);
}

TEST_CASE("pause tab: the presses the module takes have the game's own sounds") {
    Page p = UsablePage();
    OnSystem(p, 1000);
    CHECK(p.TakePagesSound() == nullptr);
    p.OnPad(kPadR, 1000);   // (R on System: the game's own, no sound of ours)
    CHECK(p.TakePagesSound() == nullptr);
    p.OnPad(0, 1016);
    Open(p, 1032);   // (A on the Multiplayer button: taken from the game, so its sound is ours)
    CHECK(std::string(p.TakePagesSound()) == "button_decide");
    CHECK(p.TakePagesSound() == nullptr);   // (once)
    p.OnPad(0, 1048);
    p.OnPad(kPadA, 1064);   // (A on a row)
    CHECK(std::string(p.TakePagesSound()) == "button_decide");
    p.OnPad(0, 1080);
    p.OnPad(kPadL, 1096);   // (L on our page: the game's own, no sound of ours)
    CHECK_FALSE(p.Shown());
    CHECK(p.TakePagesSound() == nullptr);
}

TEST_CASE("pause tab: the menu closed on our page opens on it again, not after L") {
    Page p = UsablePage();
    std::uint64_t now = 1000;
    OnSystem(p, now);
    CHECK(Open(p, now) == 0);
    CHECK(p.Shown());
    p.OnPad(0, now + 16);
    // The menu closes (+): our page goes with it.
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), now + 32);
    CHECK_FALSE(p.Shown());
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 3), now + 32);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 0), now + 300);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 0), now + 300);
    CHECK_FALSE(p.Shown());
    // The next opening, on System as the game keeps it: our page from its first opening update (the strip's).
    auto d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), now + 5000);
    CHECK(p.Shown());
    CHECK(d.set_page);
    CHECK(d.page_shown);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 1), now + 5000);   // (a new opening of the pages: set again, the same)
    // (as a run had it: the pages open while the strip still opens; the page stays, the pad's update too)
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 2), now + 5100);
    p.OnPad(0, now + 5100);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), now + 5100);
    CHECK(p.Shown());
    // L, then the menu closed and opened again: System's.
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 2), now + 5300);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 2), now + 5300);
    CHECK(p.OnPad(kPadL, now + 5400) == kPadL);   // (System back, and the L is the game's)
    CHECK_FALSE(p.Shown());
    p.OnPad(0, now + 5416);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), now + 5500);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 0), now + 5800);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), now + 9000);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 1), now + 9000);
    CHECK_FALSE(p.Shown());
}

TEST_CASE("pause tab: back at the menu's first frame, the pages still at 0 while the strip opens") {
    Page p = UsablePage();
    OnSystem(p, 1000);
    Open(p, 1000);
    p.OnPad(0, 1016);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 3), 1032);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), 1032);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 0), 1300);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 0), 1300);
    CHECK_FALSE(p.Shown());   // (closed: nothing)
    // As a run had it: the strip's first update of the opening comes first, the pages' last update still the closed
    // one (old). The page is back there, set through the strip's update, at once.
    const auto d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), 4000);
    CHECK(p.Shown());
    CHECK(d.set_page);
    CHECK(d.page_shown);
    CHECK(d.page_to == 1);
    CHECK(d.slide == 0);
    CHECK(p.OnPad(0, 4008) == 0);   // (the pad between them: it stays)
    CHECK(p.Shown());
    CHECK_FALSE(p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 0), 4018).set_page);   // (set already)
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 2), 4016);
    p.OnPad(0, 4016);
    CHECK(p.Shown());
}

TEST_CASE("pause tab: a menu that opens on another page forgets ours") {
    Page p = UsablePage();
    OnSystem(p, 1000);
    Open(p, 1000);
    p.OnPad(0, 1016);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), 1032);
    CHECK_FALSE(p.Shown());
    p.OnScreen(Pages(0xa000, 1, 1, 2), 3000);   // (open on another page)
    p.OnScreen(Strip(0x9000, 5, 1, kPlusMode, 2), 3000);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 2), 3100);   // (then stepped to System)
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 2), 3100);
    CHECK_FALSE(p.Shown());
}

TEST_CASE("pause tab: A on the Multiplayer button shows the page at once, L or R gives System back at once and is the game's") {
    Page p = UsablePage();
    std::uint64_t now = 1000;
    // First updates: our page hidden (System shown); the strip's ask nothing (the game's own tabs, System's lit).
    auto d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
    CHECK(d.set_page);
    CHECK_FALSE(d.page_shown);
    CHECK(d.set_texts);
    CHECK_FALSE(p.OnScreen(Strip(0x9000, 5, kSystemPage), now).any());
    CHECK_FALSE(p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now).any());   // (nothing again)
    CHECK_FALSE(p.OnScreen(Strip(0x9000, 5, kSystemPage), now).any());
    // R on System alone is the game's: nothing of ours.
    CHECK(p.OnPad(kPadR, now) == kPadR);
    CHECK_FALSE(p.Shown());
    CHECK(p.OnPad(0, now + 4) == 0);
    // A on another of System's buttons is the game's.
    CHECK(p.OnPad(kPadA, now + 8) == kPadA);
    CHECK_FALSE(p.Shown());
    CHECK_FALSE(p.TakeOpened());
    CHECK(p.OnPad(0, now + 12) == 0);
    // A on the Multiplayer button: taken, the page shows at once at the next update (no slide).
    CHECK(Open(p, now + 16) == 0);
    CHECK(p.Shown());
    CHECK(p.Which() == 0);
    CHECK(p.TakeOpened());
    CHECK_FALSE(p.TakeOpened());   // (once)
    d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 20);
    CHECK(d.set_page);
    CHECK(d.page_shown);
    CHECK(d.page_from == 0);
    CHECK(d.page_to == 1);
    CHECK(d.slide == 0);
    CHECK_FALSE(p.OnScreen(Strip(0x9000, 5, kSystemPage), now + 24).any());   // (no tab of ours to light)
    // A held: still the module's (no fresh A for the game, and no row's A).
    CHECK(p.OnPad(kPadA, now + 32) == 0);
    CHECK_FALSE(p.TakeDecide());
    CHECK(p.OnPad(0, now + 48) == 0);
    // L: System back at once, and the L is the game's (on to Items).
    CHECK(p.OnPad(kPadL, now + 64) == kPadL);
    CHECK_FALSE(p.Shown());
    CHECK(p.TakeWhy() == "L or R on the Multiplayer page");
    CHECK_FALSE(p.TakeLeft());   // (only B leaves for the button)
    d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 72);
    CHECK(d.set_page);
    CHECK_FALSE(d.page_shown);
    CHECK(d.page_from == 1);
    CHECK(d.page_to == 0);
    CHECK(d.slide == 0);
    CHECK(p.OnPad(kPadL, now + 80) == kPadL);   // (held: still the game's)
    CHECK(p.OnPad(0, now + 96) == 0);
    // R the same.
    CHECK(Open(p, now + 112) == 0);
    CHECK(p.Shown());
    CHECK(p.OnPad(0, now + 120) == 0);
    CHECK(p.OnPad(kPadR, now + 130) == kPadR);
    CHECK_FALSE(p.Shown());
    // ZL and ZR do the same.
    p.OnPad(0, now + 135);
    CHECK(Open(p, now + 140) == 0);
    p.OnPad(0, now + 145);
    CHECK(p.OnPad(kPadZL, now + 150) == kPadZL);
    CHECK_FALSE(p.Shown());
    p.OnPad(0, now + 155);
    CHECK(Open(p, now + 160) == 0);
    p.OnPad(0, now + 165);
    CHECK(p.OnPad(kPadZR, now + 170) == kPadZR);
    CHECK_FALSE(p.Shown());
}

TEST_CASE("pause tab: under the page the game gets only + and -, and up and down for its cursor, A is ours, B leaves") {
    Page p = UsablePage();
    std::uint64_t now = 1000;
    OnSystem(p, now);
    REQUIRE(Open(p, now) == 0);
    p.OnPad(0, now + 1);
    const std::uint64_t x = 1ull << 2, y = 1ull << 3, left = 1ull << 12, right = 1ull << 14, stick_right = 1ull << 18;
    CHECK(p.OnPad(kPadA, now + 2) == 0);   // A: ours, not the System page's
    CHECK(p.TakeDecide());
    CHECK_FALSE(p.TakeDecide());
    // (ZL: back to System and the game's; the rest kept from it until let go)
    CHECK(p.OnPad(x | y | left | right | stick_right | kPadZL, now + 3) == kPadZL);
    CHECK_FALSE(p.Shown());
    p.OnPad(0, now + 4);
    REQUIRE(Open(p, now + 5) == 0);
    p.OnPad(0, now + 6);
    // Up and down reach the game: the System page's focus moves, and the game's cursor frame with it over our rows.
    CHECK(p.OnPad(kPadDown, now + 7) == kPadDown);
    CHECK(p.OnPad(kPadUp | kPadStickUp, now + 8) == (kPadUp | kPadStickUp));
    CHECK(p.OnPad(kPadDown | kPadA, now + 9) == kPadDown);
    CHECK(p.TakeDecide());
    p.OnPad(0, now + 10);
    CHECK(p.OnPad(kPadPlus, now + 11) == kPadPlus);
    CHECK(p.OnPad(kPadMinus, now + 12) == kPadMinus);
    CHECK(p.Shown());   // (the game acts on + and -; the screens' updates then give System back)
    // B on the page's own rows: ours, System back (the game's B would close the menu).
    CHECK(p.OnPad(kPadB, now + 13) == 0);
    CHECK_FALSE(p.Shown());
    CHECK(p.TakeLeft());
}

TEST_CASE("pause tab: the page does not show off System, mid-step, in the Minus menu, on a strip of under four tabs or under a window") {
    std::uint64_t now = 1000;
    SUBCASE("on Items") {
        Page p = UsablePage();
        p.OnScreen(Pages(0xa000, 4, 4), now);
        p.OnScreen(Strip(0x9000, 5, 4), now);
        CHECK(p.OnPad(kPadR, now) == kPadR);   // (the game's: Items to System)
        CHECK_FALSE(p.Shown());
        p.OnPad(0, now + 1);
        CHECK(Open(p, now + 2) == 0);   // (on our button, the page not shown: kept from the game)
        CHECK_FALSE(p.Shown());
    }
    SUBCASE("mid-step to System") {
        Page p = UsablePage();
        p.OnScreen(Pages(0xa000, 4, kSystemPage), now);
        p.OnScreen(Strip(0x9000, 5, kSystemPage), now);
        CHECK(Open(p, now) == 0);
        CHECK_FALSE(p.Shown());
    }
    SUBCASE("the Minus menu") {
        Page p = UsablePage();
        p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
        p.OnScreen(Strip(0x9000, 5, 2, 0), now);
        CHECK(Open(p, now) == 0);
        CHECK_FALSE(p.Shown());
    }
    SUBCASE("three tabs: fewer than the game's four") {
        Page p = UsablePage();
        OnSystem(p, now, 3);
        CHECK(Open(p, now) == 0);
        CHECK_FALSE(p.Shown());
        // (the game's own four are enough: no tab of ours is needed)
        p.OnPad(0, now + 1);
        OnSystem(p, now + 2, 4);
        CHECK(Open(p, now + 2) == 0);
        CHECK(p.Shown());
    }
    SUBCASE("the menu still opening") {
        Page p = UsablePage();
        p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 1), now);
        p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), now);
        CHECK(Open(p, now) == 0);
        CHECK_FALSE(p.Shown());
    }
    SUBCASE("a window over the System page (Save, Options, a question)") {
        Page p = UsablePage();
        OnSystem(p, now);
        p.OnScreen(Blocker(2), now);
        CHECK(Open(p, now + 10) == kPadA);
        CHECK_FALSE(p.Shown());
        // Gone (state 0) for a while: the button's A is ours again.
        p.OnScreen(Blocker(0), now + 20);
        p.OnPad(0, now + 299);
        OnSystem(p, now + 300);
        CHECK(Open(p, now + 300) == 0);
        CHECK(p.Shown());
    }
    SUBCASE("the screens' updates stopped") {
        Page p = UsablePage();
        OnSystem(p, now);
        // (the pad is polled every frame while the game runs; with no call at all it would be a stall of the whole
        // game, which keeps the screens fresh: AfterStall)
        for (std::uint64_t t = now + 16; t < now + 1000; t += 16) p.OnPad(0, t);
        CHECK(Open(p, now + 1000) == 0);
        CHECK_FALSE(p.Shown());
    }
}

TEST_CASE("pause tab: closing, the Minus menu, a window or Game Over give System back at once") {
    std::uint64_t now = 1000;
    const auto shown = [&](Page& p) {
        OnSystem(p, now);
        REQUIRE(Open(p, now) == 0);
        p.OnPad(0, now + 1);
        p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 2);
        REQUIRE(p.Shown());
    };
    SUBCASE("+ closes the menu") {
        Page p = UsablePage();
        shown(p);
        CHECK(p.OnPad(kPadPlus, now + 3) == kPadPlus);
        const auto d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), now + 20);
        CHECK_FALSE(p.Shown());
        CHECK(d.set_page);
        CHECK_FALSE(d.page_shown);
        CHECK(p.TakeWhy() == "the menu is closing");
    }
    SUBCASE("the Minus menu") {
        Page p = UsablePage();
        shown(p);
        p.OnScreen(Strip(0x9000, 5, 2, 0), now + 20);
        CHECK_FALSE(p.Shown());
        CHECK(p.TakeWhy() == "the Minus menu");
    }
    SUBCASE("Game Over or a window") {
        Page p = UsablePage();
        shown(p);
        p.OnPad(kPadA, now + 3);   // (A held as the page goes)
        p.OnScreen(Blocker(1), now + 20);
        CHECK_FALSE(p.Shown());
        CHECK(p.TakeWhy() == "a window opened over it");
        CHECK(p.OnPad(kPadA, now + 21) == 0);         // still held: not a fresh A for System
        CHECK(p.OnPad(0, now + 22) == 0);
        CHECK(p.OnPad(kPadA, now + 23) == kPadA);     // a new press is the game's
    }
    SUBCASE("a new pages screen object (a load) starts over: our page hidden") {
        Page p = UsablePage();
        shown(p);
        const auto d = p.OnScreen(Pages(0xa100, kSystemPage, kSystemPage, 0), now + 20);
        CHECK_FALSE(p.Shown());
        CHECK(d.set_page);
        CHECK_FALSE(d.page_shown);
    }
}

TEST_CASE("pause tab: the probe logs a screen's first update, its first second's count and each change") {
    Probe p;
    auto lines = p.Seen(Strip(0x9000, 5, 5), 1000);
    REQUIRE(lines.size() == 1);
    CHECK(Has(lines, "the tab strip (MenuOverlay_00), screen 0x9000: its updates reach the module (patch 0030)"));
    CHECK(Has(lines, "5 tabs, the Plus menu, lights category 5, state 2"));
    for (int i = 1; i < 60; ++i) {
        CHECK(p.Seen(Strip(0x9000, 5, 5), 1000 + 16 * static_cast<std::uint64_t>(i)).empty());
    }
    lines = p.Seen(Strip(0x9000, 5, 5), 2000);
    REQUIRE(lines.size() == 1);
    CHECK(Has(lines, "the tab strip (MenuOverlay_00): 61 updates in its first 1000 ms"));
    CHECK(p.Seen(Strip(0x9000, 5, 5), 3000).empty());
    lines = p.Seen(Strip(0x9000, 5, 4), 3016);
    REQUIRE(lines.size() == 1);
    CHECK(Has(lines, "lights category 4, state 2 (was 5 tabs, the Plus menu, lights category 5, state 2)"));
    CHECK(Has(p.Seen(Strip(0x9000, 5, 2, 0), 3032), "the Minus menu, lights category 2"));   // (mode 0: Minus, seen in a run)
    lines = p.Seen(Pages(0xa000, 5, 5), 3048);
    CHECK(Has(lines, "the pause menu's pages (PlusMenu_00), screen 0xa000: its updates reach the module"));
    CHECK(Has(p.Seen(Pages(0xa000, 5, 4), 3064), "page 5, asked 4, state 2 (was page 5, asked 5, state 2)"));
    CHECK(Has(p.Seen(Strip(0x9100, 4, 5), 4000), "screen 0x9100: its updates reach the module"));
    Facts other;
    other.screen = 0x9000;
    CHECK(p.Seen(other, 0).empty());
    CHECK(p.Seen(Blocker(2), 0).empty());
}

TEST_CASE("pause tab (0.11.9): five fixed entries, each opening its own view, whatever co-op is doing") {
    Session idle;
    Session joined;
    joined.in_one = true;
    Session hosting_no_code;
    hosting_no_code.hosting = hosting_no_code.in_one = true;
    Session hosting = hosting_no_code;
    hosting.code = true;

    for (const Session& s : {idle, joined, hosting_no_code, hosting}) {
        CHECK(ActionFor(kRowFind, s) == RowAction::kViewLan);
        CHECK(ActionFor(kRowHost, s) == RowAction::kViewHost);
        CHECK(ActionFor(kRowJoin, s) == RowAction::kViewJoin);
        CHECK(ActionFor(kRowPlayers, s) == RowAction::kViewPlayers);   // (every entry opens: "just you" too)
        CHECK(ActionFor(kRowSettings, s) == RowAction::kViewMenu);
        CHECK(ActionFor(-1, s) == RowAction::kNone);              // (no row under the cursor)
        CHECK(ActionFor(kRows, s) == RowAction::kNone);
        for (int row = 0; row < kRows; ++row) CHECK(ActionFor(row, s) != RowAction::kNone);
    }
    // Co-op failed: the Host entry shows why (the Session view, which starts hosting again).
    Session failed;
    failed.failed = true;
    CHECK(ActionFor(kRowHost, failed) == RowAction::kViewSession);
    // A guest the host could not reach: the Join entry copies this game's answer code (co-op's menu's Copy answer).
    Session waiting;
    waiting.in_one = waiting.wait_host = true;
    CHECK(ActionFor(kRowJoin, waiting) == RowAction::kCopyAnswer);
    // Each has words for the log.
    for (RowAction a : {RowAction::kNone, RowAction::kOpenMain, RowAction::kCopyCode, RowAction::kAllow,
                        RowAction::kHost, RowAction::kConfirmLeave, RowAction::kViewHost}) {
        CHECK(std::string(ActionWords(a)).size() > 3);
    }
}

TEST_CASE("pause tab (0.11.9): the Host view: Start hosting alone, the game's code, Let in, details and Stop hosting") {
    Session idle;
    CHECK(ActionFor(View::kHost, 0, idle) == RowAction::kHost);
    CHECK(ActionFor(View::kHost, 1, idle) == RowAction::kViewMenu);   // (the host settings)
    CHECK(ActionFor(View::kHost, 2, idle) == RowAction::kNone);
    CHECK(ActionFor(View::kHost, kRows - 1, idle) == RowAction::kBack);
    Session host;
    host.hosting = host.in_one = true;
    CHECK(ActionFor(View::kHost, 0, host) == RowAction::kViewSession);   // (the code still coming)
    host.code = true;
    CHECK(ActionFor(View::kHost, 0, host) == RowAction::kCopyCode);
    CHECK(ActionFor(View::kHost, 1, host) == RowAction::kViewLetIn);     // (nobody asks: the view says so)
    CHECK(ActionFor(View::kHost, 2, host) == RowAction::kViewSession);
    CHECK(ActionFor(View::kHost, kHostRowStop, host) == RowAction::kConfirmLeave);
    Session guest;
    guest.in_one = true;
    CHECK(ActionFor(View::kHost, 0, guest) == RowAction::kViewSession);
    CHECK(ActionFor(View::kHost, 1, guest) == RowAction::kViewJoin);     // (leave there first)

    SessionView v;
    CHECK(RowTexts(View::kHost, v) == std::vector<std::string>{"Start hosting", "Host settings", "", "", "Back"});
    v.s.hosting = v.s.in_one = v.s.code = v.s.invite = true;
    v.players = 1;
    CHECK(RowTexts(View::kHost, v) ==
          std::vector<std::string>{"Copy invite code", "Let in: answer code", "Session details", "Stop hosting", "Back"});
    v.s.invite = false;
    v.room = "forest-gang";
    CHECK(RowTexts(View::kHost, v)[0] == "Room: forest-gang");
    CHECK(RowTexts(View::kHost, v)[1] == "Nobody asks to join");
    v.asking = {"Saria"};
    v.s.asking = true;
    CHECK(RowTexts(View::kHost, v)[1] == "Let in Saria");
    v.asking = {"Saria", "Mido", "Darunia"};
    CHECK(RowTexts(View::kHost, v)[1] == "Let in Saria (+2)");
    v.confirm_leave = true;
    CHECK(RowTexts(View::kHost, v)[kHostRowStop] == "A again: stop hosting");
    v.confirm_click = true;   // (the first a click: the second one too)
    CHECK(RowTexts(View::kHost, v)[kHostRowStop] == "Click again to stop");
    SessionView starting;   // (the page's own Host, still starting)
    starting.s.in_one = starting.s.hosting = true;
    starting.connecting = true;
    CHECK(RowTexts(View::kHost, starting)[0] == "Starting to host...");
    CHECK(RowTexts(View::kHost, starting)[kHostRowStop] == "Stop hosting");
    CHECK(RowTexts(View::kLetIn, v).back() == "Back");
}

TEST_CASE("pause tab (0.11.9): the Join view in a friend's game: its details and Leave, a host goes to its game") {
    Session guest;
    guest.in_one = true;
    CHECK(ActionFor(View::kJoin, 0, guest) == RowAction::kViewSession);
    CHECK(ActionFor(View::kJoin, kJoinRowLeave, guest) == RowAction::kConfirmLeave);
    CHECK(ActionFor(View::kJoin, 2, guest) == RowAction::kNone);
    Session waiting = guest;
    waiting.wait_host = true;
    CHECK(ActionFor(View::kJoin, 0, waiting) == RowAction::kCopyAnswer);
    Session launcher;
    launcher.by_launcher = true;
    CHECK(ActionFor(View::kJoin, kJoinRowLeave, launcher) == RowAction::kConfirmLeave);   // (left here too)
    Session host;
    host.hosting = host.in_one = true;
    CHECK(ActionFor(View::kJoin, 0, host) == RowAction::kViewHost);
    CHECK(ActionFor(View::kJoin, 1, host) == RowAction::kNone);

    SessionView g;
    g.s.in_one = true;
    CHECK(RowTexts(View::kJoin, g) == std::vector<std::string>{"Session details", "Leave the game", "", "", "Back"});
    g.confirm_leave = true;
    CHECK(RowTexts(View::kJoin, g)[kJoinRowLeave] == "A again: leave");
    g.confirm_click = true;
    CHECK(RowTexts(View::kJoin, g)[kJoinRowLeave] == "Click again to leave");
    SessionView h;
    h.s.in_one = h.s.hosting = true;
    CHECK(RowTexts(View::kJoin, h)[0] == "Hosting: open Your game");
}

TEST_CASE("pause tab (0.11.9): the five entries' words, in at most a row's width") {
    SessionView idle;
    CHECK(RowTexts(idle) ==
          std::vector<std::string>{"Find a game", "Host a game", "Join a game", "Players", "Co-op settings"});
    SessionView host;
    host.s.hosting = host.s.in_one = true;
    host.players = 1;
    CHECK(RowTexts(host) ==
          std::vector<std::string>{"Find a game", "Your game", "Join a game", "Players: just you", "Co-op settings"});
    host.players = 3;
    host.asking = {"Saria"};
    CHECK(RowTexts(host)[kRowHost] == "Your game: Saria asks");
    CHECK(RowTexts(host)[kRowPlayers] == "Players: 3");
    host.asking = {"Saria", "Mido"};
    CHECK(RowTexts(host)[kRowHost] == "Your game: 2 ask");
    SessionView guest;
    guest.s.in_one = true;
    guest.players = 2;
    CHECK(RowTexts(guest) ==
          std::vector<std::string>{"Find a game", "Host a game", "Your session", "Players: 2", "Co-op settings"});
    SessionView joining;
    joining.connecting = true;
    joining.s.in_one = true;
    CHECK(RowTexts(joining)[kRowJoin] == "Connecting...");
    SessionView w;
    w.s.in_one = w.s.wait_host = true;
    CHECK(RowTexts(w)[kRowJoin] == "Copy answer code");
    SessionView starting;
    starting.s.in_one = starting.s.hosting = true;
    starting.connecting = true;
    CHECK(RowTexts(starting)[kRowHost] == "Starting to host...");
    SessionView fv;
    fv.s.failed = true;
    CHECK(RowTexts(fv)[kRowHost] == "Co-op failed: why");
    SessionView turned;
    turned.s.in_one = true;
    turned.turned_away = "full";
    CHECK(RowTexts(turned)[kRowJoin] == "Turned away");

    // A long room name is cut at a UTF-8 character's start, with "..." (the Host view's first row).
    host.asking.clear();
    host.room = std::string(30, 'a') + "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";
    const std::string cut = RowTexts(View::kHost, host)[0];
    CHECK(cut.size() <= kRowChars);
    CHECK(cut.substr(cut.size() - 3) == "...");
    CHECK((static_cast<unsigned char>(cut[cut.size() - 4]) & 0xc0) != 0xc0);   // (no lead byte left alone)
    for (const SessionView& v : {idle, host, guest, joining, w, starting, fv, turned}) {
        for (const std::string& r : RowTexts(v)) CHECK(r.size() <= kRowChars);
        for (const std::string& r : RowTexts(View::kHost, v)) CHECK(r.size() <= kRowChars);
        for (const std::string& r : RowTexts(View::kJoin, v)) CHECK(r.size() <= kRowChars);
    }
}

TEST_CASE("pause tab: a page whose panes or game calls are missing never shows, and the mouse is told so") {
    // (an old or mismatched pack, a module without the scratch API: the page would be undrawn, so System's A and
    // its focused button must stay System's; Act would otherwise run row 0 Host or row 4 Leave on System's buttons)
    std::uint64_t now = 1000;
    Page p;   // (not usable until the module says so)
    CHECK(p.MousePage() == -2);
    OnSystem(p, now);
    CHECK(Open(p, now) == 0);   // (kept from the game: it does not know our button)
    CHECK_FALSE(p.Shown());
    CHECK_FALSE(p.TakeOpened());
    p.OnPad(0, now + 1);
    CHECK(p.OnPad(kPadA, now + 2) == kPadA);   // (System's A)
    CHECK_FALSE(p.TakeDecide());
    p.OnPad(0, now + 3);
    // Usable: the mouse's tabs are the game's four (-1, shown or not: our page is in System's place), and A on the
    // Multiplayer button shows the page.
    p.SetUsable(true);
    CHECK(p.MousePage() == -1);
    OnSystem(p, now + 4);
    CHECK(Open(p, now + 4) == 0);
    CHECK(p.Shown());
    CHECK(p.MousePage() == -1);
    // Its panes lost while it shows (a new screen whose look-up failed): System back at once, A not ours.
    p.OnPad(0, now + 5);
    p.SetUsable(false);
    CHECK_FALSE(p.Shown());
    CHECK(p.TakeWhy() == "its panes or the game's calls are not there");
    CHECK(p.MousePage() == -2);
    CHECK(p.OnPad(kPadA, now + 6) == kPadA);
    CHECK_FALSE(p.TakeDecide());
}

TEST_CASE("pause tab: a screen reopened at the same address, or with a new layout, is a new screen") {
    std::uint64_t now = 1000;
    Page p = UsablePage();
    const std::uint64_t g1 = p.Track(Pages(0xa000, kSystemPage, kSystemPage, 1));
    CHECK(g1 != 0);
    CHECK(p.Track(Pages(0xa000, kSystemPage, kSystemPage, 1)) == g1);   // (still opening)
    CHECK(p.Track(Pages(0xa000, kSystemPage, kSystemPage, 2)) == g1);   // (open)
    auto d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
    CHECK(d.set_texts);   // (once per opening: its first open update)
    CHECK_FALSE(p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 1).set_texts);
    // Closed and opened again at the same address: its texts and the page's state are set again.
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), now + 10);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 0), now + 20);
    const std::uint64_t g2 = p.Track(Pages(0xa000, kSystemPage, kSystemPage, 1));
    CHECK(g2 != g1);
    d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 2), now + 30);
    CHECK(d.set_texts);
    CHECK(d.set_page);
    // A new layout at the same address: a new screen too.
    Facts f = Pages(0xa000, kSystemPage, kSystemPage, 2);
    f.layout = 0x7000;
    CHECK(p.Track(f) != g2);
    CHECK(p.OnScreen(f, now + 40).set_texts);
    // The strip the same way (nothing of ours is set in it, but a look-up there goes stale with it).
    const std::uint64_t s1 = p.Track(Strip(0x9000, 5, kSystemPage, kPlusMode, 1));
    CHECK(s1 != 0);
    CHECK(p.Track(Strip(0x9000, 5, kSystemPage)) == s1);   // (open)
    CHECK_FALSE(p.OnScreen(Strip(0x9000, 5, kSystemPage), now + 50).any());
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 0), now + 60);
    const std::uint64_t s2 = p.Track(Strip(0x9000, 5, kSystemPage, kPlusMode, 1));
    CHECK(s2 != s1);
    CHECK(p.Track(Strip(0x9000, 5, kSystemPage)) == s2);
    CHECK(p.Track(Strip(0x9100, 5, kSystemPage)) != s2);   // (a new address)
}

TEST_CASE("pause tab: co-op that came with the game's start, and a failed Host, show the Session view") {
    // (the launcher rule, as co-op's menu: Host.bat / Join.bat / coop.ini co-op is not started or stopped here)
    Session launcher;
    launcher.by_launcher = launcher.in_one = true;
    CHECK(ActionFor(View::kHost, 0, launcher) == RowAction::kViewSession);
    CHECK(ActionFor(View::kJoin, kJoinRowLeave, launcher) == RowAction::kConfirmLeave);   // (left there, after a second A)
    launcher.in_one = false;   // (not connected yet: still not Host)
    CHECK(ActionFor(View::kHost, 0, launcher) == RowAction::kViewSession);
    CHECK(ActionFor(View::kJoin, kJoinRowLeave, launcher) == RowAction::kConfirmLeave);
    SessionView lv;
    lv.s = launcher;
    CHECK(RowTexts(lv)[kRowJoin] == "Your session");
    CHECK(RowTexts(View::kJoin, lv)[kJoinRowLeave] == "Leave the game");
    lv.confirm_leave = true;
    CHECK(RowTexts(View::kJoin, lv)[kJoinRowLeave] == "A again: leave");
    // A Host (or Join) that failed: the row says so (why: the details list), and A shows the Session view.
    Session failed;
    failed.failed = true;
    CHECK(ActionFor(kRowHost, failed) == RowAction::kViewSession);
    SessionView fv;
    fv.s = failed;
    CHECK(RowTexts(fv)[kRowHost] == "Co-op failed: why");
}

TEST_CASE("pause tab: a second A (Leave, Kick, Ban) is waited for only while the cursor stays on its row") {
    CHECK(KeepConfirm(true, kHostRowStop, kHostRowStop));
    CHECK_FALSE(KeepConfirm(true, 1, kHostRowStop));
    CHECK_FALSE(KeepConfirm(true, -1, kHostRowStop));
    CHECK_FALSE(KeepConfirm(true, -1, -1));       // (nothing waited for)
    CHECK(KeepConfirm(true, 2, 2));               // (Kick, on the host's controls)
    CHECK_FALSE(KeepConfirm(false, kHostRowStop, kHostRowStop));   // (System back: no second A waited for)
}

TEST_CASE("pause tab: the rows' own views: Players, Let in and Join on the same five rows, Back last") {
    Session idle;
    Session host;
    host.hosting = host.in_one = host.code = true;
    // Players: who is in, one a row; only Back acts.
    SessionView pv;
    pv.s.in_one = true;
    pv.names = {"dauro (you)", "Saria (host)"};
    CHECK(RowTexts(View::kPlayers, pv) == std::vector<std::string>{"dauro (you)", "Saria (host)", "", "", "Back"});
    pv.names = {"a", "b", "c", "d", "e", "f"};
    CHECK(RowTexts(View::kPlayers, pv) == std::vector<std::string>{"a", "b", "c", "More players (1/2)", "Back"});
    SessionView none;
    CHECK(RowTexts(View::kPlayers, none)[0] == "Not in a session");
    for (int r = 0; r < kRows - 1; ++r) CHECK(ActionFor(View::kPlayers, r, host) == RowAction::kNone);
    CHECK(ActionFor(View::kPlayers, kRows - 1, host) == RowAction::kBack);
    // Let in: the oldest who asks, Let in (/allow) or Refuse (/deny); nobody: nothing to press.
    CHECK(ActionFor(View::kLetIn, 0, host) == RowAction::kNone);
    host.asking = true;
    CHECK(ActionFor(View::kLetIn, 0, host) == RowAction::kAllow);
    CHECK(ActionFor(View::kLetIn, 1, host) == RowAction::kDeny);
    CHECK(ActionFor(View::kLetIn, 2, host) == RowAction::kNone);
    CHECK(ActionFor(View::kLetIn, kRows - 1, host) == RowAction::kBack);
    SessionView lv;
    lv.s = host;
    CHECK(RowTexts(View::kLetIn, lv)[0] == "Nobody asks to join");
    lv.asking = {"Mido", "Darunia"};
    CHECK(RowTexts(View::kLetIn, lv) == std::vector<std::string>{"Let in Mido", "Refuse Mido", "+1 more ask", "", "Back"});
    // Join: the clipboard's code, a room name and its password typed on their rows, then that room (or the last game
    // again while no room name is typed); never from inside a session.
    CHECK(ActionFor(View::kJoin, 0, idle) == RowAction::kJoinPaste);
    CHECK(ActionFor(View::kJoin, 1, idle) == RowAction::kTypeRoom);
    CHECK(ActionFor(View::kJoin, 3, idle) == RowAction::kNone);   // (no last game kept, no room name)
    Session again = idle;
    again.rejoin = true;
    CHECK(ActionFor(View::kJoin, 3, again) == RowAction::kRejoin);
    CHECK(ActionFor(View::kJoin, 2, idle) == RowAction::kTypePassword);
    CHECK(ActionFor(View::kJoin, kRows - 1, idle) == RowAction::kBack);
    Session in_one;
    in_one.in_one = in_one.rejoin = true;
    // (0.11.9: in a session the Join view is that session's: its details and Leave, nobody joins from inside it)
    CHECK(ActionFor(View::kJoin, 0, in_one) == RowAction::kViewSession);
    CHECK(ActionFor(View::kJoin, 1, in_one) == RowAction::kConfirmLeave);
    CHECK(ActionFor(View::kJoin, 2, in_one) == RowAction::kNone);
    CHECK(ActionFor(View::kJoin, 3, in_one) == RowAction::kNone);
    Session launcher;
    launcher.by_launcher = launcher.rejoin = true;
    CHECK(ActionFor(View::kJoin, 0, launcher) == RowAction::kViewSession);
    SessionView jv;
    jv.s = again;
    jv.rejoin_host = "Saria";
    CHECK(RowTexts(View::kJoin, jv) == std::vector<std::string>{"Paste code and join", "Room or IP: A to type",
                                                               "Password: A to type", "Join Saria again", "Back"});
    // The main view is the five rows; out of range is nothing.
    CHECK(ActionFor(View::kMain, kRowPlayers, idle) == ActionFor(kRowPlayers, idle));
    CHECK(RowTexts(View::kMain, none) == RowTexts(none));
    CHECK(ActionFor(View::kJoin, -1, idle) == RowAction::kNone);
    CHECK(ActionFor(View::kLetIn, kRows, host) == RowAction::kNone);
    for (View w : {View::kPlayers, View::kLetIn, View::kJoin}) {
        for (const std::string& r : RowTexts(w, lv)) CHECK(r.size() <= kRowChars);   // ("": not shown)
    }
    for (RowAction a : {RowAction::kViewPlayers, RowAction::kViewLetIn, RowAction::kViewJoin, RowAction::kBack,
                        RowAction::kDeny, RowAction::kJoinPaste, RowAction::kRejoin}) {
        CHECK(std::string(ActionWords(a)).size() > 3);
    }
}

TEST_CASE("pause tab: in a view, B goes back to the five rows instead of leaving the page") {
    std::uint64_t now = 1000;
    Page p = UsablePage();
    OnSystem(p, now);
    REQUIRE(Open(p, now) == 0);
    p.OnPad(0, now + 1);
    p.SetSubView(true);
    CHECK(p.OnPad(kPadB, now + 2) == 0);   // (a view: ours)
    CHECK(p.TakeBack());
    CHECK_FALSE(p.TakeBack());
    CHECK(p.Shown());   // (the page stays, on its five rows)
    CHECK_FALSE(p.TakeLeft());
    CHECK(p.OnPad(kPadB, now + 3) == 0);   // (held: still ours)
    p.SetSubView(false);
    CHECK(p.OnPad(kPadB, now + 4) == 0);   // (still held from the view: not a fresh B for the rows)
    CHECK(p.Shown());
    p.OnPad(0, now + 5);
    // The five rows: B is ours too, System back (the game's B would close the menu), kept from the game until let go.
    CHECK(p.OnPad(kPadB, now + 6) == 0);
    CHECK_FALSE(p.Shown());
    CHECK(p.TakeLeft());
    CHECK_FALSE(p.TakeBack());
    CHECK(p.OnPad(kPadB, now + 7) == 0);
    p.OnPad(0, now + 8);
    CHECK(p.OnPad(kPadB, now + 9) == kPadB);   // (a new B on System: the game's)
    // The page going ends the view.
    p.OnPad(0, now + 10);
    REQUIRE(Open(p, now + 11) == 0);
    p.OnPad(0, now + 12);
    p.SetSubView(true);
    CHECK(p.OnPad(kPadL, now + 13) == kPadL);
    CHECK_FALSE(p.Shown());
    p.OnPad(0, now + 14);
    CHECK(p.OnPad(kPadB, now + 15) == kPadB);
    CHECK_FALSE(p.TakeBack());
    p.SetSubView(true);   // (not shown: no view)
    CHECK(p.OnPad(kPadB, now + 16) == kPadB);
}

TEST_CASE("pause tab: a screen update nested on the same thread is told so, other threads are not") {
    {
        const ScreenGuard outer;
        CHECK_FALSE(outer.Nested());
        {
            const ScreenGuard inner;   // (a game call of ours ran a screen update again)
            CHECK(inner.Nested());
        }
        const ScreenGuard again;
        CHECK(again.Nested());   // (the inner one ending leaves the outer one in place)
        bool other = true;
        std::thread t([&] {
            const ScreenGuard g;
            other = g.Nested();
        });
        t.join();
        CHECK_FALSE(other);
    }
    const ScreenGuard after;
    CHECK_FALSE(after.Nested());
}

TEST_CASE("pause tab: the Session view: the host's lines to copy, a guest's host, why co-op failed") {
    // The host: its players, the room (or invite code) and the same-network and VPN lines to copy; Back last.
    Session host;
    host.hosting = host.in_one = host.code = true;
    CHECK(ActionFor(View::kSession, 0, host) == RowAction::kNone);
    CHECK(ActionFor(View::kSession, 1, host) == RowAction::kCopyCode);
    CHECK(ActionFor(View::kSession, 2, host) == RowAction::kCopyLan);
    CHECK(ActionFor(View::kSession, 3, host) == RowAction::kCopyVpn);
    CHECK(ActionFor(View::kSession, kRows - 1, host) == RowAction::kBack);
    host.code = false;   // (the code still coming: nothing to copy yet)
    CHECK(ActionFor(View::kSession, 1, host) == RowAction::kNone);
    SessionView hv;
    hv.s = host;
    hv.players = 3;
    CHECK(RowTexts(View::kSession, hv) == std::vector<std::string>{"Hosting: 3 players", "Room: coming...",
                                                                    "Copy same-network line", "Copy VPN line", "Back"});
    hv.s.code = true;
    hv.room = "forest-gang";
    hv.note = "Copied the room name";   // (what the last copy did, on the row the player looks at)
    CHECK(RowTexts(View::kSession, hv)[0] == "Copied the room name");
    CHECK(RowTexts(View::kSession, hv)[1] == "Copy room: forest-gang");
    hv.note.clear();
    hv.players = 1;
    CHECK(RowTexts(View::kSession, hv)[0] == "Hosting: just you");
    // Its join password: the first row shows it on the details list or hides it (a new one is typed from the list's
    // Join password line); the other rows keep their copies.
    hv.s.has_password = host.has_password = true;
    CHECK(ActionFor(View::kSession, 0, host) == RowAction::kShowPassword);
    CHECK(RowTexts(View::kSession, hv)[0] == "Show join password");
    CHECK(ActionFor(View::kSession, 2, host) == RowAction::kCopyLan);
    CHECK(ActionFor(View::kSession, kRows - 1, host) == RowAction::kBack);
    CHECK(RowTexts(View::kPassword, hv) ==
          std::vector<std::string>{"Show join password", "Change join password", "", "", "Back"});
    CHECK(ActionFor(View::kPassword, 0, host) == RowAction::kShowPassword);
    CHECK(ActionFor(View::kPassword, 1, host) == RowAction::kChangePassword);
    CHECK(ActionFor(View::kPassword, 2, host) == RowAction::kNone);
    CHECK(ActionFor(View::kPassword, kRows - 1, host) == RowAction::kBack);
    CHECK(BackOf(View::kPassword) == View::kSession);
    hv.host_password = "ABCDEFGHJKLMNPQRSTUV";
    hv.s.password_shown = host.password_shown = true;
    CHECK(RowTexts(View::kPassword, hv)[0] == "Hide join password");
    CHECK(DetailLines(View::kPassword, hv)[0].name == "Hosting");   // (the password on the details list there too)
    Session guest_pw;   // (not hosting: nothing to show or change)
    guest_pw.in_one = true;
    CHECK(ActionFor(View::kPassword, 1, guest_pw) == RowAction::kNone);
    hv.s.password_shown = host.password_shown = false;
    hv.host_password.clear();
    // The Host view's code row says what its copy did too.
    hv.note = "No clipboard here";
    CHECK(RowTexts(View::kHost, hv)[0] == "No clipboard here");

    // A guest: whose game, how many, Resync from host when its game can take the host's save, co-op's hint.
    Session guest;
    guest.in_one = true;
    for (int r = 0; r < kRows - 1; ++r) CHECK(ActionFor(View::kSession, r, guest) == RowAction::kNone);
    guest.can_resync = true;
    CHECK(ActionFor(View::kSession, 3, guest) == RowAction::kResync);
    SessionView gv;
    gv.s = guest;
    gv.host_name = "Saria";
    gv.players = 2;
    // (how many, and co-op's hint: on the details list; rows with nothing on them are not shown)
    CHECK(RowTexts(View::kSession, gv) == std::vector<std::string>{"Joined Saria", "", "", "Resync from host", "Back"});
    gv.why = "Voice chat is off";
    CHECK(RowTexts(View::kSession, gv)[2].empty());
    // A guest the host could not reach: its answer code to copy.
    Session waiting;
    waiting.in_one = waiting.wait_host = true;
    CHECK(ActionFor(View::kSession, 1, waiting) == RowAction::kCopyAnswer);
    SessionView wv;
    wv.s = waiting;
    wv.connecting = true;
    CHECK(RowTexts(View::kSession, wv)[0] == "Waiting for the host");
    CHECK(RowTexts(View::kSession, wv)[1] == "Copy answer code");

    // Failed: Host again (not for co-op that came with the game's start); why is on the details list.
    Session failed;
    failed.failed = true;
    CHECK(ActionFor(View::kSession, 3, failed) == RowAction::kHost);
    CHECK(ActionFor(View::kSession, 1, failed) == RowAction::kNone);
    SessionView fv;
    fv.s = failed;
    fv.why = "The port 27015 is in use by another program: close it, or pick another port in coop.ini";
    CHECK(RowTexts(View::kSession, fv) == std::vector<std::string>{"Co-op failed", "", "", "Host again", "Back"});
    fv.s.by_launcher = true;
    CHECK(ActionFor(View::kSession, 3, fv.s) == RowAction::kNone);
    CHECK(RowTexts(View::kSession, fv)[3].empty());
    fv.s.by_launcher = false;
    SessionView idle;
    CHECK(RowTexts(View::kSession, idle)[0] == "Not in a session");
    for (const SessionView& v : {hv, gv, wv, fv, idle}) {
        CHECK(!RowTexts(View::kSession, v)[0].empty());   // (the first row always says something: the cursor lands there)
        for (const std::string& r : RowTexts(View::kSession, v)) CHECK(r.size() <= kRowChars);
    }
}

TEST_CASE("pause tab: the details list on the right: the session, why co-op failed, the release") {
    // In no session: the header says so, auto-rejoin, and the release last (its brand left off).
    SessionView idle;
    idle.version = "Squadala! v0.11.6";
    idle.s.auto_rejoin = true;
    std::vector<ListLine> d = DetailLines(View::kMain, idle);
    REQUIRE(d.size() == static_cast<std::size_t>(kListRows));
    CHECK(d[0].name == "Not in a session");
    CHECK(d[0].value.empty());   // (the header: no value box)
    CHECK(d[1].name == "Room name");
    CHECK(d[1].value == "None");
    CHECK(d[2].name == "List my room");
    CHECK(d[2].value == "Off");
    CHECK(d[3].name == "Auto-rejoin");
    CHECK(d[3].value == "On");
    CHECK(d[4].name == "Release");
    CHECK(d[4].value == "v0.11.6");
    CHECK(d[5].name.empty());
    idle.room_name = "kokiri";
    idle.s.list_room = true;
    CHECK(DetailLines(View::kMain, idle)[1].value == "kokiri");
    CHECK(DetailLines(View::kMain, idle)[2].value == "On");
    CHECK(DetailLines(View::kSession, idle)[0].name == "Not in a session");   // (the same on the Session view)
    for (View w : {View::kPlayers, View::kJoin, View::kLetIn, View::kLan}) {
        for (const ListLine& l : DetailLines(w, idle)) CHECK(l.name.empty());   // (none on the other views)
    }
    // Failed: why as lines of text (no value) on the free lines, the release kept on the last.
    SessionView fv;
    fv.s.failed = true;
    fv.version = "Squadala! v0.11.6";
    fv.why = "no answer from the relay 127.0.0.1:27530: is the address right, and is the relay running? (a relay from "
             "before room names does not answer room names: ask the host for the join code)";
    d = DetailLines(View::kMain, fv);
    CHECK(d[0].name == "Co-op failed");
    CHECK(d[1].name.rfind("no answer from the relay", 0) == 0);
    for (int i = 1; i < kListRows; ++i) {
        CHECK(!d[static_cast<std::size_t>(i)].name.empty());
        CHECK(d[static_cast<std::size_t>(i)].value.empty());
        CHECK(d[static_cast<std::size_t>(i)].name.size() <= kListNameChars);
    }
    // (it needs every line: the release gives its line up, and the rest is cut)
    CHECK(d[kListRows - 1].name.substr(d[kListRows - 1].name.size() - 3) == "...");
    fv.why = "The port 27015 is in use";   // (a short one: the release on the line after it)
    d = DetailLines(View::kMain, fv);
    CHECK(d[1].name == "The port 27015 is in use");
    CHECK(d[2].name == "Release");
    // Hosting: the room, how many, the join password (hidden until asked, then in fives), who asks.
    SessionView hv;
    hv.s.hosting = hv.s.in_one = hv.s.code = hv.s.has_password = true;
    hv.room = "forest-gang";
    hv.players = 2;
    d = DetailLines(View::kSession, hv);
    CHECK(d[0].name == "Hosting");
    CHECK(d[1].name == "Room");
    CHECK(d[1].value == "forest-gang");
    CHECK(d[2].value == "2");
    CHECK(d[3].name == "Join password");
    CHECK(d[3].value == "Hidden");
    CHECK(d[4].name.empty());   // (nobody asks: the Let in row says so)
    hv.asking = {"Mido"};
    d = DetailLines(View::kSession, hv);
    CHECK(d[4].name == "Asking to join");
    CHECK(d[4].value == "1");
    hv.asking.clear();
    hv.s.password_shown = true;
    hv.host_password = "ABCDEFGHJKLMNPQRSTUV";
    d = DetailLines(View::kSession, hv);
    CHECK(d[3].value == "ABCDE FGHJK");
    CHECK(d[4].name == " ");
    CHECK(d[4].value == "LMNPQ RSTUV");
    CHECK(d[5].name.empty());
    for (const ListLine& l : d) {
        CHECK(l.name.size() <= kListNameChars);
        CHECK(l.value.size() <= kListValueChars);
    }
    // A guest: whose game and how many; a hint as text.
    SessionView gv;
    gv.s.in_one = true;
    gv.host_name = "Saria";
    gv.players = 3;
    gv.why = "Voice chat is off";
    d = DetailLines(View::kMain, gv);
    CHECK(d[0].name == "In a session");
    CHECK(d[1].name == "Host");
    CHECK(d[1].value == "Saria");
    CHECK(d[2].value == "3");
    CHECK(d[3].name == "Voice chat is off");
    CHECK(d[3].value.empty());
}

TEST_CASE("pause tab: the details list: who can reach a host, the router in words of ours, Auto-rejoin's A") {
    // The header's value says who can reach the host (it had none: the game's cursor framed an empty box there).
    SessionView hv;
    hv.s.hosting = hv.s.in_one = hv.s.has_password = true;
    hv.version = "Squadala! v0.11.7";
    hv.players = 1;
    CHECK(DetailLines(View::kMain, hv)[0].value == "Starting...");
    hv.s.invite = hv.s.code = true;
    std::vector<ListLine> d = DetailLines(View::kMain, hv);
    CHECK(d[0].name == "Hosting");
    CHECK(d[0].value == "Online");
    // The router kept its port shut: a line of ours for it and one hint line, the release still on the last.
    hv.problem = HOS_COOP_PROBLEM_UPNP_REFUSED;
    hv.why = "the router did not open the port (UPnP, PCP and NAT-PMP are off on it, or it said no)";
    hv.port = 27510;
    d = DetailLines(View::kMain, hv);
    CHECK(d[1].name == "Room");
    CHECK(d[1].value == "Invite code");
    CHECK(d[1].act == RowAction::kCopyCode);          // (an A copies it)
    CHECK(d[2].name == "Players in");
    CHECK(d[2].act == RowAction::kNone);              // (just us: no list)
    CHECK(d[3].act == RowAction::kChangePassword);    // (a new one on the keyboard)
    CHECK(d[4].name == "Port");
    CHECK(d[4].value == "27510 shut");
    CHECK(d[4].act == RowAction::kChangePort);        // (another port, hosting again on it)
    CHECK(d[5].name == "Forward UDP 27510 to this PC");   // (what to forward by hand)
    CHECK(d[5].value.empty());
    CHECK(d[5].act == RowAction::kNone);
    CHECK(d[6].name == "Release");
    for (const ListLine& l : d) {
        CHECK(l.name.find("UPnP") == std::string::npos);
        CHECK(l.name.size() <= kListNameChars);
        CHECK(l.value.size() <= kListValueChars);
    }
    hv.s.listed = 2;
    CHECK(DetailLines(View::kMain, hv)[2].act == RowAction::kViewPlayers);
    // No invite code and no room: this network only.
    SessionView lan;
    lan.s.hosting = lan.s.in_one = true;
    lan.problem = HOS_COOP_PROBLEM_NO_INVITE_CODE;
    lan.why = "no invite code: STUN did not answer";
    d = DetailLines(View::kMain, lan);
    CHECK(d[0].value == "This network");
    CHECK(d[1].value == "Coming...");
    // The port open: said so, no hint.
    SessionView open;
    open.s.hosting = open.s.in_one = open.s.invite = true;
    open.port_open = true;
    d = DetailLines(View::kMain, open);
    CHECK(d[0].value == "Online");
    CHECK(d[4].name == "Port");
    CHECK(d[4].value == "open");   // (no port known here)
    // Another hint keeps its words, with a capital letter.
    open.why = "voice chat is off";
    CHECK(DetailLines(View::kMain, open)[5].name == "Voice chat is off");
    // Not in a session: the Room name line types it, List my room and Auto-rejoin are switches (an A on them), the
    // others nothing.
    SessionView idle;
    idle.version = "Squadala! v0.11.7";
    d = DetailLines(View::kMain, idle);
    CHECK(d[1].act == RowAction::kTypeRoomName);
    CHECK(d[2].act == RowAction::kListRoom);
    CHECK(d[3].name == "Auto-rejoin");
    CHECK(d[3].act == RowAction::kAutoRejoin);
    CHECK(d[0].act == RowAction::kNone);
    CHECK(d[4].act == RowAction::kNone);
}

TEST_CASE("pause tab: the game's cursor on a hidden button goes on the way it came, past the rows back up") {
    using In = RowSkip::In;
    const auto at = [](std::uint64_t t, int focused, bool hidden) {
        In i;
        i.now_ms = t, i.focused = focused, i.hidden = hidden;
        return i;
    };
    // The Players view with one player: row 0 and Back (4) shown, 1 to 3 hidden. Down from row 0 lands on 1: on down.
    RowSkip s;
    std::uint64_t t = 1000;
    In i = at(t, 0, false);
    i.down = true;
    CHECK(s.Step(i) == 0);              // (the player's own step; shown row: nothing)
    CHECK(s.Step(at(t += 16, 1, true)) == kPadDown);
    CHECK(s.Skipping());
    CHECK(s.Step(at(t += 16, 1, true)) == 0);          // (let go)
    CHECK(s.Step(at(t += 16, 1, true)) == 0);          // (not moved yet: wait)
    CHECK(s.Step(at(t += 16, 2, true)) == kPadDown);
    CHECK(s.Step(at(t += 16, 2, true)) == 0);
    CHECK(s.Step(at(t += 16, -1, false)) == 0);        // (no button focused a read: as it was)
    CHECK(s.Step(at(t += 16, 3, true)) == kPadDown);
    CHECK(s.Step(at(t += 16, 3, true)) == 0);
    CHECK(s.Step(at(t += 16, 4, false)) == 0);         // (Back: there)
    CHECK(!s.Skipping());
    // Up from Back: on up to row 0.
    i = at(t += 16, 4, false);
    i.up = true;
    CHECK(s.Step(i) == 0);
    CHECK(s.Step(at(t += 16, 3, true)) == kPadUp);
    CHECK(s.Step(at(t += 16, 3, true)) == 0);
    CHECK(s.Step(at(t += 16, 2, true)) == kPadUp);
    // Past our rows (System's own Pa_Button_05 under Back): back up, whichever way it came.
    RowSkip past;
    i = at(t += 16, 4, false);
    i.down = true;
    CHECK(past.Step(i) == 0);
    CHECK(past.Step(at(t += 16, 5, true)) == kPadUp);
    CHECK(past.Step(at(t += 16, 5, true)) == 0);
    CHECK(past.Step(at(t += 16, 4, false)) == 0);
    // At an end of the rows (the cursor did not move): the other way.
    RowSkip end;
    i = at(t += 16, 0, false);
    i.up = true;
    CHECK(end.Step(i) == 0);
    CHECK(end.Step(at(t += 16, 1, true)) == kPadUp);   // (a row just hidden under the cursor: up, as it last went)
    CHECK(end.Step(at(t += 16, 1, true)) == 0);
    CHECK(end.Step(at(t += 200, 1, true)) == kPadDown);   // (still there: the other way)
    // The mouse's step counts as the way it went; the player's own press this read is left to the game.
    RowSkip mouse;
    i = at(t += 16, 2, true);
    i.mouse_down = true;
    CHECK(mouse.Step(i) == kPadDown);
    i = at(t += 16, 3, true);
    i.up = true;
    CHECK(mouse.Step(i) == 0);
    CHECK(mouse.Step(at(t += 16, 3, true)) == kPadUp);
}

TEST_CASE("pause tab: the Players view's pages and a player's view, with the host's controls") {
    // Up to four players a page; more: three a page, the fourth row turns it.
    CHECK(PlayerIndex(2, 0, 0) == 0);
    CHECK(PlayerIndex(2, 0, 2) == -1);
    CHECK(PlayerIndex(4, 0, 3) == 3);
    CHECK(PlayerIndex(0, 0, 0) == -1);
    CHECK(PlayerPages(4) == 1);
    CHECK(PlayerPages(5) == 2);
    CHECK(PlayerPages(7) == 3);
    CHECK(PlayerIndex(5, 0, 3) == -1);   // (the page's turn)
    CHECK(PlayerIndex(5, 1, 0) == 3);
    CHECK(PlayerIndex(5, 1, 1) == 4);
    CHECK(PlayerIndex(5, 1, 2) == -1);
    CHECK(PlayerIndex(5, 2, 0) == 0);    // (past the last page: the first again)
    CHECK(PlayerIndex(5, 0, kRows - 1) == -1);
    Session list;
    list.in_one = true;
    list.listed = 5;
    CHECK(ActionFor(View::kPlayers, 0, list) == RowAction::kViewPlayer);
    CHECK(ActionFor(View::kPlayers, 3, list) == RowAction::kMorePlayers);
    list.self = 1;   // (our own row: nothing to do to ourselves)
    CHECK(ActionFor(View::kPlayers, 1, list) == RowAction::kNone);
    CHECK(ActionFor(View::kPlayers, 2, list) == RowAction::kViewPlayer);
    list.self = -1;
    list.page = 1;
    CHECK(ActionFor(View::kPlayers, 2, list) == RowAction::kNone);
    SessionView lv;
    lv.s = list;
    lv.names = {"a", "b", "c", "d", "e"};
    CHECK(RowTexts(View::kPlayers, lv) == std::vector<std::string>{"d", "e", "", "More players (2/2)", "Back"});

    // Another player, as a guest: teleport and our own mutes; nothing more.
    Session other;
    other.in_one = true;
    other.player = 2;
    CHECK(ActionFor(View::kPlayer, 0, other) == RowAction::kTeleport);
    CHECK(ActionFor(View::kPlayer, 1, other) == RowAction::kOwnVoice);
    CHECK(ActionFor(View::kPlayer, 2, other) == RowAction::kOwnChat);
    CHECK(ActionFor(View::kPlayer, 3, other) == RowAction::kNone);
    CHECK(ActionFor(View::kPlayerHost, 2, other) == RowAction::kNone);   // (no host controls for a guest)
    SessionView pv;
    pv.s = other;
    pv.player_name = "Mido";
    CHECK(RowTexts(View::kPlayer, pv) == std::vector<std::string>{"Teleport to Mido", "Mute their voice (you)",
                                                                   "Mute their chat (you)", "", "Back"});
    pv.own_mutes = kMuteVoice | kMuteChat;
    CHECK(RowTexts(View::kPlayer, pv)[1] == "Hear their voice again");
    CHECK(RowTexts(View::kPlayer, pv)[2] == "Show their chat again");
    // The host: its controls on row 3, then mutes for everyone, Kick and Ban (each after a second A).
    Session host = other;
    host.hosting = true;
    CHECK(ActionFor(View::kPlayer, 3, host) == RowAction::kHostControls);
    CHECK(ActionFor(View::kPlayerHost, 0, host) == RowAction::kHostVoice);
    CHECK(ActionFor(View::kPlayerHost, 1, host) == RowAction::kHostChat);
    CHECK(ActionFor(View::kPlayerHost, 2, host) == RowAction::kKick);
    CHECK(ActionFor(View::kPlayerHost, 3, host) == RowAction::kBan);
    CHECK(ActionFor(View::kPlayerHost, kRows - 1, host) == RowAction::kBack);
    SessionView cv;
    cv.s = host;
    cv.player_name = "Mido";
    CHECK(RowTexts(View::kPlayerHost, cv) == std::vector<std::string>{"Mute voice for all", "Mute chat for all",
                                                                       "Kick Mido", "Ban for this session", "Back"});
    cv.host_mutes = kMuteVoice;
    cv.confirm_kick = true;
    CHECK(RowTexts(View::kPlayerHost, cv)[0] == "Unmute voice for all");
    CHECK(RowTexts(View::kPlayerHost, cv)[2] == "A again: kick Mido");
    cv.confirm_kick = false, cv.confirm_ban = true;
    CHECK(RowTexts(View::kPlayerHost, cv)[3] == "A again: ban Mido");
    // Us, or a player who left: nothing to do to them.
    Session self = host;
    self.player = 1;
    Session gone = host;
    gone.player = 0;
    for (const Session& s : {self, gone}) {
        for (int r = 0; r < kRows - 1; ++r) {
            CHECK(ActionFor(View::kPlayer, r, s) == RowAction::kNone);
            CHECK(ActionFor(View::kPlayerHost, r, s) == RowAction::kNone);
        }
    }
    SessionView sv;
    sv.s = self;
    sv.player_name = "dauro";
    CHECK(RowTexts(View::kPlayer, sv)[0] == "dauro (you)");
    sv.s = gone;
    CHECK(RowTexts(View::kPlayer, sv)[0] == "They left the session");
    CHECK(RowTexts(View::kPlayerHost, sv)[0] == "They left the session");
    // Back (and B): the host's controls to the player, the player to the list, the rest to the five rows.
    CHECK(BackOf(View::kPlayerHost) == View::kPlayer);
    CHECK(BackOf(View::kPlayer) == View::kPlayers);
    for (View w : {View::kPlayers, View::kLetIn, View::kJoin, View::kSession, View::kMain}) {
        CHECK(BackOf(w) == View::kMain);
    }
    for (RowAction a : {RowAction::kViewSession, RowAction::kCopyLan, RowAction::kCopyVpn, RowAction::kResync,
                        RowAction::kViewPlayer, RowAction::kMorePlayers, RowAction::kHostControls,
                        RowAction::kTeleport, RowAction::kOwnVoice, RowAction::kOwnChat, RowAction::kHostVoice,
                        RowAction::kHostChat, RowAction::kKick, RowAction::kBan}) {
        CHECK(std::string(ActionWords(a)).size() > 3);
    }
}

TEST_CASE("pause tab: a room name typed with a pad: Up and Down cycle a letter, Left and Right move, Y deletes") {
    TextField f(TextField::Kind::kRoom);
    CHECK(f.Shown(false, 40) == "");
    CHECK(f.Shown(true, 40) == "[ ]");
    f.Cycle(+1);                       // (after the last: a new letter, the first)
    CHECK(f.text() == "a");
    f.Cycle(+1);
    f.Cycle(+1);
    CHECK(f.text() == "c");
    f.Cycle(-1);
    CHECK(f.text() == "b");
    f.Move(+1);                        // (one past the end: the next letter)
    f.Cycle(-1);                       // (a new one from the end of the letters: _)
    CHECK(f.text() == "b_");
    CHECK(f.Shown(true, 40) == "b[_]");
    f.Move(-1);
    CHECK(f.Shown(true, 40) == "[b]_");
    f.Delete();
    CHECK(f.text() == "_");
    CHECK_FALSE(f.Ready());            // (3 letters at least)
    f.Clear();
    for (const char c : std::string("Forest-Gang")) CHECK(f.Type(static_cast<unsigned char>(c)));
    CHECK(f.text() == "forest-gang");  // (any case: the relay's)
    CHECK(f.Ready());
    CHECK_FALSE(f.Type(' '));
    CHECK_FALSE(f.Type('!'));
    CHECK_FALSE(f.Type(0xe9));         // (no accents: not a room's letter)
    f.Backspace();
    CHECK(f.text() == "forest-gan");
}

TEST_CASE("pause tab: a room name stops at 24, a password at 128, and the cursor stays on the last letter then") {
    TextField room(TextField::Kind::kRoom);
    for (int i = 0; i < 30; ++i) room.Type('a');
    CHECK(room.text().size() == 24);
    room.Move(+1);
    CHECK(room.cursor() == 23);        // (full: no place after the last)
    room.Cycle(+1);
    CHECK(room.text().back() == 'b');
    TextField pw(TextField::Kind::kPassword);
    for (int i = 0; i < 200; ++i) pw.Type('x');
    CHECK(pw.text().size() == 128);
    CHECK(pw.Ready());                 // (a password may be anything, none included)
    TextField code(TextField::Kind::kPassword);
    code.Cycle(+1);
    CHECK(code.text() == "A");         // (a host's own password's letters first: the pad reaches them soonest)
    code.Cycle(-1);
    CHECK(code.text() == "_");
}

TEST_CASE("pause tab: a password shows as stars but for the letter typed into, and a long text keeps the cursor in view") {
    TextField pw(TextField::Kind::kPassword);
    for (const char c : std::string("Hunter2!")) pw.Type(static_cast<unsigned char>(c));
    CHECK(pw.text() == "Hunter2!");    // (capitals and punctuation kept)
    CHECK(pw.Shown(false, 40) == "********");
    pw.Move(-1);
    CHECK(pw.Shown(true, 40) == "*******[!]");
    TextField room(TextField::Kind::kRoom);
    for (int i = 0; i < 24; ++i) room.Type(static_cast<char>('a' + i % 26));
    const std::string shown = room.Shown(true, 12);
    CHECK(shown.size() == 12);
    CHECK(shown.find('[') != std::string::npos);   // (the cursor's letter in view)
    // A long password with '[' in it: the view follows the cursor, wherever the brackets fall.
    TextField brackets(TextField::Kind::kPassword);
    for (int i = 0; i < 30; ++i) brackets.Type(i % 3 == 0 ? '[' : 'x');
    for (int i = 0; i < 26; ++i) brackets.Move(-1);   // (the cursor on the fifth letter, an 'x')
    CHECK(brackets.Shown(true, 12).find("[x]") != std::string::npos);
    while (brackets.cursor() < brackets.text().size()) brackets.Move(+1);
    CHECK(brackets.Shown(true, 12).find("[ ]") != std::string::npos);   // (at the end: the empty place in view)
}

TEST_CASE("pause tab: typing with the pad: the list's rows are the keys, every button the page's until OK or Cancel") {
    Page p = UsablePage();
    OnSystem(p, 1000);
    Open(p, 1000);
    p.OnPad(0, 1016);
    REQUIRE(p.Shown());
    p.StartTyping(0);
    CHECK(p.Typing() == 0);
    CHECK(p.TypingKey() == static_cast<int>(TypeKey::kLetter));
    CHECK(p.TypingLetter() == 'a');
    CHECK(p.OnPad(kPadA, 1032) == 0);           // (the A that opened it, coming again: a click taps A just after)
    CHECK(p.Field(0).text().empty());
    p.OnPad(0, 1048);
    CHECK(p.OnPad(kPadA, 1432) == 0);           // (A on Letter: its letter in, a room name's a)
    CHECK(p.Field(0).text() == "a");
    CHECK(p.OnPad(0, 1448) == 0);
    CHECK(p.OnPad(kPadRight, 1464) == 0);       // (Right on Letter: the next letter; none reaches System)
    CHECK(p.TypingLetter() == 'b');
    CHECK(p.OnPad(kPadRight | kPadA, 1480) == 0);
    CHECK(p.Field(0).text() == "ab");
    p.OnPad(0, 1485);
    p.OnPad(kPadLeft, 1486);                    // (Left: back, round the letters)
    p.OnPad(0, 1487);
    p.OnPad(kPadLeft, 1488);
    CHECK(p.TypingLetter() == '_');             // (before the first: the last, a room name's _)
    // Held: again and again after a moment (the screens updating meanwhile, as in a game).
    p.OnPad(0, 1490);
    OnSystem(p, 1500);
    p.OnPad(kPadDown, 1500);
    CHECK(p.TypingKey() == static_cast<int>(TypeKey::kDelete));
    OnSystem(p, 1650);
    p.OnPad(kPadDown, 1650);
    OnSystem(p, 1800);
    p.OnPad(kPadDown, 1800);
    OnSystem(p, 1950);
    p.OnPad(kPadDown, 1950);   // (held over 400 ms: a step)
    CHECK(p.TypingKey() == static_cast<int>(TypeKey::kOk));
    CHECK(p.OnPad(0, 2060) == 0);
    CHECK(p.OnPad(kPadY, 2070) == 0);            // (Y: a letter off)
    CHECK(p.Field(0).text() == "a");
    p.OnPad(0, 2071);
    p.OnPad(kPadUp, 2072);                       // (up: Delete; A deletes the letter before the cursor)
    p.OnPad(0, 2073);
    p.OnPad(kPadA, 2074);
    CHECK(p.Field(0).text().empty());
    p.OnPad(0, 2075);
    p.PressKey(static_cast<int>(TypeKey::kLetter));   // (the mouse's click on Letter)
    p.PressKey(static_cast<int>(TypeKey::kLetter));
    CHECK(p.Field(0).text() == "__");
    CHECK(p.Typing() == 0);
    p.OnPad(0, 2080);
    p.OnPad(kPadPlus, 2090);                     // (+: OK)
    CHECK(p.Typing() == -1);
    CHECK(p.TakeTyped() == 0);
    CHECK(p.TakeTyped() == -1);                  // (once)
    CHECK(p.Shown());
    CHECK_FALSE(p.TakeBack());
    p.OnPad(0, 2100);
    // B is Cancel: the field as it was.
    p.StartTyping(2);
    p.OnPad(kPadB, 2110);
    CHECK(p.Typing() == -1);
    CHECK(p.TakeTyped() == -1);
    p.OnPad(0, 2120);
    // The keys Cancel and OK, and up from Letter round to Cancel.
    p.StartTyping(2);
    CHECK(p.TypingLetter() == 'A');              // (a password's letters: a host's own come first)
    p.PressKey(static_cast<int>(TypeKey::kLetter));
    p.PressKey(static_cast<int>(TypeKey::kCancel));
    CHECK(p.Typing() == -1);
    CHECK(p.Field(2).text().empty());            // (Cancel: as it was)
    p.StartTyping(2);
    p.OnPad(kPadUp, 2130);
    CHECK(p.TypingKey() == static_cast<int>(TypeKey::kCancel));
    p.OnPad(0, 2140);
    p.PressKey(static_cast<int>(TypeKey::kLetter));
    p.PressKey(static_cast<int>(TypeKey::kOk));
    CHECK(p.Typing() == -1);
    CHECK(p.TakeTyped() == 2);
    CHECK(p.Field(2).text() == "A");
    p.PressKey(static_cast<int>(TypeKey::kLetter));   // (not typing: nothing)
    CHECK(p.Field(2).text() == "A");
    // The PC keyboard: its characters and keys while typing, none otherwise; Esc puts the field back.
    CHECK_FALSE(p.Char('x'));
    p.StartTyping(1);
    CHECK(p.Char('P'));
    CHECK(p.Char(' '));                          // (taken, but not a password's letter)
    CHECK(p.Key(0x25, false));                   // (Left)
    CHECK(p.Char('a'));
    CHECK(p.Field(1).text() == "aP");
    CHECK(p.Key(0x28, false));                   // (Down: the key picked)
    CHECK(p.TypingKey() == static_cast<int>(TypeKey::kDelete));
    CHECK(p.Key(0x0d, false));                   // (Enter: OK)
    CHECK(p.Typing() == -1);
    CHECK(p.TakeTyped() == 1);
    CHECK_FALSE(p.Key(0x0d, false));
    p.StartTyping(1);
    CHECK(p.Char('z'));
    CHECK(p.Key(0x1b, false));                   // (Esc: Cancel)
    CHECK(p.Field(1).text() == "aP");
}

TEST_CASE("pause tab: the typing keys on the list's rows: the field on the header, then Letter, Delete, OK, Cancel") {
    TextField pw(TextField::Kind::kPassword);
    for (const char c : std::string("Hunter2")) pw.Type(static_cast<unsigned char>(c));
    const std::vector<ListLine> l = TypingLines(2, pw, 'Q');
    REQUIRE(l.size() == static_cast<std::size_t>(kListRows));
    CHECK(l[0].name == "New join password");
    CHECK(l[0].value == pw.Shown(true, kListValueChars));
    CHECK(l[0].value.find("Hunter") == std::string::npos);   // (a password: stars)
    CHECK_FALSE(l[0].arrows);
    CHECK(l[1].name == "Letter");
    CHECK(l[1].value == "Q");
    CHECK(l[1].arrows);                          // (between the row's own arrows: Left and Right change it)
    CHECK(l[2].name == "Delete");
    CHECK(l[3].name == "OK");
    CHECK(l[4].name == "Cancel");
    for (int i = 2; i < kListRows; ++i) CHECK_FALSE(l[static_cast<std::size_t>(i)].arrows);
    CHECK(l[5].name.empty());                    // (the rest not shown)
    CHECK(l[6].name.empty());
    CHECK(std::string(TypingTitle(0)) == "Room name");
    CHECK(std::string(TypingTitle(1)) == "Join password");
}

TEST_CASE("pause tab: typing ends when the page goes, and the Join view's rows say the fields") {
    Page p = UsablePage();
    OnSystem(p, 1000);
    Open(p, 1000);
    p.OnPad(0, 1016);
    p.StartTyping(0);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), 1100);   // (the menu closes)
    CHECK(p.Typing() == -1);
    Session s;
    CHECK(ActionFor(View::kJoin, 1, s) == RowAction::kTypeRoom);
    CHECK(ActionFor(View::kJoin, 2, s) == RowAction::kTypePassword);
    CHECK(ActionFor(View::kJoin, 3, s) == RowAction::kNone);         // (no room name yet, no last game)
    s.room_ready = s.rejoin = true;
    CHECK(ActionFor(View::kJoin, 3, s) == RowAction::kJoinRoom);     // (a room name typed: that room, not the last game)
    s.in_one = true;
    CHECK(ActionFor(View::kJoin, 1, s) == RowAction::kConfirmLeave);  // (nobody joins from inside a session: Leave)
    CHECK(ActionFor(View::kJoin, 2, s) == RowAction::kNone);
    SessionView v;
    auto rows = RowTexts(View::kJoin, v);
    CHECK(rows[1] == "Room or IP: A to type");
    CHECK(rows[2] == "Password: A to type");
    CHECK(rows[3].empty());
    v.room_field = "fore[s]t";
    v.typing = 0;
    v.room_ready = true;
    rows = RowTexts(View::kJoin, v);
    CHECK(rows[1] == "Room or IP: fore[s]t");
    CHECK(rows[3] == "Join room: no relay set");
    v.relay_set = true;
    CHECK(RowTexts(View::kJoin, v)[3] == "Join this room");    v.typing = -1;
    v.room_field = "127.0.0.1:27510";
    CHECK(RowTexts(View::kJoin, v)[1] == "To: 127.0.0.1:27510");   // (typed: a short label, the address whole)
}

TEST_CASE("pause tab: the focused row is the System button's number under it, by its name") {
    CHECK(RowOfButton("Pa_Button_00") == 0);
    CHECK(RowOfButton("Pa_Button_04") == 4);
    CHECK(RowOfButton("Pa_Button_05") == 5);    // (System's Longshot Settings button, and Longshot Settings' sixth row)
    CHECK(RowOfButton("Pa_Button_06") == -1);
    CHECK(RowOfButton("Pa_Button_0") == -1);
    CHECK(RowOfButton("Pa_Button_010") == -1);
    CHECK(RowOfButton("N_SlideList_00") == -1);
    CHECK(RowOfButton("") == -1);
}

TEST_CASE("pause tab: the kit's release shows on the page's details, solo and in a session (0.11.6)") {
    SessionView solo;
    solo.version = "Squadala! v0.11.6";
    CHECK(RowTexts(solo)[kRowSettings] == "Co-op settings");
    CHECK(ActionFor(kRowSettings, solo.s) == RowAction::kViewMenu);
    const auto last = [](const std::vector<ListLine>& d) {
        for (auto it = d.rbegin(); it != d.rend(); ++it) {
            if (!it->name.empty()) return *it;
        }
        return ListLine{};
    };
    CHECK(last(DetailLines(View::kMain, solo)).name == "Release");
    CHECK(last(DetailLines(View::kMain, solo)).value == "v0.11.6");
    SessionView guest = solo;
    guest.s.in_one = true;
    guest.players = 2;
    CHECK(RowTexts(View::kJoin, guest)[kJoinRowLeave] == "Leave the game");   // in a session the Join view leaves
    CHECK(last(DetailLines(View::kSession, guest)).value == "v0.11.6");
    SessionView dev;                                  // a developer's build (no kit): no release line
    for (const ListLine& l : DetailLines(View::kMain, dev)) CHECK(l.name != "Release");
}

TEST_CASE("pause tab: the game browser: the rows choose the list, search, filter and order, the cells hold its games") {
    Session s;
    CHECK(ActionFor(View::kLan, 0, s) == RowAction::kBrowseList);
    CHECK(ActionFor(View::kLan, 1, s) == RowAction::kTypeSearch);
    CHECK(ActionFor(View::kLan, 2, s) == RowAction::kBrowseFilter);
    CHECK(ActionFor(View::kLan, 3, s) == RowAction::kBrowseSort);
    CHECK(ActionFor(View::kLan, kRows - 1, s) == RowAction::kBack);
    s.in_one = true;    // (nobody joins from inside a session)
    CHECK(ActionFor(View::kLan, 0, s) == RowAction::kNone);
    for (RowAction a : {RowAction::kBrowseList, RowAction::kBrowseFilter, RowAction::kBrowseSort, RowAction::kBrowseRefresh,
                        RowAction::kTypeSearch, RowAction::kTypeRoomName}) {
        CHECK(std::string(ActionWords(a)).size() > 3);
    }

    SessionView v;
    auto rows = RowTexts(View::kLan, v);
    CHECK(rows[0] == "This network (0)");
    CHECK(rows[1] == "Search: A to type");
    CHECK(rows[2] == "Show: All games");
    CHECK(rows[3] == "Sort: Most players");
    CHECK(rows[kRows - 1] == "Back");
    auto lines = ListLines(v);
    REQUIRE(lines.size() == static_cast<std::size_t>(kListRows));
    CHECK(lines[0].name == "This network");      // (one page: no page marks)
    CHECK(lines[0].value == "Refresh");          // (its box asks the list again)
    CHECK(lines[0].act == RowAction::kBrowseRefresh);
    CHECK(lines[1].name == "Looking for games...");
    CHECK(lines[1].value.empty());
    CHECK(lines[2].name.empty());
    v.lan_rows = {"Mido's game (1/4)", "Saria's game (full)"};
    v.lan_list = {{"Mido's game", "1/4"}, {"Saria's game: kokiri", "Full"}};
    v.s.lan_listed = 2;
    lines = ListLines(v);
    CHECK(lines[1].name == "Mido's game");
    CHECK(lines[1].value == "1/4");
    CHECK(lines[2].name == "Saria's game: kokiri");
    CHECK(lines[2].value == "Full");
    CHECK(lines[3].name.empty());
    CHECK(RowTexts(View::kLan, v)[0] == "This network (2)");
    v.s.online = true;   // (the relay's list: why there are none, in the first game row)
    v.online_state = "No relay set";
    rows = RowTexts(View::kLan, v);
    CHECK(rows[0] == "Public games (0)");
    CHECK(ListLines(v)[0].name == "Public games");
    CHECK(ListLines(v)[1].name == "No relay set: A to type one");   // (typed on the keyboard, saved)
    CHECK(ListLines(v)[1].act == RowAction::kTypeRelay);
    v.relay_set = true;   // (a relay that lists nothing: why, as it says)
    CHECK(ListLines(v)[1].name == "No relay set");
    CHECK(ListLines(v)[1].act == RowAction::kNone);
    v.browse_all_out = true;   // (rooms heard, none kept by the search or the filter)
    CHECK(ListLines(v)[1].name == "None match the search or Show");
    v.browse_all_out = false;
    v.search = "kok";
    v.s.browse_filter = 1, v.s.browse_sort = 2;
    rows = RowTexts(View::kLan, v);
    CHECK(rows[1] == "Search: kok");
    CHECK(rows[2] == "Show: Open games");
    CHECK(rows[3] == "Sort: Name");
    v.search.clear();
    v.relay_set = false;
    v.online_state.clear();
    v.online_rows.assign(10, "kokiri (1/4)");
    v.online_list.assign(10, ListLine{"kokiri (Saria)", "1/4"});
    v.online_list[7] = ListLine{"a room with a very long name indeed (Saria)", "Other version"};
    v.s.online_listed = 10;
    rows = RowTexts(View::kLan, v);
    CHECK(rows[0] == "Public games (10)");
    lines = ListLines(v);
    CHECK(lines[0].name == "Public games  < 1 of 2 >");   // (more than a page: the heading pages it)
    CHECK(lines[2].name == "kokiri (Saria)");
    v.s.cell_page = 1;   // (the second page: the 7th to 10th rooms)
    lines = ListLines(v);
    CHECK(lines[0].name == "Public games  < 2 of 2 >");
    CHECK(lines[1].name == "kokiri (Saria)");
    CHECK(lines[2].name == "a room with a very long nam...");   // (cut to kListNameChars)
    CHECK(lines[2].name.size() <= kListNameChars);
    CHECK(lines[2].value == "Other version");
    CHECK(lines[4].name == "kokiri (Saria)");
    CHECK(lines[5].name.empty());
    CHECK(CellIndex(v.s, 3) == 9);
    CHECK(CellIndex(v.s, 4) == -1);
    for (const std::string& r : RowTexts(View::kLan, v)) CHECK(r.size() <= kRowChars);
    CHECK(ListValue(2, 4, true) == "2/4");
    CHECK(ListValue(4, 4, true) == "Full");
    CHECK(ListValue(1, 4, false) == "Other version");
    CHECK(ListValue(3, 0, true) == "3");
    // A room: the name its host listed it under, with the room name after it when that is another.
    CHECK(ListName("forest-gang", "forest-gang") == "forest-gang");
    CHECK(ListName("Forest-Gang", "forest-gang") == "Forest-Gang");   // (the same room name in another case)
    CHECK(ListName("", "forest-gang") == "forest-gang");
    CHECK(ListName("Saria's forest", "forest-gang") == "Saria's forest (forest-gang)");
}

TEST_CASE("pause tab: the browser's list: up and down among its games, Left to the rows, the row under the mouse") {
    CHECK(CellMove(0, 0, 1, 6) == 1);
    CHECK(CellMove(5, 0, 1, 6) == 5);              // (the bottom)
    CHECK(CellMove(2, 0, -1, 6) == 1);
    CHECK(CellMove(0, 0, -1, 6) == 0);             // (the top)
    CHECK(CellMove(1, 0, 1, 2) == 1);              // (only two games: none below)
    CHECK(CellMove(3, -1, 0, 6) == kRowsCell);     // (Left: back to the rows)
    CHECK(CellMove(3, 1, 0, 6) == 3);              // (Right: one column, nothing to the right)
    CHECK(CellMove(-1, 0, 1, 6) == kRowsCell);
    std::array<RowBox, kCells> boxes{};
    boxes[0] = RowBox{true, -400, 180, 1480, 320};
    boxes[1] = RowBox{true, -400, 8, 1480, 148};
    CHECK(CellAt(boxes, 0, 250) == 0);
    CHECK(CellAt(boxes, 900, 80) == 1);
    CHECK(CellAt(boxes, 900, 165) == -1);          // (between them)
    boxes[1].shown = false;
    CHECK(CellAt(boxes, 900, 80) == -1);
}

TEST_CASE("pause tab: a game on this network: Join this game, its password typed first if it asks one, Back to the list") {
    Session s;
    CHECK(BackOf(View::kLanGame) == View::kLan);
    CHECK(ActionFor(View::kLanGame, 0, s) == RowAction::kNone);   // (the game itself)
    CHECK(ActionFor(View::kLanGame, 1, s) == RowAction::kTypePassword);
    CHECK(ActionFor(View::kLanGame, 2, s) == RowAction::kJoinLanGame);   // (no password: the same network's way in)
    CHECK(ActionFor(View::kLanGame, kRows - 1, s) == RowAction::kBack);
    s.lan_password = true;
    CHECK(ActionFor(View::kLanGame, 2, s) == RowAction::kJoinLanGame);
    s.in_one = true;   // (nobody joins from inside a session)
    CHECK(ActionFor(View::kLanGame, 1, s) == RowAction::kNone);
    CHECK(ActionFor(View::kLanGame, 2, s) == RowAction::kNone);
    SessionView v;
    v.lan_game = "Mido's game (1/4)";
    auto rows = RowTexts(View::kLanGame, v);
    CHECK(rows[0] == "Mido's game (1/4)");
    CHECK(rows[1] == "Password: if it asks");
    CHECK(rows[2] == "Join this game");
    CHECK(rows[kRows - 1] == "Back");
    v.password_field = "kokiri";
    v.s.lan_password = true;
    rows = RowTexts(View::kLanGame, v);
    CHECK(rows[1] == "Password: kokiri");
    CHECK(rows[2] == "Join this game");
    v.s.in_one = true;
    rows = RowTexts(View::kLanGame, v);
    CHECK(rows[1] == "In a session already");
    CHECK(rows[2].empty());
}

TEST_CASE("pause tab: the Session view says why the page's Host or Join did not start, on the details list") {
    const auto text = [](const std::vector<ListLine>& d) {
        std::string all;
        for (std::size_t i = 1; i < d.size(); ++i) all += d[i].name + " ";
        return all;
    };
    SessionView v;
    v.says = "Type the join password the host sent you (every session has one).";
    auto rows = RowTexts(View::kSession, v);
    CHECK(rows[0] == "Could not start");
    CHECK(rows[kRows - 1] == "Back");
    auto d = DetailLines(View::kSession, v);
    CHECK(d[0].name == "Could not start");
    CHECK(d[1].name.rfind("Type the join", 0) == 0);
    CHECK(text(d).find("password") != std::string::npos);
    // (co-op's longest notes fit whole: a run cut this one at "th...")
    v.says = "Your clipboard is empty: copy your friend's answer code first, then press this again.";
    d = DetailLines(View::kSession, v);
    CHECK(text(d).find("...") == std::string::npos);
    CHECK(text(d).find("again.") != std::string::npos);
    v.says.clear();   // (nothing said: the view's own lines)
    rows = RowTexts(View::kSession, v);
    CHECK(rows[0] == "Not in a session");
}

TEST_CASE("pause tab: a guest the host's game turned away: the rows say so and why, and to Leave, then join again") {
    SessionView v;
    v.s.in_one = true;   // (co-op is still on until Leave, as co-op's menu says)
    v.turned_away = "the host took you out of the game";
    CHECK(RowTexts(v)[kRowJoin] == "Turned away");
    CHECK(ActionFor(kRowJoin, v.s) == RowAction::kViewJoin);
    CHECK(RowTexts(View::kJoin, v)[0] == "Turned away: why");
    CHECK(ActionFor(View::kJoin, 0, v.s) == RowAction::kViewSession);
    CHECK(ActionFor(View::kJoin, kJoinRowLeave, v.s) == RowAction::kConfirmLeave);
    CHECK(RowTexts(View::kSession, v)[0] == "Turned away");
    // (why, and to Leave, then join again: on the details list)
    const auto text = [](const std::vector<ListLine>& d) {
        std::string all;
        for (std::size_t i = 1; i < d.size(); ++i) all += d[i].name + " ";
        return all;
    };
    std::vector<ListLine> d = DetailLines(View::kSession, v);
    CHECK(d[0].name == "Turned away");
    CHECK(text(d).find("took you out") != std::string::npos);
    CHECK(text(d).find("then join again") != std::string::npos);
    v.banned = true;
    d = DetailLines(View::kSession, v);
    CHECK(text(d).find("join again") == std::string::npos);   // (a ban: joining again cannot work)
    CHECK(RowTexts(View::kJoin, v)[0] == "Turned away: why");      // (0.11.9: the Join view leaves it)
    CHECK(RowTexts(View::kJoin, v)[kJoinRowLeave] == "Leave the game");
    CHECK(RowTexts(View::kLanGame, v)[1] == "Turned away: Leave first");
    for (const View w : {View::kMain, View::kSession, View::kJoin, View::kLanGame}) {
        for (const std::string& r : RowTexts(w, v)) CHECK(r.size() <= kRowChars);
    }
}

TEST_CASE("pause tab: a stall of the whole game keeps the page, screens that stop while the pad goes on do not") {
    Page p = UsablePage();
    std::uint64_t now = 1000;
    OnSystem(p, now);
    p.OnPad(0, now);
    CHECK(Open(p, now + 16) == 0);
    CHECK(p.Shown());
    OnSystem(p, now + 32);
    p.OnPad(0, now + 32);
    // 40 s with no pad call and no screen update (a run froze the game that long): still ours.
    now += 32 + 40000;
    p.OnPad(0, now);
    CHECK(p.Shown());
    OnSystem(p, now + 16);
    p.OnPad(0, now + 16);
    CHECK(p.Shown());
    // The screens stop while the pad's calls go on: System back once they are stale.
    std::uint64_t t = now + 32;
    for (; t < now + 32 + 400; t += 16) p.OnPad(0, t);
    CHECK_FALSE(p.Shown());
}

TEST_CASE("pause tab: after a stall, the first call may be a screen's update: the strip's or the pages' first") {
    for (int first = 0; first < 2; ++first) {
        INFO("first after the stall: " << (first == 0 ? "the strip" : "the pages"));
        Page p = UsablePage();
        std::uint64_t now = 1000;
        OnSystem(p, now);
        p.OnPad(0, now);
        CHECK(Open(p, now + 16) == 0);
        CHECK(p.Shown());
        OnSystem(p, now + 32);
        p.OnPad(0, now + 32);
        now += 32 + 40000;   // (40 s with no call at all)
        if (first == 0) {
            p.OnScreen(Strip(0x9000, 5, kSystemPage), now);
            CHECK(p.Shown());
            p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
        } else {
            p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
            CHECK(p.Shown());
            p.OnScreen(Strip(0x9000, 5, kSystemPage), now);
        }
        CHECK(p.Shown());
        p.OnPad(0, now + 1);
        CHECK(p.Shown());
        // (and screens that stop while the other calls go on are still stale)
        std::uint64_t t = now + 16;
        for (; t < now + 16 + 400; t += 16) p.OnPad(0, t);   // (the pad's calls go on: the game runs)
        CHECK_FALSE(p.Shown());
    }
}

namespace {

// The System page's five buttons as a run's page puts them (left column, about 300 units tall, gaps between; the
// divider's wider gap between rows 1 and 2).
RowBoxes FiveRows() {
    RowBoxes b{};
    const float tops[kRows] = {700, 350, -100, -450, -800};
    for (int i = 0; i < kRows; ++i) {
        b[static_cast<std::size_t>(i)] = RowBox{true, -1500, tops[i] - 260, -800, tops[i]};
    }
    return b;
}

MouseRows::In At(std::uint64_t now, float x, float y, int focused, bool left = false) {
    MouseRows::In in;
    in.now_ms = now, in.inside = true, in.x = x, in.y = y, in.focused = focused, in.left = left;
    in.boxes = FiveRows();
    return in;
}

}  // namespace

TEST_CASE("pause tab: the mouse: a client pixel in the layout's units, and the row under it, gaps shared") {
    auto [x, y] = LayoutPoint(800, 450, 1600, 900);   // (the centre)
    CHECK(x == doctest::Approx(0));
    CHECK(y == doctest::Approx(0));
    std::tie(x, y) = LayoutPoint(0, 0, 1600, 900);   // (the top left: 3840 x 2160 fit, y up)
    CHECK(x == doctest::Approx(-1920));
    std::tie(x, y) = LayoutPoint(0, 0, 1920, 1200);   // (a taller window: bars above and below, the width fits)
    CHECK(x == doctest::Approx(-1920));
    CHECK(y == doctest::Approx(1200));
    const RowBoxes b = FiveRows();
    CHECK(RowAt(b, -1100, 600) == 0);
    CHECK(RowAt(b, -1100, -900) == kRows - 1);   // (Back)
    CHECK(RowAt(b, -1100, 420) == 0);            // (in the gap between 0 and 1, nearer 0's)
    CHECK(RowAt(b, -1100, 390) == 1);
    CHECK(RowAt(b, 0, 600) == -1);               // (off the column)
    RowBoxes hidden = b;
    hidden[4].shown = false;
    CHECK(RowAt(hidden, -1100, -900) == -1);
}

TEST_CASE("pause tab: the mouse moved onto a row steps the game's cursor there with the pad, a read held, a read let go") {
    MouseRows m;
    std::uint64_t t = 1000;
    CHECK(m.Step(At(t, -1100, 600, 3)) == kPadUp);   // (onto row 0 from row 3: up)
    CHECK(m.Step(At(t += 16, -1100, 600, 3)) == 0);  // (let go)
    CHECK(m.Step(At(t += 16, -1100, 600, 3)) == 0);  // (the cursor has not moved yet: wait)
    CHECK(m.Step(At(t += 16, -1100, 600, 2)) == kPadUp);
    CHECK(m.Step(At(t += 16, -1100, 600, 2)) == 0);
    CHECK(m.Step(At(t += 16, -1100, 600, 1)) == kPadUp);
    CHECK(m.Step(At(t += 16, -1100, 600, 1)) == 0);
    CHECK(m.Step(At(t += 16, -1100, 600, 0)) == 0);  // (there)
    CHECK(m.Step(At(t += 16, -1100, 600, 0)) == 0);
    // A still mouse leaves the pad's cursor be: the player stepped down with the pad.
    CHECK(m.Step(At(t += 16, -1100, 600, 1)) == 0);
    CHECK(m.Step(At(t += 16, -1100, 600, 2)) == 0);
    // Moved onto Back: down.
    CHECK(m.Step(At(t += 16, -1100, -900, 2)) == kPadDown);
}

TEST_CASE("pause tab: a click on a row is that row's A from its press, though the game's cursor is still on another") {
    MouseRows m;
    std::uint64_t t = 1000;
    // The cursor on row 3, the mouse already over Back (moved there earlier, the steps not taken yet).
    m.Step(At(t, -1100, -900, 3));
    m.Step(At(t += 16, -1100, -900, 3, true));   // (pressed: Longshot's A comes 50 ms on, the button still down)
    CHECK(m.Clicked(t + 50) == kRows - 1);
    CHECK(m.TakeClicked(t + 50) == kRows - 1);
    CHECK(m.Clicked(t + 60) == -1);   // (one A a click)
    m.Step(At(t += 100, -1100, -900, 3, true));
    m.Step(At(t += 16, -1100, -900, 3, false));   // (let go: no second click)
    CHECK(m.Clicked(t) == -1);
    // Pressed on one row and let go on another: the row it was pressed on (where Longshot's A was for).
    m.Step(At(t += 500, -1100, 600, 0, true));
    m.Step(At(t += 100, -1100, -900, 0, false));
    CHECK(m.Clicked(t) == 0);
    m.TakeClicked(t);
    // A click long ago is not the next A's.
    m.Step(At(t += 500, -1100, 600, 0, true));
    m.Step(At(t += 100, -1100, 600, 0, false));
    CHECK(m.Clicked(t + MouseRows::kClickMs + 1) == -1);
}

TEST_CASE("pause tab: a press on the page off our rows: its A chooses no row, and with no boxes read the old A stays") {
    MouseRows m;
    std::uint64_t t = 1000;
    m.Step(At(t, 0, 600, 2));
    m.Step(At(t += 16, 0, 600, 2, true));   // (off the column)
    CHECK(m.TakeClicked(t + 50) == MouseRows::kOffRows);
    m.Step(At(t += 16, 0, 600, 2, false));
    // No row's box could be read (the page sliding): a press is nothing of ours, so the A is the focused row's.
    MouseRows::In none = At(t += 500, -1100, 600, 2, false);
    none.boxes = RowBoxes{};
    m.Step(none);
    none.now_ms = t += 16, none.left = true;
    m.Step(none);
    CHECK(m.Clicked(t + 50) == -1);
    // Pressed outside the window: nothing.
    MouseRows::In out;
    out.now_ms = t += 500, out.focused = 2, out.boxes = FiveRows();
    m.Step(out);
    out.now_ms = t += 16, out.left = true;
    m.Step(out);
    CHECK(m.Clicked(t + 50) == -1);
}

TEST_CASE("pause tab: the mouse's steps stop for the player's own, the cursor out of the window, and after 2 s") {
    MouseRows m;
    std::uint64_t t = 1000;
    CHECK(m.Step(At(t, -1100, 600, 3)) == kPadUp);
    MouseRows::In pad = At(t += 16, -1100, 600, 3);
    pad.pad = true;
    m.Step(pad);
    CHECK(m.Step(At(t += 16, -1100, 600, 3)) == 0);
    CHECK(m.Step(At(t += 16, -1100, 600, 3)) == 0);
    MouseRows::In out;
    out.now_ms = t += 16, out.focused = 3;
    out.boxes = FiveRows();
    CHECK(m.Step(out) == 0);
    // Moved onto row 0, and the cursor never gets there: given up.
    CHECK(m.Step(At(t += 16, -1100, 650, 3)) == kPadUp);
    for (int i = 0; i < 200; ++i) m.Step(At(t += 16, -1100, 650, 3));
    CHECK(m.Step(At(t += 16, -1100, 650, 3)) == 0);
}

TEST_CASE("pause tab: our page comes and goes at once, and the menu opening on it shows it at once") {
    Page p = UsablePage();
    std::uint64_t now = 1000;
    OnSystem(p, now);
    p.OnPad(0, now);
    CHECK(Open(p, now + 16) == 0);
    Page::Do d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 32);
    CHECK(d.set_page);
    CHECK(d.page_shown);
    CHECK(d.slide == 0);   // (in System's place: no slide)
    d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 48);
    CHECK_FALSE(d.set_page);   // (once)
    // The menu closed on it, then opened again: on our page, at once.
    p.OnPad(0, now + 64);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), now + 80);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 3), now + 80);
    CHECK_FALSE(p.Shown());
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 0), now + 400);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 0), now + 400);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), now + 3000);
    CHECK(p.Shown());
    CHECK(d.set_page);
    CHECK(d.page_shown);
    CHECK(d.slide == 0);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 1), now + 3000);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 3100);
    p.OnScreen(Strip(0x9000, 5, kSystemPage), now + 3100);
    // L: System back at once, no slide.
    p.OnPad(0, now + 3116);
    CHECK(p.OnPad(kPadL, now + 3132) == kPadL);
    d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 3148);
    CHECK(d.set_page);
    CHECK_FALSE(d.page_shown);
    CHECK(d.slide == 0);
}

TEST_CASE("pause tab: the slide follows the game's measured curve and moves the two pages a page's width") {
    CHECK(SlideProgress(0) == doctest::Approx(0.0f));
    CHECK(SlideProgress(17) == doctest::Approx(0.18f).epsilon(0.02));
    CHECK(SlideProgress(67) == doctest::Approx(0.51f).epsilon(0.02));   // (half by about 65 ms)
    CHECK(SlideProgress(250) > 0.92f);   // (92% by 250 ms)
    CHECK(SlideProgress(kSlideMs) == doctest::Approx(1.0f));
    CHECK(SlideProgress(5000) == 1.0f);
    for (std::uint64_t t = 0; t < kSlideMs; t += 5) CHECK(SlideProgress(t + 5) >= SlideProgress(t));   // (never back)
    auto [ours, system] = SlideOffsets(1, 0.0f);
    CHECK(ours == doctest::Approx(kPageWidth));   // (ours a page to the right, System at its place)
    CHECK(system == doctest::Approx(0.0f));
    std::tie(ours, system) = SlideOffsets(1, 1.0f);
    CHECK(ours == doctest::Approx(0.0f));
    CHECK(system == doctest::Approx(-kPageWidth));
    std::tie(ours, system) = SlideOffsets(-1, 0.0f);   // (L: ours at its place, System a page to the left)
    CHECK(ours == doctest::Approx(0.0f));
    CHECK(system == doctest::Approx(-kPageWidth));
    std::tie(ours, system) = SlideOffsets(-1, 1.0f);
    CHECK(ours == doctest::Approx(kPageWidth));
    CHECK(system == doctest::Approx(0.0f));
}

TEST_CASE("pause tab: System's Customize Link button opens the Multiplayer page on Customize, and goes back to System") {
    Page p = UsablePage();
    std::uint64_t now = 1000;
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
    p.OnScreen(Strip(0x9000, 5, kSystemPage), now);
    p.OnPad(0, now);
    p.SetSystemButton(2);
    CHECK(p.OnPad(kPadA, now + 16) == 0);   // (the game's A kept from it)
    CHECK(p.Shown());
    CHECK(p.Which() == 0);                  // (the Multiplayer page, in System's place)
    CHECK(p.OpenedFrom() == 2);
    CHECK(p.TakeCustomizeOpen());
    CHECK_FALSE(p.TakeCustomizeOpen());
    CHECK(p.TakeOpened());
    p.SetSystemButton(-1);
    p.OnPad(0, now + 32);
    p.LeaveToButton("B on Customize Link");
    CHECK_FALSE(p.Shown());
    CHECK(p.TakeLeft());
    CHECK(p.OpenedFrom() == 2);             // (the cursor back on its button)
    // The Multiplayer button's own A: no Customize page.
    p.OnPad(0, now + 48);
    p.SetSystemButton(0);
    p.OnPad(kPadA, now + 64);
    CHECK(p.Shown());
    CHECK(p.OpenedFrom() == 0);
    CHECK_FALSE(p.TakeCustomizeOpen());
}

TEST_CASE("pause tab: Longshot Settings opens from System's button with A, in System's place, B back, L or R the game's") {
    Page p = UsablePage();
    p.SetSettingsUsable(true);
    std::uint64_t now = 1000;
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
    p.OnScreen(Strip(0x9000, 5, kSystemPage), now);
    p.OnPad(0, now);
    // A on another of System's buttons is the game's.
    p.SetSystemButton(-1);
    CHECK(p.OnPad(kPadA, now + 16) == kPadA);
    CHECK_FALSE(p.Shown());
    p.OnPad(0, now + 32);
    // A on the Longshot Settings button: the page at once, the game's A kept from it.
    p.SetSystemButton(1);
    CHECK(p.OnPad(kPadA, now + 48) == 0);
    CHECK(p.TakeOpened());
    CHECK_FALSE(p.TakeOpened());
    Page::Do d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 64);
    CHECK(d.set_page);
    CHECK(d.page_from == 0);
    CHECK(d.page_to == 2);
    CHECK(d.slide == 0);
    CHECK(p.Which() == 1);
    CHECK(p.MousePage() == -1);   // (in System's place: the mouse's tabs stay the game's)
    CHECK_FALSE(p.OnScreen(Strip(0x9000, 5, kSystemPage), now + 80).any());   // (System's tab stays lit)
    p.SetSystemButton(-1);
    // B in its list goes back to its rows; B on its rows goes back to System, its cursor on the button again.
    p.OnPad(0, now + 96);
    p.SetSubView(true);
    p.OnPad(kPadB, now + 112);
    CHECK(p.TakeBack());
    CHECK(p.Shown());
    CHECK_FALSE(p.TakeLeft());
    p.SetSubView(false);
    p.OnPad(0, now + 128);
    CHECK(p.OnPad(kPadB, now + 144) == 0);   // (the game's B would close the menu)
    CHECK_FALSE(p.Shown());
    CHECK(p.TakeLeft());
    CHECK(p.Which() == 1);
    d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 160);
    CHECK(d.set_page);
    CHECK(d.page_from == 2);
    CHECK(d.page_to == 0);
    CHECK(d.slide == 0);
    // R on Longshot Settings: System back at once, and the R is the game's.
    p.OnPad(0, now + 176);
    p.SetSystemButton(1);
    p.OnPad(kPadA, now + 192);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 208);
    p.SetSystemButton(-1);
    p.OnPad(0, now + 224);
    CHECK(p.OnPad(kPadR, now + 240) == kPadR);
    CHECK_FALSE(p.Shown());
    CHECK(p.TakeWhy() == "L or R on Longshot Settings");
    d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 256);
    CHECK(d.set_page);
    CHECK(d.page_from == 2);
    CHECK(d.page_to == 0);
    CHECK(d.slide == 0);
    CHECK_FALSE(p.OnScreen(Strip(0x9000, 5, kSystemPage), now + 272).any());   // (the strip's: nothing of ours)
    // L on Longshot Settings: System back at once, and the L is the game's (on to Items).
    p.OnPad(0, now + 336);
    p.SetSystemButton(1);
    p.OnPad(kPadA, now + 352);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 368);
    p.SetSystemButton(-1);
    p.OnPad(0, now + 384);
    CHECK(p.OnPad(kPadL, now + 400) == kPadL);
    CHECK_FALSE(p.Shown());
    d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 416);
    CHECK(d.page_from == 2);
    CHECK(d.page_to == 0);
    CHECK(d.slide == 0);
}

TEST_CASE("pause tab: without Longshot Settings (an older pack, or no settings from Longshot), A on its place is the game's") {
    Page p = UsablePage();
    std::uint64_t now = 1000;
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
    p.OnScreen(Strip(0x9000, 5, kSystemPage), now);
    p.OnPad(0, now);
    p.SetSystemButton(1);
    CHECK(p.OnPad(kPadA, now + 16) == 0);
    CHECK_FALSE(p.Shown());
    CHECK_FALSE(p.TakeOpened());
    p.SetSystemButton(-1);
    // The Multiplayer button still opens its page.
    p.OnPad(0, now + 32);
    CHECK(Open(p, now + 48) == 0);
    CHECK(p.Shown());
    CHECK(p.TakeOpened());
    const Page::Do d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 64);
    CHECK(d.set_page);
    CHECK(d.page_to == 1);
    CHECK(p.Which() == 0);
}

TEST_CASE("pause tab: B on the Multiplayer page's rows gives System back, the game's cursor back on its button") {
    Page p = UsablePage();
    std::uint64_t now = 1000;
    OnSystem(p, now);
    REQUIRE(Open(p, now) == 0);
    CHECK(p.TakeOpened());
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 16);
    p.OnPad(0, now + 32);
    CHECK_FALSE(p.TakeLeft());
    CHECK(p.OnPad(kPadB, now + 48) == 0);   // (ours: the game's B would close the menu)
    CHECK_FALSE(p.Shown());
    CHECK(p.TakeLeft());
    CHECK_FALSE(p.TakeLeft());   // (once)
    CHECK(p.Which() == 0);
    CHECK(p.TakeWhy() == "B on the Multiplayer page");
    const Page::Do d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 64);
    CHECK(d.set_page);
    CHECK_FALSE(d.page_shown);
    CHECK(d.page_from == 1);
    CHECK(d.page_to == 0);
    CHECK(d.slide == 0);
}

TEST_CASE("pause tab: the Settings page's list: a category's settings, its pages, and the values a press asks for") {
    std::vector<SettingRow> all;
    for (int i = 0; i < 9; ++i) {
        SettingRow r;
        r.key = "g" + std::to_string(i), r.name = "Graphics " + std::to_string(i), r.text = "On", r.tab = "Graphics";
        r.lo = 0, r.hi = 1, r.value = 1;
        all.push_back(r);
    }
    SettingRow vol;
    vol.key = "volume", vol.name = "Volume", vol.text = "80", vol.tab = "audio", vol.lo = 0, vol.hi = 100, vol.step = 5;   // (Longshot's: lower case)
    vol.value = 80;
    all.push_back(vol);
    SettingRow dbg;
    dbg.key = "debug", dbg.name = "Debug thing", dbg.tab = "Debug";
    all.push_back(dbg);
    SettingRow res;
    res.key = "aspect", res.name = "Aspect ratio", res.text = "16:9", res.tab = "Graphics", res.restart = true;
    res.lo = 0, res.hi = 3, res.value = 3;
    all.push_back(res);

    const auto graphics = SettingsOf(all, 0);
    CHECK(graphics.size() == 10);
    CHECK(SettingsOf(all, 1).size() == 1);
    CHECK(SettingsOf(all, 4).empty());   // (Motion: none here)
    for (const SettingRow& r : graphics) CHECK(r.tab != "Debug");
    REQUIRE(SettingsOf(all, 5).size() == 1);   // (Debug: Longshot's Debug tab's settings)
    CHECK(SettingsOf(all, 5)[0].key == "debug");
    CHECK(SettingsOf(all, kSettingRows).empty());

    // No setting of Longshot's is left out: each of the tabs its control bridge names (gpu/vk/vk_menu.cpp bridge_fill,
    // v3.0.0 and rc.8: its strip's tabs, the Motion page, the FPS counter's Dev lines as gameplay, the Controls page's
    // rows as controls) lands in exactly one category.
    for (const char* tab : {"graphics", "audio", "gameplay", "controls", "nfc", "debug", "motion"}) {
        SettingRow r;
        r.key = std::string("k_") + tab, r.tab = tab;
        int in = 0;
        for (int c = 0; c < kSettingRows; ++c) in += static_cast<int>(SettingsOf({r}, c).size());
        CHECK_MESSAGE(in == 1, tab);
    }
    SettingRow nfc;
    nfc.key = "nfc_tag", nfc.name = "NFC tag", nfc.tab = "nfc";
    CHECK(SettingsOf({nfc}, 3).size() == 1);   // (NFC tag: with Controls)
    CHECK(SettingPages(6) == 1);
    CHECK(SettingPages(10) == 2);
    CHECK(SettingPages(13) == 3);
    auto lines = SettingLines(graphics, 0, "Graphics");
    REQUIRE(lines.size() == static_cast<std::size_t>(kSettingListRows));
    CHECK(lines[kSettingListRows - 1].name.empty());   // (six a page: the eighth row unused)
    CHECK(lines[0].name == "Graphics  < 1 of 2 >");   // (the header: the category, and its page when it has more)
    CHECK(lines[0].value.empty());                     // (no value: the module hides its box)
    CHECK(lines[1].name == "Graphics 0");
    CHECK(lines[1].value == "On");
    lines = SettingLines(graphics, 1, "Graphics");
    CHECK(lines[0].name == "Graphics  < 2 of 2 >");
    CHECK(lines[1].name == "Graphics 6");
    CHECK(lines[4].name == "Aspect ratio *");   // (a restart one)
    CHECK(lines[5].name.empty());
    CHECK(SettingLines(SettingsOf(all, 1), 0, "Audio")[0].name == "Audio");   // (one page: no page shown)

    CHECK(SettingNext(vol, 1) == 85);
    CHECK(SettingNext(vol, -1) == 75);
    vol.value = 100;
    CHECK(SettingNext(vol, 1) == 100);   // (the end of its range)
    CHECK(SettingNext(vol, 0) == 100);   // (A on a slider: nothing, never back to 0)
    CHECK(SettingNext(res, 0) == 0);     // (A on a few choices: the first after the last)
    SettingRow onoff = graphics[0];
    CHECK(SettingNext(onoff, 0) == 0);
    onoff.value = 0;
    CHECK(SettingNext(onoff, 0) == 1);
    onoff.fixed = true;
    CHECK(SettingNext(onoff, 1) == 0);   // (nothing to choose on this PC)

    int page = 0, row = 0;
    SettingMove(10, 1, &page, &row);
    CHECK((page == 0 && row == 1));
    page = 0, row = kSettingsPerPage - 1;
    SettingMove(10, 1, &page, &row);     // (down past the page's last: the next page)
    CHECK((page == 1 && row == 0));
    SettingMove(10, -1, &page, &row);
    CHECK((page == 0 && row == kSettingsPerPage - 1));
    page = 1, row = 3;
    SettingMove(10, 1, &page, &row);     // (the last setting: stays)
    CHECK((page == 1 && row == 3));
    page = 0, row = 0;
    SettingMove(10, -1, &page, &row);
    CHECK((page == 0 && row == 0));

    // Seven a page with a pack whose page has its eighth list row (Pa_List_07).
    CHECK(SettingPages(7, 7) == 1);
    CHECK(SettingPages(10, 7) == 2);
    lines = SettingLines(graphics, 0, "Graphics", 7);
    REQUIRE(lines.size() == static_cast<std::size_t>(kSettingListRows));
    CHECK(lines[7].name == "Graphics 6");   // (the seventh setting, on the eighth row)
    lines = SettingLines(graphics, 1, "Graphics", 7);
    CHECK(lines[1].name == "Graphics 7");
    page = 0, row = 6;
    SettingMove(10, 1, &page, &row, 7);
    CHECK((page == 1 && row == 0));
}

TEST_CASE("pause tab: the port and relay fields take what each can hold") {
    TextField port(TextField::Kind::kPort);
    for (char c : std::string("27a5.10")) port.Type(static_cast<unsigned char>(c));
    CHECK(port.text() == "27510");   // (digits only)
    CHECK(port.PortValue() == 27510);
    port.Type('9');                  // (five digits at most)
    CHECK(port.text() == "27510");
    port.Set("70000");
    CHECK(port.PortValue() == 0);    // (past 65535: no port)
    port.Set("");
    CHECK(port.PortValue() == 0);
    TextField relay(TextField::Kind::kAddress);
    for (char c : std::string("Relay.Example.org:27530 x")) relay.Type(static_cast<unsigned char>(c));
    CHECK(relay.text() == "relay.example.org:27530x");   // (lower case, no spaces)
    CHECK(relay.Shown(false, 40) == "relay.example.org:27530x");   // (shown as it is, not as a password)
}

TEST_CASE("pause tab: the online list's search keeps the rooms with its letters in their name or their host's") {
    CHECK(RoomMatches("kokiri", "Saria", ""));
    CHECK(RoomMatches("kokiri", "Saria", "kok"));
    CHECK(RoomMatches("Kokiri", "Saria", "kok"));      // (any case)
    CHECK(RoomMatches("forest", "Saria", "sar"));      // (the host's name too)
    CHECK_FALSE(RoomMatches("forest", "Saria", "fire"));
}

TEST_CASE("pause tab: Settings and look shows co-op's own menu's pages on the list") {
    using CoopPauseMenu::Row;
    using CoopPauseMenu::RowKind;
    // The view: four of co-op's menu's pages and Back; each row's A shows its page on the list.
    SessionView v;
    CHECK(RowTexts(View::kMenu, v) ==
          std::vector<std::string>{"Customize Link", "Co-op settings", "Commands", "Local co-op", "Back"});
    for (int r = 0; r < kMenuTabs; ++r) CHECK(ActionFor(View::kMenu, r, v.s) == RowAction::kMenuTab);
    CHECK(ActionFor(View::kMenu, kRows - 1, v.s) == RowAction::kBack);
    CHECK(BackOf(View::kMenu) == View::kMain);
    CHECK(MenuTabPage(0) == CoopPauseMenu::Page::kCustomize);
    CHECK(MenuTabPage(1) == CoopPauseMenu::Page::kSettings);
    CHECK(MenuTabPage(2) == CoopPauseMenu::Page::kCommands);
    CHECK(MenuTabPage(3) == CoopPauseMenu::Page::kLocal);
    CHECK(std::string(ActionWords(RowAction::kViewMenu)).size() > 3);
    CHECK(std::string(ActionWords(RowAction::kMenuTab)).size() > 3);

    // A row's value box: a choice between its arrows (only the ways it can go), a switch, an action's value.
    Row choice{"Kokiri Tunic", "Red", RowKind::kChoice};
    choice.left = choice.right = true;
    CHECK(MenuValue(choice, false) == "< Red >");
    choice.left = false;
    CHECK(MenuValue(choice, false) == "Red >");
    Row toggle{"Voice", "", RowKind::kToggle};
    toggle.on = true;
    CHECK(MenuValue(toggle, false) == "On");
    toggle.on = false;
    CHECK(MenuValue(toggle, false) == "Off");
    CHECK(MenuValue(Row{"Your name", "Link", RowKind::kAction}, false) == "Link");
    CHECK(MenuValue(Row{"Your name", "Lin", RowKind::kAction}, true) == "Lin_");   // (typed into)
    CHECK(MenuValue(Row{"Tunic", "", RowKind::kHeading}, false).empty());

    // The list: the header (the page, and which part of its rows with more than the list holds), then its rows.
    CoopPauseMenu::View m;
    m.rows.push_back(Row{"Tunic", "", RowKind::kHeading});
    for (int i = 0; i < 12; ++i) {
        Row r{"Part " + std::to_string(i), "Game's own", RowKind::kChoice};
        r.right = true;
        m.rows.push_back(r);
    }
    m.selected = 1;
    MenuList ml = MenuLines(m, 0, 0, false);
    REQUIRE(ml.lines.size() == static_cast<std::size_t>(kMenuMost) + 1);   // (the eighth line empty: six a page)
    CHECK(ml.lines[kMenuMost].name.empty());
    CHECK(ml.lines[0].name == "Customize Link  < 1 of 3 >");
    CHECK(ml.lines[1].name == "Tunic");
    CHECK(ml.rows[0] == -1);
    CHECK(ml.rows[1] == 0);
    CHECK_FALSE(ml.selectable[1]);   // (a heading)
    CHECK(ml.lines[2].name == "Part 0");
    CHECK(ml.lines[2].value == "Game's own >");
    CHECK(ml.selectable[2]);
    CHECK(ml.picked == -1);   // (the rows have the pad: nothing picked)
    // The list has the pad: the selected row is picked, and the list scrolls to keep it in view.
    ml = MenuLines(m, 0, 0, true);
    CHECK(ml.picked == 1);   // (the second line: the heading first)
    m.selected = 10;
    ml = MenuLines(m, 0, 0, true);
    CHECK(ml.top == 10 - kCells + 1);
    CHECK(ml.rows[static_cast<std::size_t>(ml.picked) + 1] == 10);
    CHECK(ml.lines[0].name == "Customize Link  < 2 of 3 >");
    m.selected = 12;   // (the last: the last page)
    ml = MenuLines(m, 0, 0, true);
    CHECK(ml.lines[0].name == "Customize Link  < 3 of 3 >");
    CHECK(ml.rows[static_cast<std::size_t>(ml.picked) + 1] == 12);
    // The rows have the pad: `top` as the mouse's header click left it, kept within the rows.
    ml = MenuLines(m, 1, 100, false);
    CHECK(ml.top == 13 - kCells);
    CHECK(ml.lines[0].name.rfind("Co-op settings", 0) == 0);
    // A short page: no "of", every row, the lines past it empty.
    CoopPauseMenu::View small;
    small.rows = {Row{"Open the chat", "T", RowKind::kAction}, Row{"/teleport name", "", RowKind::kNote}};
    small.selected = 0;
    ml = MenuLines(small, 2, 0, true);
    CHECK(ml.lines[0].name == "Commands");
    CHECK(ml.lines[2].name == "/teleport name");
    CHECK(ml.lines[2].value.empty());
    CHECK_FALSE(ml.selectable[2]);   // (a note)
    CHECK(ml.lines[3].name.empty());
    CHECK(ml.rows[3] == -1);

    // Seven a page (the pack's eighth list row): the eighth line holds a row, the header counts pages of seven.
    m.selected = 1;
    ml = MenuLines(m, 0, 0, true, kMenuMost);
    CHECK(ml.lines[0].name == "Customize Link  < 1 of 2 >");
    CHECK(ml.rows[kMenuMost] == kMenuMost - 1);
    CHECK_FALSE(ml.lines[kMenuMost].name.empty());
    m.selected = 12;
    ml = MenuLines(m, 0, 0, true, kMenuMost);
    CHECK(ml.top == 13 - kMenuMost);
    CHECK(ml.lines[0].name == "Customize Link  < 2 of 2 >");

    // A choice cut with its arrows shows its value alone ("< By player n..." was cut on the page).
    Row look{"Link's look", "By player number", RowKind::kChoice};
    look.left = look.right = true;
    CHECK(MenuValue(look, false) == "By player number");

    // The heading a row is under (Link turns his back for a shield's part).
    CHECK(MenuHeadingOf(m, 5) == "Tunic");
    CoopPauseMenu::View parts;
    parts.rows = {Row{"Tunic", "", RowKind::kHeading}, Row{"Kokiri Tunic", "", RowKind::kChoice},
                  Row{"Hylian Shield", "", RowKind::kHeading}, Row{"Frame", "", RowKind::kChoice}};
    CHECK(MenuHeadingOf(parts, 1) == "Tunic");
    CHECK(MenuHeadingOf(parts, 3) == "Hylian Shield");
    CHECK(MenuHeadingOf(parts, -1).empty());
    CHECK(MenuHeadingOf(CoopPauseMenu::View{}, 0).empty());
}

TEST_CASE("census (the - menu research): screens first seen, state changes, gone, and the menu's categories") {
    CoopPauseTab::Census c;
    CoopPauseTab::Census::Menu plus{4, 1, 0};
    auto lines = c.Seen(0x4bebaa8, 1, plus, 0);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "census: menu plus=4 minus=1 minus_on=0");
    CHECK(lines[1] == "census: screen 0x4bebaa8 seen, state 1");
    CHECK(c.Seen(0x4bebaa8, 1, plus, 16).empty());              // the same again: nothing
    CHECK(c.Seen(0x4be8aa0, 1, plus, 20) == std::vector<std::string>{"census: screen 0x4be8aa0 seen, state 1"});
    CHECK(c.Seen(0x4bebaa8, 2, plus, 30) == std::vector<std::string>{"census: screen 0x4bebaa8 state 1 -> 2"});
    CoopPauseTab::Census::Menu minus{4, 2, 1};
    CHECK(c.Seen(0x4be8aa0, 1, minus, 40) == std::vector<std::string>{"census: menu plus=4 minus=2 minus_on=1"});
    // The pages screen stops updating (the - menu): said gone once, at the next update after kCensusGoneMs.
    lines = c.Seen(0x4be8aa0, 1, minus, 40 + CoopPauseTab::Census::kCensusGoneMs);
    CHECK(lines == std::vector<std::string>{"census: screen 0x4bebaa8 gone"});
    CHECK(c.Seen(0x4be8aa0, 1, minus, 50 + CoopPauseTab::Census::kCensusGoneMs).empty());
    CHECK(c.Seen(0x4bebaa8, 1, minus, 60 + CoopPauseTab::Census::kCensusGoneMs) ==
          std::vector<std::string>{"census: screen 0x4bebaa8 seen, state 1"});   // back: seen anew
}

TEST_CASE("pause tab (row 199): the menu is up while the tab strip is open or opening and fresh") {
    CHECK(MenuUpFrom(2, 1000, 1000));
    CHECK(MenuUpFrom(1, 1000, 1000 + kMenuUpMs - 1));       // (opening; a frame or a few later)
    CHECK_FALSE(MenuUpFrom(2, 1000, 1000 + kMenuUpMs));     // (no update since: closed, or a load)
    CHECK_FALSE(MenuUpFrom(3, 1000, 1000));                 // (closing: the marks come back with the game's HUD)
    CHECK_FALSE(MenuUpFrom(0, 1000, 1000));
    CHECK_FALSE(MenuUpFrom(2, 0, 50));                      // (never seen)
    CHECK_FALSE(MenuUpFrom(2, 2000, 1000));                 // (a clock that went back)
}

TEST_CASE("pause tab (0.11.9, lane 062917): Character Select's Customize screen is taken as open only in a menu that is up") {
    // A real opening: his state reads open at 1000 with the strip's update of the frame before (990, open); taken once
    // the strip has updated since (1006), and kept while it stays up.
    CHECK(SnailEdgeFrom(false, 1000, 2, 990, 1000) == SnailEdge::kWait);
    CHECK(SnailEdgeFrom(false, 1000, 2, 1006, 1008) == SnailEdge::kOpen);
    CHECK(SnailEdgeFrom(true, 1000, 2, 1500, 1510) == SnailEdge::kOpen);
    // The - menu closing (the lane's 'opened' that kept his rows' keys a second after the menu left): his state reads
    // open again, the strip's last update still open (the frame before), then closing (3): never taken.
    CHECK(SnailEdgeFrom(false, 2000, 2, 1990, 2000) == SnailEdge::kWait);
    CHECK(SnailEdgeFrom(false, 2000, 3, 2006, 2008) == SnailEdge::kWait);
    CHECK(SnailEdgeFrom(false, 2000, 0, 2400, 2900) == SnailEdge::kWait);   // (closed, then no strip update at all)
    // Open while the menu closes: closed at once.
    CHECK(SnailEdgeFrom(true, 1000, 3, 3006, 3008) == SnailEdge::kClose);
    CHECK(SnailEdgeFrom(true, 1000, 2, 3000, 3000 + kMenuUpMs) == SnailEdge::kClose);   // (no strip update since)
}

TEST_CASE("pause tab: the fourth tab's place and fades taken one update ahead, not past where they head") {
    CHECK(Ahead(1023.83f, 1020.12f, 1000.0f) == doctest::Approx(1016.41f));   // (the strip's drop, a run's numbers)
    CHECK(Ahead(1000.48f, 1000.34f, 1000.0f) == doctest::Approx(1000.20f));
    CHECK(Ahead(1000.19f, 1000.0f, 1000.0f) == 1000.0f);                      // (at rest: stays)
    CHECK(Ahead(1001.0f, 1000.2f, 1000.0f) == 1000.0f);                       // (not past its rest)
    CHECK(Ahead(0.5f, 0.844f, 1.0f) == 1.0f);                                 // (a fade in: not past 1)
    CHECK(Ahead(0.156f, 0.5f, 1.0f) == doctest::Approx(0.844f));
    CHECK(Ahead(0.8f, 0.5f, 0.0f) == doctest::Approx(0.2f));                  // (a fade out)
    CHECK(Ahead(0.4f, 0.1f, 0.0f) == 0.0f);
    CHECK(Ahead(1.0f, 1.0f, 1.0f) == 1.0f);
}

TEST_CASE("pause tab (0.11.9): the - menu's tabs: R on Map onto Multiplayer, R and L among ours, L back to Map") {
    Page p = UsablePage();
    std::uint64_t now = 1000;
    // Without the pack's tabs: the - menu's R and L stay the game's.
    p.OnScreen(Strip(0x9000, 8, kMapCategory, kMinusMode), now);
    p.OnPad(0, now);
    CHECK(p.OnPad(kPadR, now + 10) == kPadR);
    CHECK(p.MinusTab() == -1);
    p.OnPad(0, now + 12);

    p.SetMinusTabsUsable(true);
    Page::Do d = p.OnScreen(Strip(0x9000, 8, kMapCategory, kMinusMode), now + 20);
    CHECK(d.set_minus);
    CHECK(d.minus_shown);
    CHECK(d.fill_minus);
    CHECK(d.minus_lit == -1);
    // R on Map: our first tab, sliding in, the game's tab sound; never the game's R.
    CHECK(p.OnPad(kPadR, now + 30) == 0);
    CHECK(p.MinusTab() == 0);
    CHECK(std::string(kMinusTabNames[p.MinusTab()]) == "Multiplayer");
    CHECK(std::string(p.TakeStripSound()) == "mc_TabToRight");
    d = p.OnScreen(Strip(0x9000, 8, kMapCategory, kMinusMode), now + 40);
    CHECK(d.set_minus);
    CHECK(d.minus_lit == 0);
    CHECK(d.minus_slide == 1);
    CHECK_FALSE(d.fill_minus);
    // Its page has the pad: A and the d-pad are ours, B / + / - the game's.
    p.OnPad(0, now + 42);
    CHECK(p.OnPad(kPadA, now + 44) == 0);
    p.OnPad(0, now + 46);
    CHECK((p.OnPad(kPadB, now + 48) & kPadB) != 0);
    p.OnPad(0, now + 50);
    // R to Local co-op and Mods, then nothing past the last.
    p.OnPad(kPadR, now + 52), p.OnPad(0, now + 54);
    CHECK(p.MinusTab() == 1);
    p.OnPad(kPadR, now + 56), p.OnPad(0, now + 58);
    CHECK(p.MinusTab() == 2);
    p.TakeStripSound();
    CHECK(p.OnPad(kPadR, now + 60) == 0);
    CHECK(p.MinusTab() == 2);
    CHECK(p.TakeStripSound() == nullptr);
    p.OnPad(0, now + 62);
    // L back through ours to Map.
    for (int want : {1, 0, -1}) {
        CHECK(p.OnPad(kPadL, now + 64) == 0);
        CHECK(p.MinusTab() == want);
        CHECK(std::string(p.TakeStripSound()) == "mc_TabToLeft");
        p.OnPad(0, now + 66);
    }
    d = p.OnScreen(Strip(0x9000, 8, kMapCategory, kMinusMode), now + 70);
    CHECK(d.minus_lit == -1);
    CHECK(d.minus_slide == -1);
    // R on another - page (Key Items): the game's.
    p.OnScreen(Strip(0x9000, 8, 6, kMinusMode), now + 80);
    CHECK(p.OnPad(kPadR, now + 82) == kPadR);
    CHECK(p.MinusTab() == -1);
    p.OnPad(0, now + 84);
    // The menu closing (or the + menu) leaves ours, and the tabs go.
    p.OnScreen(Strip(0x9000, 8, kMapCategory, kMinusMode), now + 90);
    p.OnPad(kPadR, now + 92), p.OnPad(0, now + 94);
    REQUIRE(p.MinusTab() == 0);
    d = p.OnScreen(Strip(0x9000, 8, kMapCategory, kMinusMode, 3), now + 100);
    CHECK(p.MinusTab() == -1);
    CHECK(d.set_minus);
    CHECK_FALSE(d.minus_shown);
    // In the + menu nothing of ours there.
    d = p.OnScreen(Strip(0x9000, 8, kSystemPage), now + 120);
    CHECK_FALSE(d.minus_shown);
}

TEST_CASE("pause tab (0.11.9): the - menu's own page: up and down among its rows shown, A picks one, then the + menu") {
    Page p = UsablePage();
    p.SetMinusTabsUsable(true);
    std::uint64_t now = 1000;
    p.OnScreen(Strip(0x9000, 8, kMapCategory, kMinusMode), now);
    p.OnPad(kPadR, now + 10), p.OnPad(0, now + 12);
    REQUIRE(p.MinusTab() == 0);
    p.TakeStripSound();
    // No rows yet (the module has not set them): A and the d-pad do nothing, and stay kept from the game.
    CHECK(p.OnPad(kPadDown, now + 14) == 0);
    p.OnPad(0, now + 16);
    CHECK(p.OnPad(kPadA, now + 18) == 0);
    CHECK(p.TakeMinusDecide() == -1);
    p.OnPad(0, now + 20);
    // Rows 0, 1 and 3 (2 empty): down skips the empty one, stops at the last, up stops at the first; the game's sound.
    p.SetMinusRows(0b1011);
    CHECK(p.MinusRow() == 0);
    p.OnPad(kPadDown, now + 22), p.OnPad(0, now + 24);
    CHECK(p.MinusRow() == 1);
    CHECK(std::string(p.TakeStripSound()) == "button_on");
    p.OnPad(kPadStickDown, now + 26), p.OnPad(0, now + 28);
    CHECK(p.MinusRow() == 3);
    p.OnPad(kPadDown, now + 30), p.OnPad(0, now + 32);
    CHECK(p.MinusRow() == 3);
    CHECK(p.TakeStripSound() != nullptr);   // (the move to 3's)
    CHECK(p.TakeStripSound() == nullptr);   // (none at the end)
    // The row under the cursor gone (out of a game): the next shown one around.
    p.SetMinusRows(0b0011);
    CHECK(p.MinusRow() == 0);
    p.OnPad(kPadDown, now + 34), p.OnPad(0, now + 36);
    p.OnPad(kPadUp, now + 38), p.OnPad(0, now + 40);
    CHECK(p.MinusRow() == 0);
    p.OnPad(kPadDown, now + 42), p.OnPad(0, now + 44);
    // A: the row, once, with the game's decide sound; never the game's A.
    CHECK(p.OnPad(kPadA, now + 46) == 0);
    CHECK(p.TakeMinusDecide() == 1);
    CHECK(p.TakeMinusDecide() == -1);
    CHECK(std::string(p.TakeStripSound()) == "button_decide");
    p.OnPad(0, now + 48);
    // Another tab: its first row, its rows set again.
    p.OnPad(kPadR, now + 50), p.OnPad(0, now + 52);
    CHECK(p.MinusTab() == 1);
    CHECK(p.MinusRow() == 0);
    p.OnPad(kPadDown, now + 54), p.OnPad(0, now + 56);
    CHECK(p.MinusRow() == 0);   // (no rows set for it yet)
    // (0.11.9, one home: a row's view shows on this same page; the game never gets a - or + from us)
    p.SetMinusRows(0b111);
    CHECK(p.OnPad(kPadA, now + 58) == 0);
    CHECK(p.TakeMinusDecide() == 0);
    p.OnPad(0, now + 60);
    for (std::uint64_t t = now + 62; t < now + 600; t += 16) CHECK((p.OnPad(0, t) & (kPadMinus | kPadPlus)) == 0);
    CHECK(p.MinusTab() == 1);
}

// The - page's views and its list on the right (the module's rows and lines set as MinusWords would).
Page MinusPageOn(std::uint64_t now) {
    Page p = UsablePage();
    p.SetMinusTabsUsable(true);
    p.OnScreen(Strip(0x9000, 8, kMapCategory, kMinusMode), now);
    p.OnPad(kPadR, now + 2), p.OnPad(0, now + 4);
    p.TakeStripSound();
    p.MinusTabChanged();
    return p;
}

TEST_CASE("pause tab (0.11.9): a view on the - page: B steps back a view, on the tab's own rows B is the game's") {
    std::uint64_t now = 1000;
    Page p = MinusPageOn(now);
    REQUIRE(p.MinusTab() == 0);
    p.SetMinusRows(0b11111);
    // The tab's own rows: B is the game's (the menu closes).
    CHECK(p.OnPad(kPadB, now + 10) == kPadB);
    CHECK_FALSE(p.TakeMinusBack());
    p.OnPad(0, now + 12);
    // A view shows (the module's): B is ours, once, with the game's cancel sound, kept from the game until let go.
    p.SetMinusInView(true);
    CHECK(p.OnPad(kPadB, now + 14) == 0);
    CHECK(p.TakeMinusBack());
    CHECK_FALSE(p.TakeMinusBack());
    CHECK(std::string(p.TakeStripSound()) == "button_cancel");
    CHECK(p.OnPad(kPadB, now + 16) == 0);   // (held: still ours)
    p.OnPad(0, now + 18);
    // The view's rows take A as the tab's did (the module acts on the view's row).
    p.OnPad(kPadDown, now + 20), p.OnPad(0, now + 22);
    CHECK(p.OnPad(kPadA, now + 24) == 0);
    CHECK(p.TakeMinusDecide() == 1);
    p.OnPad(0, now + 26);
    // R: the next tab, the view gone with it (MinusTabChanged: the module's view back to the tab's rows).
    p.OnPad(kPadR, now + 28), p.OnPad(0, now + 30);
    CHECK(p.MinusTab() == 1);
    CHECK(p.MinusTabChanged());
    CHECK_FALSE(p.MinusTabChanged());
    CHECK(p.OnPad(kPadB, now + 32) == kPadB);   // (no view on the new tab: the game's B)
    CHECK_FALSE(p.TakeMinusBack());
    p.OnPad(0, now + 34);
    // The - menu closes: the tab and its view with it.
    p.SetMinusInView(true);
    p.OnScreen(Strip(0x9000, 8, kMapCategory, kMinusMode, 3), now + 40);
    CHECK(p.MinusTab() == -1);
    CHECK(p.MinusTabChanged());
}

TEST_CASE("pause tab (0.11.9): the - page's list: Right into it, up and down among the lines picked, A, Left or B back") {
    std::uint64_t now = 1000;
    Page p = MinusPageOn(now);
    p.SetMinusRows(0b111);
    p.SetMinusInView(true);
    // No line to pick: Right does nothing.
    p.OnPad(kPadRight, now + 10), p.OnPad(0, now + 12);
    CHECK_FALSE(p.MinusInList());
    // Lines 1, 2 and 4 can be picked (0 the header, 3 a note): Right goes onto the first.
    p.SetMinusLines(0b10110, false);
    CHECK(p.OnPad(kPadRight, now + 14) == 0);
    p.OnPad(0, now + 16);
    CHECK(p.MinusInList());
    CHECK(p.MinusLine() == 1);
    CHECK(std::string(p.TakeStripSound()) == "button_on");
    // Down skips the note, stops at the last; up stops at the first; the rows' cursor stays.
    p.OnPad(kPadDown, now + 18), p.OnPad(0, now + 20);
    CHECK(p.MinusLine() == 2);
    p.OnPad(kPadDown, now + 22), p.OnPad(0, now + 24);
    CHECK(p.MinusLine() == 4);
    p.OnPad(kPadDown, now + 26), p.OnPad(0, now + 28);
    CHECK(p.MinusLine() == 4);
    CHECK(p.MinusRow() == 0);
    // A: that line, once; never a row's.
    CHECK(p.OnPad(kPadA, now + 30) == 0);
    CHECK(p.TakeMinusLineDecide() == 4);
    CHECK(p.TakeMinusLineDecide() == -1);
    CHECK(p.TakeMinusDecide() == -1);
    p.OnPad(0, now + 32);
    // The line picked gone (a game left the list): the nearest before it.
    p.SetMinusLines(0b00110, false);
    CHECK(p.MinusLine() == 2);
    // B: back to the rows, not a view back and not the game's.
    CHECK(p.OnPad(kPadB, now + 34) == 0);
    CHECK_FALSE(p.MinusInList());
    CHECK_FALSE(p.TakeMinusBack());
    p.OnPad(0, now + 36);
    // Left too.
    p.OnPad(kPadRight, now + 38), p.OnPad(0, now + 40);
    REQUIRE(p.MinusInList());
    p.OnPad(kPadLeft, now + 42), p.OnPad(0, now + 44);
    CHECK_FALSE(p.MinusInList());
    // Every line gone: the list gives the pad back.
    p.OnPad(kPadRight, now + 46), p.OnPad(0, now + 48);
    REQUIRE(p.MinusInList());
    p.SetMinusLines(0, false);
    CHECK_FALSE(p.MinusInList());
}

TEST_CASE("pause tab (0.11.9): co-op's menu's page on the - page's list: the pad's moves are its own events") {
    std::uint64_t now = 1000;
    Page p = MinusPageOn(now);
    p.SetMinusRows(0b1111);
    p.SetMinusInView(true);
    p.SetMinusLines(0b110, true);   // (Co-op settings: its rows on the list, its own selection)
    p.OnPad(kPadRight, now + 10), p.OnPad(0, now + 12);
    REQUIRE(p.MinusInList());
    p.TakeMinusMoves();
    // Up, down, left, right and A: that menu's own, each once; nothing to the game.
    CHECK(p.OnPad(kPadDown, now + 14) == 0);
    p.OnPad(0, now + 16);
    CHECK(p.OnPad(kPadRight, now + 18) == 0);
    p.OnPad(0, now + 20);
    CHECK(p.OnPad(kPadLeft, now + 22) == 0);
    p.OnPad(0, now + 24);
    p.OnPad(kPadA, now + 26), p.OnPad(0, now + 28);
    CHECK(p.TakeMinusMoves() == (Page::kMinusMoveDown | Page::kMinusMoveRight | Page::kMinusMoveLeft | Page::kMinusMoveA));
    CHECK(p.TakeMinusMoves() == 0);
    CHECK(p.MinusInList());   // (Left is the menu's: a choice changed, not back to the rows)
    CHECK(p.TakeMinusLineDecide() == -1);
    // B: the menu's Back, and the rows have the pad again.
    CHECK(p.OnPad(kPadB, now + 30) == 0);
    CHECK(p.TakeMinusListBack());
    CHECK_FALSE(p.MinusInList());
    CHECK_FALSE(p.TakeMinusBack());
}

TEST_CASE("pause tab (0.11.9): the Mods tab's page is its list: B is its Back on a mod's page, else the game's") {
    std::uint64_t now = 1000;
    Page p = MinusPageOn(now);
    p.OnPad(kPadR, now + 10), p.OnPad(0, now + 12);
    p.OnPad(kPadR, now + 14), p.OnPad(0, now + 16);
    REQUIRE(p.MinusTab() == 2);
    // No rows of its own: the list has the pad at once.
    p.SetMinusRows(0);
    p.SetMinusLines(0b1110, true);
    p.OnPad(kPadDown, now + 18), p.OnPad(0, now + 20);
    CHECK(p.MinusInList());
    CHECK(p.TakeMinusMoves() == Page::kMinusMoveDown);
    // On the mods' list: B is the game's (the menu closes).
    CHECK(p.OnPad(kPadB, now + 22) == kPadB);
    CHECK_FALSE(p.TakeMinusListBack());
    p.OnPad(0, now + 24);
    // On a mod's page: B is its Back, and the list keeps the pad (no rows to go back to).
    p.SetMinusRawBack(true);
    CHECK(p.OnPad(kPadB, now + 26) == 0);
    CHECK(p.TakeMinusListBack());
    CHECK(p.MinusInList());
    p.OnPad(0, now + 28);
    // L: Lobby again.
    p.OnPad(kPadL, now + 30), p.OnPad(0, now + 32);
    CHECK(p.MinusTab() == 1);
    CHECK_FALSE(p.MinusInList());
}

TEST_CASE("pause tab (0.11.9): a field typed into on the - page: every button is the field's, B cancels it") {
    std::uint64_t now = 1000;
    Page p = MinusPageOn(now);
    p.SetMinusRows(0b11);
    p.SetMinusInView(true);
    p.Field(0).Clear();
    p.StartTyping(0);
    REQUIRE(p.Typing() == 0);
    p.OnScreen(Strip(0x9000, 8, kMapCategory, kMinusMode), now + 498);   // (the - menu still up)
    p.OnPad(0, now + 500);   // (past the start's moment)
    CHECK(p.OnPad(kPadB, now + 510) == 0);
    p.OnPad(0, now + 512);
    CHECK(p.Typing() == -1);
    CHECK_FALSE(p.TakeMinusBack());   // (that B was the field's Cancel, not a view back)
    // Without the - page or our page: no typing starts.
    Page q = UsablePage();
    q.StartTyping(0);
    CHECK(q.Typing() == -1);
}

TEST_CASE("pause tab (0.11.9): the - menu's Lobby and Multiplayer pages: every row opens something, none empty-handed") {
    SessionView idle;
    CHECK(RowTexts(MinusPage::kLobby, idle) == std::vector<std::string>{"Find a game", "Host a game", "Join a game", "", ""});
    CHECK(RowTexts(MinusPage::kMultiplayer, idle) ==
          std::vector<std::string>{"Players", "Co-op settings", "Local co-op", "", ""});   // (not in a game: no row 3)
    Session s;
    CHECK(ActionFor(MinusPage::kLobby, 0, s) == RowAction::kViewLan);
    CHECK(ActionFor(MinusPage::kLobby, 1, s) == RowAction::kViewHost);
    CHECK(ActionFor(MinusPage::kLobby, 2, s) == RowAction::kViewJoin);
    CHECK(ActionFor(MinusPage::kLobby, 3, s) == RowAction::kNone);
    CHECK(ActionFor(MinusPage::kMultiplayer, 0, s) == RowAction::kViewPlayers);
    CHECK(ActionFor(MinusPage::kMultiplayer, 1, s) == RowAction::kViewMenu);
    CHECK(ActionFor(MinusPage::kMultiplayer, 2, s) == RowAction::kViewLocal);
    CHECK(ActionFor(MinusPage::kMultiplayer, 3, s) == RowAction::kNone);
    // Hosting: the Multiplayer page has your game (Let in, Stop there); a guest: your session (Leave there).
    SessionView host;
    host.s.hosting = host.s.in_one = true;
    host.asking = {"Saria"};
    CHECK(RowTexts(MinusPage::kMultiplayer, host)[3] == "Your game: Saria asks");
    CHECK(ActionFor(MinusPage::kMultiplayer, 3, host.s) == RowAction::kViewHost);
    SessionView guest;
    guest.s.in_one = true;
    CHECK(RowTexts(MinusPage::kMultiplayer, guest)[3] == "Your session");
    CHECK(ActionFor(MinusPage::kMultiplayer, 3, guest.s) == RowAction::kViewJoin);
    // Every row with words opens something; every row without opens nothing.
    for (const SessionView& v : {idle, host, guest}) {
        for (MinusPage p : {MinusPage::kLobby, MinusPage::kMultiplayer}) {
            const auto rows = RowTexts(p, v);
            for (int r = 0; r < kRows; ++r) {
                CHECK(rows[static_cast<std::size_t>(r)].empty() == (ActionFor(p, r, v.s) == RowAction::kNone));
            }
        }
    }
    CHECK(std::string(ActionWords(RowAction::kViewLocal)).size() > 3);
    CHECK(RowTexts(MinusPage::kMods, idle) == std::vector<std::string>(kRows));
}
