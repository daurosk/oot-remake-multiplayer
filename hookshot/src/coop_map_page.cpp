// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_map_page.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <fmt/format.h>

#include "common/logging.h"
#include "coop_hud_layout.h"
#include "coop_pause_files.h"
#include "coop_runtime.h"

namespace CoopMapPage {
namespace {

namespace map = coop::overlay::map;

// patch 0018's export (host/oot_menu.c)
struct HosCoopMenuState {
    std::uint32_t size;
    std::int32_t map_page, menu_open, reserved;
    std::uint64_t hud_screen, panel_screen;
    std::int64_t hud_ran_ns_ago;  // (rows 63/76; an older exe answers with the struct up to here)
    std::uint32_t hud_refreshes, reserved2;
    std::uint32_t screen_count, reserved3;  // (row 141; an older exe answers with the struct up to here)
    std::uint64_t screens[24];
};
using MenuStateFn = int (*)(HosCoopMenuState*);

// One of patch 0018's exports, by name in the exe: dllexport on Windows; on Linux the exe's link exports it by name
// (patch 0001, as 0008's and 0015's) and dlsym finds it. Nothing in a Longshot without it.
void* FindExport(const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), name));
#else
    return dlsym(RTLD_DEFAULT, name);
#endif
}

MenuStateFn FindMenuState() {
    static const MenuStateFn fn = [] {
        const auto f = reinterpret_cast<MenuStateFn>(FindExport("hos_coop_menu_state"));
        LOG_INFO(Core, "coop: map: {}", f ? "the game's menus are known (patch 0018): players on the Map page, the "
                                            "minimap's icons hidden behind menus"
                                          : "this Longshot has no menu state (patch 0018): nothing on the Map page, "
                                            "the minimap's icons stay over menus");
        return f;
    }();
    return fn;
}

// The panes found (0: not found), each with the screen whose layout it is in; and the screens looked in.
struct Found {
    std::atomic<std::uint64_t> pane{0}, from{0};
};
Found g_field, g_dungeon;
Found g_mini_dungeon, g_mini_field, g_mini_window, g_mini_field_window, g_mini_root;  // the HUD minimap's (HUD screen only)
std::atomic<std::uint64_t> g_searched_hud{0}, g_searched_panel{0};

// A pane is used only while the screen it came from is one of the screens that run now (a new screen: its
// old panes may be freed).
std::uint64_t PaneNow(const Found& f, const Menu& menu) {
    const std::uint64_t from = f.from.load();
    return from != 0 && (from == menu.hud_screen || from == menu.panel_screen) ? f.pane.load() : 0;
}

void Forget(Found& f, std::uint64_t screen) {
    if (screen != 0 && f.from.load() == screen) {
        f.pane = 0;
        f.from = 0;
    }
}

std::uint64_t Read64(std::uint64_t a) {
    std::uint64_t v = 0;
    return CoopRuntime::Read(a, &v, sizeof v) ? v : 0;
}

bool CanCall(const HosCoopApi* api) {
    return api != nullptr && api->call != nullptr && api->size >= offsetof(HosCoopApi, scratch) + sizeof(api->scratch) &&
           api->scratch != nullptr;
}

std::uint64_t FindPane(void* ctx, const HosCoopApi* api, std::uint64_t layout, const char* name) {
    const std::size_t n = std::strlen(name) + 1;
    const std::uint64_t at = api->scratch(ctx, name, n);
    if (at == 0) {
        return 0;
    }
    HosCoopCall k{};
    k.function = CoopRuntime::MainBase() + kFindPane;
    k.x[0] = layout;
    k.x[1] = at;
    std::uint64_t pane = 0;
    return api->call(ctx, &k, &pane) ? pane : 0;
}

