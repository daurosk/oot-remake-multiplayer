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
constexpr std::uint64_t kTitleVtable = 0x84c106e8;    // UITitleScreen
constexpr std::uint64_t kTitleActive = 0x1a8;         // u8: 1 while its step runs its work (f_83426d70)
constexpr std::uint64_t kTitleCount = 0x2c8;          // s32: its buttons
constexpr std::uint64_t kTitleButtons = 0x2d0;        // its buttons' array
constexpr std::uint64_t kTitleFocused = 0x2e0;        // the focused button (f_83426f40)
constexpr std::uint64_t kTitleLayout = 0x28;          // the screen's layout
// A pane's text from a message, as UISystemWindowScreen sets its buttons' (f_83425bc0):
constexpr std::uint64_t kMsgRefInit = 0x80a86f70;     // (ref): {0, 0, -1}, 24 bytes
constexpr std::uint64_t kMsgRefSet = 0x80a8afe0;      // (ref, &"LayoutMsg/...", &"label", 1)
constexpr std::uint64_t kPaneText = 0x80a87d30;       // (layout, "pane/T_Btn_00", ref, 1, 0)
// The title's own co-op rows (tools/puppet_pack/title_menu.py adds them to Title_00 and their
// texts to LayoutMsg/Title_00): Pa_Button_04 Host, _05 Join, _06 Co-op Settings. Each opens
// Hookshot's menu on the Multiplayer tab at that page (coop_pause.h).
constexpr int kFirstRow = 4;
constexpr std::array<const char*, 3> kRowLabels{"0100", "0101", "0102"};
constexpr std::array<CoopPauseMenu::Page, 3> kRowPages{CoopPauseMenu::Page::kHost, CoopPauseMenu::Page::kJoin,
                                                       CoopPauseMenu::Page::kSettings};
constexpr std::uint64_t kFocus = 0x8088ffc0;   // the screens' focus setter (screen, button, 0): Hookshot's oot_menu.c
// The title's row count: its layout's BtnNum animation at a frame (frame N shows N rows), as UISystemWindowScreen
// sets its own (f_83425740 takes it with kAnim, f_83425ad0.. sets it with kAnimFrame and s0).
constexpr std::uint64_t kAnim = 0x8045c7a0;        // (layout, name): the layout's animation of that name
constexpr std::uint64_t kAnimFrame = 0x802f2d90;   // (animation, s0 = frame)
constexpr std::uint64_t kBtnNumName = 0x83e3e439;  // "BtnNum" in the game's read-only data
constexpr float kRowsWithContinue = 3.0f;          // New Game, Options, Continue
constexpr int kContinueButton = CoopTitleMenu::AutoLoad::kContinue;

// --- Switch pad bits (nn::hid::NpadButton) ---
constexpr std::uint64_t kA = 1ull << 0;
constexpr std::uint64_t kMinus = 1ull << 11;

const HosCoopApi* g_api = nullptr;
bool g_native = false;   // LICHII_COOP_TITLE_NATIVE=1: the 4th title button (Pa_Button_03) opens the tab too

std::atomic<std::uint64_t> g_screen{0};
std::atomic<std::uint64_t> g_last_step_ms{0};
std::atomic<bool> g_step_seen{false};
std::atomic<bool> g_rows{false};
std::atomic<bool> g_focus_continue{false};
std::atomic<bool> g_reveal_continue{false};

