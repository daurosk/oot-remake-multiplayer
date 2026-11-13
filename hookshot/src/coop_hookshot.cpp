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

#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop_callers.h"
#include "coop_camera.h"
#include "coop_cheats.h"
#include "coop_cheats_panel.h"
#include "coop_chatbox.h"
#include "coop_draw.h"
#include "coop_game_build.h"
#include "coop_save.h"
#include "coop_hold.h"
#include "coop_mods_runtime.h"
#include "coop_hook.h"
#include "coop_hud.h"
#include "coop_hud_hearts.h"
#include "coop_png.h"
#include "coop_puppet.h"
#include "coop_puppet_config.h"
#include "coop_runtime.h"
#include "coop_settings.h"
#include "coop_svc.h"
#include "coop_pause.h"
#include "coop_title.h"
#include "core/core.h"
#include "coop_host_menu.h"
#include "hos_coop_api.h"
#include "hos_coop_host.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
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

// Our patch 0002 (nx2/coop_puppet_guard.py) is in this hookshot.exe: its log text is in the exe's read-only data.
// (Linux: always true, see the end.)
bool PuppetGuardInExe() {
#ifdef _WIN32
    static const char kMark[] = "coop: puppet guard: actor";
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 || (sec->Characteristics & IMAGE_SCN_MEM_READ) == 0) {
            continue;
        }
        const std::string_view data(reinterpret_cast<const char*>(base + sec->VirtualAddress),
                                    std::min<std::size_t>(sec->Misc.VirtualSize, sec->SizeOfRawData));
        if (data.find(std::string_view(kMark, sizeof(kMark) - 1)) != std::string_view::npos) {
            return true;
        }
    }
    return false;
#else
    return true;  // (Linux: Hookshot maps the whole guest window readable, so a puppet's null reads give 0 there
                  //  as on the emulator, with or without patch 0002; untested on Linux)
#endif
}

