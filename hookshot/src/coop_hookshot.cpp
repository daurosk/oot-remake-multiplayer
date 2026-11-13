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
// - settings: coop.ini beside longshot.exe, then the LICHII_COOP_* variables,
//   as in the emulator (the kit's launcher sets them from coop-options.ini).
#include "coop_hookshot.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <cstddef>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop_callers.h"
#include "coop_camera.h"
#include "coop_cheats.h"
#include "coop_cheats_panel.h"
#include "coop_chatbox.h"
#include "coop_draw.h"
#include "coop_flag_native.h"
#include "coop_flags.h"
#include "coop_game_build.h"
#include "coop_save.h"
#include "coop_hold.h"
#include "coop_counters.h"
#include "coop_difficulty.h"
#include "coop_actor_events.h"
#include "coop_boss_native.h"
#include "coop_dbno_game.h"
#include "coop_enemy_shots.h"
#include "coop_mods_runtime.h"
#include "coop_hook.h"
#include "coop_hud.h"
#include "coop_hud_hearts.h"
#include "coop_maps.h"
#include "coop_live_events.h"
#include "coop_objects_sync.h"
#include "coop_input.h"
#include "coop_look.h"
#include "coop_local_game.h"
#include "coop_local_layer.h"
#include "coop_local_probe.h"
#include "coop_player_input.h"
#include "coop_png.h"
#include "coop_puppet.h"
#include "coop_puppet_config.h"
#include "coop_runtime.h"
#include "coop_settings.h"
#include "coop_svc.h"
#include "coop_time.h"
#include "coop_pause.h"
#include "coop_title.h"
#include "core/core.h"
#include "coop_host_menu.h"
#include "hos_coop_api.h"
#include "hos_coop_actors.h"
#include "hos_coop_bosses.h"
#include "hos_coop_pads.h"
#include "hos_coop_host.h"
#include "hos_coop_world.h"

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
std::atomic<bool> g_on{false};  // (read on the game's threads by the world hooks)
std::atomic<std::uint32_t> g_world_version{0};  // Hookshot's HOS_COOP_WORLD_VERSION, 0 until it says hello
std::atomic<std::uint64_t> g_world_wraps{0};    // the wraps that Hookshot has, a bit for each id
std::atomic<bool> g_present_seen{false};  // our patch's present site has run
std::atomic<bool> g_flag_setters{false};  // our flag writes go through the game's setters (coop_flag_native.h)
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

// Our patch 0002 (nx2/coop_puppet_guard.py) is in this longshot.exe: its log text is in the exe's read-only data.
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
        LOG_INFO(Core, "coop: Hookshot: other players as Link {} (puppet guard {} this longshot.exe{})",
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
                    "coop: Hookshot: {} not made (this longshot.exe has no puppet guard, or "
                    "LICHII_COOP_HOOKSHOT_LINK_PUPPETS=0); other players show as their fairy",
                    actor);
    }
    return true;
}

// True while this thread runs one of the module's own game calls: a create
// or a delete the game asks for inside it is ours (a copy, a remove), not
// the game's own doing.
thread_local bool t_own_call = false;
struct OwnCall {
    OwnCall() { t_own_call = true; }
    ~OwnCall() { t_own_call = false; }
    OwnCall(const OwnCall&) = delete;
    OwnCall& operator=(const OwnCall&) = delete;
};

// The flag writes that wait for the game's own setters (coop_flag_native.h), made here on the calc thread. A setter
// that cannot be called (no function there: another build of the lift) ends this: the writes go into the game's
// queues again, the ones that wait first.
void RunFlagSetters(void* ctx) {
    if (!g_flag_setters.load(std::memory_order_relaxed)) {
        return;
    }
    CoopHook::EmulatorMemory memory(g_system.ApplicationMemory(), CoopRuntime::MainBase());
    const bool ok = CoopFlagNative::RunSetters(
        memory, [ctx](std::uint64_t function, std::uint64_t x0, std::uint64_t x1, std::uint64_t x2) {
            HosCoopCall k{};
            k.function = function;
            k.x[0] = x0;
            k.x[1] = x1;
            k.x[2] = x2;
            std::uint64_t result = 0;
            return g_api->call(ctx, &k, &result) != 0;
        });
    if (ok) {
        return;
    }
    g_flag_setters.store(false);
    coop::host::GameFlags::use_calls(nullptr);
    std::size_t moved = 0, lost = 0;
    CoopFlagNative::Calls().run(coop::host::GameFlagCalls::kMost, [&](const coop::host::GameFlagWrite& w) {
        using coop::host::GameFlags;
        using coop::host::GameFlagType;
        const auto hash = static_cast<std::uint32_t>(w.id);
        bool done = false;
        switch (w.type) {
        case GameFlagType::boolean:
            done = GameFlags::set_bool(memory, hash, w.value != 0);
            break;
        case GameFlagType::int32:
            done = GameFlags::set_int(memory, hash, static_cast<std::int32_t>(w.value));
            break;
        case GameFlagType::enum32:
            done = GameFlags::set_enum(memory, hash, w.value);
            break;
        case GameFlagType::uint32:
            done = GameFlags::set_uint(memory, hash, w.value);
            break;
        case GameFlagType::key64:
            done = GameFlags::set_key64(memory, w.id, w.value != 0);
            break;
        }
        ++(done ? moved : lost);
        return true;
    });
    LOG_WARNING(Core, "coop: flags: a game setter could not be called: writes go into the game's queues from now on "
                      "({} waiting writes moved there, {} lost)", moved, lost);
}

