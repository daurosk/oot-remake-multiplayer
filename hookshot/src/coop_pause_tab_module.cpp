// SPDX-License-Identifier: GPL-3.0-or-later
// coop_pause_tab.h, the module's part: patch 0030's call for every menu
// screen's update, the game's fields read, the game's own calls made on that
// screen's thread (its panes shown or hidden, its tabs lit, texts from the
// pack's labels), the pad filtered, the log. Only in hookshot_coop.dll (the
// tests drive Page and Probe themselves).
#include "coop_pause_tab.h"
#include "coop_i18n.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <utility>
#include <string>
#include <tuple>

#include "common/logging.h"
#include "coop_hook.h"
#include "coop_hookshot.h"
#include "coop/protocol/messages.hpp"
#include "coop_lan.h"
#include "coop_lan_net.h"
#include "coop_mod_screens.h"
#include "coop_pause.h"
#include "coop_room_list.h"
#include "coop_runtime.h"
#include "coop_lobby_game.h"
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
std::atomic<int> g_cell_page{0};
std::atomic<int> g_cell{-1};
std::atomic<int> g_cell_games{0};   // the cells on this page that hold a game (LiveCells)
std::array<std::atomic<std::uint64_t>, kCells> g_cell_pane_at{};   // the cells' panes in our page, this opening
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
std::uint64_t g_strip_gen = 0, g_tab_pane = 0;
bool g_pages_looked = false, g_pages_ok = false, g_strip_looked = false, g_strip_ok = false;
std::uint64_t g_pages_layout = 0;  // the layout the pages' panes were looked up in (the early page switch checks it)
std::uint64_t g_fourth_pane = 0;   // the game's fourth tab (Pa_Category_03), which ours follows in the strip's animation
std::uint64_t g_fourth_word = 0, g_tab_word = 0;   // the two tabs' word panes (N_MenuChangeTextIn_00)
float g_tab_rest_y = 0;            // our tab's own height in the layout (its place once the strip is open)
bool g_tab_shown = false;          // our tab shown now (the strip opening, open or closing)
// The fourth tab's place and fades at the last strip update (to take them one update ahead); none after a hide.
bool g_follow_had = false;
float g_follow_y = 0, g_follow_alpha = 0, g_follow_word = 0;
float g_alpha_set = -1, g_word_set = -1;   // the alphas last set on our tab and its word (-1: none since a hide)

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
bool LiveLookup(void* ctx, int slot, std::uint64_t gen) {
    if (slot < 0 || slot >= kLiveSlots) return false;   // (a slot past the table: never written through)
    LiveSlot& l = g_live[slot];
    if (l.bad) return false;
    if (l.gen == gen && l.text != 0) return true;
    char label[16];
    std::snprintf(label, sizeof label, "Coop_Live_%02d", slot);
    const std::string file = "LayoutMsg/PlusMenu_00";
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
bool LiveText(void* ctx, std::uint64_t layout, std::uint64_t gen, const std::string& path, int slot,
              const std::string& utf8) {
    if (slot < 0 || slot >= kLiveSlots) return false;
    LiveSlot& l = g_live[slot];
    if (l.shown == utf8) return true;
    if (path.size() >= 0x40 || utf8.size() >= kLivePlaceholder || !LiveLookup(ctx, slot, gen)) return false;
    // [0x00] the path lookup's layout, [0x10] the path, [0x60] the reference.
    std::uint8_t b[0x80] = {};
    std::memcpy(b + 0x10, path.c_str(), path.size() + 1);
    const std::uint64_t base = g_api->scratch(ctx, b, sizeof b);
    if (base == 0) return false;
    std::uint64_t name = 0;
    if (!Call(ctx, kPathFind, base + 0x10, layout, base, 0, 0, &name) || name == 0) return false;
    const std::uint64_t part = Read<std::uint64_t>(base);
    std::uint64_t table = 0, pane = 0;
    if (part == 0 || !Call(ctx, kPaneTable, part + 0x40, 0, 0, 0, 0, &table) || table == 0) return false;
    if (!Call(ctx, kPaneByName, table, name, 0, 0, 0, &pane) || pane == 0) return false;
    if (((Read<std::uint32_t>(pane + 0xa8) >> 22) & 0xf) != kTextPaneKind) return false;   // (not a text pane)
    std::string bytes = utf8;
    bytes.push_back('\0');
    if (!CoopRuntime::Write(l.text, bytes.data(), bytes.size())) return false;
    const std::uint64_t attrs = Read<std::uint64_t>(pane + 0x10);
    const std::uint32_t length = static_cast<std::uint32_t>(utf8.size()) + l.extra;
    std::uint8_t ref[0x18] = {};
    std::memcpy(ref + 0x00, &l.text, 8);
    std::memcpy(ref + 0x08, &length, 4);
    std::memcpy(ref + 0x10, &attrs, 8);
    if (!CoopRuntime::Write(base + 0x60, ref, sizeof ref)) return false;
    if (!Call(ctx, kPaneSetMsg, pane, base + 0x60, 0)) return false;
    l.shown = utf8;
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

// What the rows say now (co-op's host status and the Lobby's room, its players). No lock of ours held: they take
// their own.
SessionView ViewNow(View view) {
    HosCoopStatus st{};
    st.size = sizeof st;
    CoopHookshot::HostMenu().Status(&st);
    SessionView v;
    v.s.by_launcher = CoopPause::ByLauncher();
    v.s.failed = st.state == HOS_COOP_FAILED;
    v.s.hosting = st.state == HOS_COOP_HOSTING;
    v.s.in_one = (st.state != HOS_COOP_IDLE && st.state != HOS_COOP_FAILED) || CoopSave::SessionLive();
    v.connecting = st.state == HOS_COOP_STARTING || st.state == HOS_COOP_JOINING || st.state == HOS_COOP_WAIT_HOST;
    std::string room = st.relay_room[0] != 0 ? st.relay_room : st.relay_code[0] != 0 ? st.relay_code : "";
    if (const auto at = room.find('@'); at != std::string::npos) room.resize(at);   // (the name friends type first)
    v.room = room;
    v.s.code = v.s.hosting && (!room.empty() || st.invite[0] != 0);
    v.s.invite = v.s.hosting && room.empty() && st.invite[0] != 0;
    v.s.wait_host = st.state == HOS_COOP_WAIT_HOST;
    v.s.can_resync = !v.s.hosting && CoopSave::CanResync();
    v.why = std::string(st.message, strnlen(st.message, sizeof st.message));
    v.note = CoopPause::PageNote();
    if (const std::string& title = CoopWindowBrand::TitleOf(CoopRuntime::GameDir()); title != CoopWindowBrand::kTitle) {
        v.version = title;   // the kit's release (0.11.6), as the title bar
    }
    v.confirm_leave = Confirming(View::kMain, kRowLeave);
    v.confirm_kick = Confirming(View::kPlayerHost, 2);
    v.confirm_ban = Confirming(View::kPlayerHost, 3);
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
        v.s.auto_rejoin = CoopPause::AutoRejoin();
        v.s.rejoining = v.s.auto_rejoin && CoopPause::AutoRejoining();
    }
    if (view == View::kLan) {
        const auto games = CoopLanNet::Games(NowMs());
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
        const CoopRoomList::Snapshot rooms = CoopRoomList::Now();
        for (const CoopRoomList::Room& r : rooms.rooms) {
            v.online_rows.push_back(CoopRoomList::CellText(r, coop::protocol::kProtocolVersion));
            v.online_list.push_back(ListLine{ListName(r.host, r.room),
                                             ListValue(r.players, r.max, r.protocol == coop::protocol::kProtocolVersion)});
        }
        v.online_state = rooms.state;
        v.s.online_listed = static_cast<int>(rooms.rooms.size());
        v.s.online = g_online.load();
        v.s.cell_page = g_cell_page.load();
        v.s.list_room = CoopPause::ListRoom();   // (the files' setting; read only while this view shows)
        v.cell = g_cell.load();
        // (the relay asked again while the Online list shows: at most every CoopRoomList::kEveryMs, from its own thread)
        CoopRoomList::Look(v.s.online && !v.s.in_one && !v.s.by_launcher, NowMs(), CoopPause::Relay());
    }
    if (view == View::kJoin) {
        v.s.rejoin = CoopPause::Rejoin(&v.rejoin_host);
        v.relay_set = CoopPause::RelaySet();   // (co-op's own lock: before ours, never inside it)
    }
    if (view == View::kSession) {
        v.s.has_password = v.s.hosting && st.password[0] != 0;
        v.s.password_shown = v.s.has_password && g_show_password.load();
        if (v.s.password_shown) v.host_password.assign(st.password, strnlen(st.password, sizeof st.password));
        std::lock_guard lock(g_says_mutex);
        v.says = g_says;
    }
    if (view == View::kJoin || view == View::kLanGame) {
        std::lock_guard lock(g_mutex);   // (the fields are the page's; no other lock of ours is held here)
        v.typing = g_page.Typing();
        v.room_field = g_page.Field(0).Shown(v.typing == 0, kRowChars - 6);       // ("Room: ")
        v.password_field = g_page.Field(1).Shown(v.typing == 1, kRowChars - 10);  // ("Password: ")
        v.room_ready = v.s.room_ready = g_page.Field(0).Ready();
        v.s.lan_password = !g_page.Field(1).text().empty();
        v.lan_game = g_lan_pick.row;
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
std::array<std::uint64_t, kListRows> g_list_panes{};
std::array<int, kListRows> g_list_vis{};
std::array<float, kListRows> g_list_alpha{};
// Our page's five rows: dimmed while the list has the pad (the game's cursor frame stays on its row there); on the
// browser, the list not shown dimmed too, as a tab.
std::array<std::uint64_t, kRows> g_row_panes{};
std::array<float, kRows> g_rows_alpha{};

void LiveCells(void* ctx, std::uint64_t layout, std::uint64_t gen, const SessionView& v) {
    if (g_cells_gen != gen) {
        g_cells_gen = gen;
        std::uint64_t table = 0;
        Call(ctx, kPaneTable, layout + 0x40, 0, 0, 0, 0, &table);
        const std::uint64_t ours = PartLayout(table, "Pa_Coop_00");
        int found = 0;
        for (int i = 0; i < kListRows; ++i) {
            char name[16];
            std::snprintf(name, sizeof name, "Pa_List_%02d", i);
            const std::uint64_t pane = PaneIn(ctx, ours, name);
            const std::size_t k = static_cast<std::size_t>(i);
            g_list_panes[k] = IsPart(pane) ? pane : 0;
            if (i > 0) g_cell_pane_at[k - 1] = g_list_panes[k];
            g_list_vis[k] = -1, g_list_alpha[k] = -1;
            if (!IsPart(pane)) continue;
            ++found;
            // (inside the row: the value's arrows and the check mark an Options row has, never ours)
            const std::uint64_t row = PartLayout(ours, name);
            for (const char* inner : {"N_Content_00", "P_CheckMark_00"}) {
                if (const std::uint64_t in = PaneIn(ctx, row, inner); in != 0) SetVisible(ctx, in, false);
            }
        }
        for (int r = 0; r < kRows; ++r) {
            char name[16];
            std::snprintf(name, sizeof name, "Pa_Button_%02d", r);
            const std::uint64_t pane = PaneIn(ctx, ours, name);
            g_row_panes[static_cast<std::size_t>(r)] = IsPart(pane) ? pane : 0;
        }
        g_rows_alpha.fill(-1);
        LOG_INFO(Core, "coop: pause tab: the game browser's list rows: {} of {}{}", found, kListRows,
                 found == kListRows ? "" : " (build the puppet pack again for the browser)");
    }
    const bool browse = ViewShown() == View::kLan;
    const std::vector<ListLine> lines = ListLines(v);
    int games = 0;
    for (int c = 0; c < kCells; ++c) games += CellIndex(v.s, c) >= 0 ? 1 : 0;
    g_cell_games = browse ? games : 0;
    const int picked = browse ? g_cell.load() : -1;
    for (int i = 0; i < kListRows; ++i) {
        const std::size_t k = static_cast<std::size_t>(i);
        const std::uint64_t pane = g_list_panes[k];
        if (pane == 0) continue;
        const bool on = browse && !lines[k].name.empty();
        if (on) {
            char path[64];
            std::snprintf(path, sizeof path, "Pa_Coop_00/Pa_List_%02d/T_Option_00", i);
            // The browser's own words (the header, "Looking for games...", a relay's state) in the
            // player's language; a game's name is the player's own and comes back unchanged.
            LiveText(ctx, layout, gen, path, 8 + 2 * i, CoopI18n::Tr(lines[k].name));
            std::snprintf(path, sizeof path, "Pa_Coop_00/Pa_List_%02d/T_Text_00", i);
            LiveText(ctx, layout, gen, path, 9 + 2 * i,
                     lines[k].value.empty() ? std::string(" ") : CoopI18n::Tr(lines[k].value));
        }
        if (g_list_vis[k] != (on ? 1 : 0)) SetVisible(ctx, pane, on), g_list_vis[k] = on ? 1 : 0;
        // (the header a little dim, as a table's; a game bright when picked, or all of them when the rows have the pad)
        const float alpha = i == 0 ? 0.7f : picked < 0 || picked == i - 1 ? 1.0f : 0.45f;
        if (on && g_list_alpha[k] != alpha) CallAlpha(ctx, pane, alpha), g_list_alpha[k] = alpha;
    }
    for (int r = 0; r < kRows; ++r) {
        const std::size_t k = static_cast<std::size_t>(r);
        const bool other_list = browse && !v.s.in_one && !v.s.by_launcher && r == (v.s.online ? 0 : 1);
        const float alpha = picked >= 0 ? 0.45f : other_list ? 0.55f : 1.0f;
        if (g_row_panes[k] != 0 && g_rows_alpha[k] != alpha) CallAlpha(ctx, g_row_panes[k], alpha), g_rows_alpha[k] = alpha;
    }
}

void LiveRows(void* ctx, std::uint64_t layout, std::uint64_t gen, const SessionView& v) {
    std::lock_guard lock(g_live_mutex);
    if (layout == 0 || gen != g_live_gen) return;   // (another opening came meanwhile: its own turn sets them)
    const std::vector<std::string> rows = RowTexts(ViewShown(), v);
    for (int i = 0; i < kRows && i < kLiveSlots; ++i) {
        char path[64];
        std::snprintf(path, sizeof path, "Pa_Coop_00/Pa_Button_%02d/T_Btn_00", i);
        // The row's words in the player's language (coop_i18n.h). The rows are built in English in
        // coop_pause_tab.cpp (its tests read them so), and each whole row is looked up here, as it is
        // handed to the game: the rows with a value ("Room: 3", "Kick Saria") are the table's shapes.
        LiveText(ctx, layout, gen, path, i, CoopI18n::Tr(rows[static_cast<std::size_t>(i)]));
    }
    LiveCells(ctx, layout, gen, v);
}

// The pages' panes, looked up once per opening (`calls`: ctx may call(); else looked up at a later update).
void LookPages(void* ctx, bool calls, const Facts& f, std::uint64_t gen) {
    if (gen != g_pages_gen) {
        g_pages_gen = gen, g_pages_screen = f.screen, g_pages_looked = false, g_pages_ok = false;
        g_system_pane = 0, g_coop_pane = 0;
    }
    if (g_pages_looked || !calls || f.layout == 0) return;
    g_pages_looked = true;
    g_system_pane = FindOwnPane(ctx, f.layout, "Pa_System_00");
    g_coop_pane = FindOwnPane(ctx, f.layout, "Pa_Coop_00");
    g_pages_ok = IsPart(g_system_pane) && IsPart(g_coop_pane);
    g_pages_layout = f.layout;
    LOG_INFO(Core, "coop: pause tab: the pages' panes: Pa_System_00 {:#x}, Pa_Coop_00 {:#x}{}", g_system_pane,
             g_coop_pane, g_pages_ok ? "" : " (not both parts: the Multiplayer page stays off; is the pack's layout in?)");
    if (g_pages_ok) {
        // The rows say what they do only through the live texts (the pack's Coop_Live_NN): a pack without them would
        // show its fixed words over rows that do other things, so the page stays off then, as without its panes.
        std::lock_guard lock(g_live_mutex);
        if (!LiveLookup(ctx, 0, gen) && g_live[0].bad) {
            g_pages_ok = false;
            LOG_INFO(Core, "coop: pause tab: the pack has no live texts (Coop_Live_00): the Multiplayer page stays off "
                           "(build the puppet pack again)");
        }
    }
}

// The strip's fifth tab, the same way.
void LookStrip(void* ctx, bool calls, const Facts& f, std::uint64_t gen) {
    if (gen != g_strip_gen) {
        g_strip_gen = gen, g_strip_looked = false, g_strip_ok = false, g_tab_pane = 0;
        g_fourth_pane = 0, g_fourth_word = 0, g_tab_word = 0, g_tab_shown = false, g_follow_had = false;
        g_alpha_set = g_word_set = -1;
    }
    if (g_strip_looked || !calls || f.layout == 0) return;
    g_strip_looked = true;
    g_tab_pane = FindOwnPane(ctx, f.layout, "Pa_Category_04");
    g_strip_ok = IsPart(g_tab_pane);
    if (g_strip_ok) {   // (the fourth tab, which ours follows in the strip's animation, and the two tabs' words)
        g_fourth_pane = FindOwnPane(ctx, f.layout, "Pa_Category_03");
        g_tab_rest_y = Read<float>(g_tab_pane + kPaneOwnY);
        std::uint64_t table = 0;
        Call(ctx, kPaneTable, f.layout + 0x40, 0, 0, 0, 0, &table);
        g_fourth_word = PaneIn(ctx, PartLayout(table, "Pa_Category_03"), "N_MenuChangeTextIn_00");
        g_tab_word = PaneIn(ctx, PartLayout(table, "Pa_Category_04"), "N_MenuChangeTextIn_00");
    }
    LOG_INFO(Core, "coop: pause tab: the strip's fifth tab: Pa_Category_04 {:#x}{}", g_tab_pane,
             g_strip_ok ? "" : " (not a part: left alone, the Multiplayer page stays off)");
}

// The pages: our page over System's, or System's alone. The rows' texts once per opening.
// The pages' slide (Page::Do::slide, SlideOffsets): our page and System's both shown and moved from their own places
// each update of the pages, then the one that went hidden and both back at their places. g_mutex.
struct Slide {
    int dir = 0;                  // 0: none
    std::uint64_t start_ms = 0, gen = 0;
    float ours_x = 0, system_x = 0;   // their own places (+0x6c), as they were when it began
};
Slide g_slide;

void PlacePane(void* ctx, std::uint64_t pane, float x) {
    if (Read<float>(pane + 0x6c) == x) return;
    CoopRuntime::Write(pane + 0x6c, &x, sizeof x);
    Call(ctx, kPaneMoved, pane);   // (its matrices made again, as the game's own place setter ends)
}

void EndSlide(void* ctx) {
    if (g_slide.dir == 0) return;
    if (g_slide.gen == g_pages_gen) {   // (a new opening has its own panes, at their places)
        PlacePane(ctx, g_coop_pane, g_slide.ours_x);
        PlacePane(ctx, g_system_pane, g_slide.system_x);
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
    const auto [ours, system] = SlideOffsets(g_slide.dir, p);
    PlacePane(ctx, g_coop_pane, g_slide.ours_x + ours);
    PlacePane(ctx, g_system_pane, g_slide.system_x + system);
    if (p >= 1.0f) {
        const bool shown = g_slide.dir > 0;
        EndSlide(ctx);
        SetVisible(ctx, g_coop_pane, shown);
        SetVisible(ctx, g_system_pane, !shown);
    }
}

void ApplyPages(void* ctx, const Facts& f, const Page::Do& d) {
    const std::uint64_t layout = f.layout;
    if (layout == 0 || !g_pages_ok) return;
    if (d.set_page) {
        EndSlide(ctx);   // (one going on: both back at their places first)
        if (d.slide != 0) {
            g_slide.dir = d.slide, g_slide.start_ms = NowMs(), g_slide.gen = g_pages_gen;
            g_slide.ours_x = Read<float>(g_coop_pane + 0x6c), g_slide.system_x = Read<float>(g_system_pane + 0x6c);
            SetVisible(ctx, g_coop_pane, true);
            SetVisible(ctx, g_system_pane, true);
            StepSlide(ctx, g_slide.start_ms);
            LOG_INFO(Core, "coop: pause tab: the pages slide {} (our page's place x {:.0f}, System's {:.0f})",
                     d.slide > 0 ? "to ours" : "back to System", g_slide.ours_x, g_slide.system_x);
        } else {
            SetVisible(ctx, g_coop_pane, d.page_shown);
            SetVisible(ctx, g_system_pane, !d.page_shown);
        }
    }
    if (d.set_texts) {
        // (the rows' words are the live texts', LiveRows: the pane-text call never reaches their "=T_Btn_00" panes)
        // Inside our copy: its controller picture (System's, not ours) hidden.
        std::uint64_t table = 0;
        Call(ctx, kPaneTable, layout + 0x40, 0, 0, 0, 0, &table);
        const std::uint64_t ours = PartLayout(table, "Pa_Coop_00");
        const std::uint64_t controller = PaneIn(ctx, ours, "Pa_SystemController_00");
        if (IsPart(controller)) SetVisible(ctx, controller, false);
        LOG_INFO(Core, "coop: pause tab: the page's controller picture {}", IsPart(controller) ? "hidden" : "not found");
    }
}

// The strip: the fifth tab shown in the Plus menu (filled as a Plus tab, our word), lit while our page shows, and
// System's tab lit only when System is the page shown.
void ApplyStrip(void* ctx, const Facts& f, const Page::Do& d) {
    const std::uint64_t layout = f.layout;
    const std::uint64_t tabs = Read<std::uint64_t>(f.screen + kStripTabArray);
    if (layout == 0 || tabs == 0 || !g_strip_ok) return;
    SetVisible(ctx, g_tab_pane, d.tab_shown);
    if (!d.tab_shown) g_follow_had = false, g_alpha_set = g_word_set = -1;
    g_tab_shown = d.tab_shown;
    if (!d.tab_shown) return;
    const std::uint64_t tab3 = Read<std::uint64_t>(tabs + 8 * 3), tab4 = Read<std::uint64_t>(tabs + 8 * 4);
    if (tab3 == 0 || tab4 == 0) return;
    if (d.fill_tab) {
        Call(ctx, kTabFill, tab4, kSystemPage, 1);   // (a Plus tab's look, as the game fills its four)
        const bool text = SetText(ctx, layout, "Pa_Category_04/T_Text_00", "LayoutMsg/MenuOverlay_00",
                                  "Pa_Category_04-T_Text_00");
        Call(ctx, kTabOpen, tab4, 0);   // (opened and shown, as the game's fill ends for each of its tabs)
        // Its word faded in: the strip's own "text in" animation fades in the game's four tabs' words only (it names
        // their panes, 8 to 11), so ours stays at alpha 0 (seen in a run, 2026-11-18: N_MenuChangeTextIn_00's own
        // alpha, +0x74, 0 in ours, 1 in the game's). Set to 1 with the game's alpha setter.
        std::uint64_t table = 0;
        Call(ctx, kPaneTable, layout + 0x40, 0, 0, 0, 0, &table);
        const std::uint64_t text_in = PaneIn(ctx, PartLayout(table, "Pa_Category_04"), "N_MenuChangeTextIn_00");
        if (text_in != 0) CallAlpha(ctx, text_in, 1.0f);
        LOG_INFO(Core, "coop: pause tab: the fifth tab filled (its word {}, {})", text ? "set" : "not set",
                 text_in != 0 ? "faded in" : "its text pane not found");
    }
    Call(ctx, kTabLight, tab3, f.lit == kSystemPage && !d.tab_lit ? 1 : 0, 0);
    Call(ctx, kTabLight, tab4, d.tab_lit ? 1 : 0, 0);
}

// Our tab through the strip's opening and closing, as the game's four: the strip's animation moves and fades the
// parent of the game's tabs (seen in a run, 2026-11-19: the fourth tab's place on the screen, +0x50, from 1024 down to
// 1000 in 300 ms as the strip opens, and its alpha with its parents', +0x78, from 0.16 to 1), and ours sits under
// RootPane, out of that parent. So at each strip update ours takes the fourth's height (its own place, +0x70: RootPane
// is the screen's origin, then kPaneMoved) and alpha (the alpha setter). Not when the two
// are far apart (not the strip seen: ours stays where the pack put it).
void FollowFourth(void* ctx) {
    if (!g_tab_shown || g_fourth_pane == 0 || !g_strip_ok) return;
    const float y = Read<float>(g_fourth_pane + kPaneScreenY), alpha = Read<float>(g_fourth_pane + kPaneScreenAlpha);
    const float word = g_fourth_word != 0 ? Read<float>(g_fourth_word + kTextAlpha) : 1.0f;
    if (!std::isfinite(y) || !std::isfinite(alpha) || !std::isfinite(word) || std::fabs(y - g_tab_rest_y) > kFollowMost) {
        return;
    }
    // (one update ahead: these are the fourth's from the last frame, before the strip's own update moves it on)
    const bool had = std::exchange(g_follow_had, true);
    const float want_y = had ? Ahead(g_follow_y, y, g_tab_rest_y) : y;
    const float want_alpha = had ? Ahead(g_follow_alpha, alpha, alpha >= g_follow_alpha ? 1.0f : 0.0f) : alpha;
    const float want_word = had ? Ahead(g_follow_word, word, word >= g_follow_word ? 1.0f : 0.0f) : word;
    g_follow_y = y, g_follow_alpha = alpha, g_follow_word = word;
    if (Read<float>(g_tab_pane + kPaneOwnY) != want_y) {
        CoopRuntime::Write(g_tab_pane + kPaneOwnY, &want_y, sizeof want_y);
        Call(ctx, kPaneMoved, g_tab_pane);
    }
    // (the alphas only when they change, as the height: the strip settled, nothing is called)
    if (const float a = std::clamp(want_alpha, 0.0f, 1.0f); a != g_alpha_set) {
        CallAlpha(ctx, g_tab_pane, a);
        g_alpha_set = a;
    }
    // (and its word fades in with the fourth's: the strip's "text in" animation names only the game's four words)
    if (g_fourth_word != 0 && g_tab_word != 0) {
        if (const float w = std::clamp(want_word, 0.0f, 1.0f); w != g_word_set) {
            CallAlpha(ctx, g_tab_word + kTextAlphaBlock, w);
            g_word_set = w;
        }
    }
}

// A screen's own menu sound by name (kScreenSound), on its update.
void PlaySound(void* ctx, std::uint64_t screen, const char* name) {
    const std::uint64_t at = g_api->scratch(ctx, name, std::strlen(name) + 1);
    if (at != 0) Call(ctx, kScreenSound, screen, at);
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

// --- The mouse over our rows (MouseRows): the cursor and the rows' boxes, read at each pad read. g_mutex. ---
MouseRows g_mouse_rows;
std::uint64_t g_row_buttons_gen = 0, g_row_buttons_ms = 0;
std::array<std::uint64_t, kRows> g_row_buttons{};   // System's Pa_Button_00..04 of this opening of the pages

// The rows' boxes where the game puts them now. System's five buttons are found once an opening (again each 500 ms
// until all five are), among the pages screen's buttons: the shown ones of that name.
RowBoxes ReadRowBoxes(std::uint64_t now_ms) {
    RowBoxes boxes{};
    const std::uint64_t pages = g_pages_screen;
    if (pages == 0) return boxes;
    const auto read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
    const bool all = std::all_of(g_row_buttons.begin(), g_row_buttons.end(), [](std::uint64_t b) { return b != 0; });
    if (g_row_buttons_gen != g_pages_gen || (!all && now_ms - g_row_buttons_ms > 500)) {
        g_row_buttons_gen = g_pages_gen, g_row_buttons_ms = now_ms, g_row_buttons = {};
        for (const std::uint64_t b : CoopModScreens::Buttons(read, pages)) {
            const int row = RowOfButton(CoopModScreens::ButtonName(read, b));
            if (row >= 0 && g_row_buttons[static_cast<std::size_t>(row)] == 0 && CoopModScreens::HitBox(read, b)) {
                g_row_buttons[static_cast<std::size_t>(row)] = b;
            }
        }
    }
    for (int i = 0; i < kRows; ++i) {
        if (const auto box = CoopModScreens::HitBox(read, g_row_buttons[static_cast<std::size_t>(i)])) {
            boxes[static_cast<std::size_t>(i)] = RowBox{true, box->x0, box->y0, box->x1, box->y1};
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
    *left = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
    return true;
#else
    (void)x, (void)y, (void)left;
    return false;   // (Longshot's mouse in the game's menus is Windows' only)
#endif
}

void SetView(View v) {
    if (g_view.exchange(static_cast<int>(v)) != static_cast<int>(v)) {
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
        const CoopRoomList::Snapshot rooms = CoopRoomList::Now();
        if (i >= static_cast<int>(rooms.rooms.size())) return;
        const CoopRoomList::Room& r = rooms.rooms[static_cast<std::size_t>(i)];
        if (!CoopRoomList::Joinable(r, coop::protocol::kProtocolVersion)) {
            LOG_INFO(Core, "coop: pause tab: cell {} chosen: that room cannot be joined from here (full, or another "
                           "version)", cell);
            return;
        }
        LOG_INFO(Core, "coop: pause tab: cell {} chosen: a room on the relay's list, to the Join view", cell);
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
    const auto games = CoopLanNet::Games(NowMs());
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
    SetView(View::kLanGame);   // (the password first: every session has one)
}

// A row's action (after the lock: co-op's menu, the Lobby and the chat take their own).
void Act(int row) {
    const View view = ViewShown();
    const SessionView v = ViewNow(view);
    const RowAction a = ActionFor(view, row, v.s);
    LOG_INFO(Core, "coop: pause tab: row {} chosen (view {}): {}", row, static_cast<int>(view), ActionWords(a));
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
            SetView(View::kLanGame);   // (the password first: every session has one, and co-op's menu would ask it)
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
    case RowAction::kAllow: CoopHook::SendChat("/allow"); break;   // (its answer, or "nobody asks", shows in the chat)
    case RowAction::kPasteAnswer: CoopPause::PasteAnswerNow(); break;
    case RowAction::kNone: break;
    }
}

}  // namespace

void Init(const HosCoopApi* api) {
    g_api = api;
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
        if (f.kind == Kind::kOther) return;   // (most updates: the HUD's and the other screens', once a frame each)
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
            }
        }
        std::vector<std::string> lines;
        Page::Do d;
        std::string why;
        bool was = false, now_shown = false;
        bool early_live = false;   // our page back at the strip's update: its rows' words set there too
        int focused = -1;
        std::uint64_t gen = 0;
        const bool calls = ctx != nullptr && CanCall();
        {
            std::lock_guard lock(g_mutex);
            lines = g_probe.Seen(f, NowMs());
            was = g_page.Shown();
            // This opening's panes first, so the page is usable only with the panes of the screens updating now (an
            // undrawn page never takes System's R or A, and the mouse keeps the fifth tab out: MousePage -2).
            gen = g_page.Track(f);
            if (!g_page.Shown()) SetView(View::kMain);
            g_page.SetSubView(ViewShown() != View::kMain);
            if (f.kind == Kind::kPages) LookPages(ctx, calls, f, gen);
            if (f.kind == Kind::kStrip) LookStrip(ctx, calls, f, gen);
            if (f.kind != Kind::kBlocker) g_page.SetUsable(CanCall() && g_pages_ok && g_strip_ok);
            d = g_page.OnScreen(f, NowMs());
            now_shown = g_page.Shown();
            why = g_page.TakeWhy();
            g_mouse = g_page.MousePage();
            if (f.kind == Kind::kPages && now_shown) focused = FocusedRow();
            if (d.any() && calls) {
                if (f.kind == Kind::kPages) ApplyPages(ctx, f, d);
                if (f.kind == Kind::kStrip) ApplyStrip(ctx, f, d);
                if (f.kind == Kind::kStrip && d.set_page && g_pages_screen != 0 && g_pages_ok &&
                    Read<std::uint64_t>(g_pages_screen + kScreenLayout) == g_pages_layout) {
                    early_live = d.page_shown;
                    Facts pages;
                    pages.kind = Kind::kPages, pages.screen = g_pages_screen, pages.layout = g_pages_layout;
                    Page::Do page_only;
                    page_only.set_page = true, page_only.page_shown = d.page_shown, page_only.slide = d.slide;
                    ApplyPages(ctx, pages, page_only);   // (the pages' own update comes a frame later)
                }
            }
            if (f.kind == Kind::kPages && calls) StepSlide(ctx, NowMs());   // (each update while the pages slide)
            if (f.kind == Kind::kStrip && ctx != nullptr && CanCall()) FollowFourth(ctx);
            // (the menu sounds of the presses the module took: the game starts none for them)
            if (ctx != nullptr && CanCall()) {
                if (f.kind == Kind::kStrip) {
                    if (const char* sound = g_page.TakeStripSound()) PlaySound(ctx, screen, sound);
                } else if (f.kind == Kind::kPages) {
                    if (const char* sound = g_page.TakePagesSound()) PlaySound(ctx, screen, sound);
                }
            }
        }
        for (const std::string& line : lines) LOG_INFO(Core, "{}", line);
        if (was && !now_shown) LOG_INFO(Core, "coop: pause tab: System back ({})", why);
        if (f.kind == Kind::kPages && now_shown && f.state <= 2 && calls) {
            if (!KeepConfirm(true, focused, g_confirm_row.load())) g_confirm_until = 0;   // (the cursor left that row)
            FollowSays();   // (a Host or Join refused: why, on the Session view)
            // (after our lock: the status and the Lobby take their own; read only when the rows are due)
            if (LiveDue(gen)) LiveRows(ctx, f.layout, gen, ViewNow(ViewShown()));
        }
        // (the games on this network heard while their view shows; the listener closes a while after)
        if (f.kind == Kind::kPages) {
            CoopLanNet::Look(now_shown && ViewShown() == View::kLan, NowMs());
            // (the relay's list left, the page gone or the Local tab: its asks stop; ViewNow asks while it shows)
            if (!(now_shown && ViewShown() == View::kLan && g_online.load())) CoopRoomList::Look(false, NowMs(), {});
        }
        if (early_live && calls && LiveDue(g_pages_gen)) LiveRows(ctx, g_pages_layout, g_pages_gen, ViewNow(ViewShown()));
        if (!now_shown) g_confirm_until = 0;
    } catch (...) {
        // (a log line or a look lost is all: the game's update goes on)
    }
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
        bool typing = false;
        std::uint64_t out = 0;
        bool was = false, shown = false;
        bool decide = false;
        int row = -1, pick_cell = -1;
        std::string why;
        {
            std::lock_guard lock(g_mutex);
            was = g_page.Shown();
            const int typing_was = g_page.Typing();
            const std::uint64_t field_was = std::hash<std::string>{}(g_page.Field(0).text() + '\n' +
                                                                     g_page.Field(1).text()) ^ g_page.Field(0).cursor() ^
                                            (g_page.Field(1).cursor() << 8);
            out = g_page.OnPad(in, NowMs());
            typing = g_page.Typing() >= 0;
            const std::uint64_t field_now = std::hash<std::string>{}(g_page.Field(0).text() + '\n' +
                                                                     g_page.Field(1).text()) ^ g_page.Field(0).cursor() ^
                                            (g_page.Field(1).cursor() << 8);
            if (typing_was != g_page.Typing() || field_was != field_now) g_live_poke = true;
            shown = g_page.Shown();
            decide = g_page.TakeDecide();
            // The mouse over our rows (MouseRows): the game's cursor stepped to the row under it, and an A just after
            // a click (Longshot presses A for it) is the clicked row's, not the row the game's cursor is on.
            if (shown && !typing) {
                MouseRows::In mi;
                mi.now_ms = NowMs();
                mi.inside = ReadCursor(&mi.x, &mi.y, &mi.left);
                mi.pad = (in & (kPadUp | kPadDown | kPadStickUp | kPadStickDown)) != 0;
                mi.focused = FocusedRow();
                if (mi.inside) mi.boxes = ReadRowBoxes(mi.now_ms);
                out |= g_mouse_rows.Step(mi);
            } else {
                g_mouse_rows.Reset();
            }
            // The game browser's cells (View::kLan): Right goes into them, the arrows move among them (none reaches the
            // game's cursor), Left from the left column or B gives the pad back to the rows; the mouse picks one by
            // moving onto it, and a click on one is its A (Longshot presses nothing there: the cells are no buttons).
            const bool browse = shown && !typing && ViewShown() == View::kLan;
            const std::uint64_t bdown = in & ~g_browse_last;
            g_browse_last = in;
            int cell = browse ? g_cell.load() : -1;
            const int games = g_cell_games.load();
            if (cell >= games) cell = games - 1;
            if (browse) {
                const bool right = (bdown & (kPadRight | kPadStickRight)) != 0, left = (bdown & (kPadLeft | kPadStickLeft)) != 0;
                const bool up = (bdown & (kPadUp | kPadStickUp)) != 0, down = (bdown & (kPadDown | kPadStickDown)) != 0;
                if (cell < 0 && right && games > 0) {
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
                    if (held && !g_cells_held) g_cells_press = hover;
                    if (!held && g_cells_held && hover >= 0 && hover == g_cells_press && hover < games) {
                        pick_cell = hover;   // (a click on a cell: its A)
                        g_page.Sound("button_decide");
                    }
                    g_cells_held = held;
                }
                if (cell >= 0) out &= ~(kPadUp | kPadDown | kPadStickUp | kPadStickDown);   // (the rows' cursor stays)
            }
            if (cell != g_cell.load()) g_cell = cell, g_live_poke = true;
            if (decide) {
                const int clicked = g_mouse_rows.TakeClicked(NowMs());
                if (clicked < 0 && cell >= 0) {
                    pick_cell = cell;   // (A on a cell)
                    decide = false;
                } else {
                    row = clicked >= 0 ? clicked : FocusedRow();   // (under the lock: the pages screen is the updates')
                    if (clicked == MouseRows::kOffRows) decide = false;   // (a click off our rows: no row's A)
                    if (clicked >= 0 && cell >= 0) g_cell = -1, g_live_poke = true;   // (a row clicked: the rows' pad)
                }
            }
            if (g_page.TakeBack()) {
                if (cell >= 0) {
                    g_cell = -1, g_live_poke = true;   // (B in the cells: the rows have the pad again)
                } else {
                    SetView(BackOf(ViewShown()));   // (B in a view: where its Back goes)
                }
            }
            if (!shown) SetView(View::kMain);
            g_page.SetSubView(ViewShown() != View::kMain);
            why = g_page.TakeWhy();
            g_mouse = g_page.MousePage();
        }
        if (shown && stick != nullptr) {
            stick[0] = 0, stick[2] = 0, stick[3] = 0;   // (only the left stick's up and down: the game's cursor over our rows)
            if (typing || g_cell.load() >= 0) stick[1] = 0;   // (a field typed into, or the cells: the rows' cursor stays)
        }
        if (!was && shown) LOG_INFO(Core, "coop: pause tab: our page shown (R on System)");
        if (was && !shown) LOG_INFO(Core, "coop: pause tab: System back ({})", why);
        if (decide) Act(row);
        if (pick_cell >= 0) ActCell(pick_cell);
        // (the stick's up and down bits we added are not the game's: only what it sent, filtered)
        return out & (buttons | ~(kPadStickUp | kPadStickDown | kPadStickLeft | kPadStickRight));
    } catch (...) {
        return buttons;
    }
}

int MousePage() noexcept {
    return g_mouse.load();
}

bool Key(std::uint32_t vk, bool repeat) noexcept {
    try {
        std::lock_guard lock(g_mutex);
        const bool taken = g_page.Key(vk, repeat);
        if (taken) g_live_poke = true;
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

}  // namespace CoopPauseTab
