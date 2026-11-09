// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_world.h"

#include <array>

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

} // namespace CoopWorld
