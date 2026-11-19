// SPDX-License-Identifier: GPL-3.0-or-later
// coop_pause_tab.h, the module's part: patch 0030's call for every menu
// screen's update, the game's fields read, the game's own calls made on that
// screen's thread (its panes shown or hidden, its tabs lit, texts from the
// pack's labels), the pad filtered, the log. Only in hookshot_coop.dll (the
// tests drive Page and Probe themselves).
#include "coop_pause_tab.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <utility>
#include <string>

#include "common/logging.h"
#include "coop_hook.h"
#include "coop_hookshot.h"
#include "coop_mod_screens.h"
#include "coop_pause.h"
#include "coop_runtime.h"
#include "coop_lobby_game.h"
#include "coop_lobby.h"
#include "coop_save.h"

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
constexpr int kLiveSlots = 8;
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
    v.confirm_leave = Confirming(View::kMain, kRowLeave);
    v.confirm_kick = Confirming(View::kPlayerHost, 2);
    v.confirm_ban = Confirming(View::kPlayerHost, 3);
    const CoopLobby::Seen seen = CoopLobby::Now();
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
    if (view == View::kJoin) {
        v.s.rejoin = CoopPause::Rejoin(&v.rejoin_host);
        v.relay_set = CoopPause::RelaySet();   // (co-op's own lock: before ours, never inside it)
        std::lock_guard lock(g_mutex);   // (the fields are the page's; no other lock of ours is held here)
        v.typing = g_page.Typing();
        v.room_field = g_page.Field(0).Shown(v.typing == 0, kRowChars - 6);       // ("Room: ")
        v.password_field = g_page.Field(1).Shown(v.typing == 1, kRowChars - 10);  // ("Password: ")
        v.room_ready = v.s.room_ready = g_page.Field(0).Ready();
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

void LiveRows(void* ctx, std::uint64_t layout, std::uint64_t gen, const SessionView& v) {
    std::lock_guard lock(g_live_mutex);
    if (layout == 0 || gen != g_live_gen) return;   // (another opening came meanwhile: its own turn sets them)
    const std::vector<std::string> rows = RowTexts(ViewShown(), v);
    for (int i = 0; i < kRows && i < kLiveSlots; ++i) {
        char path[64];
        std::snprintf(path, sizeof path, "Pa_Coop_00/Pa_Button_%02d/T_Btn_00", i);
        LiveText(ctx, layout, gen, path, i, rows[static_cast<std::size_t>(i)]);
    }
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
void ApplyPages(void* ctx, const Facts& f, const Page::Do& d) {
    const std::uint64_t layout = f.layout;
    if (layout == 0 || !g_pages_ok) return;
    if (d.set_page) {
        SetVisible(ctx, g_coop_pane, d.page_shown);
        SetVisible(ctx, g_system_pane, !d.page_shown);
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

void SetView(View v) {
    if (g_view.exchange(static_cast<int>(v)) != static_cast<int>(v)) g_confirm_until = 0;   // (a new view: no A waited)
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
    case RowAction::kJoinPaste: CoopPause::JoinPasteNow(); break;
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
        CoopPause::JoinRoomNow(room, password);   // (a refused or failed join: co-op's menu says why)
        break;
    }
    case RowAction::kRejoin: CoopPause::RejoinNow(); break;
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
                    page_only.set_page = true, page_only.page_shown = d.page_shown;
                    ApplyPages(ctx, pages, page_only);   // (the pages' own update comes a frame later)
                }
            }
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
            // (after our lock: the status and the Lobby take their own; read only when the rows are due)
            if (LiveDue(gen)) LiveRows(ctx, f.layout, gen, ViewNow(ViewShown()));
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
        int row = -1;
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
            row = decide ? FocusedRow() : -1;   // (under the lock: the pages screen is the screens' updates')
            if (g_page.TakeBack()) SetView(BackOf(ViewShown()));   // (B in a view: where its Back goes)
            if (!shown) SetView(View::kMain);
            g_page.SetSubView(ViewShown() != View::kMain);
            why = g_page.TakeWhy();
            g_mouse = g_page.MousePage();
        }
        if (shown && stick != nullptr) {
            stick[0] = 0, stick[2] = 0, stick[3] = 0;   // (only the left stick's up and down: the game's cursor over our rows)
            if (typing) stick[1] = 0;                   // (a field typed into: the cursor stays on its row)
        }
        if (!was && shown) LOG_INFO(Core, "coop: pause tab: our page shown (R on System)");
        if (was && !shown) LOG_INFO(Core, "coop: pause tab: System back ({})", why);
        if (decide) Act(row);
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
