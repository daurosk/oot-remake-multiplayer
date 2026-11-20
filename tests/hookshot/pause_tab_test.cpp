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

namespace {

using namespace CoopPauseTab;

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

// The menu open on System, the strip with the pack's five tabs, at `now`.
void OnSystem(Page& p, std::uint64_t now, int tabs = 5) {
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
    p.OnScreen(Strip(0x9000, tabs, kSystemPage), now);
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

TEST_CASE("pause tab: the fifth tab comes in with the strip's opening, filled once per opening") {
    Page p = UsablePage();
    auto d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 0), 1000);   // (closed)
    CHECK_FALSE(d.tab_shown);
    CHECK_FALSE(d.fill_tab);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), 1016);   // (opening: with the game's four)
    CHECK(d.set_tabs);
    CHECK(d.tab_shown);
    CHECK(d.fill_tab);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 2), 1300);   // (open: not filled again)
    CHECK(d.set_tabs);
    CHECK(d.tab_shown);
    CHECK_FALSE(d.fill_tab);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 3), 2000);   // (closing: fading out with the four)
    CHECK(d.tab_shown);
    CHECK_FALSE(d.fill_tab);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 0), 2300);   // (closed: hidden)
    CHECK_FALSE(d.tab_shown);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), 3000);   // (the next opening: filled again)
    CHECK(d.fill_tab);
}

TEST_CASE("pause tab: the presses the module takes have the game's own sounds") {
    Page p = UsablePage();
    OnSystem(p, 1000);
    CHECK(p.TakeStripSound() == nullptr);
    p.OnPad(kPadR, 1000);
    CHECK(std::string(p.TakeStripSound()) == "mc_TabToRight");
    CHECK(p.TakeStripSound() == nullptr);   // (once)
    p.OnPad(0, 1016);
    p.OnPad(kPadA, 1032);
    CHECK(std::string(p.TakePagesSound()) == "button_decide");
    p.OnPad(0, 1048);
    p.OnPad(kPadL, 1064);
    CHECK(std::string(p.TakeStripSound()) == "mc_TabToLeft");
    p.OnPad(0, 1080);
    p.OnPad(kPadL, 1096);   // (L on System: the game's own, no sound of ours)
    CHECK(p.TakeStripSound() == nullptr);
}

TEST_CASE("pause tab: the menu closed on our page opens on it again, not after L") {
    Page p = UsablePage();
    std::uint64_t now = 1000;
    OnSystem(p, now);
    CHECK(p.OnPad(kPadR, now) == 0);
    CHECK(p.Shown());
    p.OnPad(0, now + 16);
    // The menu closes (B or +): our page goes with it.
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
    p.OnPad(kPadL, now + 5400);
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
    p.OnPad(kPadR, 1000);
    p.OnPad(0, 1016);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 3), 1032);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), 1032);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 0), 1300);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 0), 1300);
    CHECK_FALSE(p.Shown());   // (closed: nothing)
    // As a run had it: the strip's first update of the opening comes first, the pages' last update still the closed
    // one (old). The page is back there, set through the strip's update, its tab lit from the start.
    const auto d = p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), 4000);
    CHECK(p.Shown());
    CHECK(d.set_page);
    CHECK(d.page_shown);
    CHECK(d.tab_lit);
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
    p.OnPad(kPadR, 1000);
    p.OnPad(0, 1016);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 3), 1032);
    CHECK_FALSE(p.Shown());
    p.OnScreen(Pages(0xa000, 1, 1, 2), 3000);   // (open on another page)
    p.OnScreen(Strip(0x9000, 5, 1, kPlusMode, 2), 3000);
    p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 2), 3100);   // (then stepped to System)
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 2), 3100);
    CHECK_FALSE(p.Shown());
}

TEST_CASE("pause tab: R on System shows the page and lights the fifth tab, L gives System back") {
    Page p = UsablePage();
    std::uint64_t now = 1000;
    // First updates: our page hidden (System shown), the fifth tab shown, System's lit.
    auto d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
    CHECK(d.set_page);
    CHECK_FALSE(d.page_shown);
    CHECK(d.set_texts);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage), now);
    CHECK(d.set_tabs);
    CHECK(d.tab_shown);
    CHECK_FALSE(d.tab_lit);
    CHECK(d.fill_tab);
    CHECK_FALSE(p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now).any());   // (nothing again)
    CHECK_FALSE(p.OnScreen(Strip(0x9000, 5, kSystemPage), now).any());
    // R: taken, the page shows at the next updates.
    CHECK(p.OnPad(kPadR, now) == 0);
    CHECK(p.Shown());
    d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 16);
    CHECK(d.set_page);
    CHECK(d.page_shown);
    d = p.OnScreen(Strip(0x9000, 5, kSystemPage), now + 16);
    CHECK(d.set_tabs);
    CHECK(d.tab_lit);
    // R held: still the module's (no fresh R for the game).
    CHECK(p.OnPad(kPadR, now + 32) == 0);
    CHECK(p.OnPad(0, now + 48) == 0);
    // L: back to System; L stays the module's until let go, so the game does not step to Items.
    CHECK(p.OnPad(kPadL, now + 64) == 0);
    CHECK_FALSE(p.Shown());
    CHECK(p.TakeWhy() == "L");
    CHECK(p.OnPad(kPadL, now + 80) == 0);
    CHECK(p.OnPad(0, now + 96) == 0);
    CHECK(p.OnPad(kPadL, now + 112) == kPadL);   // (a new L is the game's: Items)
    d = p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 120);
    CHECK(d.set_page);
    CHECK_FALSE(d.page_shown);
    // ZR and ZL do the same.
    p.OnPad(0, now + 130);
    CHECK(p.OnPad(kPadZR, now + 140) == 0);
    CHECK(p.Shown());
    CHECK(p.OnPad(kPadZL, now + 150) == 0);
    CHECK_FALSE(p.Shown());
}