// Every 10 s while anything moved: what the applier's wrap and the setter calls did.
void LogFlagNative() {
    static std::int64_t said_ms = 0;
    static std::uint64_t said_applies = 0, said_setters = 0;
    const std::int64_t now = SteadyMs();
    if (now - said_ms < 10'000) {
        return;
    }
    const CoopFlagNative::Counters& c = CoopFlagNative::Counts();
    const std::uint64_t applies = c.applies.load(), setters = c.setters.load();
    if (applies == said_applies && setters == said_setters) {
        return;
    }
    said_ms = now;
    said_applies = applies;
    said_setters = setters;
    LOG_INFO(Core, "coop: flags native: the game's applier ran {} times, {} writes read from its queues ({} dropped), "
                   "{} setter calls made for co-op ({} waiting, {} frames waited for room)",
             applies, c.writes.load(), CoopFlagNative::Events().dropped(), setters, CoopFlagNative::Calls().size(),
             c.no_room.load());
}

// Native boss sync (coop_boss_native.h): on when this hookshot.exe has
// both wraps (the hook table's rows 4 and 5) and LICHII_COOP_BOSS_NATIVE is not
// 0; the action verbs also need LICHII_COOP_BOSS_ACTIONS not 0.
bool WrapIn(std::uint32_t id) {  // (as CoopHookshot::HasWrap)
    return id < 64 && (g_world_wraps.load() >> id & 1) != 0;
}
void SetBossNative() {
    const bool brain = WrapIn(HOS_COOP_WRAP_BRAIN_VERB) && Env("LICHII_COOP_BOSS_NATIVE") != "0";
    const bool actions = brain && WrapIn(HOS_COOP_WRAP_ACTION_VERB) && Env("LICHII_COOP_BOSS_ACTIONS") != "0";
    CoopBossNative::Table& t = CoopBossNative::TheTable();
    if (t.BrainOn() == brain && t.ActionsOn() == actions) {
        return;
    }
    t.SetOn(brain, actions);
    LOG_INFO(Core, "coop: bosses: native sync {} (brain verbs {}, action verbs {}){}", brain ? "on" : "off",
             WrapIn(HOS_COOP_WRAP_BRAIN_VERB) ? "wrapped" : "not wrapped in this hookshot.exe",
             WrapIn(HOS_COOP_WRAP_ACTION_VERB) ? (actions ? "wrapped" : "wrapped, off") : "not wrapped",
             brain ? "" : ": bosses as before (owner's animations, gohma-sync)");
}

