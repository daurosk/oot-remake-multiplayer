// SPDX-License-Identifier: GPL-3.0-or-later
// The pause menu's Multiplayer and Longshot Settings pages (issue 264, piece 4;
// docs/hookshot.md "The pause menu's Multiplayer page"): two pages opened from
// two buttons the pack adds to the game's own System page, so the tab strip
// stays the game's four tabs. The pack adds the layouts, stored hidden
// (tools/puppet_pack/pause_menu.py): the buttons Pa_Button_05 (Longshot
// Settings) and Pa_Button_06 (Multiplayer) of PaSystemMenu_00, and the pages
// Pa_Coop_00 and Pa_Settings_00 (copies of the System page) where a shown page
// sits. Patch 0030 hands the module every menu screen's update
// (hos_coop_game_screen) and asks it which page the mouse's tab strip counts
// (hos_coop_game_pause_page).
//
// The game keeps System as its page while ours shows (agreed on #264): A on one
// of the two buttons, with the menu open and still and nothing over it, shows
// that page in System's place at once. B on the page's rows gives System back
// (the cursor on the button again); L or R gives System back and is the game's
// (on to its next page). While a page shows, the pad is ours but + and - and up
// and down (the game closes the menu, goes to the Minus menu, or moves its
// cursor over our rows), so nothing else reaches the System page under it. The
// menu closing or leaving System, a window over it, or Game Over gives System
// back at once. Works with co-op idle (solo).
//
// Logic only here (no game reads or calls, no log): Page and Probe, driven by
// tests/hookshot/pause_tab_test.cpp. coop_pause_tab_module.cpp reads the game,
// makes the game's calls and logs.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "coop_pause_menu.h"
#include "hos_coop_api.h"

