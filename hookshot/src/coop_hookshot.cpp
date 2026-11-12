// SPDX-License-Identifier: GPL-3.0-or-later
//
// Co-op inside Hookshot (docs/hookshot.md): hookshot_coop.dll, loaded by our
// patch's host/coop_bridge.c. It runs lichii/hook's co-op unchanged: the
// emulator's API slice the hook uses is answered by hookshot/compat from the
// bridge's table (CoopRuntime below). What differs from the emulator is only
// how the hook is driven:
// - hos_coop_frame at the every-frame site runs CoopHook::OnFrame, then the
//   callers' game calls (CoopCallers::RunFrame) directly, where the emulator's
//   cave asks them one at a time through `svc #0xf4` (coop_svc.cpp);
// - the AI hold reads the same table the emulator's hold cave reads
//   (coop_hold.h), at the same place in main;
// - settings: coop.ini beside hookshot.exe, then the LICHII_COOP_* variables,
//   as in the emulator (the kit's launcher sets them from coop-options.ini).
#include "coop_hookshot.h"

#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop_callers.h"
#include "coop_cheats.h"
#include "coop_cheats_panel.h"
#include "coop_chatbox.h"
#include "coop_draw.h"
#include "coop_game_build.h"
#include "coop_hold.h"
#include "coop_hook.h"
#include "coop_hud.h"
#include "coop_hud_hearts.h"
#include "coop_puppet.h"
#include "coop_puppet_config.h"
#include "coop_runtime.h"
#include "coop_settings.h"
#include "coop_svc.h"
#include "core/core.h"
#include "hos_coop_api.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

const HosCoopApi* g_api = nullptr;
std::string g_build;
bool g_on = false;
std::atomic<bool> g_present_seen{false};  // our patch's present site has run
bool g_hold = false;
Core::System g_system;

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

bool EnvOn(const char* name) {
    const auto v = Env(name);
    return !v.empty() && v != "0";
}

std::uint64_t SteadyMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

class BridgeGuest final : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        return CoopRuntime::Read(address, out, size);
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        return CoopRuntime::Write(address, in, size);
    }
};

// The game calls the hook's callers ask for this frame, made here.
// The actor a requestCreateActorAsync call names: x1 is {char*, capacity}.
std::string CreateName(const CoopPuppet::Call& c) {
    std::uint64_t text = 0;
    char name[0x41] = {};
    if (!CoopRuntime::Read(c.x[1], &text, sizeof text) || text == 0 ||
        !CoopRuntime::Read(text, name, sizeof name - 1)) {
        return {};
    }
    return std::string(name);
}

// coop_hookshot.h RefuseCreate: a Link-model puppet crashes Hookshot's game.
bool Refused(const CoopPuppet::Call& c) {
    static const bool allow = EnvOn("LICHII_COOP_HOOKSHOT_LINK_PUPPETS");
    if (c.function != CoopRuntime::MainBase() + CoopPuppet::addr::kCreate) {
        return false;
    }
    const std::string actor = CreateName(c);
    if (!CoopHookshot::RefuseCreate(actor, allow)) {
        return false;
    }
    static bool said = false;
    if (!said) {
        said = true;
        LOG_WARNING(Core,
                    "coop: Hookshot: {} not made (a second Link model crashes Hookshot's game); other players "
                    "show as their fairy. LICHII_COOP_HOOKSHOT_LINK_PUPPETS=1 tries them again",
                    actor);
    }
    return true;
}

void RunCalls(void* ctx) {
    BridgeGuest guest;
    CoopCallers::RunFrame(guest, SteadyMs(), CoopSvc::kCallsPerFrame, [ctx](const CoopPuppet::Call& c) {
        if (Refused(c)) {
            return std::optional<std::uint64_t>(0);  // as the game refusing it: asked again later
        }
        HosCoopCall k{};
        k.function = c.function;
        std::memcpy(k.x, c.x.data(), sizeof k.x);
        std::memcpy(k.stack, c.stack.data(), sizeof k.stack);
        k.s0 = c.s0;
        std::uint64_t x0 = 0;
        if (!g_api->call(ctx, &k, &x0)) {
            LOG_WARNING(Core, "coop: no game function at {:#x}", c.function);
            return std::optional<std::uint64_t>();
        }
        return std::optional<std::uint64_t>(x0);
    });
}

}  // namespace

