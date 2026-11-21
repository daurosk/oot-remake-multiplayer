// SPDX-License-Identifier: GPL-3.0-or-later
// coop_pause_tab.h, the module's part: patch 0030's call for every menu
// screen's update, the game's fields read, the game's own calls made on that
// screen's thread (its panes shown or hidden, its tabs lit, texts from the
// pack's labels), the pad filtered, the log. Only in hookshot_coop.dll (the
// tests drive Page and Probe themselves).
#include "coop_i18n.h"
#include "coop_pause_tab.h"
#include "coop_browse.h"
#include "coop_title.h"

#include <algorithm>
#include <array>
#include <map>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <utility>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

#include "common/logging.h"
#include "coop_hook.h"
#include "coop_hookshot.h"
#include "coop/protocol/join_password.hpp"
#include "coop/protocol/messages.hpp"
#include "coop_lan.h"
#include "coop_map_page.h"
#include "coop_lan_net.h"
#include "coop_mod_screens.h"
#include "coop_pause.h"
#include "coop_room_list.h"
#include "coop_runtime.h"
#include "coop_lobby_game.h"
#include "coop_look.h"
#include "coop_char_colours.h"
#include "coop_mods_tab.h"
#include "coop_snail_custom.h"
#include "hos_coop_menu.h"
#include "coop_longshot_settings.h"
#include "coop_lobby.h"
#include "coop_save.h"
#include "coop_window_brand.h"
#include "coop_title_menu.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace CoopPauseTab {

