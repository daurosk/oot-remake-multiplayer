// SPDX-License-Identifier: GPL-3.0-or-later
// The Link customizer: the Customize page of the Multiplayer tab (docs/puppet.md,
// "The Link customizer"). On the left, every part of Link's look by item (the
// tunics, the shields' frames and faces, the swords' blades and hilts, the
// boots, the gauntlets): Left and Right try the preset colours, Confirm picks
// any colour (hue, saturation and brightness: the whole range, 0.11.9). On the right, a picture of Link in the
// colours chosen, with the part selected outlined: the shield, sword and
// tunic shown are the ones whose part was looked at last.
//
// The picture: when your Link is in the game, the tab shows him instead, the
// real one through a clear window, the game's camera around him
// (coop_look_view.h, patch 0019). This outline of Link in the colours chosen
// is for when there is none (the title screen, a cutscene).
//
// This file is the page's logic only (no game, no files): the tab
// (coop_pause_menu.h) hands it the presses and draws its View; a change is
// handed on with TakeChange (the module: CoopLook::SetPending).
#pragma once

#include <array>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "coop_look.h"

namespace CoopLookMenu {

// (kPagePrev / kPageNext: ZL and ZR on the Character page; kResetAll: X there, every part to the game's own)
enum class Input { kUp, kDown, kLeft, kRight, kDecide, kBack, kPagePrev, kPageNext, kResetAll };

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

// A colour as the mixer shows it (0.11.9: any colour): hue 0..359 degrees, saturation and brightness 0..100 %.
struct Hsv {
    int h = 0, s = 0, v = 0;
    bool operator==(const Hsv&) const = default;
};
Hsv ToHsv(std::uint32_t rgb);
std::uint32_t ToRgb(const Hsv& colour);

// The Character page (0.11.9, Snail's Customize layout, the owner's): the parts in pages of six, one screen each and
// nothing below a scroll: Clothes, Shields, Swords. Each row is a part's full name over its colour.
struct Group {
    const char* name;
    std::vector<CoopLook::Part> parts;   // at most six
};
const std::vector<Group>& Groups();
// A part's name on the Character page ("Kokiri Tunic", "Hylian Shield Frame", "Biggoron's Sword Hilt").
std::string PagedName(CoopLook::Part part);
// What a character other than Link shows (the owner: never a row that does nothing): only the gear pages, and only
// once a run has shown our gear colours on that character's held items; else this one line.
inline constexpr const char* kNoColourOptions = "This character has no colour options";
// npc-customize: the index of the Character group (a character's own parts) in Groups().
inline constexpr std::size_t kCharacterGroup = 4;

// The preset colours, after the game's own (unset): CoopColors' names.
const std::vector<std::string>& Presets();

// The colour a part shows in the picture: the look's, else the game's own.
std::uint32_t Shown(const CoopLook::Look& look, CoopLook::Part part);

class Page {
public:
    // The page opens on the look shown now (the one waiting, else the one worn).
    void Open(const CoopLook::Look& look);
    // The Character page's way (Groups: a page of six at a time, ZL / ZR to turn); off: the whole list (the overlay).
    void SetPaged(bool on);
    // The character shown (Character Select's number; 0 Link). `gear_shows`: our gear colours were seen on a
    // character's held items (the owner's check), so its Shields and Swords pages are offered.
    void SetCharacter(std::uint16_t character, bool gear_shows);
    // npc-customize: the rows the character's model has (CoopCharColours::Rows' bits; 0 not known: every row).
    void SetRows(std::uint8_t rows);
    // The page of groups shown (0..Groups().size()-1) and how many are offered.
    std::size_t GroupShown() const;
    std::size_t GroupsOffered() const;
    // The page shown among those offered (0 first), as its heading counts it ("page N of M": N - 1).
    std::size_t PageIndex() const;
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
    void ResetAll();                   // every part of the model shown to the game's own

    std::vector<std::size_t> Offered() const;   // the groups this character has (indexes into Groups())
    void TurnPage(int step);

    CoopLook::Look look_;
    bool paged_ = false;
    std::uint16_t character_ = 0;
    bool gear_shows_ = false;
    std::uint8_t rows_ = 0;
    std::size_t group_ = 0;   // index into Offered()
    std::optional<CoopLook::Part> editing_;   // the colour mixer's part; none: the list
    Hsv hsv_;                                 // the mixer's colour (kept as it is moved: no rounding drift)
    std::size_t selected_ = 0;
    std::size_t list_selected_ = 0;           // where the list was, for the way back from the mixer
    bool changed_ = false;
    CoopLook::Part tunic_ = CoopLook::Part::tunic;          // the tunic, shield and sword in the picture
    CoopLook::Part shield_ = CoopLook::Part::hylian_face;
    CoopLook::Part sword_ = CoopLook::Part::master_blade;
};

}  // namespace CoopLookMenu
