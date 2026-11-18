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
#include <array>
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
// The HUD minimap in MapMenu_00 (read from the game's layout; sizes in its
// units): the picture pane the game puts the map's capture in, a dungeon's
// P_ReplaceDungeonMini_00 1300 x 1300 (under N_Dungeon_01) or an area's
// P_ReplaceFieldMini_00 960 x 960 (under N_Field_01), which the game moves,
// zooms and turns; and the window it shows through: a dungeon's frame
// P_MiniMapFrame_00, 552 x 552 at 1536 right of and 690 below the layout's
// middle, an area's clip S_FieldMini_00, 960 x 960 (its map is wider than the
// frame: run hs-minimap-icons 20261114-115734). N_MiniMap_00 holds both.
inline constexpr const char* kMiniDungeonPane = "P_ReplaceDungeonMini_00";
inline constexpr float kMiniDungeonSize = 1300.0f;
inline constexpr const char* kMiniFieldPane = "P_ReplaceFieldMini_00";
inline constexpr float kMiniFieldSize = 960.0f;
inline constexpr const char* kMiniWindowPane = "P_MiniMapFrame_00";
inline constexpr float kMiniWindowSize = 552.0f;
inline constexpr const char* kMiniFieldWindowPane = "S_FieldMini_00";
inline constexpr float kMiniFieldWindowSize = 960.0f;
inline constexpr const char* kMiniRootPane = "N_MiniMap_00";
inline constexpr std::uint64_t kFindPane = 0x76e2b0;  // main+ (MainBase() + it): (layout, "name") -> pane, or 0
inline constexpr std::uint64_t kScreenLayout = 0x28;     // a screen's layout
inline constexpr std::uint64_t kPaneMatrix = 0x28;       // a pane's global matrix
inline constexpr std::uint64_t kPaneAlpha = 0x78;        // a pane's alpha with its parents' (float; run 20261114-105315:
                                                         // the minimap's panes fade 0 -> 1 together as it comes)
inline constexpr std::uint64_t kHudFloors = 0x1368;      // the HUD screen: floor shown, count, lowest (s32)

// The game's menus, from patch 0018.
struct Menu {
    bool menu_open = false;  // a game menu is in front
    bool map_page = false;   // the panel's Map page is shown
    std::uint64_t hud_screen = 0, panel_screen = 0;
    // Patch 0018 (rows 63/76): how long ago the HUD screen last ran; -1 not since it was seen, or an exe whose patch
    // 0018 does not say (then nothing is called on the screen).
    std::int64_t hud_ran_ns_ago = -1;
    std::uint32_t hud_refreshes = 0;  // patch 0018: HUD refreshes run for AskHudRefresh so far
    // Patch 0018 (row 141): the screens that ran in the last 100 ms (none from an older exe).
    std::array<std::uint64_t, 24> screens{};
    std::uint32_t screen_count = 0;
    bool Runs(std::uint64_t screen) const {
        for (std::uint32_t i = 0; i < screen_count && i < screens.size(); ++i) {
            if (screens[i] == screen) return true;
        }
        return false;
    }
    // The HUD screen ran within the last 100 ms (a few frames).
    bool HudRuns() const { return hud_screen != 0 && hud_ran_ns_ago >= 0 && hud_ran_ns_ago < 100'000'000; }
};
// None: the exe has no patch 0018. Any thread.
std::optional<Menu> MenuState();
// Rows 63/76: asks patch 0018's hos_coop_hud_refresh (the HUD screen's own refresh at its next update) and logs the
// HUD copy before, and after once the refresh has run. False when this Longshot has no such export.
bool AskHudRefresh();

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
// "Current Location"): Y turns it to the World Map and back, so an odd count
// of Y presses since the page came up is the World Map or an area picked on
// it: not ours, no icons. Fed once a present with the page's state and
// whether Y is held in the bridge's pad word (HosCoopApi::pad_buttons). That
// word has the mouse's presses too: a click on the page's Y prompt (or the
// middle button) is host/oot_menu.c's hos_hid_tap("0:Y/75"), which nn_hid.c's
// read_input adds to the buttons (scripted_input) before it stores the word
// (hos_coop_pad), held 75 to 80 ms: several presents. (The game's own state
// of the page is not read: not found yet; the in-game check says whether the
// count ever goes wrong.)
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

// Row 141, the game's own top-left HUD: the player status screen's layout,
// PlayerStatus_00 (the hearts, the magic bar and the d-pad item slots), told
// from the other running screens by its d-pad pane. Its panes and their
// boxes in their own units (y up from the pane's origin), from the game's
// layouts: Pa_HeartGauge_00 1300 x 212, Pa_MpGauge_00 1256 x 155.2 and
// Pa_AgonyStone_00 200 x 200 (centred); in Pa_GuideCross_00 (PaGuideCross_00:
// N_All_00 at -1680,264, N_Down_00 at -84,36, Pa_Down_00 at 0,-134) the
// bottom slot, 257.387 square centred at -1764,166. N_HeartPos_00 and
// N_MpGauge_00 above them move with the heart rows and the magic bar, and
// the global matrices read at each present have that in them.
inline constexpr const char* kStatusDpadPane = "Pa_GuideCross_00";
inline constexpr float kDpadSlot = 257.387f, kDpadDownX = -1764.0f, kDpadDownY = 166.0f;
inline constexpr const char* kStatusHeartsPane = "Pa_HeartGauge_00";
inline constexpr float kHeartsW = 1300.0f, kHeartsH = 212.0f;
inline constexpr const char* kStatusMagicPane = "Pa_MpGauge_00";
inline constexpr float kMagicW = 1256.0f, kMagicH = 155.2f;
inline constexpr const char* kStatusAgonyPane = "Pa_AgonyStone_00";
inline constexpr float kAgonySize = 200.0f;

// The bottom of the game's own top-left HUD this frame (the lowest of its
// shown hearts, magic bar, d-pad bottom slot and Stone of Agony), as a
// fraction of the picture height from its top (CoopHudLayout::HudBottom);
// while it cannot be read (faded out, its screen stopped behind a menu), the
// last one read (CoopHudLayout::Hold). None until the player status screen
// has been found and read once (an exe without patch 0018's screens). Any
// thread.
std::optional<double> GameHudBottom();

// On the game's thread with a context that may call (hos_coop_frame's): finds
// the map panes in the screens' layouts once (and again for a new screen),
// logging what it found.
void Frame(void* ctx, const HosCoopApi* api);
// While the Map page is shown with our place's map (PageWatch) and its pane,
// found in a screen that still runs, is shown: the picture. `dungeon` picks
// the pane (our place's map is a dungeon's). The present's thread (PageWatch).
std::optional<Picture> Shown(bool dungeon);

// The HUD minimap as the game placed it this frame: its picture pane (`dungeon`
// picks which) and its frame, from the HUD screen that runs now. None when a
// pane was not found or the game has not placed it (a matrix of 0: never shown
// since the screen came up), so no icons are drawn where the game draws no
// minimap. Any thread. Also logs, a few times a run, the panes' state bytes
// whenever the minimap comes or goes (to learn the game's own shown flag).
std::optional<coop::overlay::map::MinimapPanes> Minimap(bool dungeon);

}  // namespace CoopMapPage