// Every 10 s while a boss is synced natively: the counts (first proof lines).
void LogBossNative() {
    static std::uint64_t said_ms = 0;
    static CoopBossNative::Counts said{};
    const std::uint64_t now = SteadyMs();
    if (now - said_ms < 10'000) {
        return;
    }
    const CoopBossNative::Counts c = CoopBossNative::TheTable().Totals();
    if (c.owner_brain == said.owner_brain && c.owner_action == said.owner_action && c.copy_skipped == said.copy_skipped &&
        c.brain_applied == said.brain_applied && c.action_applied == said.action_applied) {
        return;
    }
    said_ms = now;
    said = c;
    LOG_INFO(Core, "coop: bosses: ours sent {} brain + {} action verbs ({} by plug); copies: {} own skipped, {} brain + "
                   "{} action verbs put in ({} waited for a node), {} own minions not made, {} busy",
             c.owner_brain, c.owner_action, c.plug_actions, c.copy_skipped, c.brain_applied, c.action_applied,
             c.action_waiting, c.minions_stopped, c.busy);
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
        const OwnCall own;  // (what the game asks inside it is our own asking: World())
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

// The players on the map (SetMapOptions): on unless the settings say off
// (0, no, off, false: as coop-options.ini is read, CoopTitleMenu::Options).
bool EnvOff(const char* name) {
    std::string v = Env(name);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v == "0" || v == "no" || v == "off" || v == "false";
}
std::atomic<bool> g_map_players{!EnvOff("LICHII_COOP_MAP")};
std::atomic<bool> g_map_names{!EnvOff("LICHII_COOP_MAP_NAMES")};

// The minimap's players (CoopDraw::MapShown), or none: the setting is off,
// the game's camera is not known (not playing), our own cutscene runs (the
// game hides its HUD), or our place has no map (coop_maps.txt).
std::optional<CoopDraw::MapShown> MapScene(bool camera, std::uint8_t self) {
    if (!g_map_players.load() || !camera) {
        return std::nullopt;
    }
    auto view = CoopHook::CurrentMapView();
    if (!view || view->event || view->others.empty()) {
        return std::nullopt;
    }
    const auto* capture = CoopMaps::Maps().find(view->scene);
    if (capture == nullptr) {
        return std::nullopt;
    }
    static const float zoom = [] {
        const auto z = coop::overlay::map::parse_zoom(Env("LICHII_COOP_MINIMAP_ZOOM"));
        if (z) {
            LOG_INFO(Core, "coop: map: the minimap's zoom {} (LICHII_COOP_MINIMAP_ZOOM)", *z);
        }
        return z.value_or(1.0f);
    }();
    CoopDraw::MapShown m;
    m.scene = std::move(view->scene);
    m.self = view->self;
    m.metres_across = CoopDraw::MinimapMetres(*capture, zoom);
    const bool names = g_map_names.load();
    m.players.reserve(view->others.size());
    for (auto& o : view->others) {
        coop::overlay::map::Player p;
        p.number = o.number;
        p.position = o.position;
        p.rotation = o.rotation;
        p.scene = std::move(o.scene);
        p.fade = o.fade;
        const std::uint32_t c = CoopDraw::Lifted(CoopPuppet::NameColor(o.number, self));
        p.rgb = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);  // (IM_COL32 to 0xRRGGBB)
        m.players.push_back(std::move(p));
        if (names) {
            m.names.push_back(o.name.empty() ? "P" + std::to_string(o.number) : std::move(o.name));
        }
    }
    return m;
}

}  // namespace