// coop_hookshot.h AllowLinkPuppets / RefuseCreate: without patch 0002 a Link-model puppet crashes Hookshot's game.
bool Refused(const CoopPuppet::Call& c) {
    static const bool allow = [] {
        const bool guard = PuppetGuardInExe();
        const bool on = CoopHookshot::AllowLinkPuppets(Env("LICHII_COOP_HOOKSHOT_LINK_PUPPETS"), guard);
        LOG_INFO(Core, "coop: Hookshot: other players as Link {} (puppet guard {} this hookshot.exe{})",
                 on ? "on" : "off", guard ? "in" : "NOT in",
                 guard ? "" : ": build it again with Build-Hookshot-Coop for patch 0002");
        return on;
    }();
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
                    "coop: Hookshot: {} not made (this hookshot.exe has no puppet guard, or "
                    "LICHII_COOP_HOOKSHOT_LINK_PUPPETS=0); other players show as their fairy",
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

// Host and Join from the game's menu (hos_coop_host.h, coop_host_menu.h).
CoopHostMenu::Menu& HostMenu() {
    static CoopHostMenu::Menu menu([] {
        CoopHostMenu::Hooks h;
        h.connect = [](const CoopHostMenu::Target& t) {
            const auto ini = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini";
            std::vector<std::string> errors;
            CoopHook::Settings s = CoopHook::LoadSettings(ini, errors);  // the colour, as the player set it
            s.server = t.server;
            s.port = t.port;
            s.name = t.name;
            s.password = t.password;
            return CoopHook::Begin(std::move(s));
        };
        h.disconnect = [] { CoopHook::End(); };
        h.client_id = [] { return CoopTitle::ClientId(); };
        h.log = [](const std::string& line) { LOG_INFO(Core, "{}", line); };
        h.default_name = [] {
            const auto ini = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini";
            std::vector<std::string> errors;
            return CoopHook::LoadSettings(ini, errors).name;
        };
        return h;
    }());
    return menu;
}

namespace {

// F6 and F7 (CoopDraw::Keys), only while one of this process's windows has
// the keyboard. On Linux through the table's key_down (the SDL window's
// keys, none while it does not have the keyboard).
bool KeyDown(int vk) {
#ifdef _WIN32
    DWORD pid = 0;
    const HWND front = GetForegroundWindow();
    if (front == nullptr || (GetWindowThreadProcessId(front, &pid), pid) != GetCurrentProcessId()) {
        return false;
    }
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
    return HasKeyDown(g_api) && g_api->key_down(static_cast<std::uint32_t>(vk)) != 0;
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

// The game look (coop_draw.h Look): the HUD pack's font and heart pictures,
// loaded through hos_coop_draw2's font and image calls on the render thread
// the first time it draws. Off for good when there is no pack, a file will
// not load, or Hookshot is older (it calls hos_coop_draw only).
enum : int { kLookNotYet = 0, kLookOn = 1, kLookOff = 2 };
std::atomic<int> g_look_state{kLookNotYet};
CoopDraw::Look g_look;  // the render thread's

std::string Utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

void LoadLook(const HosCoopDraw& d) {
    const auto dir = CoopHud::Folder();  // it logs why not
    if (!dir) {
        g_look_state = kLookOff;
        return;
    }
    CoopDraw::Look l;
    l.regular = d.font(d.user, Utf8(*dir / "font-regular.otf").c_str());
    l.medium = d.font(d.user, Utf8(*dir / "font-medium.otf").c_str());
    if (l.medium == 0) {
        l.medium = l.regular;
    }
    std::string missing = l.regular == 0 ? "font-regular.otf" : "";
    for (int q = 0; q < 5; ++q) {
        const std::string name = "heart-" + std::to_string(q) + ".png";
        const auto img = CoopPng::Load(*dir / name);
        l.hearts[q] = img ? d.image(d.user, img->width, img->height, img->rgba.data()) : 0;
        if (l.hearts[q] == 0 && missing.empty()) {
            missing = name;
        }
    }
    if (!l.On()) {
        LOG_WARNING(Core, "coop: hud: {} in {} could not be loaded; plain look", missing, dir->string());
        g_look_state = kLookOff;
        return;
    }
    LOG_INFO(Core, "coop: hud: game look on Hookshot: the game font and heart pictures loaded");
    g_look = l;
    g_look_state = kLookOn;
}

// The game look may be used: not known to be off, and the pack is there.
bool LookPossible() {
    return g_look_state.load() != kLookOff && CoopHud::Folder().has_value();
}

std::vector<CoopCheatsPanel::Row> CheatRows() {
    static const std::vector<std::string> items = CoopCheats::Items();
    return CoopCheatsPanel::Rows(g_cheats.filter(), items);
}

// The flag Enter or a click chose, given (under g_box_mutex).
void GiveChosen(const std::vector<CoopCheatsPanel::Row>& rows) {
    if (const auto i = g_cheats.TakeGive(); i && *i < rows.size()) {
        CoopCheats::Give(rows[*i].flag);
        LOG_INFO(Core, "coop: cheats: give {}", rows[*i].flag);
    }
}

// The picture's size at the last draw: where the mouse is on it.
std::atomic<float> g_drawn_w{0.0f};
std::atomic<float> g_drawn_h{0.0f};

bool AsyncDown(int vk) {
#ifdef _WIN32
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
    return HasKeyDown(g_api) && g_api->key_down(static_cast<std::uint32_t>(vk)) != 0;
#endif
}

// A WM_CHAR byte of the ANSI window as Unicode; 0 while a lead byte waits.
// On Linux the window hands code points already (SDL's typed text).
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
    return byte <= 0x10FFFF && (byte < 0xD800 || byte > 0xDFFF) ? byte : 0xFFFD;
#endif
}

}  // namespace

CoopDraw::Scene DrawScene() {
    CoopDraw::Scene s;
    s.title = CoopTitle::View();
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
    auto rows = CoopHook::PlayerList();
    // The HUD blocks, players' colours and place names are the game look's
    // (as the co-op window: LICHII_COOP_HUD=0 or no pack, the plain look).
    const bool game = LookPossible();
    if (game) {
        std::uint8_t self = 0;
        for (const auto& row : rows) {
            if (row.self) {
                self = row.number;
            }
        }
        for (std::uint8_t n = 1; n <= 4; ++n) {  // a tag with no name
            s.colours["P" + std::to_string(n)] = CoopDraw::Lifted(CoopPuppet::NameColor(n, self));
        }
        if (self != 0) {
            s.colours["you"] = CoopDraw::Lifted(CoopPuppet::NameColor(self, self));
        }
        for (const auto& row : rows) {
            if (row.number != 0 && !row.name.empty()) {
                s.colours[row.name] = CoopDraw::Lifted(CoopPuppet::NameColor(row.number, self));
            }
        }
    }
    for (const auto& row : game ? rows : std::vector<CoopHook::PlayerRow>{}) {
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
        if (game) {
            for (auto& row : rows) {
                if (row.scene && !row.scene->empty()) {
                    row.scene = CoopHud::PlaceName(*row.scene);
                }
            }
        }
        s.players = rows;
    }
    if (panel) {
        s.panel = CoopHook::OverlayLines();
    }
    return s;
}

bool TypeKey(std::uint32_t vk, bool repeat) {
    if (CoopPause::Key(vk, repeat)) {   // the Multiplayer tab's field, while it is typed into
        return true;
    }
    if (!g_on) {
        return false;
    }
    std::lock_guard lock(g_box_mutex);
    // The cheats panel (F8, LICHII_COOP_CHEATS=1) when it is open or the chat box is not.
    if (g_cheats.open() || (!g_box.open() && CoopCheats::On())) {
        const auto rows = CheatRows();
        if (g_cheats.Key(vk, repeat, rows.size())) {
            GiveChosen(rows);
            return true;
        }
    }
    static const bool keyed = [] {  // LICHII_COOP_CHAT_KEY: the key that opens the box (T unless set)
        const std::string v = Env("LICHII_COOP_CHAT_KEY");
        if (v.empty()) {
            return true;
        }
        if (const auto k = CoopChatBox::ParseOpenKey(v)) {
            g_box.SetOpenKey(*k);
            LOG_INFO(Core, "coop: chat box key: {}", *k == 0 ? std::string("off") : v);
        } else {
            LOG_WARNING(Core, "coop: LICHII_COOP_CHAT_KEY={} is not a letter, a digit, F1 to F12 or off; T opens the chat box", v);
        }
        return true;
    }();
    (void)keyed;
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
    if (!CoopPause::KeysTaken() && !g_box.open() && !g_cheats.open()) {
        return false;
    }
    const std::uint32_t cp = FromCodePage(byte);
    if (cp == 0) {
        return true;
    }
    if (CoopPause::Char(cp)) {
        return true;
    }
    if (g_cheats.open()) {
        return g_cheats.Char(cp, CoopChatBox::Utf8(cp));
    }
    return g_box.Char(cp);
}

bool Mouse(float fx, float fy, int event) {
    if (!g_on || event < HOS_COOP_MOUSE_MOVE || event > HOS_COOP_MOUSE_WHEEL_DOWN) {
        return false;
    }
    std::lock_guard lock(g_box_mutex);
    if (!g_cheats.open()) {
        return false;
    }
    const auto rows = CheatRows();
    const float w = g_drawn_w.load(), h = g_drawn_h.load();
    std::optional<std::size_t> row;
    if (w > 0 && h > 0) {
        CoopDraw::CheatsView v;  // its layout only: how many rows, whether a result line shows
        v.rows.resize(rows.size());
        v.result = CoopCheats::LastResult();
        row = CoopDraw::CheatsRowAt(w, h, v, fx * w, fy * h);
    }
    static constexpr CoopCheatsPanel::Mouse kWhat[] = {CoopCheatsPanel::Mouse::kMove, CoopCheatsPanel::Mouse::kLeft,
                                                      CoopCheatsPanel::Mouse::kWheelUp,
                                                      CoopCheatsPanel::Mouse::kWheelDown};
    const bool taken = g_cheats.Pointer(kWhat[event], row, rows.size());
    GiveChosen(rows);
    return taken;
}

bool KeysTaken() {
    if (CoopPause::KeysTaken()) {
        return true;
    }
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
    // Started without the kit's launcher: its coop-options.ini switches still count.
    CoopTitle::ApplyKitOptions();
    const auto ini = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini";
    std::vector<std::string> errors;
    // The Multiplayer tab (patch 0004's Hookshot and later, or HOS_COOP_MENU=1): the module stays loaded with co-op idle
    // until the player hosts or joins from the tab (coop_pause.h, hos_coop_host.h).
    const bool menu = EnvOn("HOS_COOP_MENU") || TitleMenuPossible(api, Env("LICHII_COOP_TITLE_MENU"));
    const bool server = CoopHook::LoadSettings(ini, errors).enabled() && errors.empty();
    for (const auto& e : errors) {
        LOG_WARNING(Core, "coop: {}", e);
    }
    if (!menu && !server) {
        CoopHook::Start();  // logs why co-op is off
        g_api = nullptr;
        return false;
    }
    if (!server) {
        LOG_INFO(Core, "coop: no server in coop.ini: co-op idle until Host or Join on the menu's Multiplayer tab");
    }
    // The same switches that make the emulator put its cave in.
    if (EnvOn("LICHII_COOP_PUPPET") || EnvOn("LICHII_COOP_DEBUG_PUPPET")) {
        CoopPuppetConfig::Configure();  // as the emulator's cave (coop_puppet_config.h)
    }
    g_hold = EnvOn("LICHII_COOP_OBJECTS_AI_HOLD");
    CoopHook::Start();
    g_on = true;
    CoopModsRuntime::Start();  // the mods folder and the code mods (docs/mods.md)
    CoopTitle::Init(api);
    CoopPause::Init(server);
    return true;
}

void Frame(void* ctx, int site) {
    if (!g_on) {
        return;
    }
    if (site == HOS_COOP_SITE_FRAME) {
        CoopModsRuntime::Frame();
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
    CoopModsRuntime::Stop();
    HostMenu().Leave();  // our own server goes after our own client
    if (g_on) {
        g_on = false;
        CoopHook::Shutdown();
    }
}

void NoGameLook() {
    g_look_state = kLookOff;
}

void NoteDrawn(const HosCoopDraw& d) {
    g_drawn_w = d.width;
    g_drawn_h = d.height;
}

void DrawWithLook(const HosCoopDraw& d) {
    NoteDrawn(d);
    if (g_look_state.load() == kLookNotYet) {
        if (d.font && d.use_font && d.image && d.draw_image) {
            LoadLook(d);
        } else {
            g_look_state = kLookOff;
        }
    }
    CoopDraw::Draw(d, DrawScene(), g_look_state.load() == kLookOn ? g_look : CoopDraw::Look{});
}

}  // namespace CoopHookshot

extern "C" {
// The host's save for a guest (issue 167): hookshot.exe's patch 0008 exports the account save's folder and a
// re-mount of it; with them a guest switches to the co-op folder when it joins (CoopSave, save::folder_choice).
// An exe without patch 0008: CoopSave as on a fixed folder (LICHII_COOP_HOST_SAVE, else the player's own).
void UseSaveSwitch() {
#ifdef _WIN32
    const HMODULE exe = GetModuleHandleW(nullptr);
    using RootFn = int (*)(char*, size_t);
    using RemountFn = int (*)(const char*);
    const auto root = reinterpret_cast<RootFn>(reinterpret_cast<void*>(GetProcAddress(exe, "hos_coop_fs_save_root")));
    const auto remount = reinterpret_cast<RemountFn>(reinterpret_cast<void*>(GetProcAddress(exe, "hos_coop_fs_save_remount")));
#else
    using RootFn = int (*)(char*, size_t);
    using RemountFn = int (*)(const char*);
    const auto root = reinterpret_cast<RootFn>(dlsym(RTLD_DEFAULT, "hos_coop_fs_save_root"));
    const auto remount = reinterpret_cast<RemountFn>(dlsym(RTLD_DEFAULT, "hos_coop_fs_save_remount"));
#endif
    if (!root || !remount) {
        LOG_INFO(Core, "coop: save: this Hookshot has no save-folder switch (patch 0008): the host's save goes only to "
                       "a game started with LICHII_COOP_HOST_SAVE=1");
        return;
    }
    CoopSave::SetSwitch(
        [root] {
            char buf[1024];
            return root(buf, sizeof buf) ? std::string(buf) : std::string();
        },
        [remount](const std::string& dir) { return remount(dir.c_str()) != 0; });
}

COOP_HOOKSHOT_EXPORT int hos_coop_start(const HosCoopApi* api) {
    if (!CoopHookshot::Start(api)) return 0;
    UseSaveSwitch();
    return 1;
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
// Host and Join from the game's menu (hos_coop_host.h).
COOP_HOOKSHOT_EXPORT int hos_coop_host_start(const HosCoopHostOptions* options) {
    return CoopHookshot::HostMenu().HostStart(options);
}
COOP_HOOKSHOT_EXPORT int hos_coop_join(const HosCoopJoinOptions* options) {
    return CoopHookshot::HostMenu().Join(options);
}
COOP_HOOKSHOT_EXPORT int hos_coop_host_answer(const char* answer_code) {
    return CoopHookshot::HostMenu().Answer(answer_code);
}
COOP_HOOKSHOT_EXPORT void hos_coop_leave(void) {
    CoopHookshot::HostMenu().Leave();
}
COOP_HOOKSHOT_EXPORT void hos_coop_status(HosCoopStatus* out) {
    CoopHookshot::HostMenu().Status(out);
}
// Typing (hos_coop_api.h): the chat box.
COOP_HOOKSHOT_EXPORT int hos_coop_key(uint32_t vk, int repeat) {
    return CoopHookshot::TypeKey(vk, repeat != 0) || CoopModsRuntime::Key(vk, repeat != 0) ? 1 : 0;
}
COOP_HOOKSHOT_EXPORT int hos_coop_char(uint32_t ch) {
    return CoopHookshot::TypeChar(ch) ? 1 : 0;
}
COOP_HOOKSHOT_EXPORT int hos_coop_keys_taken(void) {
    return CoopHookshot::KeysTaken() ? 1 : 0;
}
// The mouse (hos_coop_api.h): the cheats panel's clicks.
COOP_HOOKSHOT_EXPORT int hos_coop_mouse(float fx, float fy, int event) {
    return CoopHookshot::Mouse(fx, fy, event) ? 1 : 0;
}
// Drawing over the picture (hos_coop_draw.h), on Hookshot's render thread.
// The title menu (hos_coop_api.h, patch 0004): the game's title screen and the pad first.
COOP_HOOKSHOT_EXPORT void hos_coop_title(void* ctx, uint64_t screen, int event) {
    if (g_on) {
        CoopTitle::OnTitle(ctx, screen, event);
    }
}
// The Multiplayer tab of Hookshot's menu (hos_coop_menu.h, patch 0012), from Hookshot's menu tick and present thread.
COOP_HOOKSHOT_EXPORT int hos_coop_menu_tabs(void) {
    return g_on ? CoopPause::Tabs() : 0;
}
COOP_HOOKSHOT_EXPORT const char* hos_coop_menu_tab_name(int tab) {
    return g_on ? CoopPause::TabName(tab) : nullptr;
}
COOP_HOOKSHOT_EXPORT int hos_coop_menu_page(int tab, HosCoopMenuPage* out) {
    return g_on && CoopPause::Page(tab, out) ? 1 : 0;
}
COOP_HOOKSHOT_EXPORT int hos_coop_menu_input(int tab, int event, int arg, int arg2) {
    return g_on && CoopPause::Input(tab, event, arg, arg2) ? 1 : 0;
}
COOP_HOOKSHOT_EXPORT int hos_coop_menu_wanted(void) {
    return g_on ? CoopPause::Wanted() : HOS_COOP_MENU_WANT_NOTHING;
}
COOP_HOOKSHOT_EXPORT uint64_t hos_coop_pad(uint64_t buttons, int32_t stick[4]) {
    return g_on && stick != nullptr ? CoopTitle::OnPad(buttons, stick) : buttons;
}
COOP_HOOKSHOT_EXPORT int hos_coop_draw_wanted(void) {
    return g_on && (CoopDraw::Wanted(CoopHookshot::DrawScene()) || CoopModsRuntime::OverlayWanted()) ? 1 : 0;
}
COOP_HOOKSHOT_EXPORT void hos_coop_draw(const HosCoopDraw* d) {
    if (g_on && d != nullptr) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            LOG_INFO(Core, "coop: drawing over Hookshot's picture: chat lines, name tags ({}x{}); plain look: "
                     "this Hookshot has no hos_coop_draw2 (build it again with our patch for the game look)",
                     static_cast<int>(d->width), static_cast<int>(d->height));
        }
        CoopHookshot::NoGameLook();
        CoopHookshot::NoteDrawn(*d);
        // An older Hookshot's table ends at text_width: the rest stays empty.
        HosCoopDraw t{};
        t.width = d->width;
        t.height = d->height;
        t.user = d->user;
        t.rect = d->rect;
        t.circle = d->circle;
        t.triangle = d->triangle;
        t.text = d->text;
        t.text_width = d->text_width;
        CoopDraw::Draw(t, CoopHookshot::DrawScene());
        CoopModsRuntime::Overlay(t);
    }
}
// The game's camera for the name tags (hos_coop_draw.h), on the thread that records Hookshot's draws: published as
// the emulator's renderer does (lichii/hook/coop_camera_track.h), the largest scene target of a frame. The game's
// clip y runs up, as on the emulator ("clip y up"); LICHII_COOP_CAMERA_FLIP_Y turns the markers over if not.
COOP_HOOKSHOT_EXPORT int hos_coop_camera_wanted(uint64_t frame, float width, float height) {
    return g_on && CoopCamera::Wanted() && CoopCamera::Better(frame, width, height) ? 1 : 0;
}
COOP_HOOKSHOT_EXPORT void hos_coop_camera(uint64_t frame, const float* block, float width, float height) {
    if (!g_on || !CoopHookshot::CameraBlockOk(block, width, height)) {
        return;
    }
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) {
        LOG_INFO(Core, "coop: Hookshot hands co-op the game's camera ({}x{} scene): name tags on",
                 static_cast<int>(width), static_cast<int>(height));
    }
    CoopCamera::Publish(frame, block + CoopHookshot::kCameraMatrix, width, height, false);
}
COOP_HOOKSHOT_EXPORT void hos_coop_draw2(const HosCoopDraw* d) {
    if (g_on && d != nullptr) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            LOG_INFO(Core, "coop: drawing over Hookshot's picture: chat lines, name tags ({}x{})",
                     static_cast<int>(d->width), static_cast<int>(d->height));
        }
        CoopHookshot::DrawWithLook(*d);
        CoopModsRuntime::Overlay(*d);
    }
}
}
