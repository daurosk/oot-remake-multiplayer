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
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
// painting goes by in the wardrobe: "gear" when the look sets no gear part
// (the tunic and the Cap trim are not gear: TunicPainting).
inline constexpr const char* kBase = "gear";
std::string Name(const CoopLook::Look& look);
bool AnyGear(const CoopLook::Look& look);

// The gear texels painted in `look` (same textures, same sizes); a texture
// with no part set, or not in the table, as it is. A metalness texture
// (UseMetal) has the blocks under each painted metal part set to 0.
CoopLookTunic::Texels Painted(const CoopLookTunic::Texels& gear, const CoopLook::Look& look);

// The metal parts (#202). The swords, the Hylian and Mirror Shields and the
// gauntlets have a metalness texture (`Mt_..._Mtl`, BC4) beside their colour
// one; the game draws a metal texel's colour as what it reflects, with no
// diffuse light, so a metal part painted only in its colour texture shows
// much darker than the colour picked. The pack lists, for each such texture
// and part, the byte offsets of its BC4 blocks under that part, every mip
// level (tools/puppet_pack/look_metal.py, coop_look/metal.bin; gear.bin
// holds the textures' own texels). Painted sets those blocks to 0 (no metal)
// when the part has a colour, so the part looks painted; with no colour the
// texture stays the game's own.
struct MetalBlocks {
    std::string texture;   // "Mt_Weapon_Shield_Hylian_Mtl"
    CoopLook::Part part;
    std::vector<std::uint32_t> offsets;   // of 8-byte BC4 blocks in its texels
};
using Metal = std::vector<MetalBlocks>;
// metal.bin's bytes; nothing (and `why`) when they are not one.
std::optional<Metal> ParseMetal(const std::vector<std::uint8_t>& file, std::string* why = nullptr);
// The table Painted uses from now on (nothing: no metal is painted down).
void UseMetal(std::shared_ptr<const Metal> metal);
// The blocks of `texels` (the texture `name`) under the parts `look` paints, set to 0.
// Returns how many blocks it set.
std::size_t PaintMetal(const Metal& metal, const std::string& name, std::vector<std::uint8_t>& texels,
                       const CoopLook::Look& look);

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
// own colours when none) and marked (Mark): what that copy's pieces are drawn
// with.
CoopLookTunic::Texels CopyTexels(const CoopLookTunic::Texels& gear, std::size_t copy,
                                 const std::optional<CoopLook::Look>& look);
// A copy's texels as the pack's copy files hold them: the image's last byte
// XOR the copy's number (tools/puppet_pack/look_live.py mark). The game keeps
// no name with a texture's texels, so only this tells a copy's textures from
// your own Link's. That byte is in the last mip level or the padding past it.
void Mark(std::vector<std::uint8_t>& texels, std::size_t copy);
// The looks the copies are painted in (index 0: copy 1; nothing: the game's own).
using CopyLooks = std::vector<std::optional<CoopLook::Look>>;
// The copies' wardrobe's base (every copy in the game's own colours), and the
// name of a painting of them: kCopiesBase when no copy has a gear part set.
inline constexpr const char* kCopiesBase = "puppets";
std::string CopiesName(const CopyLooks& looks);