void SetMapOptions(bool players, bool names) {
    g_map_players.store(players);
    g_map_names.store(names);
}

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
    s.dbno = CoopDbno::Shown();  // down but not out: the ring
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
    {
        std::uint8_t self = 0;
        for (const auto& row : rows) {
            self = row.self ? row.number : self;
        }
        s.map = MapScene(s.markers.has_value(), self);
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

// This game's controller and keyboard (coop_input.h): longshot.exe's patch 0015 exports the choice; the kit's
// LICHII_COOP_PAD and LICHII_COOP_KEYBOARD set it. Applied before co-op may turn itself off, so two games on one PC
// keep their own pads either way (the choice stays in the exe). An exe without patch 0015: every pad counts.
void UseInputSelect() {
    CoopInput::Exports e;
#ifdef _WIN32
    const HMODULE exe = GetModuleHandleW(nullptr);
    e.select = reinterpret_cast<decltype(e.select)>(reinterpret_cast<void*>(GetProcAddress(exe, "hos_coop_input_select")));
    e.pad_name =
        reinterpret_cast<decltype(e.pad_name)>(reinterpret_cast<void*>(GetProcAddress(exe, "hos_coop_input_pad_name")));
#else
    e.select = reinterpret_cast<decltype(e.select)>(dlsym(RTLD_DEFAULT, "hos_coop_input_select"));
    e.pad_name = reinterpret_cast<decltype(e.pad_name)>(dlsym(RTLD_DEFAULT, "hos_coop_input_pad_name"));
#endif
    CoopInput::Bind(e);
    std::vector<std::string> warnings;
    const auto s = CoopInput::FromValues(Env("LICHII_COOP_PAD"), Env("LICHII_COOP_KEYBOARD"), &warnings);
    for (const auto& w : warnings) {
        LOG_WARNING(Core, "coop: {}", w);
    }
    CoopInput::Apply(s);
    if (!CoopInput::Available()) {
        if (s != CoopInput::Setting{}) {
            LOG_WARNING(Core, "coop: this Hookshot cannot choose a controller (patch 0015): every pad counts");
        }
        return;
    }
    LOG_INFO(Core, "coop: controller {}, keyboard {}", CoopInput::PadValue(s.pad), CoopInput::KeyboardValue(s.keyboard));
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
    UseInputSelect();
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
    // Flags through the game's own functions (coop_flag_native.h): its writes as events from the applier's wrap, ours
    // through its setters on the calc thread. LICHII_COOP_FLAG_EVENTS=0 / LICHII_COOP_FLAG_SETTERS=0: as on the
    // emulator (the table diff, writes into the game's queues).
    CoopFlagNative::Counts().events_on.store(Env("LICHII_COOP_FLAG_EVENTS") != "0");
    g_flag_setters.store(Env("LICHII_COOP_FLAG_SETTERS") != "0");
    coop::host::GameFlags::use_calls(g_flag_setters.load() ? &CoopFlagNative::Calls() : nullptr);
    LOG_INFO(Core, "coop: flags: the game's writes {}, our writes {}",
             CoopFlagNative::Counts().events_on.load() ? "as events from its applier (patch 0005's wrap)"
                                                       : "by reading the tables (LICHII_COOP_FLAG_EVENTS=0)",
             g_flag_setters.load() ? "through the game's setters on its calc thread"
                                   : "into the game's queues (LICHII_COOP_FLAG_SETTERS=0)");
    CoopHook::Start();
    g_on = true;
    CoopModsRuntime::Start();  // the mods folder and the code mods (docs/mods.md)
    CoopTitle::Init(api);
    CoopPause::Init(server);
    CoopLook::StartModule();  // the look chosen in the pause menu (coop_look.h)
    return true;
}

void LocalProbe();   // (below)
void LocalLayer(void* ctx);   // (below)

void Frame(void* ctx, int site) {
    if (!g_on) {
        return;
    }
    if (site == HOS_COOP_SITE_FRAME) {
        CoopModsRuntime::Frame();
        CoopPause::Tick();   // the Multiplayer tab's follow-ups, also while the menu is closed
        CoopLook::Frame();   // a look chosen in the pause menu, once it closed
        LocalProbe();
        if (ctx != nullptr) LocalLayer(ctx);
    }
    if (site == HOS_COOP_SITE_PRESENT && !g_present_seen.exchange(true)) {
        LOG_INFO(Core, "coop: Hookshot calls co-op at each present (loads and pause included)");
    }
    if (site != HOS_COOP_SITE_PRESENT && g_world_version.load() == 0) {
        // A Hookshot built without patch 0005 never says hello (one with it does at the first hooked function the game
        // runs): say so once, some thousand frame hooks in.
        static std::atomic<std::uint32_t> frames{0};
        if (frames.fetch_add(1) == 3600) {
            LOG_WARNING(Core, "coop: world hooks: hookshot.exe has not said hello yet: built without patch 0005, or no "
                              "hooked game function ran so far (until it does: no pause counters, no flag events)");
        }
    }
    const FrameWork w = WorkAt(site, g_present_seen.load());
    if (w.on_frame) {
        CoopHook::OnFrame(g_system);
    }
    if (w.calls && ctx != nullptr) {
        RunFlagSetters(ctx);
        LogFlagNative();
        RunCalls(ctx);
        LogBossNative();
    }
}

// (CREATE_FROM's caller and create argument until that thread's CREATE_REQUEST)
thread_local std::uint64_t t_create_from = 0;
thread_local std::uint64_t t_create_arg = 0;
// (CREATE_REQUEST's verdict until that thread's CREATE_STOP)
thread_local bool t_create_stop = false;
// (SHOOT_FROM's shooter until that thread's next CREATE_REQUEST; used only for
// the create the shoot code itself makes, by its return address)
thread_local std::uint64_t t_shooter = 0;
constexpr std::uint64_t kShootCreateReturn = 0xc17300;  // (after the bl at main+0xc172fc)

// The world hooks (patch 0005): the game's C hands its world events here instead of into caves and guest rings.
int World(std::uint32_t event, std::uint64_t a, std::uint64_t b) {
    if (!g_on) {
        return 0;
    }
    switch (event) {
    case HOS_COOP_WORLD_HELLO: {
        // (the first patch 0005 sent a = 0: level 1, with the flag applier's wrap)
        const WorldLevel level = HelloLevel(a, b);
        g_world_wraps.store(level.wraps);
        SetBossNative();
        if (g_world_version.exchange(level.version) != level.version) {
            LOG_INFO(Core, "coop: world hooks: hookshot.exe has level {} (this module {}), wraps {:#x}{}", level.version,
                     HOS_COOP_WORLD_VERSION, level.wraps,
                     level.version < HOS_COOP_WORLD_VERSION ? ": an older Hookshot build, some hooks are missing" : "");
        }
        return 1;
    }
    case HOS_COOP_WORLD_COUNTER_START: {  // (coop_counters.h: the Objects walk reads the ring)
        CoopCounters::TheHostRing().Push(a);
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            LOG_INFO(Core, "coop: world hooks: first pause counter start from hookshot.exe (node {:#x})", a);
        }
        return 1;
    }
    // The actor events (patch 0007, hos_coop_actors.h; coop_actor_events.h:
    // the Objects step drains them on the frame hook). Only reads here.
    case HOS_COOP_WORLD_SHOOT_FROM: {  // (a: its first 8 bytes are the shooter's actor)
        std::uint64_t actor = 0;
        t_shooter = a != 0 && CoopRuntime::Read(a, &actor, sizeof actor) ? actor : 0;
        return 1;
    }
    case HOS_COOP_WORLD_CREATE_FROM: {  // (just before CREATE_REQUEST, on this thread)
        t_create_from = a;
        t_create_arg = b;
        return 1;
    }
    case HOS_COOP_WORLD_CREATE_REQUEST: {
        CoopActorEvents::Event e;
        e.kind = CoopActorEvents::Kind::Create;
        e.flags = t_own_call ? CoopActorEvents::kOwn : 0;
        e.a = a;
        e.b = b;
        e.from = std::exchange(t_create_from, 0);
        const std::uint64_t arg = std::exchange(t_create_arg, 0);
        std::uint64_t text = 0;  // (x1: its first 8 bytes point at the name; on the caller's stack: read now)
        char name[32] = {};
        if (a != 0 && CoopRuntime::Read(a, &text, sizeof text) && text != 0 &&
            CoopRuntime::Read(text, name, sizeof name - 1)) {
            e.SetName(name);
        }
        float place[3] = {};
        if (arg != 0 && CoopRuntime::Read(arg, place, sizeof place)) {
            for (int i = 0; i < 3; ++i) {
                std::memcpy(&e.place[i], &place[i], 4);
            }
        }
        // A copy enemy's own shot is not made at all (coop_enemy_shots.h):
        // the owner's game fires, and its shot comes as a copy, which is
        // one of our own creates and never stopped.
        // The shooter is the one the game's shoot code named, when this is
        // that code's own create; any other create is judged by its place.
        const std::uint64_t shooter = std::exchange(t_shooter, 0);
        const bool shot = e.from != 0 && e.from == CoopRuntime::MainBase() + kShootCreateReturn;
        t_create_stop = !t_own_call && name[0] != 0 &&
                        (CoopEnemyShots::StopAtCreate(name, {place[0], place[1], place[2]}, shot ? shooter : 0) ||
                         // Native boss sync: a copy boss's own minion (the owner's comes as a copy).
                         CoopBossNative::TheTable().CopyMakes(CoopObjects::MinionMaker(name)));
        if (t_create_stop) {
            e.flags |= CoopActorEvents::kStopped;
        }
        CoopActorEvents::TheRing().Push(e);
        return 1;
    }
    case HOS_COOP_WORLD_CREATE_STOP: {  // (a: our C's uint32, in hookshot.exe's memory)
        auto* stop = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(a));
        if (stop != nullptr && std::exchange(t_create_stop, false)) {
            *stop = 1;
        }
        return 1;
    }
    case HOS_COOP_WORLD_DELETE_REQUEST: {
        CoopActorEvents::Event e;
        e.kind = CoopActorEvents::Kind::Delete;
        e.flags = t_own_call ? CoopActorEvents::kOwn : 0;
        e.a = a;
        e.b = b;
        CoopActorEvents::TheRing().Push(e);
        return 1;
    }
    case HOS_COOP_WORLD_LIFE_STEP: {  // (b: our C's int32 sum, in hookshot.exe's memory)
        auto* sum = reinterpret_cast<std::int32_t*>(static_cast<std::uintptr_t>(b));
        if (sum == nullptr) {
            return 0;
        }
        if (!CoopActorEvents::LifeStepSeen().exchange(true, std::memory_order_relaxed)) {
            LOG_INFO(Core, "coop: actors: the game's Life step calls the module (patch 0007): hits go in through it");
        }
        // Difficulty (coop_difficulty.h): what the game takes of the frame's own damage. What another
        // game's hit gives (the life table) was scaled where that hit was dealt.
        const std::int32_t own = CoopDifficulty::TheKinds().Step(
            a, *sum, CoopDifficulty::Now(), CoopDifficulty::SteadyMs(), [a] {
                std::uint64_t attacker = 0;
                return CoopRuntime::Read(a + CoopDifficulty::kHitAttacker, &attacker, sizeof attacker) ? attacker
                                                                                                         : 0;
            });
        *sum = CoopActorEvents::Step(CoopActorEvents::TheRing(), CoopActorEvents::TheLifeTable(), a, own);
        // Down but not out (coop_dbno_game.h): our Link's last life is held back, after every other
        // change to the sum (difficulty, PvP), so the guard sees what would really be taken.
        if (a != 0 && a == CoopDbno::OurLifeComponent()) {
            std::uint64_t value = 0;
            std::int32_t life = 0;
            if (CoopRuntime::Read(a + CoopDbno::kLifeValueEntry, &value, sizeof value) && value != 0 &&
                CoopRuntime::Read(value + 8, &life, sizeof life)) {
                *sum = CoopDbno::LifeStep(a, life, *sum);
            }
        }
        return 1;
    }
    default:
        return 0;  // (a newer Hookshot's event this module does not know)
    }
}

