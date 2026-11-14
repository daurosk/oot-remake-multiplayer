// SPDX-License-Identifier: GPL-3.0-or-later
// The game's own map screens, for the other players on them (docs/overlay.md,
// "Players on the map"): whether a game menu is in front (the minimap's icons
// hide behind it) and, while the panel's Map page is shown, where its map
// picture is on the screen, as the game's layout places it (its pan and zoom
// included).
//
// Longshot's patch 0018 (host/oot_menu.c, hos_coop_menu_state, found by name
// in the exe) says which screens run: the HUD screen (its layout, MapMenu_00,
// holds the minimap and the Map page's map) and the panel's frame. The map
// picture's pane is found by name in a screen's layout with the game's own
// lookup (f_8078e2b0, as oot_menu.c finds Key Items' panes) on the game's
// thread while the game plays; its global matrix (pane +0x28, 3 x 4 by
// column: the scale at [0] and [4], the translation at [9] and [10], in the
// layout's units, y up from the middle) is read at each present, the game's
// pause included. An exe without patch 0018: no menu state (the minimap's
// icons do not hide behind menus) and nothing on the Map page.
#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

#include "coop/overlay/map_icons.hpp"
#include "hos_coop_api.h"

namespace CoopMapPage {

// Map pictures in MapMenu_00 (their size in the layout's units): an area's
// N_PatternField_00, a dungeon's N_PatternDungeon_00.
inline constexpr const char* kFieldPane = "N_PatternField_00";
inline constexpr float kFieldW = 2600.0f, kFieldH = 1872.0f;
inline constexpr const char* kDungeonPane = "N_PatternDungeon_00";
inline constexpr float kDungeonW = 2600.0f, kDungeonH = 2600.0f;
inline constexpr std::uint64_t kFindPane = 0x8078e2b0;  // (layout, "name") -> pane, or 0
inline constexpr std::uint64_t kScreenLayout = 0x28;     // a screen's layout
inline constexpr std::uint64_t kPaneMatrix = 0x28;       // a pane's global matrix
inline constexpr std::uint64_t kHudFloors = 0x1368;      // the HUD screen: floor shown, count, lowest (s32)

// The game's menus, from patch 0018.
struct Menu {
    bool menu_open = false;  // a game menu is in front
    bool map_page = false;   // the panel's Map page is shown
    std::uint64_t hud_screen = 0, panel_screen = 0;
};
// None: the exe has no patch 0018. Any thread.
std::optional<Menu> MenuState();

// A pane's box in the layout's units (x right, y down from the top left of
// 3840 x 2160) from its global matrix and its size; none for a matrix that
// is not a pane shown (a scale of 0 or below, NaN).
inline std::optional<coop::overlay::map::Box> PaneBox(const float m[12], float w, float h) {
    const float sx = m[0], sy = m[4], tx = m[9], ty = m[10];
    if (!(sx > 1e-3f) || !(sy > 1e-3f) || !std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(tx) ||
        !std::isfinite(ty) || !(w > 0) || !(h > 0)) {
        return std::nullopt;
    }
    const float bw = w * sx, bh = h * sy;
    const float cx = 1920.0f + tx, cy = 1080.0f - ty;  // (the layout's y is up from its middle)
    return coop::overlay::map::Box{cx - bw / 2, cy - bh / 2, bw, bh};
}
// (On the picture: CoopDraw::LayoutToScreen.)

// The Map page's map now, in the layout's units, and the floors the game
// shows (dungeons: floor shown, lowest, how many; 2F 1, 1F 0, B1 -1).
struct Picture {
    coop::overlay::map::Box box;
    bool dungeon = false;
    int floor = 0, lowest = 0, floors = 0;
};

// Whether the Map page shows our own place's map (the one it opens on, Y's
// "Current Location"): Y (the pad's, or the mouse's tap on its prompt) turns
// it to the World Map and back, so an odd count of Y presses since the page
// came up is the World Map or an area picked on it: not ours, no icons. Fed
// once a present with the page's state and whether Y is held.
struct PageWatch {
    bool shown = false, y_was = false, ours = true;
    bool Update(bool map_page, bool y_down) {
        if (!map_page) {
            shown = false, y_was = false, ours = true;
            return false;
        }
        if (!shown) {
            shown = true, ours = true, y_was = y_down;   // (a Y held as the page came up is not a press on it)
        } else if (y_down && !y_was) {
            ours = !ours;
        }
        y_was = y_down;
        return ours;
    }
};
inline constexpr std::uint64_t kPadY = 1ull << 3;  // Switch NpadButton Y

// On the game's thread with a context that may call (hos_coop_frame's): finds
// the map panes in the screens' layouts once (and again for a new screen),
// logging what it found.
void Frame(void* ctx, const HosCoopApi* api);
// While the Map page is shown with our place's map (PageWatch) and its pane,
// found in a screen that still runs, is shown: the picture. `dungeon` picks
// the pane (our place's map is a dungeon's). The present's thread (PageWatch).
std::optional<Picture> Shown(bool dungeon);

}  // namespace CoopMapPage