namespace CoopPauseTab {

// The game's screens, by their class's vtable (offsets in main, build e0f3ccfaf50065ae; docs/game-ui.md).
inline constexpr std::uint64_t kPagesVtable = 0x4bebaa8;   // main+: UIPlusMenuScreen (PlusMenu_00, the four pages)
inline constexpr std::uint64_t kStripVtable = 0x4be8aa0;   // main+: UIMenuOverlayScreen (MenuOverlay_00, the tabs)
// Screens that open over the System page or end the game: while one opens, shows or closes, our page is not shown.
inline constexpr std::uint64_t kBlockerVtables[] = {
    0x4befbf8,   // UISystemWindowScreen (the System page's questions)
    0x4bef108,   // UISystemOptionScreen
    0x4bef840,   // UISystemSaveLoadScreen
    0x4beeba0,   // UISystemLoadListScreen
    0x4bef4a0,   // UISystemOverlayScreen (the System page's child windows)
    0x4be91f8,   // MessageDialogScreen
    0x4be3820,   // UIAmiiboWindowScreen
    0x4be7068,   // UIGameOverScreen
};
// Fields read (docs/game-ui.md, "The pause menu"; seen in a run, 2026-11-18):
inline constexpr std::uint64_t kScreenState = 0x1a6;   // u8, every screen: 0, 1 opening, 2 open, 3 closing
inline constexpr std::uint64_t kScreenLayout = 0x28;   // a screen's layout
inline constexpr std::uint64_t kStripTabs = 0x328;     // u32: its Pa_Category_%02d parts (4; 5 with an older pack's tab)
inline constexpr std::uint64_t kStripTabArray = 0x330; // -> its tab objects, one pointer each
inline constexpr std::uint64_t kStripMode = 0x350;     // s32: 1 the Plus menu's tabs, else the Minus menu's
inline constexpr std::uint64_t kStripLit = 0x358;      // s32: the category it lights (0, 3, 4, 5 the Plus pages)
inline constexpr std::uint64_t kMenuManager = 0x4e7cfa8;   // main+: the global holding the menu manager
inline constexpr std::uint64_t kManagerPage = 0x530;   // s32: the Plus menu's current page (a category)
inline constexpr std::uint64_t kManagerAsked = 0x538;  // s32: the page asked for (the step's)
inline constexpr int kSystemPage = 5;                  // the System page's category
inline constexpr int kPlusMode = 1;
inline constexpr int kMapCategory = 2;   // the - menu's Map page's category (its last tab: Key Items, Threads, Map)
// The - menu's tabs of ours (0.11.9, the owner: Multiplayer, Lobby and Mods are tabs right of Map), in order.
inline constexpr int kMinusTabs = 3;
// (the owner, 0.11.9: Character Select's Characters, then Multiplayer, Lobby (the online Find / Host / Join) and Mods)
inline constexpr const char* kMinusTabNames[kMinusTabs] = {"Multiplayer", "Lobby", "Mods"};
inline constexpr int kRows = 5;                        // the page's buttons (Pa_Button_00..04), System's too
// The buttons of the pages made of System's layout with the pack's Longshot Settings button (Pa_Button_05: System's
// last button, and Longshot Settings' sixth category row).
inline constexpr int kButtonRows = 6;
// The game's functions (main+), as its own code calls them:
inline constexpr std::uint64_t kFindPane = 0x76e2b0;   // (layout, "name") -> pane, or 0 (coop_map_page.h's)
inline constexpr std::uint64_t kPaneTable = 0x43c040;  // (layout + 0x40) -> its pane table (kFindPane's first call)
inline constexpr std::uint64_t kPaneByName = 0x43e300; // (table, "name") -> the pane, a part's own (kFindPane gives
                                                       //  a part's inner root instead)
inline constexpr std::uint64_t kSetVisible = 0x20aea0; // (pane, visible): pane +0xa8 bit 0 (as f_802edef0 hides a tab)
// A layout's animation by name or by a path through its parts ("Pa_A/Pa_B/Select": main+0x43c4a0 walks the parts by
// '/', read from main 2026-11-21), stopped at a frame: the title's BtnNum way (coop_title.cpp, run in the game).
inline constexpr std::uint64_t kAnimFind = 0x43d870;      // (layout, "name or path") -> the animation, 0 none
inline constexpr std::uint64_t kAnimPlayer = 0x20;        // an animation's player
inline constexpr std::uint64_t kAnimLastFrame = 0x2d1340; // (player) -> s32: its last frame
inline constexpr std::uint64_t kAnimStopAt = 0x2d2d90;    // (animation, s0 frame): stopped at that frame
inline constexpr std::uint64_t kTabFill = 0xa6ab50;    // (tab, category, enabled): its text and Plus/Minus look
inline constexpr std::uint64_t kTabLight = 0x2ccf60;   // (tab, lit, instant): its CurrentIn / CurrentOut
inline constexpr std::uint64_t kPaneAlpha = 0x2d8ac0;  // (pane, s0 alpha 0..1): a pane's own alpha (+0x74), marked dirty
// A pane's own place's height (+0x70, its x at +0x6c), its place on the screen (+0x4c, +0x50) and its alpha with its
// parents' (+0x78), as read in a run (2026-11-19: the strip's fourth tab and our fifth through the strip's opening).
inline constexpr std::uint64_t kPaneOwnY = 0x70, kPaneScreenX = 0x4c, kPaneScreenY = 0x50, kPaneScreenAlpha = 0x78;
// A null or text pane's second alpha block (+0xb0: its marks at +0, its own alpha at +0x74, so +0x124): the one the
// strip's "text in" animation fades, from 0 to 1 in about 250 ms as the strip opens (seen in a run, 2026-11-19, on the
// fourth tab's N_MenuChangeTextIn_00). kPaneAlpha takes it as its pane.
inline constexpr std::uint64_t kTextAlphaBlock = 0xb0, kTextAlpha = 0xb0 + 0x74;
inline constexpr float kFollowMost = 200.0f;   // the most our tab is moved from its own place to follow the fourth
// (pane): the pane's place marked changed (bit 0x40 of its first word) and its matrices made again, as the game's own
// place setter (main+0x20a3e0: (pane, x, y, z)) ends for a part after storing x and y at +0x6c; nothing for a pane whose
// +0xa8 lacks bit 20. Read from main, 2026-11-19: a place written without it stays where it was drawn.
inline constexpr std::uint64_t kPaneMoved = 0x152af50;
inline constexpr std::uint64_t kTabOpen = 0x208350;    // (tab, 0): the tab's part opened and shown, as the game's fill
                                                       //  ends for each of its tabs (else no text shows)
// (screen, const char* name) -> bit 0: started. A screen's own menu sound by name, as the game's screens start theirs:
// xlink2's searchAndEmit (main+0x43e400, "searchAndEmit(%s)") on the screen's xlink user ([screen+0x198]+8). Seen in
// a run, 2026-11-19: the strip starts "mc_TabToRight" / "mc_TabToLeft" when L and R change its tab, the pages
// "button_on" at a cursor step and "button_decide" at a choice.
inline constexpr std::uint64_t kScreenSound = 0x43e400;
inline constexpr std::uint64_t kMsgRefInit = 0xa66f70; // (ref): {0, 0, -1}, 24 bytes (coop_title.cpp's)
inline constexpr std::uint64_t kMsgRefSet = 0xa6afe0;  // (ref, &"LayoutMsg/...", &"label", 1)
inline constexpr std::uint64_t kPaneText = 0xa67d30;   // (layout, "part/T_Text_00", ref, 1, 0)
// (path "Part/Part/Pane", layout, &layout out) -> the pane's name in the last part's layout (written to out): the game's
// path lookup, the one kPaneText starts with (main+0x43c4a0, read from main 2026-11-19).
inline constexpr std::uint64_t kPathFind = 0x43c4a0;
// (text pane, &{text, length, attributes}, 0): the pane's text, as kPaneText sets each pane it matches
// (main+0x439520): the text and length to its text box's set-string, which keeps them (a pointer and a length, not a
// copy: seen in a run, 2026-11-19); the attributes kept at +0x10.
inline constexpr std::uint64_t kPaneSetMsg = 0x439520;
inline constexpr std::uint32_t kTextPaneKind = 2;   // a pane's kind (+0xa8 bits 22..25): a text box

// Switch pad bits (nn::hid::NpadButton), as hos_coop_pad gets them.
inline constexpr std::uint64_t kPadA = 1ull << 0, kPadB = 1ull << 1, kPadL = 1ull << 6, kPadR = 1ull << 7,
                               kPadZL = 1ull << 8, kPadZR = 1ull << 9, kPadPlus = 1ull << 10, kPadMinus = 1ull << 11,
                               kPadUp = 1ull << 13, kPadDown = 1ull << 15, kPadStickUp = 1ull << 17,
                               kPadStickDown = 1ull << 19;
// A text field's own buttons (nn::hid's NpadButton bits: X, Y, the d-pad's left and right, the left stick's left and
// right), taken from the game while the field is typed into.
inline constexpr std::uint64_t kPadX = 1ull << 2, kPadY = 1ull << 3, kPadLeft = 1ull << 12, kPadRight = 1ull << 14,
                               kPadStickLeft = 1ull << 16, kPadStickRight = 1ull << 18;
// What the game still gets while our page shows: closing the menu, the Minus menu, and up and down, which move the
// System page's focus under ours: the game's own cursor frame (drawn where the focused button is, and our rows sit
// where System's buttons do) moves over our rows, with its sound (seen in a run, 2026-11-18). A is ours: the row is the
// System page's focused button, and B leaves the page. Left and right are not passed (past the page's edge they would
// step the menu).
inline constexpr std::uint64_t kPadPassed = kPadB | kPadPlus | kPadMinus | kPadUp | kPadDown | kPadStickUp | kPadStickDown;

enum class Kind { kOther, kPages, kStrip, kBlocker };
Kind KindOf(std::uint64_t vtable_offset);

// What one update of a screen shows (read by the module from the game).
struct Facts {
    Kind kind = Kind::kOther;
    std::uint64_t screen = 0;
    std::uint64_t layout = 0;          // the screen's layout (+0x28): a new one is a new screen, as a new address is
    int state = 0;
    int tabs = 0, mode = 0, lit = 0;   // the strip's
    int page = -1, asked = -1;         // the pages' (the menu manager's)
};

// A text field on the page's rows. The game has no text entry or keyboard of its own (docs/game-ui.md), so a pad types
// by cycling the letter under the cursor (Up, Down), moving along the text (Left, Right) and deleting (Y); a keyboard
// types into it (its characters, Backspace, Delete, the arrows). A room name: letters, digits, - and _ (the relay
// takes them in any case), 3 to 24. A join password: any printable character but a space, at most 128.
class TextField {
public:
    enum class Kind { kRoom, kPassword, kPort, kAddress };   // (kPort: digits; kAddress: a relay's host[:port])
    explicit TextField(Kind kind) : kind_(kind) {}
    Kind kind() const { return kind_; }
    const std::string& text() const { return text_; }
    std::size_t cursor() const { return cursor_; }
    std::size_t Max() const { return kind_ == Kind::kRoom ? 24 : kind_ == Kind::kPort ? 5 : kind_ == Kind::kAddress ? 64 : 128; }
    // A room name the relay takes (3 or more of its letters); a password: any, none included.
    bool Ready() const { return kind_ == Kind::kRoom ? text_.size() >= 3 : true; }
    // A port typed: 1 to 65535, else 0.
    int PortValue() const;
    bool Allowed(char c) const;
    // The letters a pad cycles through, in order.
    const std::string& Letters() const;
    void Cycle(int dir);      // +1 Up, -1 Down: the letter at the cursor (after the last: a new one, the first letter)
    void Move(int dir);       // -1 Left, +1 Right: within the text, and one past its end while it can still grow
    void Delete();            // the letter at the cursor (past the end: the last one)
    void Backspace();         // the letter before the cursor
    bool Type(std::uint32_t cp);   // a typed character at the cursor (a room name lower-cased): true when taken
    void Clear() { text_.clear(), cursor_ = 0; }
    void Set(std::string text) { text_ = std::move(text), cursor_ = text_.size(); }   // (Cancel: as it was)
    // What its row shows after its label: the text, the letter at the cursor in brackets while it is typed into; a
    // password as * but for that letter. At most `room` bytes, scrolled to keep the cursor in view.
    std::string Shown(bool typing, std::size_t room) const;

private:
    Kind kind_;
    std::string text_;
    std::size_t cursor_ = 0;
};

// The page: when it shows, and what the module does to the game on each screen's update.
// One caller at a time (the module's lock): screen updates on the game's threads, the pad on its input thread.
class Page {
public:
    // What to do to the game at this screen's update (on its thread, with its ctx).
    struct Do {
        bool set_page = false;   // the pages: our page shown (and System hidden) or not (at the strip's update too:
                                 // the module then sets it through the pages screen it knows)
        bool page_shown = false;
        // Which page goes and which comes: 0 System's, 1 the Multiplayer page, 2 the Longshot Settings page. Both of
        // ours open from a button of System's own (Pa_Button_06, Pa_Button_05: the pack's), in System's place, so the
        // game's tabs stay its four and System's is lit for them.
        int page_from = 0, page_to = 0;
        // How the page comes or goes: +1 the page to the right slides in as the one before slides out to the left; -1
        // the other way; 0 at once (now always: our pages open from System's buttons and leave in place).
        int slide = 0;
        bool set_texts = false;  // the pages: our rows' texts (once per screen object)
        // The - menu's tabs of ours (kMinusTabNames, after Map): shown while the - menu's strip is open, which one
        // lit (-1 none: the game's Map, or its other pages), filled once per opening; slide +1 / -1 as R / L moved.
        bool set_minus = false;
        bool minus_shown = false, fill_minus = false;
        int minus_lit = -1, minus_slide = 0;
        bool any() const { return set_page || set_texts || set_minus; }
    };
    Do OnScreen(const Facts& f, std::uint64_t now_ms);
    // Player 1's pad before the game gets it: the buttons the game gets.
    std::uint64_t OnPad(std::uint64_t buttons, std::uint64_t now_ms);