namespace {

const HosCoopApi* g_api = nullptr;
std::mutex g_mutex;   // everything below: screen updates on several of the game's threads, the pad on its own
Probe g_probe;
Page g_page;
std::atomic<int> g_mouse{-2};       // Page::MousePage, for the mouse (it asks often, from any thread)
std::atomic<int> g_view{static_cast<int>(View::kMain)};   // the rows' view (View); kMain whenever the page goes

View ViewShown() {
    return static_cast<View>(g_view.load());
}
// A second A (Leave, Kick, Ban): until when it is waited for (0: not), and on which row of which view. Any thread.
std::atomic<std::uint64_t> g_confirm_until{0};
std::atomic<int> g_confirm_row{-1}, g_confirm_view{0};
std::atomic<bool> g_confirm_by_mouse{false};   // (the waiting A came from a click: SecondA)
bool g_act_clicked = false;                    // (Act's row came from a click; Act's thread only)
// The Players view's page, and the player whose view shows (their number; the views kPlayer, kPlayerHost).
std::atomic<int> g_players_page{0}, g_player{0};
std::atomic<int> g_lan_page{0};   // the games on this network: the view's page
// The game picked on that list, for its own view (View::kLanGame): where it is, and its row's words. g_mutex.
struct LanPick {
    std::string address, row;
    std::uint16_t port = 0;
};
LanPick g_lan_pick;
// The game browser (View::kLan): its list (the relay's rooms, else this network's games), its page of cells, and the
// cell picked with the pad or the mouse (-1: the rows have the pad). Any thread.
std::atomic<bool> g_online{false};
std::atomic<int> g_browse_filter{0}, g_browse_sort{0};   // the browser's filter and order (CoopBrowse)
std::atomic<int> g_browse_listed{0};        // the games the browser's list keeps now (its pages)
std::atomic<std::uint64_t> g_head_click_ms{0};   // the left button last down on the browser's heading
std::atomic<int> g_cell_page{0};
std::atomic<int> g_cell{-1};
std::atomic<int> g_cell_games{0};   // the cells on this page that hold a game (LiveCells)
// Settings and look (View::kMenu): co-op's menu's page shown on the list (kMenuTabNames' row), whether the list has the
// pad, the list's first row (MenuLines keeps the selected one in view), and each list line's menu row and whether it
// can be picked (LiveCells, from what it showed). Any thread.
std::atomic<int> g_menu_tab{0};
std::atomic<bool> g_menu_list{false};
std::atomic<int> g_menu_top{0};
std::array<std::atomic<int>, kMenuMost + 1> g_menu_line_row{};
std::array<std::atomic<bool>, kMenuMost + 1> g_menu_line_pick{};
// The page co-op's menu shows for this view (CoopPause::OpenNative; -1 none): FollowMenu's, on the pad's thread.
std::atomic<int> g_menu_open{-1};
std::atomic<int> g_strip_state{0};            // row 199: the tab strip's state at its last update
std::atomic<std::uint64_t> g_strip_seen_ms{0};   // and when (GameMenuUp)
// Co-op's menu types into its row (a name, a port, the relay): the list's rows are the typing keys then, as for the
// page's own fields (TypingLines): the key picked, Letter's letter (its place in kMenuLetters) and the rows shown
// (empty while it does not type). g_mutex.
int g_menu_key = 0;
std::size_t g_menu_letter = 0;
std::vector<ListLine> g_menu_typing_lines;
// (the letters Letter offers on co-op's menu's rows; the menu itself takes only those its row allows, Menu::Char)
constexpr std::string_view kMenuLetters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.: ";
bool g_menu_typed_last = false;
std::uint64_t g_menu_pad_last = 0;
std::atomic<bool> g_menu_focus_reset{false};   // (the view came: the game's cursor's row then is no choice)
int g_menu_focus_seen = -2;                     // the game's cursor's row as last followed. g_mutex
std::uint64_t g_menu_click_ms = 0;   // the left button last down on the list or Link (Longshot's A for it: not one)
bool g_model_dragging = false;       // the left button went down on Link (Customize Link): its moves turn him
float g_model_drag_x = 0.0f;
// The Multiplayer page's rows with nothing on them (bit per row, LiveRows), not shown (the game's cursor on one goes on
// to a shown one: RowSkip); none hidden while a moment's hiding goes on (SystemButtons: it shows its rows again
// itself).
std::atomic<std::uint32_t> g_rows_blank{0};
std::atomic<bool> g_rows_moment{false};
std::array<std::atomic<std::uint64_t>, kCells + 1> g_cell_pane_at{};   // the cells' panes in our page, this opening
std::atomic<std::uint64_t> g_head_pane_at{0};                      // (and the list's first line's, above the cells)
bool g_lines_held = false;   // (the details list's own click: the left button as last read, the line it went down on)
int g_lines_press = -1;
// The pad's and the mouse's last state for the cells (OnPad, g_mutex).
std::uint64_t g_browse_last = 0;
float g_cells_mouse_x = 0, g_cells_mouse_y = 0;
bool g_cells_held = false;
int g_cells_press = -1;
// What the page's last action came to when co-op did not start (CoopPause::PageSays), shown on the Session view from
// the update that saw it until another view shows; and the newest one seen. g_says_mutex (SetView runs under g_mutex
// and outside it).
std::atomic<bool> g_show_password{false};   // the host's Session view shows the join password (A on its row)
std::mutex g_says_mutex;
std::string g_says;
std::uint64_t g_says_seen = 0;
static_assert(kMuteVoice == CoopLobby::kVoice && kMuteChat == CoopLobby::kChat);
// Panes found, per opening of a screen (Page::Track's number; a menu reopened at the same address is looked up again,
// so no pane of a freed layout is used). *_ok: both looked up for that opening and parts; the page is usable only then.
std::uint64_t g_pages_gen = 0, g_pages_screen = 0, g_system_pane = 0, g_coop_pane = 0;
std::uint64_t g_settings_pane = 0;   // the Settings page (Pa_Settings_00; 0 with an older pack)
// System's two buttons of ours (the pack's Pa_Button_05, Longshot Settings, and Pa_Button_06, Multiplayer, of
// PaSystemMenu_00), and the same two in our pages (copies of System's page: all hidden but Longshot Settings' sixth
// row, its Debug category); 0 with an older pack. Each page's own rows (Pa_Button_00..04), and System's lower buttons'
// own heights (Options, Special Actions, Title Screen: moved to make room for ours, Title Screen last).
std::uint64_t g_settings_button = 0, g_settings_button_coop = 0, g_settings_button_set = 0;
std::uint64_t g_mp_button = 0, g_mp_button_coop = 0, g_mp_button_set = 0;
// Customize Link (Pa_Button_07): Settings and look's Customize page straight from System (a tester found it too deep
// under Multiplayer, 2026-11-20). Its page is the Multiplayer page, opened on that view; B there is System again.
std::uint64_t g_cl_button = 0, g_cl_button_coop = 0, g_cl_button_set = 0;
std::atomic<bool> g_customize_direct{false};   // the page shows because of that button (B: System, not the rows)
std::array<std::uint64_t, kRows> g_set_buttons{}, g_coop_buttons{}, g_sys_buttons{};
std::array<float, kRows> g_sys_rest_y{};
int g_settings_button_shown = -1;   // (which of ours System shows: 1 Multiplayer, 2 Longshot Settings; -1 not set yet)
bool g_sys_hidden = false;          // (System's own buttons hidden: one of our pages is in its place, SystemButtons)
bool g_pages_looked = false, g_pages_ok = false;
std::uint64_t g_pages_layout = 0;  // the layout the pages' panes were looked up in (the early page switch checks it)

template <class T>
T Read(std::uint64_t a) {
    T v{};
    return CoopRuntime::Read(a, &v, sizeof v) ? v : T{};
}

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

bool CanCall() {
    return g_api != nullptr && g_api->call != nullptr && g_api->size >= offsetof(HosCoopApi, scratch) + sizeof(g_api->scratch) &&
           g_api->scratch != nullptr;
}

bool Call(void* ctx, std::uint64_t offset, std::uint64_t a0, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
          std::uint64_t a3 = 0, std::uint64_t a4 = 0, std::uint64_t* x0 = nullptr) {
    HosCoopCall k{};
    k.function = CoopRuntime::MainBase() + offset;
    k.x[0] = a0, k.x[1] = a1, k.x[2] = a2, k.x[3] = a3, k.x[4] = a4;
    std::uint64_t r = 0;
    const bool ok = g_api->call(ctx, &k, &r);
    if (x0 != nullptr) *x0 = r;
    return ok;
}

void CallAlpha(void* ctx, std::uint64_t pane, float alpha) {
    HosCoopCall k{};
    k.function = CoopRuntime::MainBase() + kPaneAlpha;
    k.x[0] = pane;
    k.s0 = alpha;
    std::uint64_t r = 0;
    g_api->call(ctx, &k, &r);
}

// (layout, "name") -> the pane itself, a part's own pane too. kFindPane (main+0x76e2b0) hands a part's inner root
// instead (its layout's pane 0, the one the game hides a tab through); the pause tab's copies are stored hidden on the
// part's own pane, so that is the one shown. The same two calls kFindPane makes first (read from main, 2026-11-18):
// main+0x43c040(layout + 0x40) -> the layout's pane table, main+0x43e300(table, "name") -> the pane by name.
std::uint64_t FindOwnPane(void* ctx, std::uint64_t layout, const char* name) {
    std::uint64_t table = 0, pane = 0;
    if (!Call(ctx, kPaneTable, layout + 0x40, 0, 0, 0, 0, &table) || table == 0) return 0;
    const std::uint64_t at = g_api->scratch(ctx, name, std::strlen(name) + 1);
    return at != 0 && Call(ctx, kPaneByName, table, at, 0, 0, 0, &pane) ? pane : 0;
}

// A part's own layout, from the parts list of the layout holding it (the list kFindPane walks when the name table has
// no such pane, read from main 2026-11-18: sentinel table + 0x128, first [table + 0x130], next [node + 8], each node a
// layout whose name is at [node + 0x148]). `table` is a layout as kPaneTable gives it: its panes by name are kPaneByName.
std::uint64_t PartLayout(std::uint64_t table, const char* name) {
    if (table == 0) return 0;
    const std::uint64_t end = table + 0x128;
    std::uint64_t node = Read<std::uint64_t>(table + 0x130);
    for (int guard = 0; node != 0 && node != end && guard < 256; ++guard) {
        char got[64] = {};
        const std::uint64_t at = Read<std::uint64_t>(node + 0x148);
        if (at != 0 && CoopRuntime::Read(at, got, sizeof got - 1) && std::strcmp(got, name) == 0) return node;
        node = Read<std::uint64_t>(node + 8);
    }
    return 0;
}

std::uint64_t PaneIn(void* ctx, std::uint64_t table, const char* name) {
    if (table == 0) return 0;
    const std::uint64_t at = g_api->scratch(ctx, name, std::strlen(name) + 1);
    std::uint64_t pane = 0;
    return at != 0 && Call(ctx, kPaneByName, table, at, 0, 0, 0, &pane) ? pane : 0;
}

// A part's pane (kind 6 at +0xa8 bits 22..25, docs/game-ui.md): what the visibility call is made on.
bool IsPart(std::uint64_t pane) {
    return pane != 0 && ((Read<std::uint32_t>(pane + 0xa8) >> 22) & 0xf) == 6;
}

void SetVisible(void* ctx, std::uint64_t pane, bool on) {
    Call(ctx, kSetVisible, pane, on ? 1 : 0);
}

// A text pane's text from a label of the pack's (LayoutMsg/<file>), the way the game sets its own (coop_title.cpp's
// SetRowTexts): [0x00] the message ref, [0x20] &file, [0x28] &label, [0x30] file, [0x50] label, [0x90] the pane's path.
bool SetText(void* ctx, std::uint64_t layout, const std::string& path, const std::string& file, const std::string& label) {
    if (file.size() >= 0x20 || label.size() >= 0x40 || path.size() >= 0x40) return false;
    std::uint8_t b[0xd0] = {};
    const std::uint64_t base = g_api->scratch(ctx, b, sizeof b);
    if (base == 0) return false;
    const std::uint64_t at_file = base + 0x30, at_label = base + 0x50, at_path = base + 0x90;
    std::memcpy(b + 0x20, &at_file, 8);
    std::memcpy(b + 0x28, &at_label, 8);
    std::memcpy(b + 0x30, file.c_str(), file.size() + 1);
    std::memcpy(b + 0x50, label.c_str(), label.size() + 1);
    std::memcpy(b + 0x90, path.c_str(), path.size() + 1);
    if (g_api->scratch(ctx, b, sizeof b) != base) return false;
    return Call(ctx, kMsgRefInit, base) && Call(ctx, kMsgRefSet, base, base + 0x20, base + 0x28, 1) &&
           Call(ctx, kPaneText, layout, at_path, base, 1, 0);
}

// --- Live text: our own words in the game's text panes ---
// The rows' text panes are tagged "=T_Btn_00" (seen in a run, 2026-11-19, on both System's buttons and ours), and the
// game's pane-text call (kPaneText -> main+0x43b970) sets only "@"-tagged ones: it reaches the tab's word, never a
// row (0 panes set, its own count). So the module does what that call does for each pane it sets: the pane by the
// game's path lookup (kPathFind: the last part's layout and the name) and its name there (kPaneByName), then the
// pane's set-message call (kPaneSetMsg) with a reference {text, length in bytes, the message's attributes}, the
// attributes the pane's own (+0x10, where the call keeps them).
// The text box keeps that text where it is (a pointer and a length, not a copy: a run with every row's text in the
// same scratch memory showed every row the last one's words, cut to its own length), so each row's text lives in a
// message of the pack's (strings/en.txt: Coop_Live_00..07, 64 dots each, in LayoutMsg/PlusMenu_00): the module looks
// it up for each opening of the pages and writes its words there.
constexpr int kLiveSlots = 48;   // 0..4 the rows, 8..21 the browser's list, 22..40 the Settings page (the pack's 00..47)
constexpr std::uint32_t kLivePlaceholder = 64;   // the placeholder's bytes (strings/en.txt)
struct LiveSlot {
    std::uint64_t gen = 0;      // the pages' opening it was looked up for (Page::Track): looked up again for each
    std::uint64_t text = 0;     // the placeholder's text in the loaded message file (0: not looked up)
    std::uint32_t extra = 0;    // what the game's length counts beyond the text (its 0 at the end: 1, else 0)
    std::uint64_t attrs = 0;    // the reference's third word, kept
    bool bad = false;           // looked up and not ours (an old pack): no live text
    std::string shown;          // what the pane shows now (per pages screen)
};
std::mutex g_live_mutex;        // the slots (the pages' updates come on several threads)
LiveSlot g_live[kLiveSlots];
std::uint64_t g_live_gen = 0, g_live_ms = 0;

// The slot's placeholder, looked up again for each opening of the pages (the reference the game makes for it now): a
// message file loaded again (another language, the archive reloaded) is found where it is now, never written through
// an old address. Ours when its length is the placeholder's and its text is the placeholder's dots, or our own words at
// the address we wrote before (the same file still loaded).
// (any message file of the pack's placeholders: `file` "LayoutMsg/PlusMenu_00" and "Coop_Live_NN" for the pages' rows;
// "LayoutMsg/CharacterCustomize_00" and "Coop_Custom_NN" for Character Select's Customize screen, 0.11.9)
bool LiveLookupIn(void* ctx, LiveSlot& l, const std::string& file, const char* label, std::uint64_t gen) {
    if (l.bad) return false;
    if (l.gen == gen && l.text != 0) return true;
    std::uint8_t b[0x80] = {};
    const std::uint64_t base = g_api->scratch(ctx, b, sizeof b);
    if (base == 0) return false;
    const std::uint64_t at_file = base + 0x30, at_label = base + 0x60;
    std::memcpy(b + 0x20, &at_file, 8);
    std::memcpy(b + 0x28, &at_label, 8);
    std::memcpy(b + 0x30, file.c_str(), file.size() + 1);
    std::memcpy(b + 0x60, label, std::strlen(label) + 1);
    if (g_api->scratch(ctx, b, sizeof b) != base) return false;
    if (!Call(ctx, kMsgRefInit, base) || !Call(ctx, kMsgRefSet, base, base + 0x20, base + 0x28, 1)) return false;
    const std::uint64_t text = Read<std::uint64_t>(base);
    const std::uint32_t length = Read<std::uint32_t>(base + 8);
    char head[8] = {};
    CoopRuntime::Read(text, head, sizeof head);
    const bool dots = std::memcmp(head, "........", 8) == 0;
    if (text == 0 || (!dots && text != l.text) || length < kLivePlaceholder || length > kLivePlaceholder + 1) {
        const bool before = l.text != 0;
        l.text = 0, l.gen = 0;
        l.bad = !before;   // (never ours: an old pack; ours before: only this opening goes without, looked up again next)
        LOG_INFO(Core, "coop: pause tab: live text: {} is not the pack's placeholder (text {:#x}, length {}): the rows "
                       "keep their own words{}", label, text, length, before ? " this time" : " (build the puppet pack again)");
        return false;
    }
    const bool moved = l.text != 0 && l.text != text;
    const bool first = l.text == 0;
    l.gen = gen;
    l.text = text;
    l.extra = length - kLivePlaceholder;
    l.attrs = Read<std::uint64_t>(base + 0x10);
    if (first || moved) {
        LOG_INFO(Core, "coop: pause tab: live text: {} at {:#x}, the game counts {} byte(s) more than its text{}", label,
                 text, l.extra, moved ? " (the message file moved: found again)" : "");
    }
    return true;
}

// `utf8` (at most kLivePlaceholder - 1 bytes) in the text pane at `path` ("Part/Part/Pane"), through slot `slot`.
// True when shown.
bool LiveLookup(void* ctx, int slot, std::uint64_t gen) {
    if (slot < 0 || slot >= kLiveSlots) return false;   // (a slot past the table: never written through)
    char label[16];
    std::snprintf(label, sizeof label, "Coop_Live_%02d", slot);
    return LiveLookupIn(ctx, g_live[slot], "LayoutMsg/PlusMenu_00", label, gen);
}

// `utf8` in the text pane at `path` of `layout`, through the placeholder `l` (looked up with LiveLookupIn). `force`:
// set again even when it shows these words (another pane given the same placeholder). True when shown.
bool LiveTextIn(void* ctx, std::uint64_t layout, const std::string& path, LiveSlot& l, const std::string& utf8,
                bool force = false);
thread_local const char* g_live_why = "";   // (why LiveTextIn last returned false: named in the Customize line)

bool LiveText(void* ctx, std::uint64_t layout, std::uint64_t gen, const std::string& path, int slot,
              const std::string& utf8) {
    if (slot < 0 || slot >= kLiveSlots) return false;
    LiveSlot& l = g_live[slot];
    if (l.shown == utf8) return true;
    if (path.size() >= 0x40 || utf8.size() >= kLivePlaceholder || !LiveLookup(ctx, slot, gen)) return false;
    return LiveTextIn(ctx, layout, path, l, utf8);
}

bool LiveTextIn(void* ctx, std::uint64_t layout, const std::string& path, LiveSlot& l, const std::string& utf8,
                bool force) {
    if (!force && l.shown == utf8) return true;
    if (path.size() >= 0x40 || utf8.size() >= kLivePlaceholder || l.text == 0) {
        return g_live_why = "no placeholder", false;
    }
    // [0x00] the path lookup's layout, [0x10] the path, [0x60] the reference.
    std::uint8_t b[0x80] = {};
    std::memcpy(b + 0x10, path.c_str(), path.size() + 1);
    const std::uint64_t base = g_api->scratch(ctx, b, sizeof b);
    if (base == 0) return g_live_why = "no scratch", false;
    std::uint64_t name = 0;
    if (!Call(ctx, kPathFind, base + 0x10, layout, base, 0, 0, &name) || name == 0) return g_live_why = "path not in the layout", false;
    const std::uint64_t part = Read<std::uint64_t>(base);
    std::uint64_t table = 0, pane = 0;
    if (part == 0 || !Call(ctx, kPaneTable, part + 0x40, 0, 0, 0, 0, &table) || table == 0) return g_live_why = "no pane table", false;
    if (!Call(ctx, kPaneByName, table, name, 0, 0, 0, &pane) || pane == 0) return g_live_why = "pane not found", false;
    if (((Read<std::uint32_t>(pane + 0xa8) >> 22) & 0xf) != kTextPaneKind) return g_live_why = "not a text pane", false;   // (not a text pane)
    std::string bytes = utf8;
    bytes.push_back('\0');
    if (!CoopRuntime::Write(l.text, bytes.data(), bytes.size())) return g_live_why = "not written", false;
    const std::uint64_t attrs = Read<std::uint64_t>(pane + 0x10);
    const std::uint32_t length = static_cast<std::uint32_t>(utf8.size()) + l.extra;
    std::uint8_t ref[0x18] = {};
    std::memcpy(ref + 0x00, &l.text, 8);
    std::memcpy(ref + 0x08, &length, 4);
    std::memcpy(ref + 0x10, &attrs, 8);
    if (!CoopRuntime::Write(base + 0x60, ref, sizeof ref)) return g_live_why = "reference not written", false;
    if (!Call(ctx, kPaneSetMsg, pane, base + 0x60, 0)) return g_live_why = "the game's set failed", false;
    l.shown = utf8;
    return true;
}

// --- Character Select's Customize screen with co-op's customization (0.11.9, coop_snail_custom.h) ---
// His screen (layout CharacterCustomize_00) is known by its own panes, once per layout. While it is open its texts are
// co-op's (through the pack's placeholders in his message file, LayoutMsg/CharacterCustomize_00 Coop_Custom_00..12,
// added at the kit's start by merge_tables.py), its keys co-op's but the d-pad, B and -. Fallback: when his texts
// cannot be set (no placeholders: a start without the merge), co-op keeps his keys but the d-pad, B and - from the
// game there, so none of his six dead rows changes anything. g_snail_mutex (the game's screen updates and the pad).
constexpr int kSnailSlots = 13;   // 0 the title, 1..6 the labels, 7..12 the values
constexpr std::uint64_t kSnailStaleMs = 300;
std::mutex g_snail_mutex;
CoopSnailCustom::Driver g_snail;
bool g_snail_open = false, g_snail_fallback = false;
// A look chosen on his screen since it opened (a preview waiting): worn and sent when the screen is left, also when
// co-op stopped driving it meanwhile (the fallback, his panes gone). Row 198 (0.11.9): only a screen still driven at
// its close applied the look, so a change made before co-op let go of it stayed a preview, never sent nor saved.
bool g_snail_changed = false;
std::uint64_t g_snail_screen = 0, g_snail_layout = 0, g_snail_seen_ms = 0, g_snail_gen = 0, g_snail_pad_last = 0;
std::uint64_t g_snail_edge_ms = 0;   // when his screen's state was first seen open (0: not open), SnailEdgeFrom
struct SnailLayout {
    std::uint64_t screen = 0;   // the screen it was looked at with
    bool his = false;           // his Customize screen's
};
std::map<std::uint64_t, SnailLayout> g_snail_layouts;   // a layout: his Customize screen's or not (once per screen)
std::array<LiveSlot, kSnailSlots> g_snail_live{};
CoopSnailCustom::Texts g_snail_shown;
std::string g_snail_failed_said;   // (the panes last named as not set: said once per list)
bool g_snail_wait = false;   // his screen open, his panes not found in its layout: co-op waits (keys kept from his rows)
std::size_t CountOf(const std::string& text, const char* what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}

void SnailClose(const char* why) {
    if (!g_snail_open) return;
    g_snail_open = false, g_snail_wait = false;
    const bool driven = g_snail.Active();
    g_snail.Close();
    // (Link wears the look chosen, as when co-op's Customize closes; also one chosen before co-op let go of it)
    if (driven || std::exchange(g_snail_changed, false)) CoopLook::ApplyOnUnpause();
    g_snail_changed = false;
    LOG_INFO(Core, "coop: characters: Character Select's Customize screen left ({})", why);
}

// His screen's texts from co-op's (the placeholders looked up for this opening first). kNoPlaceholders: one of them is
// not in his message file (a start without the kit's merge: the fallback). Otherwise the panes whose text could not be
// set are named in `failed` and the rest show (2026-11-21, lane 035925: the screen opened again for Saria went to the
// fallback, "no co-op placeholders", on the same file: her fewer rows gave empty labels and values, which the game's
// set-message call does not take, and any one pane's failure was taken for the placeholders' absence).
enum class SnailWritten { kAll, kSome, kNoPlaceholders };
SnailWritten SnailWrite(void* ctx, const CoopSnailCustom::Texts& t, std::string& failed) {
    for (int i = 0; i < kSnailSlots; ++i) {
        char label[20];
        std::snprintf(label, sizeof label, "Coop_Custom_%02d", i);
        if (!LiveLookupIn(ctx, g_snail_live[static_cast<std::size_t>(i)], "LayoutMsg/CharacterCustomize_00", label,
                          g_snail_gen)) {
            failed = label;
            return SnailWritten::kNoPlaceholders;
        }
    }
    using CoopSnailCustom::PaneText;
    const auto set = [&](const char* path, LiveSlot& l, const std::string& text, bool force) {
        if (LiveTextIn(ctx, g_snail_layout, path, l, PaneText(text), force)) return true;
        failed += std::string(failed.empty() ? "" : ", ") + path + " (" + g_live_why + ")";
        return false;
    };
    set("T_Head_00", g_snail_live[0], t.title, false);
    char path[24];
    for (int f = 0; f < CoopSnailCustom::kOptions; ++f) {
        const std::size_t k = static_cast<std::size_t>(f);
        std::snprintf(path, sizeof path, "T_Label_%02d", f);
        set(path, g_snail_live[1 + k], t.labels[k], false);
        // (every value pane of the option: whichever his "Val" frame shows, co-op's value shows; the first one must
        // take it, the others stop at the first his layout does not have)
        std::snprintf(path, sizeof path, "T_Val_%02d_00", f);
        if (!set(path, g_snail_live[7 + k], t.values[k], false)) continue;
        for (int v = 1; v < 16; ++v) {
            std::snprintf(path, sizeof path, "T_Val_%02d_%02d", f, v);
            if (!LiveTextIn(ctx, g_snail_layout, path, g_snail_live[7 + k], PaneText(t.values[k]), true)) break;
        }
    }
    return failed.empty() ? SnailWritten::kAll : SnailWritten::kSome;
}

// One update of any game screen (OnScreen, before the kinds of ours): his Customize screen's.
void SnailScreen(void* ctx, std::uint64_t screen) {
    if (ctx == nullptr || !CanCall()) return;
    const std::uint64_t layout = Read<std::uint64_t>(screen + kScreenLayout);
    if (layout == 0) return;
    std::lock_guard lock(g_snail_mutex);
    const auto his_panes = [&] {
        return FindOwnPane(ctx, layout, "T_Val_00_00") != 0 && FindOwnPane(ctx, layout, "T_Label_00") != 0 &&
               FindOwnPane(ctx, layout, "N_Mark_00") != 0;
    };
    // (a layout is looked at once per screen it comes with. Lane 053539, 0.11.9: Saria's reopen failed every one of
    // his panes at each update: the layout was known as his by its address alone, and a layout freed and made again at
    // that address is another screen's. A known address with another screen is looked at again, either way.)
    auto known = g_snail_layouts.find(layout);
    if (known == g_snail_layouts.end() || known->second.screen != screen) {
        if (known == g_snail_layouts.end() && g_snail_layouts.size() > 512) g_snail_layouts.clear();   // (a bound)
        const bool was = known != g_snail_layouts.end() && known->second.his;
        const bool his = his_panes();
        known = g_snail_layouts.insert_or_assign(layout, SnailLayout{screen, his}).first;
        if (his && !was) {
            LOG_INFO(Core, "coop: characters: Character Select's Customize screen found (layout {:#x})", layout);
        } else if (was && !his) {
            LOG_INFO(Core, "coop: characters: layout {:#x} (screen {:#x}) was Character Select's Customize screen's and "
                           "has not its panes now: not taken", layout, screen);
        }
    }
    if (!known->second.his) return;
    const int state = Read<std::uint8_t>(screen + kScreenState);
    if (state != 1 && state != 2) {
        g_snail_edge_ms = 0;
        SnailClose("closed");
        return;
    }
    // (taken as open only inside a menu that is up: lane 062917, his layout read open for a second while the - menu
    // closed, and co-op kept his rows' keys from the field then. CoopPauseTab::SnailEdgeFrom)
    const std::uint64_t now = NowMs();
    if (g_snail_edge_ms == 0) g_snail_edge_ms = now;
    switch (SnailEdgeFrom(g_snail_open, g_snail_edge_ms, g_strip_state.load(), g_strip_seen_ms.load(), now)) {
        case SnailEdge::kWait: return;
        case SnailEdge::kClose: SnailClose("the menu is closing"); return;
        case SnailEdge::kOpen: break;
    }
    g_snail_seen_ms = now;
    if (!g_snail_open || screen != g_snail_screen || layout != g_snail_layout) {
        g_snail_open = true, g_snail_fallback = false;
        g_snail_screen = screen, g_snail_layout = layout, ++g_snail_gen;
        g_snail_shown = {};
        g_snail_failed_said.clear();
        for (auto& l : g_snail_live) l.shown.clear();
        // (every opening looks at his panes again, the open edge itself: lane 061445, 0.11.9, Saria's 4th opening
        // came with the same screen and layout as the three before and none of his panes could be found in it, so a
        // check keyed on a changed address never ran. Not there: co-op waits, his keys kept from his rows meanwhile,
        // and looks again at each update)
        g_snail_wait = !his_panes();
        if (!g_snail_wait) g_snail.Open(CoopLook::Shown(), CoopCharColours::ShownCharacter());
        LOG_INFO(Core, "coop: characters: Character Select's Customize screen opened (screen {:#x}, layout {:#x}): {}",
                 screen, layout, g_snail_wait ? "his panes are not in its layout yet: co-op waits for them (his keys "
                                                "kept from his rows)" : "co-op's customization in it");
    } else if (g_snail_wait && his_panes()) {
        g_snail_wait = false, ++g_snail_gen;
        g_snail_shown = {};
        g_snail_failed_said.clear();
        for (auto& l : g_snail_live) l.shown.clear();
        g_snail.Open(CoopLook::Shown(), CoopCharColours::ShownCharacter());
        LOG_INFO(Core, "coop: characters: Character Select's Customize screen: his panes are there now: co-op's "
                       "customization in it");
    }
    if (g_snail_wait || !g_snail.Active()) return;
    if (auto look = g_snail.TakeChange()) {
        CoopLook::SetPending(*look);   // (your Link wears it at once, a preview)
        g_snail_changed = true;
    }
    const auto t = g_snail.Shown();
    if (t == g_snail_shown) return;
    std::string failed;
    const SnailWritten w = SnailWrite(ctx, t, failed);
    if (w == SnailWritten::kNoPlaceholders) {
        g_snail.Close();   // (his screen stays; its keys are kept from the game: the fallback)
        g_snail_fallback = true;
        LOG_INFO(Core, "coop: characters: Character Select's Customize screen: {} is not in its messages (start the "
                       "game from the kit, which adds co-op's placeholders): its keys are kept from it", failed);
        return;
    }
    if (w == SnailWritten::kSome && CountOf(failed, "(pane not found)") + CountOf(failed, "(path not in the layout)") ==
                                           static_cast<std::size_t>(kSnailSlots) && !his_panes()) {
        // (every pane gone from under co-op while open: wait for them as at an opening, his keys kept from his rows)
        g_snail.Close();
        g_snail_wait = true;
        LOG_INFO(Core, "coop: characters: Character Select's Customize screen: none of his panes is in its layout now: "
                       "co-op waits for them (his keys kept from his rows)");
        return;
    }
    if (w == SnailWritten::kSome) {
        // (the rest shows and co-op keeps the screen. Tried again every update until all are set: lane 0.11.9, a
        // reopen for Saria failed every pane on its opening update, and keeping that as shown left them unset. The
        // line is said once per different list, so a pane that never takes it is named once, not every frame.)
        if (failed != g_snail_failed_said) {
            g_snail_failed_said = failed;
            LOG_INFO(Core, "coop: characters: Character Select's Customize screen: the text of {} could not be set "
                           "(tried again at each update)", failed);
        }
        return;
    }
    if (!g_snail_failed_said.empty()) {
        g_snail_failed_said.clear();
        LOG_INFO(Core, "coop: characters: Character Select's Customize screen: every text set");
    }
    g_snail_shown = t;
}

// The pad while his screen is open: the buttons the game gets (or nothing to do: false).
bool SnailPad(std::uint64_t in, std::uint64_t* out) {
    std::lock_guard lock(g_snail_mutex);
    if (g_snail_open && NowMs() - g_snail_seen_ms > kSnailStaleMs) SnailClose("not seen");
    const std::uint64_t down = in & ~g_snail_pad_last;
    g_snail_pad_last = in;
    if (!g_snail_open) return false;
    using namespace CoopSnailCustom;
    constexpr std::uint64_t kDirs = kUp | kDown | kLeft | kRight | kStickUp | kStickDown | kStickLeft | kStickRight;
    std::uint64_t pass = kDirs | kB | kMinus | kPadPlus;
    if (g_snail.Active()) {
        g_snail.Pad(down);
        if (g_snail.Editing()) pass &= ~(kB | kMinus);   // (B leaves co-op's mixer, not his screen)
    }
    *out = in & pass;
    return true;
}

// A second A on this row of this view is waited for.
bool Confirming(View view, int row) {
    return NowMs() < g_confirm_until.load() && g_confirm_view.load() == static_cast<int>(view) &&
           g_confirm_row.load() == row;
}

// The player whose view shows, in the session as it is now (null: they left, or no session).
const CoopLobby::PlayerSeen* ShownPlayer(const CoopLobby::Seen& seen) {
    if (!seen.connected) return nullptr;
    for (const CoopLobby::PlayerSeen& p : seen.players) {
        if (p.number == g_player.load()) return &p;
    }
    return nullptr;
}

// The relay's rooms and this network's games the browser lists (its search, filter and order: CoopBrowse), in its
// order; the cells and a pick read the same lists.
std::vector<CoopRoomList::Room> SearchedRooms(const std::string& search) {
    const std::vector<CoopRoomList::Room> all = CoopRoomList::Now().rooms;
    std::vector<CoopBrowse::Entry> entries;
    for (const CoopRoomList::Room& r : all) {
        entries.push_back({r.room, r.host, r.players, r.max, r.protocol == coop::protocol::kProtocolVersion});
    }
    std::vector<CoopRoomList::Room> kept;
    for (const int i : CoopBrowse::Order(entries, search, g_browse_filter.load(), g_browse_sort.load())) {
        kept.push_back(all[static_cast<std::size_t>(i)]);
    }
    return kept;
}

std::vector<CoopLan::Heard> SearchedGames(const std::string& search) {
    const std::vector<CoopLan::Heard> all = CoopLanNet::Games(NowMs());
    std::vector<CoopBrowse::Entry> entries;
    for (const CoopLan::Heard& h : all) {
        entries.push_back({h.game.name + "'s game" + (h.game.room.empty() ? "" : ": " + h.game.room), h.game.name,
                           h.game.players, h.game.max, h.game.protocol == coop::protocol::kProtocolVersion});
    }
    std::vector<CoopLan::Heard> kept;
    for (const int i : CoopBrowse::Order(entries, search, g_browse_filter.load(), g_browse_sort.load())) {
        kept.push_back(all[static_cast<std::size_t>(i)]);
    }
    return kept;
}

std::string BrowseSearch() {
    std::lock_guard lock(g_mutex);   // (the search is the page's field; no other lock of ours is held by the callers)
    return g_page.Field(5).text();
}

// What the rows say now (co-op's host status and the Lobby's room, its players). No lock of ours held: they take
// their own.
SessionView ViewNow(View view) {
    HosCoopStatus st{};
    st.size = sizeof st;
    CoopHookshot::HostMenu().Status(&st);
    SessionView v;
    if (view == View::kMenu) {
        v.menu = CoopPause::NativeView();
        v.menu_tab = g_menu_tab.load(), v.menu_top = g_menu_top.load();
    }
    v.s.menu_list = g_menu_list.load();
    v.s.by_launcher = CoopPause::ByLauncher();
    v.s.failed = st.state == HOS_COOP_FAILED;
    // (still starting: a host's rows already, not a guest's "Leave" for a moment)
    v.s.hosting = st.state == HOS_COOP_HOSTING || (st.state == HOS_COOP_STARTING && CoopPause::HostStarted());
    v.s.in_one = (st.state != HOS_COOP_IDLE && st.state != HOS_COOP_FAILED) || CoopSave::SessionLive();
    v.connecting = st.state == HOS_COOP_STARTING || st.state == HOS_COOP_JOINING || st.state == HOS_COOP_WAIT_HOST;
    std::string room = st.relay_room[0] != 0 ? st.relay_room : st.relay_code[0] != 0 ? st.relay_code : "";
    if (const auto at = room.find('@'); at != std::string::npos) room.resize(at);   // (the name friends type first)
    v.room = room;
    v.s.code = v.s.hosting && (!room.empty() || st.invite[0] != 0);
    v.s.invite = v.s.hosting && room.empty() && st.invite[0] != 0;
    v.s.wait_host = st.state == HOS_COOP_WAIT_HOST;
    v.s.can_resync = !v.s.hosting && CoopSave::CanResync();
    // (a guest in: co-op's status line is the join's old "connecting through it", left on the details list as three
    // lines of text; only while joining, waiting or failed it says something)
    const bool guest_in = v.s.in_one && !v.s.hosting && st.state == HOS_COOP_CONNECTED;
    if (!guest_in) v.why = std::string(st.message, strnlen(st.message, sizeof st.message));
    v.problem = st.problem;
    v.port_open = v.s.hosting && st.upnp_open != 0;
    v.port = v.s.hosting ? st.port : 0;
    v.note = CoopPause::PageNote();
    if (const std::string& title = CoopWindowBrand::TitleOf(CoopRuntime::GameDir()); title != CoopWindowBrand::kTitle) {
        v.version = title;   // the kit's release (0.11.6), as the title bar
    }
    v.confirm_leave = Confirming(View::kHost, kHostRowStop) || Confirming(View::kJoin, kJoinRowLeave);
    v.confirm_kick = Confirming(View::kPlayerHost, 2);
    v.confirm_ban = Confirming(View::kPlayerHost, 3);
    v.confirm_click = g_confirm_by_mouse.load();
    const CoopLobby::Seen seen = CoopLobby::Now();
    if (!v.s.hosting && !seen.refused.empty()) {   // (turned away: co-op's menu says "Press Leave, then join again")
        v.turned_away = CoopTitleMenu::ShortReason(seen.refused);
        v.banned = seen.banned;
    } else if (CoopTitleMenu::ShownState(st.state, v.s.hosting, seen.connected) == HOS_COOP_JOINING) {
        // (told to connect, not yet taken in: still joining, as co-op's menu and the Lobby tab say; a host's approval
        // can take a while. A run showed "In a session" while the host had not let this game in yet)
        v.connecting = true;
    }
    v.players = seen.connected ? static_cast<int>(seen.players.size()) : 0;
    v.s.listed = v.players;
    v.s.page = g_players_page.load();
    if (seen.connected) {
        for (const CoopLobby::PlayerSeen& p : seen.players) {
            if (p.self && v.s.self < 0) v.s.self = static_cast<int>(&p - seen.players.data());
            if (view == View::kPlayers) v.names.push_back(p.name + (p.self ? " (you)" : p.host ? " (host)" : ""));
            if (p.host && !p.self) v.host_name = p.name;
        }
    }
    if (const CoopLobby::PlayerSeen* p = ShownPlayer(seen)) {
        v.s.player = p->self ? 1 : 2;
        v.player_name = p->name;
        v.own_mutes = p->own_mutes;
        v.host_mutes = p->host_mutes;
    }
    if (view == View::kMain && !v.s.in_one && !v.s.by_launcher) {
        v.room_name = CoopPause::RoomName();
        v.s.list_room = CoopPause::ListRoom();
        v.s.auto_rejoin = CoopPause::AutoRejoin();
        v.s.rejoining = v.s.auto_rejoin && CoopPause::AutoRejoining();
    }
    if (view == View::kLan) {
        v.search = BrowseSearch();
        v.s.browse_filter = g_browse_filter.load(), v.s.browse_sort = g_browse_sort.load();
        const auto games = SearchedGames(v.search);
        v.s.lan_listed = static_cast<int>(games.size());
        v.s.lan_page = g_lan_page.load();
        for (std::size_t i = 0; i < games.size(); ++i) {
            const CoopLan::Heard& h = games[i];
            v.lan_rows.push_back(CoopLan::RowText(h, coop::protocol::kProtocolVersion));
            v.lan_list.push_back(ListLine{h.game.name + "'s game" + (h.game.room.empty() ? "" : ": " + h.game.room),
                                          ListValue(h.game.players, h.game.max,
                                                    h.game.protocol == coop::protocol::kProtocolVersion)});
            if (i < 32 && CoopLan::Joinable(h, coop::protocol::kProtocolVersion)) v.s.lan_joinable |= 1u << i;
        }
        const std::vector<CoopRoomList::Room> rooms = SearchedRooms(v.search);
        for (const CoopRoomList::Room& r : rooms) {
            v.online_rows.push_back(CoopRoomList::CellText(r, coop::protocol::kProtocolVersion));
            // (the same words as the Lobby tab's Public games: CoopRoomList::DetailText)
            v.online_list.push_back(ListLine{ListName(r.host, r.room),
                                             CoopI18n::Tr(CoopRoomList::DetailText(r, coop::protocol::kProtocolVersion))});
        }
        const CoopRoomList::Snapshot all = CoopRoomList::Now();
        v.online_state = all.state.empty() ? std::string() : CoopI18n::Tr(all.state);
        v.s.online_listed = static_cast<int>(rooms.size());
        v.s.online = g_online.load();
        g_browse_listed = BrowseListed(v.s);
        v.browse_all_out = v.s.online ? rooms.empty() && !all.rooms.empty()
                                      : games.empty() && !CoopLanNet::Games(NowMs()).empty();
        v.s.cell_page = g_cell_page.load();
        v.s.list_room = CoopPause::ListRoom();   // (the files' setting; read only while this view shows)
        v.relay_set = CoopPause::RelaySet();   // (no relay: the online list's line types one)
        v.cell = g_cell.load();
        // (the relay asked again while the Online list shows: at most every CoopRoomList::kEveryMs, from its own thread)
        CoopRoomList::Look(CoopRoomList::Viewer::kFindAGame, v.s.online && !v.s.in_one && !v.s.by_launcher, NowMs(),
                           CoopPause::Relay());
    }
    if (view == View::kJoin) {
        v.s.rejoin = CoopPause::Rejoin(&v.rejoin_host);
        v.relay_set = CoopPause::RelaySet();   // (co-op's own lock: before ours, never inside it)
    }
    if (view == View::kSession || view == View::kMain || view == View::kPassword) {   // (the details list)
        v.s.has_password = v.s.hosting && st.password[0] != 0;
        v.s.password_shown = v.s.has_password && g_show_password.load();
        if (v.s.password_shown) v.host_password.assign(st.password, strnlen(st.password, sizeof st.password));
    }
    if (view == View::kSession) {
        std::lock_guard lock(g_says_mutex);
        v.says = g_says;
    }
    if (view == View::kJoin || view == View::kLanGame) {
        std::lock_guard lock(g_mutex);   // (the fields are the page's; no other lock of ours is held here)
        v.typing = g_page.Typing();
        v.room_field = g_page.Field(0).Shown(v.typing == 0, kRowChars - (v.typing == 0 ? 12 : 4));   // ("Room or IP: " typing, else "To: ")
        v.join_address = g_page.Field(0).text().find_first_of(".:") != std::string::npos;
        if (const std::string& f = g_page.Field(0).text(); v.join_address) {
            const auto colon = f.rfind(':');
            v.join_port = colon != std::string::npos ? std::atoi(f.c_str() + colon + 1) : coop::protocol::kDefaultPort;
        }
        v.password_field = g_page.Field(1).Shown(v.typing == 1, kRowChars - 10);  // ("Password: ")
        v.room_ready = v.s.room_ready = g_page.Field(0).Ready();
        v.s.lan_password = !g_page.Field(1).text().empty();
        v.lan_game = g_lan_pick.row;
    }
    {
        std::lock_guard lock(g_mutex);   // (the page's typing: the list's rows are its keys while a field is typed into)
        if (const int t = g_page.Typing(); t >= 0) {
            v.typing_lines = TypingLines(t, g_page.Field(t), g_page.TypingLetter());
            v.typing_key = g_page.TypingKey();
        } else if (!g_menu_typing_lines.empty()) {   // (co-op's menu types into its row: Settings and look)
            v.typing_lines = g_menu_typing_lines;
            v.typing_key = g_menu_key;
        }
    }
    if (v.s.hosting) v.asking = CoopLobby::Asking();
    v.s.asking = !v.asking.empty();
    return v;
}

// The rows' live texts while our page shows: at most every 250 ms (asked before co-op's status and the Lobby are read,
// so they are not copied every frame), at once for a new opening of the pages; a pane only when its words changed.
std::atomic<bool> g_live_poke{false};   // a field changed: the rows are due at their next update

bool LiveDue(std::uint64_t gen) {
    std::lock_guard lock(g_live_mutex);
    const std::uint64_t now = NowMs();
    if (gen != g_live_gen) {
        g_live_gen = gen;
        for (LiveSlot& l : g_live) l.shown.clear();   // (a new opening: its panes show the pack's words again)
    } else if (now - g_live_ms < 250 && !g_live_poke.exchange(false)) {
        return false;
    }
    g_live_ms = now;
    return true;
}

// The game browser's list (View::kLan): the header and a page of games in the pack's Options rows (Pa_List_00..06 in our
// page's own layout), each its name on the left and its players on the right; the arrows between which an Options row
// shows its value hidden. The row picked bright and the others dimmed while the pad or the mouse is in the list; all
// hidden on every other view. Looked up once an opening; an older pack has none, and the browser lists nothing then.
// g_live_mutex.
std::uint64_t g_cells_gen = 0;
// (the list's panes: kListRows, and the eighth, Pa_List_07, only Settings and look shows: kMenuMost a page)
constexpr int kLinePanes = kMenuMost + 1;
std::array<std::uint64_t, kLinePanes> g_list_panes{};
std::array<int, kLinePanes> g_list_vis{};
std::array<float, kLinePanes> g_list_alpha{};
std::array<float, kLinePanes> g_list_home{};   // their own x (+0x6c) as the layout puts them (Settings and look moves them)
// Our page's five rows: dimmed while the list has the pad (the game's cursor frame stays on its row there); on the
// browser, the list not shown dimmed too, as a tab.
std::array<std::uint64_t, kRows> g_row_panes{};
std::array<float, kRows> g_rows_alpha{};
std::atomic<bool> g_rows_alpha_stale{false};   // (a moment's hiding changed one: LiveCells sets them all again)
std::uint64_t g_rows_keep = 0;   // (the button left shown in a moment's hiding, see-through: HideRowsAMoment)
// Each list row's value box (W_Base_01 and W_Base_03 in PaOptionBtnLevel_00): shown for a line with a value, not for a
// line of text (the details' header and their problem or hint), as Longshot Settings' header.
std::array<std::array<std::uint64_t, 2>, kLinePanes> g_list_box{};
std::array<std::array<bool, 2>, kLinePanes> g_list_box_own{};   // (as the layout has them: shown again so)
std::array<int, kLinePanes> g_list_box_vis{};
// Each list row's own value arrows (N_Content_00): shown only on a line that changes with Left and Right (ListLine::
// arrows: the typing keys' Letter).
std::array<std::uint64_t, kLinePanes> g_list_arrows{};
std::array<int, kLinePanes> g_list_arrows_vis{};
// Settings and look: the list moved left, beside the page's rows, so the menu's Link stands on the right (a tester: the
// model on the right, the options on the left, not so wide). In the layout's units (3840 across).
constexpr float kMenuListShift = -400.0f;   // (-520 overlapped the rows' panel, a tester)
std::atomic<int> g_menu_per_page{kCells};   // (seven with the pack's eighth list row)

// The rows with nothing on them not shown; the game's cursor on one goes on to the nearest shown (RowSkip, OnPad).
// g_live_mutex.
void ApplyBlankRows(void* ctx) {
    if (g_rows_moment.load()) return;
    const std::uint32_t blank = g_rows_blank.load();
    for (int r = 0; r < kRows; ++r) {
        const std::uint64_t pane = g_row_panes[static_cast<std::size_t>(r)];
        if (pane == 0) continue;
        const bool want = (blank & (1u << r)) == 0;
        if (((Read<std::uint32_t>(pane + 0xa8) & 1u) != 0) != want) {
            SetVisible(ctx, pane, want);
        }
    }
}

void PlacePane(void* ctx, std::uint64_t pane, float x);   // (below: the pages' slide's)

void LiveCells(void* ctx, std::uint64_t layout, std::uint64_t gen, const SessionView& v) {
    if (g_cells_gen != gen) {
        g_cells_gen = gen;
        std::uint64_t table = 0;
        Call(ctx, kPaneTable, layout + 0x40, 0, 0, 0, 0, &table);
        const std::uint64_t ours = PartLayout(table, "Pa_Coop_00");
        int found = 0;
        for (int i = 0; i < kLinePanes; ++i) {
            char name[16];
            std::snprintf(name, sizeof name, "Pa_List_%02d", i);
            const std::uint64_t pane = PaneIn(ctx, ours, name);
            const std::size_t k = static_cast<std::size_t>(i);
            g_list_panes[k] = IsPart(pane) ? pane : 0;
            if (i > 0) g_cell_pane_at[k - 1] = g_list_panes[k];
            if (i == 0) g_head_pane_at = g_list_panes[k];
            g_list_vis[k] = -1, g_list_alpha[k] = -1, g_list_box_vis[k] = -1, g_list_box[k] = {};
            g_list_arrows[k] = 0, g_list_arrows_vis[k] = 0;
            g_list_home[k] = IsPart(pane) ? Read<float>(pane + 0x6c) : 0.0f;
            if (!IsPart(pane)) continue;
            if (i < kListRows) ++found;
            // (inside the row: the value's arrows and the check mark an Options row has, never ours; its value box)
            const std::uint64_t row = PartLayout(ours, name);
            for (const char* inner : {"N_Content_00", "P_CheckMark_00"}) {
                if (const std::uint64_t in = PaneIn(ctx, row, inner); in != 0) SetVisible(ctx, in, false);
            }
            g_list_arrows[k] = PaneIn(ctx, row, "N_Content_00");
            g_list_box[k] = {PaneIn(ctx, row, "W_Base_01"), PaneIn(ctx, row, "W_Base_03")};
            for (std::size_t j = 0; j < 2; ++j) {
                const std::uint64_t b = g_list_box[k][j];
                g_list_box_own[k][j] = b != 0 && (Read<std::uint32_t>(b + 0xa8) & 1u) != 0;
            }
        }
        for (int r = 0; r < kRows; ++r) {
            char name[16];
            std::snprintf(name, sizeof name, "Pa_Button_%02d", r);
            const std::uint64_t pane = PaneIn(ctx, ours, name);
            g_row_panes[static_cast<std::size_t>(r)] = IsPart(pane) ? pane : 0;
        }
        g_rows_alpha.fill(-1);
        g_menu_per_page = g_list_panes[kLinePanes - 1] != 0 ? kMenuMost : kCells;
        LOG_INFO(Core, "coop: pause tab: the game browser's list rows: {} of {}{}{}", found, kListRows,
                 found == kListRows ? "" : " (build the puppet pack again for the browser)",
                 g_list_panes[kLinePanes - 1] != 0 ? "; the eighth for Settings and look" : "");
    }
    // (the browser's games; on the main view and the Session view the session's details, DetailLines)
    const View view = ViewShown();
    // (a field typed into: the list is its keys, TypingLines, whatever the view; the page's own fields and Settings
    // and look's alike)
    const bool keys = !v.typing_lines.empty();
    const bool browse = !keys && view == View::kLan;
    const bool menu = view == View::kMenu;
    MenuList ml;
    if (menu) {   // (co-op's menu's page: its rows; the lines' menu rows kept for the pad and the mouse)
        ml = MenuLines(v.menu, v.menu_tab, v.menu_top, v.s.menu_list, g_menu_per_page.load());
        // (kept within the rows, or with the selected row in view; not over a header click made since the view was
        // read: a review's lost update)
        int was = v.menu_top;
        g_menu_top.compare_exchange_strong(was, ml.top);
        for (std::size_t k = 0; k < static_cast<std::size_t>(kLinePanes); ++k) {
            g_menu_line_row[k] = keys ? -1 : ml.rows[k], g_menu_line_pick[k] = !keys && ml.selectable[k];
        }
    }
    std::vector<ListLine> lines = keys ? v.typing_lines : browse ? ListLines(v) : menu ? ml.lines : DetailLines(view, v);
    lines.resize(kLinePanes);   // (the eighth: Settings and look's only)
    // (the list beside the rows on Settings and look, where it was on the others)
    for (std::size_t k = 0; k < static_cast<std::size_t>(kLinePanes); ++k) {
        if (g_list_panes[k] != 0) PlacePane(ctx, g_list_panes[k], g_list_home[k] + (menu ? kMenuListShift : 0.0f));
    }
    int games = 0;
    for (int c = 0; c < kCells; ++c) games += CellIndex(v.s, c) >= 0 ? 1 : 0;
    g_cell_games = browse ? games : 0;
    const int picked = keys ? v.typing_key : browse ? g_cell.load() : menu ? ml.picked : -1;
    for (int i = 0; i < kLinePanes; ++i) {
        const std::size_t k = static_cast<std::size_t>(i);
        const std::uint64_t pane = g_list_panes[k];
        if (pane == 0) continue;
        const bool on = !lines[k].name.empty();
        if (on) {
            // (the live text slots: 8..21 the first seven lines' name and value, 44 and 45 the eighth's)
            const int name_slot = i < kListRows ? 8 + 2 * i : 44, value_slot = i < kListRows ? 9 + 2 * i : 45;
            char path[64];
            std::snprintf(path, sizeof path, "Pa_Coop_00/Pa_List_%02d/T_Option_00", i);
            // The browser's own words (the header, "Looking for games...", a relay's state) in the
            // player's language; a game's name is the player's own and comes back unchanged.
            LiveText(ctx, layout, gen, path, name_slot, CoopI18n::Tr(lines[k].name));
            std::snprintf(path, sizeof path, "Pa_Coop_00/Pa_List_%02d/T_Text_00", i);
            LiveText(ctx, layout, gen, path, value_slot,
                     lines[k].value.empty() ? std::string(" ") : CoopI18n::Tr(lines[k].value));
            if (const int box = lines[k].value.empty() ? 0 : 1; g_list_box_vis[k] != box) {
                for (std::size_t j = 0; j < 2; ++j) {   // (with a value: as the layout has them; a line of text: none)
                    if (const std::uint64_t b = g_list_box[k][j]; b != 0) SetVisible(ctx, b, box != 0 && g_list_box_own[k][j]);
                }
                g_list_box_vis[k] = box;
            }
        }
        if (g_list_vis[k] != (on ? 1 : 0)) SetVisible(ctx, pane, on), g_list_vis[k] = on ? 1 : 0;
        if (const int arrows = on && lines[k].arrows ? 1 : 0; g_list_arrows[k] != 0 && g_list_arrows_vis[k] != arrows) {
            SetVisible(ctx, g_list_arrows[k], arrows != 0), g_list_arrows_vis[k] = arrows;
        }
        // (the header a little dim, as a table's; a game bright when picked, or all of them when the rows have the pad;
        // the details, which nothing picks, as Longshot Settings' lines not picked: an Options row's value box is the
        // game's bright orange, dimmed to its brown)
        const float alpha = keys                            ? (i == 0 || picked == i - 1 ? 1.0f : 0.45f)
                            : i == 0                        ? 0.7f
                            : menu && !ml.selectable[k]     ? 0.55f   // (a heading, a note)
                            : !browse && !menu              ? 0.55f
                            : picked < 0 || picked == i - 1 ? 1.0f
                                                            : 0.45f;
        if (on && g_list_alpha[k] != alpha) CallAlpha(ctx, pane, alpha), g_list_alpha[k] = alpha;
    }
    if (!g_rows_moment.load() && g_rows_alpha_stale.exchange(false)) g_rows_alpha.fill(-1);
    for (int r = 0; r < kRows; ++r) {
        const std::size_t k = static_cast<std::size_t>(r);
        const bool other_list = false;   // (one List row now: nothing dimmed for the list not shown)
        const float alpha = picked >= 0 ? 0.45f : other_list ? 0.55f : 1.0f;
        if (g_rows_moment.load()) break;   // (a moment's hiding: SystemButtons sets them back, then they are set again)
        if (g_row_panes[k] != 0 && g_rows_alpha[k] != alpha) CallAlpha(ctx, g_row_panes[k], alpha), g_rows_alpha[k] = alpha;
    }
}

// --- The Settings page (the sixth tab: Longshot's options; coop_pause_tab.h "The Settings page") ---
// The category is the row the game's cursor is on while the rows have the pad; the list's page and its row picked
// (-1: the rows have the pad). Any thread.
std::atomic<int> g_set_category{0}, g_set_page{0}, g_set_row{-1};
std::atomic<int> g_set_count{0};   // the category's settings (the list's length)
// The rows' cursor as last read on the Settings page, in this opening of the pages: when it moves (the pad, or the
// mouse's steps), the category follows and the rows have the pad. g_mutex.
std::uint64_t g_set_seen_gen = 0;
int g_set_seen_row = -1;
constexpr std::uint64_t kMouseAMs = 400;   // an A this soon after the left button was down is the mouse's
std::uint64_t g_set_left_ms = 0;          // the left button last seen down on Longshot Settings. g_mutex.
std::uint64_t g_list_focus_fix_ms = 0;    // a Left last sent to bring the game's cursor back from the list. g_mutex.
// (that Left lands on the category beside the list's row, not the one shown: the category kept, and the steps back to
// it until then. A test run 2026-11-20: a click on the last setting's arrow turned the category to Debug. g_mutex)
std::atomic<std::uint64_t> g_key_end_ms{0};
std::uint64_t g_set_enter_ms = 0;   // when the settings list was entered (Right or A on a category). g_mutex   // when the PC's Enter or Esc last ended the page's typing
bool g_typing_seen = false;          // (the page typed at the last pad read; when typing last ended. g_mutex)
std::uint64_t g_typing_end_ms = 0;
// Back to the title (ToTitle): 0 none; 1 the pause menu to System and its cursor to Title Screen; 2 Title Screen
// pressed, the question awaited; 3 the question answered. With when it began, the last tap, and the pages as last seen.
std::atomic<int> g_to_title{0};
std::atomic<std::uint64_t> g_to_title_ms{0}, g_to_title_phase_ms{0};
std::uint64_t g_tap = 0, g_tap_until = 0, g_tap_next = 0;   // (a button held for a tap, then let go; g_mutex)
std::atomic<int> g_tap_downs{0};   // (the steps toward Title Screen so far)
std::atomic<std::uint64_t> g_pages_seen_ms{0};
std::atomic<int> g_pages_seen_state{-1}, g_pages_seen_page{-1};
std::atomic<std::uint64_t> g_test_tab_in{0}, g_test_tab_out{0};   // (the last pad with buttons, in and out: TestState)
int g_set_keep = -1;
float g_set_mouse_x = 0, g_set_mouse_y = 0;   // (the mouse's last place, when it last moved: settings)
std::uint64_t g_set_mouse_ms = 0;
std::uint64_t g_set_keep_until = 0, g_set_keep_step_ms = 0;
std::uint64_t g_set_click_ms = 0;          // our click on a setting (its arrow or its row): Longshot's A after it is not one
// Longshot's settings as last read (every 2 s while the page shows, and after a change). g_set_mutex.
std::mutex g_set_mutex;
std::vector<SettingRow> g_set_all;
std::uint64_t g_set_read_ms = 0;

std::vector<SettingRow> SettingsNow(std::uint64_t now_ms, bool fresh = false) {
    std::lock_guard lock(g_set_mutex);
    if (fresh || g_set_read_ms == 0 || now_ms - g_set_read_ms > 2000) {
        g_set_all.clear();
        for (const CoopLongshotSettings::Setting& x : CoopLongshotSettings::All()) {
            g_set_all.push_back(SettingRow{x.key, x.name, x.text, x.tab, x.value, x.lo, x.hi, x.step, x.restart, x.fixed});
            // (Debug tools turns on only in Longshot's own menu, which asks first: set through the bridge it only raises
            // that question there. Shown, dimmed, as one with nothing to choose here)
            if (x.key == "debug_tools") g_set_all.back().fixed = true, g_set_all.back().name = "Debug tools (F1 menu only)";
        }
        g_set_read_ms = now_ms;
    }
    return g_set_all;
}

// A setting moved as a press asks (SettingNext), on past a value Longshot does not take here (a list row walks only to
// the choices this PC has: the Upscaler to its engines): each next step that way, until one changes the setting. Nothing
// when no step does (its end, or nothing else here).
std::optional<CoopLongshotSettings::Setting> StepSetting(const SettingRow& r, int dir) {
    SettingRow at = r;
    for (int tries = 0; tries < 64; ++tries) {
        const int want = SettingNext(at, dir);
        if (want == at.value || want == r.value) return std::nullopt;
        if (auto after = CoopLongshotSettings::Set(r.key, want); after && after->value != r.value) return after;
        at.value = want;
    }
    return std::nullopt;
}

// (test, COOP_TEST_LONGSHOT_SETTINGS=1 only) Once a run, as Longshot Settings first shows: every setting Longshot lists
// is found in one category, and each one that can change is set a step (Right, or Left at its end) the way the page
// sets it, checked, and set back, checked; the log says each one and a summary. On a thread of its own: each set saves
// Longshot's options file. A tester's options are theirs to back up first (a Quality preset moved and put back sets
// its rows again).
std::atomic<bool> g_settings_test_ran{false};
void TestSettingsOnce() {
    const char* on = std::getenv("COOP_TEST_LONGSHOT_SETTINGS");
    if (on == nullptr || std::string_view(on) != "1" || g_settings_test_ran.exchange(true)) return;
    std::thread([] {
        const std::vector<CoopLongshotSettings::Setting> all = CoopLongshotSettings::All();
        std::vector<SettingRow> rows;
        for (const auto& x : all) {
            rows.push_back(SettingRow{x.key, x.name, x.text, x.tab, x.value, x.lo, x.hi, x.step, x.restart, x.fixed});
            if (x.key == "debug_tools") rows.back().fixed = true;   // (as SettingsNow: Longshot's own menu only)
        }
        int changed = 0, fixed = 0, failed = 0, nowhere = 0;
        std::string per;
        for (int c = 0; c < kSettingRows; ++c) {
            per += std::string(" ") + kSettingTabs[static_cast<std::size_t>(c)] + " " +
                   std::to_string(SettingsOf(rows, c).size());
        }
        for (const SettingRow& r : rows) {
            int in = 0;
            for (int c = 0; c < kSettingRows; ++c) {
                for (const SettingRow& o : SettingsOf(rows, c)) in += o.key == r.key ? 1 : 0;
            }
            if (in != 1) {
                ++nowhere;
                LOG_WARNING(Core, "coop: settings test: {} (tab {}) is in {} categories", r.key, r.tab, in);
            }
            if (r.fixed || r.hi <= r.lo) {
                ++fixed;
                LOG_INFO(Core, "coop: settings test: {} ({}, {}): {} - nothing to choose here", r.key, r.tab, r.name, r.text);
                continue;
            }
            auto there = StepSetting(r, 1);   // (as the page's Right, or Left at its end)
            if (!there) there = StepSetting(r, -1);
            const auto back = CoopLongshotSettings::Set(r.key, r.value);
            const bool went = there && there->value != r.value, returned = back && back->value == r.value;
            if (went && returned) {
                ++changed;
            } else {
                ++failed;
            }
            LOG_INFO(Core, "coop: settings test: {} ({}, {}): {} -> {} -> {}: {}", r.key, r.tab, r.name, r.text,
                     there ? there->text : std::string("(refused)"), back ? back->text : std::string("(refused)"),
                     went && returned ? "ok" : !went ? "NOT CHANGED" : "NOT PUT BACK");
        }
        LOG_INFO(Core, "coop: settings test: {} settings: {} changed and put back, {} with nothing to choose, {} failed, {} "
                       "not in exactly one category; by category:{}",
                 rows.size(), changed, fixed, failed, nowhere, per);
        g_live_poke = true;
    }).detach();
}

// A setting of the category's list set as a press asks (dir: -1 Left, +1 Right, 0 A). After the lock, as Act.
void ChangeSetting(int index, int dir) {
    const std::vector<SettingRow> rows = SettingsOf(SettingsNow(NowMs()), g_set_category.load());
    if (index < 0 || index >= static_cast<int>(rows.size())) return;
    const SettingRow& r = rows[static_cast<std::size_t>(index)];
    if (r.fixed || SettingNext(r, dir) == r.value) return;
    const auto after = StepSetting(r, dir);
    LOG_INFO(Core, "coop: pause tab: settings: {} from {} to {}{}{}", r.key, r.text, after ? after->text : r.text,
             after ? "" : " (Longshot took no step that way)", after && after->restart ? " (after a restart)" : "");
    SettingsNow(NowMs(), true);
    g_live_poke = true;
}

// The Settings page's words: its rows (the categories) and the list (a page of the category's settings), its picked
// row bright and the others dimmed while the list has the pad, a fixed one dimmed always, the rows dimmed then too.
// Live texts 22..26 and 41 (the six rows) and 27..40 (the list: a name and a value each). g_live_mutex.
std::uint64_t g_set_gen = 0;
std::array<std::uint64_t, kSettingListRows> g_set_list{};
std::array<std::uint64_t, kSettingRows> g_set_rows{};
std::array<int, kSettingListRows> g_set_vis{};
std::array<float, kSettingListRows> g_set_alpha{};
// Settings a page: seven when the pack's page has its eighth list row (Pa_List_07), else six. Any thread.
std::atomic<int> g_set_per_page{kSettingsPerPage};
// The live text slots of the list's row i: its name, its value (27..40 for the first seven, 42 and 43 the eighth's).
int SetNameSlot(int i) { return i < 7 ? 27 + 2 * i : 42; }
int SetValueSlot(int i) { return i < 7 ? 28 + 2 * i : 43; }
std::array<float, kSettingRows> g_set_rows_alpha{};

void LiveSettings(void* ctx, std::uint64_t layout, std::uint64_t gen) {
    if (g_settings_pane == 0) return;
    if (g_set_gen != gen) {
        g_set_gen = gen;
        std::uint64_t table = 0;
        Call(ctx, kPaneTable, layout + 0x40, 0, 0, 0, 0, &table);
        const std::uint64_t page = PartLayout(table, "Pa_Settings_00");
        for (int i = 0; i < kSettingListRows; ++i) {
            char name[16];
            std::snprintf(name, sizeof name, "Pa_List_%02d", i);
            const std::uint64_t pane = PaneIn(ctx, page, name);
            g_set_list[static_cast<std::size_t>(i)] = IsPart(pane) ? pane : 0;
            g_set_vis[static_cast<std::size_t>(i)] = -1, g_set_alpha[static_cast<std::size_t>(i)] = -1;
            if (const std::uint64_t mark = PaneIn(ctx, PartLayout(page, name), "P_CheckMark_00"); mark != 0) {
                SetVisible(ctx, mark, false);   // (an Options row's check mark: never ours)
            }
            if (i == 0) {   // (the header: no value box and no arrows, so it never looks like a setting with no value)
                for (const char* inner : {"W_Base_01", "W_Base_03", "N_Content_00"}) {
                    if (const std::uint64_t in = PaneIn(ctx, PartLayout(page, name), inner); in != 0) SetVisible(ctx, in, false);
                }
            }
        }
        for (int r = 0; r < kSettingRows; ++r) {
            char name[16];
            std::snprintf(name, sizeof name, "Pa_Button_%02d", r);
            const std::uint64_t pane = PaneIn(ctx, page, name);
            g_set_rows[static_cast<std::size_t>(r)] = IsPart(pane) ? pane : 0;
            g_set_rows_alpha[static_cast<std::size_t>(r)] = -1;
        }
    }
    for (int r = 0; r < kSettingRows; ++r) {
        char path[64];
        std::snprintf(path, sizeof path, "Pa_Settings_00/Pa_Button_%02d/T_Btn_00", r);
        LiveText(ctx, layout, gen, path, r < kRows ? 22 + r : 41, kSettingTabs[static_cast<std::size_t>(r)]);
    }
    const std::vector<SettingRow> rows = SettingsOf(SettingsNow(NowMs()), g_set_category.load());
    g_set_count = static_cast<int>(rows.size());
    const int page = g_set_page.load(), picked = g_set_row.load(), category = g_set_category.load();
    const int per_page = g_set_list[kSettingListRows - 1] != 0 ? kSettingsPerPageMost : kSettingsPerPage;
    g_set_per_page = per_page;
    const std::vector<ListLine> lines = SettingLines(
        rows, page, category >= 0 && category < kSettingRows ? kSettingTabs[static_cast<std::size_t>(category)] : "", per_page);
    const int pages = SettingPages(static_cast<int>(rows.size()), per_page), at_page = (page % pages + pages) % pages;
    for (int i = 0; i < kSettingListRows; ++i) {
        const std::size_t k = static_cast<std::size_t>(i);
        const std::uint64_t pane = g_set_list[k];
        if (pane == 0) continue;
        const bool on = !lines[k].name.empty();
        if (on) {
            char path[64];
            std::snprintf(path, sizeof path, "Pa_Settings_00/Pa_List_%02d/T_Option_00", i);
            LiveText(ctx, layout, gen, path, SetNameSlot(i), lines[k].name);
            std::snprintf(path, sizeof path, "Pa_Settings_00/Pa_List_%02d/T_Text_00", i);
            LiveText(ctx, layout, gen, path, SetValueSlot(i), lines[k].value.empty() ? std::string(" ") : lines[k].value);
        }
        if (g_set_vis[k] != (on ? 1 : 0)) SetVisible(ctx, pane, on), g_set_vis[k] = on ? 1 : 0;
        // (the header a little dim, as the browser's; a setting bright when picked, or all while the rows have the pad)
        const std::size_t at = static_cast<std::size_t>(at_page * per_page + i - 1);
        const bool fixed = i > 0 && at < rows.size() && rows[at].fixed;
        const float alpha = i == 0 ? 0.7f : fixed ? 0.4f : picked < 0 || picked == i - 1 ? 1.0f : 0.5f;
        if (on && g_set_alpha[k] != alpha) CallAlpha(ctx, pane, alpha), g_set_alpha[k] = alpha;
    }
    for (int r = 0; r < kSettingRows; ++r) {
        const std::size_t k = static_cast<std::size_t>(r);
        const float alpha = picked >= 0 ? 0.45f : 1.0f;
        if (g_set_rows[k] != 0 && g_set_rows_alpha[k] != alpha) CallAlpha(ctx, g_set_rows[k], alpha), g_set_rows_alpha[k] = alpha;
    }
}

// The settings list's rows where the game puts them now (as ReadCellBoxes: an Options row's bar, 1880 wide), and the
// x of a row's two arrows from its centre (PaOptionBtnLevel_00: its value at +500, the arrows 360 either side of it).
constexpr float kArrowLeftX = 140.0f, kArrowRightX = 860.0f, kArrowReach = 90.0f;
std::array<RowBox, kSettingListRows> ReadSettingBoxes() {
    std::array<RowBox, kSettingListRows> boxes{};
    for (int i = 0; i < kSettingListRows; ++i) {
        const std::uint64_t pane = g_set_list[static_cast<std::size_t>(i)];
        float m[12] = {};
        if (pane == 0 || !CoopRuntime::Read(pane + 0x28, m, sizeof m) || !(m[0] > 1e-3f) || !(m[4] > 1e-3f)) continue;
        if ((Read<std::uint32_t>(pane + 0xa8) & 1u) == 0) continue;
        boxes[static_cast<std::size_t>(i)] = RowBox{true, m[9] - 940.0f * m[0], m[10] - 70.0f * m[4],
                                                    m[9] + 940.0f * m[0], m[10] + 70.0f * m[4]};
    }
    return boxes;
}

void LiveRows(void* ctx, std::uint64_t layout, std::uint64_t gen, const SessionView& v) {
    std::lock_guard lock(g_live_mutex);
    if (layout == 0 || gen != g_live_gen) return;   // (another opening came meanwhile: its own turn sets them)
    const std::vector<std::string> rows = RowTexts(ViewShown(), v);
    std::uint32_t blank = 0;
    for (int i = 0; i < kRows && i < kLiveSlots; ++i) {
        char path[64];
        std::snprintf(path, sizeof path, "Pa_Coop_00/Pa_Button_%02d/T_Btn_00", i);
        // The row's words in the player's language (coop_i18n.h). The rows are built in English in
        // coop_pause_tab.cpp (its tests read them so), and each whole row is looked up here, as it is
        // handed to the game: the rows with a value ("Room: 3", "Kick Saria") are the table's shapes.
        const std::string& text = rows[static_cast<std::size_t>(i)];
        if (text.empty()) blank |= 1u << i;
        LiveText(ctx, layout, gen, path, i, text.empty() ? std::string(" ") : CoopI18n::Tr(text));   // (a pane's text is never empty)
    }
    g_rows_blank = blank;
    LiveCells(ctx, layout, gen, v);
    ApplyBlankRows(ctx);   // (after LiveCells: it finds the rows' panes)
    if (g_page.SettingsUsable()) LiveSettings(ctx, layout, gen);
}

// The pages' panes, looked up once per opening (`calls`: ctx may call(); else looked up at a later update).
void LookPages(void* ctx, bool calls, const Facts& f, std::uint64_t gen) {
    if (gen != g_pages_gen) {
        g_pages_gen = gen, g_pages_screen = f.screen, g_pages_looked = false, g_pages_ok = false;
        g_system_pane = 0, g_coop_pane = 0;
        g_settings_button = g_settings_button_coop = g_settings_button_set = 0, g_settings_button_shown = -1, g_set_buttons = {};
        g_mp_button = g_mp_button_coop = g_mp_button_set = 0, g_coop_buttons = {}, g_sys_buttons = {};
        g_cl_button = g_cl_button_coop = g_cl_button_set = 0;
        g_sys_hidden = false;   // (a new opening: System's buttons as the game shows them)
        g_rows_keep = 0;        // (an older opening's pane: never called on)
    }
    if (g_pages_looked || !calls || f.layout == 0) return;
    g_pages_looked = true;
    g_system_pane = FindOwnPane(ctx, f.layout, "Pa_System_00");
    // (0.11.9, the owner: one home per thing: co-op's pages are the - menu's tabs, so the + menu has no Multiplayer
    // page and System no Multiplayer or Customize Link button; Longshot Settings is System's one page of ours)
    g_coop_pane = 0;
    g_pages_ok = IsPart(g_system_pane);
    g_settings_pane = FindOwnPane(ctx, f.layout, "Pa_Settings_00");
    if (!IsPart(g_settings_pane)) g_settings_pane = 0;
    // System's two buttons of ours and the same two in our pages, the pages' own rows, and System's own lower buttons
    // with their heights
    {
        std::uint64_t table = 0;
        Call(ctx, kPaneTable, f.layout + 0x40, 0, 0, 0, 0, &table);
        const auto button = [&](const char* page, const char* name) {
            const std::uint64_t b = PaneIn(ctx, PartLayout(table, page), name);
            return IsPart(b) ? b : 0;
        };
        g_settings_button = button("Pa_System_00", "Pa_Button_05");
        g_settings_button_coop = button("Pa_Coop_00", "Pa_Button_05");
        g_settings_button_set = button("Pa_Settings_00", "Pa_Button_05");
        // (an older pack's Multiplayer and Customize Link buttons, Pa_Button_06 and 07: hidden, as the pack stores them)
        g_mp_button_set = button("Pa_Settings_00", "Pa_Button_06");
        g_cl_button_set = button("Pa_Settings_00", "Pa_Button_07");
        for (int r = 0; r < kRows; ++r) {
            char name[16];
            std::snprintf(name, sizeof name, "Pa_Button_%02d", r);
            const std::size_t k = static_cast<std::size_t>(r);
            g_set_buttons[k] = button("Pa_Settings_00", name);
            g_coop_buttons[k] = button("Pa_Coop_00", name);
            g_sys_buttons[k] = button("Pa_System_00", name);
            g_sys_rest_y[k] = g_sys_buttons[k] != 0 ? Read<float>(g_sys_buttons[k] + kPaneOwnY) : 0.0f;
        }
        g_settings_button_shown = -1;
        LOG_INFO(Core, "coop: pause tab: System's button of ours: Longshot Settings (Pa_Button_05) {:#x}{}",
                 g_settings_button, g_settings_button != 0 ? "" : " (not in the pack: build it again)");
    }
    g_pages_layout = f.layout;
    LOG_INFO(Core, "coop: pause tab: the pages' panes: Pa_System_00 {:#x}, Pa_Settings_00 {:#x}{}", g_system_pane,
             g_settings_pane, g_pages_ok ? "" : " (no System page: Longshot Settings stays off)");
    if (g_pages_ok) {
        // The rows say what they do only through the live texts (the pack's Coop_Live_NN): a pack without them would
        // show its fixed words over rows that do other things, so the page stays off then, as without its panes.
        std::lock_guard lock(g_live_mutex);
        if (!LiveLookup(ctx, 0, gen) && g_live[0].bad) {
            g_pages_ok = false;
            LOG_INFO(Core, "coop: pause tab: the pack has no live texts (Coop_Live_00): Longshot Settings stays off "
                           "(build the puppet pack again)");
        }
    }
}

// The pages: our page over System's, or System's alone. The rows' texts once per opening.
// The pages' slide (Page::Do::slide, SlideOffsets): our page and System's both shown and moved from their own places
// each update of the pages, then the one that went hidden and both back at their places. g_mutex.
struct Slide {
    int dir = 0;                  // 0: none
    std::uint64_t start_ms = 0, gen = 0;
    // The two pages (the one to the right: the later tab; the one to the left), their own places (+0x6c) as they were
    // when it began, and which one stays shown at its end.
    std::uint64_t right = 0, left = 0, stays = 0;
    float right_x = 0, left_x = 0;
};
Slide g_slide;

// A page of Page::Do's numbering: 0 System's, 1 the Multiplayer page, 2 the Settings page (0 when the pack has none).
std::uint64_t PageOf(int page) {
    return page == 0 ? g_system_pane : page == 1 ? g_coop_pane : g_settings_pane;
}

void PlacePane(void* ctx, std::uint64_t pane, float x) {
    if (Read<float>(pane + 0x6c) == x) return;
    CoopRuntime::Write(pane + 0x6c, &x, sizeof x);
    Call(ctx, kPaneMoved, pane);   // (its matrices made again, as the game's own place setter ends)
}

// --- Settings and look: the pause menu's own Link on the page ---
// The pause menu draws a 3D model of its own (its "pouch actor": Link on Equipment, an item on Collection and Items,
// none on System) through the menu's 3D camera. The menu manager's object (+0x4e8) keeps the models in slots (+0x28,
// 0x20 each: a link to the actor, +0x10 its holder, +0x18 its id) and its display type at +0x228, which each page sets
// with f_80e01af0(object, type) (the page graph's SeqOneShotSetPouchActorDisplayType): 0 Equipment (slot 0, Link shown),
// 1 Collection and Items, 4 System. Read from main and tried in a run (2026-11-20): asked for 0 on System, Link stands
// on our page, his colours as painted (CoopLook's preview). While Customize Link shows, the module asks for 0, stands
// him on the right (the actor's place +0x31c, and +0x358 the place asked of an actor with no body, as the puppets'),
// turns him with the right stick (his back for a shield's part), and puts him back as he was when it goes; on System
// it asks for System's 4 again (another page has set its own by then).
constexpr std::uint64_t kPouchObject = 0x4e8, kPouchType = 0x228, kSetPouchType = 0xde1af0;
constexpr int kPouchEquipment = 0, kPouchSystem = 4;
constexpr std::uint64_t kPouchSlots = 0x28, kPouchSlotHolder = 0x10, kPouchSlotId = 0x18;
constexpr std::uint64_t kHolderActor = 0x40, kHolderId = 0x48;
constexpr std::uint64_t kActorPos = 0x31c, kActorRot = 0x328, kActorPosAsk = 0x358, kActorRotAsk = 0x364;
// His place on the right of the list (the menu's 3D units: x right, z toward the camera), set with a tester in a run
// (2026-11-20): the Equipment page has him at about (0.08, -0.2, 0.02); x 2.30 stands him in the middle of the space
// right of the moved list, z -1.3 a step back (about 5% smaller). Not lower and not further back: the menu's scene
// has a floor at his feet that hides what goes under it (lowered by 0.13 his soles went; at z -1.7 his boots, at
// -2.2 his legs to the knee).
constexpr float kModelX = 2.30f, kModelZ = -1.3f, kModelDropY = 0.0f;
// Child Link: the game stands him higher on Equipment (y about 0.30, adult about -0.2) and its camera is closer, so the
// adult's place is outside the picture (a tester's child save, 2026-11-20: no Link at all). His own, set with a tester.
constexpr float kChildModelX = 1.25f, kChildModelZ = -0.35f;
constexpr float kChildFromY = 0.05f;   // (his place's y on Equipment above this: child Link)
std::atomic<bool> g_model_test{false};   // model.now set the place (a test run's): it holds for either Link
// (a test run's own: model.now in the game's folder, "x drop z", read with LICHII_COOP_TEST_STATE=1: TestPouch)
std::atomic<float> g_model_x{kModelX}, g_model_drop{kModelDropY}, g_model_z{kModelZ};
constexpr float kModelTurnPerMs = 0.0035f;   // (the right stick at its end: a turn in about 1.8 s)
constexpr float kModelDragTurn = 0.0045f;    // (the mouse dragged across him: a turn per layout unit, ~1400 a turn)
struct MenuModelState {
    bool on = false;
    bool saved = false;          // his place and turn as they were, kept to put back
    std::uint64_t actor = 0;
    float pos[3] = {}, rot[9] = {};
    float yaw0 = 0, yaw = 0;     // his own turn when shown, and ours on top of it
    bool turned = false;         // the player turned him (the stick, the mouse): he stays so until another part's side
    bool back_was = false;
    std::uint64_t last_ms = 0;
    int type_was = -1;
    std::uint64_t shown_ms = 0;  // when shown: what the menu holds is logged once a second later (Said)
    bool said = false;
    int slot = 0;                // the slot whose actor is moved (0, Link's on Equipment; another when 0 is empty)
};
MenuModelState g_model;                 // (the pages' update: g_mutex)
std::atomic<float> g_model_spin{0.0f};  // the right stick's x (-1 .. 1), OnPad's
std::atomic<bool> g_model_back{false};  // a shield's part picked: his back to the camera
std::atomic<float> g_model_drag{0.0f};  // the mouse's turn since the last update (radians), OnPad's

std::uint64_t PouchActor(std::uint64_t obj, int slot) {
    const std::uint64_t at = obj + kPouchSlots + static_cast<std::uint64_t>(slot) * 0x20;
    const std::uint64_t holder = Read<std::uint64_t>(at + kPouchSlotHolder);
    if (holder == 0 || Read<std::uint32_t>(holder + kHolderId) != Read<std::uint32_t>(at + kPouchSlotId)) return 0;
    return Read<std::uint64_t>(holder + kHolderActor);
}

void PutModel(std::uint64_t actor, const float pos[3], const float rot[9]) {
    CoopRuntime::Write(actor + kActorPos, pos, 3 * sizeof(float));
    CoopRuntime::Write(actor + kActorPosAsk, pos, 3 * sizeof(float));
    CoopRuntime::Write(actor + kActorRot, rot, 9 * sizeof(float));
    CoopRuntime::Write(actor + kActorRotAsk, rot, 9 * sizeof(float));
}

void MenuModel(void* ctx, const Facts& f, bool want) {
    // (row 207: with Character Select, never the menu's Link model forced here: its code patches that model's making
    // for its own pages, and Customize is in its screen)
    want = want && !CoopCharColours::ModPresent();
    const std::uint64_t base = CoopRuntime::MainBase();
    const std::uint64_t manager = Read<std::uint64_t>(base + kMenuManager);
    const std::uint64_t obj = manager != 0 ? Read<std::uint64_t>(manager + kPouchObject) : 0;
    if (obj == 0) {
        g_model = MenuModelState{};
        return;
    }
    const std::uint64_t now = NowMs();
    if (want && !g_model.on) {
        g_model.type_was = Read<std::int32_t>(obj + kPouchType);
        Call(ctx, kSetPouchType, obj, kPouchEquipment);
        g_model.on = true, g_model.saved = false, g_model.yaw = 0, g_model.last_ms = now, g_model.shown_ms = now;
        LOG_INFO(Core, "coop: pause tab: settings and look: the menu's Link shown on the page (its display type was {})",
                 g_model.type_was);
    }
    if (!g_model.on) return;
    // (a tester's 4K game, 2026-11-20: Link not seen on the page though asked for; what the menu holds, said once)
    if (!g_model.said && now - g_model.shown_ms > 1000) {
        g_model.said = true;
        std::string held;
        for (int slot = 0; slot < 4; ++slot) {
            const std::uint64_t a = PouchActor(obj, slot);
            float at[3] = {};
            if (a != 0) CoopRuntime::Read(a + kActorPos, at, sizeof at);
            held += fmt::format("{}slot {}: {}", slot ? ", " : "", slot,
                                a == 0 ? std::string("none") : fmt::format("an actor at ({:.2f}, {:.2f}, {:.2f})", at[0], at[1], at[2]));
        }
        LOG_INFO(Core, "coop: pause tab: settings and look: the menu's model ({} Link): display type {}, {}; moved: slot {}",
                 !g_model.saved ? "no" : g_model.pos[1] > kChildFromY ? "child" : "adult",
                 Read<std::int32_t>(obj + kPouchType), held, g_model.slot);
    }
    std::uint64_t actor = PouchActor(obj, g_model.slot);
    if (actor == 0 && !g_model.saved) {   // (slot 0 empty: the first slot that has an actor)
        for (int slot = 1; slot < 4 && actor == 0; ++slot) {
            if ((actor = PouchActor(obj, slot)) != 0) g_model.slot = slot;
        }
    }
    if (!want) {
        if (g_model.saved && actor != 0 && actor == g_model.actor) PutModel(actor, g_model.pos, g_model.rot);
        if (f.page == kSystemPage && Read<std::int32_t>(obj + kPouchType) == kPouchEquipment) {
            Call(ctx, kSetPouchType, obj, kPouchSystem);   // (still System's page: none, as it has)
        }
        LOG_INFO(Core, "coop: pause tab: settings and look: the menu's Link put back ({})",
                 f.page == kSystemPage ? "System again" : "another page");
        g_model = MenuModelState{};
        return;
    }
    if (actor == 0) return;
    if (!g_model.saved || actor != g_model.actor) {
        CoopRuntime::Read(actor + kActorPos, g_model.pos, sizeof g_model.pos);
        CoopRuntime::Read(actor + kActorRot, g_model.rot, sizeof g_model.rot);
        g_model.actor = actor, g_model.saved = true;
        g_model.yaw0 = std::atan2(g_model.rot[2], g_model.rot[0]);   // (rows of 3: a turn about y is [c 0 s; 0 1 0; -s 0 c])
    }
    const float dt = static_cast<float>(std::min<std::uint64_t>(now - g_model.last_ms, 100));
    g_model.last_ms = now;
    const float spin = g_model_spin.load(), drag = g_model_drag.exchange(0.0f);
    const bool back = g_model_back.load();
    if (back != g_model.back_was) g_model.back_was = back, g_model.turned = false;   // (another part's side: turned to it)
    if (std::abs(spin) > 0.25f || drag != 0.0f) {
        g_model.yaw += (std::abs(spin) > 0.25f ? spin * kModelTurnPerMs * dt : 0.0f) + drag;
        g_model.turned = true;   // (the player's turn stays: no turning back by itself)
    } else if (!g_model.turned) {
        // (his face, or his back for a shield's part, turned to by itself)
        const float pi = 3.14159265f;
        const float target = back ? pi : 0.0f;
        const float d = std::remainder(target - g_model.yaw, 2.0f * pi);
        g_model.yaw += d * std::min(1.0f, 0.008f * dt);
    }
    const float a = g_model.yaw0 + g_model.yaw, c = std::cos(a), s = std::sin(a);
    const float rot[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    const bool child = !g_model_test.load() && g_model.pos[1] > kChildFromY;
    const float pos[3] = {child ? kChildModelX : g_model_x.load(), g_model.pos[1] + g_model_drop.load(),
                          child ? kChildModelZ : g_model_z.load()};
    PutModel(actor, pos, rot);
}

void EndSlide(void* ctx) {
    if (g_slide.dir == 0) return;
    if (g_slide.gen == g_pages_gen) {   // (a new opening has its own panes, at their places)
        PlacePane(ctx, g_slide.right, g_slide.right_x);
        PlacePane(ctx, g_slide.left, g_slide.left_x);
    }
    g_slide.dir = 0;
}

// Each update of the pages while a slide goes on; at its end the page that went is hidden.
void StepSlide(void* ctx, std::uint64_t now_ms) {
    if (g_slide.dir == 0) return;
    if (g_slide.gen != g_pages_gen || !g_pages_ok) {
        g_slide.dir = 0;
        return;
    }
    const float p = SlideProgress(now_ms - g_slide.start_ms);
    const auto [right, left] = SlideOffsets(g_slide.dir, p);
    PlacePane(ctx, g_slide.right, g_slide.right_x + right);
    PlacePane(ctx, g_slide.left, g_slide.left_x + left);
    if (p >= 1.0f) {
        const Slide was = g_slide;
        EndSlide(ctx);
        SetVisible(ctx, was.right, was.stays == was.right);
        SetVisible(ctx, was.left, was.stays == was.left);
    }
}

// Our page shown (no slide going on): it alone of the three, at each update of the pages. Seen in a run (2026-11-19):
// with the menu opened again on the Settings page, two Pa_Button_00 handed the focus to each other at every update
// and the pad's up and down did nothing, as if System's page were shown too; a slide from Multiplayer was right. A
// page found shown that should not be is hidden, and logged the first times.
int g_page_fixes = 0;
void KeepPageShown(void* ctx, int which) {
    const std::uint64_t want = which == 1 ? g_settings_pane : g_coop_pane;
    if (want == 0) return;
    for (const std::uint64_t pane : {g_system_pane, g_coop_pane, g_settings_pane}) {
        if (pane == 0) continue;
        const bool shown = (Read<std::uint32_t>(pane + 0xa8) & 1u) != 0;
        if (shown == (pane == want)) continue;
        SetVisible(ctx, pane, pane == want);
        if (++g_page_fixes <= 8) {
            LOG_INFO(Core, "coop: pause tab: the {} page was {}: {} again ({} so far)",
                     pane == g_system_pane ? "System" : pane == g_coop_pane ? "Multiplayer" : "Settings",
                     shown ? "shown" : "hidden", shown ? "hidden" : "shown", g_page_fixes);
        }
    }
}

// The pages screen's one shown button in a focus state, by its name ("" none, or more than one): System's Longshot
// Settings button (Pa_Button_05) is not a row RowOfButton knows.
std::string FocusedShownName() {
    const std::uint64_t pages = g_pages_screen;
    if (pages == 0) return {};
    const auto read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
    std::string name;
    int found = 0;
    for (const std::uint64_t b : CoopModScreens::Buttons(read, pages)) {
        const std::uint8_t state = CoopModScreens::FocusState(read, b);
        if (state == 0 || (state & 0xfe) == 4 || !CoopModScreens::HitBox(read, b)) continue;
        name = CoopModScreens::ButtonName(read, b);
        ++found;
    }
    return found == 1 ? name : std::string();
}

// Which of System's buttons of ours the game's cursor is on or going to (0 Multiplayer, 1 Longshot Settings, -1
// neither): any shown one in a focus state but leaving. FocusedShownName wants one alone, and while the focus moves
// (Longshot's mouse moved it just before its click's A) two are: that A went to the game, which does not know the
// button and then took no B, + or A (the menu stuck open, a test run 2026-11-20).
int OurSystemButtonFocused() {
    const std::uint64_t pages = g_pages_screen;
    if (pages == 0) return -1;
    const auto read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
    bool mp = false, ls = false, cl = false;
    std::optional<CoopModScreens::Box> mp_box, ls_box, cl_box;
    for (const std::uint64_t b : CoopModScreens::Buttons(read, pages)) {
        const std::uint8_t state = CoopModScreens::FocusState(read, b);
        const auto box = CoopModScreens::HitBox(read, b);
        if (state == 0 || (state & 0xfe) == 4 || !box) continue;
        const std::string name = CoopModScreens::ButtonName(read, b);
        if (name == "Pa_Button_06" && g_mp_button != 0) mp = true, mp_box = box;   // (0.11.9: never; an old pack's)
        if (name == "Pa_Button_05" && g_page.SettingsUsable()) ls = true, ls_box = box;
        if (name == "Pa_Button_07" && g_cl_button != 0) cl = true, cl_box = box;
    }
    if (static_cast<int>(mp) + static_cast<int>(ls) + static_cast<int>(cl) > 1) {
        // (two while the focus moves from one to another: the one under the mouse, else the focused one's name)
        float x = 0, y = 0;
        bool held = false;
        if (LayoutCursor(&x, &y, &held)) {
            const auto in = [&](const std::optional<CoopModScreens::Box>& b) {
                return b && x >= std::min(b->x0, b->x1) && x <= std::max(b->x0, b->x1) && y >= std::min(b->y0, b->y1) &&
                       y <= std::max(b->y0, b->y1);
            };
            if (ls && in(ls_box)) return 1;
            if (cl && in(cl_box)) return 2;
            if (mp && in(mp_box)) return 0;
        }
        const std::string name = FocusedShownName();
        return name == "Pa_Button_05" ? 1 : name == "Pa_Button_07" ? 2 : mp ? 0 : cl ? 2 : 1;
    }
    return mp ? 0 : ls ? 1 : cl ? 2 : -1;
}

// System's two buttons of ours, at each update of the pages: Multiplayer shown while our pages can be used, Longshot
// Settings while it can (with its page's sixth row, its Debug category), System's lower buttons placed around them
// (Options, Special Actions, Multiplayer, Longshot Settings, Title Screen). The game's cursor is never moved by a call:
// moved onto System's button with f_8088ffc0 as a page was left, neither B nor + closed the menu any more (a run,
// 2026-11-19). The game carries its cursor by the buttons' names, else to the nearest shown: so a page opening hides
// its rows but the first (ApplyPages), and a page left by B hides System's other buttons, until the cursor is on the
// one it should be (kRowsBackMs at most). Where it went is logged. g_mutex.
constexpr std::uint64_t kRowsBackMs = 400;
constexpr float kSystemTightStep = 160.0f;   // (the lower group with ours: five rows from Options down)
std::uint64_t g_rows_back_ms = 0;            // the hidden ones shown again then (0: none hidden)
int g_rows_back_logged = 0;                  // (how soon they came back, logged the first times)
std::string g_rows_back_when;                // ... or as soon as the game's cursor is on this button
std::vector<std::pair<std::uint64_t, bool>> g_rows_hidden;   // (a pane, and whether it is shown again)
int g_open_trace = 0, g_left_trace = 0;  // (trace) the game's cursor's button logged this many more times
std::uint64_t g_open_since = 0, g_left_since = 0;
std::string g_open_said, g_left_said;
void TraceCursor(const char* when, int* left, std::uint64_t since, std::string* said, std::uint64_t now) {
    if (*left <= 0 || now - since < 100) return;
    const std::string name = FocusedShownName();
    if (name != *said) {
        *said = name;
        LOG_INFO(Core, "coop: pause tab: {}, the game's cursor on {}", when, name.empty() ? std::string("no one shown button") : name);
        --*left;
    }
    if (now - since > 5000) *left = 0, said->clear();
}
// `keep`: the one button left shown for the game's cursor to land on, see-through meanwhile, so that the page's
// buttons come all at once: the game puts its cursor there only ~250 ms after (a run's log), and that button alone
// showed first, then the others (a tester: "it loads menu items top to bottom").
constexpr float kKeepAlpha = 0.02f;   // (the landing button's: not 0, which the game's cursor skips)
void HideRowsAMoment(void* ctx, std::vector<std::pair<std::uint64_t, bool>> panes, const char* until,
                     std::uint64_t keep) {
    for (const auto& [pane, back] : panes) {
        if (pane != 0) SetVisible(ctx, pane, false);
    }
    if (keep != 0) CallAlpha(ctx, keep, kKeepAlpha);
    g_rows_keep = keep;
    g_rows_hidden = std::move(panes), g_rows_back_when = until, g_rows_back_ms = NowMs() + kRowsBackMs;
    g_rows_moment = true;
}
void PlacePaneY(void* ctx, std::uint64_t pane, float y) {
    if (pane == 0 || Read<float>(pane + kPaneOwnY) == y) return;
    CoopRuntime::Write(pane + kPaneOwnY, &y, sizeof y);
    Call(ctx, kPaneMoved, pane);
}
// System's lower buttons in order, ours among them (shown: 1 Multiplayer, 2 Longshot Settings); or all at their own
// places with none of ours.
void PlaceSystemButtons(void* ctx, int shown) {
    if (g_sys_buttons[2] == 0 || g_sys_buttons[3] == 0 || g_sys_buttons[4] == 0) return;
    std::vector<std::uint64_t> order{g_sys_buttons[2], g_sys_buttons[3]};
    if ((shown & 4) != 0) order.push_back(g_cl_button);
    if ((shown & 1) != 0) order.push_back(g_mp_button);
    if ((shown & 2) != 0) order.push_back(g_settings_button);
    order.push_back(g_sys_buttons[4]);
    if (shown == 0) {
        for (int r = 2; r < kRows; ++r) PlacePaneY(ctx, g_sys_buttons[static_cast<std::size_t>(r)], g_sys_rest_y[static_cast<std::size_t>(r)]);
        return;
    }
    const float top = g_sys_rest_y[2], step = order.size() > 4 ? kSystemTightStep : g_sys_rest_y[2] - g_sys_rest_y[3];
    for (std::size_t i = 0; i < order.size(); ++i) PlacePaneY(ctx, order[i], top - step * static_cast<float>(i));
}
// System's own buttons (and its two of ours) hidden while one of our pages is in System's place (g_sys_hidden): hidden
// with their page only, the game's cursor and Longshot's mouse still went to them (a run, 2026-11-19: an A on the
// Multiplayer page's Session view chose System's Longshot Settings button, under Back, once System's lower buttons
// were placed lower).
bool SystemOwn(std::uint64_t pane) {
    return pane != 0 && (pane == g_mp_button || pane == g_settings_button || pane == g_cl_button ||
                         std::find(g_sys_buttons.begin(), g_sys_buttons.end(), pane) != g_sys_buttons.end());
}
void SystemButtons(void* ctx) {
    const std::uint64_t now = NowMs();
    const int want = (g_page.Usable() && g_mp_button != 0 ? 1 : 0) | (g_page.SettingsUsable() && g_settings_button != 0 ? 2 : 0) |
                     (g_page.Usable() && g_cl_button != 0 && !CoopCharColours::ModPresent() ? 4 : 0);   // (row 207: with
                                                                // Character Select, Customize is in its own screen)
    const bool over = g_page.Shown();
    if (want != g_settings_button_shown || over != g_sys_hidden) {
        if (over != g_sys_hidden) {
            for (const std::uint64_t b : g_sys_buttons) {
                if (b != 0) SetVisible(ctx, b, !over);
            }
            g_sys_hidden = over;
        }
        if (g_mp_button != 0) SetVisible(ctx, g_mp_button, !over && (want & 1) != 0);
        if (g_settings_button != 0) SetVisible(ctx, g_settings_button, !over && (want & 2) != 0);
        if (g_cl_button != 0) SetVisible(ctx, g_cl_button, !over && (want & 4) != 0);
        if (g_rows_back_ms == 0 && g_settings_button_set != 0) SetVisible(ctx, g_settings_button_set, (want & 2) != 0);
        if (want != g_settings_button_shown) {
            PlaceSystemButtons(ctx, want);
            g_settings_button_shown = want;
            LOG_INFO(Core, "coop: pause tab: System's buttons of ours: Customize Link {}, Multiplayer {}, Longshot Settings {}",
                     (want & 4) != 0 ? "shown" : g_cl_button == 0 ? "not in the pack" : "hidden",
                     (want & 1) != 0 ? "shown" : "hidden", (want & 2) != 0 ? "shown" : "hidden");
        }
    }
    // (the rows hidden for an opening or a leaving back once the game's cursor is where it should be; not System's
    // own while our page is back over it, nor our page's rows with nothing on them, ApplyBlankRows'. The focused
    // button's own name: the one shown button in a focus state (FocusedShownName) came only with the 400 ms, as
    // others were still in a focus state, so a tester saw System's Multiplayer button alone half a second after B, and
    // the page's rows come in one after another as it opened, 2026-11-19)
    bool landed = false;
    if (g_rows_back_ms != 0 && g_pages_screen != 0) {
        const auto read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
        landed = CoopModScreens::ButtonName(read, CoopModScreens::FocusedButton(read, g_pages_screen)) == g_rows_back_when;
    }
    if (g_rows_back_ms != 0 && (now >= g_rows_back_ms || landed)) {
        if (++g_rows_back_logged <= 8) {
            LOG_INFO(Core, "coop: pause tab: the rows hidden a moment shown again after {} ms ({})",
                     now + kRowsBackMs - g_rows_back_ms, landed ? "the game's cursor on " + g_rows_back_when : "time");
        }
        g_rows_back_ms = 0;
        const std::uint32_t blank = g_rows_blank.load();
        for (const auto& [pane, back] : g_rows_hidden) {
            if (pane == 0 || !back || (g_sys_hidden && SystemOwn(pane))) continue;
            bool empty = false;
            for (int r = 0; r < kRows; ++r) {
                if (g_coop_buttons[static_cast<std::size_t>(r)] == pane) empty = (blank & (1u << r)) != 0;
            }
            if (!empty) SetVisible(ctx, pane, true);
        }
        g_rows_hidden.clear();
        if (g_rows_keep != 0) CallAlpha(ctx, g_rows_keep, 1.0f), g_rows_keep = 0, g_rows_alpha_stale = true;
        g_rows_moment = false;
    }
    if (g_page.TakeOpened()) {
        if (g_page.Which() == 1) {
            g_set_page = 0, g_set_row = -1, g_set_seen_row = -1, g_live_poke = true;   // (the category: the cursor's row)
            TestSettingsOnce();   // (COOP_TEST_LONGSHOT_SETTINGS=1 only)
        } else {
            g_live_poke = true;
        }
        g_open_trace = 4, g_open_since = now, g_open_said.clear();
        LOG_INFO(Core, "coop: pause tab: {} opened from System", g_page.Which() == 1 ? "Longshot Settings" : "the Multiplayer page");
    }
    if (g_page.TakeLeft()) {
        // (B: System's other buttons hidden a moment, so the game's cursor comes back to the page's button)
        const bool settings = g_page.Which() == 1;
        const bool customize = !settings && g_page.OpenedFrom() == 2;   // (back onto the Customize Link button)
        std::vector<std::pair<std::uint64_t, bool>> others;
        for (const std::uint64_t b : g_sys_buttons) others.emplace_back(b, true);
        if (!settings) others.emplace_back(g_settings_button, (want & 2) != 0);
        if (settings || customize) others.emplace_back(g_mp_button, (want & 1) != 0);
        if (!customize && g_cl_button != 0) others.emplace_back(g_cl_button, (want & 4) != 0);
        // (the button to land on all but see-through, so the page's buttons come all at once: at alpha 0 the game's
        // cursor never landed on it and every page change waited the whole 400 ms, a run's log 2026-11-19; 0.02 is
        // tried, and the log line after says whether it landed)
        HideRowsAMoment(ctx, std::move(others), settings ? "Pa_Button_05" : customize ? "Pa_Button_07" : "Pa_Button_06",
                        settings ? g_settings_button : customize ? g_cl_button : g_mp_button);
        g_left_trace = 4, g_left_since = now, g_left_said.clear();
        LOG_INFO(Core, "coop: pause tab: {} left (B): System again", settings ? "Longshot Settings" : "the Multiplayer page");
    }
    TraceCursor("as the page opened", &g_open_trace, g_open_since, &g_open_said, now);
    TraceCursor("after the page", &g_left_trace, g_left_since, &g_left_said, now);
}

// Where a page sits in the slides: System and Longshot Settings (in System's place) at 0, Multiplayer to the right.
int SlidePlace(int page) {
    return page == 1 ? 1 : 0;
}

void ApplyPages(void* ctx, const Facts& f, const Page::Do& d) {
    const std::uint64_t layout = f.layout;
    if (layout == 0 || !g_pages_ok) return;
    if (d.set_page) {
        EndSlide(ctx);   // (one going on: both back at their places first)
        const std::uint64_t from = PageOf(d.page_from), to = PageOf(d.page_to);
        if (d.slide != 0 && from != 0 && to != 0 && from != to) {
            g_slide.dir = d.slide, g_slide.start_ms = NowMs(), g_slide.gen = g_pages_gen;
            const bool to_right = SlidePlace(d.page_to) > SlidePlace(d.page_from);
            g_slide.right = to_right ? to : from, g_slide.left = to_right ? from : to;
            g_slide.stays = to;
            g_slide.right_x = Read<float>(g_slide.right + 0x6c), g_slide.left_x = Read<float>(g_slide.left + 0x6c);
            for (const std::uint64_t pane : {g_system_pane, g_coop_pane, g_settings_pane}) {
                if (pane != 0) SetVisible(ctx, pane, pane == from || pane == to);
            }
            StepSlide(ctx, g_slide.start_ms);
            LOG_INFO(Core, "coop: pause tab: the pages slide from page {} to page {} (places x {:.0f} and {:.0f})",
                     d.page_from, d.page_to, g_slide.left_x, g_slide.right_x);
        } else {
            for (const std::uint64_t pane : {g_system_pane, g_coop_pane, g_settings_pane}) {
                if (pane != 0) SetVisible(ctx, pane, pane == (to != 0 ? to : g_system_pane));
            }
            // (a page of ours opening from its System button: its rows but the first hidden a moment, so the game's
            // cursor lands on the first: carried by the button's name it went to Longshot Settings' sixth row, Debug,
            // and with that one hidden to the nearest, Motion (runs, 2026-11-19); SystemButtons shows them again)
            if (d.page_from == 0 && (d.page_to == 1 || d.page_to == 2)) {
                const auto& rows = d.page_to == 2 ? g_set_buttons : g_coop_buttons;
                std::vector<std::pair<std::uint64_t, bool>> hide;
                for (int r = 1; r < kRows; ++r) hide.emplace_back(rows[static_cast<std::size_t>(r)], true);
                if (d.page_to == 2) hide.emplace_back(g_settings_button_set, g_page.SettingsUsable());
                HideRowsAMoment(ctx, std::move(hide), "Pa_Button_00", rows[0]);   // (as above)
            }
        }
    }
    if (d.set_texts) {
        // (the rows' words are the live texts', LiveRows: the pane-text call never reaches their "=T_Btn_00" panes)
        // Inside our copy: its controller picture (System's, not ours) hidden.
        std::uint64_t table = 0;
        Call(ctx, kPaneTable, layout + 0x40, 0, 0, 0, 0, &table);
        for (const char* page : {"Pa_Coop_00", "Pa_Settings_00"}) {
            const std::uint64_t ours = PartLayout(table, page);
            if (ours == 0) continue;
            const std::uint64_t controller = PaneIn(ctx, ours, "Pa_SystemController_00");
            if (IsPart(controller)) SetVisible(ctx, controller, false);
            LOG_INFO(Core, "coop: pause tab: {}'s controller picture {}", page, IsPart(controller) ? "hidden" : "not found");
        }
        // (and the Longshot Settings button our copies of System's page have too: the Multiplayer page's stays hidden;
        // Longshot Settings' Longshot Settings button is its sixth category row, SystemButtons')
        for (const std::uint64_t b : {g_settings_button_coop, g_mp_button_coop, g_mp_button_set, g_cl_button_coop,
                                      g_cl_button_set}) {
            if (b != 0) SetVisible(ctx, b, false);
        }
    }
}

// A screen's own menu sound by name (kScreenSound), on its update.
void PlaySound(void* ctx, std::uint64_t screen, const char* name) {
    const std::uint64_t at = g_api->scratch(ctx, name, std::strlen(name) + 1);
    if (at != 0) Call(ctx, kScreenSound, screen, at);
}

// --- The - menu's tabs of ours (0.11.9): Multiplayer, Lobby, Mods, right of Map ---
// The pack's Pa_Category_04..06 (pause_menu.py add_tab, stored hidden; the strip counts its Pa_Category_%02d parts up
// to the first missing, so ours follow the game's four) found once per opening of the strip, with the game's tabs to
// place ours one game step apart after the - menu's last shown tab: slot 3 with Character Select's header (his code
// shows four - tabs: Characters, Key Items, Threads of Time, Map), else slot 2 (the game hides slot 3 there). g_mutex.
std::uint64_t g_strip_gen = 0;
bool g_strip_looked = false;
std::array<std::uint64_t, 4> g_game_tabs{};
std::array<float, 4> g_game_tabs_x{};
std::array<std::uint64_t, kMinusTabs> g_minus_panes{}, g_minus_words{};
// Ours sit under the strip's root (the pack's), the game's under N_MenuChange_00: the two frames apart by how far ours
// were packed from where the game's step puts them (their own places read at the lookup against the fourth tab's).
float g_minus_frame_x = 0.0f, g_minus_frame_y = 0.0f;
bool g_minus_frame = false;
bool g_minus_ok = false, g_minus_shown = false;
// The strip's L and R labels (T_BtnL_00, T_BtnR_00) and their own places; whether the row is laid out as ours now.
std::array<std::uint64_t, 2> g_strip_btns{};
std::array<float, 2> g_strip_btns_x{};
bool g_strip_moved = false;

void LookMinusPage(void* ctx, std::uint64_t layout, std::uint64_t gen);   // (below: the - menu's own page)

void LookStrip(void* ctx, bool calls, const Facts& f, std::uint64_t gen) {
    if (gen != g_strip_gen) {
        g_strip_gen = gen, g_strip_looked = false;
        g_game_tabs = {}, g_minus_panes = {}, g_minus_words = {}, g_minus_ok = false, g_minus_shown = false;
        g_strip_btns = {}, g_strip_moved = false;
    }
    if (g_strip_looked || !calls || f.layout == 0) return;
    g_strip_looked = true;
    std::uint64_t table = 0;
    Call(ctx, kPaneTable, f.layout + 0x40, 0, 0, 0, 0, &table);
    for (int i = 0; i < 4; ++i) {
        char name[16];
        std::snprintf(name, sizeof name, "Pa_Category_%02d", i);
        const std::uint64_t tab = FindOwnPane(ctx, f.layout, name);
        if (IsPart(tab)) {
            g_game_tabs[static_cast<std::size_t>(i)] = tab;
            g_game_tabs_x[static_cast<std::size_t>(i)] = Read<float>(tab + 0x6c);
        }
    }
    g_minus_ok = g_game_tabs[2] != 0;
    for (int i = 0; i < 2; ++i) {
        const std::uint64_t b = FindOwnPane(ctx, f.layout, i == 0 ? "T_BtnL_00" : "T_BtnR_00");
        g_strip_btns[static_cast<std::size_t>(i)] = b;
        if (b != 0) g_strip_btns_x[static_cast<std::size_t>(i)] = Read<float>(b + 0x6c);
    }
    for (int i = 0; i < kMinusTabs; ++i) {
        char name[16];
        std::snprintf(name, sizeof name, "Pa_Category_%02d", 4 + i);
        g_minus_panes[static_cast<std::size_t>(i)] = FindOwnPane(ctx, f.layout, name);
        g_minus_words[static_cast<std::size_t>(i)] = PaneIn(ctx, PartLayout(table, name), "N_MenuChangeTextIn_00");
        g_minus_ok = g_minus_ok && IsPart(g_minus_panes[static_cast<std::size_t>(i)]);
    }
    // (pause_menu.add_tab: ours i at the fourth tab's place plus (i + 1) steps, in the root's frame)
    // (once per pane: a strip opened again on the same layout has ours where FollowMinus last put them)
    static std::uint64_t framed = 0;
    if (g_minus_panes[0] != framed) g_minus_frame = false;
    if (!g_minus_frame && g_minus_ok && g_game_tabs[3] != 0 && g_game_tabs[0] != 0 && g_game_tabs[1] != 0) {
        framed = g_minus_panes[0];
        const float step = g_game_tabs_x[1] - g_game_tabs_x[0];
        const std::uint64_t first = g_minus_panes[0];
        g_minus_frame_x = Read<float>(first + 0x6c) - (g_game_tabs_x[3] + step);
        g_minus_frame_y = Read<float>(first + kPaneOwnY) - Read<float>(g_game_tabs[3] + kPaneOwnY);
        g_minus_frame = std::isfinite(g_minus_frame_x) && std::isfinite(g_minus_frame_y);
    }
    LOG_INFO(Core, "coop: pause tab: the - menu's tabs of ours (Pa_Category_04..06): {} (frames {:.1f}, {:.1f}; the "
                   "strip's tab objects {})",
             g_minus_ok ? "found" : "not in the pack (the - menu stays the game's)", g_minus_frame_x, g_minus_frame_y,
             f.tabs);
    if (g_minus_ok) LookMinusPage(ctx, f.layout, gen);
}

// The strip's Do for ours: shown while the - menu's strip is open, filled once per opening as - tabs with our words,
// ours lit (the game's Map then not). WHAT THE LANE RUN MUST CONFIRM: the strip's tab objects 4..6 take kTabFill with a
// - category's look, and which screen draws these tabs' pages (R1); until then COOP_TEST_MINUS_TABS=1 only.
void ApplyMinus(void* ctx, const Facts& f, const Page::Do& d) {
    const std::uint64_t tabs = Read<std::uint64_t>(f.screen + kStripTabArray);
    if (f.layout == 0 || tabs == 0 || !g_minus_ok || !d.set_minus) return;
    g_minus_shown = d.minus_shown;
    for (int i = 0; i < kMinusTabs; ++i) SetVisible(ctx, g_minus_panes[static_cast<std::size_t>(i)], d.minus_shown);
    if (!d.minus_shown) return;
    if (static_cast<int>(Read<std::uint32_t>(f.screen + kStripTabs)) < 4 + kMinusTabs) return;   // (an older strip)
    for (int i = 0; i < kMinusTabs; ++i) {
        const std::uint64_t tab = Read<std::uint64_t>(tabs + 8 * static_cast<std::uint64_t>(4 + i));
        if (tab == 0) continue;
        if (d.fill_minus) {
            Call(ctx, kTabFill, tab, kMapCategory, 1);   // (a - tab's look, as the game fills its own)
            char path[48], label[48];
            std::snprintf(path, sizeof path, "Pa_Category_%02d/T_Text_00", 4 + i);
            std::snprintf(label, sizeof label, "Pa_Category_%02d-T_Text_00", 4 + i);
            SetText(ctx, f.layout, path, "LayoutMsg/MenuOverlay_00", label);
            Call(ctx, kTabOpen, tab, 0);
            if (const std::uint64_t w = g_minus_words[static_cast<std::size_t>(i)]; w != 0) CallAlpha(ctx, w, 1.0f);
        }
        Call(ctx, kTabLight, tab, d.minus_lit == i ? 1 : 0, 0);
    }
    // (the game's Map tab: unlit while one of ours is; its slot is the last of the game's shown)
    for (int slot = 2; slot <= 3; ++slot) {
        const std::uint64_t t = Read<std::uint64_t>(tabs + 8 * static_cast<std::uint64_t>(slot));
        if (t != 0 && d.minus_lit >= 0) Call(ctx, kTabLight, t, 0, 0);
    }
    if (d.fill_minus) LOG_INFO(Core, "coop: pause tab: the - menu's tabs of ours shown and filled");
    if (d.minus_slide != 0) {
        LOG_INFO(Core, "coop: pause tab: the - menu: {} ({})", d.minus_lit >= 0 ? kMinusTabNames[d.minus_lit] : "Map",
                 d.minus_slide > 0 ? "R" : "L");
    }
}

// The - strip as one row (R1, 2026-11-21: ours placed after Map ran past the strip's right end, Mods not seen, and the
// game's R label stayed on Multiplayer): the game's shown tabs and ours centred where the game's were, one game step
// apart, the L and R labels as far from the row's ends as the game has them. Everything back at the game's own places
// whenever ours are not shown (the + menu uses the same strip). Own X (+0x6c) only, as the game's own place setter.
// g_mutex.
void FollowMinus(void* ctx) {
    const auto put_back = [&] {
        if (!g_strip_moved) return;
        g_strip_moved = false;
        for (std::size_t i = 0; i < g_game_tabs.size(); ++i) {
            if (g_game_tabs[i] != 0) PlacePane(ctx, g_game_tabs[i], g_game_tabs_x[i]);
        }
        for (std::size_t i = 0; i < g_strip_btns.size(); ++i) {
            if (g_strip_btns[i] != 0) PlacePane(ctx, g_strip_btns[i], g_strip_btns_x[i]);
        }
    };
    if (!g_minus_shown || !g_minus_ok || !g_minus_frame || g_game_tabs[0] == 0 || g_game_tabs[1] == 0) {
        put_back();
        return;
    }
    // (the game's shown: slots 0..3 when Character Select's header shows slot 3, else 0..2)
    const int game = g_game_tabs[3] != 0 && (Read<std::uint32_t>(g_game_tabs[3] + 0xa8) & 1u) != 0 ? 4 : 3;
    const float step = g_game_tabs_x[1] - g_game_tabs_x[0];
    if (!std::isfinite(step) || step == 0.0f) return;
    const float first = g_game_tabs_x[0], last = g_game_tabs_x[static_cast<std::size_t>(game - 1)];
    const float centre = (first + last) / 2.0f;
    const int n = game + kMinusTabs;
    const auto x_of = [&](int k) { return centre + step * (static_cast<float>(k) - static_cast<float>(n - 1) / 2.0f); };
    for (int k = 0; k < game; ++k) PlacePane(ctx, g_game_tabs[static_cast<std::size_t>(k)], x_of(k));
    // (ours in the root's frame: the game's places plus the frames' distance. R1 lane 12:52 had ours placed with the
    // game's own numbers, so they sat a header's distance off, over the Map)
    const float y = Read<float>(g_game_tabs[0] + kPaneOwnY) + g_minus_frame_y;
    for (int i = 0; i < kMinusTabs; ++i) {
        const std::uint64_t pane = g_minus_panes[static_cast<std::size_t>(i)];
        PlacePane(ctx, pane, x_of(game + i) + g_minus_frame_x);
        if (std::isfinite(y) && Read<float>(pane + kPaneOwnY) != y) {
            CoopRuntime::Write(pane + kPaneOwnY, &y, sizeof y);
            Call(ctx, kPaneMoved, pane);
        }
    }
    if (g_strip_btns[0] != 0) PlacePane(ctx, g_strip_btns[0], x_of(0) - (first - g_strip_btns_x[0]));
    if (g_strip_btns[1] != 0) PlacePane(ctx, g_strip_btns[1], x_of(n - 1) + (g_strip_btns_x[1] - last));
    g_strip_moved = true;
}


// --- The - menu's own page (0.11.9; pause_menu.py's Pa_CoopMinus_00, in the strip's layout) ---
// R1 found no screen of the game's for a page past Map, so the pack puts ours in the strip's layout (MenuOverlay_00),
// which shows over every - page: a part of the System page's own layout, where a shown + page sits. While Multiplayer
// or Lobby is lit it shows their rows on System's own buttons (Pa_Button_00..04, the game's PaCommonBtnThin_00: its
// frame, words and cursor), the rest of that layout hidden (the controller picture, the list rows, our System
// buttons), and the Map page under it hidden (MapMenu_00's root, the screen UIMapMenuScreen, put back the moment ours
// goes, at that screen's own next update). The rows' words: the pack's placeholders Coop_Minus_00..04 in
// LayoutMsg/MenuOverlay_00 (the strip's own message file, loaded while the strip shows). Mods: the page shows, empty
// (its own page is still the overlay's Mods tab), so no row shows that does nothing.
constexpr std::uint64_t kMapMenuVtable = 0x4be8300;   // main+: UIMapMenuScreen (MapMenu_00: the HUD and the Map page)
std::uint64_t g_minus_page = 0, g_minus_part = 0;      // Pa_CoopMinus_00's pane and its part layout (LookStrip). g_mutex
std::uint64_t g_minus_layout = 0;                       // the strip's layout they were found in (its animations by path)
bool g_minus_anim_said = false;                         // (the focus animations' result said once per opening)
std::uint64_t g_minus_page_gen = 0;                     // the strip's opening they were looked up for
std::array<std::uint64_t, kRows> g_minus_buttons{}, g_minus_cursors{};
// Each button's W_Select_00: the focus frame the game shows (lane R1: a frame stayed on the last button copied and our
// N_Cursor_00 moved nothing; PaCommonBtnThin_00's Select / UnSelect animations show and hide W_Select_00)
std::array<std::uint64_t, kRows> g_minus_selects{};
std::vector<std::uint64_t> g_minus_hide;               // (that layout's panes ours never shows)
// The page's list on the right (its Pa_List_00..07, the game's Options rows, as the + page's was): a view's details,
// the game browser's games, the typing keys, co-op's menu's page (Co-op settings) or the Mods tab's. Their panes, each
// one's value box and arrows (as LiveCells has them), and the lines MinusWords set for ApplyMinusPage to show.
struct MinusList {
    std::uint64_t pane = 0, arrows = 0;
    std::array<std::uint64_t, 2> box{};
    std::array<bool, 2> box_own{};
    int vis = -1, box_vis = -1, arrows_vis = -1;
    float alpha = -1.0f;
};
std::array<MinusList, kLinePanes> g_minus_lists{};
std::array<ListLine, kLinePanes> g_minus_lines{};       // (g_mutex) the lines shown; name "" not shown
std::array<float, kLinePanes> g_minus_line_alpha{};      // (g_mutex) each line's alpha
float g_minus_rows_alpha = 1.0f;                         // (g_mutex) the rows: dimmed while the list has the pad
std::array<float, kRows> g_minus_rows_set{};
bool g_minus_mods_shown = false;                          // the Mods tab's page told it shows (HOS_COOP_MENU_SHOW)
int g_minus_mods_top = 0;
int g_minus_vis = -1;                                   // the page shown (1) or not (0) as last set; -1 not set
int g_minus_page_tab = -1, g_minus_focus = -1;          // the tab its rows are for, the row its cursor shows on
std::uint32_t g_minus_shown_rows = ~0u;                 // the buttons shown as last set
std::uint64_t g_minus_rows_ms = 0;                      // the rows' words last read
std::array<std::string, kRows> g_minus_texts{};
// The rows' words (Coop_Minus_00..04) and the list's, a name and a value a line (Coop_Minus_05..20). (g_live_mutex)
constexpr int kMinusSlots = kRows + 2 * kLinePanes;
std::array<LiveSlot, kMinusSlots> g_minus_live{};
std::atomic<bool> g_map_hide{false};                    // the Map page under ours to be hidden
std::uint64_t g_map_root = 0, g_map_layout = 0;
bool g_map_hidden = false;                              // (the map screen's updates: one thread)

// A part's animation (a path from `layout`, kAnimFind) stopped at its first frame or its last, the title's BtnNum way.
// nullptr when done, else why not.
const char* StopPartAnim(void* ctx, std::uint64_t layout, const std::string& path, bool last) {
    const std::uint64_t name = g_api->scratch(ctx, path.c_str(), path.size() + 1);
    if (name == 0) return "no scratch";
    std::uint64_t anim = 0;
    if (!Call(ctx, kAnimFind, layout, name, 0, 0, 0, &anim) || anim == 0) return "not found";
    float frame = 0.0f;
    if (last) {
        const std::uint64_t player = Read<std::uint64_t>(anim + kAnimPlayer);
        std::uint64_t end = 0;
        if (player == 0 || !Call(ctx, kAnimLastFrame, player, 0, 0, 0, 0, &end)) return "no last frame";
        frame = static_cast<float>(static_cast<std::int32_t>(end));
    }
    HosCoopCall k{};
    k.function = CoopRuntime::MainBase() + kAnimStopAt;
    k.x[0] = anim;
    k.s0 = frame;
    std::uint64_t r = 0;
    return g_api->call(ctx, &k, &r) ? nullptr : "not stopped";
}

void LookMinusPage(void* ctx, std::uint64_t layout, std::uint64_t gen) {
    g_minus_page_gen = gen;
    g_minus_layout = layout, g_minus_anim_said = false;
    g_minus_page = 0, g_minus_part = 0, g_minus_buttons = {}, g_minus_cursors = {}, g_minus_selects = {};
    g_minus_hide.clear();
    g_minus_vis = -1, g_minus_page_tab = -1, g_minus_focus = -1, g_minus_shown_rows = ~0u, g_minus_rows_ms = 0;
    g_minus_texts = {};
    std::uint64_t table = 0;
    Call(ctx, kPaneTable, layout + 0x40, 0, 0, 0, 0, &table);
    const std::uint64_t page = FindOwnPane(ctx, layout, "Pa_CoopMinus_00");
    const std::uint64_t part = PartLayout(table, "Pa_CoopMinus_00");
    if (!IsPart(page) || part == 0) {
        LOG_INFO(Core, "coop: pause tab: the - menu's page (Pa_CoopMinus_00): not in the pack (build the puppet pack again)");
        return;
    }
    int found = 0;
    for (int r = 0; r < kRows; ++r) {
        char name[16];
        std::snprintf(name, sizeof name, "Pa_Button_%02d", r);
        const std::uint64_t b = PaneIn(ctx, part, name);
        if (!IsPart(b)) continue;
        g_minus_buttons[static_cast<std::size_t>(r)] = b;
        g_minus_cursors[static_cast<std::size_t>(r)] = PaneIn(ctx, PartLayout(part, name), "N_Cursor_00");
        g_minus_selects[static_cast<std::size_t>(r)] = PaneIn(ctx, PartLayout(part, name), "W_Select_00");
        ++found;
    }
    for (const char* name : {"Pa_SystemController_00", "Pa_Button_05", "Pa_Button_06", "Pa_Button_07"}) {
        if (const std::uint64_t q = PaneIn(ctx, part, name); q != 0) g_minus_hide.push_back(q);
    }
    int lists = 0;
    for (int i = 0; i < kLinePanes; ++i) {
        char name[16];
        std::snprintf(name, sizeof name, "Pa_List_%02d", i);
        MinusList& l = g_minus_lists[static_cast<std::size_t>(i)];
        l = MinusList{};
        const std::uint64_t q = PaneIn(ctx, part, name);
        if (!IsPart(q)) continue;
        l.pane = q, ++lists;
        SetVisible(ctx, q, false);   // (shown line by line: ApplyMinusPage)
        const std::uint64_t row = PartLayout(part, name);
        if (const std::uint64_t mark = PaneIn(ctx, row, "P_CheckMark_00"); mark != 0) SetVisible(ctx, mark, false);
        l.arrows = PaneIn(ctx, row, "N_Content_00");
        l.box = {PaneIn(ctx, row, "W_Base_01"), PaneIn(ctx, row, "W_Base_03")};
        for (std::size_t j = 0; j < 2; ++j) l.box_own[j] = l.box[j] != 0 && (Read<std::uint32_t>(l.box[j] + 0xa8) & 1u) != 0;
    }
    g_minus_lines = {}, g_minus_line_alpha.fill(1.0f), g_minus_rows_set.fill(-1.0f);
    if (found == kRows) g_minus_page = page, g_minus_part = part;
    int frames = 0;
    for (const std::uint64_t q : g_minus_selects) frames += q != 0 ? 1 : 0;
    LOG_INFO(Core, "coop: pause tab: the - menu's page (Pa_CoopMinus_00): {} of {} buttons, {} focus frames (W_Select_00), "
                   "{} list rows{}",
             found, kRows, frames, lists, found == kRows ? "" : " (not used)");
}

// The strip's update, g_mutex: the page shown while one of ours is lit, its buttons those with words (MinusWords, set
// after the lock), the cursor on the Page's row. The tab whose words are due (-1 none).
int ApplyMinusPage(void* ctx, std::uint64_t gen) {
    if (gen != g_minus_page_gen) {   // (another opening, not looked up: never through the old one's panes)
        g_minus_vis = -1, g_map_hide = false;
        return -1;
    }
    const int tab = g_minus_shown ? g_page.MinusTab() : -1;
    const bool on = tab >= 0 && g_minus_page != 0;
    g_map_hide = on;
    if ((on ? 1 : 0) != g_minus_vis) {
        if (on) {
            for (const std::uint64_t q : g_minus_hide) SetVisible(ctx, q, false);
        }
        SetVisible(ctx, g_minus_page, on);
        g_minus_vis = on ? 1 : 0;
        LOG_INFO(Core, "coop: pause tab: the - menu's page {}", on ? kMinusTabNames[tab] : "gone");
    }
    if (!on) {
        g_minus_page_tab = -1;
        return -1;
    }
    if (tab != g_minus_page_tab) {   // (another tab: its rows read again before they show)
        g_minus_page_tab = tab, g_minus_rows_ms = 0, g_minus_shown_rows = ~0u, g_minus_focus = -1;
        g_minus_texts = {};
    }
    std::uint32_t rows = 0;
    for (int r = 0; r < kRows; ++r) rows |= g_minus_texts[static_cast<std::size_t>(r)].empty() ? 0u : 1u << r;
    g_page.SetMinusRows(rows);
    if (rows != g_minus_shown_rows) {
        for (int r = 0; r < kRows; ++r) SetVisible(ctx, g_minus_buttons[static_cast<std::size_t>(r)], ((rows >> r) & 1u) != 0);
        g_minus_shown_rows = rows, g_minus_focus = -1;
    }
    const int focus = rows != 0 ? g_page.MinusRow() : -1;
    // (the game's focus frame on the row picked, as on System's own focused button. Set at every update: the part's
    // own animation may set it back, lane R1)
    for (int r = 0; r < kRows; ++r) {
        const std::size_t k = static_cast<std::size_t>(r);
        if (g_minus_cursors[k] != 0) SetVisible(ctx, g_minus_cursors[k], r == focus);
        if (g_minus_selects[k] != 0) SetVisible(ctx, g_minus_selects[k], r == focus);
    }
    if (focus != g_minus_focus) {
        // (the frame itself is the button's own Select animation, as the game's buttons show it: lane R1 20261121-085053, the
        // pane alone made visible drew nothing, as PaCommonBtnThin_00's FunctionalAnimSelect / UnSelect key it and
        // no button controller plays them on the strip's copies. The focused row's Select at its last frame, the
        // others' at its first.)
        std::string anim_why;
        for (int r = 0; r < kRows; ++r) {
            const std::size_t k = static_cast<std::size_t>(r);
            if (g_minus_buttons[k] == 0 || g_minus_layout == 0) continue;
            char path[48];
            std::snprintf(path, sizeof path, "Pa_CoopMinus_00/Pa_Button_%02d/Select", r);
            if (const char* why = StopPartAnim(ctx, g_minus_layout, path, r == focus)) {
                anim_why += std::string(anim_why.empty() ? "" : ", ") + path + " " + why;
            }
        }
        if (!g_minus_anim_said || !anim_why.empty()) {
            g_minus_anim_said = anim_why.empty();
            LOG_INFO(Core, "coop: pause tab: the - menu's page: the focus frames by the buttons' Select animation: {}",
                     anim_why.empty() ? std::string("set") : anim_why);
        }
        LOG_INFO(Core, "coop: pause tab: the - menu's page: focus on row {}", focus);
        g_minus_focus = focus;
    }
    // (the list on the right: each line shown with its words (MinusWords set them), its value box only with a value,
    // its arrows only where Left and Right change it; the line picked bright, as the + page's list)
    for (int i = 0; i < kLinePanes; ++i) {
        const std::size_t k = static_cast<std::size_t>(i);
        MinusList& l = g_minus_lists[k];
        if (l.pane == 0) continue;
        const ListLine& line = g_minus_lines[k];
        const bool shown = !line.name.empty();
        if (l.vis != (shown ? 1 : 0)) SetVisible(ctx, l.pane, shown), l.vis = shown ? 1 : 0;
        if (!shown) continue;
        if (const int box = line.value.empty() ? 0 : 1; l.box_vis != box) {
            for (std::size_t j = 0; j < 2; ++j) {
                if (l.box[j] != 0) SetVisible(ctx, l.box[j], box != 0 && l.box_own[j]);
            }
            l.box_vis = box;
        }
        if (const int arrows = line.arrows ? 1 : 0; l.arrows != 0 && l.arrows_vis != arrows) {
            SetVisible(ctx, l.arrows, arrows != 0), l.arrows_vis = arrows;
        }
        if (l.alpha != g_minus_line_alpha[k]) CallAlpha(ctx, l.pane, g_minus_line_alpha[k]), l.alpha = g_minus_line_alpha[k];
    }
    // (the rows: the focused one bright, the others a little dim, all dimmed while the list has the pad)
    for (int r = 0; r < kRows; ++r) {
        const std::size_t k = static_cast<std::size_t>(r);
        const float alpha = g_minus_rows_alpha < 1.0f ? g_minus_rows_alpha : r == focus ? 1.0f : 0.8f;
        if (g_minus_buttons[k] == 0 || g_minus_rows_set[k] == alpha) continue;
        CallAlpha(ctx, g_minus_buttons[k], alpha);
        g_minus_rows_set[k] = alpha;
    }
    // (every 250 ms, and at once after a press changed something: g_live_poke, the + page's own not up meanwhile)
    return NowMs() - g_minus_rows_ms >= 250 || g_live_poke.exchange(false) ? tab : -1;
}

// After the lock, on the strip's update: the page's words for `tab`, set through the placeholders (a row whose words
// cannot be set does not show): the tab's own rows (RowTexts(MinusPage)) or, once a row opened one, its view's rows
// (RowTexts(View)), and the list on the right: the view's details (DetailLines), the game browser's games
// (ListLines), the typing keys (TypingLines), co-op's menu's page (Co-op settings, MenuLines) or the Mods tab's page.
// Which lines can be picked go to the Page (SetMinusLines), as whether a view shows (SetMinusInView).
void FollowSays();
int g_minus_view_seen = -1;   // the view MinusWords last showed (g_mutex)
void MinusWords(void* ctx, std::uint64_t layout, std::uint64_t gen, int tab) {
    if (tab < 0 || tab >= kMinusTabs) return;
    if (tab != static_cast<int>(MinusPage::kMods)) FollowSays();   // (a Host or Join refused: why, on the Session view)
    const View view = ViewShown();
    bool in_list = false;
    {
        std::lock_guard lock(g_mutex);
        if (static_cast<int>(view) != g_minus_view_seen) {   // (another view: its first row, Co-op settings' its page's)
            g_minus_view_seen = static_cast<int>(view);
            g_page.MinusViewChanged(view == View::kMenu ? g_menu_tab.load() : 0);
        }
        in_list = g_page.MinusInList();
    }
    if (view == View::kMenu) g_menu_list = in_list;   // (co-op's menu's selected row in view while the list has the pad)
    const SessionView v = ViewNow(view);
    const bool mods = tab == static_cast<int>(MinusPage::kMods);
    std::vector<std::string> rows = mods                 ? std::vector<std::string>(kRows)
                                    : view == View::kMain ? RowTexts(static_cast<MinusPage>(tab), v)
                                                          : RowTexts(view, v);
    rows.resize(kRows);
    // (the list: its lines, which can be picked, whether its moves are co-op's menu's own, the line picked)
    std::vector<ListLine> lines;
    std::uint32_t pickable = 0;
    bool raw = false, raw_back = false;
    int picked = -1;
    const int per_page = g_minus_lists[kLinePanes - 1].pane != 0 ? kMenuMost : kCells;
    const auto menu_list = [&](const CoopPauseMenu::View& mv, int menu_tab, int* top, const std::string& head) {
        MenuList ml = MenuLines(mv, menu_tab, *top, in_list, per_page);
        *top = ml.top;
        if (!head.empty()) ml.lines[0].name = head;
        lines = ml.lines;
        raw = true;
        for (std::size_t k = 0; k < static_cast<std::size_t>(kLinePanes) && k < ml.rows.size(); ++k) {
            if (ml.selectable[k]) pickable |= 1u << k;
        }
        picked = ml.picked >= 0 ? ml.picked + 1 : -1;
    };
    if (mods) {
        CoopPauseMenu::View mv;
        if (CoopModsTab::View(&mv, &raw_back)) menu_list(mv, -1, &g_minus_mods_top, "Mods");
    } else if (!v.typing_lines.empty()) {
        lines = v.typing_lines;   // (the keys: the Page's typing has the pad, the key picked bright)
        picked = v.typing_key + 1;
    } else if (view == View::kLan) {
        lines = ListLines(v);
        for (std::size_t k = 0; k < lines.size() && k < 32; ++k) {
            const bool game = k >= 1 && CellIndex(v.s, static_cast<int>(k) - 1) >= 0;
            if (game || (!lines[k].name.empty() && lines[k].act != RowAction::kNone)) pickable |= 1u << k;
        }
    } else if (view == View::kMenu) {
        int top = v.menu_top;
        menu_list(v.menu, v.menu_tab, &top, "");
        int was = v.menu_top;
        g_menu_top.compare_exchange_strong(was, top);
    } else {
        lines = DetailLines(view, v);
        for (std::size_t k = 0; k < lines.size() && k < 32; ++k) {
            if (!lines[k].name.empty() && lines[k].act != RowAction::kNone) pickable |= 1u << k;
        }
    }
    lines.resize(kLinePanes);
    // (the words: the rows' Coop_Minus_00..04, the lines' 05.. a name and a value each)
    std::array<std::string, kRows> texts{};
    for (std::size_t k = 0; k < texts.size(); ++k) texts[k] = rows[k];
    {
        std::lock_guard lock(g_live_mutex);
        const auto put = [&](int slot, const std::string& path, const std::string& words) {
            char label[20];
            std::snprintf(label, sizeof label, "Coop_Minus_%02d", slot);
            LiveSlot& l = g_minus_live[static_cast<std::size_t>(slot)];
            return LiveLookupIn(ctx, l, "LayoutMsg/MenuOverlay_00", label, gen) && LiveTextIn(ctx, layout, path, l, words);
        };
        for (std::size_t k = 0; k < texts.size(); ++k) {
            if (texts[k].empty()) continue;
            char path[64];
            std::snprintf(path, sizeof path, "Pa_CoopMinus_00/Pa_Button_%02d/T_Btn_00", static_cast<int>(k));
            if (!put(static_cast<int>(k), path, texts[k])) texts[k].clear();
        }
        for (int i = 0; i < kLinePanes; ++i) {
            ListLine& line = lines[static_cast<std::size_t>(i)];
            if (line.name.empty() || g_minus_lists[static_cast<std::size_t>(i)].pane == 0) {
                line.name.clear();
                pickable &= ~(1u << i);
                continue;
            }
            char path[64];
            std::snprintf(path, sizeof path, "Pa_CoopMinus_00/Pa_List_%02d/T_Option_00", i);
            const bool named = put(kRows + 2 * i, path, line.name);
            std::snprintf(path, sizeof path, "Pa_CoopMinus_00/Pa_List_%02d/T_Text_00", i);
            const bool valued = put(kRows + 2 * i + 1, path, line.value.empty() ? std::string(" ") : line.value);
            if (!named || !valued) line.name.clear(), pickable &= ~(1u << i);   // (a line without its words: not shown)
        }
    }
    std::lock_guard lock(g_mutex);
    if (g_minus_page_tab != tab || g_page.MinusTab() != tab) return;   // (another tab meanwhile: its own turn)
    g_minus_texts = texts, g_minus_rows_ms = NowMs();
    g_page.SetMinusInView(!mods && view != View::kMain);
    g_page.SetMinusLines(pickable, raw);
    g_page.SetMinusRawBack(raw_back);
    const bool list_now = g_page.MinusInList() || g_page.Typing() >= 0;
    if (!raw && g_page.MinusInList()) picked = g_page.MinusLine();
    for (int i = 0; i < kLinePanes; ++i) {
        const std::size_t k = static_cast<std::size_t>(i);
        g_minus_lines[k] = lines[k];
        const bool pick = (pickable & (1u << i)) != 0;
        g_minus_line_alpha[k] = i == 0 && !pick           ? 0.7f    // (a header, as a table's)
                                : !pick && picked != i     ? 0.55f   // (a line of text, a heading)
                                : !list_now || picked == i ? 1.0f
                                                           : 0.45f;
    }
    g_minus_rows_alpha = list_now ? 0.45f : 1.0f;
    // (co-op's menu's view: its rows are the menu's pages, the cursor's row the page on the list, as on the + page)
    if (view == View::kMenu && !g_page.MinusInList()) {
        const int row = g_page.MinusRow();
        if (row >= 0 && row < kMenuTabs && row != g_menu_tab.load()) g_menu_tab = row, g_menu_top = 0, g_live_poke = true;
    }
}

// A row picked on our page (after the lock): its action, the tab's own row's (ActionFor(MinusPage)) or its view's
// (Act); a view it opens shows on the same page.
void Act(int row, int line, bool clicked, RowAction forced);
void MinusDecide(int tab, int row) {
    if (tab < 0 || tab >= kMinusTabs) return;
    if (ViewShown() == View::kMenu) {   // (Co-op settings: the row's page on the list, and the list has the pad)
        if (row >= 0 && row < kMenuTabs && g_menu_tab.exchange(row) != row) g_menu_top = 0;
        std::lock_guard lock(g_mutex);
        g_page.MinusEnterList();
        g_live_poke = true;
        return;
    }
    if (ViewShown() != View::kMain) {   // (a view's row: as the view's own)
        Act(row, -1, false, RowAction::kNone);
        return;
    }
    const SessionView v = ViewNow(View::kMain);
    const RowAction a = ActionFor(static_cast<MinusPage>(tab), row, v.s);
    LOG_INFO(Core, "coop: pause tab: the - menu's {} row {} chosen: {}", kMinusTabNames[tab], row, ActionWords(a));
    if (a == RowAction::kNone) return;
    Act(row, -1, false, a);
}

// The Mods tab's page told it shows, or no more (after the lock: the tab takes its own).
void FollowMods(bool shown) {
    if (shown == g_minus_mods_shown) return;
    g_minus_mods_shown = shown;
    if (shown) g_minus_mods_top = 0;
    CoopModsTab::MenuInput(shown ? HOS_COOP_MENU_SHOW : HOS_COOP_MENU_HIDE, 0, 0);
    LOG_INFO(Core, "coop: pause tab: the - menu's Mods page {}", shown ? "shown" : "gone");
}

// The map screen's update (UIMapMenuScreen): the Map page under ours hidden while ours shows and the strip still
// updates, shown again otherwise (the menu closed, L back to Map). One thread (that screen's).
void FollowMapUnder(void* ctx, std::uint64_t screen) {
    const std::uint64_t layout = Read<std::uint64_t>(screen + kScreenLayout);
    if (layout == 0) return;
    const int state = g_strip_state.load();
    const bool want = g_map_hide.load() && NowMs() - g_strip_seen_ms.load() < 300 && (state == 1 || state == 2);
    if (layout != g_map_layout) {
        if (g_map_hidden && g_map_root != 0) SetVisible(ctx, g_map_root, true);   // (an old layout's: shown back)
        g_map_layout = layout, g_map_root = FindOwnPane(ctx, layout, "RootPane"), g_map_hidden = false;
    }
    if (g_map_root == 0 || want == g_map_hidden) return;
    SetVisible(ctx, g_map_root, !want);
    g_map_hidden = want;
    LOG_INFO(Core, "coop: pause tab: the Map page under the - menu's page {}", want ? "hidden" : "shown again");
}

// The row of ours under the game's cursor frame (0..4): the pages screen's focused button, by the focus manager's walk
// (CoopModScreens::FocusedButton, as the mods' screens) and its name, System's Pa_Button_00..04 (RowOfButton); -1 none.
// The game focuses no button for a frame or two while the cursor steps: the row it had last in this opening of the
// pages is kept then. (Read in a run, 2026-11-19: the walk names Pa_Button_00, 01, 02 as the cursor steps down. The
// System page object's button array this read before, +0x320 then +0x38, gave no focus state there, so every A was
// row 0's.)
int FocusedRow() {
    static std::uint64_t last_gen = 0;
    static int last_row = -1;
    const std::uint64_t pages = g_pages_screen;
    if (pages == 0) return -1;
    const auto read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
    const int row = RowOfButton(CoopModScreens::ButtonName(read, CoopModScreens::FocusedButton(read, pages)));
    if (last_gen != g_pages_gen) last_gen = g_pages_gen, last_row = -1;   // (a new opening: nothing kept)
    if (row >= 0) last_row = row;
    return last_row;
}

// The game's cursor's button as a row of the pages screen (Pa_Button_NN: NN, System's 05 and 06 too), -1 none or
// another, and whether that button is hidden now (no hit box), for RowSkip. g_mutex.
int FocusedButtonRow(bool* hidden) {
    *hidden = false;
    const std::uint64_t pages = g_pages_screen;
    if (pages == 0) return -1;
    const auto read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
    const std::uint64_t b = CoopModScreens::FocusedButton(read, pages);
    if (b == 0) return -1;
    const std::string name = CoopModScreens::ButtonName(read, b);
    if (name.size() != 12 || name.compare(0, 10, "Pa_Button_") != 0 || name[10] < '0' || name[10] > '9' ||
        name[11] < '0' || name[11] > '9') {
        return -1;
    }
    *hidden = !CoopModScreens::HitBox(read, b);
    return (name[10] - '0') * 10 + (name[11] - '0');
}

// (trace) The pages screen's buttons in a focus state: name, state, shown or not; logged when that changes while
// `why` (the Settings page) shows, so a run says which button the rows' cursor is read from. g_mutex.
void TraceFocus(const char* why) {
    static std::string last;
    const std::uint64_t pages = g_pages_screen;
    if (pages == 0) return;
    const auto read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
    std::string now;
    for (const std::uint64_t b : CoopModScreens::Buttons(read, pages)) {
        const std::uint8_t state = CoopModScreens::FocusState(read, b);
        if (state == 0) continue;
        char at[24];
        std::snprintf(at, sizeof at, "@%llx", static_cast<unsigned long long>(b & 0xfffff));
        now += CoopModScreens::ButtonName(read, b) + at + "=" + std::to_string(state) +
               (CoopModScreens::HitBox(read, b) ? "(shown) " : "(hidden) ");
    }
    if (now == last) return;
    last = now;
    LOG_INFO(Core, "coop: pause tab: {}: buttons in a focus state: {}; the rows' cursor read as row {}", why,
             now.empty() ? std::string("none") : now, FocusedRow());
}

// --- The mouse over our rows (MouseRows): the cursor and the rows' boxes, read at each pad read. g_mutex. ---
MouseRows g_mouse_rows;
std::uint64_t g_b_list_last = 0;    // (the pad's last read, for B on the list: OnPad)
bool g_b_swallow = false;           // (that B, held: not the page's)
std::uint64_t g_list_left_ms = 0;    // (the mouse's Left off the list: the last one)
std::uint64_t g_page_since_ms = 0;
int g_mouse_steps_logged = 0;        // (trace: the mouse's steps logged so far)
float g_skip_mouse_x = 0, g_skip_mouse_y = 0;   // (the mouse's last place and when it moved: RowSkip waits then)
std::uint64_t g_skip_mouse_ms = 0;   // (when our page showed last: RowSkip waits a moment after)
int g_slow_logged = 0;               // (trace: slow updates of ours logged so far)
RowSkip g_row_skip;          // (the game's cursor kept off hidden buttons on the Multiplayer page)
int g_skip_logged = 0;       // (its steps logged the first times)
std::uint64_t g_row_buttons_gen = 0;
// Every button of the pages screen named Pa_Button_00..05 (System's, the Multiplayer page's, Longshot Settings'), by
// row; found once an opening.
std::array<std::vector<std::uint64_t>, kButtonRows> g_row_buttons{};

// The shown page's rows' boxes where the game puts them now. Each row is the button (of those of its name) whose box
// holds that page's own row pane's place: the first of the name was System's at times, whose lower buttons sit lower
// now (a tester's run, 2026-11-19: the mouse's steps went to the wrong row and the cursor went up and down without
// end). A row of the page not shown (one with nothing on it, ApplyBlankRows) has no box. g_mutex.
RowBoxes ReadRowBoxes(std::uint64_t now_ms) {
    (void)now_ms;
    RowBoxes boxes{};
    const std::uint64_t pages = g_pages_screen;
    if (pages == 0) return boxes;
    const auto read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
    if (g_row_buttons_gen != g_pages_gen) {
        g_row_buttons_gen = g_pages_gen, g_row_buttons = {};
        for (const std::uint64_t b : CoopModScreens::Buttons(read, pages)) {
            const int row = RowOfButton(CoopModScreens::ButtonName(read, b));
            if (row >= 0) g_row_buttons[static_cast<std::size_t>(row)].push_back(b);
        }
    }
    const bool settings = g_page.Which() == 1;
    for (int i = 0; i < kButtonRows; ++i) {
        const std::size_t k = static_cast<std::size_t>(i);
        const std::uint64_t pane = i < kRows ? (settings ? g_set_buttons[k] : g_coop_buttons[k])
                                             : (settings ? g_settings_button_set : 0);
        if (pane == 0 || (Read<std::uint32_t>(pane + 0xa8) & 1u) == 0) continue;   // (not on the page, or hidden)
        float m[12] = {};
        if (!CoopRuntime::Read(pane + 0x28, m, sizeof m)) continue;   // (its global matrix: the place at [9], [10])
        const float cx = m[9], cy = m[10];
        float best = 1e30f;
        for (const std::uint64_t b : g_row_buttons[k]) {
            const auto box = CoopModScreens::HitBox(read, b);
            if (!box) continue;
            const float dx = (box->x0 + box->x1) / 2 - cx, dy = (box->y0 + box->y1) / 2 - cy;
            if (const float d = dx * dx + dy * dy; d < best) {
                best = d;
                boxes[k] = RowBox{true, box->x0, box->y0, box->x1, box->y1};
            }
        }
    }
    return boxes;
}

// The browser's game rows where the game puts them now: each pane's global matrix (+0x28: the scales at [0] and [4],
// the place at [9] and [10], as the map page reads its panes) and an Options row's bar (1880 wide).
std::array<RowBox, kCells> ReadCellBoxes() {
    std::array<RowBox, kCells> boxes{};
    for (int i = 0; i < kCells; ++i) {
        const std::uint64_t pane = g_cell_pane_at[static_cast<std::size_t>(i)].load();
        float m[12] = {};
        if (pane == 0 || !CoopRuntime::Read(pane + 0x28, m, sizeof m) || !(m[0] > 1e-3f) || !(m[4] > 1e-3f)) continue;
        if ((Read<std::uint32_t>(pane + 0xa8) & 1u) == 0) continue;   // (hidden: bit 0 of its flags, kSetVisible's)
        const float hw = 940.0f * m[0], hh = 70.0f * m[4];   // (an Options row: 1880 wide, its bar about 140 tall)
        boxes[static_cast<std::size_t>(i)] = RowBox{true, m[9] - hw, m[10] - hh, m[9] + hw, m[10] + hh};
    }
    return boxes;
}

// The list's first line's box (the heading), or none shown.
RowBox HeadBox() {
    const std::uint64_t pane = g_head_pane_at.load();
    float m[12] = {};
    if (pane == 0 || !CoopRuntime::Read(pane + 0x28, m, sizeof m) || !(m[0] > 1e-3f) || !(m[4] > 1e-3f)) return {};
    if ((Read<std::uint32_t>(pane + 0xa8) & 1u) == 0) return {};
    const float hw = 940.0f * m[0], hh = 70.0f * m[4];
    return RowBox{true, m[9] - hw, m[10] - hh, m[9] + hw, m[10] + hh};
}

// The list's eighth line's box (Settings and look's seventh row), or none shown.
RowBox LastLineBox() {
    const std::uint64_t pane = g_cell_pane_at[kCells].load();
    float m[12] = {};
    if (pane == 0 || !CoopRuntime::Read(pane + 0x28, m, sizeof m) || !(m[0] > 1e-3f) || !(m[4] > 1e-3f)) return {};
    if ((Read<std::uint32_t>(pane + 0xa8) & 1u) == 0) return {};
    const float hw = 940.0f * m[0], hh = 70.0f * m[4];
    return RowBox{true, m[9] - hw, m[10] - hh, m[9] + hw, m[10] + hh};
}

// The list's line under x, y (its first, the header, then the cells' lines, the eighth too), or -1.
int LineAt(float x, float y) {
    const std::array<RowBox, kCells> cells = ReadCellBoxes();
    if (const int c = CellAt(cells, x, y); c >= 0) return c + 1;
    if (const RowBox b = LastLineBox(); b.shown && x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) return kCells + 1;
    const std::uint64_t pane = g_head_pane_at.load();
    float m[12] = {};
    if (pane == 0 || !CoopRuntime::Read(pane + 0x28, m, sizeof m) || !(m[0] > 1e-3f) || !(m[4] > 1e-3f)) return -1;
    if ((Read<std::uint32_t>(pane + 0xa8) & 1u) == 0) return -1;
    const float hw = 940.0f * m[0], hh = 70.0f * m[4];
    return std::abs(x - m[9]) <= hw && std::abs(y - m[10]) <= hh ? 0 : -1;
}

#ifdef _WIN32
BOOL CALLBACK FindGameWindow(HWND h, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    char cls[64] = {};
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetClassNameA(h, cls, sizeof cls) > 0 &&
        CoopWindowBrand::IsGameWindowClass(cls)) {
        *reinterpret_cast<HWND*>(out) = h;
        return FALSE;
    }
    return TRUE;
}
#endif

// (a test run's, LICHII_COOP_TEST_STATE=1 only: the left button as the file mouse.now in the game's folder says it,
// "1" down, "0" up, when the PC's own state does not show a test's injected clicks; read at most every 15 ms)
bool TestLeftButton(bool held) {
    static const bool on = [] {
        const char* v = std::getenv("LICHII_COOP_TEST_STATE");
        return v != nullptr && v[0] == '1';
    }();
    if (!on) return held;
    static std::atomic<std::uint64_t> next{0};
    static std::atomic<int> said{-1};
    if (const std::uint64_t now = NowMs(); now >= next.load()) {
        next = now + 15;
        int v = -1;
        if (FILE* f = std::fopen((CoopRuntime::GameDir() / "mouse.now").string().c_str(), "rb")) {
            const int c = std::fgetc(f);
            v = c == '1' ? 1 : c == '0' ? 0 : -1;
            std::fclose(f);
        }
        said = v;
    }
    return said.load() < 0 ? held : said.load() == 1;
}

// The cursor over the game's picture, in the layout's units, with the game's window in front (false otherwise), and
// the left button.
bool ReadCursor(float* x, float* y, bool* left) {
#ifdef _WIN32
    static HWND window = nullptr;
    if (window == nullptr || !IsWindow(window)) {
        window = nullptr;
        EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&window));
        if (window == nullptr) return false;
    }
    if (GetForegroundWindow() != window) return false;
    POINT p{};
    RECT rc{};
    if (!GetCursorPos(&p) || !ScreenToClient(window, &p) || !GetClientRect(window, &rc)) return false;
    if (p.x < 0 || p.y < 0 || p.x >= rc.right || p.y >= rc.bottom) return false;
    std::tie(*x, *y) = LayoutPoint(p.x, p.y, rc.right, rc.bottom);
    *left = TestLeftButton((GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0);
    return true;
#else
    (void)x, (void)y, (void)left;
    return false;   // (Longshot's mouse in the game's menus is Windows' only)
#endif
}