// The HUD minimap's panes in the HUD screen's layout.
void SearchMinimap(void* ctx, const HosCoopApi* api, std::uint64_t screen, std::uint64_t layout) {
    const struct {
        Found* f;
        const char* name;
    } want[] = {{&g_mini_dungeon, kMiniDungeonPane},
                {&g_mini_field, kMiniFieldPane},
                {&g_mini_window, kMiniWindowPane},
                {&g_mini_field_window, kMiniFieldWindowPane},
                {&g_mini_root, kMiniRootPane}};
    std::string found;
    for (const auto& w : want) {
        const std::uint64_t pane = FindPane(ctx, api, layout, w.name);
        if (pane != 0) w.f->pane = pane, w.f->from = screen;
        found += fmt::format(" {} {:#x}", w.name, pane);
    }
    LOG_INFO(Core, "coop: map: the HUD minimap's panes:{}", found);
}

// Looks for both panes in `screen`'s layout; keeps what it finds (a pane found before stays).
void Search(void* ctx, const HosCoopApi* api, std::uint64_t screen, const char* which) {
    const std::uint64_t layout = Read64(screen + kScreenLayout);
    if (layout == 0) {
        return;
    }
    if (std::strcmp(which, "HUD") == 0) {
        SearchMinimap(ctx, api, screen, layout);
    }
    const std::uint64_t field = FindPane(ctx, api, layout, kFieldPane);
    const std::uint64_t dungeon = FindPane(ctx, api, layout, kDungeonPane);
    if (field != 0) g_field.pane = field, g_field.from = screen;
    if (dungeon != 0) g_dungeon.pane = dungeon, g_dungeon.from = screen;
    float m[12] = {};
    const bool read = field != 0 && CoopRuntime::Read(field + kPaneMatrix, m, sizeof m);
    LOG_INFO(Core, "coop: map: the {} screen {:#x} (layout {:#x}): {} {:#x}, {} {:#x}{}", which, screen, layout,
             kFieldPane, field, kDungeonPane, dungeon,
             read ? fmt::format(" (matrix scale {:.3f},{:.3f} at {:.1f},{:.1f})", m[0], m[4], m[9], m[10])
                  : std::string());
}

// dungeon-maps-a (row 76, #146): LICHII_COOP_MAP_PROBE=1 logs the HUD screen's own map copy whenever it changes, read
// only: the mode (+0x1358: 0 field, 2 dungeon, ...), the floors shown (+0x1368: floor, count, lowest), the floors
// byte the minimap and the Map page draw (+0x1500, filled by the game's rebuild main+0xb28380: every floor when the
// dungeon's IsGetMap is set) and the cached IsGetMap, IsGetCompass, IsGetBossKey and 4th Bool (+0x1538..+0x153b).
// Research only: it says whether a map get (the game's own, another player's, the randomizer's) reached the copy.
void Probe(const Menu& menu) {
    static const bool on = CoopFiles::Env("LICHII_COOP_MAP_PROBE") == "1";
    if (!on || menu.hud_screen == 0) {
        return;
    }
    struct Copy {
        std::int32_t mode = 0, floor = 0, count = 0, lowest = 0;
        std::uint8_t floors = 0, cached[4]{};
        bool operator==(const Copy&) const = default;
    } now{};
    const std::uint64_t s = menu.hud_screen;
    std::int32_t f[3]{};
    if (!CoopRuntime::Read(s + 0x1358, &now.mode, sizeof now.mode) || !CoopRuntime::Read(s + kHudFloors, f, sizeof f) ||
        !CoopRuntime::Read(s + 0x1500, &now.floors, 1) || !CoopRuntime::Read(s + 0x1538, now.cached, 4)) {
        return;
    }
    now.floor = f[0], now.count = f[1], now.lowest = f[2];
    static Copy was{};
    static std::uint64_t was_screen = 0;
    if (now == was && s == was_screen) {
        return;
    }
    LOG_INFO(Core,
             "coop: map probe: the HUD screen {:#x}{}: mode {} floor {} of {} (lowest {}) floors 0x{:02x}, map {} "
             "compass {} boss key {} bool4 {} (menu {}, Map page {})",
             s, s != was_screen ? " (new)" : "", now.mode, now.floor, now.count, now.lowest, now.floors, now.cached[0],
             now.cached[1], now.cached[2], now.cached[3], menu.menu_open ? "open" : "closed",
             menu.map_page ? "shown" : "not shown");
    was = now, was_screen = s;
}

}  // namespace

