// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_title.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "coop_hook.h"
#include "coop_i18n.h"
#include "coop_mod_screens.h"
#include "coop_mods_runtime.h"
#include "coop_pause.h"
#include "coop_pause_files.h"
#include "coop_pause_tab.h"
#include "coop_runtime.h"
#include "coop_save.h"
#include "coop_keyboard.h"
#include "coop_title_menu.h"
#include "coop_title_native.h"
#include "hos_coop_menu.h"

namespace CoopTitle {

namespace {

namespace fs = std::filesystem;
using CoopFiles::CoopIni;
using CoopFiles::Env;
using CoopFiles::FindKit;
using CoopFiles::PathOf;
using CoopFiles::ReadText;
using CoopFiles::SetEnv;
using CoopFiles::Utf8Path;
using CoopFiles::WriteText;

// --- The game (build e0f3ccfa..., the only one the module runs on) ---
// Addresses in main are OFFSETS in main (as read from main and as docs/hookshot.md names them, main+0x...), made
// guest addresses only through Main(): main is mapped at CoopRuntime::MainBase() (0x80020000); no base is written here.
std::uint64_t Main(std::uint64_t offset) {
    return CoopRuntime::MainBase() + offset;
}
constexpr std::uint64_t kTitleVtable = 0x4bf06e8;     // main+: UITitleScreen
constexpr std::uint64_t kTitleActive = 0x1a8;         // u8: 1 while its step runs its work (f_83426d70)
constexpr std::uint64_t kTitleCount = 0x2c8;          // s32: its buttons
constexpr std::uint64_t kTitleButtons = 0x2d0;        // its buttons' array
constexpr std::uint64_t kTitleFocused = 0x2e0;        // the focused button (f_83426f40)
constexpr std::uint64_t kTitleLayout = 0x28;          // the screen's layout
// A pane's text from a message, as UISystemWindowScreen sets its buttons' (f_83425bc0):
constexpr std::uint64_t kMsgRefInit = 0xa66f70;       // main+: (ref): {0, 0, -1}, 24 bytes
constexpr std::uint64_t kMsgRefSet = 0xa6afe0;        // main+: (ref, &"LayoutMsg/...", &"label", 1)
constexpr std::uint64_t kPaneText = 0xa67d30;         // main+: (layout, "pane/T_Btn_00", ref, 1, 0)
// The game's save list (player report 124; read from main, see docs/hookshot.md "The save list"): the caption
// manager, whose bytes the title's SeqQueryIsExistSave reads (+0x8e a save, +0x8f a Master Quest save, +0x88 the
// latest slot), and the body of SeqOneshotRequestLoadAndSerializeCaption, which re-reads every slot's caption from the
// mounted save folder (the boot's BeforeInitializeCommon and the way back to the title run it).
// (These four were once written as 0x80000000 + the offset, 0x20000 short of main's base: a code word read as the GOT
// and calls into the middle of other functions, so the list was never re-read. Offsets through Main() rule that out.)
constexpr std::uint64_t kCaptionMgrGot = 0x4c81660;   // main+: GOT -> the global that holds the caption manager
constexpr std::uint64_t kCaptionBusy = 0x33ad380;     // main+: (mgr) -> bool: the game's own test, still reading
constexpr std::uint64_t kCaptionReset = 0xbafd40;     // main+: () -> bool: the game's list reset (registered callback;
                                                      // to state 0 when idle and no save screen holds it)
constexpr std::uint64_t kCaptionReload = 0xf6da40;    // main+: (mgr): the re-read; starts only from state 0
constexpr std::uint64_t kCaptionState = 0x8c;         // u8: 0 cleared, 1-3 reading, 4 read
constexpr std::uint64_t kCaptionLatest = 0x88;        // s32: the latest slot, -1 none
constexpr std::uint64_t kCaptionAnySave = 0x8e;       // u8: a save exists (Continue)
constexpr std::uint64_t kCaptionAnyMq = 0x8f;         // u8: a Master Quest save exists
// The title's buttons as OpenTitle.module arranges them (CoopTitleMenu::ArrangementFor), through the functions its
// nodes end in (read from main): SeqOneShotRequestStopAnimationScreen queues a request on the screen's +0xb8, which the
// game carries out with the layout at that object's +0x28 (main+0xbad070: the animation found by name, then stopped at
// the frame if the frame is not past its last); SeqOneShotSetSLinkPropertyTitleBtnArrangementType calls the title's
// own setter. The texts go the way SetRowTexts sets the rows' (kPaneText), as SeqOneShotSetMessageScreen's do.
constexpr std::uint64_t kTitleAnimLayout = 0xe0;      // the layout the screen's requests use (+0xb8, then +0x28)
constexpr std::uint64_t kAnimFind = 0x43d870;         // main+: (layout, name) -> its animation by name, 0 none
constexpr std::uint64_t kAnimPlayer = 0x20;           // an animation's player
constexpr std::uint64_t kAnimLastFrame = 0x2d1340;    // main+: (player) -> s32: its last frame
constexpr std::uint64_t kAnimStopAt = 0x2d2d90;       // main+: (animation, s0 frame): stopped at that frame
constexpr std::uint64_t kTitleSoundType = 0x3407000;  // main+: (UITitleScreen, type): its sound-link arrangement
// The title's own co-op rows (tools/puppet_pack/title_menu.py adds them to Title_00 and their
// texts to LayoutMsg/Title_00): Pa_Button_04 Host, _05 Join, _06 Co-op Settings. Each opens
// Hookshot's menu on the Multiplayer tab at that page (coop_pause.h).
constexpr int kFirstRow = 4;
constexpr std::array<const char*, 3> kRowLabels{"0100", "0101", "0102"};
constexpr std::array<CoopPauseMenu::Page, 3> kRowPages{CoopPauseMenu::Page::kHost, CoopPauseMenu::Page::kJoin,
                                                       CoopPauseMenu::Page::kSettings};

// The title's own menu (CoopTitleMenu::MenuFor): the marker pane found by name the way the pause tab finds its panes
// (main+0x43c040(layout + 0x40) -> the layout's pane table, main+0x43e300(table, "name") -> the pane; read from main,
// 2026-11-18), the focus given as the title's set-up gives it to its first button (f_8088ffc0(screen, button, 0),
// docs/game-ui.md "Buttons and focus"), a button chosen without input (Button::Decide, CoopModScreens::kButtonDecide)
// and the screen's own menu sound by name (CoopModScreens::kScreenSound: the title's "button_decide").
constexpr std::uint64_t kPaneTable = 0x43c040;
constexpr std::uint64_t kPaneByName = 0x43e300;
constexpr std::uint64_t kSetFocus = 0x86ffc0;
using CoopModScreens::kButtonDecide;
using CoopModScreens::kScreenSound;
constexpr std::uint64_t kApplyForMs = 1500;   // the look set again each 250 ms this long after it changes (the game's
                                              // OpenTitle sets its own texts after the set-up)

// --- Switch pad bits (nn::hid::NpadButton) ---
constexpr std::uint64_t kA = 1ull << 0;
constexpr std::uint64_t kB = 1ull << 1;
constexpr std::uint64_t kY = 1ull << 3;   // (nn::hid: X is bit 2, Y bit 3)
constexpr std::uint64_t kMinus = 1ull << 11;

const HosCoopApi* g_api = nullptr;
bool g_native = false;   // LICHII_COOP_TITLE_NATIVE=1: the 4th title button (Pa_Button_03) opens the tab too

std::atomic<std::uint64_t> g_screen{0};
std::atomic<std::uint64_t> g_last_step_ms{0};
std::atomic<bool> g_step_seen{false};
std::atomic<bool> g_rows{false};

// Everything below. Never held while calling into CoopPause (it calls in here: StartSaveWait, StopSaveWait).
CoopTitleMenu::SaveListRefresh g_list;   // the title's step only (one thread)
std::atomic<bool> g_mod_asked{false};    // a code mod asked for a re-read (AskSaveListRead), not taken yet
std::atomic<bool> g_list_open{false};    // a re-read is asked or running (a mod's or co-op's): not ready
std::mutex g_mutex;
CoopTitleMenu::SaveWait g_wait;   // Join at the title -> what the title says while the host's save comes
CoopTitleMenu::PlayerChoice g_choice;   // the player's own A on the title, before the mods' holds (NotePlayerPad)
std::uint64_t g_pad_last = 0;
int g_focus_logged = -2;
// The title's own menu (a pack with CoopTitleMenu::kMenuMarker). g_mutex, but g_menu.
std::atomic<bool> g_menu{false};   // this title has the menu's labels (the marker found at its set-up)
// Co-op's menu page on the title's own buttons (coop_title_native.h): the page opened (-1: none), the first row shown,
// the title's cursor to be put on the selected row at the next step, the on-screen keyboard's cursor for a field.
std::atomic<int> g_native_page{-1};
int g_native_top = 0;                 // (g_mutex)
bool g_native_refocus = false;        // (g_mutex)
CoopKeyboard::Cursor g_native_keys;   // (g_mutex)
bool g_title_mouse_held = false;     // (the left button: OnPad)
float g_title_mouse_x = -1, g_title_mouse_y = -1;
std::atomic<std::uint64_t> g_title_mouse_move_ms{0}, g_title_pad_ms{0};
int g_edge_logged = 0;               // (trace: the edge test logged so far)
int g_hover_edge = -1;               // (the page's first or last row under the mouse, and since when: its scroll)
std::uint64_t g_hover_since = 0;
std::atomic<std::uint64_t> g_title_mouse_down_ms{0};
// A press within this long of the left button going down is the mouse's (Longshot's A for a click)
constexpr std::uint64_t kMouseClickMs = 500;
std::uint64_t NowMs();
bool MouseClicked() { return NowMs() - g_title_mouse_down_ms.load() < kMouseClickMs; }
int g_kb_logged = 0;                  // (trace: the keyboard's mouse presses logged so far)
bool g_kb_held = false;               // (the keyboard by mouse: the left button, the key it went down on, the last click)
int g_kb_press = -1;
std::uint64_t g_kb_click_ms = 0;
// The live texts' slots (the pack's Coop_Live_T0..T3 in LayoutMsg/Title_00): the message's text in the game's memory,
// the bytes the game counts past it, what it says now. Found again on a new title layout.
struct LiveSlot {
    std::uint64_t layout = 0, text = 0;
    std::uint32_t extra = 0;
    std::string shown;
};
std::array<LiveSlot, CoopTitleNative::kButtons> g_live{};   // (the title's step only)
std::array<std::uint64_t, CoopTitleNative::kButtons> g_live_known{};   // (a placeholder's text once found: ours since)
std::array<std::uint32_t, CoopTitleNative::kButtons> g_live_extra{};   // (the bytes the game counts past its text)
constexpr std::uint32_t kLivePlaceholder = 64;
std::atomic<bool> g_sub{false};    // the Multiplayer submenu shows (atomic: RowsOurs reads it without the lock)
bool g_any_save = true;            // the look's save list: a save or not (the caption manager's)
bool g_one_start = false;          // one Start row for Continue and New Game (a file select mod runs)
std::uint64_t g_apply_until = 0, g_apply_next = 0;   // the look set again until then (kApplyForMs)
int g_focus_to = -1;               // a button to give the focus at the next step
bool g_click_remapped = false;     // ClickedButton took a click's A for the button under the mouse (g_mutex)
int g_decide = -1;                 // a game button to decide at the next step
std::atomic<std::uint64_t> g_title_view_ms{0};   // when the title's rows or a page's view last changed
std::atomic<bool> g_close_native_asked{false};   // the auto-load closed co-op's page: CoopPause told outside the lock
int g_native_seen = -1;                          // the page and view NativePad saw last
constexpr std::uint64_t kTitleClickThroughMs = 450;
// The A of the click that changed the rows or the page, or of a double click's second press: the button went down
// before the change or within 300 ms after it, and the A comes within kTitleClickThroughMs. A new click made after
// looking at the new rows is a press of its own (a quick one, half a second on, was lost, a test run 2026-11-20).
bool ClickThrough() {
    const std::uint64_t view = g_title_view_ms.load(), down = g_title_mouse_down_ms.load(), now = NowMs();
    return now - view < kTitleClickThroughMs && now - down < 350 && (down < view || down - view < 300);
}
std::atomic<std::uint64_t> g_auto_load_until{0};   // LoadHostSaveOnTitle: Continue chosen once the host's save is in
std::atomic<bool> g_auto_load_test{false};          // (a test's: no session to wait for)
std::atomic<std::uint64_t> g_auto_press_ms{0};      // the presses that load it began (0: none)
// (the game's Continue, then its save list's newest save (the host's, the co-op folder's only one), then Load: an A
// each, at these times after the focus went to Continue; the player's buttons wait meanwhile)
constexpr std::array<std::uint64_t, 4> kAutoPressAt{400, 2900, 5400, 7900};   // (the last again: Load came late once)
constexpr std::uint64_t kAutoPressHold = 100, kAutoPressEnd = 9000;
std::uint64_t g_title_since = 0;   // when the title's menu came up (0: not up)
const char* g_sound = nullptr;     // a sound to play at the next step
std::uint64_t g_swallow = 0;       // pad bits kept from the game until let go (the rows the module answers)

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t Read64(std::uint64_t a) {
    std::uint64_t v = 0;
    return CoopRuntime::Read(a, &v, sizeof v) ? v : 0;
}

bool HasScratch() {
    return g_api != nullptr && g_api->size >= offsetof(HosCoopApi, scratch) + sizeof(g_api->scratch) &&
           g_api->scratch != nullptr;
}

// The title screen, while it is up and showing its buttons: the object is
// still a UITitleScreen, its step's flag is on, and (once its step has been
// seen) a step came in the last 300 ms.
bool TitleUp() {
    const std::uint64_t s = g_screen.load();
    if (s == 0 || Read64(s) != Main(kTitleVtable)) {
        return false;
    }
    std::uint8_t active = 0;
    if (!CoopRuntime::Read(s + kTitleActive, &active, 1) || active != 1) {
        return false;
    }
    return !g_step_seen.load() || NowMs() - g_last_step_ms.load() < 300;
}

void LogButtons(std::uint64_t screen) {
    std::int32_t count = 0;
    CoopRuntime::Read(screen + kTitleCount, &count, sizeof count);
    const std::uint64_t buttons = Read64(screen + kTitleButtons);
    std::string line = "coop: title menu: the title screen " + fmt::format("{:#x}", screen) + ", " +
                       std::to_string(count) + " buttons:";
    for (std::int32_t i = 0; i < count && i < 8; ++i) {
        const std::uint64_t b = Read64(buttons + 8ull * static_cast<std::uint64_t>(i));
        std::uint32_t flags = 0;
        CoopRuntime::Read(b + 0x40, &flags, sizeof flags);
        line += fmt::format(" [{}] {:#x} flags {:#x}", i, b, flags);
    }
    LOG_INFO(Core, "{}", line);
    static bool dumped = false;   // (the first time only: what the 4th button's state looks like, for the native button)
    if (dumped || count < 1) {
        return;
    }
    dumped = true;
    for (std::int32_t i = 0; i < count && i < 4; ++i) {
        const std::uint64_t b = Read64(buttons + 8ull * static_cast<std::uint64_t>(i));
        std::array<std::uint8_t, 0x80> bytes{};
        if (!CoopRuntime::Read(b, bytes.data(), bytes.size())) {
            continue;
        }
        std::string hex;
        for (std::size_t k = 0; k < bytes.size(); ++k) {
            hex += fmt::format("{:02x}{}", bytes[k], (k % 8 == 7) ? " " : "");
        }
        LOG_INFO(Core, "coop: title menu: button {} at {:#x}: {}", i, b, hex);
        const std::uint64_t pane = Read64(b + 0x10);
        if (pane != 0 && CoopRuntime::Read(pane, bytes.data(), bytes.size())) {
            hex.clear();
            for (std::size_t k = 0; k < bytes.size(); ++k) {
                hex += fmt::format("{:02x}{}", bytes[k], (k % 8 == 7) ? " " : "");
            }
            LOG_INFO(Core, "coop: title menu: button {} +0x10 -> {:#x}: {}", i, pane, hex);
        }
    }
}

// The focused button's number (LICHII_COOP_TITLE_NATIVE and the log), -1 when none.
int FocusedIndex(std::uint64_t screen) {
    std::int32_t count = 0;
    CoopRuntime::Read(screen + kTitleCount, &count, sizeof count);
    const std::uint64_t buttons = Read64(screen + kTitleButtons), focused = Read64(screen + kTitleFocused);
    for (std::int32_t i = 0; i < count && i < 8; ++i) {
        if (focused != 0 && Read64(buttons + 8ull * static_cast<std::uint64_t>(i)) == focused) {
            return i;
        }
    }
    return -1;
}

std::int32_t ButtonCount(std::uint64_t screen) {
    std::int32_t count = 0;
    CoopRuntime::Read(screen + kTitleCount, &count, sizeof count);
    return count;
}

// A button's text from the game's message files (LayoutMsg/Title_00), set the way the game sets its own buttons'
// (kPaneText; SeqOneShotSetMessageScreen's way). It needs the strings and the message ref in the game's memory: one
// scratch block (patch 0004) for its calls.
bool SetPaneText(void* ctx, std::uint64_t layout, const std::string& pane, const std::string& label) {
    // [0x00] the ref, [0x20] &path, [0x28] &label, [0x30] path, [0x50] label, [0x60] pane
    std::array<std::uint8_t, 0x90> b{};
    const std::uint64_t base = g_api->scratch(ctx, b.data(), b.size());   // (where it goes: the same each time)
    if (base == 0) {
        return false;
    }
    const std::string path = "LayoutMsg/Title_00";
    const std::uint64_t at_path = base + 0x30, at_label = base + 0x50, at_pane = base + 0x60;
    std::memcpy(b.data() + 0x20, &at_path, 8);
    std::memcpy(b.data() + 0x28, &at_label, 8);
    std::memcpy(b.data() + 0x30, path.c_str(), path.size() + 1);
    std::memcpy(b.data() + 0x50, label.c_str(), label.size() + 1);
    std::memcpy(b.data() + 0x60, pane.c_str(), pane.size() + 1);
    if (g_api->scratch(ctx, b.data(), b.size()) != base) {
        return false;
    }
    std::uint64_t x0 = 0;
    HosCoopCall k{};
    k.function = Main(kMsgRefInit);
    k.x[0] = base;
    if (!g_api->call(ctx, &k, &x0)) {
        return false;
    }
    k = HosCoopCall{};
    k.function = Main(kMsgRefSet);
    k.x[0] = base, k.x[1] = base + 0x20, k.x[2] = base + 0x28, k.x[3] = 1;
    if (!g_api->call(ctx, &k, &x0)) {
        return false;
    }
    k = HosCoopCall{};
    k.function = Main(kPaneText);
    k.x[0] = layout, k.x[1] = at_pane, k.x[2] = base, k.x[3] = 1, k.x[4] = 0;
    return g_api->call(ctx, &k, &x0);
}

// A title button's live text (co-op's menu on the title's buttons): the pack's placeholder Coop_Live_T<slot>
// (LayoutMsg/Title_00) written over in the game's memory with `utf8`, then set on the button's text pane as the game
// sets its own (SetPaneText). False when the pack has no placeholder (an older pack) or a call failed.
bool SetLiveText(void* ctx, std::uint64_t layout, int slot, std::string utf8) {
    if (slot < 0 || slot >= CoopTitleNative::kButtons || !HasScratch()) return false;
    LiveSlot& l = g_live[static_cast<std::size_t>(slot)];
    if (l.layout != layout) l = LiveSlot{}, l.layout = layout;
    if (utf8.size() >= kLivePlaceholder) {
        std::size_t k = kLivePlaceholder - 1;
        while (k > 0 && (static_cast<unsigned char>(utf8[k]) & 0xc0) == 0x80) --k;
        utf8.resize(k);
    }
    if (utf8.empty()) utf8 = " ";
    if (l.text != 0 && l.shown == utf8) return true;
    // [0x00] the ref, [0x20] &path, [0x28] &label, [0x30] path, [0x50] label, [0x60] pane
    std::array<std::uint8_t, 0x90> b{};
    const std::uint64_t base = g_api->scratch(ctx, b.data(), b.size());
    if (base == 0) return false;
    const std::string path = "LayoutMsg/Title_00";
    const std::string label = fmt::format("Coop_Live_T{}", slot);
    const std::string pane = fmt::format("Pa_Button_{:02d}/T_Btn_00", slot);
    const std::uint64_t at_path = base + 0x30, at_label = base + 0x50, at_pane = base + 0x60;
    std::memcpy(b.data() + 0x20, &at_path, 8);
    std::memcpy(b.data() + 0x28, &at_label, 8);
    std::memcpy(b.data() + 0x30, path.c_str(), path.size() + 1);
    std::memcpy(b.data() + 0x50, label.c_str(), label.size() + 1);
    std::memcpy(b.data() + 0x60, pane.c_str(), pane.size() + 1);
    if (g_api->scratch(ctx, b.data(), b.size()) != base) return false;
    std::uint64_t x0 = 0;
    HosCoopCall k{};
    k.function = Main(kMsgRefInit);
    k.x[0] = base;
    if (!g_api->call(ctx, &k, &x0)) return false;
    k = HosCoopCall{};
    k.function = Main(kMsgRefSet);
    k.x[0] = base, k.x[1] = base + 0x20, k.x[2] = base + 0x28, k.x[3] = 1;
    if (!g_api->call(ctx, &k, &x0)) return false;
    const std::uint64_t text = Read64(base);
    std::uint32_t length = 0;
    CoopRuntime::Read(base + 8, &length, sizeof length);
    char head[8] = {};
    CoopRuntime::Read(text, head, sizeof head);
    std::uint64_t& known = g_live_known[static_cast<std::size_t>(slot)];
    const bool dots = std::memcmp(head, "........", 8) == 0;
    if (text == 0 || (!dots && text != known)) {
        static bool said = false;
        if (!std::exchange(said, true)) {
            LOG_INFO(Core, "coop: title: live text: {} is not the pack's placeholder: co-op's pages open in the overlay "
                           "(build the puppet pack again)", label);
        }
        return false;
    }
    if (dots) g_live_extra[static_cast<std::size_t>(slot)] = length >= kLivePlaceholder ? length - kLivePlaceholder : 0;
    l.extra = g_live_extra[static_cast<std::size_t>(slot)];
    known = text;
    l.text = text;
    std::string bytes = utf8;
    bytes.push_back('\0');
    if (!CoopRuntime::Write(text, bytes.data(), bytes.size())) return false;
    const std::uint32_t now = static_cast<std::uint32_t>(utf8.size()) + l.extra;
    if (!CoopRuntime::Write(base + 8, &now, sizeof now)) return false;
    k = HosCoopCall{};
    k.function = Main(kPaneText);
    k.x[0] = layout, k.x[1] = at_pane, k.x[2] = base, k.x[3] = 1, k.x[4] = 0;
    if (!g_api->call(ctx, &k, &x0)) return false;
    l.shown = utf8;
    return true;
}

// The rows' texts (labels 0100..0102). True when all three were set.
bool SetRowTexts(void* ctx, std::uint64_t screen) {
    const std::uint64_t layout = Read64(screen + kTitleLayout);
    if (ctx == nullptr || layout == 0 || !HasScratch()) {
        return false;
    }
    for (std::size_t i = 0; i < kRowLabels.size(); ++i) {
        const std::string pane = fmt::format("Pa_Button_{:02d}/T_Btn_00", kFirstRow + static_cast<int>(i));
        if (!SetPaneText(ctx, layout, pane, kRowLabels[i])) {
            return false;
        }
    }
    return true;
}


// The caption manager (0: not made yet).
std::uint64_t CaptionMgr() {
    const std::uint64_t global = Read64(Main(kCaptionMgrGot));
    return global != 0 ? Read64(global) : 0;
}

std::string CaptionLine(std::uint64_t mgr) {
    std::uint8_t any = 0, mq = 0;
    std::int32_t latest = -1;
    CoopRuntime::Read(mgr + kCaptionAnySave, &any, 1);
    CoopRuntime::Read(mgr + kCaptionAnyMq, &mq, 1);
    CoopRuntime::Read(mgr + kCaptionLatest, &latest, sizeof latest);
    return fmt::format("a save {}, a Master Quest save {}, latest slot {}", any != 0 ? "yes" : "no",
                       mq != 0 ? "yes" : "no", latest);
}

// One game call from the title's step: x0 back (nothing when it could not be made).
std::optional<std::uint64_t> GameCall(void* ctx, std::uint64_t function, std::uint64_t x0_in) {
    HosCoopCall k{};
    k.function = function;
    k.x[0] = x0_in;
    std::uint64_t x0 = 0;
    if (!g_api->call(ctx, &k, &x0)) return std::nullopt;
    return x0;
}

// After the game's re-read (SaveListRefresh's done, so only after a switch of the save folder): the open title gets
// the buttons its own OpenTitle.module gives that list (CoopTitleMenu::ArrangementFor), through the same functions.
// nullptr when done, else why not (nothing is changed then: the title keeps its buttons until it opens again, as
// before). The module writes nothing of the title or the list itself.
const char* ArrangeTitle(void* ctx, std::uint64_t mgr) {
    const std::uint64_t screen = g_screen.load();
    if (!HasScratch()) return "this Hookshot has no patch 0004 scratch";
    if (screen == 0 || Read64(screen) != Main(kTitleVtable)) return "no title screen";
    const std::uint64_t layout = Read64(screen + kTitleLayout);
    if (const std::uint64_t used = Read64(screen + kTitleAnimLayout); layout == 0 || used != layout) {
        LOG_INFO(Core, "coop: title: save list: the title's layout {:#x}, its requests' {:#x}", layout, used);
        return "the title's layout is not the one its requests use";   // (not the screen read from main)
    }
    std::uint8_t any = 0;
    if (!CoopRuntime::Read(mgr + kCaptionAnySave, &any, 1)) return "the list could not be read";
    const CoopTitleMenu::TitleArrangement a = CoopTitleMenu::ArrangementFor(any != 0);
    static constexpr char kBtnNum[] = "BtnNum";
    const std::uint64_t name = g_api->scratch(ctx, kBtnNum, sizeof kBtnNum);
    if (name == 0) return "no scratch";
    HosCoopCall k{};
    k.function = Main(kAnimFind);
    k.x[0] = layout, k.x[1] = name;
    std::uint64_t anim = 0;
    if (!g_api->call(ctx, &k, &anim) || anim == 0) return "the title has no BtnNum animation";
    const std::uint64_t player = Read64(anim + kAnimPlayer);
    if (player == 0) return "BtnNum has no player";
    const auto last = GameCall(ctx, Main(kAnimLastFrame), player);
    if (!last) return "BtnNum's last frame could not be asked";
    if (a.btn_num_frame > static_cast<float>(static_cast<std::int32_t>(*last))) {
        return "BtnNum ends before the frame";   // (the game's own request skips it then too)
    }
    k = HosCoopCall{};
    k.function = Main(kAnimStopAt);
    k.x[0] = anim;
    k.s0 = a.btn_num_frame;
    std::uint64_t x0 = 0;
    if (!g_api->call(ctx, &k, &x0)) return "BtnNum could not be stopped at the frame";
    // Then the texts, as the graph's branch does after its stop.
    for (std::size_t i = 0; i < a.count; ++i) {
        if (!SetPaneText(ctx, layout, a.texts[i].pane, a.texts[i].label)) return "a button's text could not be set";
    }
    k = HosCoopCall{};
    k.function = Main(kTitleSoundType);
    k.x[0] = screen, k.x[1] = static_cast<std::uint32_t>(a.sound_type);
    if (!g_api->call(ctx, &k, &x0)) return "the title's sound arrangement could not be set";
    return nullptr;
}

// A game call with up to three arguments from the title's step: x0 back (nothing when it could not be made).
std::optional<std::uint64_t> GameCall3(void* ctx, std::uint64_t function, std::uint64_t x0_in, std::uint64_t x1,
                                       std::uint64_t x2 = 0) {
    HosCoopCall k{};
    k.function = function;
    k.x[0] = x0_in, k.x[1] = x1, k.x[2] = x2;
    std::uint64_t x0 = 0;
    if (!g_api->call(ctx, &k, &x0)) return std::nullopt;
    return x0;
}

// A pane of the title's layout by name (0: none).
std::uint64_t TitlePane(void* ctx, std::uint64_t layout, const char* name) {
    const auto table = GameCall3(ctx, Main(kPaneTable), layout + 0x40, 0);
    const std::uint64_t at = g_api->scratch(ctx, name, std::strlen(name) + 1);
    if (!table || *table == 0 || at == 0) return 0;
    return GameCall3(ctx, Main(kPaneByName), *table, at).value_or(0);
}

// The title's button `i` (0: none).
std::uint64_t TitleButton(std::uint64_t screen, int i) {
    if (i < 0 || i >= ButtonCount(screen)) return 0;
    return Read64(Read64(screen + kTitleButtons) + 8ull * static_cast<std::uint64_t>(i));
}

// The title's button under the mouse (its box as Longshot's mouse widens it: 1100 across, to halfway to the next row),
// -1 none. For a click's A: the focus our calls gave a button (a page left, B) is not Longshot's own, which then took
// a click on the button it last hovered as no move and left the focus where ours put it: the A went to that button (a
// click on Back opened Join, a test run 2026-11-20).
int ButtonUnderMouse() {
    float x = 0, y = 0;
    bool held = false;
    if (!CoopPauseTab::LayoutCursor(&x, &y, &held)) return -1;
    const auto read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
    const std::uint64_t screen = g_screen.load();
    int best = -1;
    float best_dy = 1e9f;
    for (int i = 0; i < CoopTitleNative::kButtons; ++i) {
        const std::uint64_t b = TitleButton(screen, i);
        const auto box = b != 0 ? CoopModScreens::HitBox(read, b) : std::nullopt;
        if (!box) continue;
        const float cx = (box->x0 + box->x1) / 2, cy = (box->y0 + box->y1) / 2;
        const float dy = std::abs(y - cy);
        if (std::abs(x - cx) <= 550 && dy < best_dy) best = i, best_dy = dy;
    }
    return best_dy <= 70 ? best : -1;   // (rows about 120 apart: halfway, with a little room)
}

// The button an A is for: a click's, the one under the mouse (its focus given to it); else the focused one.
int ClickedButton(bool a_down, int focused) {
    if (!a_down || !MouseClicked()) return focused;
    const int under = ButtonUnderMouse();
    static int traced = 0;
    if (++traced <= 40) LOG_INFO(Core, "coop: title: trace: a click's A: under the mouse {}, focused {}", under, focused);
    if (under < 0 || under == focused) return focused;
    g_focus_to = under;
    g_click_remapped = true;
    LOG_INFO(Core, "coop: title: a click on button {} with the focus on button {}: the click's button", under, focused);
    return under;
}

// BtnNum stopped at `frame`: that many of the game's buttons shown (OpenTitle's own way, ArrangeTitle). nullptr when
// done, else why not.
const char* StopBtnNum(void* ctx, std::uint64_t layout, float frame) {
    static constexpr char kBtnNum[] = "BtnNum";
    const std::uint64_t name = g_api->scratch(ctx, kBtnNum, sizeof kBtnNum);
    if (name == 0) return "no scratch";
    const auto anim = GameCall3(ctx, Main(kAnimFind), layout, name);
    if (!anim || *anim == 0) return "the title has no BtnNum animation";
    const std::uint64_t player = Read64(*anim + kAnimPlayer);
    if (player == 0) return "BtnNum has no player";
    const auto last = GameCall(ctx, Main(kAnimLastFrame), player);
    if (!last) return "BtnNum's last frame could not be asked";
    if (frame > static_cast<float>(static_cast<std::int32_t>(*last))) return "BtnNum ends before the frame";
    HosCoopCall k{};
    k.function = Main(kAnimStopAt);
    k.x[0] = *anim;
    k.s0 = frame;
    std::uint64_t x0 = 0;
    return g_api->call(ctx, &k, &x0) ? nullptr : "BtnNum could not be stopped at the frame";
}

// The title's own menu as it is now (MenuFor): the game's buttons shown, their texts, and what the step was asked to do
// (the focus moved, a button decided, a sound). From the title's step.
// Co-op's menu page on the title's own buttons (coop_title_native.h), at each step of the title: the rows' live texts,
// the title's cursor on the selected row after a press, the keyboard for a field typed into. An older pack (no
// placeholder): the overlay instead, as before.
void StepNative(void* ctx, std::uint64_t screen) {
    const CoopPauseMenu::View v = CoopPause::NativeView();
    const std::uint64_t layout = Read64(screen + kTitleLayout);
    if (layout == 0) return;
    CoopTitleNative::Look look;
    bool refocus = false;
    const char* sound = nullptr;
    CoopKeyboard::Shown kb;
    float frame = 4;
    bool apply = false;
    {
        std::lock_guard lock(g_mutex);
        look = CoopTitleNative::LookFor(v, g_native_top);
        g_native_top = look.top;
        refocus = std::exchange(g_native_refocus, false);
        // (the menu's selected row changed by itself or by a key, typing ended by Enter (Join next), a pick: the title's
        // cursor goes with it, unless the mouse leads (its hover is the selection then). Left on Password after Enter,
        // the next A typed the password again, a test run 2026-11-20)
        static int last_selected = -1, last_typing = 0;
        const int typing_now = v.typing ? 1 : 0;
        const bool mouse_leads = g_title_mouse_move_ms.load() > g_title_pad_ms.load() &&
                                 NowMs() - g_title_mouse_move_ms.load() < 800;
        if ((v.selected != last_selected || typing_now != last_typing) && !v.typing && !mouse_leads) refocus = true;
        last_selected = v.selected, last_typing = typing_now;
        std::swap(sound, g_sound);
        if (v.typing && v.selected >= 0 && static_cast<std::size_t>(v.selected) < v.rows.size()) {
            kb.open = true;
            kb.title = v.rows[static_cast<std::size_t>(v.selected)].name;
            kb.text = v.rows[static_cast<std::size_t>(v.selected)].value;
            kb.row = g_native_keys.row(), kb.col = g_native_keys.col();
        }
        frame = static_cast<float>(look.buttons);   // (the rows and Back: no blank buttons between them)
        const std::uint64_t now = NowMs();
        if (now >= g_apply_next) apply = true, g_apply_next = now + 250;
    }
    CoopKeyboard::Publish(kb);
    if (apply) StopBtnNum(ctx, layout, frame);
    bool ok = true;
    for (int i = 0; i < CoopTitleNative::kButtons; ++i) {
        ok = SetLiveText(ctx, layout, i, look.texts[static_cast<std::size_t>(i)]) && ok;
    }
    if (!ok) {   // (an older pack: the page in the overlay, as before)
        const int page = g_native_page.exchange(-1);
        CoopPause::CloseNative();
        CoopKeyboard::Publish(CoopKeyboard::Shown{});
        if (page >= 0) CoopPause::Open(static_cast<CoopPauseMenu::Page>(page));
        std::lock_guard lock(g_mutex);
        g_apply_until = NowMs() + kApplyForMs, g_apply_next = 0;
        return;
    }
    if (refocus) {
        if (const std::uint64_t button = TitleButton(screen, look.focus); button != 0) {
            GameCall3(ctx, Main(kSetFocus), screen, button, 0);
        }
    }
    if (sound != nullptr) {
        if (const std::uint64_t at = g_api->scratch(ctx, sound, std::strlen(sound) + 1); at != 0) {
            GameCall3(ctx, Main(kScreenSound), screen, at);
        }
    }
}

void StepMenu(void* ctx, std::uint64_t screen) {
    if (!g_menu.load() || ctx == nullptr || !HasScratch()) return;
    if (g_native_page.load() >= 0) {
        StepNative(ctx, screen);
        return;
    }
    const std::uint64_t now = NowMs();
    bool apply = false, any_save = true, sub = false;
    int focus = -1, decide = -1;
    const char* sound = nullptr;
    bool one_start = CoopModsRuntime::FileSelectRuns();   // (before g_mutex: the mods' calls take it, coop_busy)
    {
        std::lock_guard lock(g_mutex);
        if (one_start != g_one_start) {
            g_one_start = one_start;   // (a file select mod started or stopped under the open title)
            g_apply_until = now + kApplyForMs, g_apply_next = 0;
        }
        if (const std::uint64_t mgr = CaptionMgr(); mgr != 0) {
            std::uint8_t any = 1;
            CoopRuntime::Read(mgr + kCaptionAnySave, &any, 1);
            if ((any != 0) != g_any_save) {
                g_any_save = any != 0;
                g_apply_until = now + kApplyForMs, g_apply_next = 0;   // (the list changed under the open title)
            }
        }
        if (now < g_apply_until && now >= g_apply_next) apply = true, g_apply_next = now + 250;
        any_save = g_any_save, sub = g_sub.load();
        std::swap(focus, g_focus_to);
        std::swap(decide, g_decide);
        std::swap(sound, g_sound);
    }
    const CoopTitleMenu::TitleMenuLook look = CoopTitleMenu::MenuFor(any_save, sub, one_start);
    const std::uint64_t layout = Read64(screen + kTitleLayout);
    if (apply && layout != 0) {
        const char* why = StopBtnNum(ctx, layout, look.btn_num_frame);
        int texts = 0;
        for (std::size_t i = 0; i < look.count; ++i) {
            texts += SetPaneText(ctx, layout, fmt::format("Pa_Button_{:02d}/T_Btn_00", i), look.rows[i].label) ? 1 : 0;
        }
        static std::string logged;
        const std::string line = fmt::format("{} rows{}{}, {} of {} texts{}", sub ? "Multiplayer's" : "the title's",
                                             any_save ? "" : " (no save yet)",
                                             !sub && one_start ? " (one Start: a file select mod runs)" : "",
                                             texts, look.count,
                                             why != nullptr ? fmt::format("; {}", why) : std::string());
        if (line != logged) {
            logged = line;
            LOG_INFO(Core, "coop: title: menu: {}", line);
        }
    }
    if (focus >= 0) {
        const std::uint64_t button = TitleButton(screen, focus);
        const auto r = button != 0 ? GameCall3(ctx, Main(kSetFocus), screen, button, 0) : std::nullopt;
        LOG_INFO(Core, "coop: title: menu: the focus to button {}{}", focus, r ? "" : " (not made)");
    }
    if (decide >= 0) {
        const std::uint64_t button = TitleButton(screen, decide);
        const auto r = button != 0 ? GameCall3(ctx, Main(kButtonDecide), button, 0) : std::nullopt;
        LOG_INFO(Core, "coop: title: menu: the game's button {} chosen in the row's place{}", decide,
                 r && (*r & 1) != 0 ? "" : " (it did not take it)");
    }
    if (sound != nullptr) {
        if (const std::uint64_t at = g_api->scratch(ctx, sound, std::strlen(sound) + 1); at != 0) {
            GameCall3(ctx, Main(kScreenSound), screen, at);
        }
    }
}

// From the title's step (its ctx may call()): after a switch of the save folder (CoopSave::TakeSaveListStale) the game
// re-reads its save list in its own order: its idle test, its list reset, then its read
// (SeqOneshotRequestLoadAndSerializeCaption's body). Nothing of the list is written here: the game fills it.
void RefreshSaveList(void* ctx) {
    if (g_api == nullptr || g_api->call == nullptr || ctx == nullptr) {
        return;   // (the switch stays marked: taken at a step that may call)
    }
    using R = CoopTitleMenu::SaveListRefresh;
    const std::uint64_t mgr = CaptionMgr();
    std::uint8_t state = 0;
    const bool found = mgr != 0 && CoopRuntime::Read(mgr + kCaptionState, &state, 1);
    R::Input in;
    in.stale = CoopSave::TakeSaveListStale();
    in.stale = g_mod_asked.exchange(false) || in.stale;
    if (in.stale) {
        g_list_open = true;
    }
    in.title = true;
    in.manager = found;
    in.state = state;
    in.now_ms = NowMs();
    if (found && (g_list.pending() || g_list.asked() || in.stale)) {
        const auto busy = GameCall(ctx, Main(kCaptionBusy), mgr);
        in.busy = !busy || (*busy & 1) != 0;
    }
    const R::Wait before = g_list.waiting();
    const R::Action action = g_list.Step(in);
    if (const R::Wait now = g_list.waiting(); now != before && now != R::Wait::none) {
        LOG_INFO(Core, "coop: title: save list: the save folder changed: waiting ({}; state {})",
                 now == R::Wait::manager ? "no caption manager yet" : now == R::Wait::busy ? "the game is reading it"
                                                                                          : "not on the title",
                 state);
    }
    switch (action) {
    case R::Action::none:
        return;
    case R::Action::ask: {
        LOG_INFO(Core, "coop: title: save list: the save folder changed: the game re-reads its save list (before: {}; "
                       "state {})", CaptionLine(mgr), state);
        const auto reset = GameCall(ctx, Main(kCaptionReset), 0);
        if (!reset || (*reset & 1) == 0) {
            g_list.Refused();
            LOG_INFO(Core, "coop: title: save list: the game's list reset said not now ({}): asked again",
                     reset ? "a save screen holds it" : "the call could not be made");
            return;
        }
        std::uint8_t cleared = 0xff;
        CoopRuntime::Read(mgr + kCaptionState, &cleared, 1);
        if (!GameCall(ctx, Main(kCaptionReload), mgr)) {
            LOG_WARNING(Core, "coop: title: save list: the game's re-read could not be called: Continue shows after a "
                              "restart");
            return;
        }
        std::uint8_t reading = 0xff;
        CoopRuntime::Read(mgr + kCaptionState, &reading, 1);
        LOG_INFO(Core, "coop: title: save list: reset (state {}), read started (state {})", cleared, reading);
        return;
    }
    case R::Action::done:
        g_list_open = false;
        LOG_INFO(Core, "coop: title: save list: read again by the game: {}", CaptionLine(mgr));
        if (g_menu.load()) {
            std::lock_guard lock(g_mutex);   // (the title's own menu: its look, for the new list, at the next steps)
            g_apply_until = NowMs() + kApplyForMs, g_apply_next = 0;
        }
        if (const char* why = ArrangeTitle(ctx, mgr)) {
            LOG_WARNING(Core, "coop: title: save list: the open title keeps its buttons ({}): the new list shows when "
                              "the title opens again", why);
        } else {
            std::uint8_t any = 0;
            CoopRuntime::Read(mgr + kCaptionAnySave, &any, 1);
            LOG_INFO(Core, "coop: title: save list: the open title's buttons arranged as its OpenTitle does for this "
                           "list: {}", any != 0 ? "Continue, New Game, Options" : "New Game, Options");
        }
        return;
    case R::Action::gave_up:
        g_list_open = false;
        LOG_WARNING(Core, "coop: title: save list: not read again after {} s (state {}; {})", R::kGiveUpMs / 1000,
                    state, CaptionLine(mgr));
        return;
    }
}

// Link is in the game: this game's own row has a scene (CoopHook::PlayerList).
bool InGame() {
    if (TitleUp()) return false;   // (the title's own scene has a Link: not a file played)
    for (const auto& row : CoopHook::PlayerList()) {
        if (row.self) {
            return row.scene.has_value() && !row.scene->empty();
        }
    }
    return false;
}

}  // namespace

void Init(const HosCoopApi* api) {
    g_api = api;
    g_native = Env("LICHII_COOP_TITLE_NATIVE") == "1";
    LOG_INFO(Core, "coop: title: {}{}",
             HasScratch() ? "the Multiplayer rows open the menu's Multiplayer tab"
                          : "this Hookshot has no patch 0004 (build it again): co-op is in the menu's Multiplayer tab",
             g_native ? "; the 4th title button opens it too (LICHII_COOP_TITLE_NATIVE)" : "");
}

void SaveLook(const CoopLook::Look& look) {
    const auto kit = KitOptionsFile();   // (the one the game reads: the kit's, or beside longshot.exe, as Save)
    std::string coop = ReadText(CoopIni());
    std::string options = kit ? ReadText(*kit) : std::string();
    const bool ok = !CoopLook::WriteSettings(look, coop, kit ? &options : nullptr) ||
                    (kit ? WriteText(*kit, options) : WriteText(CoopIni(), coop));
    const std::string colour = CoopLook::ColourText(look[CoopLook::Part::tunic]);
    LOG_INFO(Core, "coop: look: colour {} saved to {}{}", colour.empty() ? std::string("by player number") : colour,
             kit ? "coop-options.ini" : "coop.ini", ok ? "" : " (the file could not be written)");
}

// Whether `kit` (FindKit's) is this game's own: named (OOT_COOP_KIT, coop-kit.txt), or the game is its
// hookshot\game (or the kit folder itself). A kit found further up is someone else's folder the game sits in.
static bool OwnKit(const fs::path& kit) {
    if (!Env("OOT_COOP_KIT").empty() || !ReadText(CoopRuntime::GameDir() / "coop-kit.txt").empty()) {
        return true;
    }
    const fs::path game = CoopRuntime::GameDir();
    std::error_code ec;
    return fs::equivalent(kit, game, ec) || fs::equivalent(kit, game.parent_path().parent_path(), ec);
}

std::optional<fs::path> KitOptionsFile() {
    // No kit of its own (a game folder copied out of its kit, Nucleus Co-op's instance folders, also with Nucleus
    // unzipped inside a kit: docs/play-lan.md, "Split-screen with Nucleus Co-op"): the coop-options.ini beside
    // longshot.exe, so each copy can have its own.
    auto found = FindKit();
    std::error_code exists_ec;
    const bool beside = fs::exists(CoopRuntime::GameDir() / "coop-options.ini", exists_ec);
    if (found && beside && !OwnKit(*found)) {
        found.reset();
    }
    if (!found && !beside) {
        return std::nullopt;
    }
    return (found ? *found : CoopRuntime::GameDir()) / "coop-options.ini";
}

void ApplyKitOptions() {
    const auto file = KitOptionsFile();
    if (!file) {
        return;
    }
    const fs::path kit = file->parent_path();
    std::error_code same_ec;
    const bool found = !fs::equivalent(kit, CoopRuntime::GameDir(), same_ec);   // (a kit above the game)
    int set = 0;
    for (auto [k, v] : CoopTitleMenu::KitVariables(ReadText(*file))) {
        if (std::getenv(k.c_str()) != nullptr) {
            continue;   // the launcher set it (or the player did)
        }
        std::error_code ec;
        if (!v.empty() && !PathOf(v).is_absolute() && fs::exists(kit / PathOf(v), ec)) {
            v = Utf8Path(fs::absolute(kit / PathOf(v), ec));   // as play.ps1: a file of the kit
        }
        SetEnv(k, v);
        ++set;
    }
    if (set != 0) {
        LOG_INFO(Core, "coop: {} switch(es) from {}\\coop-options.ini ({})", set, Utf8Path(kit),
                 found ? "the game was started without the kit's launcher" : "beside the game: no kit above it");
    }
}


void OnTitle(void* ctx, std::uint64_t screen, int event) {
    if (event == HOS_COOP_TITLE_GONE) {
        std::uint64_t was = screen;
        g_screen.compare_exchange_strong(was, 0);
        return;
    }
    g_screen = screen;
    if (event == HOS_COOP_TITLE_SETUP) {
        LogButtons(screen);
        const std::int32_t count = ButtonCount(screen);
        const std::uint64_t layout = Read64(screen + kTitleLayout);
        const bool menu = ctx != nullptr && HasScratch() && layout != 0 &&
                          TitlePane(ctx, layout, CoopTitleMenu::kMenuMarker) != 0;
        g_menu = menu;
        if (menu) {
            // The pack's own menu: Start, Multiplayer, Options on the game's buttons (StepMenu sets them from the
            // title's step on, once the game's OpenTitle has set its own).
            g_rows = true;
            std::lock_guard lock(g_mutex);
            g_sub = false;
            g_swallow = 0, g_focus_to = -1, g_decide = -1, g_sound = nullptr;
            g_one_start = CoopModsRuntime::FileSelectRuns();
            g_apply_until = NowMs() + kApplyForMs, g_apply_next = 0;
            LOG_INFO(Core, "coop: title: the title's own menu: {}, Multiplayer, Options ({} buttons)",
                     g_one_start ? "Start" : "Continue, New Game", count);
        } else if (count >= kFirstRow + static_cast<std::int32_t>(kRowLabels.size())) {
            const bool ok = SetRowTexts(ctx, screen);
            g_rows = ok;
            LOG_INFO(Core, "coop: title: the title's own rows Host, Join, Co-op Settings (buttons {} to {}){}",
                     kFirstRow, kFirstRow + 2, ok ? "" : ": their texts could not be set");
        } else {
            g_rows = false;
            LOG_INFO(Core, "coop: title: the title has {} buttons, not the puppet pack's {}: the Minus prompt "
                     "instead (build the puppet pack again for the title's own rows)",
                     count, kFirstRow + static_cast<int>(kRowLabels.size()));
        }
    }
    if (event == HOS_COOP_TITLE_STEP) {
        if (!g_step_seen.exchange(true)) {
            LOG_INFO(Core, "coop: title: the title screen's step runs");
        }
        RefreshSaveList(ctx);
        StepMenu(ctx, screen);
        g_last_step_ms = NowMs();
        const int f = FocusedIndex(screen);
        std::lock_guard lock(g_mutex);
        if (f != g_focus_logged) {
            g_focus_logged = f;
            LOG_INFO(Core, "coop: title: the title's focus is on button {}", f);
        }
    }
}

// One of the title's own co-op rows (Host, Join, Co-op Settings: they open our menu, the game's title sequence has
// nothing for them). In the title's own menu every row that is not the player's own (CoopTitleMenu::PlayersOwn):
// Multiplayer and the submenu's, whose first is Host in the game's Continue place. g_mutex.
bool OnCoopRow(int focused) {
    if (g_menu.load()) {
        const CoopTitleMenu::TitleMenuLook look = CoopTitleMenu::MenuFor(g_any_save, g_sub, g_one_start);
        return focused < 0 || static_cast<std::size_t>(focused) >= look.count ||
               !CoopTitleMenu::PlayersOwn(&look.rows[static_cast<std::size_t>(focused)]);
    }
    return g_rows && focused >= kFirstRow && focused < kFirstRow + static_cast<int>(kRowLabels.size());
}

void NotePlayerPad(std::uint64_t buttons) {
    std::lock_guard lock(g_mutex);
    CoopTitleMenu::PlayerChoice::Pad in;
    in.buttons = buttons;
    in.title_up = TitleUp();
    in.focused = in.title_up ? FocusedIndex(g_screen.load()) : -1;
    in.on_coop_row = OnCoopRow(in.focused);
    in.in_game = !in.title_up && g_choice.Lately(NowMs()) && InGame();   // (the player list only while it matters)
    const auto seen = g_choice.Step(NowMs(), in);
    if (seen.picks_file) {
        // The player's own A on the title (not on our co-op rows): they pick their own file or a new game while the
        // host's save is still coming (or late). Seen here, before a code mod's hold (Save Profiles' file select
        // takes this A and loads the file picked in it). No switch after this: it would change the save folder under
        // the load.
        CoopSave::CancelPendingSwitch("the player chose a file on the title");
        if (g_wait.waiting()) {
            g_wait.PlayerChose();
            LOG_INFO(Core, "coop: title: host's save: not waited for (the player chose a file)");
        }
    }
    if (seen.window_opened) {
        LOG_INFO(Core, "coop: title: the player's own A on the title's first button (a code mod's choice for them goes "
                       "through for {} s after it, co-op busy or not)",
                 CoopTitleMenu::PlayerChoice::kWindowMs / 1000);
    }
}

// The pad while co-op's menu page is on the title's own buttons: every button is the page's (the title moves nothing
// itself). Up and down move the menu's selection, left and right change a value, A does the row the title's cursor is
// on (a mouse click is the same A), B goes back (off the page opened: the Multiplayer rows again). A field typed into:
// the on-screen keyboard's keys (A presses the one under its cursor, B deletes, + is OK). CoopPause is called after
// the lock (it calls in here).
std::uint64_t g_native_last = 0;   // (g_mutex)
std::uint64_t NativePad(std::uint64_t buttons, std::int32_t stick[4]) {
    constexpr std::uint64_t kPlus = 1ull << 10, kLeft = 1ull << 12, kUp = 1ull << 13, kRight = 1ull << 14,
                            kDown = 1ull << 15;
    std::uint64_t in = buttons;
    if (stick != nullptr) {
        if (stick[1] > 20000) in |= kUp;
        if (stick[1] < -20000) in |= kDown;
        if (stick[0] > 20000) in |= kRight;
        if (stick[0] < -20000) in |= kLeft;
    }
    const CoopPauseMenu::View v = CoopPause::NativeView();
    const CoopPauseMenu::Page page = CoopPause::NativePage();
    std::vector<std::pair<int, int>> events;
    std::vector<std::uint32_t> keys, chars;
    bool leave = false;
    {
        std::lock_guard lock(g_mutex);
        const int opened = g_native_page.load();
        std::uint64_t down = in & ~g_native_last;
        g_native_last = in;
        if (opened < 0) {
            g_native_seen = -1;   // (none open: the next one, the same page again too, is a page that came)
            return 0;
        }
        // (a page or another of its views came: a click's A just after is the click that brought it, not the row now
        // under the mouse: a tester's click on Join opened Room or address's keyboard)
        if (const int seen = static_cast<int>(page) * 16 + opened + (v.typing ? 256 : 0); seen != g_native_seen) {
            g_native_seen = seen, g_title_view_ms = NowMs();
        }
        if ((down & kA) != 0 && ClickThrough()) {
            down &= ~kA;
            LOG_INFO(Core, "coop: title: page: a click's A {} ms after the page came: not its row's",
                     NowMs() - g_title_view_ms.load());
        }
        const int focused = ClickedButton((down & kA) != 0, FocusedIndex(g_screen.load()));
        g_click_remapped = false;   // (the page's rows are all ours: the remapped A acts at once)
        const CoopTitleNative::Look look = CoopTitleNative::LookFor(v, g_native_top);
        if (v.typing) {
            // The mouse on the keyboard's keys, as on the pause page: a key pressed and let go on is pressed, and the
            // A Longshot's mouse sends for a click is not the keyboard cursor's (a tester's clicks went to the title
            // behind the keyboard, 2026-11-19).
            {
                const std::uint64_t now = NowMs();
                float fx = 0, fy = 0;
                bool held = false;
                const bool got = CoopPauseTab::ClientCursor(&fx, &fy, &held);
                if (held && !g_kb_held && ++g_kb_logged <= 20) {   // (trace: where a press on the keyboard went)
                    int r = -1, c = -1;
                    const bool on = got && CoopKeyboard::KeyAtPoint(fx, fy, &r, &c);
                    LOG_INFO(Core, "coop: title: keyboard: the left button down at ({:.3f}, {:.3f}): {}", fx, fy,
                             !got ? "the window not in front" : on ? fmt::format("key row {} column {}", r, c) : "no key there");
                }
                if (got) {
                    int r = -1, c = -1;
                    const bool on = CoopKeyboard::KeyAtPoint(fx, fy, &r, &c);
                    if (held && !g_kb_held) g_kb_press = on ? r * 16 + c : -1;
                    if (!held && g_kb_held && on && g_kb_press == r * 16 + c) {
                        g_native_keys.Set(r, c);
                        const CoopKeyboard::KeyAt k = CoopKeyboard::At(r, c);
                        switch (k.key) {
                        case CoopKeyboard::Key::kChar: chars.push_back(static_cast<unsigned char>(k.ch)); break;
                        case CoopKeyboard::Key::kSpace: chars.push_back(' '); break;
                        case CoopKeyboard::Key::kDelete: keys.push_back(0x08); break;
                        case CoopKeyboard::Key::kCancel: keys.push_back(0x1B); break;
                        case CoopKeyboard::Key::kOk: keys.push_back(0x0D); break;
                        case CoopKeyboard::Key::kNone: break;
                        }
                        g_sound = "button_decide";
                        g_kb_click_ms = now;
                    }
                    g_kb_held = held;
                }
                if (g_kb_held || now - g_kb_click_ms < 400 || MouseClicked()) down &= ~kA;
            }
            const int dr = (down & kDown) != 0 ? 1 : (down & kUp) != 0 ? -1 : 0;
            const int dc = (down & kRight) != 0 ? 1 : (down & kLeft) != 0 ? -1 : 0;
            if (dr != 0 || dc != 0) g_native_keys.Move(dr, dc), g_sound = "button_on";
            if ((down & kA) != 0) {
                const CoopKeyboard::KeyAt k = CoopKeyboard::At(g_native_keys.row(), g_native_keys.col());
                switch (k.key) {
                case CoopKeyboard::Key::kChar: chars.push_back(static_cast<unsigned char>(k.ch)); break;
                case CoopKeyboard::Key::kSpace: chars.push_back(' '); break;
                case CoopKeyboard::Key::kDelete: keys.push_back(0x08); break;
                case CoopKeyboard::Key::kCancel: keys.push_back(0x1B); break;
                case CoopKeyboard::Key::kOk: keys.push_back(0x0D); break;
                case CoopKeyboard::Key::kNone: break;
                }
                g_sound = "button_decide";
            }
            if ((down & kB) != 0) keys.push_back(0x1B);   // (B: Cancel, the field as it was, as on the pause page)
            if ((down & kY) != 0) keys.push_back(0x08);   // (Y: the letter before the cursor off)
            if ((down & kPlus) != 0) keys.push_back(0x0D);
        } else {
            // (the step from the row the title shows lit: the mouse's hover moves the game's focus, not the menu's own
            // row, and Down on the last shown row went from an older row, wrapping to the top instead of scrolling on
            // to Room name and Relay, a test run 2026-11-20)
            if ((down & (kUp | kDown)) != 0 && focused >= 0 && focused < CoopTitleNative::kRowButtons &&
                look.rows[static_cast<std::size_t>(focused)] >= 0) {
                events.emplace_back(HOS_COOP_MENU_SELECT, look.rows[static_cast<std::size_t>(focused)]);
            }
            if ((down & kUp) != 0) events.emplace_back(HOS_COOP_MENU_UP, 0);
            if ((down & kDown) != 0) events.emplace_back(HOS_COOP_MENU_DOWN, 0);
            if ((down & kLeft) != 0) events.emplace_back(HOS_COOP_MENU_LEFT, 0);
            if ((down & kRight) != 0) events.emplace_back(HOS_COOP_MENU_RIGHT, 0);
            const bool back = (down & kB) != 0 || ((down & kA) != 0 && focused == look.back);
            if ((down & kA) != 0) {   // (the log: what an A did here, for a tester's run)
                LOG_INFO(Core, "coop: title: page: A on button {} (the menu's row {}{})", focused,
                         focused >= 0 && focused < CoopTitleNative::kButtons ? look.rows[static_cast<std::size_t>(focused)]
                                                                             : -1,
                         back ? ", Back" : "");
            }
            if ((down & kA) != 0 && !back && focused >= 0 && focused < CoopTitleNative::kButtons &&
                look.rows[static_cast<std::size_t>(focused)] >= 0) {
                events.emplace_back(HOS_COOP_MENU_SELECT, look.rows[static_cast<std::size_t>(focused)]);
                events.emplace_back(HOS_COOP_MENU_CONFIRM, 0);
                g_sound = "button_decide";
                g_native_keys.Reset();   // (a field it may open: its keyboard from its first key)
            }
            if (back) {
                if (page == static_cast<CoopPauseMenu::Page>(opened) || page == CoopPauseMenu::Page::kMain) {
                    leave = true;
                } else {
                    events.emplace_back(HOS_COOP_MENU_BACK, 0);
                }
            }
            // The mouse resting on the page's last row with more rows below (or its first with more above): the page
            // scrolls a row each 450 ms, so a mouse reaches every row. The wheel only moved the title's focus, and the
            // Join page's Join row (its fourth) could not be clicked: a test run, 2026-11-20.
            const std::uint64_t now_ms = NowMs();
            const bool mouse_leads = g_title_mouse_move_ms.load() > g_title_pad_ms.load() &&
                                     now_ms - g_title_mouse_move_ms.load() < 10000;
            const int last_button = CoopTitleNative::kRowButtons - 1;
            int edge_dir = 0;
            // (only on the row's outer edge, its lower part for the last, its upper part for the first: resting on the
            // middle of the row to read or click it scrolled it away, a test run 2026-11-20)
            float mx = 0, my = 0;
            bool mheld = false;
            std::optional<CoopModScreens::Box> box;
            if (mouse_leads && (focused == 0 || focused == last_button) && CoopPauseTab::LayoutCursor(&mx, &my, &mheld)) {
                const auto read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
                if (const std::uint64_t b = TitleButton(g_screen.load(), focused); b != 0) box = CoopModScreens::HitBox(read, b);
            }
            const float band = box ? (box->y1 - box->y0) * 0.35f : 0.0f;
            if (mouse_leads && (focused == 0 || focused == last_button) && ++g_edge_logged <= 6) {   // (trace)
                LOG_INFO(Core, "coop: title: page: the mouse at ({:.0f}, {:.0f}) on button {}: its box {}", mx, my, focused,
                         box ? fmt::format("x {:.0f}..{:.0f}, y {:.0f}..{:.0f}", box->x0, box->x1, box->y0, box->y1)
                             : std::string("not read"));
            }
            // (the box's two y in either order: its lower edge the smaller, y up)
            const float lo_y = box ? std::min(box->y0, box->y1) : 0.0f, hi_y = box ? std::max(box->y0, box->y1) : 0.0f;
            const float lo_x = box ? std::min(box->x0, box->x1) : 0.0f, hi_x = box ? std::max(box->x0, box->x1) : 0.0f;
            const float edge = std::abs(band);
            // (the game's hit box is the row's words only, about a third of the way to the next row: below the middle
            // of the last row's words, or above the first's, counts, as far as the game's own hover keeps the row; a
            // trace, 2026-11-20)
            (void)lo_x, (void)hi_x, (void)edge;
            const float mid_y = (lo_y + hi_y) / 2, margin = (hi_y - lo_y) * 0.15f;
            const bool at_bottom = box.has_value() && my < mid_y - margin;
            const bool at_top = box.has_value() && my > mid_y + margin;
            if (mouse_leads && events.empty() && focused >= 0 && focused <= last_button) {
                const int row = look.rows[static_cast<std::size_t>(focused)];
                bool more = false;
                if (focused == last_button && row >= 0 && at_bottom) {
                    for (std::size_t i = static_cast<std::size_t>(row) + 1; i < v.rows.size() && !more; ++i) {
                        more = v.rows[i].kind != CoopPauseMenu::RowKind::kHeading;
                    }
                    if (more) edge_dir = 1;
                } else if (focused == 0 && look.top > 0 && at_top) {
                    edge_dir = -1;
                }
            }
            if (edge_dir == 0) {
                g_hover_edge = -1;
            } else if (g_hover_edge != focused) {
                g_hover_edge = focused, g_hover_since = now_ms;
            } else if (now_ms - g_hover_since >= 450) {
                events.emplace_back(HOS_COOP_MENU_SELECT, look.rows[static_cast<std::size_t>(focused)]);
                events.emplace_back(edge_dir > 0 ? HOS_COOP_MENU_DOWN : HOS_COOP_MENU_UP, 0);
                g_hover_since = now_ms;
                g_sound = "button_on";
            }
            if (!events.empty() && edge_dir == 0) g_native_refocus = true;   // (a scroll by the mouse: its focus stays)
            if ((down & (kUp | kDown)) != 0) g_sound = "button_on";
        }
        if (leave) {   // (the Multiplayer rows again, the title's cursor on the page's row)
            using CoopTitleMenu::TitleDo;
            const TitleDo row = opened == static_cast<int>(CoopPauseMenu::Page::kHost)   ? TitleDo::kHost
                                : opened == static_cast<int>(CoopPauseMenu::Page::kJoin) ? TitleDo::kJoin
                                                                                          : TitleDo::kSettings;
            g_native_page = -1;
            g_sub = true;
            g_focus_to = CoopTitleMenu::RowOf(CoopTitleMenu::MenuFor(g_any_save, true, g_one_start), row);
            g_sound = "button_decide";
            g_apply_until = NowMs() + kApplyForMs, g_apply_next = 0;
            g_swallow |= buttons;   // (the B or A that left: not the Multiplayer rows' too)
            // (nor a press of theirs: still held at their next read, the B went on to leave Multiplayer's rows as
            // well, a test run 2026-11-20)
            g_pad_last |= buttons;
            LOG_INFO(Core, "coop: title: menu: the page left: Multiplayer's rows again");
        }
    }
    for (const auto& [e, a] : events) CoopPause::NativeInput(e, a);
    for (const std::uint32_t k : keys) CoopPause::Key(k, false);
    for (const std::uint32_t c : chars) CoopPause::Char(c);
    if (leave) {
        CoopPause::CloseNative();
        CoopKeyboard::Publish(CoopKeyboard::Shown{});
    }
    return 0;   // (the title gets none of it)
}

std::uint64_t OnPad(std::uint64_t buttons, std::int32_t stick[4]) {
    if (g_close_native_asked.exchange(false)) {   // (the auto-load's: co-op's page shut, outside our lock)
        CoopPause::CloseNative();
        CoopKeyboard::Publish(CoopKeyboard::Shown{});
    }
    if (const std::uint64_t t0 = g_auto_press_ms.load(); t0 != 0) {   // (the host's save loading by itself)
        const std::uint64_t e = NowMs() - t0;
        if (e >= kAutoPressEnd || InGame()) {
            g_auto_press_ms = 0;
            LOG_INFO(Core, "coop: title: the host's save: the presses that load it are done ({})",
                     InGame() ? "Link is in the game" : "the player has the pad again");
        } else {
            for (const std::uint64_t at : kAutoPressAt) {
                if (e >= at && e < at + kAutoPressHold) return kA;
            }
            return 0;
        }
    }
    {   // (the mouse's left button: when it last went down, so a press it made is told from the pad's; when the mouse
        // last moved, and the pad last pressed: which of them leads, for the page's scroll by the mouse)
        float fx = 0, fy = 0;
        bool held = false;
        const bool over = CoopPauseTab::ClientCursor(&fx, &fy, &held);
        const bool in = over && held;
        if (in && !g_title_mouse_held) g_title_mouse_down_ms = NowMs();
        g_title_mouse_held = in;
        if (over && (std::abs(fx - g_title_mouse_x) > 0.002f || std::abs(fy - g_title_mouse_y) > 0.002f)) {
            g_title_mouse_x = fx, g_title_mouse_y = fy, g_title_mouse_move_ms = NowMs();
        }
        if ((buttons & 0xF0FFull) != 0) g_title_pad_ms = NowMs();   // (A B X Y L R ZL ZR + - and the arrows)
    }
    // (the host's save in after a join made on co-op's page: the page is shut here, before NativePad takes the pad,
    // and the presses that load it follow on the title's rows; with the page open the step below never ran, a test
    // run 2026-11-20)
    if (g_auto_load_until.load() != 0 && g_native_page.load() >= 0 && TitleUp() && CoopSave::HostSaveReady()) {
        {
            std::lock_guard lock(g_mutex);
            g_native_page = -1;
            g_sub = false;
            g_focus_to = 0;
            g_apply_until = NowMs() + kApplyForMs, g_apply_next = 0;
            g_title_view_ms = NowMs();
        }
        CoopPause::CloseNative();
        CoopKeyboard::Publish(CoopKeyboard::Shown{});
        LOG_INFO(Core, "coop: title: the host's save is in: co-op's page closed for its Continue");
        return 0;
    }
    if (g_native_page.load() >= 0 && g_menu.load()) {
        if (TitleUp()) return NativePad(buttons, stick);
        g_native_page = -1;   // (the title went: the page with it)
        CoopPause::CloseNative();
        CoopKeyboard::Publish(CoopKeyboard::Shown{});
    }
    std::optional<CoopPauseMenu::Page> open;   // (opened after the lock: CoopPause calls in here)
    bool open_native = false;                  // (on the title's own buttons, OpenNative)
    std::uint64_t keep = 0;                    // (the pad bits kept from the game: g_swallow)
    {
        std::lock_guard lock(g_mutex);
        const bool title = TitleUp();
        CoopSave::NoteTitle(title);   // (a guest switches to the host's save only on the title's menu)
        if (g_wait.waiting() && CoopSave::HostHasNoSave()) {
            g_wait.NoSave();
            LOG_INFO(Core, "coop: title: host's save: {}", g_wait.Line());
        }
        if (g_wait.watching()) {
            // What the title says while the host's save comes. Nothing is pressed: the player loads a save
            // themselves when the chat says Success (CoopSave, save::GuestNotices).
            const std::string before = g_wait.Line();
            CoopTitleMenu::SaveWait::Input in;
            in.host_save_ready = CoopSave::HostSaveReady();
            in.save_coming = CoopSave::HostSaveComing();
            in.title_up = title;
            in.in_game = InGame();
            in.session_live = CoopSave::SessionLive();
            if (g_wait.Step(NowMs(), in).rearm) {
                CoopSave::ResumePendingSwitch();   // (they backed out of their own pick: the host's save again)
                g_choice.Clear();   // (and their earlier A no longer makes a mod's choice theirs)
            }
            if (const std::string now = g_wait.Line(); now != before && !g_wait.chose()) {
                LOG_INFO(Core, "coop: title: host's save: {}", now.empty() ? "done" : now);
            }
        }
        // (back at the title from a join with a file loaded: the host's save, once in, loads by itself: the game's
        // Continue chosen, as the player's A on it, a moment after the title's menu came up)
        if (!title) g_title_since = 0;
        else if (g_title_since == 0) g_title_since = NowMs();
        if (const std::uint64_t until = g_auto_load_until.load(); until != 0) {
            const bool test = g_auto_load_test.load();
            if (NowMs() > until || (!test && (!CoopSave::SessionLive() || CoopSave::HostHasNoSave()))) {
                g_auto_load_until = 0;
                LOG_INFO(Core, "coop: title: the host's save did not come to load by itself: the player loads one");
            } else if (title && (g_sub.load() || g_native_page >= 0) && !InGame() &&
                       (test || CoopSave::HostSaveReady())) {
                // (a join made on the title: its page or Multiplayer's rows still up. The title's own rows again,
                // the cursor on Continue; the presses come once they are there)
                g_native_page = -1;
                g_sub = false;
                g_focus_to = 0;
                g_apply_until = NowMs() + kApplyForMs, g_apply_next = 0;
                g_title_view_ms = NowMs();
                g_close_native_asked = true;
                LOG_INFO(Core, "coop: title: the host's save is in: co-op's page closed for its Continue");
            } else if (title && !g_sub.load() && g_native_page < 0 && !InGame() && (test || CoopSave::HostSaveReady()) &&
                       g_any_save && !g_list_open.load() && NowMs() - g_title_since >= 1500 &&
                       NowMs() - g_title_view_ms.load() >= 1000) {
                g_auto_load_until = 0;
                g_focus_to = 0;   // (the game's first button, Continue; then the A presses: OnPad's start)
                g_auto_press_ms = NowMs();
                LOG_INFO(Core, "coop: title: the host's save is in: Continue, its newest save and Load pressed for "
                               "the player");
            }
        }
        const std::uint64_t down = buttons & ~g_pad_last;
        g_pad_last = buttons;
        g_swallow &= buttons;   // (let go: the game gets it again)
        const int focused = title ? ClickedButton((down & kA) != 0, FocusedIndex(g_screen.load())) : -1;
        const bool menu = g_menu.load();
        const CoopTitleMenu::TitleMenuLook look = CoopTitleMenu::MenuFor(g_any_save, g_sub.load(), g_one_start);
        const CoopTitleMenu::TitleMenuRow* menu_row =
            menu && focused >= 0 && static_cast<std::size_t>(focused) < look.count ? &look.rows[static_cast<std::size_t>(focused)]
                                                                                 : nullptr;
        if (!HasScratch() || !title) {
            return buttons & ~g_swallow;
        }
        // The host's save on its way (Join at the title): the co-op rows wait. The title's own menu still keeps the
        // A of every row the module answers from the game, or its NormalTitle would start Continue or New Game for
        // Multiplayer, Host or Join with the switch still pending (a mouse click is the same A: Longshot taps it on
        // the button under the cursor once the focus is there).
        const bool rows_wait = g_wait.active();
        if (menu) {
            // The title's own menu: the rows the module answers keep their A from the game (its NormalTitle would
            // start Continue, New Game or Options for them); the step makes the sound, the focus and the look.
            using CoopTitleMenu::TitleDo;
            // (a click's: the focus stays on the button under the mouse. Moved, Longshot's mouse gave it back only once
            // the mouse went onto another button: a tester waited "a good second" for the highlight to follow)
            const bool clicked = MouseClicked();
            const auto change = [&](bool sub, int focus) {
                g_sub = sub;
                g_title_view_ms = NowMs();
                g_focus_to = clicked ? -1 : focus, g_sound = "button_decide";
                g_apply_until = NowMs() + kApplyForMs, g_apply_next = 0;
            };
            const auto back = [&] {   // (the title's rows again, the cursor on Multiplayer)
                change(false, CoopTitleMenu::RowOf(CoopTitleMenu::MenuFor(g_any_save, false, g_one_start),
                                                   TitleDo::kMultiplayer));
            };
            const auto page = [&](CoopPauseMenu::Page p) {
                if (rows_wait) {
                    LOG_INFO(Core, "coop: title: menu: the row waits (the host's save is on its way)");
                    return;
                }
                open = p, g_sound = "button_decide";
                // (the A that opened it, still held a few reads after a click, is not the page's first press: a
                // tester's click on Join went on to the page's Password row, 2026-11-19)
                g_native_last = buttons;
                // (the title's own buttons show the page: no overlay; StepNative falls back to it for an older pack)
                g_native_page = static_cast<int>(p), g_native_top = 0, g_native_refocus = !clicked;
                g_title_view_ms = NowMs(), g_native_seen = -1;   // (a click's next A is not the page's: NativePad)
                g_native_keys.Reset();
                open_native = true;
            };
            // (a click's A just after the rows changed under the mouse (Multiplayer's rows, the title's again): the
            // click that changed them, not the row now there; kept from the game too)
            if ((down & kA) != 0 && ClickThrough()) {
                g_swallow |= kA;
                LOG_INFO(Core, "coop: title: menu: a click's A {} ms after the rows changed: not the row's",
                         NowMs() - g_title_view_ms.load());
                return buttons & ~g_swallow;
            }
            // (a click remapped to a game's own row: the game's A would go to the button it focuses: kept, the focus
            // moved there; the next click is that row's)
            if ((down & kA) != 0 && std::exchange(g_click_remapped, false) && menu_row != nullptr &&
                menu_row->action == TitleDo::kGame) {
                g_swallow |= kA;
                return buttons & ~g_swallow;
            }
            if ((down & kA) != 0 && menu_row != nullptr && menu_row->action != TitleDo::kGame) {
                g_swallow |= kA;
                LOG_INFO(Core, "coop: title: menu: row {} pressed ({})", focused, menu_row->label);
                switch (menu_row->action) {
                case TitleDo::kGame: break;
                case TitleDo::kDecide: g_decide = menu_row->decide; break;   // (Options: the player's own)
                case TitleDo::kMultiplayer: change(true, CoopTitleMenu::MenuFor(g_any_save, true, g_one_start).focus); break;
                case TitleDo::kBack: back(); break;
                case TitleDo::kHost: page(CoopPauseMenu::Page::kHost); break;
                case TitleDo::kJoin: page(CoopPauseMenu::Page::kJoin); break;
                case TitleDo::kSettings: page(CoopPauseMenu::Page::kSettings); break;
                }
            } else if ((down & kB) != 0 && g_sub) {
                g_swallow |= kB;   // (B in the submenu: the three rows again)
                back();
                LOG_INFO(Core, "coop: title: menu: B: the title's rows again");
            }
            if (!open) return buttons & ~g_swallow;
        }
        if (rows_wait) {
            return buttons & ~g_swallow;   // (the older title's co-op rows wait)
        }
        const int f = (down & kA) != 0 && !menu ? FocusedIndex(g_screen.load()) : -1;
        if (menu) {
            // (opened below)
        } else if (OnCoopRow(f)) {
            // The title's own rows: the game takes the A too (the button's own press look and sound), and its
            // title sequence has nothing for these buttons; the Multiplayer tab opens at the row's page.
            const auto row = static_cast<std::size_t>(f - kFirstRow);
            LOG_INFO(Core, "coop: title: the title's row {} pressed", kRowLabels[row]);
            open = kRowPages[row];
        } else if ((!g_rows && (down & kMinus) != 0) || (g_native && f == 3)) {
            open = CoopPauseMenu::Page::kMain;   // the Minus prompt, or the 4th title button (LICHII_COOP_TITLE_NATIVE)
        }
        keep = g_swallow;
    }
    if (open) {
        if (open_native) {
            CoopPause::OpenNative(*open);
        } else {
            CoopPause::Open(*open);
        }
    }
    return buttons & ~keep;
}

std::string TestState() {
    const bool up = TitleUp();
    return fmt::format("title={} sub={} native_page={} title_focus={} auto_load={} auto_press={}", up ? 1 : 0,
                       g_sub.load() ? 1 : 0, g_native_page.load(), up ? FocusedIndex(g_screen.load()) : -1,
                       g_auto_load_until.load() != 0 ? 1 : 0, g_auto_press_ms.load() != 0 ? 1 : 0);
}

bool Up() {
    return TitleUp();
}

bool FocusOffFirst() {
    if (!TitleUp()) return false;
    const int f = FocusedIndex(g_screen.load());
    return f > 0;
}

bool StartSaveWait(const std::string& host) {
    std::lock_guard lock(g_mutex);
    g_wait.Start(NowMs(), host);
    g_choice.Clear();   // (an A before the join was not a choice of the host's save or not)
    return TitleUp();
}

void StopSaveWait() {
    std::lock_guard lock(g_mutex);
    g_wait.Stop();
    g_auto_load_until = 0;
}

void LoadHostSaveOnTitle(bool test) {
    g_auto_load_test = test;
    g_auto_load_until = NowMs() + kAutoLoadMs;
    LOG_INFO(Core, "coop: title: the host's save loads by itself once it is in (back from a join with a file loaded)");
}

bool SaveWaitWaiting() {
    std::lock_guard lock(g_mutex);
    return g_wait.waiting();
}

bool PlayerChoseLately() {
    std::lock_guard lock(g_mutex);
    return g_choice.Lately(NowMs());
}

bool RowsOurs() {
    if (!g_menu.load() || !TitleUp()) return false;
    // (no lock: the game's screen update that asks may come from inside a call the title makes under it)
    return g_native_page.load() >= 0 || g_sub.load();
}

bool AskSaveListRead() {
    if (!TitleUp()) {
        return false;   // (RefreshSaveList runs only at the title's step: off it the read would never complete)
    }
    g_list_open = true;
    g_mod_asked = true;
    return true;
}

bool SaveListReady() {
    if (g_list_open.load()) {
        return false;
    }
    const std::uint64_t mgr = CaptionMgr();
    std::uint8_t state = 0;
    return mgr != 0 && CoopRuntime::Read(mgr + kCaptionState, &state, 1) &&
           state == CoopTitleMenu::SaveListRefresh::kRead;
}

std::string ClientId() {
    static std::mutex m;
    std::lock_guard lock(m);
    static std::string id;
    if (!id.empty()) {
        return id;
    }
    std::string coop = ReadText(CoopIni());
    if (const auto have = CoopTitleMenu::IniGet(coop, "client_id"); have && have->size() >= 16) {
        id = *have;
        return id;
    }
    std::random_device rd;
    for (int i = 0; i < 4; ++i) {
        id += fmt::format("{:08x}", rd());
    }
    coop = CoopTitleMenu::IniSet(coop, "client_id", id, true);
    WriteText(CoopIni(), coop);
    return id;
}


// The "(-) Multiplayer" prompt, bottom left (Minus opens the menu's Multiplayer tab; the owner, 2026-11-14: keep it,
// with the button lined up with the word), or the host's save loading ("Loading Saria's save...") without the button.
// The title's own rows (a pack built with --title-rows): no prompt.
std::optional<CoopDraw::TitleView> View() {
    std::lock_guard lock(g_mutex);
    if (!HasScratch() || !TitleUp()) {
        return std::nullopt;
    }
    if (const std::string line = g_wait.Line(); !line.empty()) {
        // ("Loading Saria's save..." in the player's language: the name is data)
        return CoopDraw::TitleView{CoopI18n::Tr(line)};
    }
    if (g_rows) {
        return std::nullopt;
    }
    return CoopDraw::TitleView{CoopI18n::Tr("Multiplayer"), true};
}

}  // namespace CoopTitle