// The cursor over the game window's client area, 0 to 1 across it, and the left button (the title's mouse reads it,
// coop_title.cpp). False when the game's window is not in front or the cursor is off it.
bool ReadClientCursor(float* fx, float* fy, bool* left) {
#ifdef _WIN32
    static HWND window = nullptr;
    if (window == nullptr || !IsWindow(window)) {
        window = nullptr;
        EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&window));
        if (window == nullptr) return false;
    }
    if (GetForegroundWindow() != window) return false;
    POINT p{};
    RECT rc{};
    if (!GetCursorPos(&p) || !ScreenToClient(window, &p) || !GetClientRect(window, &rc)) return false;
    if (p.x < 0 || p.y < 0 || p.x >= rc.right || p.y >= rc.bottom || rc.right <= 0 || rc.bottom <= 0) return false;
    *fx = static_cast<float>(p.x) / static_cast<float>(rc.right), *fy = static_cast<float>(p.y) / static_cast<float>(rc.bottom);
    *left = TestLeftButton((GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0);
    return true;
#else
    (void)fx, (void)fy, (void)left;
    return false;
#endif
}

// What changes the list's typing keys show: the fields, their cursors, the key and the letter picked. g_mutex.
std::uint64_t TypingHash() {
    std::string all;
    for (int f = 0; f < Page::kFields; ++f) all += g_page.Field(f).text() + '\n' + std::to_string(g_page.Field(f).cursor()) + '\n';
    all += std::to_string(g_page.TypingKey()) + '\n' + g_page.TypingLetter();
    return std::hash<std::string>{}(all);
}
// The mouse on the typing keys (the list's rows): a key pressed where the left button went down and came up; Longshot's
// own A for the click (it presses A for a click in the game's menus) is not a second press. g_mutex.
bool g_kb_held = false;
int g_kb_press = -1;
std::uint64_t g_kb_click_ms = 0;