TEST_CASE("pause tab: under the page the game gets only B, + and -, and up and down for its cursor, A is ours") {
    Page p = UsablePage();
    std::uint64_t now = 1000;
    OnSystem(p, now);
    REQUIRE(p.OnPad(kPadR, now) == 0);
    p.OnPad(0, now + 1);
    const std::uint64_t x = 1ull << 2, y = 1ull << 3, left = 1ull << 12, right = 1ull << 14, stick_right = 1ull << 18;
    CHECK(p.OnPad(kPadA, now + 2) == 0);   // A: ours, not the System page's
    CHECK(p.TakeDecide());
    CHECK_FALSE(p.TakeDecide());
    CHECK(p.OnPad(x | y | left | right | stick_right | kPadZL, now + 3) == 0);   // (ZL: back to System, taken)
    CHECK_FALSE(p.Shown());
    p.OnPad(0, now + 4);
    REQUIRE(p.OnPad(kPadR, now + 5) == 0);
    p.OnPad(0, now + 6);
    // Up and down reach the game: the System page's focus moves, and the game's cursor frame with it over our rows.
    CHECK(p.OnPad(kPadDown, now + 7) == kPadDown);
    CHECK(p.OnPad(kPadUp | kPadStickUp, now + 8) == (kPadUp | kPadStickUp));
    CHECK(p.OnPad(kPadDown | kPadA, now + 9) == kPadDown);
    CHECK(p.TakeDecide());
    p.OnPad(0, now + 10);
    CHECK(p.OnPad(kPadB, now + 11) == kPadB);
    CHECK(p.OnPad(kPadPlus, now + 12) == kPadPlus);
    CHECK(p.OnPad(kPadMinus, now + 13) == kPadMinus);
    CHECK(p.Shown());   // (the game acts on B, + and -; the screens' updates then give System back)
}

TEST_CASE("pause tab: the page does not show off System, mid-step, in the Minus menu, without the pack or under a window") {
    std::uint64_t now = 1000;
    SUBCASE("on Items") {
        Page p = UsablePage();
        p.OnScreen(Pages(0xa000, 4, 4), now);
        p.OnScreen(Strip(0x9000, 5, 4), now);
        CHECK(p.OnPad(kPadR, now) == kPadR);   // (the game's: Items to System)
        CHECK_FALSE(p.Shown());
    }
    SUBCASE("mid-step to System") {
        Page p = UsablePage();
        p.OnScreen(Pages(0xa000, 4, kSystemPage), now);
        p.OnScreen(Strip(0x9000, 5, kSystemPage), now);
        CHECK(p.OnPad(kPadR, now) == kPadR);
        CHECK_FALSE(p.Shown());
    }
    SUBCASE("the Minus menu") {
        Page p = UsablePage();
        p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now);
        p.OnScreen(Strip(0x9000, 5, 2, 0), now);
        CHECK(p.OnPad(kPadR, now) == kPadR);
        CHECK_FALSE(p.Shown());
    }
    SUBCASE("four tabs: the pack's layouts are not there") {
        Page p = UsablePage();
        OnSystem(p, now, 4);
        CHECK(p.OnPad(kPadR, now) == kPadR);
        CHECK_FALSE(p.Shown());
        CHECK_FALSE(p.OnScreen(Strip(0x9000, 4, kSystemPage), now).set_tabs);   // (no fifth tab to touch)
    }
    SUBCASE("the menu still opening") {
        Page p = UsablePage();
        p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage, 1), now);
        p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 1), now);
        CHECK(p.OnPad(kPadR, now) == kPadR);
        CHECK_FALSE(p.Shown());
    }
    SUBCASE("a window over the System page (Save, Options, a question)") {
        Page p = UsablePage();
        OnSystem(p, now);
        p.OnScreen(Blocker(2), now);
        CHECK(p.OnPad(kPadR, now + 10) == kPadR);
        CHECK_FALSE(p.Shown());
        // Gone (state 0) for a while: R is ours again.
        p.OnScreen(Blocker(0), now + 20);
        p.OnPad(0, now + 299);
        OnSystem(p, now + 300);
        CHECK(p.OnPad(kPadR, now + 300) == 0);
        CHECK(p.Shown());
    }
    SUBCASE("the screens' updates stopped") {
        Page p = UsablePage();
        OnSystem(p, now);
        // (the pad is polled every frame while the game runs; with no call at all it would be a stall of the whole
        // game, which keeps the screens fresh: AfterStall)
        for (std::uint64_t t = now + 16; t < now + 1000; t += 16) p.OnPad(0, t);
        CHECK(p.OnPad(kPadR, now + 1000) == kPadR);
        CHECK_FALSE(p.Shown());
    }
}

