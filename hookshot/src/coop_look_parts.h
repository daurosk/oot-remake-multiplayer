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
#include <filesystem>
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
// pack's item copies name texture files of their own, "Mt_..." as
// "C<copy>_..." (tools/puppet_pack/look_items.py tex_name), kept as zstd
// frames of raw blocks. The module writes a copy's files again in a player's
// look before their puppet wears it; the game loads them through its own file
// system (Hookshot opens a mod file each time it is read).
//
// A gear texture's name in copy `copy` (1 to 9); "" for a name that is not
// a gear texture's ("Mt_..." is), or that could name another folder (a
// separator or "..": the name becomes a file name).
std::string SlotName(const std::string& name, std::size_t copy);
// A zstd frame of raw blocks holding `data` (look_items.py raw_zstd), and its
// data again (nothing for any other frame).
std::vector<std::uint8_t> RawZstd(const std::vector<std::uint8_t>& data);
std::optional<std::vector<std::uint8_t>> UnrawZstd(const std::vector<std::uint8_t>& frame);
// A copy's texture file (a raw frame of its BNTX) with `texture`'s texels in
// place of its own: nothing when the file is not a raw frame or the texels do
// not fit at their offset.
std::optional<std::vector<std::uint8_t>> Repainted(const std::vector<std::uint8_t>& frame,
                                                   const CoopLookTunic::Texture& texture);

// Paints copy `copy` of the items in `look`: for each gear texture the copy
// has a file of in `tex` (the pack's romfs/Tex), the game's own texels
// (`gear`, from coop_look/gear.bin) painted in the look are written into that
// file (a ".part" file renamed over it; a file already in these colours is
// left as it is). Only the textures the copy has are painted.
struct CopyResult {
    enum class Status {
        written,   // every file the copy has is in the look
        retry,     // a file could not be written now (held open, a full disk): try again later
        broken,    // a file is not a copy this code can paint (make the pack again)
    };
    Status status = Status::written;
    std::size_t files = 0;   // in the look now
    std::string why;         // for retry and broken
};
CopyResult PaintCopyFiles(const std::filesystem::path& tex, const CoopLookTunic::Texels& gear, std::size_t copy,
                          const CoopLook::Look& look);

}  // namespace CoopLookParts
