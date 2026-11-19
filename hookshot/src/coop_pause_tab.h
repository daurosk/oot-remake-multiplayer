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

#include <cstdint>
#include <string>
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
inline constexpr std::uint64_t kTabOpen = 0x208350;    // (tab, 0): the tab's part opened and shown, as the game's fill
                                                       //  ends for each of its tabs (else no text shows)
inline constexpr std::uint64_t kMsgRefInit = 0xa66f70; // (ref): {0, 0, -1}, 24 bytes (coop_title.cpp's)
inline constexpr std::uint64_t kMsgRefSet = 0xa6afe0;  // (ref, &"LayoutMsg/...", &"label", 1)
inline constexpr std::uint64_t kPaneText = 0xa67d30;   // (layout, "part/T_Text_00", ref, 1, 0)

// Switch pad bits (nn::hid::NpadButton), as hos_coop_pad gets them.
inline constexpr std::uint64_t kPadA = 1ull << 0, kPadB = 1ull << 1, kPadL = 1ull << 6, kPadR = 1ull << 7,
                               kPadZL = 1ull << 8, kPadZR = 1ull << 9, kPadPlus = 1ull << 10, kPadMinus = 1ull << 11,
                               kPadUp = 1ull << 13, kPadDown = 1ull << 15, kPadStickUp = 1ull << 17,
                               kPadStickDown = 1ull << 19;
// What the game still gets while our page shows: closing the menu, the Minus menu, and up and down, which move the
// System page's focus under ours: the game's own cursor frame (drawn where the focused button is, and our rows sit
// where System's buttons do) moves over our rows, with its sound (seen in a run, 2026-11-18). A is ours: the row is the
// System page's focused button. Left and right are not passed (past the page's edge they would step the menu).
inline constexpr std::uint64_t kPadPassed = kPadB | kPadPlus | kPadMinus | kPadUp | kPadDown | kPadStickUp | kPadStickDown;
// The System page's buttons (read from main: its set-up f_80f990e0): the page object at the pages screen + 0x320,
// its buttons at +0x38, +0x40 ... (5); a button's +0x38 byte is its focus state (Longshot's oot_autoload.c: non-zero and
// not 4 or 5 is focused).
inline constexpr std::uint64_t kSystemPageObject = 0x320, kSystemButtons = 0x38, kButtonState = 0x38;

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

// The page: when it shows, and what the module does to the game on each screen's update.
// One caller at a time (the module's lock): screen updates on the game's threads, the pad on its input thread.
class Page {
public:
    // What to do to the game at this screen's update (on its thread, with its ctx).
    struct Do {
        bool set_page = false;   // the pages: our page shown (and System hidden) or not
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

private:
    bool CanShow(std::uint64_t now_ms) const;
    void Hide(const char* why);

    Facts pages_, strip_;
    std::uint64_t pages_ms_ = 0, strip_ms_ = 0, blocked_until_ms_ = 0;
    bool shown_ = false;
    std::uint64_t pad_last_ = 0;
    std::uint64_t swallow_ = 0;   // buttons the module took, kept from the game until let go
    bool decide_ = false;
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
};

// The rows (Pa_Button_00..04): Session, Join code, Players, Let in, Leave. Each A acts on the page itself (hosting,
// a copy, the second A of Leave), or shows one of the rows' own views on the same five rows (View: Session, Players,
// a player, Let in, Join). Only a Paste answer, a refused Host or Join, and co-op that came with the game's start open
// co-op's menu (Hookshot's menu, over the game's pause menu) on its main page, whose note or help says why.
// The game has no text entry or keyboard of its own (docs/game-ui.md, "Frames for a text field"), so a code is pasted.
enum class View { kMain, kPlayers, kLetIn, kJoin, kSession, kPlayer, kPlayerHost };
// Where B (or Back) goes from a view: a player's host controls to that player, a player to the Players view, else the
// five rows.
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
// A row of a view (kMain: ActionFor).
RowAction ActionFor(View view, int row, const Session& s);
// A second A (Leave, Kick, Ban) is still waited for: our page shows and the game's cursor is on that row still.
inline bool KeepConfirm(bool shown, int focused_row, int confirm_row) {
    return shown && focused_row >= 0 && focused_row == confirm_row;
}

// What the rows say, in the game's own text panes (the module writes them through the pack's placeholder messages,
// Coop_Live_NN, in LayoutMsg/PlusMenu_00): the session's state on the rows themselves.
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
    std::uint8_t own_mutes = 0, host_mutes = 0;   // a player's view: kMuteVoice / kMuteChat bits
    bool confirm_kick = false, confirm_ban = false;   // the host's Kick or Ban waits for its second A
};
inline constexpr std::size_t kRowChars = 40;   // the most a row's text holds (the System page's button width)
std::vector<std::string> RowTexts(const SessionView& v);
// A view's rows (kMain: RowTexts). A row with nothing on it says " " (a pane's text is never empty).
std::vector<std::string> RowTexts(View view, const SessionView& v);
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

}  // namespace CoopPauseTab
