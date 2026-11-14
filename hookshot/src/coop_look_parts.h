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
// a CoopLookTunic::Wardrobe, which finds the textures in the game's memory
// and writes them, as for the tunic.
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
// pack's per-slot item copies name textures of their own, "Mt_..." as
// "C<slot>_..." (tools/puppet_pack/look_items.py tex_name). One wardrobe
// keeps every slot's textures in the look of the player in that slot.
//
// A gear texture's name in slot `slot` (1 to 9); "" for a name that is not
// a gear texture's ("Mt_..." is).
std::string SlotName(const std::string& name, std::size_t slot);
// The looks worn in the slots (index 0: slot 1; nothing: a free slot).
using SlotLooks = std::vector<std::optional<CoopLook::Look>>;
// The wardrobe's base: the game's own texels under every slot's names.
inline constexpr const char* kSlotsBase = "puppets";
// The name of the slots' painting: kSlotsBase when no slot sets a gear part.
std::string SlotsName(const SlotLooks& looks);
// Every gear texture once per slot under the slot's name, painted in that
// slot's look (as it is in a free slot): looks.size() slots, at most 9.
CoopLookTunic::Texels SlotTexels(const CoopLookTunic::Texels& gear, const SlotLooks& looks);

}  // namespace CoopLookParts
