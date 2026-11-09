// SPDX-License-Identifier: GPL-3.0-or-later
//
// World objects shared like enemies (docs/world.md): the breakables. Pots,
// crates, liftable and breakable rocks, grass, signs, cracked walls and the
// rest have the same Life and Death components as enemies (their packs'
// LifeRef, DeathRef and DestructionRef), so the enemy sharing in
// coop_objects_sync.cpp carries them: one owner, a break on a copy goes to
// the owner as DAMAGE, and the owner's life 0 reaches every copy, where the
// game's own death breaks it. No emulator code.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopWorld {

// True for the breakable world objects (by actor name).
bool IsBreakable(std::string_view name);

// Research, not sync (docs/world.md, "Field grass"): the grass that is not an
// actor is drawn by game::foliage, and its shader reads a buffer named
// SsboGrassCut. Look() follows main+0x4e22540 -> +0xf0 -> +0x338 (the foliage
// scene, as the game's own 0x2c013c0 does) and returns a log line for every
// word that changed in the scene header and in what its SsboGrassCut field
// (scene+0x58, 0x30 bytes) points to, two pointers deep, with where our Link
// stood. Called under LICHII_COOP_DEBUG_OBJECTS only.
class FoliageProbe {
public:
    // Reads 8 bytes of the game's memory; nothing when the address is bad.
    using ReadWord = std::function<std::optional<std::uint64_t>(std::uint64_t)>;

    std::vector<std::string> Look(const ReadWord& read, std::uint64_t main,
                                  const std::optional<std::array<float, 3>>& link);

private:
    struct Block {
        std::uint64_t address = 0;
        std::vector<std::uint64_t> words;
        std::vector<int> changes;  // per word; a word that changes all the time is left out
        int lines = 0;
    };
    void Compare(const ReadWord& read, const std::string& key, std::uint64_t address,
                 std::size_t count, const std::string& where, std::vector<std::string>& out);

    std::uint64_t scene_ = 0;
    std::map<std::string, Block> blocks_;
    int lines_ = 0;
    int calls_ = 0;
};

} // namespace CoopWorld
