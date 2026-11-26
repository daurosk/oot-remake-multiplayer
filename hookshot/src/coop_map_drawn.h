// SPDX-License-Identifier: GPL-3.0-or-later
// What co-op drew on the maps in the last present, as the play-test bridge's `map_icons` state member
// (docs/playtest.md "map_icons"): the others' map arrows and names are drawn through Hookshot's overlay
// (coop_draw.cpp DrawMap), and a bridge shot leaves out the overlays, so a tester reads them here.
// Test-only read: nothing here changes what is drawn. Logic only (no game); coop_draw.cpp writes it on the
// present's thread, coop_playtest.cpp reads it on the bridge's; tests/hookshot/map_drawn_test.cpp drives it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace CoopMapDrawn {

struct Icon {
    int number = 0;                 // the player's number
    float x = 0, y = 0;             // centre on the screen, pixels, y down (the present's size)
    std::optional<float> turn;      // the arrow's turn, radians clockwise (CoopDraw::MapArrowTurn); none: facing unknown
    bool arrow = false;             // the game's map arrow was drawn (else the name alone)
    bool clamped = false;           // off the map: held at its edge
    float alpha = 1;                // 0..1 as drawn (dimmer at the edge, on another floor)
    int floor_delta = 0;            // their floor minus the one shown
    std::string label;              // the words drawn under it (name or "P<n>", and the floor words)
};

struct Frame {
    std::string view;               // "minimap", "page", or "none" (no map drawn this present)
    int width = 0, height = 0;      // the present's size the icons are placed in
    std::vector<Icon> icons;
};

// Keep `f` as the last present's (DrawMap / Draw); every present writes one.
void Set(Frame f);
// The last present's frame and how many presents have written one (0: none yet).
Frame Last(std::uint64_t* presents = nullptr);
// The `map_icons` member's value: {"view":...,"presents":N,"width":W,"height":H,"icons":[{...}]}.
std::string Json(const Frame& f, std::uint64_t presents);

}  // namespace CoopMapDrawn