std::optional<Menu> MenuState() {
    const auto fn = FindMenuState();
    if (fn == nullptr) {
        return std::nullopt;
    }
    HosCoopMenuState s{};
    s.size = sizeof s;
    if (!fn(&s)) {
        return std::nullopt;
    }
    Menu m{s.menu_open != 0, s.map_page != 0, s.hud_screen, s.panel_screen};
    if (s.size >= offsetof(HosCoopMenuState, screen_count)) m.hud_ran_ns_ago = s.hud_ran_ns_ago, m.hud_refreshes = s.hud_refreshes;
    if (s.size >= sizeof s) {  // (row 141: the running screens)
        m.screen_count = (std::min<std::uint32_t>)(s.screen_count, static_cast<std::uint32_t>(m.screens.size()));
        std::copy(s.screens, s.screens + m.screen_count, m.screens.begin());
    }
    return m;
}

namespace {
std::string HudCopy(std::uint64_t screen) {
    std::uint8_t floors = 0, held[4]{};
    if (screen == 0 || !CoopRuntime::Read(screen + 0x1500, &floors, 1) || !CoopRuntime::Read(screen + 0x1538, held, 4)) {
        return "not readable";
    }
    return fmt::format("floors 0x{:02x}, map {} compass {} boss key {}", floors, held[0], held[1], held[2]);
}
std::atomic<std::uint32_t> g_refresh_asked_at{0};  // hud_refreshes when the last one was asked
std::atomic<bool> g_refresh_pending{false};
}  // namespace

bool AskHudRefresh() {
    using Fn = int (*)();
    static const Fn fn = reinterpret_cast<Fn>(FindExport("hos_coop_hud_refresh"));
    static bool told = false;
    if (fn == nullptr) {
        if (!told) LOG_INFO(Core, "coop: map refresh: this Longshot's patch 0018 has no HUD refresh: not asked");
        told = true;
        return false;
    }
    const auto menu = MenuState();
    g_refresh_asked_at = menu ? menu->hud_refreshes : 0;
    g_refresh_pending = true;
    fn();
    LOG_INFO(Core, "coop: map refresh: asked (a dungeon item written by co-op); the HUD screen's copy {}",
             menu ? HudCopy(menu->hud_screen) : std::string("unknown"));
    return true;
}