    // Which opening of a pages or strip screen this update is (OnScreen calls it too): a new number (never 0) for a new
    // screen object, a new layout, or the screen opening again (its state 1 after another); the same while it stays.
    // What was looked up or set in the game for an earlier number is stale (a menu reopened at the same address).
    std::uint64_t Track(const Facts& f);
    // Whether the page can be used at all (the module: both screens' panes found as parts, and the game's calls there).
    // Not usable: the page never shows (A stays System's) and MousePage says -2.
    void SetUsable(bool usable);
    bool Usable() const { return usable_; }
    // The Longshot Settings page can be used too (the module: its page and System's Longshot Settings button found, and
    // Longshot's settings there).
    void SetSettingsUsable(bool usable) { settings_ok_ = usable; }
    bool SettingsUsable() const { return settings_ok_; }
    // Which of System's buttons of ours the game's cursor is on now (the module reads it while A is held): 0 the
    // Multiplayer button (Pa_Button_06), 1 the Longshot Settings button (Pa_Button_05), 2 the Customize Link button
    // (Pa_Button_07: the Multiplayer page, opened on Settings and look's Customize page), -1 neither. Its A opens that
    // page.
    void SetSystemButton(int which) { system_button_ = which; }
    // One of our pages was opened (A on its button) or left (B on its rows) since the last call (Which() says which):
    // the module hides rows a moment so the game's cursor lands where it should (it carries its cursor by the
    // buttons' names).
    bool TakeOpened() { return std::exchange(opened_, false); }
    bool TakeLeft() { return std::exchange(left_, false); }
    // The button the page was opened from (0, 1, 2 as SetSystemButton): its cursor goes back to it when the page goes.
    int OpenedFrom() const { return opened_from_; }
    // Opened from the Customize Link button since the last call: the module shows Settings and look's Customize page.
    bool TakeCustomizeOpen() { return std::exchange(customize_open_, false); }
    // Back to System from a view (B on Settings and look opened by the Customize Link button), the cursor on its button.
    void LeaveToButton(const char* why) { Hide(why); left_ = true; }
    // Which of our pages shows (while Shown): 0 Multiplayer, 1 Longshot Settings.
    int Which() const { return which_; }
    // hos_coop_game_pause_page: -1 while our pages can be used (they are in System's place: the mouse's tabs are the
    // game's four, System's page the one shown), -2 while they cannot.
    int MousePage() const { return !usable_ ? -2 : -1; }

    // One of the rows' own views shows (Players, Let in, Join: View): B is ours then and goes back to the five rows
    // (TakeBack), instead of closing the menu. Off whenever the page goes.
    void SetSubView(bool on) { sub_ = on && shown_; }
    bool TakeBack();
    // An A on one of System's buttons of ours came while the page could not show (kept from the game): once.
    bool TakeRefusedA() { const bool r = refused_a_; refused_a_ = false; return r; }
    bool CanShowNow(std::uint64_t now_ms) const { return CanShow(now_ms); }
    void Leave(const char* why) { Hide(why); }   // (back to System at once: the module's way back to the title)

    bool Shown() const { return shown_; }
    // A pressed on our page since the last call (the row is the System page's focused button).
    bool TakeDecide();
    // Why it last went back to System, for the log ("" when none since the last call).
    std::string TakeWhy();
    // The menu sound the module starts at the pages' next update (A on one of System's buttons of ours, or on a row):
    // the game does not start them, the module took the press. nullptr when none.
    const char* TakePagesSound();
    void Sound(const char* name) { pages_sound_ = name; }   // a menu sound for the module's own move (the cells)
    // The tab sound the module starts at the strip's next update (R onto a - tab of ours, L off it: the game's own
    // mc_TabToRight / mc_TabToLeft, which its graph plays on MenuOverlay_00 for its own tabs). nullptr when none.
    const char* TakeStripSound() { return std::exchange(strip_sound_, nullptr); }

    // The - menu's tabs (0.11.9): usable once the module found the pack's three tabs in the strip
    // (SetMinusTabsUsable). R on Map (the - menu's last page, open and still) goes to the first of ours, R and L step
    // among them, L on the first goes back to Map; each with the game's own tab sound. While one shows, the pad is its
    // page's but B, + and - (the game's: the menu closes or changes, which also leaves ours). Which screen draws these
    // tabs' pages is what the R1 probe answers (reports/ui-tabs-0119.md); until then the module keeps them off unless
    // COOP_TEST_MINUS_TABS=1.
    void SetMinusTabsUsable(bool usable) { minus_ok_ = usable; }
    // Ours shown now: 0 Multiplayer, 1 Lobby, 2 Mods; -1 none.
    int MinusTab() const { return minus_tab_; }
    // Our tab's own page (the pack's Pa_CoopMinus_00, the System page's buttons): which of its rows show (bit r: row r,
    // RowTexts(MinusPage) not ""), set by the module for the tab lit. Up and down move the cursor among the rows shown
    // (the game's button_on), A picks the row under it (button_decide): TakeMinusDecide says which, once.
    void SetMinusRows(std::uint32_t shown);
    int MinusRow() const { return minus_row_; }
    int TakeMinusDecide() { return std::exchange(minus_decide_, -1); }
    // The page's views (0.11.9, the owner: one home, everything on the - menu): a row whose action opens a view shows
    // that view on the same page (the module's View; SetMinusInView says one shows). The page has two places for the
    // pad: its rows on the left, and the list on the right (SetMinusLines: which of its lines can be picked, bit k:
    // line k). Right on the rows goes into the list when it has a line to pick; up and down move there, A picks the
    // line (TakeMinusLineDecide), Left or B back to the rows. A list of co-op's menu's own page (Co-op settings, Mods:
    // SetMinusLines' raw) takes the pad's moves as that menu's own events instead (TakeMinusMoves: up, down, left, right,
    // A as kMinusMove*), B back to the rows with that menu's Back (TakeMinusListBack). B on a view's rows steps back a
    // view (TakeMinusBack); B on the tab's own rows is the game's (the menu closes). L and R change the tab from
    // anywhere (the module puts the view back to the tab's own rows then: MinusTabChanged).
    void SetMinusInView(bool in_view) { minus_in_view_ = in_view; }
    void SetMinusLines(std::uint32_t pickable, bool raw);
    // A tab with no rows of its own (Mods: its list is the page) has the list always; B there is the list's Back while
    // it has somewhere to go back to (SetMinusRawBack: a mod's page), else the game's (the menu closes).
    void SetMinusRawBack(bool can) { minus_raw_back_ = can; }
    bool MinusInList() const { return minus_list_; }
    // Another view shows (the module saw it change): the rows' cursor on `row` (a view's own: Co-op settings' page
    // chosen), the list without the pad. MinusEnterList: the list has the pad (A on a Co-op settings row), when it
    // has a line.
    void MinusViewChanged(int row);
    void MinusEnterList();
    int MinusLine() const { return minus_line_; }
    int TakeMinusLineDecide() { return std::exchange(minus_line_decide_, -1); }
    bool TakeMinusBack() { return std::exchange(minus_back_, false); }
    bool TakeMinusListBack() { return std::exchange(minus_list_back_, false); }
    static constexpr std::uint32_t kMinusMoveUp = 1, kMinusMoveDown = 2, kMinusMoveLeft = 4, kMinusMoveRight = 8,
                                   kMinusMoveA = 16;
    std::uint32_t TakeMinusMoves() { return std::exchange(minus_moves_, 0u); }
    // The tab lit changed (or the - menu closed) since the last call: the module's view back to the tab's rows.
    bool MinusTabChanged() { return std::exchange(minus_tab_changed_, false); }

