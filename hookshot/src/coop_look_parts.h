// SPDX-License-Identifier: GPL-3.0-or-later
// Your Link's gear in the colours of its look (docs/puppet.md, "The Link
// customizer"): which textures hold which parts, and those textures painted.
//
// Each shield, sword, pair of boots and pair of gauntlets has one colour
// texture holding all its parts; the Goron and Zora tunics have a few. A
// part is told from the rest by the tone of its end colours
// (coop_look_paint.h), as read from the game's own textures: the Hylian
// Shield's frame is grey, its face blue, its bird red; a sword's blade grey.
//
// The puppet pack has the game's own texels of these textures
// (coop_look/gear.bin, tools/puppet_pack/look_live.py, the same file format as
// the tunic's colours). The module paints them in the look and hands them to
// a CoopLookTunic::Wardrobe, which finds the textures the game made
// and has Longshot draw them painted (texture overrides), as for the tunic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "coop_look.h"
#include "coop_look_paint.h"
#include "coop_look_tunic.h"

namespace CoopLookParts {

struct GearRule {
    CoopLook::Part part;
    std::uint32_t tones;   // CoopLookPaint::Tone bits
};
struct GearTexture {
    const char* name;      // in the game ("Mt_Weapon_Shield_Hylian_Alb")
    CoopLookPaint::Format format;
    std::vector<GearRule> rules;
};
// Every gear texture (tools/puppet_pack/look_live.py GEAR lists the same names).
const std::vector<GearTexture>& Gear();

// The pack's file of the game's own gear texels, and the name a look's
// painting goes by in the wardrobe: "gear" when the look sets no gear part.
inline constexpr const char* kBase = "gear";
std::string Name(const CoopLook::Look& look);
bool AnyGear(const CoopLook::Look& look);

// The gear texels painted in `look` (same textures, same sizes); a texture
// with no part set, or not in the table, as it is.
CoopLookTunic::Texels Painted(const CoopLookTunic::Texels& gear, const CoopLook::Look& look);

// The puppets' gear (docs/puppet.md, "Gear colours on the puppets"): the
// pack's item copies name textures of their own, "Mt_..." as "C<copy>_..."
// (tools/puppet_pack/look_items.py tex_name). One more wardrobe keeps every
// copy's textures in the look of the player it is painted for, drawn by
// Longshot's texture overrides as your own gear is (nothing of the game is
// written; the game loads and draws the copies itself).
//
// A gear texture's name in copy `copy` (1 to 9); "" for a name that is not
// a gear texture's ("Mt_..." is), or that could name another folder.
std::string SlotName(const std::string& name, std::size_t copy);
// The gear textures under copy `copy`'s names, painted in `look` (the game's
// own colours when none): what that copy's pieces are drawn with.
CoopLookTunic::Texels CopyTexels(const CoopLookTunic::Texels& gear, std::size_t copy,
                                 const std::optional<CoopLook::Look>& look);
// The looks the copies are painted in (index 0: copy 1; nothing: the game's own).
using CopyLooks = std::vector<std::optional<CoopLook::Look>>;
// The copies' wardrobe's base (every copy in the game's own colours), and the
// name of a painting of them: kCopiesBase when no copy has a gear part set.
inline constexpr const char* kCopiesBase = "puppets";
std::string CopiesName(const CopyLooks& looks);

}  // namespace CoopLookParts