namespace {
// Row 141: the player status screen (PlayerStatus_00) and its HUD panes; the screens already looked in.
std::atomic<std::uint64_t> g_status_screen{0};
Found g_status_dpad, g_status_hearts, g_status_magic, g_status_agony;
std::array<std::uint64_t, 24> g_status_looked{};
bool g_status_told = false;

void ForgetStatus() {
    for (Found* f : {&g_status_dpad, &g_status_hearts, &g_status_magic, &g_status_agony}) f->pane = 0, f->from = 0;
    g_status_screen = 0;
    g_status_looked = {};  // (a new screen may come at a gone one's address)
}

// Looks in each running screen not looked in before for the d-pad pane, only in a layout of the same kind as the HUD
// screen's (its object's vtable), so the game's lookup is never handed something that is not a layout.
void SearchStatus(void* ctx, const HosCoopApi* api, const Menu& menu) {
    const std::uint64_t had = g_status_screen.load();
    if (had != 0 && !menu.Runs(had)) {
        ForgetStatus();
        LOG_INFO(Core, "coop: hud: the player status screen {:#x} no longer runs: forgotten", had);
    }
    if (g_status_screen.load() != 0 || menu.hud_screen == 0 || menu.screen_count == 0) {
        return;
    }
    const std::uint64_t hud_layout = Read64(menu.hud_screen + kScreenLayout);
    const std::uint64_t layout_kind = hud_layout != 0 ? Read64(hud_layout) : 0;
    if (layout_kind == 0) {
        return;
    }
    std::array<std::uint64_t, 24> looked{};
    for (std::uint32_t i = 0; i < menu.screen_count && i < looked.size(); ++i) {
        const std::uint64_t screen = menu.screens[i];
        looked[i] = screen;
        if (screen == 0 || screen == menu.hud_screen || screen == menu.panel_screen ||
            std::find(g_status_looked.begin(), g_status_looked.end(), screen) != g_status_looked.end()) {
            continue;
        }
        const std::uint64_t layout = Read64(screen + kScreenLayout);
        if (layout == 0 || Read64(layout) != layout_kind) {
            continue;
        }
        const std::uint64_t dpad = FindPane(ctx, api, layout, kStatusDpadPane);
        if (dpad == 0) {
            continue;
        }
        const std::uint64_t hearts = FindPane(ctx, api, layout, kStatusHeartsPane);
        const std::uint64_t magic = FindPane(ctx, api, layout, kStatusMagicPane);
        const std::uint64_t agony = FindPane(ctx, api, layout, kStatusAgonyPane);
        g_status_dpad.pane = dpad, g_status_hearts.pane = hearts, g_status_magic.pane = magic, g_status_agony.pane = agony;
        for (Found* f : {&g_status_dpad, &g_status_hearts, &g_status_magic, &g_status_agony}) f->from = screen;
        g_status_screen = screen;
        LOG_INFO(Core, "coop: hud: the player status screen {:#x} (layout {:#x}): {} {:#x}, {} {:#x}, {} {:#x}, {} {:#x}",
                 screen, layout, kStatusDpadPane, dpad, kStatusHeartsPane, hearts, kStatusMagicPane, magic,
                 kStatusAgonyPane, agony);
        break;
    }
    g_status_looked = looked;
    if (g_status_screen.load() == 0 && !g_status_told) {
        g_status_told = true;
        LOG_INFO(Core, "coop: hud: no running screen has {} yet ({} screens): the player list starts at the fixed top "
                       "until one does", kStatusDpadPane, menu.screen_count);
    }
}
}  // namespace

std::optional<double> GameHudBottom() {
    static std::mutex mutex;
    static CoopHudLayout::Hold hold;
    const auto menu = MenuState();
    const std::uint64_t screen = g_status_screen.load();
    const bool runs = menu && screen != 0 && menu->Runs(screen);
    std::optional<double> now;
    if (runs) {
        std::vector<CoopHudLayout::HudPiece> pieces;
        const auto add = [&](const Found& f, auto&& piece) {
            const std::uint64_t pane = f.from.load() == screen ? f.pane.load() : 0;
            float m[12] = {};
            if (pane == 0 || !CoopRuntime::Read(pane + kPaneMatrix, m, sizeof m)) {
                return;
            }
            float alpha = 1.0f;
            if (!CoopRuntime::Read(pane + kPaneAlpha, &alpha, sizeof alpha) || !std::isfinite(alpha)) {
                alpha = 1.0f;  // (unreadable: counted as shown, the blocks rather lower than over it)
            }
            pieces.push_back(piece(m, alpha));
        };
        add(g_status_dpad, [](const float* m, float a) {
            CoopHudLayout::HudPiece p = CoopHudLayout::Centred(m, a, kDpadSlot, kDpadSlot);
            p.x0 += kDpadDownX, p.x1 += kDpadDownX, p.y0 += kDpadDownY, p.y1 += kDpadDownY;
            return p;
        });
        add(g_status_hearts, [](const float* m, float a) { return CoopHudLayout::Centred(m, a, kHeartsW, kHeartsH); });
        add(g_status_magic, [](const float* m, float a) { return CoopHudLayout::Centred(m, a, kMagicW, kMagicH); });
        add(g_status_agony, [](const float* m, float a) { return CoopHudLayout::Centred(m, a, kAgonySize, kAgonySize); });
        now = CoopHudLayout::HudBottom(pieces);
    }
    std::lock_guard lock(mutex);
    return hold.Update(now);
}

