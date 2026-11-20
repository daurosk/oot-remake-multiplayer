// SPDX-License-Identifier: GPL-3.0-or-later
// The pause menu's Multiplayer tab (issue 264, piece 4; docs/hookshot.md "The
// pause menu's Multiplayer tab"): a fifth tab after System in the game's own
// pause menu, and its page. The pack adds the layouts, both stored hidden
// (tools/puppet_pack/pause_menu.py): the tab Pa_Category_04 on the tab strip,
// the page Pa_Coop_00 (a copy of the System page) where a shown page sits.
// Patch 0030 hands the module every menu screen's update (hos_coop_game_screen)
// and asks it which page the mouse's tab strip counts (hos_coop_game_pause_page).
//
// The game keeps System as its page while ours shows (agreed on #264): R or ZR
// on System, with the menu open and still and nothing over it, shows our page
// over System's and lights the fifth tab; L or ZL gives System back. While it
// shows, the pad is ours but B, + and - (the game closes the menu, or goes to
// the Minus menu, as from any page), so nothing reaches the System page under
// it. The menu closing or leaving System, a window over it, or Game Over gives
// System back at once. Works with co-op idle (solo).
//
// Logic only here (no game reads or calls, no log): Page and Probe, driven by
// tests/hookshot/pause_tab_test.cpp. coop_pause_tab_module.cpp reads the game,
// makes the game's calls and logs.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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
inline constexpr std::uint64_t kStripTabs = 0x328;     // u32: its Pa_Category_%02d parts (5 with the pack's tab)
inline constexpr std::uint64_t kStripTabArray = 0x330; // -> its tab objects, one pointer each
inline constexpr std::uint64_t kStripMode = 0x350;     // s32: 1 the Plus menu's tabs, else the Minus menu's
inline constexpr std::uint64_t kStripLit = 0x358;      // s32: the category it lights (0, 3, 4, 5 the Plus pages)
inline constexpr std::uint64_t kMenuManager = 0x4e7cfa8;   // main+: the global holding the menu manager
inline constexpr std::uint64_t kManagerPage = 0x530;   // s32: the Plus menu's current page (a category)
inline constexpr std::uint64_t kManagerAsked = 0x538;  // s32: the page asked for (the step's)
inline constexpr int kSystemPage = 5;                  // the System page's category
inline constexpr int kPlusMode = 1;
inline constexpr int kRows = 5;                        // the page's buttons (Pa_Button_00..04), System's too
// The game's functions (main+), as its own code calls them:
inline constexpr std::uint64_t kFindPane = 0x76e2b0;   // (layout, "name") -> pane, or 0 (coop_map_page.h's)
inline constexpr std::uint64_t kPaneTable = 0x43c040;  // (layout + 0x40) -> its pane table (kFindPane's first call)
inline constexpr std::uint64_t kPaneByName = 0x43e300; // (table, "name") -> the pane, a part's own (kFindPane gives
                                                       //  a part's inner root instead)
