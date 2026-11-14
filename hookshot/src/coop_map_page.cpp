// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_map_page.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <fmt/format.h>

#include "common/logging.h"
#include "coop_runtime.h"

namespace CoopMapPage {
namespace {

namespace map = coop::overlay::map;

// patch 0018's export (host/oot_menu.c)
struct HosCoopMenuState {
    std::uint32_t size;
    std::int32_t map_page, menu_open, reserved;
    std::uint64_t hud_screen, panel_screen;
};
using MenuStateFn = int (*)(HosCoopMenuState*);

MenuStateFn FindMenuState() {
#ifdef _WIN32
    static const MenuStateFn fn = [] {
        const auto f = reinterpret_cast<MenuStateFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "hos_coop_menu_state")));
        LOG_INFO(Core, "coop: map: {}", f ? "the game's menus are known (patch 0018): players on the Map page, the "
                                            "minimap's icons hidden behind menus"
                                          : "this Longshot has no menu state (patch 0018): nothing on the Map page, "
                                            "the minimap's icons stay over menus");
        return f;
    }();
    return fn;
#else
    return nullptr;
#endif
}

// The panes found (0: not found), and the screen each was looked for in.
std::atomic<std::uint64_t> g_field{0}, g_dungeon{0}, g_searched_hud{0}, g_searched_panel{0};

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
    k.function = kFindPane;
    k.x[0] = layout;
    k.x[1] = at;
    std::uint64_t pane = 0;
    return api->call(ctx, &k, &pane) ? pane : 0;
}

// Looks for both panes in `screen`'s layout; keeps what it finds (a pane found before stays).
void Search(void* ctx, const HosCoopApi* api, std::uint64_t screen, const char* which) {
    const std::uint64_t layout = Read64(screen + kScreenLayout);
    if (layout == 0) {
        return;
    }
    const std::uint64_t field = FindPane(ctx, api, layout, kFieldPane);
    const std::uint64_t dungeon = FindPane(ctx, api, layout, kDungeonPane);
    if (field != 0) g_field = field;
    if (dungeon != 0) g_dungeon = dungeon;
    float m[12] = {};
    const bool read = field != 0 && CoopRuntime::Read(field + kPaneMatrix, m, sizeof m);
    LOG_INFO(Core, "coop: map: the {} screen {:#x} (layout {:#x}): {} {:#x}, {} {:#x}{}", which, screen, layout,
             kFieldPane, field, kDungeonPane, dungeon,
             read ? fmt::format(" (matrix scale {:.3f},{:.3f} at {:.1f},{:.1f})", m[0], m[4], m[9], m[10])
                  : std::string());
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
    return Menu{s.menu_open != 0, s.map_page != 0, s.hud_screen, s.panel_screen};
}

void Frame(void* ctx, const HosCoopApi* api) {
    if (ctx == nullptr || !CanCall(api)) {
        return;
    }
    const auto menu = MenuState();
    if (!menu) {
        return;
    }
    if (menu->hud_screen != 0 && menu->hud_screen != g_searched_hud.load()) {
        g_searched_hud = menu->hud_screen;
        Search(ctx, api, menu->hud_screen, "HUD");
    }
    if (menu->panel_screen != 0 && menu->panel_screen != g_searched_panel.load()) {
        g_searched_panel = menu->panel_screen;
        Search(ctx, api, menu->panel_screen, "panel's");
    }
}

std::optional<Picture> Shown(bool dungeon) {
    const auto menu = MenuState();
    if (!menu || !menu->map_page) {
        return std::nullopt;
    }
    const std::uint64_t pane = dungeon ? g_dungeon.load() : g_field.load();
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

}  // namespace CoopMapPage