namespace CoopRuntime {
bool Running() {
    return g_api != nullptr;
}
std::uint64_t MainBase() {
    return g_api != nullptr ? g_api->main_base : 0;
}
const std::string& MainBuildHex() {
    return g_build;
}
std::filesystem::path GameDir() {
    if (g_api == nullptr || g_api->game_dir == nullptr) {
        return std::filesystem::path(".");
    }
    // UTF-8 from the bridge: a folder name outside the ANSI code page stays whole.
    const std::string dir = g_api->game_dir;
    return std::filesystem::path(std::u8string(dir.begin(), dir.end()));
}
bool Valid(std::uint64_t address, std::size_t size) {
    // The bridge checks a whole range before it copies: one byte at each end
    // says whether the range is mapped (Hookshot's guest memory has no holes
    // inside a mapped range the hook reads).
    std::uint8_t b = 0;
    return g_api != nullptr && size != 0 && address + size > address && g_api->read(address, &b, 1) != 0 &&
           g_api->read(address + size - 1, &b, 1) != 0;
}
bool Read(std::uint64_t address, void* out, std::size_t size) {
    return g_api != nullptr && g_api->read(address, out, size) != 0;
}
bool Write(std::uint64_t address, const void* in, std::size_t size) {
    return g_api != nullptr && g_api->write(address, in, size) != 0;
}
bool Cas32(std::uint64_t address, std::uint32_t expected, std::uint32_t desired, std::uint32_t& seen) {
    return g_api != nullptr && g_api->cas32(address, expected, desired, &seen) != 0;
}
void Log(const std::string& line) {
    if (g_api != nullptr && g_api->log != nullptr) {
        g_api->log(line.c_str());
    }
}
bool PadButtons(std::uint64_t& out) {
    if (!CoopHookshot::HasPadButtons(g_api)) {
        return false;
    }
    out = g_api->pad_buttons();
    return true;
}
}  // namespace CoopRuntime

// The emulator-only parts of the hook, as Hookshot has them.
namespace CoopSvc {
std::string Refusal() {
    return {};  // nothing to refuse: Hookshot applies no exefs patches (its host/mods.c)
}
}  // namespace CoopSvc