// A click's own A after a view or the page came: the same click, not the row's that took its place under the mouse
// (a tester's click on Join a game opened the Join view and then the keyboard of its Room or IP row, there).
std::atomic<std::uint64_t> g_view_ms{0}, g_click_down_ms{0};
bool g_click_held = false;
constexpr std::uint64_t kClickThroughMs = 450;

void SetView(View v) {
    if (g_view.exchange(static_cast<int>(v)) != static_cast<int>(v)) {
        g_live_poke = true;       // (its rows and list at the next update: the list's place too)
        g_view_ms = NowMs();
        g_confirm_until = 0;      // (a new view: no A waited)
        g_show_password = false;  // (and the password hidden again)
        g_cell = -1;              // (the browser's cells give the pad back)
    }
    if (v != View::kSession) {
        std::lock_guard lock(g_says_mutex);
        g_says.clear();
    }
}

// A new word from the page's last action (CoopPause::PageSays): the Session view shows it. Outside g_mutex (co-op's
// own lock is taken).
void FollowSays() {
    std::uint64_t gen = 0;
    std::string says = CoopPause::PageSays(&gen);
    {
        std::lock_guard lock(g_says_mutex);
        if (gen == g_says_seen) return;
        g_says_seen = gen;
    }
    SetView(View::kSession);
    std::lock_guard lock(g_says_mutex);
    g_says = std::move(says);
    g_live_poke = true;
}