WorldLevel WorldHooks() {
    return WorldLevel{g_world_version.load(), g_world_wraps.load()};
}

bool HasWrap(std::uint32_t id) {
    return id < 64 && (g_world_wraps.load() >> id & 1) != 0;
}

// The wrapped game functions (patch 0005, nx2/coop_world_wraps.py). The game's function always runs (0 at phase 0).
int Wrap(std::uint32_t id, int phase, std::uint64_t* x, void* /*ctx*/) {
    if (!g_on || x == nullptr) {
        return 0;
    }
    if (id == CoopPlayerInput::kWrapButtons || id == CoopPlayerInput::kWrapSticks ||
        id == CoopPlayerInput::kWrapInputOff || id == CoopPlayerInput::kWrapControllerGet) {
        // Local co-op: an extra player's Player reads its own controller. (native-puppets-b2's remote input, once
        // wired, is a second handler of these ids: both must run, and either may take phase 0.)
        return CoopLocalGame::Wrap(id, phase, x);
    }
    if (id == HOS_COOP_WRAP_EVENT_REQUEST) {
        // The game asks for an event: its name at the request (phase 0: x1 is the request), whether the manager took
        // it after the game's function (phase 1: x0). Both on the same game thread, one after the other.
        thread_local char name[sizeof(CoopLiveEvents::GameRequest::name)];
        thread_local bool named = false;
        if (phase == 0) {
            std::uint64_t text = 0;
            named = x[1] != 0 && CoopRuntime::Read(x[1] + CoopLiveEvents::addr::kArgName, &text, sizeof text) &&
                    text != 0 && CoopRuntime::Read(text, name, sizeof name - 1);
            name[sizeof name - 1] = 0;
        } else if (named) {
            named = false;
            CoopLiveEvents::TheGameRequests().Push(name, x[0] != 0);
        }
        return 0;
    }
    if (id == HOS_COOP_WRAP_BRAIN_VERB && phase == 0) {
        // Native boss sync: a boss's brain verb change (x0 BSA, w1 verb, w2 force). A copy's own is skipped
        // once the owner's verbs have come (1: the game's function does not run; x0 0 as a defined result, though
        // no caller reads one).
        if (!CoopBossNative::TheTable().OnBrainVerb(x[0], static_cast<std::int32_t>(x[1]), (x[2] & 1) != 0,
                                                    t_own_call)) {
            return 0;
        }
        x[0] = 0;
        return 1;
    }
    if (id == HOS_COOP_WRAP_ACTION_VERB && phase == 0) {
        // Native boss sync: a boss's action verb request (x0 BSA, x1 the request in guest memory).
        CoopBossNative::Table& t = CoopBossNative::TheTable();
        if (!t.ActionsOn() || x[1] == 0) {
            return 0;
        }
        CoopBossNative::Request r{};
        if (!CoopRuntime::Read(x[1], r.data(), r.size())) {
            return 0;
        }
        if (!t.OnActionVerb(x[0], r, (x[2] & 1) != 0, t_own_call)) {
            return 0;
        }
        // Skipped: the result its callers pass up is what the game gives for a request it took (0, its main path
        // main+0xc10f4; 2 and 3 are its refusals), so the asking node goes on as after its own request.
        x[0] = CoopBossNative::addr::kActionTaken;
        return 1;
    }
    if (id == HOS_COOP_WRAP_TIME_SET && phase == 0) {
        // The game's own time change, with its exact time: the time sync's jump (coop_time_sync.h, GameSet).
        CoopTime::OnGameSet(static_cast<std::uint32_t>(x[1]), static_cast<std::uint32_t>(x[2]),
                            static_cast<std::int32_t>(x[3]));
        return 0;
    }
    if (id == HOS_COOP_WRAP_FLAG_APPLY && phase == 0) {
        // The writes the game is about to apply, read from its queues (x0 = the manager): the flag watcher's events.
        CoopHook::EmulatorMemory memory(g_system.ApplicationMemory(), CoopRuntime::MainBase());
        CoopFlagNative::OnApply(memory, x[0]);
        // First proof: how often the game applies its queued flag writes (one line at the first, then every 10 s).
        static std::atomic<std::uint64_t> runs{0};
        static std::atomic<std::int64_t> said_ms{0};
        const std::uint64_t n = runs.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
        std::int64_t last = said_ms.load(std::memory_order_relaxed);
        if ((n == 1 || now - last >= 10'000) && said_ms.compare_exchange_strong(last, now)) {
            LOG_INFO(Core, "coop: world wraps: the game's flag applier ran {} times so far (x0 {:#x})", n, x[0]);
        }
    }
    return 0;
}

// The AI hold (native-plan row 6): a held component's calc bits become
// (bits & and) | or, from the set the Objects step published in our own
// memory (CoopHold::TheHostTable; no guest table at main+0x3c46400).
std::uint32_t Hold(std::uint64_t component, std::uint32_t bits) {
    if (!g_hold || component == 0) {
        return bits;
    }
    return CoopHold::TheHostTable().Apply(component, bits);
}

void Stop() {
    CoopModsRuntime::Stop();
    HostMenu().Leave();  // our own server goes after our own client
    CoopHold::TheHostTable().Clear();  // (no copy stays held once co-op is off)
    if (g_on) {
        g_on = false;
        CoopLook::StopModule();
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

// Local co-op's research probe (coop_local_probe.h, LICHII_COOP_LOCAL_PROBE=1): the renderer's layers and the
// controller manager, logged every 10 s, five times. Reads only.
void LocalProbe() {
    static const bool wanted = [] {
        const char* v = std::getenv("LICHII_COOP_LOCAL_PROBE");
        return v != nullptr && *v != 0 && std::strcmp(v, "0") != 0;
    }();
    static int said = 0;
    static std::chrono::steady_clock::time_point next{};
    const auto now = std::chrono::steady_clock::now();
    if (!wanted || said >= 5 || now < next) return;
    next = now + std::chrono::seconds(10);
    said++;
    const CoopLocalProbe::ReadFn read = [](std::uint64_t a, void* out, std::size_t n) {
        return CoopRuntime::Read(a, out, n);
    };
    for (const auto* lines : {&CoopLocalProbe::Layers, &CoopLocalProbe::Controllers})
        for (const std::string& line : (*lines)(read, CoopRuntime::MainBase()))
            LOG_INFO(Core, "coop: local probe {}: {}", said, line);
}

// Split screen's second world layer, research (coop_local_layer.h): LICHII_COOP_LOCAL_LAYER=1 only, with an exe whose
// call() and scratch() are there (patch 0004). The calc thread's every-frame site.
void LocalLayer(void* ctx) {
    static const bool wanted = [] {
        const char* v = std::getenv("LICHII_COOP_LOCAL_LAYER");
        return v != nullptr && std::string_view(v) == "1";
    }();
    if (!wanted || g_api == nullptr || g_api->size < offsetof(HosCoopApi, scratch) + sizeof(g_api->scratch) ||
        g_api->scratch == nullptr || g_api->call == nullptr)
        return;
    static CoopLocalLayer::Second second{[] {
        CoopLocalLayer::Game g;
        g.main_base = [] { return CoopRuntime::MainBase(); };
        g.read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
        g.write = [](std::uint64_t a, const void* in, std::size_t n) { return CoopRuntime::Write(a, in, n); };
        g.log = [](const std::string& line) { LOG_INFO(Core, "coop: {}", line); };
        return g;
    }()};
    static void* frame_ctx = nullptr;   // (this call's: scratch() and call() take it)
    frame_ctx = ctx;
    static const bool set = [] {
        second.Set([](const std::vector<std::uint8_t>& b) { return g_api->scratch(frame_ctx, b.data(), b.size()); },
                   [](std::uint64_t f, const std::array<std::uint64_t, 8>& x) -> std::optional<std::uint64_t> {
                       HosCoopCall k{};
                       k.function = f;
                       for (std::size_t i = 0; i < 8; i++) k.x[i] = x[i];
                       std::uint64_t x0 = 0;
                       if (!g_api->call(frame_ctx, &k, &x0)) return std::nullopt;
                       return x0;
                   });
        return true;
    }();
    (void)set;
    std::optional<std::array<float, 3>> first;
    if (const auto p = CoopLocalGame::FirstLink()) first = std::array<float, 3>{(*p)[0], (*p)[1], (*p)[2]};
    second.Step(first);
}

// Local co-op (coop_local_game.h): the exe's pads (patch 0016, hos_coop_pads.h) and the Player's wraps 16, 17 and 19.
// An exe without them: CoopLocal says NotSupported. LICHII_COOP_LOCAL=<pad slot> starts a second player on that pad
// at the first frame in play, without the menu (tests).
void UseLocalPads() {
#ifdef _WIN32
    const HMODULE exe = GetModuleHandleW(nullptr);
    const auto find = [exe](const char* name) { return reinterpret_cast<void*>(GetProcAddress(exe, name)); };
#else
    const auto find = [](const char* name) { return dlsym(RTLD_DEFAULT, name); };
#endif
    const auto info = reinterpret_cast<hos_coop_pad_info_fn>(find(HOS_COOP_PAD_INFO));
    const auto take = reinterpret_cast<hos_coop_pad_take_fn>(find(HOS_COOP_PAD_TAKE));
    CoopLocalGame::Deps deps;
    if (info && take) {
        deps.pads = [info] {
            std::vector<CoopLocalGame::Pad> pads;
            for (int slot = 1; slot <= 64; slot++) {   // (by place in SDL's list, from 1: patch 0016)
                char name[64] = {};
                std::uint32_t serial = 0;
                int npad = 0;
                const int there = info(slot, &serial, name, sizeof name, &npad);
                if (there < 0) break;
                if (there > 0) pads.push_back({slot, serial, name, npad});
            }
            return pads;
        };
        deps.take = [take](int slot, std::uint32_t serial, int npad) { return take(slot, serial, npad) != 0; };
    }
    deps.wraps = [] {
        return HasWrap(CoopPlayerInput::kWrapButtons) && HasWrap(CoopPlayerInput::kWrapSticks) &&
               HasWrap(CoopPlayerInput::kWrapInputOff) && HasWrap(CoopPlayerInput::kWrapControllerGet);
    };
    deps.main_base = [] { return CoopRuntime::MainBase(); };
    deps.slots = [] { return CoopPuppet::TheDirector().Slots(); };
    deps.read = [](std::uint64_t address, void* out, std::size_t size) { return CoopRuntime::Read(address, out, size); };
    deps.write = [](std::uint64_t address, const void* in, std::size_t size) {
        return CoopRuntime::Write(address, in, size);
    };
    deps.log = [](const std::string& line) { LOG_INFO(Core, "coop: {}", line); };
    LOG_INFO(Core, "coop: local co-op: {}", info && take ? "this Hookshot gives the module its pads (patch 0016)"
                                                          : "this Hookshot has no pads for local co-op (patch 0016)");
    CoopLocalGame::Install(std::move(deps));
    if (const char* slot = std::getenv("LICHII_COOP_LOCAL"); slot && *slot) {
        CoopLocalGame::AutoStart(std::atoi(slot));
    }
    if (const char* views = std::getenv("LICHII_COOP_LOCAL_VIEWS"); views && *views) {   // (tests: the split alone)
        CoopLocalGame::ForceViews(std::atoi(views));
        LOG_INFO(Core, "coop: local co-op: LICHII_COOP_LOCAL_VIEWS={}: the views split with no second player", views);
    }
}

}  // namespace CoopHookshot

extern "C" {
// The host's save for a guest (issue 167): longshot.exe's patch 0008 exports the account save's folder and a
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
    CoopHookshot::UseLocalPads();
    return 1;
}
COOP_HOOKSHOT_EXPORT void hos_coop_frame(void* ctx, int site) {
    CoopHookshot::Frame(ctx, site);
}
COOP_HOOKSHOT_EXPORT uint32_t hos_coop_hold(uint64_t component, uint32_t bits) {
    return CoopHookshot::Hold(component, bits);
}
// The world hooks (hos_coop_world.h): Hookshot's host/coop_world.c, on the game's threads.
COOP_HOOKSHOT_EXPORT int hos_coop_world(uint32_t event, uint64_t a, uint64_t b) {
    return CoopHookshot::World(event, a, b);
}
COOP_HOOKSHOT_EXPORT int hos_coop_wrap(uint32_t id, int phase, uint64_t* x, void* ctx) {
    return CoopHookshot::Wrap(id, phase, x, ctx);
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
    if (!g_on || stick == nullptr) {
        return buttons;
    }
    buttons = CoopTitle::OnPad(buttons, stick);
    // Down but not out (coop_dbno_game.h): a downed Link crawls and uses nothing; beside one, A revives.
    CoopDbno::Pad pad{buttons, {stick[0], stick[1], stick[2], stick[3]}};
    pad = CoopDbno::OnPad(pad);
    for (int i = 0; i < 4; ++i) {
        stick[i] = pad.stick[i];
    }
    return pad.buttons;
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