void Frame(void* ctx, const HosCoopApi* api) {
    if (ctx == nullptr || !CanCall(api)) {
        return;
    }
    const auto menu = MenuState();
    if (!menu) {
        return;
    }
    Probe(*menu);
    SearchStatus(ctx, api, *menu);  // (row 141: the game's own HUD, for the player list's top)
    if (g_refresh_pending && menu->hud_refreshes != g_refresh_asked_at) {  // rows 63/76: the asked refresh has run
        g_refresh_pending = false;
        LOG_INFO(Core, "coop: map refresh: run at the HUD screen's update; its copy now {}", HudCopy(menu->hud_screen));
    }
    if (menu->hud_screen != 0 && menu->hud_screen != g_searched_hud.load()) {
        const std::uint64_t old = g_searched_hud.exchange(menu->hud_screen);
        Forget(g_field, old), Forget(g_dungeon, old);
        for (Found* f : {&g_mini_dungeon, &g_mini_field, &g_mini_window, &g_mini_field_window, &g_mini_root}) Forget(*f, old);
        Search(ctx, api, menu->hud_screen, "HUD");
    }
    if (menu->panel_screen != 0 && menu->panel_screen != g_searched_panel.load()) {
        const std::uint64_t old = g_searched_panel.exchange(menu->panel_screen);
        Forget(g_field, old), Forget(g_dungeon, old);
        Search(ctx, api, menu->panel_screen, "panel's");
    }
}

std::optional<Picture> Shown(bool dungeon) {
    const auto menu = MenuState();
    static PageWatch watch;
    std::uint64_t pad = 0;
    const bool y = CoopRuntime::PadButtons(pad) && (pad & kPadY) != 0;
    const bool ours = watch.Update(menu && menu->map_page, y);
    if (!menu || !menu->map_page || !ours) {
        return std::nullopt;
    }
    const std::uint64_t pane = PaneNow(dungeon ? g_dungeon : g_field, *menu);
    float m[12] = {};
    if (pane == 0 || !CoopRuntime::Read(pane + kPaneMatrix, m, sizeof m)) {
        return std::nullopt;
    }
    const auto box = dungeon ? PaneBox(m, kDungeonW, kDungeonH) : PaneBox(m, kFieldW, kFieldH);
    if (!box) {
        return std::nullopt;
    }
    Picture p;
    p.box = *box;
    p.dungeon = dungeon;
    std::int32_t f[3] = {};
    if (dungeon && menu->hud_screen != 0 && CoopRuntime::Read(menu->hud_screen + kHudFloors, f, sizeof f) && f[1] >= 1 &&
        f[1] <= 12) {
        p.floor = f[0], p.floors = f[1], p.lowest = f[2];
    }
    return p;
}