// A row that wants a second A: true on that second A (within kConfirmMs, the cursor still on the row), else it waits.
bool SecondA(View view, int row) {
    if (Confirming(view, row)) {
        g_confirm_until = 0;
        return true;
    }
    g_confirm_view = static_cast<int>(view), g_confirm_row = row;
    // (a click's: the game's cursor may not be on that row yet, the mouse steps it there; the wait stays until the
    // second click or its time. Kept while the cursor was elsewhere, a tester's Stop hosting never took its second A)
    g_confirm_by_mouse = g_act_clicked;
    g_confirm_until = NowMs() + kConfirmMs;   // (the row says "A again" until then)
    return false;
}

// A cell of the game browser chosen (A on it, or a click): that game's own view. A game on this network: its view
// (its password, then Join this game). A room on the relay: the Join view with its name typed in (its password next).
// After the lock, as Act.
void ActCell(int cell) {
    const SessionView v = ViewNow(View::kLan);
    const int i = CellIndex(v.s, cell);
    if (i < 0) return;
    if (v.s.online) {
        const std::vector<CoopRoomList::Room> rooms = SearchedRooms(v.search);   // (as the cells list them)
        if (i >= static_cast<int>(rooms.size())) return;
        const CoopRoomList::Room& r = rooms[static_cast<std::size_t>(i)];
        if (!CoopRoomList::Joinable(r, coop::protocol::kProtocolVersion)) {
            LOG_INFO(Core, "coop: pause tab: cell {} chosen: that room cannot be joined from here (full, or another "
                           "version)", cell);
            return;
        }
        if (r.is_public) {
            // A public game: joined at once, no password to type (its password is the room's, as its host made it).
            LOG_INFO(Core, "coop: pause tab: cell {} chosen: a public game on the list, joining it", cell);
            CoopPause::JoinRoomNow(r.room, coop::protocol::public_join_password(r.room));
            return;
        }
        // A private game (or an older kit's listing, which may lock out wrong passwords): never tried with the
        // public password; the Join view with the room typed, for the host's password.
        LOG_INFO(Core, "coop: pause tab: cell {} chosen: a private game on the list (needs the host's password), to "
                       "the Join view", cell);
        {
            std::lock_guard lock(g_mutex);
            g_page.Field(0).Clear();
            for (const char c : r.room) g_page.Field(0).Type(static_cast<unsigned char>(c));
            g_page.Field(1).Clear();
            g_live_poke = true;
        }
        SetView(View::kJoin);
        return;
    }
    const auto games = SearchedGames(v.search);   // (as the cells list them)
    if (i >= static_cast<int>(games.size())) return;
    const CoopLan::Heard& h = games[static_cast<std::size_t>(i)];
    if (!CoopLan::Joinable(h, coop::protocol::kProtocolVersion)) {
        LOG_INFO(Core, "coop: pause tab: cell {} chosen: that game cannot be joined from here (full, or another "
                       "version)", cell);
        return;
    }
    LOG_INFO(Core, "coop: pause tab: cell {} chosen: a game on this network, to its view", cell);
    {
        std::lock_guard lock(g_mutex);
        g_lan_pick = LanPick{h.address, CoopLan::RowText(h, coop::protocol::kProtocolVersion), h.game.port};
    }
    SetView(View::kLanGame);   // (its view: Join this game, a password typed first if its host asks one)
}