// Your own Link's Kokiri tunic in any colour, painted while the game runs as
// the gear is. The pack's coop_look/green.bin holds the game's own texels of
// every tunic texture of both ages (marked as your own Link's green copies
// are, tools/puppet_pack/look_live.py); the kit puts green's files in for your
// own Link at every start, so those are the texels the game's memory holds,
// and the tunic wardrobe draws them painted in the exact colour chosen. The
// other players' puppets of you keep the pack's tunic colours (TunicName).
//
// The rules are the pack tool's (tools/puppet_pack/tunic.py, tunic_extra.py):
// the tile is recoloured whole, the other textures only in their green
// (CoopLookPaint::IsGreen), every one shaded against the tile's average, so
// each keeps its brightness against the tile (the child's dark tile stays
// about 0.64 of it, tunic.py DARK_SCALE).
struct TunicTexture {
    const char* name;   // in the game and in green.bin ("Angin_Tile_A_Alb")
    CoopLookPaint::Format format;
    CoopLookPaint::Takes takes;
};
// Every tunic texture (tools/puppet_pack/tunic.py TILE, DARK and
// tunic_extra.py EXTRA: the same names, in the same order as green.bin).
const std::vector<TunicTexture>& TunicTextures();
// The tile whose average every tunic texture is shaded against.
inline constexpr const char* kTunicTile = "Angin_Tile_A_Alb";
// The tunic wardrobe's base: the game's own green (green.bin).
inline constexpr const char* kTunicBase = "green";
// The name a tunic colour's painting goes by in the wardrobe: kTunicBase for
// none (the game's own), "tunic-RRGGBB" for a colour; TunicColour reads it
// back (nothing for any other name). With a Cap trim colour (below) the name
// says both: "tunic-RRGGBB-cap-RRGGBB", or "tunic-green-cap-RRGGBB" on the
// game's own green; TunicPaintOf reads every painting's name back.
std::string TunicPainting(const std::optional<std::uint32_t>& colour,
                          const std::optional<std::uint32_t>& cap = std::nullopt);
std::optional<std::uint32_t> TunicColour(const std::string& name);
// The tunic texels of `green` painted in `colour` (0xRRGGBB): the same
// textures, sizes and order; a texture not in TunicTextures as it is.
CoopLookTunic::Texels TunicPainted(const CoopLookTunic::Texels& green, std::uint32_t colour);

// The Cap trim (CoopLook::Part::cap_trim): your own Link's, and since 0.11.9 each player's tunic copy on the puppets
// (PuppetTunicsTexels). The cap's
// cloth is the tunic's tile (no texel of it is the cap's alone), so it keeps
// the tunic's colour; but three tunic textures have blocks only the cap uses:
// the child's headband and knot (Mt_Cloth_Parts_Player_Child_Alb) and the
// adult's hood trims (Mt_Vest_Player_Adult_Alb, Mt_Angin_Acs_Player_Adult_Alb).
// The pack lists them, from Link's models (tools/puppet_pack/look_cap.py,
// coop_look/cap.bin: per texture the byte offsets of those blocks in
// green.bin's texels, every mip level). A painting paints them after the
// tunic, from green's own texels, by the tunic's rule for those textures
// (their green ends, shaded against the tile's average). With no cap colour
// they follow the tunic; a pack without cap.bin paints no cap blocks.
struct CapBlocks {
    std::string texture;                  // "Mt_Cloth_Parts_Player_Child_Alb"
    std::uint32_t block = 0;              // bytes of a block: 8 (BC1) or 16
    std::vector<std::uint32_t> offsets;   // of its blocks in the texels
};
using Cap = std::vector<CapBlocks>;
// cap.bin's bytes; nothing (and `why`) when they are not one.
std::optional<Cap> ParseCap(const std::vector<std::uint8_t>& file, std::string* why = nullptr);
// What a tunic painting holds: the tunic's colour (nothing: the game's own
// green) and the Cap trim's (nothing: as the tunic).
struct TunicPaint {
    std::optional<std::uint32_t> tunic;
    std::optional<std::uint32_t> cap;
    bool operator==(const TunicPaint&) const = default;
};
// A painting's name read back (TunicPainting); nothing for kTunicBase and for
// a name that is no painting (a pack colour's file).
std::optional<TunicPaint> TunicPaintOf(const std::string& name);
// The tunic texels of `green` in `paint`: the tunic's colour first, then the
// cap's blocks (`cap`, may be null) in the cap's colour.
CoopLookTunic::Texels TunicPainted(const CoopLookTunic::Texels& green, const TunicPaint& paint, const Cap* cap);
// The cap's blocks of the texture `name` (in `format`) painted in `colour`:
// each block of `green` (the texture's own texels) recoloured into `texels`
// (as long), shaded against `base`; the other bytes stay. Returns how many
// blocks changed. An offset not on a block, past the texels, or of another
// block size is passed over.
std::size_t PaintCap(const Cap& cap, const std::string& name, CoopLookPaint::Format format,
                     std::span<const std::uint8_t> green, std::vector<std::uint8_t>& texels, std::uint32_t colour,
                     double base);

