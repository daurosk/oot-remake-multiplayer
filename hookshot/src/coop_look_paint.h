// SPDX-License-Identifier: GPL-3.0-or-later
// A texture's parts in another colour, any colour, without decoding it
// (docs/puppet.md, "The Link customizer").
//
// Link's shields, swords, boots and gauntlets each have one colour texture
// (the albedo), BC1 or ASTC, holding every part of the item: the Hylian
// Shield's grey frame, blue face and red bird lie side by side in one image.
// Every block of such a texture has a few end colours and its texels are
// blends of them, so changing the ends of one kind (the blue ones) recolours
// that part and leaves the others: no encoder, same size, same layout.
// A part is a set of tones (ToneOf): which tones make which part of which
// item is coop_look_parts.cpp's table, read from the game's own textures.
//
// The same rewrite the puppet pack tool makes for the tunic's other textures
// (tools/puppet_pack/tunic_extra.py), here in C++ so the module can paint any
// colour the player picks while the game runs: the gear by tones, your own
// Kokiri tunic by the tool's own rules (Takes, Rule::base;
// CoopLookParts::TunicPainted). ASTC blocks keep their
// weights; their colour values are re-encoded in the same range (base+offset
// modes become direct ones, same value count). A block that cannot be
// re-encoded exactly as asked stays as it was.
//
// No game, no memory: bytes in, bytes out. Any thread.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace CoopLookPaint {

// What a colour looks like, one bit each, from its hue and how grey it is.
enum Tone : std::uint32_t {
    kBlack = 1u << 0,    // too dark to say
    kGrey = 1u << 1,     // metal, a blade, white
    kRed = 1u << 2,
    kBrown = 1u << 3,    // orange hue, not bright: leather, wood
    kGold = 1u << 4,     // orange or yellow hue, bright
    kYellow = 1u << 5,   // yellow hue, not bright
    kGreen = 1u << 6,
    kCyan = 1u << 7,
    kBlue = 1u << 8,
    kPurple = 1u << 9,
    kPink = 1u << 10,
    // Row 185: a pale warm grey, always with kGold: a bright colour of gold hue whose spread is under a
    // quarter of its brightness (the Hylian Shield's frame shades, ~(168, 168, 136)). Rules that take it
    // (the frame) paint it; every other rule sees only its kGold, as before (the real gold stays gold).
    kWarmGrey = 1u << 11,
};
using Rgb = std::array<std::uint8_t, 3>;
std::uint32_t ToneOf(const Rgb& c);

// The tunic's green (tools/puppet_pack/tunic_extra.py is_green): hue 40 to
// 180, olive included, not brown leather (about 30), grey or white. Wider than
// kGreen | kYellow: the pale greens of the adult's lower tunic are grey or gold
// by ToneOf, so a rule of tones left them green.
bool IsGreen(const Rgb& c);

// Which end colours a rule takes: those of its `tones`, the tunic's green
// (IsGreen), or every one (the tunic's tile: tools/puppet_pack/tunic.py
// recolours all of it).
enum class Takes : std::uint8_t { tones, green, all };

// One part: its end colours (any of `tones`) become `colour` (0xRRGGBB), each
// as bright against the part's average as it was, so the shading stays and
// the part's average becomes the colour. `base` (above 0): the linear luma
// that becomes the colour instead of the part's own average: the tunic's
// textures are all shaded against the tile's average, so a darker texture
// (the child's dark tile) stays as much darker as it was.
struct Rule {
    Rule() = default;
    Rule(std::uint32_t tones_, std::uint32_t colour_, Takes takes_ = Takes::tones, double base_ = 0)
        : tones(tones_), colour(colour_), takes(takes_), base(base_) {}
    std::uint32_t tones = 0;
    std::uint32_t colour = 0;
    Takes takes = Takes::tones;
    double base = 0;
};

// The texture formats this rewrites, as a BNTX names them (the high byte of
// the format: 0x1a BC1, 0x1c BC3, 0x2d to 0x3a ASTC of any block size;
// hair-link: 0x20 BC7, Link's hair Mt_Hairs_Player_Adult_Alb).
enum class Format : std::uint8_t { bc1, bc3, astc, bc7 };
std::optional<Format> FormatOf(std::uint32_t bntx_format);
std::size_t BlockBytes(Format f);