namespace {

// A pane as the game placed it, or none (not found in the HUD screen that runs, or not readable).
std::optional<map::Pane> ReadPane(const Found& f, const Menu& menu, float w, float h) {
    const std::uint64_t pane = PaneNow(f, menu);
    map::Pane p;
    if (pane == 0 || !CoopRuntime::Read(pane + kPaneMatrix, p.m.data(), sizeof(float) * p.m.size())) {
        return std::nullopt;
    }
    p.w = w, p.h = h;
    if (!CoopRuntime::Read(pane + kPaneAlpha, &p.alpha, sizeof p.alpha) || !std::isfinite(p.alpha)) {
        p.alpha = 0.0f;  // (unreadable: drawn as hidden)
    }
    return p;
}

// What the minimap panes hold besides their matrices, to learn the game's own shown flag from a run: the
// first 0x28 bytes and the 0x58 after the matrix of the minimap's root, its picture and its frame, logged when
// the minimap comes or goes (by its matrices) or those bytes change; at most kDiagLines lines a run.
constexpr int kDiagLines = 24;
void Diagnose(const Menu& menu, bool dungeon, bool placed) {
    static std::atomic<int> lines{0};
    static std::string last;
    static std::atomic<bool> busy{false};
    if (lines.load() >= kDiagLines || busy.exchange(true)) {
        return;
    }
    std::string now;
    for (const Found* f : {&g_mini_root, dungeon ? &g_mini_dungeon : &g_mini_field, dungeon ? &g_mini_window : &g_mini_field_window}) {
        const std::uint64_t pane = PaneNow(*f, menu);
        std::uint8_t b[0xb0] = {};
        if (pane == 0 || !CoopRuntime::Read(pane, b, sizeof b)) {
            now += " -";
            continue;
        }
        now += " ";
        for (std::size_t i = 0; i < sizeof b; ++i) {
            if (i >= kPaneMatrix && i < kPaneMatrix + 48) continue;  // (the matrix: logged below as numbers)
            now += fmt::format("{:02x}", b[i]);
            if (i == kPaneMatrix - 1) now += "|";
        }
    }
    now = (placed ? "placed" : "not placed") + now;
    if (now == last) {
        busy = false;
        return;
    }
    last = now;
    for (const Found* f : {dungeon ? &g_mini_dungeon : &g_mini_field, dungeon ? &g_mini_window : &g_mini_field_window}) {
        float m[12] = {};
        const std::uint64_t pane = PaneNow(*f, menu);
        if (pane != 0 && CoopRuntime::Read(pane + kPaneMatrix, m, sizeof m)) {
            now += fmt::format(" m[{:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} | {:.1f} {:.1f} {:.1f}]",
                               m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
        }
    }
    {
        LOG_INFO(Core, "coop: map: minimap panes ({}, root/picture/frame bytes 0-27|58-af): {}",
                 dungeon ? "dungeon" : "area", now);
        ++lines;
    }
    busy = false;
}

}  // namespace

std::optional<map::MinimapPanes> Minimap(bool dungeon) {
    const auto menu = MenuState();
    if (!menu || menu->hud_screen == 0) {
        return std::nullopt;
    }
    const auto picture = dungeon ? ReadPane(g_mini_dungeon, *menu, kMiniDungeonSize, kMiniDungeonSize)
                                 : ReadPane(g_mini_field, *menu, kMiniFieldSize, kMiniFieldSize);
    const auto window = dungeon ? ReadPane(g_mini_window, *menu, kMiniWindowSize, kMiniWindowSize)
                                : ReadPane(g_mini_field_window, *menu, kMiniFieldWindowSize, kMiniFieldWindowSize);
    float alpha = 0.0f;
    const std::uint64_t root = PaneNow(g_mini_root, *menu);
    if (root == 0 || !CoopRuntime::Read(root + kPaneAlpha, &alpha, sizeof alpha) || !std::isfinite(alpha)) {
        alpha = 0.0f;
    }
    const bool placed = picture && window && map::pane_usable(*picture) && map::pane_usable(*window) && alpha > 0.0f &&
                        picture->alpha > 0.0f && window->alpha > 0.0f;  // (the frame alone over no picture: none)
    Diagnose(*menu, dungeon, placed);
    if (!placed) {
        return std::nullopt;
    }
    map::MinimapPanes mm;
    mm.picture = *picture;
    mm.window = *window;
    mm.alpha = (std::min)(alpha, 1.0f);
    return mm;
}

}  // namespace CoopMapPage