// Settings and look: co-op's menu shows the view's page (CoopPause::OpenNative, no overlay) while the view shows, and
// shuts with it; then Link wears the look chosen (as the overlay's menu does when it closes). On the pad's thread, no
// lock of ours held (co-op's menu takes its own).
void FollowMenu(bool shown) {
    const int want = shown && ViewShown() == View::kMenu ? g_menu_tab.load() : -1;
    const int open = g_menu_open.load();
    if (want == open) return;
    g_menu_open = want;
    if (want < 0) {
        CoopPause::CloseNative();
        CoopLook::ApplyOnUnpause();
        g_menu_list = false;
        {
            std::lock_guard lock(g_mutex);
            g_menu_typing_lines.clear();
        }
        LOG_INFO(Core, "coop: pause tab: settings and look: closed (Link wears the look chosen)");
    } else {
        if (open < 0) CoopPause::OpenNative(MenuTabPage(want));   // (the files' settings read once, as the view opens)
        else CoopPause::NativeGo(MenuTabPage(want));              // (another of its pages: no files or devices read)
        g_menu_top = 0;
        LOG_INFO(Core, "coop: pause tab: settings and look: {} on the list", kMenuTabNames[static_cast<std::size_t>(want)]);
    }
    g_live_poke = true;
}

// A row's action, or (line >= 0) a line of the details list's (DetailLines' act: their Auto-rejoin) (after the lock:
// co-op's menu, the Lobby and the chat take their own).
void Act(int row, int line = -1, bool clicked = false, RowAction forced = RowAction::kNone) {
    g_act_clicked = clicked;
    const View view = ViewShown();
    const SessionView v = ViewNow(view);
    RowAction a = forced;
    if (forced != RowAction::kNone) {
        // (a row of the - menu's page: MinusDecide named its action)
    } else if (line >= 0) {
        const std::vector<ListLine> lines = view == View::kLan ? ListLines(v) : DetailLines(view, v);
        if (line < static_cast<int>(lines.size())) a = lines[static_cast<std::size_t>(line)].act;
        LOG_INFO(Core, "coop: pause tab: details line {} chosen (view {}): {}", line, static_cast<int>(view), ActionWords(a));
    } else {
        a = ActionFor(view, row, v.s);
        LOG_INFO(Core, "coop: pause tab: row {} chosen (view {}): {}", row, static_cast<int>(view), ActionWords(a));
    }
    const std::uint8_t player = static_cast<std::uint8_t>(g_player.load());
    const auto host_action = [player](coop::protocol::LobbyActionKind kind) {
        CoopLobby::Ask(coop::protocol::LobbyAction{kind, player});   // (the server's answer shows in the Lobby, the chat)
    };
    using Lobby = coop::protocol::LobbyActionKind;
    switch (a) {
    case RowAction::kViewPlayers:
        g_players_page = 0;
        SetView(View::kPlayers);
        break;
    case RowAction::kViewPlayer: {
        const CoopLobby::Seen seen = CoopLobby::Now();
        const int i = PlayerIndex(static_cast<int>(seen.players.size()), v.s.page, row);
        if (seen.connected && i >= 0 && i < static_cast<int>(seen.players.size())) {
            g_player = seen.players[static_cast<std::size_t>(i)].number;
            SetView(View::kPlayer);
        }
        break;
    }
    case RowAction::kMorePlayers: g_players_page = (v.s.page + 1) % PlayerPages(v.s.listed); break;
    case RowAction::kHostControls: SetView(View::kPlayerHost); break;
    case RowAction::kViewLetIn: SetView(View::kLetIn); break;
    case RowAction::kViewJoin: SetView(View::kJoin); break;
    case RowAction::kViewSession: SetView(View::kSession); break;
    case RowAction::kViewHost: SetView(View::kHost); break;
    case RowAction::kBack: SetView(BackOf(view)); break;
    case RowAction::kTeleport: CoopHook::SendChat("/teleport " + v.player_name); break;   // (as the Lobby tab's)
    case RowAction::kOwnVoice: CoopLobby::SetOwnMute(player, CoopLobby::kVoice, (v.own_mutes & kMuteVoice) == 0); break;
    case RowAction::kOwnChat: CoopLobby::SetOwnMute(player, CoopLobby::kChat, (v.own_mutes & kMuteChat) == 0); break;
    case RowAction::kHostVoice: host_action((v.host_mutes & kMuteVoice) != 0 ? Lobby::unmute_voice : Lobby::mute_voice); break;
    case RowAction::kHostChat: host_action((v.host_mutes & kMuteChat) != 0 ? Lobby::unmute_chat : Lobby::mute_chat); break;
    case RowAction::kKick:
        if (SecondA(view, row)) {
            LOG_INFO(Core, "coop: pause tab: the second A: player {} kicked", player);
            host_action(Lobby::kick);
        }
        break;
    case RowAction::kBan:
        if (SecondA(view, row)) {
            LOG_INFO(Core, "coop: pause tab: the second A: player {} banned for this session", player);
            host_action(Lobby::ban);
        }
        break;
    case RowAction::kResync: CoopSave::RequestResync(); break;   // (the chat says how it goes)
    case RowAction::kDeny: CoopHook::SendChat("/deny"); break;   // (its answer shows in the chat)
    case RowAction::kJoinPaste:
        SetView(View::kMain);   // (a join asked: the five rows follow it; a refusal shows the Session view, FollowSays)
        CoopPause::JoinPasteNow();
        break;
    case RowAction::kViewLan:
        g_lan_page = 0, g_cell_page = 0, g_cell = -1;
        SetView(View::kLan);
        CoopLanNet::Look(true, NowMs());   // (the listener opens at once: the first games come within two seconds)
        break;
    case RowAction::kMoreLan: g_lan_page = g_lan_page.load() + 1; break;
    case RowAction::kBrowseLan:
    case RowAction::kBrowseOnline: {
        // (the list already shown: A goes into its cells, as Right does; the other: the cells show it)
        const bool online = a == RowAction::kBrowseOnline;
        if (online == v.s.online) {
            if (g_cell_games.load() > 0) g_cell = 0, g_live_poke = true;
            break;
        }
        g_online = online, g_cell_page = 0, g_cell = -1;
        if (!online) CoopLanNet::Look(true, NowMs());
        break;
    }
    case RowAction::kBrowseRefresh:
        g_cell = -1;
        if (v.s.online) {
            CoopRoomList::AskNow();
        } else {
            CoopLanNet::Forget();
        }
        break;
    case RowAction::kBrowsePage:
        g_cell_page = (v.s.cell_page + 1) % CellPages(BrowseListed(v.s)), g_cell = -1;
        break;
    case RowAction::kBrowseList:
        g_online = !v.s.online, g_cell_page = 0, g_cell = -1;
        if (v.s.online) CoopLanNet::Look(true, NowMs());   // (this network's games: heard from now)
        g_live_poke = true;
        break;
    case RowAction::kBrowseFilter:
        g_browse_filter = (v.s.browse_filter + 1) % CoopBrowse::kFilters, g_cell_page = 0, g_cell = -1, g_live_poke = true;
        break;
    case RowAction::kBrowseSort:
        g_browse_sort = (v.s.browse_sort + 1) % CoopBrowse::kSorts, g_cell_page = 0, g_cell = -1, g_live_poke = true;
        break;
    case RowAction::kTypeRoomName: {   // (this game's room name on the keyboard; OnPad saves it)
        const std::string name = CoopPause::RoomName();   // (co-op's own lock: before ours)
        std::lock_guard lock(g_mutex);
        if (g_page.Typing() == 6) break;
        g_page.Field(6).Set(name);
        g_page.StartTyping(6);
        g_live_poke = true;
        break;
    }
    case RowAction::kListRoom: CoopPause::SetListRoom(!v.s.list_room); break;
    case RowAction::kJoinLan: {
        const auto games = CoopLanNet::Games(NowMs());
        const int i = PlayerIndex(static_cast<int>(games.size()), v.s.lan_page, row);
        if (i >= 0 && i < static_cast<int>(games.size())) {
            const CoopLan::Heard& h = games[static_cast<std::size_t>(i)];
            {
                std::lock_guard lock(g_mutex);
                g_lan_pick = LanPick{h.address, CoopLan::RowText(h, coop::protocol::kProtocolVersion), h.game.port};
            }
            SetView(View::kLanGame);   // (its view: Join this game, a password typed first if its host asks one)
        }
        break;
    }
    case RowAction::kJoinLanGame: {
        LanPick pick;
        std::string password;
        {
            std::lock_guard lock(g_mutex);
            pick = g_lan_pick, password = g_page.Field(1).text();
            g_page.Field(1).Clear();   // (not kept once used, as after a room's join)
            g_live_poke = true;
        }
        if (!pick.address.empty()) {
            SetView(View::kMain);
            CoopPause::JoinLanNow(pick.address, pick.port, password);
        }
        break;
    }
    case RowAction::kAutoRejoin: CoopPause::SetAutoRejoin(!CoopPause::AutoRejoin()); break;
    case RowAction::kShowPassword: g_show_password = !g_show_password.load(); break;
    case RowAction::kViewPassword: SetView(View::kPassword); break;
    case RowAction::kChangePort: {   // (the port on the keyboard, the one now to start from; OnPad hands it over)
        std::lock_guard lock(g_mutex);
        g_page.Field(3).Set(v.port > 0 ? std::to_string(v.port) : std::string());
        g_page.StartTyping(3);
        g_live_poke = true;
        break;
    }
    case RowAction::kTypeSearch: {   // (the online list's search on the keyboard; the list follows it as typed)
        std::lock_guard lock(g_mutex);
        if (g_page.Typing() == 5) break;   // (a click's and Longshot's A for the same row: once)
        g_page.StartTyping(5);
        g_live_poke = true;
        break;
    }
    case RowAction::kTypeRelay: {   // (the relay's address on the keyboard; OnPad saves it)
        const std::string relay = CoopPause::Relay();   // (co-op's own lock: before ours)
        std::lock_guard lock(g_mutex);
        g_page.Field(4).Set(relay);
        g_page.StartTyping(4);
        g_live_poke = true;
        break;
    }
    case RowAction::kChangePassword: {   // (a new one on the keyboard; TakeTyped hands it to the host, OnPad)
        std::lock_guard lock(g_mutex);
        g_page.Field(2).Clear();
        g_page.StartTyping(2);
        g_live_poke = true;
        break;
    }
    case RowAction::kTypeRoom:
    case RowAction::kTypePassword: {
        std::lock_guard lock(g_mutex);
        g_page.StartTyping(a == RowAction::kTypePassword ? 1 : 0);
        g_live_poke = true;
        break;
    }
    case RowAction::kJoinRoom: {
        std::string room, password;
        {
            std::lock_guard lock(g_mutex);
            room = g_page.Field(0).text(), password = g_page.Field(1).text();
            g_page.Field(1).Clear();   // (the password is not kept once used; the room name stays for a retry)
            g_live_poke = true;
        }
        SetView(View::kMain);
        if (room.find_first_of(".:") != std::string::npos) {   // (an address, its port after a colon: joined directly)
            std::string host = room;
            int port = coop::protocol::kDefaultPort;
            if (const auto colon = room.rfind(':'); colon != std::string::npos) {
                host = room.substr(0, colon);
                port = std::atoi(room.c_str() + colon + 1);
            }
            if (!host.empty() && port >= 1 && port <= 65535) {
                CoopPause::JoinLanNow(host, static_cast<std::uint16_t>(port), password);
            } else {
                LOG_INFO(Core, "coop: pause tab: the Join view's address has no usable port");
            }
            break;
        }
        CoopPause::JoinRoomNow(room, password);   // (refused: the Session view says why, FollowSays)
        break;
    }
    case RowAction::kRejoin:
        SetView(View::kMain);
        CoopPause::RejoinNow();
        break;
    case RowAction::kHost: CoopPause::HostNow(); break;
    case RowAction::kConfirmLeave:
        if (SecondA(view, row)) {
            LOG_INFO(Core, "coop: pause tab: the second A: co-op stops");
            CoopPause::LeaveNow();
        }
        break;
    case RowAction::kOpenMain: CoopPause::Open(CoopPauseMenu::Page::kMain); break;
    case RowAction::kCopyCode: CoopPause::PageCopyNow(CoopPause::PageCopy::kCode); break;
    case RowAction::kCopyAnswer: CoopPause::PageCopyNow(CoopPause::PageCopy::kAnswer); break;
    case RowAction::kCopyLan: CoopPause::PageCopyNow(CoopPause::PageCopy::kLan); break;
    case RowAction::kCopyVpn: CoopPause::PageCopyNow(CoopPause::PageCopy::kVpn); break;
    case RowAction::kViewMenu:
        g_menu_tab = 0, g_menu_list = false, g_menu_top = 0, g_menu_focus_reset = true;
        SetView(View::kMenu);
        break;
    case RowAction::kViewLocal:   // (Settings and look on its Local co-op page: kMenuTabNames' last)
        g_menu_tab = kMenuTabs - 1, g_menu_list = false, g_menu_top = 0, g_menu_focus_reset = true;
        SetView(View::kMenu);
        break;
    case RowAction::kMenuTab:   // (A on a page's row: its rows on the list, which has the pad)
        if (row >= 0 && row < kMenuTabs) {
            if (g_menu_tab.exchange(row) != row) g_menu_top = 0;
            g_menu_list = true;
            g_live_poke = true;
        }
        break;
    case RowAction::kAllow: CoopHook::SendChat("/allow"); break;   // (its answer, or "nobody asks", shows in the chat)
    case RowAction::kPasteAnswer: CoopPause::PasteAnswerNow(); break;
    case RowAction::kNone: break;
    }
}

// A click on a line of the details list (OnPad: pressed and let go on it): its action, with the menu's sound; a line
// with none (a value to read) does nothing. No lock of ours held.
void ClickLine(int line) {
    const View view = ViewShown();
    const SessionView v = ViewNow(view);
    const std::vector<ListLine> lines = view == View::kLan ? ListLines(v) : DetailLines(view, v);
    if (line < 0 || line >= static_cast<int>(lines.size()) || lines[static_cast<std::size_t>(line)].act == RowAction::kNone) return;
    {
        std::lock_guard lock(g_mutex);
        g_page.Sound("button_decide");
    }
    Act(-1, line, true);
}

}  // namespace

void Init(const HosCoopApi* api) {
    g_api = api;
}

// (a test run's view, LICHII_COOP_TEST_STATE=1 only) The pause menu's 3D model's display type (TestState's pouch:
// 0 Equipment's Link, 1 an item, 4 none), and Settings and look's Link moved by the file model.now in the game's
// folder ("x drop z": its place set with a tester, MenuModel's kModelX, kModelDropY, kModelZ).
std::atomic<int> g_test_pouch{-9};
void TestPouch(std::uint64_t manager) {
    static const bool on = [] {
        const char* v = std::getenv("LICHII_COOP_TEST_STATE");
        return v != nullptr && v[0] == '1';
    }();
    if (!on || manager == 0) return;
    const std::uint64_t obj = Read<std::uint64_t>(manager + kPouchObject);
    g_test_pouch = obj != 0 ? Read<std::int32_t>(obj + kPouchType) : -8;
    static std::uint64_t next = 0;
    if (NowMs() < next) return;
    next = NowMs() + 200;
    std::error_code ec;
    if (const std::filesystem::path mf = CoopRuntime::GameDir() / "model.now"; std::filesystem::exists(mf, ec)) {
        float x = 0, dy = 0, z = 0;
        if (FILE* in = std::fopen(mf.string().c_str(), "rb")) {
            if (std::fscanf(in, "%f %f %f", &x, &dy, &z) == 3) g_model_x = x, g_model_drop = dy, g_model_z = z, g_model_test = true;
            std::fclose(in);
        }
        std::filesystem::remove(mf, ec);
        LOG_INFO(Core, "coop: pause tab: test: the menu's Link at x {:.2f}, drop {:.2f}, z {:.2f}", x, dy, z);
    }
}

// The - menu research (ui-tabs-0119): with COOP_TEST_SCREEN_PROBE=1 every screen update is counted (Census) and the
// changes logged, with the menu manager's Plus and Minus categories. Off: one cached read of the variable.
static void CensusUpdate(std::uint64_t screen, std::uint64_t vtable_offset) {
    static const bool on = [] {
        const char* v = std::getenv("COOP_TEST_SCREEN_PROBE");
        return v != nullptr && v[0] == '1';
    }();
    if (!on) return;
    static std::mutex census_mutex;
    static Census census;
    Census::Menu m;
    if (const std::uint64_t manager = Read<std::uint64_t>(CoopRuntime::MainBase() + kMenuManager); manager != 0) {
        m.plus = Read<std::int32_t>(manager + kManagerPage);
        m.minus = Read<std::int32_t>(manager + 0x52c);
        m.minus_on = Read<std::uint8_t>(manager + 0x53c);
    }
    const int state = Read<std::uint8_t>(screen + kScreenState);
    std::vector<std::string> lines;
    {
        std::lock_guard lock(census_mutex);
        lines = census.Seen(vtable_offset, state, m, NowMs());
    }
    for (const auto& l : lines) LOG_INFO(Core, "coop: pause tab: {}", l);
}

void OnScreen(void* ctx, std::uint64_t screen) noexcept {
    try {
        if (screen == 0) return;
        const ScreenGuard guard;   // (a screen update run again by one of our game calls, on this thread: skipped)
        if (guard.Nested()) return;
        const std::uint64_t base = CoopRuntime::MainBase();
        const std::uint64_t vtable = Read<std::uint64_t>(screen);
        if (vtable < base) return;
        Facts f;
        f.kind = KindOf(vtable - base);
        CensusUpdate(screen, vtable - base);   // (the - menu research: only with COOP_TEST_SCREEN_PROBE=1)
        SnailScreen(ctx, screen);              // (Character Select's Customize screen: co-op's customization in it)
        if (vtable - base == kMapMenuVtable && ctx != nullptr && CanCall()) FollowMapUnder(ctx, screen);
        if (f.kind == Kind::kOther) return;   // (most updates: the HUD's and the other screens', once a frame each)
        if (f.kind == Kind::kStrip) {   // (row 199: any thread may ask whether the menu is up, GameMenuUp)
            g_strip_state = Read<std::uint8_t>(screen + kScreenState);
            g_strip_seen_ms = NowMs();
        }
        f.screen = screen;
        f.state = Read<std::uint8_t>(screen + kScreenState);
        if (f.kind != Kind::kBlocker) f.layout = Read<std::uint64_t>(screen + kScreenLayout);
        if (f.kind == Kind::kStrip) {
            f.tabs = static_cast<int>(Read<std::uint32_t>(screen + kStripTabs));
            f.mode = Read<std::int32_t>(screen + kStripMode);
            f.lit = Read<std::int32_t>(screen + kStripLit);
        } else if (f.kind == Kind::kPages) {
            if (const std::uint64_t manager = Read<std::uint64_t>(base + kMenuManager); manager != 0) {
                f.page = Read<std::int32_t>(manager + kManagerPage);
                f.asked = Read<std::int32_t>(manager + kManagerAsked);
                TestPouch(manager);
            }
        }
        if (f.kind == Kind::kPages) {
            g_pages_seen_ms = NowMs(), g_pages_seen_state = f.state, g_pages_seen_page = f.page;
        }
        // (back to the title: the question "Return to the title screen?" open, its first button, Title Screen, chosen
        // as the player's A on it would)
        if (f.kind == Kind::kBlocker && vtable - base == kSystemWindowVtable && f.state == 2 && g_to_title.load() == 2 &&
            ctx != nullptr && CanCall()) {
            const auto read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
            std::string names;
            std::uint64_t yes = 0;
            for (const std::uint64_t b : CoopModScreens::Buttons(read, screen)) {
                const std::string name = CoopModScreens::ButtonName(read, b);
                names += (names.empty() ? "" : ", ") + name;
                if (name == "Pa_Button_00") yes = b;
            }
            std::uint64_t took = 0;
            const bool called = yes != 0 && Call(ctx, CoopModScreens::kButtonDecide, yes, 0, 0, 0, 0, &took);
            LOG_INFO(Core, "coop: pause tab: back to the title: the question's buttons {}: Title Screen (Pa_Button_00) "
                           "chosen{}",
                     names, called && (took & 1) != 0 ? "" : " (it did not take it)");
            g_to_title = 3, g_to_title_phase_ms = NowMs();
        }
        std::vector<std::string> lines;
        Page::Do d;
        std::string why;
        bool was = false, now_shown = false;
        bool early_live = false;   // our page back at the strip's update: its rows' words set there too
        int focused = -1;
        int minus_words = -1, minus_tab = -1, minus_decide = -1;   // (the - menu's page: after the lock)
        std::uint64_t gen = 0;
        const bool calls = ctx != nullptr && CanCall();
        const std::uint64_t update_t0 = NowMs();   // (trace: a slow update of ours is logged, below)
        std::uint64_t locked_ms = 0;
        {
            std::lock_guard lock(g_mutex);
            lines = g_probe.Seen(f, NowMs());
            was = g_page.Shown();
            // This opening's panes first, so the page is usable only with the panes of the screens updating now (an
            // undrawn page never takes System's A: MousePage -2).
            gen = g_page.Track(f);
            if (f.kind == Kind::kStrip) {   // (the - menu's tabs of ours, 0.11.9: the pack's in this opening)
                LookStrip(ctx, calls, f, gen);
                static const bool minus_tabs = [] {   // (on: the one home of co-op's pages; COOP_TEST_MINUS_TABS=0 off)
                    const char* v = std::getenv("COOP_TEST_MINUS_TABS");
                    return v == nullptr || v[0] != '0';
                }();
                g_page.SetMinusTabsUsable(minus_tabs && g_minus_ok && g_minus_page != 0 && CanCall());
            }
            // (a view of the - menu's page: kept while its tab shows; another tab, or the menu closed: its rows again)
            if (g_page.MinusTabChanged() || (!g_page.Shown() && g_page.MinusTab() < 0)) SetView(View::kMain);
            // (B is ours in a view of the Multiplayer page, and in the Settings page's list)
            g_page.SetSubView(g_page.Which() == 1 ? g_set_row.load() >= 0 : ViewShown() != View::kMain);
            if (f.kind == Kind::kPages) LookPages(ctx, calls, f, gen);
            if (f.kind != Kind::kBlocker) g_page.SetUsable(CanCall() && g_pages_ok && g_settings_button != 0);
            if (f.kind != Kind::kBlocker) {   // (Longshot Settings: its page, System's button and Longshot's settings)
                g_page.SetSettingsUsable(g_page.Usable() && g_settings_pane != 0 && g_settings_button != 0 &&
                                         CoopLongshotSettings::Available());
            }
            d = g_page.OnScreen(f, NowMs());
            now_shown = g_page.Shown();
            why = g_page.TakeWhy();
            g_mouse = g_page.MousePage();
            if (f.kind == Kind::kPages && now_shown) focused = FocusedRow();
            if (d.any() && calls) {
                if (f.kind == Kind::kPages) ApplyPages(ctx, f, d);
                if (f.kind == Kind::kStrip && d.set_page && g_pages_screen != 0 && g_pages_ok &&
                    Read<std::uint64_t>(g_pages_screen + kScreenLayout) == g_pages_layout) {
                    early_live = d.page_shown;
                    Facts pages;
                    pages.kind = Kind::kPages, pages.screen = g_pages_screen, pages.layout = g_pages_layout;
                    Page::Do page_only;
                    page_only.set_page = true, page_only.page_shown = d.page_shown, page_only.slide = d.slide;
                    page_only.page_from = d.page_from, page_only.page_to = d.page_to;
                    ApplyPages(ctx, pages, page_only);   // (the pages' own update comes a frame later)
                }
            }
            if (f.kind == Kind::kStrip && calls) {   // (the - menu's tabs of ours: set, kept after Map, their sound)
                if (d.set_minus) ApplyMinus(ctx, f, d);
                FollowMinus(ctx);
                minus_words = ApplyMinusPage(ctx, gen);   // (its own page: shown, rows, cursor)
                minus_tab = g_page.MinusTab(), minus_decide = g_page.TakeMinusDecide();
                if (const char* sound = g_page.TakeStripSound()) PlaySound(ctx, screen, sound);
            }
            if (f.kind == Kind::kPages && calls) StepSlide(ctx, NowMs());   // (each update while the pages slide)
            if (f.kind == Kind::kPages && calls && now_shown && g_slide.dir == 0) KeepPageShown(ctx, g_page.Which());
            if (f.kind == Kind::kPages && calls) SystemButtons(ctx);
            if (f.kind == Kind::kPages && calls) {
                MenuModel(ctx, f, now_shown && g_page.Which() == 0 && ViewShown() == View::kMenu &&
                                      g_menu_tab.load() == 0 && g_slide.dir == 0 && f.state == 2 && f.page == kSystemPage);
            }
            // (the menu sounds of the presses the module took: the game starts none for them)
            if (ctx != nullptr && CanCall() && f.kind == Kind::kPages) {
                if (const char* sound = g_page.TakePagesSound()) PlaySound(ctx, screen, sound);
            }
        }
        locked_ms = NowMs() - update_t0;
        for (const std::string& line : lines) LOG_INFO(Core, "{}", line);
        if (was && !now_shown) LOG_INFO(Core, "coop: pause tab: System back ({})", why);
        if (f.kind == Kind::kStrip && calls) {
            MinusWords(ctx, f.layout, gen, minus_words);
            if (minus_decide >= 0) MinusDecide(minus_tab, minus_decide);
            // (the games on this network and the relay's rooms heard while the - page's Find a game shows)
            const bool finding = minus_tab >= 0 && minus_tab != static_cast<int>(MinusPage::kMods) && ViewShown() == View::kLan;
            CoopLanNet::Look(finding, NowMs());
            if (!(finding && g_online.load())) CoopRoomList::Look(CoopRoomList::Viewer::kFindAGame, false, NowMs(), {});
        }
        if (f.kind == Kind::kPages && now_shown && f.state <= 2 && calls) {
            if (!g_confirm_by_mouse.load() && !KeepConfirm(true, focused, g_confirm_row.load())) g_confirm_until = 0;   // (the cursor left that row)
            FollowSays();   // (a Host or Join refused: why, on the Session view)
            // (after our lock: the status and the Lobby take their own; read only when the rows are due)
            if (LiveDue(gen)) LiveRows(ctx, f.layout, gen, ViewNow(ViewShown()));
        }
        // (the games on this network heard while their view shows; the listener closes a while after)
        if (f.kind == Kind::kPages) {
            CoopLanNet::Look(now_shown && ViewShown() == View::kLan, NowMs());
            // (the relay's list left, the page gone or the Local tab: its asks stop; ViewNow asks while it shows)
            if (!(now_shown && ViewShown() == View::kLan && g_online.load())) {
                CoopRoomList::Look(CoopRoomList::Viewer::kFindAGame, false, NowMs(), {});
            }
        }
        if (early_live && calls && LiveDue(g_pages_gen)) LiveRows(ctx, g_pages_layout, g_pages_gen, ViewNow(ViewShown()));
        if (!now_shown) g_confirm_until = 0;
        // (trace) A tester saw the screen stop for a moment as the Multiplayer page opened: an update of ours that
        // took long is logged, with the part under our lock (the page's calls) and the rest (the rows' words).
        if (const std::uint64_t took = NowMs() - update_t0; took >= 25 && g_slow_logged < 20) {
            ++g_slow_logged;
            LOG_INFO(Core, "coop: pause tab: an update ({}) took {} ms: {} ms the page's calls, {} ms the rows' words",
                     f.kind == Kind::kPages ? "pages" : f.kind == Kind::kStrip ? "tab strip" : "other", took, locked_ms,
                     took - locked_ms);
        }
    } catch (...) {
        // (a log line or a look lost is all: the game's update goes on)
    }
}

