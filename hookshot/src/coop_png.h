// SPDX-License-Identifier: GPL-3.0-or-later
// The HUD pack's heart pictures (tools/puppet_pack/hud.py: heart-0..4.png) as
// RGBA pixels, for the game look of the overlay on Hookshot (coop_draw.h).
// The emulator reads them with Qt; the module has no image library, so this
// is a small PNG reader of its own: 8-bit grey, grey + alpha, RGB, RGBA and
// palette pictures, not interlaced, with zlib's inflate. Anything else (16-bit
// channels, interlaced, a broken file) is refused: the overlay then keeps its
// plain look.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace CoopPng {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4, rows top to bottom, alpha not premultiplied
};

// zlib data (RFC 1950 around RFC 1951) inflated; nothing when it is broken or
// would be bigger than `limit` bytes.
std::optional<std::vector<std::uint8_t>> Inflate(const std::uint8_t* data, std::size_t size,
                                                 std::size_t limit);

// A PNG file's bytes as RGBA; nothing when it is not one this reads.
std::optional<Image> Decode(const std::uint8_t* data, std::size_t size);

// The same, read from a file.
std::optional<Image> Load(const std::filesystem::path& path);

}  // namespace CoopPng
