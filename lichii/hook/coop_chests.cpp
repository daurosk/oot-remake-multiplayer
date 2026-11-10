// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_chests.h"

#include <array>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

#ifdef LICHII_COOP
#include "common/logging.h"
#endif

namespace CoopChests {
namespace {

// Every chest placement outside the dungeons (Banc/<Area>/*.bcett.byml of
// build e0f3ccfaf50065ae, Gyaml containing "TBox") whose Hash is a
// Bool64bitKey of GameDataList with ResetTypeValue 24, the saved type. The
// dungeons' 140 chests are in coop_dungeon.cpp. Left out: the 12 chests of
// the Treasure Chest Shop minigame (Market, Obj_HrlCity_Shop_TBox_*), which
// each player plays for themselves.
constexpr std::array kKeys = std::to_array<Key>({
    {0x1014a2481de2b403, "DeathMountainCrater", "Obj_TBox_A_01"},
    {0x7eb06d3218678cf3, "DeathMountainCrater", "Obj_TBox_A_01_NoBake"},
    {0x88226065e35dab4d, "DeathMountainTrail", "Obj_TBox_A_01"},
    {0x10761d44d9d4fa3e, "DesertColossus", "Obj_TBox_A_01_NoBake"},
    {0x228e7b7a60ba339b, "DesertColossus", "Obj_TBox_A_02"},
    {0x8af25d13333bf6dc, "DesertColossus", "Obj_TBox_A_02_NoBake"},
    {0x07db33e50254c85b, "DreamWorld", "Obj_TBox_A_01"},
    {0x279c31ffc14aa617, "DreamWorld", "Obj_TBox_A_01"},
    {0x30e2387fd5071354, "DreamWorld", "Obj_TBox_A_01"},
    {0x3edce5d6144f47fc, "DreamWorld", "Obj_TBox_A_01"},
    {0x646699aa534cf985, "DreamWorld", "Obj_TBox_A_01"},
    {0x65f4e1cfc4d74f7e, "DreamWorld", "Obj_TBox_A_01"},
    {0x67816ff715c8c366, "DreamWorld", "Obj_TBox_A_01"},
    {0x69550c08f6b6e880, "DreamWorld", "Obj_TBox_A_01"},
    {0x8a0c173f2136dc70, "DreamWorld", "Obj_TBox_A_01"},
    {0xdc1572f90fe769f1, "DreamWorld", "Obj_TBox_A_01"},
    {0xe6a415884d4825c9, "GerudoFortress", "Obj_TBox_A_01"},
    {0xab4dd26b0113ac53, "GerudoFortress", "Obj_TBox_A_02"},
    {0x59b30a4094780bae, "GerudoValley", "Obj_TBox_A_01"},
    {0xbceaaaf2bc02291d, "GerudoValley", "Obj_TBox_A_01"},
    {0xf6571bb2b874c05d, "GerudoValley", "Obj_TBox_A_01_NoBake"},
    {0xdc0cbd0ac9f0b18a, "GerudoValley", "Obj_TBox_A_02"},
    {0x561cbe4b374f3739, "GerudoValley", "Obj_TBox_GlassTruth_A_01"},
    {0xff8a367dbd828c94, "GerudoValley", "Obj_TBox_GlassTruth_A_01"},
    {0x10557564b63cb936, "GoronCity", "Obj_TBox_A_01"},
    {0x35b54149ba5a641a, "GoronCity", "Obj_TBox_A_01"},
    {0x3d1a64837516e782, "GoronCity", "Obj_TBox_A_01"},
    {0xcb461988f4162350, "GoronCity", "Obj_TBox_A_01"},
    {0x7f1dd488b05f0bf6, "GoronCity", "Obj_TBox_A_01_NoBake"},
    {0x97e2ec1944316626, "GoronCity", "Obj_TBox_A_01_NoBake"},
    {0x033f5b9324bf56aa, "Graveyard", "Obj_TBox_A_01_NoBake"},
    {0x3bfd359c80644eeb, "Graveyard", "Obj_TBox_A_01_NoBake"},
    {0x8588aaeeac14eed7, "Graveyard", "Obj_TBox_A_02"},
    {0x4ccffbd5778b67e2, "Graveyard", "Obj_TBox_A_02_NoBake"},
    {0xde9e62394333948e, "Graveyard", "Obj_TBox_A_02_NoBake"},
    {0xeab569abb9d4d59a, "Graveyard", "Obj_TBox_A_06"},
    {0x7a7a8d591e365abe, "HountedWasteland", "Obj_TBox_A_01_NoBake"},
    {0x10266fe15a73cb29, "HyruleField", "Obj_TBox_A_01"},
    {0x89444976a912e2fa, "HyruleField", "Obj_TBox_A_01"},
    {0xec3f782c547551a1, "HyruleField", "Obj_TBox_A_01"},
    {0xf21b7ae43e7ff515, "HyruleField", "Obj_TBox_A_01"},
    {0x4fdf975f9a02007c, "HyruleField", "Obj_TBox_A_01_NoBake"},
    {0x5fc4b82a3d4ae3c4, "HyruleField", "Obj_TBox_A_01_NoBake"},
    {0x292e3ed6acacbe44, "HyruleField", "Obj_TBox_A_01_memo11"},
    {0xa58e4896b72a2f2a, "KakarikoVillage", "Obj_TBox_A_01"},
    {0xb30e25a1c5e03c1e, "KakarikoVillage", "Obj_TBox_A_01"},
    {0xb588e3f93990af69, "KakarikoVillage", "Obj_TBox_A_01"},
    {0x3c5bb811b4cf54dd, "KokiriForest", "Obj_TBox_A_01"},
    {0x3f38f9b9a399e19a, "KokiriForest", "Obj_TBox_A_01"},
    {0x86f700bfe72c18fc, "KokiriForest", "Obj_TBox_A_01"},
    {0x9652edaf3158c9b6, "KokiriForest", "Obj_TBox_A_01"},
    {0xb171aee0229fa97e, "KokiriForest", "Obj_TBox_A_01"},
    {0xcda7447b91ab4dba, "KokiriForest", "Obj_TBox_A_01_NoBake"},
    {0xe89059b57059473a, "KokiriForest", "Obj_TBox_A_02"},
    {0xf2948997ede7b25f, "LostWood", "Obj_TBox_A_01"},
    {0x187c597584cfdb4a, "SacredForestMeadow", "Obj_TBox_A_01"},
    {0x6ef4c6ed9df5f1d9, "SacredForestMeadow", "Obj_TBox_A_01_NoBake"},
    {0x2b702a67d25f1421, "ZoraDomain", "Obj_TBox_A_02_NoBake"},
    {0xd0ee852c5e8a8bea, "ZoraFountain", "Obj_TBox_A_01"},
    {0x7aa865b1d1bc6070, "ZoraRiver", "Obj_TBox_A_01"},
});

bool EnvOff(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return false;
    }
    const bool off = std::string_view(value) == "0";
    std::free(value);
    return off;
#else
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "0";
#endif
}

} // namespace

std::span<const Key> Keys() {
    return kKeys;
}

std::size_t Merge(std::optional<coop::host::SharedFlags>& shared, bool on) {
    if (!on) {
        return 0;
    }
    std::vector<coop::host::FlagKey> keys;
    if (shared) {
        keys = shared->keys();
    }
    const std::size_t before = shared ? shared->size() : 0;
    for (const Key& k : kKeys) {
        keys.push_back(coop::host::FlagKey::key64_flag(k.key));
    }
    shared = coop::host::SharedFlags(std::move(keys));  // sorts, drops repeats
    return shared->size() - before;
}

bool On() {
    return !EnvOff("LICHII_COOP_CHESTS");
}

void AddShared(std::optional<coop::host::SharedFlags>& shared) {
    const bool on = On();
    const std::size_t added = Merge(shared, on);
#ifdef LICHII_COOP
    LOG_INFO(Core, "coop: chests: sharing {} chests outside the dungeons{}", added,
             on ? "" : " (off: LICHII_COOP_CHESTS=0)");
#else
    (void)added;
#endif
}

} // namespace CoopChests