namespace CoopHookshot {

namespace {

// F6 and F7 (CoopDraw::Keys), only while one of this process's windows has
// the keyboard. Windows only: on Linux the list and panel stay hidden.
bool KeyDown(int vk) {
#ifdef _WIN32
    DWORD pid = 0;
    const HWND front = GetForegroundWindow();
    if (front == nullptr || (GetWindowThreadProcessId(front, &pid), pid) != GetCurrentProcessId()) {
        return false;
    }
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
    (void)vk;
    return false;
#endif
}

std::mutex g_keys_mutex;
CoopDraw::Keys g_keys;

// The chat box (coop_chatbox.h): the window's thread types, the game's input
// thread asks whether it has the keyboard, the render thread draws it.
std::mutex g_box_mutex;
CoopChatBox::Box g_box;
unsigned g_box_lead = 0;  // a double-byte code page's lead byte, waiting for its second
// The item cheats panel (coop_cheats_panel.h), under the same mutex.
CoopCheatsPanel::Panel g_cheats;

std::vector<CoopCheatsPanel::Row> CheatRows() {
    static const std::vector<std::string> items = CoopCheats::Items();
    return CoopCheatsPanel::Rows(g_cheats.filter(), items);
}

bool AsyncDown(int vk) {
#ifdef _WIN32
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
    (void)vk;
    return false;
#endif
}

// A WM_CHAR byte of the ANSI window as Unicode; 0 while a lead byte waits.
std::uint32_t FromCodePage(unsigned byte) {
#ifdef _WIN32
    char b[2];
    int n = 0;
    if (g_box_lead != 0) {
        b[n++] = static_cast<char>(g_box_lead);
        g_box_lead = 0;
    } else if (byte >= 0x80 && IsDBCSLeadByte(static_cast<BYTE>(byte))) {
        g_box_lead = byte;
        return 0;
    }
    b[n++] = static_cast<char>(byte);
    wchar_t w[2] = {};
    if (MultiByteToWideChar(CP_ACP, 0, b, n, w, 2) != 1) {
        return 0xFFFD;
    }
    return w[0];
#else
    return byte < 0x80 ? byte : 0xFFFD;
#endif
}

}  // namespace

CoopDraw::Scene DrawScene() {
    CoopDraw::Scene s;
    {
        std::lock_guard lock(g_box_mutex);
        if (g_box.open()) {
            s.box = CoopDraw::ChatBoxView{g_box.text(), g_box.failed()};
        }
        if (g_cheats.open()) {
            CoopDraw::CheatsView v;
            v.filter = g_cheats.filter();
            for (const auto& r : CheatRows()) {
                v.rows.push_back(r.label);
            }
            v.selected = g_cheats.selected();
            v.result = CoopCheats::LastResult();
            s.cheats = std::move(v);
        }
    }
    s.chat = CoopDraw::ShownChat(CoopHook::ChatLines(), s.box.has_value());
    s.markers = CoopHook::CurrentMarkers();
    bool list = false, panel = false;
    {
        std::lock_guard lock(g_keys_mutex);
        static bool panel_env = [] {
            g_keys.panel = EnvOn("LICHII_COOP_DEBUG_PANEL");
            return true;
        }();
        (void)panel_env;
        g_keys.Update(KeyDown(0x75 /* F6 */), KeyDown(0x76 /* F7 */));
        static const bool always = EnvOn("LICHII_COOP_SHOW_PLAYERS");  // tests: no F6 to hold
        list = g_keys.list || always;
        panel = g_keys.panel;
    }
    const auto rows = CoopHook::PlayerList();
    static const bool hud_off = Env("LICHII_COOP_HUD") == "0";
    for (const auto& row : hud_off ? std::vector<CoopHook::PlayerRow>{} : rows) {
        if (row.self) {
            continue;
        }
        CoopDraw::HudBlock b;
        b.name = row.name.empty() ? "Player " + std::to_string(row.number) : row.name;
        if (row.scene && !row.scene->empty()) {
            b.place = CoopHud::PlaceName(*row.scene);
        }
        if (const auto life = CoopHud::PlayerLife(row.number); life && life->max > 0) {
            b.hearts = CoopHudHearts::Quarters(life->life, life->max);
        }
        b.dim = row.state != "connected";
        s.hud.push_back(std::move(b));
    }
    if (list) {
        s.players = rows;
    }
    if (panel) {
        s.panel = CoopHook::OverlayLines();
    }
    return s;
}

bool TypeKey(std::uint32_t vk, bool repeat) {
    if (!g_on) {
        return false;
    }
    std::lock_guard lock(g_box_mutex);
    // The cheats panel (F8, LICHII_COOP_CHEATS=1) when it is open or the chat box is not.
    if (g_cheats.open() || (!g_box.open() && CoopCheats::On())) {
        const auto rows = CheatRows();
        if (g_cheats.Key(vk, repeat, rows.size())) {
            if (const auto i = g_cheats.TakeGive(); i && *i < rows.size()) {
                CoopCheats::Give(rows[*i].flag);
                LOG_INFO(Core, "coop: cheats: give {}", rows[*i].flag);
            }
            return true;
        }
    }
    const bool taken = g_box.Key(vk, repeat);
    if (auto text = g_box.TakeSend()) {
        const bool ok = CoopHook::SendChat(*text);
        g_box.Sent(ok);
        if (!ok) {
            LOG_INFO(Core, "coop: chat not sent (not connected, or refused); the box stays open");
        }
    }
    return taken;
}

bool TypeChar(std::uint32_t byte) {
    if (!g_on) {
        return false;
    }
    std::lock_guard lock(g_box_mutex);
    if (!g_box.open() && !g_cheats.open()) {
        return false;
    }
    const std::uint32_t cp = FromCodePage(byte);
    if (cp == 0) {
        return true;
    }
    if (g_cheats.open()) {
        return g_cheats.Char(cp, CoopChatBox::Utf8(cp));
    }
    return g_box.Char(cp);
}

bool KeysTaken() {
    if (!g_on) {
        return false;
    }
    std::lock_guard lock(g_box_mutex);
    const bool closing = AsyncDown(0x0D) || AsyncDown(0x1B);
    const bool box = g_box.KeysTaken(closing);
    const bool cheats = g_cheats.KeysTaken(closing || AsyncDown(CoopCheatsPanel::kKeyToggle));
    return box || cheats;
}

bool Start(const HosCoopApi* api) {
    if (api == nullptr || api->version != HOS_COOP_API_VERSION || api->size < kFirstApiSize) {
        return false;
    }
    g_api = api;
    g_build = api->main_build_id != nullptr ? api->main_build_id : "";
    if (!BuildSupported(g_build)) {
        LOG_WARNING(Core, "coop: {}", CoopGameBuild::LogRefusal(g_build));
        g_api = nullptr;
        return false;
    }
    LOG_INFO(Core, "coop: Hookshot module, game build {}, main at {:#x}{}", g_build.substr(0, 16), api->main_base,
             HasPadButtons(api) ? "" : "; this Hookshot gives no pad word (own Ocarina notes not sent: build it again)");
    const auto ini = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini";
    std::vector<std::string> errors;
    if (!CoopHook::LoadSettings(ini, errors).enabled() || !errors.empty()) {
        CoopHook::Start();  // logs why co-op is off
        g_api = nullptr;
        return false;
    }
    // The same switches that make the emulator put its cave in.
    if (EnvOn("LICHII_COOP_PUPPET") || EnvOn("LICHII_COOP_DEBUG_PUPPET")) {
        CoopPuppetConfig::Configure();  // as the emulator's cave (coop_puppet_config.h)
    }
    g_hold = EnvOn("LICHII_COOP_OBJECTS_AI_HOLD");
    CoopHook::Start();
    g_on = true;
    return true;
}

void Frame(void* ctx, int site) {
    if (!g_on) {
        return;
    }
    if (site == HOS_COOP_SITE_PRESENT && !g_present_seen.exchange(true)) {
        LOG_INFO(Core, "coop: Hookshot calls co-op at each present (loads and pause included)");
    }
    const FrameWork w = WorkAt(site, g_present_seen.load());
    if (w.on_frame) {
        CoopHook::OnFrame(g_system);
    }
    if (w.calls && ctx != nullptr) {
        RunCalls(ctx);
    }
}

// coop_hold.s: a held component's calc bits become (bits & and) | or.
std::uint32_t Hold(std::uint64_t component, std::uint32_t bits) {
    if (!g_hold || component == 0) {
        return bits;
    }
    const std::uint64_t table = CoopRuntime::MainBase() + CoopHold::kTableOffset;
    std::uint32_t head[3] = {};
    if (!CoopRuntime::Read(table, head, sizeof head) || head[0] == 0) {
        return bits;
    }
    const std::uint32_t count =
        head[0] < CoopHold::kMaxHeld ? head[0] : static_cast<std::uint32_t>(CoopHold::kMaxHeld);
    std::uint64_t held[CoopHold::kMaxHeld] = {};
    if (!CoopRuntime::Read(table + 0x10, held, 8 * count)) {
        return bits;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (held[i] == component) {
            return (bits & head[1]) | head[2];
        }
    }
    return bits;
}

void Stop() {
    if (g_on) {
        g_on = false;
        CoopHook::Shutdown();
    }
}

}  // namespace CoopHookshot

