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
// colour the player picks while the game runs. ASTC blocks keep their
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
};
using Rgb = std::array<std::uint8_t, 3>;
std::uint32_t ToneOf(const Rgb& c);

// One part: its end colours (any of `tones`) become `colour` (0xRRGGBB), each
// as bright against the part's average as it was, so the shading stays and
// the part's average becomes the colour.
struct Rule {
    std::uint32_t tones = 0;
    std::uint32_t colour = 0;
};

// The texture formats this rewrites, as a BNTX names them (the high byte of
// the format: 0x1a BC1, 0x1c BC3, 0x2d to 0x3a ASTC of any block size).
enum class Format : std::uint8_t { bc1, bc3, astc };
std::optional<Format> FormatOf(std::uint32_t bntx_format);
std::size_t BlockBytes(Format f);

// Every end colour of the texels (whole blocks), for a part's average.
std::vector<Rgb> Ends(Format f, std::span<const std::uint8_t> texels);

// The texels with each rule's part in its colour; the first rule whose tones
// match an end colour takes it. Returns how many blocks changed. `texels` is
// the texture's original (a part's average is taken from it), and `out` (as
// long) gets the result; they may be the same bytes.
std::size_t Recolour(Format f, std::span<const std::uint8_t> texels, std::span<std::uint8_t> out,
                     const std::vector<Rule>& rules);

// --- the pieces, for the tests ---

// sRGB colour scaled to `target`'s colour at its linear brightness against `base`.
Rgb Shade(const Rgb& c, const Rgb& target, double base);
// Linear luma of an sRGB colour.
double Luma(const Rgb& c);

// An ASTC block's end colours (RGBA), two per partition; empty when the block
// is not one this reads (HDR, a bad mode).
std::vector<std::array<std::uint8_t, 4>> AstcEnds(const std::array<std::uint8_t, 16>& block);

}  // namespace CoopLookPaint
