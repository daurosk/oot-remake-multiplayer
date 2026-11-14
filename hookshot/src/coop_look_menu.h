// SPDX-License-Identifier: GPL-3.0-or-later
// The Link customizer: the Customize page of the Multiplayer tab (docs/puppet.md,
// "The Link customizer"). On the left, every part of Link's look by item (the
// tunics, the shields' frames and faces, the swords' blades and hilts, the
// boots, the gauntlets): Left and Right try the preset colours, Confirm mixes
// one's own (red, green and blue). On the right, a picture of Link in the
// colours chosen, with the part selected outlined: the shield, sword and
// tunic shown are the ones whose part was looked at last.
//
// Why a picture and not the game's model: the menu draws over a paused game
// and has no way to draw a model (Hookshot's menu draws flat shapes, patch
// 0019); a picture of Link's outline in the real colours shows the result at
// once, part by part. The game's own Link wears the look when the menu closes.
//
// This file is the page's logic only (no game, no files): the tab
// (coop_pause_menu.h) hands it the presses and draws its View; a change is
// handed on with TakeChange (the module: CoopLook::SetPending).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "coop_look.h"

namespace CoopLookMenu {

enum class Input { kUp, kDown, kLeft, kRight, kDecide, kBack };

// A row as the tab draws it (kind: hos_coop_menu.h's HOS_COOP_MENU_*: 0
// action, 1 choice, 3 heading, 4 note).
struct Row {
    std::string name;
    std::string value;
    std::uint8_t kind = 0;
    bool left = false, right = false;
};

// One shape of the picture (hos_coop_menu_picture.h): a convex polygon in the
// unit square (y down), filled or outlined, 0xRRGGBBAA.
struct Shape {
    std::uint32_t rgba = 0;
    bool outline = false;
    std::vector<std::pair<float, float>> points;
};

struct View {
    std::vector<Row> rows;
    int selected = -1;
    std::string help_title, help, change, confirm, back;
    std::vector<Shape> picture;
};

// The preset colours, after the game's own (unset): CoopColors' names.
const std::vector<std::string>& Presets();

// The colour a part shows in the picture: the look's, else the game's own.
std::uint32_t Shown(const CoopLook::Look& look, CoopLook::Part part);

class Page {
public:
    // The page opens on the look shown now (the one waiting, else the one worn).
    void Open(const CoopLook::Look& look);
    // One press; false for a Back on the list (the tab goes back to its first page).
    bool Press(Input input);
    // The mouse over a row, and a click on its name (0) or the left (-1) or right (1) half of its value.
    void Select(std::size_t row);
    void Click(std::size_t row, int side);
    // The look after a change, once.
    std::optional<CoopLook::Look> TakeChange();

    const CoopLook::Look& look() const { return look_; }
    bool editing() const { return editing_.has_value(); }
    View view() const;

private:
    struct Line;
    std::vector<Line> Lines() const;
    bool Selectable(std::size_t row) const;
    void Move(int step);
    void Change(std::size_t row, int step);
    void Decide(std::size_t row);
    void Set(CoopLook::Part part, std::optional<std::uint32_t> colour);
    void Focus(CoopLook::Part part);   // the picture shows this part's item

    CoopLook::Look look_;
    std::optional<CoopLook::Part> editing_;   // the colour mixer's part; none: the list
    std::size_t selected_ = 0;
    std::size_t list_selected_ = 0;           // where the list was, for the way back from the mixer
    bool changed_ = false;
    CoopLook::Part tunic_ = CoopLook::Part::tunic;          // the tunic, shield and sword in the picture
    CoopLook::Part shield_ = CoopLook::Part::hylian_face;
    CoopLook::Part sword_ = CoopLook::Part::master_blade;
};

}  // namespace CoopLookMenu