extern "C" {
COOP_HOOKSHOT_EXPORT int hos_coop_start(const HosCoopApi* api) {
    return CoopHookshot::Start(api) ? 1 : 0;
}
COOP_HOOKSHOT_EXPORT void hos_coop_frame(void* ctx, int site) {
    CoopHookshot::Frame(ctx, site);
}
COOP_HOOKSHOT_EXPORT uint32_t hos_coop_hold(uint64_t component, uint32_t bits) {
    return CoopHookshot::Hold(component, bits);
}
COOP_HOOKSHOT_EXPORT void hos_coop_stop(void) {
    CoopHookshot::Stop();
}
// Typing (hos_coop_api.h): the chat box.
COOP_HOOKSHOT_EXPORT int hos_coop_key(uint32_t vk, int repeat) {
    return CoopHookshot::TypeKey(vk, repeat != 0) ? 1 : 0;
}
COOP_HOOKSHOT_EXPORT int hos_coop_char(uint32_t ch) {
    return CoopHookshot::TypeChar(ch) ? 1 : 0;
}
COOP_HOOKSHOT_EXPORT int hos_coop_keys_taken(void) {
    return CoopHookshot::KeysTaken() ? 1 : 0;
}
// Drawing over the picture (hos_coop_draw.h), on Hookshot's render thread.
COOP_HOOKSHOT_EXPORT int hos_coop_draw_wanted(void) {
    return g_on && CoopDraw::Wanted(CoopHookshot::DrawScene()) ? 1 : 0;
}
COOP_HOOKSHOT_EXPORT void hos_coop_draw(const HosCoopDraw* d) {
    if (g_on && d != nullptr) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            LOG_INFO(Core, "coop: drawing over Hookshot's picture: chat lines, name tags ({}x{})",
                     static_cast<int>(d->width), static_cast<int>(d->height));
        }
        CoopDraw::Draw(*d, CoopHookshot::DrawScene());
    }
}
}