    // The Join view's two fields (the room name, the join password) and which one is typed into (-1: none). While one
    // is, the pad's buttons are all the page's (none reach System) and the keyboard's keys are the field's (Key, Char).
    // The fields: 0 the room name, 1 the join password (Join view, a game's view), 2 the host's new join password,
    // 3 the port when hosting (the details' Router line), 4 the relay's address (the browser's online list), 5 the
    // browser's search, 6 this game's room name when it hosts (the main view's details).
    static constexpr int kFields = 7;
    TextField& Field(int which) {
        return which == 6 ? room_name_ : which == 5 ? search_ : which == 4 ? relay_ : which == 3 ? port_ : which == 2 ? new_password_ : which == 1 ? password_ : room_;
    }
    const TextField& Field(int which) const {
        return which == 6 ? room_name_ : which == 5 ? search_ : which == 4 ? relay_ : which == 3 ? port_ : which == 2 ? new_password_ : which == 1 ? password_ : room_;
    }
    // Typing into a field turns the list on the right into its keys (TypingLines: the game's own Options rows, the
    // game has no keyboard of its own): up and down pick a key, Left and Right the letter on Letter, A presses the key
    // picked, B is Cancel, Y deletes, + is OK, L and R move in the text.
    void StartTyping(int which);
    int Typing() const { return typing_; }
    // A key pressed some other way (the mouse's click on its row): as A on it.
    void PressKey(int key);
    int TypingKey() const { return key_; }        // the key picked (TypeKey)
    char TypingLetter() const;                    // the letter Letter puts in
    // A field typed into ended with OK (or Enter, or +) since the last call: which (-1 none). Cancel (Esc) puts the
    // field back as it was and says none.
    int TakeTyped() { return std::exchange(typed_, -1); }
    bool Key(std::uint32_t vk, bool repeat);   // a Windows virtual-key code while typing: true when the field took it
    bool Char(std::uint32_t cp);               // a typed character while typing

private:
    void TypingPad(std::uint64_t down, std::uint64_t held, std::uint64_t now_ms);
    TextField room_{TextField::Kind::kRoom}, password_{TextField::Kind::kPassword},
        new_password_{TextField::Kind::kPassword}, port_{TextField::Kind::kPort}, relay_{TextField::Kind::kAddress},
        search_{TextField::Kind::kRoom}, room_name_{TextField::Kind::kRoom};
    int typing_ = -1, typed_ = -1;
    int key_ = 0;                // the key picked while typing (TypeKey)
    std::size_t letter_ = 0;     // Letter's letter: its place in the field's Letters()
    std::string typing_was_;   // (the field before typing: Cancel puts it back)
    void EndTyping(bool ok);
    std::uint64_t repeat_since_ = 0, repeat_last_ = 0;
    std::uint64_t pad_ms_ = 0, typing_from_ms_ = 0;   // (the last pad read; when typing began: its first A is not a key's)   // a held direction: when it went down, its last step

    bool CanShow(std::uint64_t now_ms) const;
    bool CanStay(std::uint64_t now_ms) const;
    void AfterStall(std::uint64_t now_ms);   // the screens' times moved on by a whole-game stall's gap
    void ShowAgain(std::uint64_t now_ms);
    // keep: the menu closed with our page shown, so the next opening on System shows it again (as the game opens on
    // the page it closed on).
    void Hide(const char* why, bool keep = false);