inline constexpr std::uint64_t kSetVisible = 0x20aea0; // (pane, visible): pane +0xa8 bit 0 (as f_802edef0 hides a tab)
inline constexpr std::uint64_t kTabFill = 0xa6ab50;    // (tab, category, enabled): its text and Plus/Minus look
inline constexpr std::uint64_t kTabLight = 0x2ccf60;   // (tab, lit, instant): its CurrentIn / CurrentOut
inline constexpr std::uint64_t kPaneAlpha = 0x2d8ac0;  // (pane, s0 alpha 0..1): a pane's own alpha (+0x74), marked dirty
// A pane's own place's height (+0x70, its x at +0x6c), its place on the screen (+0x4c, +0x50) and its alpha with its
// parents' (+0x78), as read in a run (2026-11-19: the strip's fourth tab and our fifth through the strip's opening).
inline constexpr std::uint64_t kPaneOwnY = 0x70, kPaneScreenY = 0x50, kPaneScreenAlpha = 0x78;
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
// System page's focused button. Left and right are not passed (past the page's edge they would step the menu).
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
    enum class Kind { kRoom, kPassword };
    explicit TextField(Kind kind) : kind_(kind) {}
    Kind kind() const { return kind_; }
    const std::string& text() const { return text_; }
    std::size_t cursor() const { return cursor_; }
    std::size_t Max() const { return kind_ == Kind::kRoom ? 24 : 128; }
    // A room name the relay takes (3 or more of its letters); a password: any, none included.
    bool Ready() const { return kind_ == Kind::kRoom ? text_.size() >= 3 : true; }
    bool Allowed(char c) const;
    // The letters a pad cycles through, in order.
    const std::string& Letters() const;
    void Cycle(int dir);      // +1 Up, -1 Down: the letter at the cursor (after the last: a new one, the first letter)
    void Move(int dir);       // -1 Left, +1 Right: within the text, and one past its end while it can still grow
    void Delete();            // the letter at the cursor (past the end: the last one)
    void Backspace();         // the letter before the cursor
    bool Type(std::uint32_t cp);   // a typed character at the cursor (a room name lower-cased): true when taken
    void Clear() { text_.clear(), cursor_ = 0; }
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
        bool set_texts = false;  // the pages: our rows' texts (once per screen object)
        bool set_tabs = false;   // the strip: the fifth tab shown or not, lit or not; System's tab lit or not
        bool tab_shown = false, tab_lit = false, fill_tab = false;
        bool any() const { return set_page || set_texts || set_tabs; }
    };
    Do OnScreen(const Facts& f, std::uint64_t now_ms);
    // Player 1's pad before the game gets it: the buttons the game gets.
    std::uint64_t OnPad(std::uint64_t buttons, std::uint64_t now_ms);

    // Which opening of a pages or strip screen this update is (OnScreen calls it too): a new number (never 0) for a new
    // screen object, a new layout, or the screen opening again (its state 1 after another); the same while it stays.
    // What was looked up or set in the game for an earlier number is stale (a menu reopened at the same address).
    std::uint64_t Track(const Facts& f);
    // Whether the page can be used at all (the module: both screens' panes found as parts, and the game's calls there).
    // Not usable: the page never shows (R and A stay System's) and the mouse keeps the fifth tab out.
    void SetUsable(bool usable);
    bool Usable() const { return usable_; }
    // hos_coop_game_pause_page: 4 while our page shows, -1 while it can show, -2 while it cannot be used.
    int MousePage() const { return !usable_ ? -2 : shown_ ? 4 : -1; }

    // One of the rows' own views shows (Players, Let in, Join: View): B is ours then and goes back to the five rows
    // (TakeBack), instead of closing the menu. Off whenever the page goes.
    void SetSubView(bool on) { sub_ = on && shown_; }
    bool TakeBack();

    bool Shown() const { return shown_; }
    // A pressed on our page since the last call (the row is the System page's focused button).
    bool TakeDecide();
    // Why it last went back to System, for the log ("" when none since the last call).
    std::string TakeWhy();
    // The menu sound the module starts at the strip's next update (R onto our page, L off it) and at the pages' (A on
    // a row): the game does not start them, the module took the press. nullptr when none.
    const char* TakeStripSound();
    const char* TakePagesSound();
    void Sound(const char* name) { pages_sound_ = name; }   // a menu sound for the module's own move (the cells)

    // The Join view's two fields (the room name, the join password) and which one is typed into (-1: none). While one
    // is, the pad's buttons are all the page's (none reach System) and the keyboard's keys are the field's (Key, Char).
    TextField& Field(int which) { return which == 1 ? password_ : room_; }
    const TextField& Field(int which) const { return which == 1 ? password_ : room_; }
    void StartTyping(int which);
    int Typing() const { return typing_; }
    bool Key(std::uint32_t vk, bool repeat);   // a Windows virtual-key code while typing: true when the field took it
    bool Char(std::uint32_t cp);               // a typed character while typing

private:
    void TypingPad(std::uint64_t down, std::uint64_t held, std::uint64_t now_ms);
    TextField room_{TextField::Kind::kRoom}, password_{TextField::Kind::kPassword};
    int typing_ = -1;
    std::uint64_t repeat_since_ = 0, repeat_last_ = 0;   // a held direction: when it went down, its last step

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
    const char* strip_sound_ = nullptr;
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
    int tab_key_ = -1;
    bool tab_filled_ = false;   // the fifth tab filled in this opening of the strip
};

// The rows (Pa_Button_00..04): Session, Join code, Players, Let in, Leave. Each A acts on the page itself (hosting,
// a copy, the second A of Leave), or shows one of the rows' own views on the same five rows (View: Session, Players,
// a player, Let in, Join). Only a Paste answer, a refused Host or Join, and co-op that came with the game's start open
// co-op's menu (Hookshot's menu, over the game's pause menu) on its main page, whose note or help says why.
// The game has no text entry or keyboard of its own (docs/game-ui.md, "Frames for a text field"), so a code is pasted.
enum class View { kMain, kPlayers, kLetIn, kJoin, kSession, kPlayer, kPlayerHost, kLan, kLanGame };
// Where B (or Back) goes from a view: a player's host controls to that player, a player to the Players view, a game on
// this network to the list of them, else the five rows.
View BackOf(View view);
enum Row { kRowSession = 0, kRowJoinCode = 1, kRowPlayers = 2, kRowLetIn = 3, kRowLeave = 4 };
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
    kShowPassword,   // the host's Session view: its join password shown on the row, or hidden again (only on A:
                     // friends who found the game on this network type it)
    // The game browser (View::kLan): the games in the cells on the right, the list chosen on the rows.
    kBrowseLan,      // the cells list the games on this network
    kBrowseOnline,   // the cells list the relay's public rooms (coop_room_list.h)
    kBrowsePage,     // the cells' next page
    kBrowseRefresh,  // the list asked again (this network's games forgotten; the relay asked at once)
    kListRoom,       // this game's room on the relay's public list when it hosts, on or off (coop-options.ini
                     // list_room; saved at once)
};
inline constexpr std::uint64_t kConfirmMs = 3000;
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
    int online_listed = 0;      // the rooms it lists
    int cell_page = 0;          // the browser's page of cells (kCells a page)
    bool list_room = false;     // hosting lists this game's room on the relay's public list
    bool lan_password = false;  // a game's view: its join password is typed (every session has one)
    bool has_password = false;  // hosting, and the session's join password is known (the status)
    bool password_shown = false;   // the host's Session view shows it now (its rows' A hides it again)
    bool can_resync = false;    // a guest whose game can take the host's save again (CoopSave::CanResync)
    int listed = 0;         // the Players view: how many players it lists
    int page = 0;           // the Players view: its page (PlayerIndex)
    int player = 0;         // a player's view: 0 that player left, 1 it is us, 2 another player
};
// The Players view: which player (an index of the list) a row shows on a page, or -1. Up to four fit; more: three a
// page and the fourth row turns the page ("More players").
int PlayerIndex(int listed, int page, int row);
int PlayerPages(int listed);
RowAction ActionFor(int row, const Session& s);
// The row of ours a focused button of the pages screen lies under: System's Pa_Button_00..04 (by the name the focus
// manager's walk gives, CoopModScreens::ButtonName), -1 for any other.
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
using RowBoxes = std::array<RowBox, kRows>;

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