// Every end colour of the texels (whole blocks), for a part's average.
std::vector<Rgb> Ends(Format f, std::span<const std::uint8_t> texels);
// The mean linear luma of all of them (tools/puppet_pack/tunic.py
// average_luma: blocks that are all zero are left out); 0 when there is none.
double AverageLuma(Format f, std::span<const std::uint8_t> texels);

// The texels with each rule's part in its colour; the first rule whose tones
// match an end colour takes it. Returns how many blocks changed. `texels` is
// the texture's original (a part's average is taken from it), and `out` (as
// long) gets the result; they may be the same bytes.
std::size_t Recolour(Format f, std::span<const std::uint8_t> texels, std::span<std::uint8_t> out,
                     const std::vector<Rule>& rules);

// Row 185: a block's end colours are the encoder's, not the texture's. Beside
// the Hylian Shield frame's grey the encoder often put a pale green
// (165, 199, 148) or a dark brown (66, 60, 49) end that no texel shows as such;
// no rule took it by its tone, so the block was painted at one end only and
// its texels came out half painted: specks of the old grey on the frame. The
// pack lists the blocks wholly on a part (by the game's own metalness texture:
// tools/puppet_pack/look_metal.py regions, coop_look/region.bin); in such a
// block the part's rule also takes every end that is Forceable.
struct Region {
    std::span<const std::int8_t> rule;   // per block (texel bytes / BlockBytes): a rule's index, or -1
    std::uint32_t tones = 0;             // every part's tones in this texture (painted or not)
};
std::size_t Recolour(Format f, std::span<const std::uint8_t> texels, std::span<std::uint8_t> out,
                     const std::vector<Rule>& rules, const Region& region);
// An end a part's region may take: no tone of `tones` (no part's own colour), and
// grey enough (spread under 35% of its brightness) or too dark to say: not a
// coloured part such as the Triforce's gold (tools/puppet_pack/look_metal.py forceable).
bool Forceable(const Rgb& c, std::uint32_t tones);

// hair-link (report 293): a hair texture in any colour, the way Character Select's mod makes its hair palette
// (snail-cs-src, MIT, Copyright (c) 2026 snaildev99, HEAD 785d8d0d: mods/customize.py hair_fn, tools/texrecolor.py
// bc7_blocks; THIRD_PARTY_NOTICES.md). Every end colour of every block: its hue goes to the colour's and keeps half of
// its own difference from the texture's mean hue (the strands), its saturation is the colour's scaled by the square
// root of its own against the mean's, and its brightness is raised to the power that takes the mean brightness to the
// colour's (0 and 1 stay). The mean is the texture's end colours' (the mod takes the decoded texels'). BC7 only (the
// game's hair is BC7): the end points of all eight modes are rewritten in place (p-bits kept, a rotation's alpha
// field holds that channel), the weights, partitions and alpha stay. Another format, or a block of no mode (all zero:
// padding), is copied as it is. Returns how many blocks changed.
std::size_t HairRecolour(Format f, std::span<const std::uint8_t> texels, std::span<std::uint8_t> out,
                         std::uint32_t colour);
// The pieces, for the tests: a BC7 block's end colours (RGB, rotation undone; none for a block of no mode), and the
// colour HairRecolour makes of an end colour `c` for a texture whose mean is `mean`.
std::vector<Rgb> Bc7Ends(const std::array<std::uint8_t, 16>& block);
Rgb HairShift(const Rgb& c, const Rgb& mean, std::uint32_t colour);

// --- the pieces, for the tests ---

// sRGB colour scaled to `target`'s colour at its linear brightness against `base`.
Rgb Shade(const Rgb& c, const Rgb& target, double base);
// Linear luma of an sRGB colour.
double Luma(const Rgb& c);

// An ASTC block's end colours (RGBA), two per partition; empty when the block
// is not one this reads (HDR, a bad mode).
std::vector<std::array<std::uint8_t, 4>> AstcEnds(const std::array<std::uint8_t, 16>& block);

}  // namespace CoopLookPaint