    Facts pages_, strip_;
    std::uint64_t pages_ms_ = 0, strip_ms_ = 0, blocked_until_ms_ = 0;
    bool shown_ = false;
    bool again_ = false;          // shown again at the menu's next opening (it closed with our page shown)
    std::uint64_t pad_last_ = 0;
    std::uint64_t call_ms_ = 0;  // the last call of any kind (pad or screen): one later than kStaleMs is the whole
                                 // game stalled (AfterStall)
    std::uint64_t swallow_ = 0;   // buttons the module took, kept from the game until let go
    bool decide_ = false;
    bool refused_a_ = false;
    int slide_ = 0;   // the next set_page's Do::slide
    bool minus_ok_ = false;    // SetMinusTabsUsable
    int minus_tab_ = -1;       // MinusTab
    int minus_set_ = -2;       // the - tabs' state last set (Do::set_minus): -2 none, -1 shown none lit, else lit
    int minus_slide_ = 0;
    std::uint64_t minus_gen_ = 0;   // the strip opening whose - tabs were filled
    int minus_row_ = 0;             // MinusRow
    int minus_decide_ = -1;         // TakeMinusDecide
    std::uint32_t minus_rows_ = 0;  // SetMinusRows
    bool minus_in_view_ = false;        // SetMinusInView
    bool minus_list_ = false;           // MinusInList
    bool minus_raw_ = false;            // SetMinusLines' raw
    bool minus_raw_back_ = false;       // SetMinusRawBack
    int minus_line_ = -1;               // MinusLine
    std::uint32_t minus_lines_ = 0;     // SetMinusLines' pickable
    int minus_line_decide_ = -1;        // TakeMinusLineDecide
    bool minus_back_ = false, minus_list_back_ = false;
    std::uint32_t minus_moves_ = 0;     // TakeMinusMoves
    bool minus_tab_changed_ = false;    // MinusTabChanged
    void MinusLineStep(int dir);
    void MinusTabMoved();
    void MinusStep(int dir);
    bool CanMinus(std::uint64_t now_ms) const;
    std::uint64_t MinusPad(std::uint64_t buttons, std::uint64_t down);
    const char* strip_sound_ = nullptr;
    int which_ = 0;   // our page shown: 0 Multiplayer, 1 Longshot Settings (kept while hidden: the menu opens on it again)
    bool settings_ok_ = false;
    int system_button_ = -1;
    int opened_from_ = 0;
    bool customize_open_ = false;
    bool opened_ = false, left_ = false;
    const char* pages_sound_ = nullptr;
    std::string why_;
    bool usable_ = false;
    bool sub_ = false, back_ = false;
    // What is set in the game, per opening of a screen object (Track; -1: not yet).
    struct Seen {
        std::uint64_t screen = 0, layout = 0, gen = 0;
        int state = -1;
    };
    Seen pages_seen_, strip_seen_;
    std::uint64_t next_gen_ = 0;
    int page_set_ = -1;
    bool texts_set_ = false;
};

// The rows (Pa_Button_00..04): Session, Join code, Players, Let in, Leave. Each A acts on the page itself (hosting,
// a copy, the second A of Leave), or shows one of the rows' own views on the same five rows (View: Session, Players,
// a player, Let in, Join). Only a Paste answer, a refused Host or Join, and co-op that came with the game's start open
// co-op's menu (Hookshot's menu, over the game's pause menu) on its main page, whose note or help says why.
// The game has no text entry or keyboard of its own (docs/game-ui.md, "Frames for a text field"), so a code is pasted.
enum class View { kMain, kPlayers, kLetIn, kJoin, kSession, kPlayer, kPlayerHost, kLan, kLanGame, kPassword, kMenu,
                  kHost };   // (kHost: the Host a game view, 0.11.9; appended: a view is kept as its number)
// Where B (or Back) goes from a view: a player's host controls to that player, a player to the Players view, a game on
// this network to the list of them, else the five rows.
View BackOf(View view);
// The main view's five entries (0.11.9, the owner: "clean, user friendly"): each always says what it opens and always
// opens it (no row that does nothing); what co-op does now is on the list on the right (DetailLines).
enum Row { kRowFind = 0, kRowHost = 1, kRowJoin = 2, kRowPlayers = 3, kRowSettings = 4 };
// The Host view's rows (hosting): the room or code, Let in, the session's details, Stop hosting; the Join view's Leave row
// (in a friend's game). Both Stop and Leave wait for a second A.
inline constexpr int kHostRowStop = 3, kJoinRowLeave = 1;
enum class RowAction {
    kNone,
    kOpenMain,    // co-op's menu, its main page: what co-op does now, Host and Join, Stop and Leave
    kCopyCode,    // the room name (or join code, or invite code) to the clipboard; the row says what was copied
    kAllow,       // the Lobby's /allow: the oldest player who asks to join is let in (its line says who, or nobody)
    kHost,        // host at once, with the kit's settings (its room name, relay, port, approval); refused or failed:
                  // co-op's main page, which says why
    kConfirmLeave,   // Leave or Stop hosting: the row asks for a second A first (kConfirmMs), then co-op stops
    kPasteAnswer,    // the host: a friend's answer code from the clipboard (co-op's menu's Paste answer), its main page
    kCopyAnswer,     // a guest the host could not reach: its answer code to the clipboard (Copy answer), on the row
    // The views (View) and what their rows do:
    kViewPlayers,    // the Players view: who is in the session, one a row
    kViewLetIn,      // the host's Let in view: the oldest who asks, Let in or Refuse
    kViewJoin,       // the Join view: a code from the clipboard, the last game again, or typing one in co-op's menu
    kBack,           // back to the five rows (B does the same)
    kDeny,           // the Lobby's /deny: the oldest player who asks is refused
    kJoinPaste,      // join with the code (or address line) on the clipboard, as co-op's menu's Join page
    kRejoin,         // join the last game again (its code kept in coop.ini), as the Join page's "Join <host> again"
    kViewSession,    // the Session view: what co-op does, the host's lines to copy, why co-op failed
    kCopyLan,        // the host's same-network join line to the clipboard (the menu's Same network row)
    kCopyVpn,        // the host's VPN join line to the clipboard (the menu's VPN row)
    kResync,         // a guest: the host's save again (CoopSave::RequestResync, the Lobby tab's Resync from host)
    kViewPlayer,     // a player's view (the Players view's row): teleport, own mutes; the host's controls
    kMorePlayers,    // the Players view's next page of players
    kHostControls,   // the host's controls for that player: mutes for everyone, Kick, Ban
    kTeleport,       // /teleport to that player, as the Lobby tab's
    kOwnVoice,       // our own mute of their voice, on or off (only in this game)
    kOwnChat,        // our own mute of their chat
    kHostVoice,      // the host: their voice muted for everyone, on or off (the Lobby's server action)
    kHostChat,       // the host: their chat muted for everyone
    kKick,           // the host: out of the game, after a second A (kConfirmMs)
    kBan,            // the host: out for this session, after a second A
    kTypeRoom,       // the Join view: type the room name on the row (A or B ends it)
    kTypePassword,   // the Join view: type the join password on the row
    kJoinRoom,       // the Join view: join that room on the kit's relay, with that password
    kViewLan,        // the games on this network (coop_lan.h): one a row, A joins
    kJoinLan,        // that game's own view (View::kLanGame): its join password typed on a row, then Join this game
    kJoinLanGame,    // the game's view: join it at its address and port, with the password typed
    kMoreLan,        // the next page of games on this network
    kAutoRejoin,     // auto-rejoin on or off (coop_auto_rejoin.h; saved at once)
    kShowPassword,   // the host's Join password view: its join password shown on the details list, or hidden again
                     // (only on A: friends who found the game on this network type it)
    kViewPassword,   // the host's Session view: the Join password view (show it, change it)
    kChangePassword, // the Join password view: a new one typed with the typing keys, then the session's from then on
    kChangePort,     // the details' Router line: the port when hosting typed with the typing keys, hosting again on it
    kTypeRelay,      // the browser's online list with no relay: the relay's address typed with the typing keys, saved
                     // (players who join after need it; players in stay in)
    kTypeSearch,     // the browser's online list's heading: words typed with the typing keys, the rooms that hold them listed
    // The game browser (View::kLan): the games in the cells on the right, the list chosen on the rows.
    kBrowseLan,      // the cells list the games on this network
    kBrowseOnline,   // the cells list the relay's public rooms (coop_room_list.h)
    kBrowsePage,     // the cells' next page
    kBrowseRefresh,  // the list asked again (this network's games forgotten; the relay asked at once)
    kListRoom,       // this game's room on the relay's public list when it hosts, on or off (coop-options.ini
                     // list_room; saved at once)
    kBrowseList,     // the browser's list: the relay's public rooms or this network's games, the other one
    kBrowseFilter,   // the browser's filter, the next one (CoopBrowse: all, open, this version)
    kBrowseSort,     // the browser's order, the next one (CoopBrowse: most players, fewest, name)
    kTypeRoomName,   // this game's room name when it hosts (coop-options.ini relay_room), typed with the typing keys, saved
    kViewMenu,       // Settings and look (View::kMenu): co-op's own menu's Customize, Settings, Commands, Local co-op
    kMenuTab,        // that view's row: its page on the list, and the list has the pad
    kViewHost,       // the Host a game view (View::kHost): start hosting, or the game hosted: its code, Let in, Stop
    kViewLocal,      // Local co-op (0.11.9, the Multiplayer tab's row): Settings and look on its Local co-op page
};
inline constexpr std::uint64_t kConfirmMs = 5000;   // (3 s ran out before a test's second click)
// proto::lobby_mute's bits (CoopLobby's kVoice, kChat; the module checks they match).
inline constexpr std::uint8_t kMuteVoice = 1u << 0, kMuteChat = 1u << 1;
// What co-op is doing, as the rows need it.
struct Session {
    bool hosting = false;   // this game hosts (its server listens)
    bool in_one = false;    // in a session, hosted or joined (or starting one)
    bool code = false;      // hosting, and a room name or a join code is there to give
    bool invite = false;    // hosting with an invite code (no relay room): a friend may send back an answer code
    bool asking = false;    // hosting, and somebody asks to join
    bool wait_host = false; // joining, and the host must take this game's answer code first
    bool by_launcher = false;   // co-op came with the game's start (Host.bat, Join.bat, coop.ini): not started or
                                // stopped here; Session and Leave open co-op's main page, whose help says why
    bool failed = false;    // the last Host or Join did not work (co-op's main page says why)
    bool rejoin = false;    // a last game's code is kept (coop.ini last_invite): the Join view's "Join <host> again"
    bool room_ready = false;    // the Join view's room name is long enough to join (TextField::Ready)
    bool auto_rejoin = false;   // auto-rejoin is on
    bool rejoining = false;     // auto-rejoin tries a dropped connection again now
    int lan_listed = 0;         // the games on this network the view lists
    int lan_page = 0;           // its page (PlayerIndex)
    std::uint32_t lan_joinable = 0;   // bit i: the list's game i can be joined (same version, room for one)
    bool online = false;        // the game browser lists the relay's public rooms (else the games on this network)
    int browse_filter = 0, browse_sort = 0;   // its filter and its order (CoopBrowse::Filter, Sort)
    int online_listed = 0;      // the rooms it lists
    int cell_page = 0;          // the browser's page of cells (kCells a page)
    bool list_room = false;     // hosting lists this game's room on the relay's public list
    bool lan_password = false;  // a game's view: a join password is typed (none: the same network's way in)
    bool has_password = false;  // hosting, and the session's join password is known (the status)
    bool password_shown = false;   // the host's Session view shows it now (its rows' A hides it again)
    bool can_resync = false;    // a guest whose game can take the host's save again (CoopSave::CanResync)
    bool menu_list = false;     // Settings and look: its list (co-op's menu's rows) has the pad
    int listed = 0;         // the Players view: how many players it lists
    int self = -1;          // the Players view: our own place in its list (-1 not listed): no view of our own
    int page = 0;           // the Players view: its page (PlayerIndex)
    int player = 0;         // a player's view: 0 that player left, 1 it is us, 2 another player
};
// The Players view: which player (an index of the list) a row shows on a page, or -1. Up to four fit; more: three a
// page and the fourth row turns the page ("More players").
int PlayerIndex(int listed, int page, int row);
int PlayerPages(int listed);
RowAction ActionFor(int row, const Session& s);
// The row of ours a focused button of the pages screen lies under: System's Pa_Button_00..04 (by the name the focus
// manager's walk gives, CoopModScreens::ButtonName), and the Longshot Settings button (Pa_Button_05: 5); -1 for any
// other.
int RowOfButton(const std::string& name);
// A row of a view (kMain: ActionFor).
RowAction ActionFor(View view, int row, const Session& s);
// A second A (Leave, Kick, Ban) is still waited for: our page shows and the game's cursor is on that row still.
inline bool KeepConfirm(bool shown, int focused_row, int confirm_row) {
    return shown && focused_row >= 0 && focused_row == confirm_row;
}

// --- The mouse over our rows ---
// Longshot's mouse focuses the button under the cursor with the game's focus setter, then presses A. While our page
// shows, that setter leaves System's buttons as they are (seen in a run, 2026-11-19: "the focus to Pa_Button_04 (1)",
// then "row 3 chosen"): a click on Back chose the row the cursor was on. The pad's up and down do move the cursor, so
// the page follows the mouse itself: the module reads the cursor and the rows' boxes each pad read, MouseRows steps
// the game's cursor to the row under the mouse with the pad's up and down, and an A just after a click on a row is
// that row's.

// A row's box in the layout's units (CoopModScreens::HitBox); shown false when it is not on the screen.
struct RowBox {
    bool shown = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
using RowBoxes = std::array<RowBox, kButtonRows>;

// A client pixel in the layout's units: the game's 3840 x 2160 picture fit in a w x h client area about its centre,
// y up (as Longshot's mouse takes it).
std::pair<float, float> LayoutPoint(int px, int py, int w, int h);

// The row whose box holds (x, y), -1 for none. Boxes one above the other meet halfway across a gap under 240 units, and
// the column's ends grow by as much (no dead line between two rows, as Longshot's mouse takes the game's lists).
int RowAt(const RowBoxes& boxes, float x, float y);

class MouseRows {
public:
    static constexpr std::uint64_t kClickMs = 400;     // an A this long after a click on a row is that row's
    static constexpr int kOffRows = -2;                // Clicked: a press on the page off our rows (its A chooses none)
    static constexpr std::uint64_t kStepWaitMs = 250;  // a step not seen as the cursor's move: the next after this
    static constexpr std::uint64_t kGiveUpMs = 2000;   // the cursor not there by then: let be