// One update ahead of `now`, from its step since `prev`, and not past `limit` when it heads for it. The module reads
// the fourth tab's place and fades before the strip's own update, so they are one update old: ours takes them one
// update ahead to move with the fourth, not a frame after it.
float Ahead(float prev, float now, float limit);

// What the rows say, in the game's own text panes (the module writes them through the pack's placeholder messages,
// Coop_Live_NN, in LayoutMsg/PlusMenu_00): the session's state on the rows themselves.
// A row of the game browser's list (View::kLan, ListLines): its left text and its right one; both "" for a row not shown.
struct ListLine {
    std::string name, value;   // the row's left text and its right one; both "" for a row not shown
};

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
    std::string player_name;          // a player's view: who
    // 0.11.6 (owner OK, 2026-11-19): this kit's release, "Squadala! v0.11.6" (CoopWindowBrand::TitleOf), "" none
    // (a developer's build). On the Leave row when in no session (solo too) and on a free row of the Session view.
    std::string version;
    std::uint8_t own_mutes = 0, host_mutes = 0;   // a player's view: kMuteVoice / kMuteChat bits
    bool confirm_kick = false, confirm_ban = false;   // the host's Kick or Ban waits for its second A
    // The Join view's fields: their rows' words (TextField::Shown) and whether a relay is set to join a room through.
    std::string room_field, password_field;
    bool room_ready = false, relay_set = false;
    std::vector<std::string> lan_rows;    // the games on this network, as their rows say them (CoopLan::RowText)
    std::vector<std::string> online_rows; // the relay's public rooms, as their cells say them (CoopRoomList::CellText)
    std::vector<ListLine> lan_list, online_list;   // the same, as the browser's list shows them (name, players)
    std::string online_state;             // the browser's online list when it has no rooms: why ("" asking)
    int cell = -1;                        // the browser's cell picked with the pad or the mouse (-1: the rows')
    std::string lan_game;                 // a game's view: the game picked, as its row said it
    std::string turned_away;              // a guest the host's game turned away (kicked, banned, full: the Lobby's
                                          // refusal, its short reason), "" not; Leave, then join again
    bool banned = false;                  // (by the host's ban: joining again cannot work)
    std::string host_password;            // the host's Session view: the join password, once A asked for it
    std::string says;                     // the Session view: why the page's last Host or Join did not start
                                          // (CoopPause::PageSays), while that view shows after it; "" none
    int typing = -1;                  // the field typed into (0 the room, 1 the password), -1 none
};
// The most a row's text holds: the System page's button fits about 24 of the game's letters (seen in a run,
// 2026-11-19: "Leave (not in a session)" fills it, 31 letters ran past both edges).
inline constexpr std::size_t kRowChars = 24;
std::vector<std::string> RowTexts(const SessionView& v);
// A view's rows (kMain: RowTexts). A row with nothing on it says " " (a pane's text is never empty).
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
// What a game's right column says: "2/4", "Full", "Other version" (it cannot be joined from here).
std::string ListValue(int players, int max, bool same_version);
// What a room's left column says: the name its host listed it under (the room name unless they picked one), with the
// room name after it when that is another: "Saria's forest (forest-gang)".
std::string ListName(const std::string& shown, const std::string& room);
// The games the browser lists now (the chosen list's), and its pages.
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
// hos_coop_game_pause_page: Page::MousePage (4 shown, -1 can show, -2 cannot be used).
int MousePage() noexcept;
// The keyboard while one of the page's fields is typed into (hos_coop_key, hos_coop_char; KeysTaken: the game gets
// none of its keys then): true when the field took it.
bool Key(std::uint32_t vk, bool repeat) noexcept;
bool Char(std::uint32_t cp) noexcept;
bool KeysTaken() noexcept;

}  // namespace CoopPauseTab
