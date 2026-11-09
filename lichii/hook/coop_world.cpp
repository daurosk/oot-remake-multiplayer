// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_world.h"

#include <array>
#include <cstdio>

namespace CoopWorld {

namespace {

// Every Obj_ pack of build e0f3ccfaf50065ae that has LifeRef and
// DestructionRef (read from the romfs, 2026-11-09), by name part. Left out:
// the chest Obj_TBox_A_04 (opening a chest is a flag, flags.md), shop
// targets and moving obstacles (minigames), icicles and hammer pillars
// (they fall or tilt rather than break), Obj_Well_CandleStick_A_03.
constexpr std::array<std::string_view, 6> kPrefixes{
    "Obj_Pot_",          // pots, the respawning and dream ones too
    "Obj_BrokenBox_",    // crates
    "Obj_LiftRock_",     // rocks Link lifts and throws
    "Obj_BrokenRock_",   // rocks a bomb or hammer breaks
    "Obj_Plant_Grass_",  // grass
    "Obj_GuidePost_",    // signposts
};
constexpr std::array<std::string_view, 10> kParts{
    "SignBoard_",  "Signboard_",      // cuttable signs, every region's
    "CrackWall_",  "CrackFloor_",     // bombable walls and floors
    "BrokenRock_",                    // Goron and Death Mountain rocks
    "BrokenIcePillar_", "SkullPot_", "ArcheryPot_", "Beehive_", "SpiderWeb_",
};

// The grass that is an actor without an Obj_Plant_Grass_ name: every pack
// with the life of Obj_Plant_DestructibleGrass_Base (a pot's: 1, then gone).
// It has no DestructionRef, so the list above missed it; the 13 tufts by
// the Kokiri side of Hyrule Field are the third. The rest of the field's
// grass is foliage, no actor at all, and is not shared (world.md).
constexpr std::array<std::string_view, 4> kGrass{
    "Obj_Plant_DestructibleGrass_Base",
    "Land_Kokiri_Plant_G_01",
    "Land_kokiri_Plant_A_02_DropFixed_03",
    "Land_Kokiri_Zenmai_A_02",
};

} // namespace

bool IsBreakable(std::string_view name) {
    for (const auto grass : kGrass) {
        if (name == grass) {
            return true;
        }
    }
    if (!name.starts_with("Obj_")) {
        return false;
    }
    for (const auto prefix : kPrefixes) {
        if (name.starts_with(prefix)) {
            return true;
        }
    }
    for (const auto part : kParts) {
        if (name.find(part) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

namespace {

constexpr std::uint64_t kFoliageModule = 0x4e22540;  // main + this: game::foliage::Module*
constexpr int kBlockLines = 60;                      // per block
constexpr int kAllLines = 1500;                      // per session

bool LooksLikePointer(std::uint64_t v) {
    return (v >> 32) != 0 && v < (1ull << 40) && v % 4 == 0;
}

} // namespace

void FoliageProbe::Compare(const ReadWord& read, const std::string& key, std::uint64_t address,
                           std::size_t count, const std::string& where,
                           std::vector<std::string>& out) {
    std::vector<std::uint64_t> now(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto w = read(address + 8 * i);
        if (!w) {
            return;  // not readable: not a buffer
        }
        now[i] = *w;
    }
    Block& b = blocks_[key];
    if (b.lines >= kBlockLines || lines_ >= kAllLines) {
        b.address = address;
        b.words = std::move(now);
        return;
    }
    char text[64];
    std::string line;
    if (b.address != address || b.words.size() != count) {
        std::snprintf(text, sizeof text, " at %#llx:", static_cast<unsigned long long>(address));
        line = "foliage: " + key + text;
        for (const std::uint64_t w : now) {
            std::snprintf(text, sizeof text, " %llx", static_cast<unsigned long long>(w));
            line += text;
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            if (now[i] != b.words[i]) {
                std::snprintf(text, sizeof text, " +%#zx:%llx>%llx", 8 * i,
                              static_cast<unsigned long long>(b.words[i]),
                              static_cast<unsigned long long>(now[i]));
                line += text;
            }
        }
        if (line.empty()) {
            return;
        }
        line = "foliage: " + key + " changed" + line;
    }
    b.address = address;
    b.words = std::move(now);
    ++b.lines;
    ++lines_;
    out.push_back(line + where);
}

std::vector<std::string> FoliageProbe::Look(const ReadWord& read, std::uint64_t main,
                                            const std::optional<std::array<float, 3>>& link) {
    std::vector<std::string> out;
    if (++calls_ % 4 != 0 || main == 0) {
        return out;
    }
    const auto module = read(main + kFoliageModule);
    const auto holder = module && *module ? read(*module + 0xf0) : std::nullopt;
    const auto scene = holder && *holder ? read(*holder + 0x338) : std::nullopt;
    const std::uint64_t s = scene.value_or(0);
    char text[160];
    if (s != scene_) {
        scene_ = s;
        blocks_.clear();
        std::snprintf(text, sizeof text, "foliage: module %#llx flags %#llx holder %#llx scene %#llx",
                      static_cast<unsigned long long>(module.value_or(0)),
                      static_cast<unsigned long long>(
                          module && *module ? read(*module + 0x30).value_or(0) & 0xff : 0),
                      static_cast<unsigned long long>(holder.value_or(0)),
                      static_cast<unsigned long long>(s));
        out.emplace_back(text);
    }
    if (s == 0) {
        return out;
    }
    std::string where;
    if (link) {
        std::snprintf(text, sizeof text, " | Link %.2f %.2f %.2f", (*link)[0], (*link)[1],
                      (*link)[2]);
        where = text;
    }
    // The scene around the field: SsboGrassCut is +0x58, SsboGather +0x88.
    Compare(read, "scene+0x40", s + 0x40, 16, where, out);
    for (std::uint64_t field = 0x58; field < 0x88; field += 8) {
        const auto p = read(s + field);
        if (!p || !LooksLikePointer(*p)) {
            continue;
        }
        std::snprintf(text, sizeof text, "cut+%#llx", static_cast<unsigned long long>(field));
        const std::string key = text;
        Compare(read, key, *p, 64, where, out);
        for (std::uint64_t inner = 0; inner < 0x40; inner += 8) {
            const auto q = read(*p + inner);
            if (!q || !LooksLikePointer(*q) || *q == *p) {
                continue;
            }
            std::snprintf(text, sizeof text, "->+%#llx", static_cast<unsigned long long>(inner));
            Compare(read, key + text, *q, 64, where, out);
        }
    }
    return out;
}

} // namespace CoopWorld