    struct In {
        std::uint64_t now_ms = 0;
        bool inside = false;   // the cursor over the game's client area, the game's window in front
        float x = 0, y = 0;    // where (LayoutPoint)
        bool left = false;     // the left button held
        bool pad = false;      // the player's own up or down this read (the mouse's steps stop)
        int focused = -1;      // the game's cursor's row (-1 not known)
        RowBoxes boxes{};
    };

    // The pad bits to add for the game this read: a step (up or down, one read held, one let go) toward the row the
    // mouse moved onto or clicked.
    std::uint64_t Step(const In& in);
    // The row clicked (pressed on) in the last kClickMs, kOffRows for a press on the page off every row (Longshot's A
    // for a button that is none of ours), -1 none; Take forgets it (one A each click).
    int Clicked(std::uint64_t now_ms) const;
    int TakeClicked(std::uint64_t now_ms);
    void Reset();

private:
    bool seen_ = false, left_ = false, held_ = false;
    float x_ = 0, y_ = 0;
    int target_ = -1, clicked_ = -1, step_from_ = -1;
    std::uint64_t since_ = 0, clicked_ms_ = 0, step_ms_ = 0;
};

// The game's cursor kept on the Multiplayer page's shown rows. The game walks its cursor over every button of the
// pages screen, hidden ones too: our rows with nothing on them (ApplyBlankRows) and System's own buttons under our page
// (Pa_Button_05 and 06, below Back). A tester's run (2026-11-19) had the cursor on those, out of sight: an A there was
// no row's ("row 5 chosen: nothing"), and the mouse's steps to Back and the one Up off a row just hidden undid each
// other, so the cursor went between the first row and Back without end. Now the cursor on a hidden button goes on the
// way it came (a press each step, one read held and one let go), and back up from past our rows; at the end of the
// rows (the cursor did not move) the other way.
class RowSkip {
public:
    static constexpr std::uint64_t kStepWaitMs = 150;   // a step not seen as the cursor's move: the next after this
    struct In {
        std::uint64_t now_ms = 0;
        int focused = -1;        // the game's cursor's button as a row (Pa_Button_NN: NN), -1 none or another
        bool hidden = false;     // ... and that button is not shown
        int rows = kRows;        // the page's rows (a higher row is System's own)
        bool up = false, down = false;   // the player's own up or down this read
        bool mouse_up = false, mouse_down = false;   // the mouse's step this read (MouseRows)
    };
    // The pad bits to add for the game this read (kPadUp, kPadDown or none).
    std::uint64_t Step(const In& in);
    // Whether the cursor is on a hidden button now (the last Step's): the mouse's own steps wait meanwhile.
    bool Skipping() const { return skipping_; }
    void Reset();

private:
    int dir_ = -1;   // the way the cursor went last (-1 up, +1 down)
    bool held_ = false, skipping_ = false;
    int from_ = -1;
    std::uint64_t step_ms_ = 0;
};
// --- The pages' slide (Page::Do::slide) ---
// The game slides its pages a page's width apart (N_MenuChange_01's 4280) when L or R changes the page: measured in a
// run (2026-11-19, the strip's own x each update from Items to System), 4280 in about 430 ms, a fast ease-out with no
// fade: 18% at the first update, half by 65 ms, 92% by 250 ms. The module moves our page and System's the same way.
inline constexpr float kPageWidth = 4280.0f;
inline constexpr std::uint64_t kSlideMs = 433;
// How far a slide is (0 to 1) `ms` after it began: the game's measured curve, between its samples a straight line.
float SlideProgress(std::uint64_t ms);
// The pages' own x offsets at a slide's progress p (0..1): {ours, System's}, added to their own places. dir +1: ours
// from the right to its place, System from its place to the left; dir -1: back.
std::pair<float, float> SlideOffsets(int dir, float p);

// What the rows say, in the game's own text panes (the module writes them through the pack's placeholder messages,
// Coop_Live_NN, in LayoutMsg/PlusMenu_00): the session's state on the rows themselves.
// A row of the game browser's list (View::kLan, ListLines): its left text and its right one; both "" for a row not shown.
struct ListLine {
    std::string name, value;   // the row's left text and its right one; both "" for a row not shown
    RowAction act = RowAction::kNone;   // an A on this line of the details (their Auto-rejoin), else nothing
    bool arrows = false;       // its value between the Options row's own arrows (N_Content_00): Left and Right change it
};

// --- Typing with a pad: the list's rows are the keys ---
// The game has no text entry and no keyboard of its own (docs/game-ui.md, "Frames for a text field"; a grid of its
// buttons would take some 70 copies of PaCommonBtnThin_00), so while a field is typed into the list on the right, the
// game's own Options rows (Pa_List_00..06), shows: the header, the field's name and its text (the letter at the cursor
// in brackets, a password as stars), then the keys: Letter (Left and Right pick it between the row's arrows, A puts it
// in at the cursor), Delete (the letter before the cursor), OK and Cancel. The key picked bright, the others dimmed, as
// the browser's games. The PC keyboard types into the field as before.
enum class TypeKey { kLetter, kDelete, kOk, kCancel };
inline constexpr int kTypeKeys = 4;
// The field's name on the header: 0 the room name, 1 the join password, 2 the host's new join password.
const char* TypingTitle(int which);
// The list's rows (kListRows) while `f` is typed into: the header, then the keys (TypeKey order), the rest not shown.
std::vector<ListLine> TypingLines(int which, const TextField& f, char letter);

struct SessionView {
    Session s;
    bool connecting = false;     // starting a server, or joining (not in yet)
    std::string room;            // hosting: the room name (else a join code) friends type, "" none yet
    int players = 0;             // in the session, us included (0: not in one)
    std::vector<std::string> asking;   // hosting: who asks to join, oldest first
    bool confirm_leave = false;  // the Leave row waits for its second A
    std::vector<std::string> names;   // the Players view: who is in the session, by number ("(you)", "(host)" added)
    std::string rejoin_host;          // the Join view: the last game's host ("" its name is not known)
    std::string note;                 // what the last copy on the page did, a few seconds (CoopPause::PageNote)
    std::string host_name;            // a guest: the host's name ("" not known)
    std::string why;                  // co-op's last problem or hint (its status message), "" none
    int problem = 0;                  // ... as its number (HOS_COOP_PROBLEM_*), 0 none: the known ones in words of ours
    bool port_open = false;           // hosting: the router forwards the port (UPnP, PCP or NAT-PMP)
    int port = 0;                     // hosting: the port the game listens on (0 not known)
    std::string player_name;          // a player's view: who
    // 0.11.6 (owner OK, 2026-11-19): this kit's release, "Squadala! v0.11.6" (CoopWindowBrand::TitleOf), "" none
    // (a developer's build). The details list's last line (DetailLines), "Release".
    std::string version;
    std::uint8_t own_mutes = 0, host_mutes = 0;   // a player's view: kMuteVoice / kMuteChat bits
    bool confirm_kick = false, confirm_ban = false;   // the host's Kick or Ban waits for its second A
    bool confirm_click = false;   // the first was a click: "Click again", not "A again"
    // The Join view's fields: their rows' words (TextField::Shown) and whether a relay is set to join a room through.
    std::string room_field, password_field;
    bool room_ready = false, relay_set = false;
    bool join_address = false;   // the Join view's first field holds an address (a . or a :), not a room name
    int join_port = 0;           // ... the port it joins on: typed after a colon, else the default (27510)
    std::vector<std::string> lan_rows;    // the games on this network, as their rows say them (CoopLan::RowText)
    std::vector<std::string> online_rows; // the relay's public rooms, as their cells say them (CoopRoomList::CellText)
    std::vector<ListLine> lan_list, online_list;   // the same, as the browser's list shows them (name, players)
    std::string online_state;             // the browser's online list when it has no rooms: why ("" asking)
    std::string search;                   // the browser's search, as typed ("" every game)
    bool browse_all_out = false;          // games were heard, but the search or the filter keeps none
    std::string room_name;                // this game's room name when it hosts ("" none: a join code)
    int cell = -1;                        // the browser's cell picked with the pad or the mouse (-1: the rows')
    std::string lan_game;                 // a game's view: the game picked, as its row said it
    std::string turned_away;              // a guest the host's game turned away (kicked, banned, full: the Lobby's
                                          // refusal, its short reason), "" not; Leave, then join again
    bool banned = false;                  // (by the host's ban: joining again cannot work)
    std::string host_password;            // the host's Session view: the join password, once A asked for it
    std::string says;                     // the Session view: why the page's last Host or Join did not start
                                          // (CoopPause::PageSays), while that view shows after it; "" none
    int typing = -1;                  // the field typed into (0 the room, 1 the password), -1 none
    // While any field is typed into (the Join view's, a game's view's, the host's new password): the list's rows as its
    // keys (TypingLines) and the key picked; empty and -1 otherwise.
    std::vector<ListLine> typing_lines;
    int typing_key = -1;
    // Settings and look (View::kMenu): co-op's menu's page as it shows it (CoopPause::NativeView), the row of the view
    // that chose it (kMenuTabNames), the first of its rows on the list
    CoopPauseMenu::View menu;
    int menu_tab = 0, menu_top = 0;
};
// The most a row's text holds: the System page's button fits about 24 of the game's letters (seen in a run,
// 2026-11-19: "Leave (not in a session)" fills it, 31 letters ran past both edges).
inline constexpr std::size_t kRowChars = 24;
std::vector<std::string> RowTexts(const SessionView& v);

// The - menu's tabs' own pages (0.11.9; the owner: Lobby = the online Find / Host / Join, Local co-op folded into
// Multiplayer). Each row always opens something; a row with nothing to open is not shown (""). Which screen draws them
// is the R1 probe's answer (the module's part); these are their rows.
// - Multiplayer: 0 Players, 1 Co-op settings, 2 Local co-op, 3 your game or session (its details, Stop or Leave there),
//   shown only in one.
// - Lobby: 0 Find a game, 1 Host a game (or Your game), 2 Join a game (or Your session, Copy answer code).
// - Mods: the Mods tab's own page (coop_mods_tab.h), not rows of these.
enum class MinusPage { kMultiplayer = 0, kLobby = 1, kMods = 2 };
std::vector<std::string> RowTexts(MinusPage page, const SessionView& v);
RowAction ActionFor(MinusPage page, int row, const Session& s);
// A view's rows (kMain: RowTexts). A row with nothing on it is "": the module does not show it (but while the game's
// cursor is on it).
std::vector<std::string> RowTexts(View view, const SessionView& v);

// --- The game browser (View::kLan): a list of the games on the right of the page, as a server browser ---
// The page's right half held System's controller picture (hidden on ours): the pack puts kListRows of the game's own
// Options rows there (PaOptionBtnLevel_00: a name on the left, a value on the right), Pa_List_00..06
// (tools/puppet_pack/pause_menu.py), stored hidden. The first is the list's header (its columns' names), the others a
// page of kCells games: the game and its host on the left, its players on the right. The rows on the left choose the
// list (this network's games, or the relay's public rooms) and its page. Right (or the mouse, or A on the list shown)
// goes into the list, up and down move in it, Left or B back to the rows; A on a game opens its view (a game on this
// network: its password, then Join; a room: the Join view with its name). ("Cells" below are the list's game rows.)
inline constexpr int kCells = 6, kCellColumns = 1, kListRows = kCells + 1;
inline constexpr std::size_t kListNameChars = 30, kListValueChars = 16;   // (the row's left box is 908 units, its right 682)
// The list's rows for this page: the header, then a page of games ("" rows not shown). With nothing listed, the
// first game row says why.
std::vector<ListLine> ListLines(const SessionView& v);
// A relay's room kept by the online list's search: the search's letters in its name or its host's, any case ("": all).
bool RoomMatches(const std::string& room, const std::string& host, const std::string& search);
// The same list on the main view and the Session view: the session's details (a header, what it is, then name and
// value lines), co-op's problem or hint as lines of text (no value: their value box not shown), and the release last.
// None on the other views. The rows keep what can be done.
std::vector<ListLine> DetailLines(View view, const SessionView& v);
// What a game's right column says: "2/4", "Full", "Other version" (it cannot be joined from here).
std::string ListValue(int players, int max, bool same_version);
// What a room's left column says: the name its host listed it under (the room name unless they picked one), with the
// room name after it when that is another: "Saria's forest (forest-gang)".
std::string ListName(const std::string& shown, const std::string& room);

// --- Settings and look (View::kMenu): co-op's own menu on the page ---
// Hookshot's menu (the overlay) has co-op's look and settings: Customize Link's part colours, Co-op Settings' name,
// Link's look, voice, controller... A tester asked for all of them in the game's own menu. The view's rows are four of
// that menu's pages (and Back); the list on the right shows the page's rows, as the overlay does (the same model,
// coop_pause_menu.h, so the two never differ): Right or A on a row goes into the list, up and down move in it, Left and
// Right change a choice, A does a row (a name typed with the typing keys on the list, a colour's mixer), B back out.
inline constexpr int kMenuTabs = 4;
inline constexpr std::array<const char*, kMenuTabs> kMenuTabNames{"Customize Link", "Co-op settings", "Commands",
                                                                   "Local co-op"};
CoopPauseMenu::Page MenuTabPage(int tab);
// Its list: seven rows under the header with a pack whose page has the eighth list row (Pa_List_07, as Longshot
// Settings' seven a page), else six.
inline constexpr int kMenuMost = kCells + 1;
// The list for co-op's menu's page: its header (the page, "< 2 of 5 >" with more rows than the list holds), then
// `per_page` of its rows from `top` (kept so the selected row shows while the list has the pad). `lines` always
// kMenuMost + 1 (those past per_page empty). `rows`: the menu's row on each line (-1 none, the header's too);
// `selectable`: a line the pad and the mouse can pick (not a heading or a note); `picked`: the line of the menu's
// selected row while the list has the pad (-1 none), counted from the first under the header.
struct MenuList {
    std::vector<ListLine> lines;
    std::array<int, kMenuMost + 1> rows{};
    std::array<bool, kMenuMost + 1> selectable{};
    int top = 0;
    int picked = -1;
};
MenuList MenuLines(const CoopPauseMenu::View& menu, int tab, int top, bool list, int per_page = kCells);
// The heading a row is under (a part's group on Customize Link: "Tunic", "Hylian Shield"; in a colour's mixer its
// own heading, "Hylian Shield: Face"), "" none.
std::string MenuHeadingOf(const CoopPauseMenu::View& menu, int row);
// What a row's value box says: a choice between its arrows ("< Red >"), a switch's On or Off, an action's value.
std::string MenuValue(const CoopPauseMenu::Row& r, bool typing);

// --- Longshot Settings (System's last button): Longshot's own options, from its Insert menu ---
// The page's five rows are the categories, Longshot's menu's tabs (its Debug and NFC tabs, its buttons such as Test
// microphone, and the FPS counter's developer rows stay in that menu). The list on the right, the same Options rows as
// the game browser's, shows a category's settings under a header (the category, "Page 1 of 4"), kSettingsPerPage a
// page: the name on the left, the value between the arrows on the right. The rows choose the category (the game's
// cursor on them); Right or A goes into its list, up and down move in it (turning the page at its ends), Left and Right
// change the value, A takes the next one (of a few choices), B gives the pad back to the rows. A change goes through
// Longshot as its own menu's Left and Right do (clamped, saved, a restart noted).
// Seven a page with a pack whose page has an eighth list row (Pa_List_07: tools/puppet_pack/pause_menu.py ROWS, a
// tester: "it fits"), else six (kListRows less the header): the module says which (`per_page` below).
inline constexpr int kSettingsPerPage = 6;
inline constexpr int kSettingsPerPageMost = 7;
inline constexpr int kSettingListRows = kSettingsPerPageMost + 1;   // (the header and the most a page)
inline constexpr int kSettingRows = 6;   // (the five rows and the Longshot Settings button's copy)
inline constexpr std::array<const char*, kSettingRows> kSettingTabs{"Graphics", "Audio", "Gameplay", "Controls", "Motion", "Debug"};
struct SettingRow {
    std::string key, name, text, tab;   // its line in longshot_options.ini, what the menu calls it, its value's words
    int value = 0, lo = 0, hi = 0, step = 1;
    bool restart = false;   // it applies at the next start
    bool fixed = false;     // nothing to choose on this PC
};
// A category's settings (kSettingTabs[category]: their tab in Longshot's menu), in Longshot's order.
std::vector<SettingRow> SettingsOf(const std::vector<SettingRow>& all, int category);
int SettingPages(int count, int per_page = kSettingsPerPage);
// A page of a category's settings as the list's rows: the header (`title`, and "Page 2 of 4" when there are more),
// then the settings: the name ("*" after a restart one), the value's words.
// kSettingListRows lines, the ones past `per_page` + 1 empty.
std::vector<ListLine> SettingLines(const std::vector<SettingRow>& rows, int page, const std::string& title,
                                   int per_page = kSettingsPerPage);
// The value a press asks for: dir -1 Left, +1 Right (a step, kept within lo..hi), 0 A (the next, the first after the
// last, for a setting of at most kCycleChoices choices: a slider such as Volume is never sent from its end back to its
// start by A or a click on the row). The same value back when nothing would change (a fixed setting, the end of its
// range, A on a slider).
inline constexpr int kCycleChoices = 8;
int SettingNext(const SettingRow& r, int dir);
// The cursor moved up (-1) or down (+1) in a category's list of `count`: across its pages, staying at its ends. `row`
// is the setting's place on its page (0 to kSettingsPerPage - 1; the list's row under the header is row + 1).
void SettingMove(int count, int dy, int* page, int* row, int per_page = kSettingsPerPage);
// The games the browser lists now (the chosen list's), and its pages.
// Row 199: the strip's last update says the menu is up: open or opening (state 1, 2) and seen within kMenuUpMs.
inline constexpr std::uint64_t kMenuUpMs = 200;
bool MenuUpFrom(int strip_state, std::uint64_t seen_ms, std::uint64_t now_ms);
// Character Select's Customize screen while its own state reads open (1, 2): whether co-op takes it as open. Lane 062917
// (0.11.9): his layout reads open again for about a second while the - menu closes (the strip at 3, closing), and an
// opening taken there kept his rows' keys from the field. So an opening is taken only with the menu up, and only once the
// strip has updated since the edge (`edge_ms`, when his state was first seen open; a frame's strip update may come after
// his screen's, with the strip's state of the frame before); an open screen closes when the menu is no longer up.
enum class SnailEdge { kWait, kOpen, kClose };
SnailEdge SnailEdgeFrom(bool open, std::uint64_t edge_ms, int strip_state, std::uint64_t strip_seen_ms, std::uint64_t now_ms);
// One update ahead of `now`, from its step since `prev`, and not past `limit` when it heads for it. The module reads
// the fourth tab's place and fades before the strip's own update, so they are one update old: the Character tab takes
// them one update ahead to move with the fourth, not a frame after it.
float Ahead(float prev, float now, float limit);

int BrowseListed(const Session& s);
int CellPages(int listed);
// The list's index of a cell on the browser's page, -1 none.
int CellIndex(const Session& s, int cell);
// A cell moved by (dx, dy) (columns, rows) among `shown` cells: the cell it reaches, kRowsCell when Left leaves the left
// column (back to the rows), or the same cell at an edge.
inline constexpr int kRowsCell = -1;
int CellMove(int cell, int dx, int dy, int shown);
// The cell whose box holds (x, y) (the layout's units, LayoutPoint), -1 none.
int CellAt(const std::array<RowBox, kCells>& boxes, float x, float y);
const char* ActionWords(RowAction a);   // for the log

// The log lines a run needs (the first step's probe, kept): a screen object's first update, its updates over its
// first second, and every change of what it shows. Pages and strip only.
// The - menu research (ui-tabs-0119, COOP_TEST_SCREEN_PROBE=1): every screen that updates, by its vtable's offset in main,
// and the menu manager's Plus and Minus categories, logged when a screen is first seen, when its state changes, and
// when the categories change; a screen not seen for kCensusGoneMs is said gone. Log only: nothing in the game is set.
class Census {
public:
    static constexpr std::uint64_t kCensusGoneMs = 1500;
    struct Menu {
        int plus = -1, minus = -1, minus_on = -1;   // manager +0x530, +0x52c, +0x53c
        bool operator==(const Menu& o) const { return plus == o.plus && minus == o.minus && minus_on == o.minus_on; }
    };
    // One screen update: the lines to log (none for most).
    std::vector<std::string> Seen(std::uint64_t vtable_offset, int state, const Menu& menu, std::uint64_t now_ms);

private:
    struct Screen {
        std::uint64_t vtable = 0;
        int state = -1;
        std::uint64_t last_ms = 0;
    };
    std::vector<Screen> screens_;
    std::optional<Menu> menu_;
};

class Probe {
public:
    std::vector<std::string> Seen(const Facts& f, std::uint64_t now_ms);

private:
    struct Last {
        std::uint64_t screen = 0;
        Facts facts;
        std::uint64_t first_ms = 0;
        unsigned updates = 0;
        bool counted = false;
    };
    Last pages_, strip_;
};

// One screen update of ours on this thread (OnScreen): the game calls it makes (under the module's locks) may run a
// screen update again on the same thread, which is skipped (Nested), never a second lock of a mutex this thread holds.
class ScreenGuard {
public:
    ScreenGuard();
    ~ScreenGuard();
    ScreenGuard(const ScreenGuard&) = delete;
    ScreenGuard& operator=(const ScreenGuard&) = delete;
    bool Nested() const { return nested_; }

private:
    bool nested_;
};

// The module's part (coop_pause_tab_module.cpp).
void Init(const HosCoopApi* api);
// hos_coop_game_screen: every menu screen's update, on the game's threads; ctx may call() (patch 0030).
void OnScreen(void* ctx, std::uint64_t screen) noexcept;
// hos_coop_pad, first (coop_pad_order.h): what the game and the mods get (sticks zeroed while our page shows).
std::uint64_t OnPad(std::uint64_t buttons, std::int32_t stick[4]) noexcept;
// A join made with a file loaded connected: the game goes back to its title screen by itself, as the System page's
// Title Screen and its question's Title Screen would (the pause menu opened on System if it is not, the cursor stepped
// to Title Screen, A, then the question answered), so the host's save is loaded from there. The player's buttons wait
// meanwhile; it gives up after kToTitleMs. Any thread.
inline constexpr std::uint64_t kToTitleMs = 25000;
void ToTitle() noexcept;
bool GoingToTitle() noexcept;
// A test's view of the page (LICHII_COOP_TEST_STATE=1, CoopPause::Tick writes it): "key=value" words, one line.
std::string TestState();
// The System page's question's screen (UISystemWindowScreen: "Return to the title screen?").
inline constexpr std::uint64_t kSystemWindowVtable = 0x4befbf8;
// hos_coop_game_pause_page: Page::MousePage (-1 usable, -2 cannot be used).
// Row 199: the game's pause menu is up (the + or the - menu, any page, a mod's page such as Character Select's
// included: the tab strip shows on every one of them), as of the strip's last update. Any thread. Co-op's minimap
// marks and player blocks wait while it is (the Map page draws its own).
// The - menu research with COOP_TEST_SCREEN_PROBE=1 on a game without patch 0030: the screens patch 0018 says ran in
// the last 100 ms, through the same census (any thread; nothing without the variable).
void CensusFromMenuState() noexcept;
bool GameMenuUp() noexcept;
// The pause menu's model actors (the menu manager's object, its four slots: Link on Equipment, and the picture Character
// Select's Customize screen shows), 0 for an empty slot or with no menu. Any thread (reads only). The eyes' and hair's
// colours are each actor's own material values, so the picture shows them only when they are dressed on these too.
struct MenuActor {
    std::uint64_t actor = 0;
    std::uint32_t id = 0;   // its slot's id: another actor made at the same address has another
};
std::array<MenuActor, 4> MenuActors() noexcept;
int MousePage() noexcept;
// The mouse in the game's window (its client area, 0..1 each way) and the left button, when the window is in front:
// the title's mouse reads it too (coop_title.cpp).
bool ClientCursor(float* fx, float* fy, bool* left) noexcept;
// The same in the game's layout units (as the buttons' hit boxes: the centre 0,0, y up).
bool LayoutCursor(float* x, float* y, bool* left) noexcept;
// The keyboard while one of the page's fields is typed into (hos_coop_key, hos_coop_char; KeysTaken: the game gets
// none of its keys then): true when the field took it.
bool Key(std::uint32_t vk, bool repeat) noexcept;
bool Char(std::uint32_t cp) noexcept;
bool KeysTaken() noexcept;

}  // namespace CoopPauseTab