// The boots in Link's own model (the adult's Kokiri boots, the child's
// leather, which holds his belts too): not an item, so no item copies. Every
// Link draws one texture of a name; the pack's puppet body models, one per
// tunic colour, each name a copy of their own, "Mt_..." as "P<letter>_..."
// (tools/puppet_pack/look_items.py BODY_BOOTS, boots_name, boots_mark: the
// same names, letters and marks). One more wardrobe keeps each colour's
// copies in the boots colour of the player who wears that colour. Your own
// Link's model (the game's, or its own copy) names the game's boots: the
// gear wardrobe paints those (Gear() lists them).
//
// The body boots textures (a subset of Gear()).
const std::vector<std::string>& BodyBoots();
// Each tunic colour's letter, by the name CoopLook::TunicName gives ("red",
// ..., "green"): tools/puppet_pack/tunic.py COLORS. Nothing for another name.
std::optional<char> LetterOf(std::string_view tunic);
// A body boots texture's name in colour `letter`'s models; "" if `name` is not one.
std::string BootsName(const std::string& name, char letter);
// Each colour's boots colour (0xRRGGBB) by its letter; a letter not in it
// keeps the game's colour.
using Wearers = std::map<char, std::uint32_t>;
// Who paints which colour's boots: your own Link first (`own`: the letter
// of the model he wears, and his boots colour), then the others in the
// order given (the first of a letter has it; nothing: the game's colour).
struct Wearer {
    std::optional<char> letter;
    std::optional<std::uint32_t> boots;
};
Wearers Assign(const Wearer& own, const std::vector<Wearer>& others);
// The boots wardrobe's base (every colour's copies in the game's colour) and
// the name of a painting: kBootsBase when no colour has boots set.
inline constexpr const char* kBootsBase = "boots";
std::string BootsPaintingName(const Wearers& wearers);
// Every colour's copies of the body boots in `gear` (renamed, painted for
// its wearer, marked with the letter's code as the pack marks them).
CoopLookTunic::Texels BootsTexels(const CoopLookTunic::Texels& gear, const Wearers& wearers);

// 0.11.9, exact puppet tunics: the coloured tunic copies (coop_look/puppet-<colour>.bin, "red".."black", each the
// textures of green.bin in its order), each repainted for the player it is given to (CoopColors::AssignTunicCopies).
// The wardrobe's base (every copy as the pack wrote it), and a painting's name: kPuppetTunicsBase when none is
// painted, else "ptunics|<colour>=RRGGBB|..." by colour.
inline constexpr const char* kPuppetTunicsBase = "ptunics";
// colour ("red") -> what it is painted in: its player's exact tunic colour and Cap trim (0.11.9: the trim too)
using TunicPaints = std::map<std::string, TunicPaint>;
std::string PuppetTunicsName(const TunicPaints& paints);
// The copies' texels for a painting: a copy given to a player, every texture of it from green painted in that
// colour (TunicPainted) where it is the copy's size; a texture of another size (the child's dark tile, BC3 in the
// copies and ASTC in green) and every other copy as the pack wrote it. `copies`: (colour, its texels), in order.
// The copies' dark tile (the child's tunic body) is the main tile recoloured darker (tunic.py dark_tile_for,
// DARK_SCALE): it is painted so from green's main tile in the colour scaled by kDarkScale (review round 2).
inline constexpr float kDarkScale = 0.64f;
// `cap`: the Cap trim's blocks (cap.bin; may be null: the trim as the tunic).
CoopLookTunic::Texels PuppetTunicsTexels(const std::vector<std::pair<std::string, CoopLookTunic::Texels>>& copies,
                                         const CoopLookTunic::Texels& green, const TunicPaints& paints,
                                         const Cap* cap = nullptr);

}  // namespace CoopLookParts
