// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_title.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <system_error>

#include <fmt/format.h>

#include "common/logging.h"
#include "coop_hook.h"
#include "coop_i18n.h"
#include "coop_mod_screens.h"
#include "coop_mods_runtime.h"
#include "coop_pause.h"
#include "coop_pause_files.h"
#include "coop_runtime.h"
#include "coop_save.h"
#include "coop_title_menu.h"

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
bool g_sub = false;                // the Multiplayer submenu shows
bool g_any_save = true;            // the look's save list: a save or not (the caption manager's)
bool g_one_start = false;          // one Start row for Continue and New Game (a file select mod runs)
std::uint64_t g_apply_until = 0, g_apply_next = 0;   // the look set again until then (kApplyForMs)
int g_focus_to = -1;               // a button to give the focus at the next step
int g_decide = -1;                 // a game button to decide at the next step
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
void StepMenu(void* ctx, std::uint64_t screen) {
    if (!g_menu.load() || ctx == nullptr || !HasScratch()) return;
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
        any_save = g_any_save, sub = g_sub;
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
    const auto kit = FindKit();
    std::string coop = ReadText(CoopIni());
    std::string options = kit ? ReadText(*kit / "coop-options.ini") : std::string();
    const bool ok = !CoopLook::WriteSettings(look, coop, kit ? &options : nullptr) ||
                    (kit ? WriteText(*kit / "coop-options.ini", options) : WriteText(CoopIni(), coop));
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

void ApplyKitOptions() {
    // No kit of its own (a game folder copied out of its kit, Nucleus Co-op's instance folders, also with Nucleus
    // unzipped inside a kit: docs/play-lan.md, "Split-screen with Nucleus Co-op"): the coop-options.ini beside
    // longshot.exe, so each copy can have its own.
    auto found = FindKit();
    std::error_code exists_ec;
    const bool beside = fs::exists(CoopRuntime::GameDir() / "coop-options.ini", exists_ec);
    if (found && beside && !OwnKit(*found)) {
        found.reset();
    }
    const std::optional<fs::path> kit = found ? *found : CoopRuntime::GameDir();
    if (!found && !beside) {
        return;
    }
    int set = 0;
    for (auto [k, v] : CoopTitleMenu::KitVariables(ReadText(*kit / "coop-options.ini"))) {
        if (std::getenv(k.c_str()) != nullptr) {
            continue;   // the launcher set it (or the player did)
        }
        std::error_code ec;
        if (!v.empty() && !PathOf(v).is_absolute() && fs::exists(*kit / PathOf(v), ec)) {
            v = Utf8Path(fs::absolute(*kit / PathOf(v), ec));   // as play.ps1: a file of the kit
        }
        SetEnv(k, v);
        ++set;
    }
    if (set != 0) {
        LOG_INFO(Core, "coop: {} switch(es) from {}\\coop-options.ini ({})", set, Utf8Path(*kit),
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
            g_sub = false, g_swallow = 0, g_focus_to = -1, g_decide = -1, g_sound = nullptr;
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

std::uint64_t OnPad(std::uint64_t buttons, std::int32_t stick[4]) {
    (void)stick;
    std::optional<CoopPauseMenu::Page> open;   // (opened after the lock: CoopPause calls in here)
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
        const std::uint64_t down = buttons & ~g_pad_last;
        g_pad_last = buttons;
        g_swallow &= buttons;   // (let go: the game gets it again)
        const int focused = title ? FocusedIndex(g_screen.load()) : -1;
        const bool menu = g_menu.load();
        const CoopTitleMenu::TitleMenuLook look = CoopTitleMenu::MenuFor(g_any_save, g_sub, g_one_start);
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
            const auto change = [&](bool sub, int focus) {
                g_sub = sub, g_focus_to = focus, g_sound = "button_decide";
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
            };
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
        CoopPause::Open(*open);
    }
    return buttons & ~keep;
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
}

bool SaveWaitWaiting() {
    std::lock_guard lock(g_mutex);
    return g_wait.waiting();
}

bool PlayerChoseLately() {
    std::lock_guard lock(g_mutex);
    return g_choice.Lately(NowMs());
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