// Everything below. Never held while calling into CoopPause (it calls in here: StartAutoLoad, StopAutoLoad).
std::mutex g_mutex;
CoopTitleMenu::AutoLoad g_autoload;   // Join -> the host's save loaded in one step
std::uint64_t g_pad_last = 0;
int g_focus_logged = -2;

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
    if (s == 0 || Read64(s) != kTitleVtable) {
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

// The rows' texts, from the game's message files (LayoutMsg/Title_00: labels 0100..0102), set the way
// the game sets its own buttons' (kPaneText). Each needs the strings and the message ref in the game's
// memory: one scratch block (patch 0004) for a row's calls. True when all three were set.
bool SetRowTexts(void* ctx, std::uint64_t screen) {
    const std::uint64_t layout = Read64(screen + kTitleLayout);
    if (ctx == nullptr || layout == 0 || !HasScratch()) {
        return false;
    }
    int set = 0;
    for (std::size_t i = 0; i < kRowLabels.size(); ++i) {
        // [0x00] the ref, [0x20] &path, [0x28] &label, [0x30] path, [0x50] label, [0x60] pane
        std::array<std::uint8_t, 0x90> b{};
        const std::uint64_t base = g_api->scratch(ctx, b.data(), b.size());   // (where it goes: the same each time)
        if (base == 0) {
            return false;
        }
        const std::string path = "LayoutMsg/Title_00", label = kRowLabels[i];
        const std::string pane = fmt::format("Pa_Button_{:02d}/T_Btn_00", kFirstRow + static_cast<int>(i));
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
        k.function = kMsgRefInit;
        k.x[0] = base;
        if (!g_api->call(ctx, &k, &x0)) {
            return false;
        }
        k = HosCoopCall{};
        k.function = kMsgRefSet;
        k.x[0] = base, k.x[1] = base + 0x20, k.x[2] = base + 0x28, k.x[3] = 1;
        if (!g_api->call(ctx, &k, &x0)) {
            return false;
        }
        k = HosCoopCall{};
        k.function = kPaneText;
        k.x[0] = layout, k.x[1] = at_pane, k.x[2] = base, k.x[3] = 1, k.x[4] = 0;
        if (!g_api->call(ctx, &k, &x0)) {
            return false;
        }
        ++set;
    }
    return set == static_cast<int>(kRowLabels.size());
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

void ApplyKitOptions() {
    const auto kit = FindKit();
    if (!kit) {
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
        LOG_INFO(Core, "coop: {} switch(es) from {}\\coop-options.ini (the game was started without the kit's launcher)",
                 set, Utf8Path(*kit));
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
        if (count >= kFirstRow + static_cast<std::int32_t>(kRowLabels.size())) {
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
    if (event == HOS_COOP_TITLE_STEP && g_reveal_continue.exchange(false) && ctx != nullptr) {
        // Continue shown: a joiner with no save of their own got a title without it, and the host's save came after.
        char name[8] = {};
        const std::uint64_t layout = Read64(screen + kTitleLayout);
        if (layout != 0 && CoopRuntime::Read(kBtnNumName, name, 7) && std::strcmp(name, "BtnNum") == 0) {
            HosCoopCall k{};
            k.function = kAnim;
            k.x[0] = layout, k.x[1] = kBtnNumName;
            std::uint64_t anim = 0;
            if (g_api->call(ctx, &k, &anim) && anim != 0) {
                k = HosCoopCall{};
                k.function = kAnimFrame;
                k.x[0] = anim;
                k.s0 = kRowsWithContinue;
                std::uint64_t x0 = 0;
                g_api->call(ctx, &k, &x0);
                LOG_INFO(Core, "coop: title: the title's Continue shown for the host's save");
            }
        }
    }
    if (event == HOS_COOP_TITLE_STEP && g_focus_continue.exchange(false) && ctx != nullptr) {
        // The cursor to Continue for the host's save (AutoLoad), as the game's own screens move it.
        std::int32_t count = 0;
        CoopRuntime::Read(screen + kTitleCount, &count, sizeof count);
        const std::uint64_t button =
            count > kContinueButton ? Read64(Read64(screen + kTitleButtons) + 8ull * kContinueButton) : 0;
        if (button != 0) {
            HosCoopCall k{};
            k.function = kFocus;
            k.x[0] = screen, k.x[1] = button, k.x[2] = 0;
            std::uint64_t x0 = 0;
            g_api->call(ctx, &k, &x0);
            LOG_INFO(Core, "coop: title: the cursor on Continue for the host's save");
        }
    }
    if (event == HOS_COOP_TITLE_STEP) {
        if (!g_step_seen.exchange(true)) {
            LOG_INFO(Core, "coop: title: the title screen's step runs");
        }
        g_last_step_ms = NowMs();
        const int f = FocusedIndex(screen);
        std::lock_guard lock(g_mutex);
        if (f != g_focus_logged) {
            g_focus_logged = f;
            LOG_INFO(Core, "coop: title: the title's focus is on button {}", f);
        }
    }
}

std::uint64_t OnPad(std::uint64_t buttons, std::int32_t stick[4]) {
    (void)stick;
    std::optional<CoopPauseMenu::Page> open;   // (opened after the lock: CoopPause calls in here)
    {
        std::lock_guard lock(g_mutex);
        const bool title = TitleUp();
        CoopSave::NoteTitle(title);   // (a guest switches to the host's save only on the title's menu)
        bool auto_a = false;          // this poll's A is AutoLoad's, not the player's
        if (g_autoload.active() && CoopSave::HostHasNoSave()) {
            g_autoload.NoSave();
            LOG_INFO(Core, "coop: title: host's save: {}", g_autoload.Line());
        }
        if (g_autoload.active()) {
            const auto tick = g_autoload.Step(NowMs(), CoopSave::HostSaveReady(), title, InGame());
            if (tick.reveal_continue) {
                g_reveal_continue = true;   // (made at the title's next step, with its game context)
            }
            if (tick.focus_continue) {
                g_focus_continue = true;   // (made at the title's next step, with its game context)
            }
            // Only on Continue: if the title kept it hidden (no save when it opened), the cursor stays elsewhere
            // (New Game) and no A goes in; the give-up line then says to press Continue.
            if (tick.press_a && CoopTitleMenu::AutoLoad::PressAllowed(title, title ? FocusedIndex(g_screen.load()) : -1)) {
                buttons |= kA;
                auto_a = true;
            }
            if (!g_autoload.active()) {
                LOG_INFO(Core, "coop: title: host's save: {}", g_autoload.Line().empty() ? "loaded" : g_autoload.Line());
                if (!g_autoload.Line().empty()) {
                    // The title said "Continue plays your own save": a save coming later must not switch it.
                    CoopSave::CancelPendingSwitch("the host's save did not come in time");
                }
            }
        }
        const std::uint64_t down = buttons & ~g_pad_last;
        g_pad_last = buttons;
        if ((down & kA) != 0 && title) {
            // The player's own A on the title (not on our co-op rows): they pick their own file or a new game while
            // the host's save is still coming. No switch after this: it would change the save folder under the load.
            const int pf = FocusedIndex(g_screen.load());
            const bool our_row = g_rows && pf >= kFirstRow && pf < kFirstRow + static_cast<int>(kRowLabels.size());
            if (CoopTitleMenu::AutoLoad::PlayerPicksFile(true, auto_a, pf, our_row)) {
                CoopSave::CancelPendingSwitch("the player chose a file on the title");
                if (g_autoload.waiting()) {
                    g_autoload.PlayerChose();
                    LOG_INFO(Core, "coop: title: host's save: not waited for (the player chose a file)");
                }
            }
        }
        if (!HasScratch() || !title || g_autoload.active()) {
            return buttons;
        }
        const int f = (down & kA) != 0 ? FocusedIndex(g_screen.load()) : -1;
        if (g_rows && f >= kFirstRow && f < kFirstRow + static_cast<int>(kRowLabels.size())) {
            // The title's own rows: the game takes the A too (the button's own press look and sound), and its
            // title sequence has nothing for these buttons; the Multiplayer tab opens at the row's page.
            const auto row = static_cast<std::size_t>(f - kFirstRow);
            LOG_INFO(Core, "coop: title: the title's row {} pressed", kRowLabels[row]);
            open = kRowPages[row];
        } else if ((!g_rows && (down & kMinus) != 0) || (g_native && f == 3)) {
            open = CoopPauseMenu::Page::kMain;   // the Minus prompt, or the 4th title button (LICHII_COOP_TITLE_NATIVE)
        }
    }
    if (open) {
        CoopPause::Open(*open);
    }
    return buttons;
}

bool StartAutoLoad(const std::string& host) {
    std::lock_guard lock(g_mutex);
    g_autoload.Start(NowMs(), host);
    return TitleUp();
}

void StopAutoLoad() {
    std::lock_guard lock(g_mutex);
    g_autoload.Stop();
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


// Only the host's save loading ("Loading Saria's save..."): the title has no co-op hint of its own (the owner:
// the title looks like the plain game; Multiplayer is the pause menu's first tab).
std::optional<CoopDraw::TitleView> View() {
    std::lock_guard lock(g_mutex);
    if (!HasScratch() || !TitleUp()) {
        return std::nullopt;
    }
    if (const std::string line = g_autoload.Line(); !line.empty()) {
        return CoopDraw::TitleView{line};   // ("Loading Saria's save...")
    }
    return std::nullopt;
}

}  // namespace CoopTitle