// One pad read of the way back to the title (ToTitle): the buttons the game gets (the player's wait), or kNoStep when
// it is over. g_mutex.
constexpr std::uint64_t kNoStep = ~0ull;
// (our page off: System's cursor comes back onto its Multiplayer button a few reads later. The focus read before that is
// the page's row, and an A then lands on our button, which leaves System dead to A and B: a guest's way back after a
// join in a session gave up so, a test run 2026-11-20. So a wait after the page goes, and Title Screen seen twice.)
constexpr std::uint64_t kToTitleSettleMs = 450;
constexpr std::uint64_t kToTitleSteadyMs = 150;
std::uint64_t g_to_title_left_ms = 0, g_to_title_on04_ms = 0;
std::uint64_t ToTitleStep(int phase) {
    const std::uint64_t now = NowMs();
    if (now - g_to_title_ms.load() > kToTitleMs || (phase == 3 && now - g_to_title_phase_ms.load() > 3000)) {
        if (phase != 3) LOG_INFO(Core, "coop: pause tab: back to the title: given up (the menu did not get there)");
        g_to_title = 0, g_tap = 0;
        return kNoStep;
    }
    if (phase != 3 && CoopTitle::Up()) {   // (the title already: nothing to go back from, no + tapped over it)
        LOG_INFO(Core, "coop: pause tab: back to the title: the title is up already");
        g_to_title = 0, g_tap = 0;
        return kNoStep;
    }
    if (now < g_tap_until) return g_tap;   // (a tap held)
    g_tap = 0;
    if (phase == 3 || now < g_tap_next) return 0;   // (let go between taps; answered: the game's own way to the title)
    const auto tap = [&](std::uint64_t b, std::uint64_t hold, std::uint64_t gap) {
        g_tap = b, g_tap_until = now + hold, g_tap_next = now + hold + gap;
        return b;
    };
    const bool seen = now - g_pages_seen_ms.load() < 400, open = seen && g_pages_seen_state.load() == 2;
    if (phase == 2) {   // (Title Screen pressed: the question comes; none after 3 s, again from the start)
        if (now - g_to_title_phase_ms.load() > 3000) g_to_title = 1, g_tap_downs = 0;
        return 0;
    }
    if (!seen) return tap(kPadPlus, 80, 1500);   // (the pause menu, shut: opened)
    if (!open) return 0;                          // (opening or closing: its own pace)
    if (g_pages_seen_page.load() != kSystemPage) return tap(kPadR, 80, 500);   // (on to System)
    if (g_page.Shown()) {
        g_page.Leave("back to the title");   // (our page off: System's own buttons)
        g_to_title_left_ms = now, g_to_title_on04_ms = 0;
        return 0;
    }
    if (now - g_to_title_left_ms < kToTitleSettleMs) return 0;   // (System's cursor back first)
    if (FocusedShownName() != "Pa_Button_04") g_to_title_on04_ms = 0;
    else if (g_to_title_on04_ms == 0) g_to_title_on04_ms = now;
    if (g_to_title_on04_ms != 0 && now - g_to_title_on04_ms < kToTitleSteadyMs) return 0;   // (seen again first)
    if (g_to_title_on04_ms != 0) {   // (System's Title Screen)
        g_to_title_on04_ms = 0;
        LOG_INFO(Core, "coop: pause tab: back to the title: A on System's Title Screen");
        g_to_title = 2, g_to_title_phase_ms = now;
        return tap(kPadA, 80, 300);
    }
    if (++g_tap_downs > 12) {
        LOG_INFO(Core, "coop: pause tab: back to the title: the cursor never reached Title Screen (on {})", FocusedShownName());
        g_to_title = 0;
        return kNoStep;
    }
    return tap(kPadDown, 80, 250);
}

std::uint64_t OnPad(std::uint64_t buttons, std::int32_t stick[4]) noexcept {
    try {
        std::uint64_t in = buttons;
        if (stick != nullptr) {   // (the left stick as up and down for our rows; nn::hid: y up)
            if (stick[1] > 20000) in |= kPadStickUp;
            if (stick[1] < -20000) in |= kPadStickDown;
            if (stick[0] > 20000) in |= kPadStickRight;
            if (stick[0] < -20000) in |= kPadStickLeft;
        }
        if (std::uint64_t snail = 0; SnailPad(in, &snail)) return snail;   // (Character Select's Customize screen)
        bool typing = false;
        std::uint64_t out = 0;
        bool was = false, shown = false;
        bool decide = false;
        int row = -1, pick_cell = -1, set_change = -1, set_dir = 0, list_line = -1, line_clicked = -1;
        bool settings_shown = false;
        std::string why, new_password, new_relay;
        bool row_clicked = false;
        int new_port = 0;
        bool relay_typed = false, room_name_typed = false;
        bool browse_refresh = false;   // (Y, or a click on the browser heading's Refresh box: after the lock, as Act)
        std::string new_room_name;
        // Settings and look: co-op's menu as it shows now (its page open for the view), and what the pad and the mouse
        // do to it (sent after our lock: co-op's menu takes its own)
        struct MenuEvent {
            int event, arg, arg2;
        };
        std::vector<MenuEvent> menu_events;
        std::vector<std::uint32_t> menu_keys, menu_chars;
        bool menu_back = false;
        // The - menu's page (0.11.9): its tab, and what its pad did (Page's Take*), done after our lock
        int minus_tab = -1, minus_line = -1;
        std::uint32_t minus_moves = 0;
        bool minus_back = false, minus_list_back = false;
        CoopPauseMenu::View mv;
        if (g_menu_open.load() >= 0) mv = CoopPause::NativeView();
        // (Customize Link: a shield's part picked, Link turns his back, where the shield is)
        g_model_back = g_menu_open.load() == 0 && mv.selected >= 0 &&
                       MenuHeadingOf(mv, mv.selected).find("Shield") != std::string::npos;
        if (const int phase = g_to_title.load(); phase != 0) {
            std::uint64_t steps = kNoStep;
            {
                std::lock_guard lock(g_mutex);
                steps = ToTitleStep(phase);
            }
            if (steps != kNoStep) {
                FollowMenu(false);   // (Settings and look shut on the way: co-op's menu's page with it)
                return steps;
            }
        }
        {   // (the mouse's left button: when it last went down, for a click's A just after a view came)
            float cx = 0, cy = 0;
            bool cl = false;
            const bool over = ReadCursor(&cx, &cy, &cl);
            const bool held = over && cl;
            if (held && !g_click_held) g_click_down_ms = NowMs();
            g_click_held = held;
            const std::uint64_t now = NowMs();
            // (the click that brought the view, or a double click's second press: down before the view came or
            // within 250 ms after (a test run clicked a row 290 ms after the page came: its own); a later click is its own)
            const std::uint64_t view = g_view_ms.load(), cd = g_click_down_ms.load();
            if (now - view < kClickThroughMs && now - cd < 350 && (cd < view || cd - view < 250) && (in & kPadA) != 0) {
                in &= ~kPadA;   // (the click that brought this view: not this view's)
                static int logged = 0;
                if (++logged <= 30) {
                    LOG_INFO(Core, "coop: pause tab: a click's A {} ms after the view came: not the view's",
                             now - g_view_ms.load());
                }
            }
        }
        {
            std::lock_guard lock(g_mutex);
            was = g_page.Shown();
            const int typing_was = g_page.Typing();
            const std::uint64_t field_was = TypingHash();
            // (A on System's Longshot Settings button opens its page: the button's name read only while A is held)
            {   // (which of System's buttons of ours the game's cursor is on: their names, read only while A is held)
                int on = -1;
                // (only while the pause menu's pages update: closed, System's layout keeps its last focus, and every A
                // in the game read as one on our button and was kept from it: nobody could roll or talk, four
                // testers 2026-11-20)
                if ((in & kPadA) != 0 && !g_page.Shown() && NowMs() - g_pages_seen_ms.load() < 500) {
                    on = OurSystemButtonFocused();
                }
                g_page.SetSystemButton(on);
            }
            if (g_page.Typing() >= 0) {
                const std::uint64_t now = NowMs();
                float x = 0, y = 0;
                bool left = false;
                if (ReadCursor(&x, &y, &left)) {
                    const int cell = CellAt(ReadCellBoxes(), x, y);   // (the list's row under the header: a key)
                    const int key = cell >= 0 && cell < kTypeKeys ? cell : -1;
                    if (left && !g_kb_held) g_kb_press = key;
                    if (!left && g_kb_held && key >= 0 && g_kb_press == key) {
                        g_page.PressKey(key);
                        g_kb_click_ms = now;
                    }
                    g_kb_held = left;
                }
                if (g_kb_held || now - g_kb_click_ms < 400) in &= ~kPadA;
            }
            // (typing just ended, by Enter, OK or +: an A a moment after is that same press again, as the game saw it
            // (Enter is A to it), not a new one: it opened the keyboard again on the search, a test run 2026-11-20)
            {
                const bool typing_now = g_page.Typing() >= 0;
                if (g_typing_seen && !typing_now) g_typing_end_ms = NowMs();
#ifdef _WIN32
                // (the PC key that ended it still held: the moment lasts until it is let go)
                if (!typing_now && NowMs() - g_key_end_ms.load() < 400 &&
                    ((GetAsyncKeyState(VK_RETURN) | GetAsyncKeyState(VK_ESCAPE)) & 0x8000) != 0) {
                    g_key_end_ms = NowMs();
                }
#endif
                g_typing_seen = typing_now;
                if (!typing_now && NowMs() - g_typing_end_ms < 300) in &= ~kPadA;
                // (ended by the PC's Enter or Esc: the game reads them as + and B too, and they came after the keyboard
                // shut: + shut the menu, a test run 2026-11-20. Not after the pad's B: its second B goes back a view)
                if (!typing_now && NowMs() - g_key_end_ms.load() < 400) in &= ~(kPadA | kPadB | kPadPlus);
            }
            // (a click on a setting is ours, made at its release: Longshot's A for the same click, when its mouse took
            // the row for one of the game's buttons, would change it again. A test run: Auto to Off, then back to Auto)
            if (g_page.Which() == 1 && g_page.Shown()) {
                const std::uint64_t now = NowMs();
                float mx = 0, my = 0;
                bool mheld = false;
                if (ReadCursor(&mx, &my, &mheld) && mheld) {   // (held on the list now: the A Longshot taps at the press)
                    for (const RowBox& b : ReadSettingBoxes()) {
                        if (b.shown && mx >= b.x0 && mx <= b.x1 && my >= b.y0 && my <= b.y1) {
                            g_set_click_ms = now;
                            break;
                        }
                    }
                }
                if (now - g_set_click_ms < 400) in &= ~kPadA;
            }
            // B with the game's cursor on the list on the right (the details, or the browser's games list without a
            // picked cell): back to the rows on the left, not out of the page (a tester: "B doesn't kick you back over
            // to the left menu"). The press is ours: a Left for the game's cursor instead.
            bool b_to_rows = false;
            if ((in & kPadB) != 0 && g_page.Shown() && g_page.Which() == 0 && g_page.Typing() < 0 && g_cell.load() < 0 &&
                ViewShown() != View::kMenu && FocusedShownName().rfind("Pa_List_", 0) == 0) {
                b_to_rows = (g_b_list_last & kPadB) == 0;   // (its press; held, nothing more)
                g_b_swallow = true;
            }
            if (g_b_swallow) {   // (held on after the cursor went left: still not the page's B, until let go)
                if ((buttons & kPadB) == 0) g_b_swallow = false;
                in &= ~kPadB;
            }
            g_b_list_last = buttons;
            // Settings and look: co-op's menu types into its row (A on Your name, Port, Relay...): the on-screen
            // keyboard, as the page's own fields have it (the pad's arrows over its keys, A presses one, B is Cancel, Y
            // deletes, + is OK; the mouse presses a key it is let go on). The PC's keys go to co-op's menu itself
            // (CoopPause::Key, Char). Nothing reaches the page or the game meanwhile.
            {
                const bool menu_typing = g_page.Shown() && g_page.Typing() < 0 && ViewShown() == View::kMenu && mv.typing;
                const std::uint64_t now = NowMs();
                if (g_menu_typed_last && !menu_typing) {
                    // (the press that ended it, still held now (+, B, A on OK, Enter): not the page's or the game's.
                    // The masks above start at the next read; this one too, or + shut the pause menu, a test run)
                    g_key_end_ms = now, g_typing_end_ms = now;
                    in &= ~(kPadA | kPadB | kPadPlus);
                }
                if (!g_menu_typed_last && menu_typing) g_menu_key = 0, g_menu_letter = 0;
                g_menu_typed_last = menu_typing;
                const std::uint64_t mdown = in & ~g_menu_pad_last;
                g_menu_pad_last = in;
                if (menu_typing) {
                    const std::size_t nl = kMenuLetters.size();
                    const auto press = [&](int key) {
                        g_menu_key = key;
                        switch (static_cast<TypeKey>(key)) {
                        case TypeKey::kLetter: menu_chars.push_back(static_cast<unsigned char>(kMenuLetters[g_menu_letter % nl])); break;
                        case TypeKey::kDelete: menu_keys.push_back(0x08); break;
                        case TypeKey::kCancel: menu_keys.push_back(0x1B); break;
                        case TypeKey::kOk: menu_keys.push_back(0x0D); break;
                        }
                        g_page.Sound("button_decide");
                    };
                    float x = 0, y = 0;
                    bool left = false;
                    if (ReadCursor(&x, &y, &left)) {   // (a click on a key's row presses it, as on the page's fields)
                        const int cell = CellAt(ReadCellBoxes(), x, y);
                        const int key = cell >= 0 && cell < kTypeKeys ? cell : -1;
                        if (left && !g_kb_held) g_kb_press = key;
                        if (!left && g_kb_held && key >= 0 && g_kb_press == key) press(key), g_kb_click_ms = now;
                        g_kb_held = left;
                    }
                    std::uint64_t d = mdown;
                    if (g_kb_held || now - g_kb_click_ms < 400) d &= ~kPadA;
                    // (up and down: the key, round the four; Left and Right: Letter's letter, round kMenuLetters)
                    const int dr = (d & (kPadDown | kPadStickDown)) != 0 ? 1 : (d & (kPadUp | kPadStickUp)) != 0 ? -1 : 0;
                    const int dc = (d & (kPadRight | kPadStickRight)) != 0 ? 1 : (d & (kPadLeft | kPadStickLeft)) != 0 ? -1 : 0;
                    if (dr != 0) g_menu_key = (g_menu_key + dr + kTypeKeys) % kTypeKeys, g_page.Sound("button_on");
                    if (dc != 0 && g_menu_key == static_cast<int>(TypeKey::kLetter)) {
                        g_menu_letter = (g_menu_letter % nl + nl + static_cast<std::size_t>(dc + static_cast<int>(nl))) % nl;
                        g_page.Sound("button_on");
                    }
                    if ((d & kPadA) != 0) press(g_menu_key);
                    if ((d & kPadB) != 0) menu_keys.push_back(0x1B);
                    if ((d & kPadY) != 0) menu_keys.push_back(0x08);
                    if ((d & kPadPlus) != 0) menu_keys.push_back(0x0D);
                    const bool sel_ok = mv.selected >= 0 && static_cast<std::size_t>(mv.selected) < mv.rows.size();
                    std::vector<ListLine> lines(kListRows);
                    lines[0] = {sel_ok ? mv.rows[static_cast<std::size_t>(mv.selected)].name : std::string("Type"),
                                sel_ok ? mv.rows[static_cast<std::size_t>(mv.selected)].value : std::string()};
                    lines[1] = {"Letter", std::string(1, kMenuLetters[g_menu_letter % nl] == ' ' ? '_' : kMenuLetters[g_menu_letter % nl])};
                    lines[1].arrows = true;
                    lines[2] = {"Delete", ""};
                    lines[3] = {"OK", ""};
                    lines[4] = {"Cancel", ""};
                    const auto same = [](const std::vector<ListLine>& a, const std::vector<ListLine>& b) {
                        if (a.size() != b.size()) return false;
                        for (std::size_t i = 0; i < a.size(); ++i) {
                            if (a[i].name != b[i].name || a[i].value != b[i].value || a[i].arrows != b[i].arrows) return false;
                        }
                        return true;
                    };
                    if (!same(lines, g_menu_typing_lines) || dr != 0) g_live_poke = true;
                    g_menu_typing_lines = std::move(lines);
                    in = 0;   // (the page sees no press: B is typing's Cancel, not the view's Back)
                    // (the list's own view of the mouse kept current meanwhile: the keys are its lines, and the last
                    // key clicked (OK) read as a move onto the line under it once typing ended, picking that part, so
                    // the next A opened its mixer: a test run, 2026-11-20)
                    g_cells_mouse_x = x, g_cells_mouse_y = y, g_cells_held = left;
                    g_cells_press = -1;
                } else if (!g_menu_typing_lines.empty()) {
                    g_menu_typing_lines.clear();
                    g_live_poke = true;
                }
            }
            out = g_page.OnPad(in, NowMs());
            minus_tab = g_page.MinusTab();
            if (minus_tab >= 0) {
                minus_back = g_page.TakeMinusBack(), minus_list_back = g_page.TakeMinusListBack();
                minus_line = g_page.TakeMinusLineDecide(), minus_moves = g_page.TakeMinusMoves();
            }
            if (g_page.TakeCustomizeOpen()) {   // (the Customize Link button: Settings and look's Customize page)
                g_customize_direct = true;
                g_menu_tab = 0, g_menu_list = false, g_menu_top = 0, g_menu_focus_reset = true;
                SetView(View::kMenu);
                LOG_INFO(Core, "coop: pause tab: Customize Link from System: Settings and look's Customize page");
            }
            if (g_page.TakeRefusedA()) {
                LOG_INFO(Core, "coop: pause tab: an A on System's button of ours while the page cannot show (usable {}, "
                               "panes {}, Multiplayer button {:#x}, the menu seen {} ms ago in state {}): kept from the "
                               "game",
                         g_page.Usable(), g_pages_ok, g_mp_button, NowMs() - g_pages_seen_ms.load(),
                         g_pages_seen_state.load());
            }
            if (b_to_rows) out |= kPadLeft, g_page.Sound("button_cancel");
            typing = g_page.Typing() >= 0;
            if (const int typed = g_page.TakeTyped(); typed == 2) {
                new_password = g_page.Field(2).text();
                g_page.Field(2).Clear();
            } else if (typed == 3) {
                new_port = g_page.Field(3).PortValue();
                if (new_port == 0) new_port = -1;   // (typed, but no port: said so below)
            } else if (typed == 4) {
                new_relay = g_page.Field(4).text(), relay_typed = true;
            } else if (typed == 5) {   // (a search: the list from its first page)
                g_cell_page = 0, g_cell = -1, g_live_poke = true;
            } else if (typed == 6) {
                new_room_name = g_page.Field(6).text(), room_name_typed = true;
            }
            const std::uint64_t field_now = TypingHash();
            if (typing_was != g_page.Typing() || field_was != field_now) g_live_poke = true;
            shown = g_page.Shown();
            decide = g_page.TakeDecide();
            // The mouse over our rows (MouseRows): the game's cursor stepped to the row under it, and an A just after
            // a click (Longshot presses A for it) is the clicked row's, not the row the game's cursor is on.
            if (shown && !typing && !mv.typing) {   // (co-op's menu typing: the keyboard's)
                MouseRows::In mi;
                mi.now_ms = NowMs();
                mi.inside = ReadCursor(&mi.x, &mi.y, &mi.left);
                mi.pad = (in & (kPadUp | kPadDown | kPadStickUp | kPadStickDown)) != 0;
                mi.focused = FocusedRow();
                if (mi.inside) mi.boxes = ReadRowBoxes(mi.now_ms);
                std::uint64_t mouse = g_mouse_rows.Step(mi);
                // (the game's cursor on the list on the right, which Longshot's mouse put there: the mouse's steps to a
                // row on the left would walk the list instead, slowly to its top, a tester saw. A Left first, once
                // each 150 ms, takes the cursor back to the rows; the steps go on from there)
                if ((mouse & (kPadUp | kPadDown)) != 0 && FocusedShownName().rfind("Pa_List_", 0) == 0) {
                    mouse = mi.now_ms - g_list_left_ms >= 150 ? kPadLeft : 0;
                    if (mouse != 0) g_list_left_ms = mi.now_ms;
                }
                // (never past the first or last shown row: the game's cursor wraps from the top row to the bottom one,
                // so a step there went between Hosting and Stop hosting without end, a tester's run 2026-11-19)
                if ((mouse & (kPadUp | kPadDown)) != 0 && mi.focused >= 0) {
                    int first = -1, last = -1;
                    for (int r = 0; r < kRows; ++r) {
                        if (mi.boxes[static_cast<std::size_t>(r)].shown) last = r, first = first < 0 ? r : first;
                    }
                    const bool wraps = ((mouse & kPadUp) != 0 && mi.focused <= first) ||
                                       ((mouse & kPadDown) != 0 && last >= 0 && mi.focused >= last);
                    if (++g_mouse_steps_logged <= 40) {
                        LOG_INFO(Core, "coop: pause tab: the mouse steps {} from row {} (the cursor on {}; rows shown {}..{}){}",
                                 (mouse & kPadUp) != 0 ? "up" : "down", mi.focused, FocusedShownName(), first, last,
                                 wraps ? ": not past the end" : "");
                    }
                    if (wraps) mouse &= ~(kPadUp | kPadDown);
                }
                // The game's cursor off the Multiplayer page's shown rows (RowSkip): on its way past them, the mouse's
                // own steps wait (a moment's hiding and a slide leave it be: the cursor lands as they want).
                std::uint64_t skip = 0;
                if (!was) g_page_since_ms = mi.now_ms;
                // (the mouse over the rows' column but on no shown row, or just moved: Longshot's mouse puts the game's
                // cursor on the hidden button under it, and stepping it off there went on without end as the mouse
                // passed, a tester's "bounces between Hosting and Stop hosting", 2026-11-19. The cursor is left there
                // then; the next step of the pad or the mouse onto a row takes it on)
                bool mouse_over_gap = false;
                if (mi.inside) {
                    if (std::abs(mi.x - g_skip_mouse_x) > 2 || std::abs(mi.y - g_skip_mouse_y) > 2) {
                        g_skip_mouse_x = mi.x, g_skip_mouse_y = mi.y, g_skip_mouse_ms = mi.now_ms;
                    }
                    float x0 = 1e30f, x1 = -1e30f;
                    for (const RowBox& b : mi.boxes) {
                        if (b.shown) x0 = std::min(x0, b.x0), x1 = std::max(x1, b.x1);
                    }
                    mouse_over_gap = (mi.x >= x0 && mi.x <= x1 && RowAt(mi.boxes, mi.x, mi.y) < 0) ||
                                     mi.now_ms - g_skip_mouse_ms < 300;
                }
                // (not while the page opens: the cursor is still on System's button then, and an Up there went into
                // the opening's own landing, a run's log)
                if (g_page.Which() == 0 && !g_rows_moment.load() && g_slide.dir == 0 && g_rows_back_ms == 0 &&
                    mi.now_ms - g_page_since_ms > 500 && !mouse_over_gap) {
                    RowSkip::In si;
                    si.now_ms = mi.now_ms;
                    si.focused = FocusedButtonRow(&si.hidden);
                    si.up = (in & (kPadUp | kPadStickUp)) != 0, si.down = (in & (kPadDown | kPadStickDown)) != 0;
                    si.mouse_up = (mouse & kPadUp) != 0, si.mouse_down = (mouse & kPadDown) != 0;
                    skip = g_row_skip.Step(si);
                    if (skip != 0 && ++g_skip_logged <= 12) {
                        LOG_INFO(Core, "coop: pause tab: the game's cursor on button {} ({}): {} to the page's rows",
                                 si.focused, si.hidden ? "hidden" : "past our rows", (skip & kPadUp) != 0 ? "up" : "down");
                    }
                } else {
                    g_row_skip.Reset();
                }
                out |= g_row_skip.Skipping() ? skip : mouse | skip;
            } else {
                g_mouse_rows.Reset();
                g_row_skip.Reset();
            }
            // The game browser's cells (View::kLan): Right goes into them, the arrows move among them (none reaches the
            // game's cursor), Left from the left column or B gives the pad back to the rows; the mouse picks one by
            // moving onto it, and a click on one is its A (Longshot presses nothing there: the cells are no buttons).
            const bool settings = shown && !typing && g_page.Which() == 1;
            const bool browse = shown && !typing && !settings && ViewShown() == View::kLan;
            // (while typing, the mouse's presses are the keyboard's: one begun on the list before it opened (Longshot's
            // A opens it at the press) is forgotten, or its release after typing was a click again, a test run's search)
            if (typing) g_cells_held = false, g_cells_press = -1, g_lines_held = false, g_lines_press = -1;
            const std::uint64_t bdown = in & ~g_browse_last;
            g_browse_last = in;
            int cell = browse ? g_cell.load() : -1;
            const int games = g_cell_games.load();
            if (cell >= games) cell = games - 1;
            if (browse) {
                const bool right = (bdown & (kPadRight | kPadStickRight)) != 0, left = (bdown & (kPadLeft | kPadStickLeft)) != 0;
                const bool up = (bdown & (kPadUp | kPadStickUp)) != 0, down = (bdown & (kPadDown | kPadStickDown)) != 0;
                const int pages = CellPages(g_browse_listed.load());
                if ((bdown & kPadY) != 0) {
                    browse_refresh = true;   // (Y: the list asked again, as the heading's Refresh box)
                } else if (cell >= 0 && down && cell == games - 1 && pages > 1) {
                    g_cell_page = (g_cell_page.load() + 1) % pages, cell = 0;   // (past the page's last: the next page)
                    g_page.Sound("button_on"), g_live_poke = true;
                } else if (cell == 0 && up && g_cell_page.load() > 0) {
                    g_cell_page = g_cell_page.load() - 1, cell = kCells - 1;   // (above its first: the page before)
                    g_page.Sound("button_on"), g_live_poke = true;
                } else if (cell < 0 && right && games > 0) {
                    cell = 0;   // (the list's first game)
                    g_page.Sound("button_on");
                } else if (cell >= 0 && (right || left || up || down)) {
                    const int to = CellMove(cell, right ? 1 : left ? -1 : 0, down ? 1 : up ? -1 : 0, games);
                    if (to != cell) g_page.Sound("button_on");
                    cell = to;
                }
                float x = 0, y = 0;
                bool held = false;
                if (ReadCursor(&x, &y, &held)) {
                    const int hover = CellAt(ReadCellBoxes(), x, y);
                    const bool moved = std::abs(x - g_cells_mouse_x) > 2 || std::abs(y - g_cells_mouse_y) > 2;
                    g_cells_mouse_x = x, g_cells_mouse_y = y;
                    if (moved && hover >= 0 && hover < games && hover != cell) cell = hover, g_page.Sound("button_on");
                    const bool on_head = hover < 0 && LineAt(x, y) == 0;   // (the heading: its page and Refresh)
                    if (held && !g_cells_held) g_cells_press = on_head ? -2 : hover;
                    if (held && on_head) g_head_click_ms = NowMs();   // (Longshot's A for it is not the line's)
                    if (!held && g_cells_held && hover >= 0 && hover == g_cells_press && hover < games) {
                        pick_cell = hover;   // (a click on a cell: its A)
                        g_page.Sound("button_decide");
                    }
                    if (!held && g_cells_held && on_head && g_cells_press == -2) {
                        // (its box at the right: Refresh; its words: back a page on their left half, on on their right)
                        const RowBox head = HeadBox();
                        const float w = head.x1 - head.x0, box_x = head.x1 - w * 0.36f;
                        const int pages = CellPages(g_browse_listed.load());
                        if (x >= box_x) {
                            browse_refresh = true;
                        } else if (pages > 1) {
                            const int dir = x < (head.x0 + box_x) / 2 ? -1 : 1;
                            g_cell_page = ((g_cell_page.load() + dir) % pages + pages) % pages, cell = -1;
                            g_page.Sound("button_decide"), g_live_poke = true;
                        }
                    }
                    g_cells_held = held;
                }
                if (cell >= 0) out &= ~(kPadUp | kPadDown | kPadStickUp | kPadStickDown);   // (the rows' cursor stays)
            }
            if (cell != g_cell.load()) g_cell = cell, g_live_poke = true;
            // The details list on the right (the main and Session views: DetailLines): Longshot's mouse does not reach
            // its lines there (a click on Port did nothing, a test run 2026-11-20), so a click on a line with an action
            // (the port, the join password, a code to copy) is its A here: pressed and let go on the same line.
            if (shown && !typing && !settings && !browse && ViewShown() != View::kMenu) {
                float x = 0, y = 0;
                bool held = false;
                if (ReadCursor(&x, &y, &held)) {
                    const int on = LineAt(x, y);
                    if (held && !g_lines_held) g_lines_press = on;
                    if (!held && g_lines_held && on >= 0 && on == g_lines_press) line_clicked = on;
                    g_lines_held = held;
                }
            } else {
                g_lines_held = false, g_lines_press = -1;
            }
            // The Settings page: the rows choose the category; Right or A goes into its list; up and down move in it,
            // Left and Right change the value, A the next one, B back to the rows. The mouse picks a row and a click
            // on an arrow changes it (elsewhere on the row: A).
            int set_row = settings ? g_set_row.load() : -1;
            if (settings) {
                const bool right = (bdown & (kPadRight | kPadStickRight)) != 0, left = (bdown & (kPadLeft | kPadStickLeft)) != 0;
                const bool up = (bdown & (kPadUp | kPadStickUp)) != 0, down = (bdown & (kPadDown | kPadStickDown)) != 0;
                const int count = g_set_count.load();
                const std::uint64_t now = NowMs();
                TraceFocus("settings");
                // The rows' cursor moved (the pad's up and down while the rows have it, or the mouse's steps): the
                // category is its row's, and the rows have the pad.
                if (g_set_seen_gen != g_pages_gen) g_set_seen_gen = g_pages_gen, g_set_seen_row = -1;
                std::uint64_t keep_step = 0;   // (a step back to the kept category, past the list's mask below)
                // (the game's cursor on the list: the category kept from now, before the rows' cursor reads as the row
                // beside it, as it does in the same frame)
                const bool on_list_now = FocusedShownName().rfind("Pa_List_", 0) == 0;
                // (and the mouse on the list, moving or held: Longshot's mouse takes the game's cursor there, on
                // another thread, so the frame it reads as the row beside it may come first)
                bool mouse_on_list = false;
                {
                    float mx = 0, my = 0;
                    bool mheld = false;
                    if (ReadCursor(&mx, &my, &mheld)) {
                        if (std::abs(mx - g_set_mouse_x) > 2 || std::abs(my - g_set_mouse_y) > 2) g_set_mouse_ms = now;
                        g_set_mouse_x = mx, g_set_mouse_y = my;
                        if (mheld || now - g_set_mouse_ms < 1000) {
                            for (const RowBox& b : ReadSettingBoxes()) {
                                if (b.shown && mx >= b.x0 && mx <= b.x1 && my >= b.y0 && my <= b.y1) mouse_on_list = true;
                            }
                        }
                        // (the mouse onto the categories: its hover picks one, as ever; nothing kept)
                        if (!mouse_on_list && RowAt(ReadRowBoxes(now), mx, my) >= 0) g_set_keep = -1;
                    }
                }
                if (on_list_now || mouse_on_list) {
                    if (g_set_keep < 0) g_set_keep = g_set_category.load();
                    g_set_keep_until = now + 1500;
                }
                if (const int focused = FocusedRow(); focused >= 0 && g_set_keep >= 0 && now < g_set_keep_until) {
                    if (focused == g_set_keep && !on_list_now) {
                        g_set_keep = -1, g_set_seen_row = focused;   // (back: the category as it was)
                    } else if (!on_list_now && now - g_set_keep_step_ms >= 120) {
                        g_set_keep_step_ms = now;
                        keep_step = focused > g_set_keep ? kPadUp : kPadDown;
                    }
                } else if (const int focused = FocusedRow(); focused >= 0 && focused != g_set_seen_row) {
                    g_set_keep = -1;
                    g_set_seen_row = focused;
                    if (focused != g_set_category.load()) {
                        g_set_category = focused, g_set_page = 0, g_live_poke = true;
                        LOG_INFO(Core, "coop: pause tab: settings: the category is {} (the rows' cursor on it)",
                                 kSettingTabs[static_cast<std::size_t>(focused)]);
                    }
                    if (set_row >= 0 && now - g_set_enter_ms < 400) {
                        // (the cursor's step of a Down just before the Right that entered the list, read only now: the
                        // new category's list, entered as asked; a fast pad's Right was lost, a test run 2026-11-20)
                        set_row = 0, g_set_page = 0;
                    } else if (set_row >= 0) {
                        set_row = -1;
                        LOG_INFO(Core, "coop: pause tab: settings: the rows have the pad again (their cursor moved)");
                    }
                }
                // A click on a category (Longshot presses A for it) only picks it: it never enters the list, and never
                // reaches a setting there (it changed the one picked before: Volume from 100% to 0%). The click is
                // MouseRows' (pressed and let go on a row), or the left button down in the last kMouseAMs with the
                // mouse on a category (Longshot's A can come before the button is let go).
                {
                    float mx = 0, my = 0;
                    bool mheld = false;
                    const bool inside = ReadCursor(&mx, &my, &mheld);
                    if (inside && mheld) g_set_left_ms = now;
                    const int mouse_row = inside && now - g_set_left_ms < kMouseAMs ? RowAt(ReadRowBoxes(now), mx, my) : -1;
                    const int clicked = g_mouse_rows.Clicked(now);
                    if (decide && (clicked >= 0 || mouse_row >= 0)) {
                        g_mouse_rows.TakeClicked(now);
                        decide = false;
                        if (set_row >= 0) set_row = -1;
                        const int row = clicked >= 0 ? clicked : mouse_row;
                        LOG_INFO(Core, "coop: pause tab: settings: a click on the category {} ({}): it is picked, the list "
                                       "not entered",
                                 kSettingTabs[static_cast<std::size_t>(row)], clicked >= 0 ? "let go on it" : "held on it");
                    } else if (decide) {
                        LOG_INFO(Core, "coop: pause tab: settings: A (the pad's: the mouse {})",
                                 inside ? "not on a category just now" : "not over the game");
                    }
                }
                if (set_row < 0) {
                    if ((right || decide) && count > 0) {
                        set_row = 0, g_set_page = 0, decide = false, g_set_enter_ms = now;
                        g_page.Sound("button_on");
                    }
                } else {
                    if (up || down) {
                        int page = g_set_page.load(), r = set_row;
                        SettingMove(count, down ? 1 : -1, &page, &r, g_set_per_page.load());
                        if (page != g_set_page.load() || r != set_row) g_page.Sound("button_on");
                        g_set_page = page, set_row = r;
                    }
                    if (left || right) set_change = g_set_page.load() * g_set_per_page.load() + set_row, set_dir = left ? -1 : 1;
                    if (decide) set_change = g_set_page.load() * g_set_per_page.load() + set_row, set_dir = 0, decide = false;
                }
                float x = 0, y = 0;
                bool held = false;
                if (ReadCursor(&x, &y, &held)) {
                    const auto boxes = ReadSettingBoxes();
                    int hover = -1;   // (the setting under the mouse, as set_row counts: the header's row is none)
                    for (int i = 1; i < kSettingListRows; ++i) {
                        const RowBox& b = boxes[static_cast<std::size_t>(i)];
                        if (b.shown && x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) hover = i - 1;
                    }
                    const bool moved = std::abs(x - g_cells_mouse_x) > 2 || std::abs(y - g_cells_mouse_y) > 2;
                    g_cells_mouse_x = x, g_cells_mouse_y = y;
                    if (moved && hover >= 0 && hover != set_row) set_row = hover, g_page.Sound("button_on");
                    // The mouse onto the categories while the list has the pad: the rows have it again, so the mouse's
                    // steps move their cursor (and the category with it).
                    if (moved && hover < 0 && set_row >= 0 && RowAt(ReadRowBoxes(now), x, y) >= 0) {
                        set_row = -1;
                        LOG_INFO(Core, "coop: pause tab: settings: the mouse went onto the categories: the rows have the pad");
                    }
                    if (held && !g_cells_held) g_cells_press = hover;
                    if (held && (hover >= 0 || g_cells_press == -2)) g_set_click_ms = now;   // (held on the list: Longshot's A is not one)
                    // (the header, "Motion  < 1 of 4 >": a click on its left half turns back a page, on its right half
                    // on, as a tester asked for the mouse; the keyboard's and the pad's ways stay)
                    const RowBox& head = boxes[0];
                    const bool on_head = head.shown && x >= head.x0 && x <= head.x1 && y >= head.y0 && y <= head.y1;
                    if (held && !g_cells_held && on_head) g_cells_press = -2;
                    if (!held && g_cells_held && on_head && g_cells_press == -2) {
                        const int pages = SettingPages(g_set_count.load(), g_set_per_page.load());
                        if (pages > 1) {
                            const int dir = x < (head.x0 + head.x1) / 2 ? -1 : 1;
                            g_set_page = ((g_set_page.load() + dir) % pages + pages) % pages;
                            set_row = -1, g_live_poke = true;
                            g_page.Sound("button_decide");
                            LOG_INFO(Core, "coop: pause tab: settings: the header clicked: page {} of {}", g_set_page.load() + 1, pages);
                        }
                    }
                    if (!held && g_cells_held && hover >= 0 && hover == g_cells_press) {
                        const RowBox& b = boxes[static_cast<std::size_t>(hover) + 1];
                        const float cx = (b.x0 + b.x1) / 2, scale = (b.x1 - b.x0) / 1880.0f;
                        const float to_left = std::abs(x - (cx + kArrowLeftX * scale)), to_right = std::abs(x - (cx + kArrowRightX * scale));
                        set_row = hover;
                        g_set_click_ms = now;
                        set_change = g_set_page.load() * g_set_per_page.load() + hover;
                        set_dir = to_left < kArrowReach * scale ? -1 : to_right < kArrowReach * scale ? 1 : 0;
                    }
                    g_cells_held = held;
                }
                if (set_row >= 0) out &= ~(kPadUp | kPadDown | kPadStickUp | kPadStickDown);   // (the rows' cursor stays)
                // The game's cursor on a row of the list: the Options rows are buttons of the game's, and Longshot's
                // mouse puts its cursor on one it clicks. The categories' cursor then walked that column (a run: the
                // mouse's steps toward a category went down and up the list, the category never changed, B was needed
                // first). A Left sends it back to the categories, each 300 ms at most while it is there.
                out |= keep_step;
                if (const std::string on = FocusedShownName(); on.rfind("Pa_List_", 0) == 0 && now - g_list_focus_fix_ms > 300) {
                    g_list_focus_fix_ms = now;
                    out |= kPadLeft;
                    if (g_set_keep < 0) g_set_keep = g_set_category.load();
                    g_set_keep_until = now + 1500;
                    LOG_INFO(Core, "coop: pause tab: settings: the game's cursor on {} (the list): Left sends it back to "
                                   "the categories",
                             on);
                }
                g_page.SetSubView(set_row >= 0);   // (B: back to the rows)
            }
            if (set_row != g_set_row.load()) g_set_row = set_row, g_live_poke = true;
            // Settings and look (View::kMenu): the rows choose co-op's menu's page (the game's cursor on one shows it
            // on the list); Right or A goes into the list; there up and down move co-op's menu's selection, Left and
            // Right change a choice, A does the row; B gives the pad back to the rows (or closes a colour's mixer
            // first). The mouse picks a line by moving onto it; a click is its A (on a choice's arrow: that way); a
            // click on the header's left or right half shows the rows before or after.
            const bool menu = shown && !typing && !settings && ViewShown() == View::kMenu && !mv.typing;
            if (menu) {
                const bool right = (bdown & (kPadRight | kPadStickRight)) != 0, left = (bdown & (kPadLeft | kPadStickLeft)) != 0;
                const bool up = (bdown & (kPadUp | kPadStickUp)) != 0, down = (bdown & (kPadDown | kPadStickDown)) != 0;
                const std::uint64_t now = NowMs();
                bool list = g_menu_list.load();
                // (the page follows the game's cursor only once it moves to another row: where it was as the view
                // came (the view's own row: in a session row 4, Local co-op's) or as the list gave the pad back (the row
                // it was on all along) is no choice: a test run opened Co-op settings after a click on the header)
                const int focused = FocusedRow();
                if (g_menu_focus_reset.exchange(false)) g_menu_focus_seen = focused;
                if (!list && focused != g_menu_focus_seen) {
                    g_menu_focus_seen = focused;
                    if (focused >= 0 && focused < kMenuTabs && focused != g_menu_tab.load()) {
                        g_menu_tab = focused, g_menu_top = 0, g_live_poke = true;   // (FollowMenu opens its page)
                    }
                }
                if (!list) {
                    if (right) list = true, g_page.Sound("button_on");
                } else {
                    if (up) menu_events.push_back({HOS_COOP_MENU_UP, 0, 0});
                    if (down) menu_events.push_back({HOS_COOP_MENU_DOWN, 0, 0});
                    if (left) menu_events.push_back({HOS_COOP_MENU_LEFT, 0, 0});
                    if (right) menu_events.push_back({HOS_COOP_MENU_RIGHT, 0, 0});
                    if (up || down || left || right) g_page.Sound("button_on");
                }
                // The mouse. Longshot's mouse does not press A on the list's lines (it presses A only as a click takes
                // its focus onto a button), so a click on a line is the module's: pressed and let go on the same line.
                // The A Longshot does send for a click (the focus coming onto a line, or onto Link's side) is not the
                // pad's: kept from co-op's menu while the button is down and a moment after (a review: the same click
                // stepped a choice twice; a part's mixer opened under the click, a test run 2026-11-20).
                float x = 0, y = 0;
                bool held = false;
                if (ReadCursor(&x, &y, &held)) {
                    const int line = LineAt(x, y);   // (0 the header, 1.. the rows)
                    const int mrow = line >= 1 ? g_menu_line_row[static_cast<std::size_t>(line)].load() : -1;
                    const bool pick = line >= 1 && g_menu_line_pick[static_cast<std::size_t>(line)].load() && mrow >= 0;
                    const bool moved = std::abs(x - g_cells_mouse_x) > 2 || std::abs(y - g_cells_mouse_y) > 2;
                    g_cells_mouse_x = x, g_cells_mouse_y = y;
                    if (moved && pick && (!list || mrow != mv.selected)) {
                        menu_events.push_back({HOS_COOP_MENU_SELECT, mrow, 0});
                        list = true;
                        g_page.Sound("button_on");
                    }
                    if (moved && line < 0 && list && RowAt(ReadRowBoxes(now), x, y) >= 0) list = false;   // (onto the rows)
                    if (held && !g_cells_held) {
                        g_cells_press = line;
                        // (Customize Link: a drag that starts right of the list, on Link, turns him)
                        const RowBox head = HeadBox();
                        g_model_dragging = g_menu_tab.load() == 0 && head.shown && line < 0 && x > head.x1 + 40.0f;
                        g_model_drag_x = x;
                    }
                    if (held && (line >= 0 || g_model_dragging)) g_menu_click_ms = now;
                    if (held && g_model_dragging) {
                        g_model_drag = g_model_drag.load() + (x - g_model_drag_x) * kModelDragTurn;
                        g_model_drag_x = x;
                    }
                    if (!held) g_model_dragging = false;
                    if (!held && g_cells_held && line >= 0 && line == g_cells_press) {
                        g_menu_click_ms = now;
                        if (line == 0) {   // (the header: the rows before or after, a page's worth)
                            const RowBox head = HeadBox();
                            const int dir = x < (head.x0 + head.x1) / 2 ? -1 : 1;
                            g_menu_top = std::max(0, g_menu_top.load() + dir * g_menu_per_page.load());
                            list = false;
                            g_page.Sound("button_decide"), g_live_poke = true;
                        } else if (pick) {
                            const std::array<RowBox, kCells> boxes = ReadCellBoxes();
                            const RowBox b = line - 1 < kCells ? boxes[static_cast<std::size_t>(line - 1)] : LastLineBox();
                            const float cx = (b.x0 + b.x1) / 2, scale = (b.x1 - b.x0) / 1880.0f;
                            const float to_left = std::abs(x - (cx + kArrowLeftX * scale)), to_right = std::abs(x - (cx + kArrowRightX * scale));
                            const int side = to_left < kArrowReach * scale ? -1 : to_right < kArrowReach * scale ? 1 : 0;
                            if (side != 0) {
                                menu_events.push_back({HOS_COOP_MENU_CLICK, mrow, side});   // (an arrow: that way)
                            } else {   // (elsewhere on the row: as A on it: a part's mixer, a switch turned)
                                menu_events.push_back({HOS_COOP_MENU_SELECT, mrow, 0});
                                menu_events.push_back({HOS_COOP_MENU_CONFIRM, 0, 0});
                            }
                            list = true;
                            g_page.Sound("button_decide");
                        }
                    }
                    g_cells_held = held;
                }
                // (Longshot's A for a click on the list or on Link: not one. A click on the page's rows is Longshot's
                // own A, as ever)
                if ((held && (g_cells_press >= 0 || g_model_dragging)) || now - g_menu_click_ms < 400) decide = false;
                if (decide && list) {   // (the pad's A: the selected row's)
                    menu_events.push_back({HOS_COOP_MENU_CONFIRM, 0, 0});
                    g_page.Sound("button_decide");
                    decide = false;
                }
                if (list) {   // (the rows' cursor stays: left and right too, or they took it onto the list or back)
                    out &= ~(kPadUp | kPadDown | kPadStickUp | kPadStickDown | kPadLeft | kPadRight | kPadStickLeft | kPadStickRight);
                }
                // (the game's cursor on a line of the list, where Longshot's mouse put it: left there while the list
                // has the pad, its line the one picked. A Left each time sent it onto the page's row beside that line,
                // another page, so the mouse over the list bounced to the rows, a tester. Only the rows' own up and
                // down with the cursor still there take it back first)
                if (const std::string on = FocusedShownName(); !list && (up || down) && on.rfind("Pa_List_", 0) == 0 &&
                                                               now - g_list_focus_fix_ms > 300) {
                    g_list_focus_fix_ms = now;
                    out = (out & ~(kPadUp | kPadDown | kPadStickUp | kPadStickDown)) | kPadLeft;
                }
                if (list != g_menu_list.load()) {
                    if (!list) g_menu_focus_seen = FocusedRow();   // (the rows have the pad: their cursor where it is)
                    g_menu_list = list, g_live_poke = true;
                }
                if (!menu_events.empty()) g_live_poke = true;
            }
            // The Multiplayer page's details list (DetailLines, on the main and Session views) is the game's Options
            // rows, its buttons: Longshot's mouse puts the game's cursor on the one under it. Nothing there to pick: an
            // A there is no row's (the rows' last focused one would have been taken). The cursor is left there (a Left
            // to send it back bounced it each time the mouse passed over the list, a tester's run 2026-11-19); the
            // mouse onto the rows takes it back.
            bool on_list = false;
            if (shown && !typing && !settings) on_list = FocusedShownName().rfind("Pa_List_", 0) == 0;
            if (decide) {
                const int clicked = g_mouse_rows.TakeClicked(NowMs());
                if (clicked < 0 && cell >= 0) {
                    pick_cell = cell;   // (A on a cell)
                    decide = false;
                } else if (clicked < 0 && on_list) {
                    // (A on a line of the details: that line's own action, else nothing; Act reads which. The
                    // browser's heading clicked: ours (its page or Refresh), not Longshot's A for it)
                    const std::string name = FocusedShownName();
                    list_line = name.size() == 10 ? std::atoi(name.c_str() + 8) : -1;
                    decide = list_line >= 0 && !(list_line == 0 && NowMs() - g_head_click_ms.load() < 800);
                } else {
                    row = clicked >= 0 ? clicked : FocusedRow();   // (under the lock: the pages screen is the updates')
                    row_clicked = clicked >= 0;
                    if (clicked == MouseRows::kOffRows) decide = false;   // (a click off our rows: no row's A)
                    if (clicked >= 0 && cell >= 0) g_cell = -1, g_live_poke = true;   // (a row clicked: the rows' pad)
                }
            }
            if (settings && g_page.TakeBack()) {
                g_set_row = -1, g_live_poke = true;   // (B in the settings list: the rows have the pad again)
            } else if (g_page.TakeBack()) {
                if (cell >= 0) {
                    g_cell = -1, g_live_poke = true;   // (B in the cells: the rows have the pad again)
                } else if (ViewShown() == View::kMenu && g_menu_list.load()) {
                    menu_back = true;   // (co-op's menu's Back first: a colour's mixer shuts; else the rows have the pad)
                } else if (g_customize_direct.load() && ViewShown() == View::kMenu) {
                    g_page.LeaveToButton("B on Customize Link");   // (opened from its own button: System again)
                } else {
                    SetView(BackOf(ViewShown()));   // (B in a view: where its Back goes)
                }
            }
            if (!g_page.Shown()) g_customize_direct = false;
            if (!shown && minus_tab < 0) SetView(View::kMain);   // (a - page's view stays while its tab shows)
            if (!shown) g_set_row = -1;
            g_page.SetSubView(g_page.Which() == 1 ? g_set_row.load() >= 0 : ViewShown() != View::kMain);
            why = g_page.TakeWhy();
            g_mouse = g_page.MousePage();
            settings_shown = shown && g_page.Which() == 1;
        }
        if (stick != nullptr) g_model_spin = shown ? static_cast<float>(stick[2]) / 32767.0f : 0.0f;   // (Link's turn)
        if (shown && stick != nullptr) {
            stick[0] = 0, stick[2] = 0, stick[3] = 0;   // (only the left stick's up and down: the game's cursor over our rows)
            if (typing || g_cell.load() >= 0 || (g_page.Which() == 1 && g_set_row.load() >= 0) ||
                (ViewShown() == View::kMenu && (g_menu_list.load() || mv.typing))) {
                stick[1] = 0;
            }
        }
        if (!was && shown) {
            g_view_ms = NowMs();
            LOG_INFO(Core, "coop: pause tab: our page shown (A on its System button)");
        }
        if (was && !shown) {
            // (trace: what came with the press that left the page: a tester's page went back by itself as it opened)
            float mx = 0, my = 0;
            bool mleft = false;
            const bool over = ReadCursor(&mx, &my, &mleft);
#ifdef _WIN32
            const bool mright = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0, esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
#else
            const bool mright = false, esc = false;
#endif
            LOG_INFO(Core, "coop: pause tab: System back ({}); the pad {:#x}, the mouse {} (left {}, right {}), Esc {}, "
                           "{} ms after the page showed",
                     why, buttons, over ? fmt::format("at ({:.0f}, {:.0f})", mx, my) : std::string("off the game"),
                     mleft ? "down" : "up", mright ? "down" : "up", esc ? "down" : "up", NowMs() - g_page_since_ms);
        }
        if (new_port != 0) {   // (the port typed on the details' Port line: saved, and hosting again on it)
            const bool taken = new_port > 0 && CoopPause::SetHostPort(new_port);
            LOG_INFO(Core, "coop: pause tab: a port typed: {}", taken ? std::to_string(new_port) : std::string("not a port (1 to 65535)"));
            g_live_poke = true;
        }
        if (room_name_typed) {   // (the room name typed on the main view's details: saved as Co-op Settings' Room name)
            const bool taken = CoopPause::SetRoomName(new_room_name);
            LOG_INFO(Core, "coop: pause tab: a room name typed: {}", taken ? "saved" : "not a room name (3 to 24 letters, digits, - or _)");
            g_live_poke = true;
        }
        if (relay_typed) {   // (the relay typed on the online list: saved as Co-op Settings' Relay)
            const bool taken = CoopPause::SetRelay(new_relay);
            LOG_INFO(Core, "coop: pause tab: a relay typed: {}", taken ? "saved" : "not a relay address");
            g_live_poke = true;
        }
        if (!new_password.empty()) {   // (the host's new join password, typed on the keyboard: never in the log)
            const bool taken = CoopHookshot::HostMenu().SetPassword(new_password.c_str()) != 0;
            LOG_INFO(Core, "coop: pause tab: a new join password typed: {}", taken ? "handed to the host" : "not taken (see why)");
            if (taken) g_show_password = true;
            g_live_poke = true;
        }
        if (browse_refresh) {
            Act(-1, 0, true);   // (the heading's line: Refresh)
        }
        // (the - menu's page: B back a view, a line picked, co-op's menu's or the Mods tab's own moves)
        const bool minus_mods = minus_tab == static_cast<int>(MinusPage::kMods);
        if (minus_tab >= 0) {
            const View view = ViewShown();
            if (minus_back) SetView(BackOf(view));
            if (minus_line >= 0 && view == View::kLan && minus_line >= 1) {
                ActCell(minus_line - 1);
            } else if (minus_line >= 0) {
                Act(-1, minus_line);
            }
            const auto moves = [&](const std::function<void(int, int)>& send) {
                if ((minus_moves & Page::kMinusMoveUp) != 0) send(HOS_COOP_MENU_UP, 0);
                if ((minus_moves & Page::kMinusMoveDown) != 0) send(HOS_COOP_MENU_DOWN, 0);
                if ((minus_moves & Page::kMinusMoveLeft) != 0) send(HOS_COOP_MENU_LEFT, 0);
                if ((minus_moves & Page::kMinusMoveRight) != 0) send(HOS_COOP_MENU_RIGHT, 0);
                if ((minus_moves & Page::kMinusMoveA) != 0) send(HOS_COOP_MENU_CONFIRM, 0);
            };
            if (minus_mods) {
                FollowMods(true);
                moves([](int event, int arg) { CoopModsTab::MenuInput(event, arg, 0); });
                if (minus_list_back) CoopModsTab::MenuInput(HOS_COOP_MENU_BACK, 0, 0);
            } else if (view == View::kMenu) {
                moves([&](int event, int arg) { menu_events.push_back({event, arg, 0}); });
                if (minus_list_back) menu_back = true;
            }
            if (minus_moves != 0 || minus_list_back || minus_back || minus_line >= 0) g_live_poke = true;
        }
        if (!minus_mods) FollowMods(false);
        FollowMenu(shown || minus_tab >= 0);
        for (const MenuEvent& e : menu_events) CoopPause::NativeInput(e.event, e.arg, e.arg2);
        for (const std::uint32_t k : menu_keys) CoopPause::Key(k, false);
        for (const std::uint32_t c : menu_chars) CoopPause::Char(c);
        if (menu_back && g_menu_open.load() >= 0) {
            const CoopPauseMenu::Page page = MenuTabPage(g_menu_open.load());
            CoopPause::NativeInput(HOS_COOP_MENU_BACK);
            if (CoopPause::NativePage() != page) {   // (out of the page: open again, the rows have the pad)
                CoopPause::OpenNative(page);
                g_menu_list = false, g_menu_top = 0, g_menu_focus_reset = true;
            }
            g_live_poke = true;
        }
        if (!menu_events.empty() || !menu_keys.empty() || !menu_chars.empty()) g_live_poke = true;
        if (decide && !settings_shown) Act(row, list_line, row_clicked);
        else if (line_clicked >= 0 && !settings_shown) ClickLine(line_clicked);
        if (pick_cell >= 0) ActCell(pick_cell);
        if (set_change >= 0) ChangeSetting(set_change, set_dir);
        // (the stick's up and down bits we added are not the game's: only what it sent, filtered)
        const std::uint64_t to_game = out & (buttons | ~(kPadStickUp | kPadStickDown | kPadStickLeft | kPadStickRight));
        if (buttons != 0) g_test_tab_in = buttons, g_test_tab_out = to_game;   // (TestState)
        return to_game;
    } catch (...) {
        return buttons;
    }
}