TEST_CASE("pause tab: closing, the Minus menu, a window or Game Over give System back at once") {
    std::uint64_t now = 1000;
    const auto shown = [&](Page& p) {
        OnSystem(p, now);
        REQUIRE(p.OnPad(kPadR, now) == 0);
        p.OnPad(0, now + 1);
        p.OnScreen(Pages(0xa000, kSystemPage, kSystemPage), now + 2);
        REQUIRE(p.Shown());
    };
    SUBCASE("B closes the menu") {
        Page p = UsablePage();
        shown(p);
        CHECK(p.OnPad(kPadB, now + 3) == kPadB);
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

TEST_CASE("pause tab: each row leads to co-op's own part, by what co-op is doing") {
    Session idle;
    Session joined;
    joined.in_one = true;
    Session hosting_no_code;
    hosting_no_code.hosting = hosting_no_code.in_one = true;
    Session hosting = hosting_no_code;
    hosting.code = true;

    for (const Session& s : {idle, joined, hosting_no_code, hosting}) {
        CHECK(ActionFor(kRowSession, s) == (s.in_one ? RowAction::kViewSession : RowAction::kHost));
        // (not in a session, the Players row is the games on this network, and Let in is auto-rejoin's switch)
        CHECK(ActionFor(kRowPlayers, s) == (s.in_one ? RowAction::kViewPlayers : RowAction::kViewLan));
        CHECK(ActionFor(kRowLeave, s) == (s.in_one ? RowAction::kConfirmLeave : RowAction::kNone));
        CHECK(ActionFor(-1, s) == RowAction::kNone);              // (no row under the cursor)
        CHECK(ActionFor(kRows, s) == RowAction::kNone);
    }
    // Join code: the host copies its room name or code (the row says so); a host whose code is still coming, or a
    // guest, the Session view; alone the Join view.
    CHECK(ActionFor(kRowJoinCode, hosting) == RowAction::kCopyCode);
    CHECK(ActionFor(kRowJoinCode, hosting_no_code) == RowAction::kViewSession);
    CHECK(ActionFor(kRowJoinCode, joined) == RowAction::kViewSession);
    CHECK(ActionFor(kRowJoinCode, idle) == RowAction::kViewJoin);
    // Let in: the host's Let in view; a guest Resync from host when its game can take the host's save; else nothing.
    CHECK(ActionFor(kRowLetIn, hosting) == RowAction::kViewLetIn);
    CHECK(ActionFor(kRowLetIn, hosting_no_code) == RowAction::kViewLetIn);
    CHECK(ActionFor(kRowLetIn, joined) == RowAction::kNone);
    CHECK(ActionFor(kRowLetIn, idle) == RowAction::kAutoRejoin);
    Session resync = joined;
    resync.can_resync = true;
    CHECK(ActionFor(kRowLetIn, resync) == RowAction::kResync);
    idle.can_resync = true;   // (not in a session: nothing to resync from; the row is auto-rejoin's)
    CHECK(ActionFor(kRowLetIn, idle) == RowAction::kAutoRejoin);
    idle.can_resync = false;
    // Each has words for the log.
    for (RowAction a : {RowAction::kNone, RowAction::kOpenMain, RowAction::kCopyCode, RowAction::kAllow,
                        RowAction::kHost, RowAction::kConfirmLeave}) {
        CHECK(std::string(ActionWords(a)).size() > 3);
    }
}

TEST_CASE("pause tab: the invite code's two rows: the host's Paste answer, a waiting guest's Copy answer") {
    // A host with an invite code (no relay room): a friend whose direct join failed sends back an answer code, which
    // the Let in view's fourth row pastes (co-op's menu's Paste answer). The five rows keep Let in: a run showed
    // players pressing "Paste answer code" there without knowing what it was (2026-11-19).
    Session host;
    host.hosting = host.in_one = host.code = host.invite = true;
    CHECK(ActionFor(kRowLetIn, host) == RowAction::kViewLetIn);
    CHECK(ActionFor(View::kLetIn, 3, host) == RowAction::kPasteAnswer);
    CHECK(ActionFor(kRowJoinCode, host) == RowAction::kCopyCode);
    host.asking = true;
    CHECK(ActionFor(kRowLetIn, host) == RowAction::kViewLetIn);
    // A relay room has no answer codes: Let in is its view (Let in or Refuse who asks).
    Session relay;
    relay.hosting = relay.in_one = relay.code = true;
    CHECK(ActionFor(kRowLetIn, relay) == RowAction::kViewLetIn);
    // A guest the host could not reach: Join code copies this game's answer code (co-op's menu's Copy answer).
    Session waiting;
    waiting.in_one = waiting.wait_host = true;
    CHECK(ActionFor(kRowJoinCode, waiting) == RowAction::kCopyAnswer);
    CHECK(ActionFor(kRowLeave, waiting) == RowAction::kConfirmLeave);
    for (RowAction a : {RowAction::kPasteAnswer, RowAction::kCopyAnswer}) {
        CHECK(std::string(ActionWords(a)).size() > 3);
    }

    SessionView v;
    v.s = Session{};
    v.s.hosting = v.s.in_one = v.s.code = v.s.invite = true;
    v.players = 1;
    CHECK(RowTexts(v) ==
          std::vector<std::string>{"Hosting", "Copy invite code", "Players: just you", "Let in: nobody asks", "Stop hosting"});
    CHECK(RowTexts(View::kLetIn, v)[3] == "Paste answer code");
    v.asking = {"Saria"};
    v.s.asking = true;
    CHECK(RowTexts(v)[kRowLetIn] == "Let in Saria");
    SessionView w;
    w.s.in_one = w.s.wait_host = true;
    w.connecting = true;
    CHECK(RowTexts(w)[kRowSession] == "Connecting...");
    CHECK(RowTexts(w)[kRowJoinCode] == "Copy answer code");
}

TEST_CASE("pause tab: the rows say what co-op is doing, in at most a row's width") {
    SessionView idle;
    CHECK(RowTexts(idle) == std::vector<std::string>{"Host a game", "Join a game", "Find a game",
                                                     "Auto-rejoin: Off", "Leave (not in a session)"});
    idle.s.auto_rejoin = true;
    CHECK(RowTexts(idle)[kRowLetIn] == "Auto-rejoin: On");
    idle.s.rejoining = true;
    CHECK(RowTexts(idle)[kRowLetIn] == "Auto-rejoin: trying...");
    idle.s.auto_rejoin = idle.s.rejoining = false;

    SessionView host;
    host.s.hosting = host.s.in_one = true;
    host.players = 1;
    CHECK(RowTexts(host) ==
          std::vector<std::string>{"Hosting", "Room: coming...", "Players: just you", "Let in: nobody asks", "Stop hosting"});
    host.room = "forest-gang";
    host.players = 3;
    host.asking = {"Saria"};
    CHECK(RowTexts(host)[kRowJoinCode] == "Room: forest-gang");
    CHECK(RowTexts(host)[kRowPlayers] == "Players: 3");
    CHECK(RowTexts(host)[kRowLetIn] == "Let in Saria");
    host.asking = {"Saria", "Mido", "Darunia"};
    CHECK(RowTexts(host)[kRowLetIn] == "Let in Saria (+2)");

    SessionView guest;
    guest.s.in_one = true;
    guest.players = 2;
    CHECK(RowTexts(guest) ==
          std::vector<std::string>{"In a session", "Session details", "Players: 2", "Let in: the host does", "Leave"});
    guest.s.can_resync = true;
    CHECK(RowTexts(guest)[kRowLetIn] == "Resync from host");
    guest.s.can_resync = false;
    guest.confirm_leave = true;
    CHECK(RowTexts(guest)[kRowLeave] == "A again: leave");
    host.confirm_leave = true;
    CHECK(RowTexts(host)[kRowLeave] == "A again: stop hosting");
    host.confirm_leave = false;
    SessionView joining;
    joining.connecting = true;
    joining.s.in_one = true;
    CHECK(RowTexts(joining)[kRowSession] == "Connecting...");

    // A long room name is cut at a UTF-8 character's start, with "...".
    host.room = std::string(30, 'a') + "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";
    const std::string cut = RowTexts(host)[kRowJoinCode];
    CHECK(cut.size() <= kRowChars);
    CHECK(cut.substr(cut.size() - 3) == "...");
    CHECK((static_cast<unsigned char>(cut[cut.size() - 4]) & 0xc0) != 0xc0);   // (no lead byte left alone)
    for (const SessionView& v : {idle, host, guest, joining}) {
        for (const std::string& r : RowTexts(v)) CHECK(r.size() <= kRowChars);
    }
}

TEST_CASE("pause tab: a page whose panes or game calls are missing never shows, and the mouse keeps tab 5 out") {
    // (an old or mismatched pack, a module without the scratch API: the page would be undrawn, so System's A and
    // its focused button must stay System's; Act would otherwise run row 0 Host or row 4 Leave on System's buttons)
    std::uint64_t now = 1000;
    Page p;   // (not usable until the module says so)
    CHECK(p.MousePage() == -2);
    OnSystem(p, now);
    CHECK(p.OnPad(kPadR, now) == kPadR);   // (the game's R: nothing of ours)
    CHECK_FALSE(p.Shown());
    p.OnPad(0, now + 1);
    CHECK(p.OnPad(kPadA, now + 2) == kPadA);   // (System's A)
    CHECK_FALSE(p.TakeDecide());
    p.OnPad(0, now + 3);
    // Usable: tab 5 counts for the mouse, R shows the page.
    p.SetUsable(true);
    CHECK(p.MousePage() == -1);
    OnSystem(p, now + 4);
    CHECK(p.OnPad(kPadR, now + 4) == 0);
    CHECK(p.Shown());
    CHECK(p.MousePage() == 4);
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
    // The strip the same way: its fifth tab set again.
    const std::uint64_t s1 = p.Track(Strip(0x9000, 5, kSystemPage, kPlusMode, 1));
    CHECK(s1 != 0);
    CHECK(p.OnScreen(Strip(0x9000, 5, kSystemPage), now + 50).set_tabs);
    CHECK_FALSE(p.OnScreen(Strip(0x9000, 5, kSystemPage), now + 51).set_tabs);
    p.OnScreen(Strip(0x9000, 5, kSystemPage, kPlusMode, 0), now + 60);
    CHECK(p.Track(Strip(0x9000, 5, kSystemPage, kPlusMode, 1)) != s1);
    CHECK(p.OnScreen(Strip(0x9000, 5, kSystemPage), now + 70).set_tabs);
}

TEST_CASE("pause tab: co-op that came with the game's start, and a failed Host, show the Session view") {
    // (the launcher rule, as co-op's menu: Host.bat / Join.bat / coop.ini co-op is not started or stopped here)
    Session launcher;
    launcher.by_launcher = launcher.in_one = true;
    CHECK(ActionFor(kRowSession, launcher) == RowAction::kViewSession);
    CHECK(ActionFor(kRowLeave, launcher) == RowAction::kOpenMain);   // (never two A's to a teardown)
    launcher.in_one = false;   // (not connected yet: still not Host)
    CHECK(ActionFor(kRowSession, launcher) == RowAction::kViewSession);
    CHECK(ActionFor(kRowLeave, launcher) == RowAction::kOpenMain);
    SessionView lv;
    lv.s = launcher;
    CHECK(RowTexts(lv)[kRowLeave] == "Leave: restart the game");
    // A Host (or Join) that failed: the row says so, and A shows the Session view, which says why.
    Session failed;
    failed.failed = true;
    CHECK(ActionFor(kRowSession, failed) == RowAction::kViewSession);
    SessionView fv;
    fv.s = failed;
    CHECK(RowTexts(fv)[kRowSession] == "Co-op failed: A says why");
    CHECK(RowTexts(fv)[kRowSession].size() <= kRowChars);
}

TEST_CASE("pause tab: a second A (Leave, Kick, Ban) is waited for only while the cursor stays on its row") {
    CHECK(KeepConfirm(true, kRowLeave, kRowLeave));
    CHECK_FALSE(KeepConfirm(true, kRowLetIn, kRowLeave));
    CHECK_FALSE(KeepConfirm(true, -1, kRowLeave));
    CHECK_FALSE(KeepConfirm(true, -1, -1));       // (nothing waited for)
    CHECK(KeepConfirm(true, 2, 2));               // (Kick, on the host's controls)
    CHECK_FALSE(KeepConfirm(false, kRowLeave, kRowLeave));   // (System back: no second A waited for)
}

TEST_CASE("pause tab: the rows' own views: Players, Let in and Join on the same five rows, Back last") {
    Session idle;
    Session host;
    host.hosting = host.in_one = host.code = true;
    // Players: who is in, one a row; only Back acts.
    SessionView pv;
    pv.s.in_one = true;
    pv.names = {"dauro (you)", "Saria (host)"};
    CHECK(RowTexts(View::kPlayers, pv) == std::vector<std::string>{"dauro (you)", "Saria (host)", " ", " ", "Back"});
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
    CHECK(RowTexts(View::kLetIn, lv) == std::vector<std::string>{"Let in Mido", "Refuse Mido", "+1 more ask", " ", "Back"});
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
    CHECK(ActionFor(View::kJoin, 0, in_one) == RowAction::kNone);
    CHECK(ActionFor(View::kJoin, 1, in_one) == RowAction::kNone);
    Session launcher;
    launcher.by_launcher = launcher.rejoin = true;
    CHECK(ActionFor(View::kJoin, 0, launcher) == RowAction::kNone);
    SessionView jv;
    jv.s = again;
    jv.rejoin_host = "Saria";
    CHECK(RowTexts(View::kJoin, jv) == std::vector<std::string>{"Paste code and join", "Room: A to type its name",
                                                               "Password: A to type it", "Join Saria again", "Back"});
    // The main view is the five rows; out of range is nothing.
    CHECK(ActionFor(View::kMain, kRowPlayers, idle) == ActionFor(kRowPlayers, idle));
    CHECK(RowTexts(View::kMain, none) == RowTexts(none));
    CHECK(ActionFor(View::kJoin, -1, idle) == RowAction::kNone);
    CHECK(ActionFor(View::kLetIn, kRows, host) == RowAction::kNone);
    for (View w : {View::kPlayers, View::kLetIn, View::kJoin}) {
        for (const std::string& r : RowTexts(w, lv)) CHECK((!r.empty() && r.size() <= kRowChars));
    }
    for (RowAction a : {RowAction::kViewPlayers, RowAction::kViewLetIn, RowAction::kViewJoin, RowAction::kBack,
                        RowAction::kDeny, RowAction::kJoinPaste, RowAction::kRejoin}) {
        CHECK(std::string(ActionWords(a)).size() > 3);
    }
}

TEST_CASE("pause tab: in a view, B goes back to the five rows instead of closing the menu") {
    std::uint64_t now = 1000;
    Page p = UsablePage();
    OnSystem(p, now);
    REQUIRE(p.OnPad(kPadR, now) == 0);
    p.OnPad(0, now + 1);
    CHECK(p.OnPad(kPadB, now + 2) == kPadB);   // (the five rows: B is the game's, it closes the menu)
    CHECK_FALSE(p.TakeBack());
    p.OnPad(0, now + 3);
    p.SetSubView(true);
    CHECK(p.OnPad(kPadB, now + 4) == 0);   // (a view: ours)
    CHECK(p.TakeBack());
    CHECK_FALSE(p.TakeBack());
    CHECK(p.OnPad(kPadB, now + 5) == 0);   // (held: still ours)
    p.SetSubView(false);
    CHECK(p.OnPad(kPadB, now + 6) == 0);   // (still held from the view: not a fresh B for the game)
    p.OnPad(0, now + 7);
    CHECK(p.OnPad(kPadB, now + 8) == kPadB);
    // The page going ends the view.
    p.OnPad(0, now + 9);
    p.SetSubView(true);
    CHECK(p.OnPad(kPadL, now + 10) == 0);
    CHECK_FALSE(p.Shown());
    p.OnPad(0, now + 11);
    CHECK(p.OnPad(kPadB, now + 12) == kPadB);
    CHECK_FALSE(p.TakeBack());
    p.SetSubView(true);   // (not shown: no view)
    CHECK(p.OnPad(kPadB, now + 13) == kPadB);
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
    // Its join password: on the first row once A asks (friends who found the game on this network type it).
    hv.s.has_password = host.has_password = true;
    CHECK(ActionFor(View::kSession, 0, host) == RowAction::kShowPassword);
    CHECK(RowTexts(View::kSession, hv)[0] == "Show join password");
    hv.host_password = "ABCDEFGHJKLMNPQRSTUV";
    hv.s.password_shown = host.password_shown = true;   // (two fives a row: 23 capitals ran past the button's edges)
    CHECK(RowTexts(View::kSession, hv) == std::vector<std::string>{"Password (A hides it):", "ABCDE FGHJK", "LMNPQ RSTUV",
                                                                    " ", "Back"});
    for (int r = 0; r < kRows - 1; ++r) CHECK(ActionFor(View::kSession, r, host) == RowAction::kShowPassword);
    CHECK(ActionFor(View::kSession, kRows - 1, host) == RowAction::kBack);
    hv.s.password_shown = host.password_shown = false;
    hv.host_password.clear();
    // The main view's Join code row says what its copy did too.
    hv.note = "No clipboard here";
    CHECK(RowTexts(hv)[kRowJoinCode] == "No clipboard here");

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
    CHECK(RowTexts(View::kSession, gv) ==
          std::vector<std::string>{"Joined Saria", "With 2 players", " ", "Resync from host", "Back"});
    gv.why = "Voice chat is off";
    CHECK(RowTexts(View::kSession, gv)[2] == "Voice chat is off");
    // A guest the host could not reach: its answer code to copy.
    Session waiting;
    waiting.in_one = waiting.wait_host = true;
    CHECK(ActionFor(View::kSession, 1, waiting) == RowAction::kCopyAnswer);
    SessionView wv;
    wv.s = waiting;
    wv.connecting = true;
    CHECK(RowTexts(View::kSession, wv)[0] == "Waiting for the host");
    CHECK(RowTexts(View::kSession, wv)[1] == "Copy answer code");

    // Failed: why, over the free rows, and Host again (not for co-op that came with the game's start).
    Session failed;
    failed.failed = true;
    CHECK(ActionFor(View::kSession, 3, failed) == RowAction::kHost);
    CHECK(ActionFor(View::kSession, 1, failed) == RowAction::kNone);
    SessionView fv;
    fv.s = failed;
    fv.why = "The port 27015 is in use by another program: close it, or pick another port in coop.ini";
    const std::vector<std::string> rows = RowTexts(View::kSession, fv);
    CHECK(rows[0] == "Co-op failed:");
    CHECK(rows[1] == "The port 27015 is in use");
    CHECK(rows[2].substr(rows[2].size() - 3) == "...");   // (two rows for it: the rest is cut)
    CHECK(rows[3] == "Host again");
    fv.s.by_launcher = true;
    CHECK(ActionFor(View::kSession, 3, fv.s) == RowAction::kNone);
    CHECK(RowTexts(View::kSession, fv)[3].substr(0, 5) == "close");   // (its row goes to why: the third of it)
    SessionView idle;
    CHECK(RowTexts(View::kSession, idle)[0] == "Not in a session");
    for (const SessionView& v : {hv, gv, wv, fv, idle}) {
        for (const std::string& r : RowTexts(View::kSession, v)) CHECK((!r.empty() && r.size() <= kRowChars));
    }
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
    list.page = 1;
    CHECK(ActionFor(View::kPlayers, 2, list) == RowAction::kNone);
    SessionView lv;
    lv.s = list;
    lv.names = {"a", "b", "c", "d", "e"};
    CHECK(RowTexts(View::kPlayers, lv) == std::vector<std::string>{"d", "e", " ", "More players (2/2)", "Back"});

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
                                                                   "Mute their chat (you)", " ", "Back"});
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

TEST_CASE("pause tab: the Join view's fields: typing takes every button from System until A or B") {
    Page p = UsablePage();
    OnSystem(p, 1000);
    p.OnPad(kPadR, 1000);
    p.OnPad(0, 1016);
    REQUIRE(p.Shown());
    p.StartTyping(0);
    CHECK(p.Typing() == 0);
    CHECK(p.OnPad(kPadUp, 1032) == 0);          // (Up is the field's: none reaches System)
    CHECK(p.Field(0).text() == "a");
    CHECK(p.OnPad(0, 1048) == 0);
    CHECK(p.OnPad(kPadRight, 1064) == 0);
    CHECK(p.OnPad(kPadRight | kPadDown, 1080) == 0);   // (down with right still held: a new letter, _)
    CHECK(p.Field(0).text() == "a_");
    // Held: again and again after a moment (the screens updating meanwhile, as in a game).
    OnSystem(p, 1100);
    p.OnPad(kPadUp, 1100);
    OnSystem(p, 1250);
    p.OnPad(kPadUp, 1250);
    OnSystem(p, 1400);
    p.OnPad(kPadUp, 1400);
    OnSystem(p, 1550);
    p.OnPad(kPadUp, 1550);   // (held over 400 ms: a step)
    CHECK(p.Field(0).text().size() == 2);
    CHECK(p.Field(0).text()[1] != '_');          // (stepped on from _ by the held Up)
    CHECK(p.OnPad(0, 1660) == 0);
    CHECK(p.OnPad(kPadB, 1670) == 0);            // (B ends typing; it does not leave the view or the page)
    CHECK(p.Typing() == -1);
    CHECK(p.Shown());
    CHECK_FALSE(p.TakeBack());
    p.OnPad(0, 1680);
    // The keyboard: its characters and keys while typing, none otherwise.
    CHECK_FALSE(p.Char('x'));
    p.StartTyping(1);
    CHECK(p.Char('P'));
    CHECK(p.Char(' '));                          // (taken, but not a password's letter)
    CHECK(p.Key(0x25, false));                   // (Left)
    CHECK(p.Char('a'));
    CHECK(p.Field(1).text() == "aP");
    CHECK(p.Key(0x0d, false));                   // (Enter: done)
    CHECK(p.Typing() == -1);
    CHECK_FALSE(p.Key(0x0d, false));
}

TEST_CASE("pause tab: typing ends when the page goes, and the Join view's rows say the fields") {
    Page p = UsablePage();
    OnSystem(p, 1000);
    p.OnPad(kPadR, 1000);
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
    CHECK(ActionFor(View::kJoin, 1, s) == RowAction::kNone);         // (nobody joins from inside a session)
    SessionView v;
    auto rows = RowTexts(View::kJoin, v);
    CHECK(rows[1] == "Room: A to type its name");
    CHECK(rows[2] == "Password: A to type it");
    CHECK(rows[3] == " ");
    v.room_field = "fore[s]t";
    v.typing = 0;
    v.room_ready = true;
    rows = RowTexts(View::kJoin, v);
    CHECK(rows[1] == "Room: fore[s]t");
    CHECK(rows[3] == "Join room: no relay set");
    v.relay_set = true;
    CHECK(RowTexts(View::kJoin, v)[3] == "Join this room");
}

TEST_CASE("pause tab: the focused row is the System button's number under it, by its name") {
    CHECK(RowOfButton("Pa_Button_00") == 0);
    CHECK(RowOfButton("Pa_Button_04") == 4);
    CHECK(RowOfButton("Pa_Button_05") == -1);   // (five rows)
    CHECK(RowOfButton("Pa_Button_0") == -1);
    CHECK(RowOfButton("Pa_Button_010") == -1);
    CHECK(RowOfButton("N_SlideList_00") == -1);
    CHECK(RowOfButton("") == -1);
}

TEST_CASE("pause tab: the kit's release shows on the page, solo and in a session (0.11.6)") {
    SessionView solo;
    solo.version = "Squadala! v0.11.6";
    const auto rows = RowTexts(solo);
    CHECK(rows[kRowLeave] == "Squadala! v0.11.6");   // co-op idle: the Leave row has nothing to do, it says the release
    CHECK(ActionFor(kRowLeave, solo.s) == RowAction::kNone);
    SessionView guest = solo;
    guest.s.in_one = true;
    guest.players = 2;
    CHECK(RowTexts(guest)[kRowLeave] == "Leave");    // in a session the row leaves, as before
    const auto details = RowTexts(View::kSession, guest);
    CHECK(details[3] == "Squadala! v0.11.6");        // the Session view's free row
    guest.s.can_resync = true;
    const auto with_resync = RowTexts(View::kSession, guest);
    CHECK(with_resync[3] == "Resync from host");
    CHECK(with_resync[2] == "Squadala! v0.11.6");
    SessionView dev;                                  // a developer's build (no kit): as before
    CHECK(RowTexts(dev)[kRowLeave] == "Leave (not in a session)");
}

TEST_CASE("pause tab: the game browser: the rows choose the list and its page, the cells hold its games") {
    Session s;
    CHECK(ActionFor(View::kLan, 0, s) == RowAction::kBrowseLan);
    CHECK(ActionFor(View::kLan, 1, s) == RowAction::kBrowseOnline);
    CHECK(ActionFor(View::kLan, 2, s) == RowAction::kBrowseRefresh);   // (one page)
    CHECK(ActionFor(View::kLan, 3, s) == RowAction::kListRoom);
    CHECK(ActionFor(View::kLan, kRows - 1, s) == RowAction::kBack);
    s.lan_listed = 9;   // (more than a page of cells)
    CHECK(ActionFor(View::kLan, 2, s) == RowAction::kBrowsePage);
    s.online = true;    // (the other list: its own count)
    CHECK(ActionFor(View::kLan, 2, s) == RowAction::kBrowseRefresh);
    s.in_one = true;    // (nobody joins from inside a session)
    CHECK(ActionFor(View::kLan, 0, s) == RowAction::kNone);
    for (RowAction a : {RowAction::kBrowseLan, RowAction::kBrowseOnline, RowAction::kBrowsePage, RowAction::kBrowseRefresh,
                        RowAction::kListRoom}) {
        CHECK(std::string(ActionWords(a)).size() > 3);
    }

    SessionView v;
    auto rows = RowTexts(View::kLan, v);
    CHECK(rows[0] == "Local games (0)");   // (the list the cells show: the module dims the other's row)
    CHECK(rows[1] == "Online games");      // (not asked yet)
    CHECK(rows[2] == "Refresh list");      // (one page: no empty row)
    CHECK(rows[3] == "List my room: Off");
    CHECK(rows[kRows - 1] == "Back");
    auto lines = ListLines(v);
    REQUIRE(lines.size() == static_cast<std::size_t>(kListRows));
    CHECK(lines[0].name == "Game");      // (the header: the columns' names)
    CHECK(lines[0].value == "Players");
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
    v.s.online = true;   // (the relay's list: why there are none, in the first game row)
    v.online_state = "No relay set";
    rows = RowTexts(View::kLan, v);
    CHECK(rows[0] == "Local games (2)");
    CHECK(rows[1] == "Online games (0)");
    CHECK(ListLines(v)[0].name == "Game");
    CHECK(ListLines(v)[1].name == "No relay set");
    v.online_state.clear();
    v.online_rows.assign(10, "kokiri (1/4)");
    v.online_list.assign(10, ListLine{"kokiri (Saria)", "1/4"});
    v.online_list[7] = ListLine{"a room with a very long name indeed (Saria)", "Other version"};
    v.s.online_listed = 10;
    v.s.list_room = true;
    rows = RowTexts(View::kLan, v);
    CHECK(rows[1] == "Online games (10)");
    CHECK(rows[2] == "Next page (1/2)");
    CHECK(rows[3] == "List my room: On");
    lines = ListLines(v);
    CHECK(lines[2].name == "kokiri (Saria)");
    v.s.cell_page = 1;   // (the second page: the 7th to 10th rooms)
    lines = ListLines(v);
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

TEST_CASE("pause tab: a game on this network: its password typed on a row, then Join this game, and Back to the list") {
    Session s;
    CHECK(BackOf(View::kLanGame) == View::kLan);
    CHECK(ActionFor(View::kLanGame, 0, s) == RowAction::kNone);   // (the game itself)
    CHECK(ActionFor(View::kLanGame, 1, s) == RowAction::kTypePassword);
    CHECK(ActionFor(View::kLanGame, 2, s) == RowAction::kTypePassword);   // (no password yet: typing it first)
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
    CHECK(rows[1] == "Password: A to type it");
    CHECK(rows[2] == "Join: type its password");
    CHECK(rows[kRows - 1] == "Back");
    v.password_field = "kokiri";
    v.s.lan_password = true;
    rows = RowTexts(View::kLanGame, v);
    CHECK(rows[1] == "Password: kokiri");
    CHECK(rows[2] == "Join this game");
    v.s.in_one = true;
    rows = RowTexts(View::kLanGame, v);
    CHECK(rows[1] == "In a session already");
    CHECK(rows[2] == " ");
}

TEST_CASE("pause tab: the Session view says why the page's Host or Join did not start, over four rows") {
    SessionView v;
    v.says = "Type the join password the host sent you (every session has one).";
    auto rows = RowTexts(View::kSession, v);
    CHECK(rows[0].rfind("Type the join", 0) == 0);
    CHECK_FALSE(rows[1].empty());
    CHECK(rows[kRows - 1] == "Back");
    std::string all;
    for (int r = 0; r < 4; ++r) all += rows[static_cast<std::size_t>(r)] + " ";
    CHECK(all.find("password") != std::string::npos);
    // (co-op's longest notes fit whole: a run cut this one at "th...")
    v.says = "Your clipboard is empty: copy your friend's answer code first, then press this again.";
    rows = RowTexts(View::kSession, v);
    all.clear();
    for (int r = 0; r < 4; ++r) all += rows[static_cast<std::size_t>(r)] + " ";
    CHECK(all.find("...") == std::string::npos);
    CHECK(all.find("again.") != std::string::npos);
    v.says.clear();   // (nothing said: the view's own lines)
    rows = RowTexts(View::kSession, v);
    CHECK(rows[0] == "Not in a session");
}

TEST_CASE("pause tab: a guest the host's game turned away: the rows say so and why, and to Leave, then join again") {
    SessionView v;
    v.s.in_one = true;   // (co-op is still on until Leave, as co-op's menu says)
    v.turned_away = "the host took you out of the game";
    CHECK(RowTexts(v)[kRowSession] == "Turned away: A says why");
    CHECK(ActionFor(kRowSession, v.s) == RowAction::kViewSession);
    CHECK(ActionFor(kRowLeave, v.s) == RowAction::kConfirmLeave);
    auto rows = RowTexts(View::kSession, v);
    CHECK(rows[0] == "Turned away:");
    std::string all;
    for (int r = 1; r < 4; ++r) all += rows[static_cast<std::size_t>(r)] + " ";
    CHECK(all.find("took you out") != std::string::npos);
    CHECK(all.find("then join again") != std::string::npos);
    v.banned = true;
    rows = RowTexts(View::kSession, v);
    all.clear();
    for (int r = 1; r < 4; ++r) all += rows[static_cast<std::size_t>(r)] + " ";
    CHECK(all.find("join again") == std::string::npos);   // (a ban: joining again cannot work)
    CHECK(RowTexts(View::kJoin, v)[0] == "Turned away: Leave first");
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
    CHECK(p.OnPad(kPadR, now + 16) == 0);
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
        CHECK(p.OnPad(kPadR, now + 16) == 0);
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