bool LayoutCursor(float* x, float* y, bool* left) noexcept {
    try {
        return ReadCursor(x, y, left);
    } catch (...) {
        return false;
    }
}

bool ClientCursor(float* fx, float* fy, bool* left) noexcept {
    try {
        return ReadClientCursor(fx, fy, left);
    } catch (...) {
        return false;
    }
}

void CensusFromMenuState() noexcept {
    try {
        static const bool on = [] {
            const char* v = std::getenv("COOP_TEST_SCREEN_PROBE");
            return v != nullptr && v[0] == '1';
        }();
        if (!on) return;
        static std::uint64_t next_ms = 0;
        if (NowMs() < next_ms) return;
        next_ms = NowMs() + 50;
        const auto menu = CoopMapPage::MenuState();
        if (!menu) return;
        const std::uint64_t base = CoopRuntime::MainBase();
        for (std::uint32_t i = 0; i < menu->screen_count && i < menu->screens.size(); ++i) {
            const std::uint64_t screen = menu->screens[i];
            const std::uint64_t vtable = screen != 0 ? Read<std::uint64_t>(screen) : 0;
            if (vtable >= base) CensusUpdate(screen, vtable - base);
        }
    } catch (...) {
    }
}

bool GameMenuUp() noexcept {
    return MenuUpFrom(g_strip_state.load(), g_strip_seen_ms.load(), NowMs());
}

std::array<MenuActor, 4> MenuActors() noexcept {
    std::array<MenuActor, 4> out{};
    try {
        const std::uint64_t manager = Read<std::uint64_t>(CoopRuntime::MainBase() + kMenuManager);
        const std::uint64_t obj = manager != 0 ? Read<std::uint64_t>(manager + kPouchObject) : 0;
        if (obj == 0) return out;
        for (int slot = 0; slot < 4; ++slot) {
            const std::uint64_t at = obj + kPouchSlots + static_cast<std::uint64_t>(slot) * 0x20;
            out[static_cast<std::size_t>(slot)] = {PouchActor(obj, slot), Read<std::uint32_t>(at + kPouchSlotId)};
        }
    } catch (...) {
        out = {};
    }
    return out;
}

int MousePage() noexcept {
    return g_mouse.load();
}

bool Key(std::uint32_t vk, bool repeat) noexcept {
    try {
        std::lock_guard lock(g_mutex);
        const int was = g_page.Typing();
        const bool taken = g_page.Key(vk, repeat);
        if (was >= 0 && g_page.Typing() < 0) g_key_end_ms = NowMs();   // (Enter or Esc ended it: OnPad's moment)
        if (taken) g_live_poke = true;
        if ((vk == 0x0d || vk == 0x1b) && was >= 0) {   // (trace: Enter and Esc while typing)
            LOG_INFO(Core, "coop: pause tab: keyboard: {}{} while typing field {}: {}", vk == 0x0d ? "Enter" : "Esc",
                     repeat ? " (a repeat)" : "", was, g_page.Typing() < 0 ? "typing ended" : "still typing");
        }
        return taken;
    } catch (...) {
        return false;
    }
}

bool Char(std::uint32_t cp) noexcept {
    try {
        std::lock_guard lock(g_mutex);
        const bool taken = g_page.Char(cp);
        if (taken) g_live_poke = true;
        return taken;
    } catch (...) {
        return false;
    }
}

bool KeysTaken() noexcept {
    try {
        std::lock_guard lock(g_mutex);
        return g_page.Typing() >= 0;
    } catch (...) {
        return false;
    }
}

void ToTitle() noexcept {
    if (g_to_title.load() != 0) return;
    g_to_title_ms = NowMs(), g_to_title_phase_ms = NowMs();
    g_tap_downs = 0;
    g_to_title = 1;
    LOG_INFO(Core, "coop: pause tab: back to the title: a join with a file loaded (the host's save is loaded there)");
}

bool GoingToTitle() noexcept {
    return g_to_title.load() != 0;
}

std::string TestState() {
    const std::uint64_t now = NowMs();
    const std::string pads = fmt::format(" tab_in={:#x} tab_out={:#x}", g_test_tab_in.load(), g_test_tab_out.load());
    const bool menu = now - g_pages_seen_ms.load() < 400;
    // (Settings and look: co-op's menu's selected row, its name and value (spaces as _), its rows, the list's first
    // shown and whether it has the pad, the page's row, and the menu's Link shown and his turn; read before our lock:
    // co-op's menu takes its own)
    std::string mpart;
    if (g_menu_open.load() >= 0) {
        const CoopPauseMenu::View mv = CoopPause::NativeView();
        const auto under = [](std::string t) {
            for (char& c : t) c = c == ' ' ? '_' : c;
            return t.empty() ? std::string("-") : t;
        };
        const bool ok = mv.selected >= 0 && static_cast<std::size_t>(mv.selected) < mv.rows.size();
        mpart = fmt::format(" msel={} mname={} mvalue={} mrows={} mtyping={} mpage={}", mv.selected,
                            ok ? under(mv.rows[static_cast<std::size_t>(mv.selected)].name) : std::string("-"),
                            !ok ? std::string("-")
                            : mv.rows[static_cast<std::size_t>(mv.selected)].name.find("assword") != std::string::npos
                                ? std::string("(hidden)")
                                : under(MenuValue(mv.rows[static_cast<std::size_t>(mv.selected)], false)),
                            mv.rows.size(), mv.typing ? 1 : 0, static_cast<int>(CoopPause::NativePage()));
    }
    std::lock_guard lock(g_mutex);
    mpart += fmt::format(" mtop={} mlist={} mtab={} mopen={} model={} yaw={:.2f}", g_menu_top.load(),
                         g_menu_list.load() ? 1 : 0, g_menu_tab.load(), g_menu_open.load(), g_model.on ? 1 : 0, g_model.yaw);
    return fmt::format("menu={} pages_state={} pages_page={} shown={} which={} view={} typing={} cell={} set_row={} "
                       "set_category={} focused={} to_title={} kb={} online={} pouch={}",
                       menu ? 1 : 0, menu ? g_pages_seen_state.load() : -1, menu ? g_pages_seen_page.load() : -1,
                       g_page.Shown() ? 1 : 0, g_page.Which(), g_view.load(), g_page.Typing(), g_cell.load(),
                       g_set_row.load(), g_set_category.load(), menu ? FocusedShownName() : std::string("-"),
                       g_to_title.load(), !g_menu_typing_lines.empty() || g_page.Typing() >= 0 ? 1 : 0, g_online.load() ? 1 : 0,
                       g_test_pouch.load()) + pads + mpart;
}

}  // namespace CoopPauseTab
