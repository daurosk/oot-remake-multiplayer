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

// The flag a chest's own game sets when it is opened (issue 87). It is not
// the placement's 64-bit key: the TreasureBox AI (AI/TreasureBox.module in
// Pack/AI.Global) looks up its instance id in the GameData struct
// 0xce0c4dfe, whose member is Murmur3 of the placement hash written in
// decimal and whose value is the hash of a saved (ResetTypeValue 24) Bool:
// the chest's "opened". Every one of the struct's 206 members is a chest
// placement in Banc/*/*.bcett.byml (build e0f3ccfaf50065ae); the 12 chests
// of the Treasure Chest Shop (Market, Obj_HrlCity_Shop_TBox_*) are left out.
// Live check: the Graveyard's flower-grave chest (0xeab569abb9d4d59a) set
// exactly its Bool 0x39e4b7ec when opened, and no 64-bit key (docs/flags.md).
constexpr std::array kOpened = std::to_array<Opened>({
    {0x39c8aedb, 0x08c44828386468bf, "BottomOfTheWell", "Obj_TBox_A_01"},
    {0x0b81bd32, 0x4b5a948bcf608499, "BottomOfTheWell", "Obj_TBox_A_01"},
    {0xd9214b05, 0x5428ef06d6db4316, "BottomOfTheWell", "Obj_TBox_A_01"},
    {0x4ee0b514, 0x748a6f06e9487abb, "BottomOfTheWell", "Obj_TBox_A_01"},
    {0xf9aa7a38, 0x7c8fdee01d0ef972, "BottomOfTheWell", "Obj_TBox_A_01"},
    {0x0b5af5af, 0x9758695f1fdc4a3c, "BottomOfTheWell", "Obj_TBox_A_01"},
    {0xd1eb2a11, 0xb441c38683852b40, "BottomOfTheWell", "Obj_TBox_A_01"},
    {0x9ebb94ff, 0x83602ac1d21befb2, "BottomOfTheWell", "Obj_TBox_A_02"},
    {0x7c3dcc16, 0xcef0a7e181e1af55, "BottomOfTheWell", "Obj_TBox_A_02"},
    {0x2b12706d, 0xe721a50e0511d8be, "BottomOfTheWell", "Obj_TBox_A_02_NoBake"},
    {0x63c159d6, 0x0c2484b6cc20b4cd, "BottomOfTheWell", "Obj_TBox_A_06"},
    {0x7f04a28a, 0xa2a137bb90f701fa, "BottomOfTheWell", "Obj_TBox_GlassTruth_A_01"},
    {0x74eedb5e, 0x1014a2481de2b403, "DeathMountainCrater", "Obj_TBox_A_01"},
    {0x2243de4f, 0x7eb06d3218678cf3, "DeathMountainCrater", "Obj_TBox_A_01_NoBake"},
    {0x69d957a5, 0x88226065e35dab4d, "DeathMountainTrail", "Obj_TBox_A_01"},
    {0x50eb3d8e, 0x10761d44d9d4fa3e, "DesertColossus", "Obj_TBox_A_01_NoBake"},
    {0x0c3f127e, 0x228e7b7a60ba339b, "DesertColossus", "Obj_TBox_A_02"},
    {0x96828946, 0x8af25d13333bf6dc, "DesertColossus", "Obj_TBox_A_02_NoBake"},
    {0xc46a2f36, 0x77750b6072f44277, "DodongoCavern", "Obj_TBox_A_01"},
    {0xa9c7155e, 0xaa0e8f651deeafa5, "DodongoCavern", "Obj_TBox_A_01"},
    {0x88abf94f, 0x0bcd752563b0aa40, "DodongoCavern", "Obj_TBox_A_01_NoBake"},
    {0x7b876429, 0x6bbf538133fc2e17, "DodongoCavern", "Obj_TBox_A_01_NoBake"},
    {0x57a4bd6d, 0x730ff8dc1ed0a3c9, "DodongoCavern", "Obj_TBox_A_02"},
    {0xf0324c6c, 0xa8c7127dd81a8bb9, "DodongoCavern", "Obj_TBox_A_02"},
    {0x96200b9f, 0xf34e286424822e73, "DodongoCavern", "Obj_TBox_A_02"},
    {0x35c75a99, 0x686c7d83d07add16, "DodongoCavern", "Obj_TBox_A_06"},
    {0x23dfbccf, 0xa0bdd6b7a2265cb1, "DodongoCavern", "Obj_TBox_A_06"},
    {0xafbb5671, 0x07db33e50254c85b, "DreamWorld", "Obj_TBox_A_01"},
    {0x0a4f7bd1, 0x279c31ffc14aa617, "DreamWorld", "Obj_TBox_A_01"},
    {0xa10905e3, 0x30e2387fd5071354, "DreamWorld", "Obj_TBox_A_01"},
    {0x21827e47, 0x3edce5d6144f47fc, "DreamWorld", "Obj_TBox_A_01"},
    {0x42cd6f18, 0x646699aa534cf985, "DreamWorld", "Obj_TBox_A_01"},
    {0x709d94cc, 0x65f4e1cfc4d74f7e, "DreamWorld", "Obj_TBox_A_01"},
    {0xd0b42fbd, 0x67816ff715c8c366, "DreamWorld", "Obj_TBox_A_01"},
    {0xf93a9700, 0x69550c08f6b6e880, "DreamWorld", "Obj_TBox_A_01"},
    {0x306ce15c, 0x8a0c173f2136dc70, "DreamWorld", "Obj_TBox_A_01"},
    {0x2cfeffbf, 0xdc1572f90fe769f1, "DreamWorld", "Obj_TBox_A_01"},
    {0x4df1310a, 0x0bf7cf354d5ab5a2, "FireTemple", "Obj_TBox_A_01"},
    {0x34c864c6, 0x17a99b8b3e705af7, "FireTemple", "Obj_TBox_A_01"},
    {0x946efbe1, 0x36cf66ad4f0b9d14, "FireTemple", "Obj_TBox_A_01"},
    {0xfaa8c214, 0x4e328fd6ca19fe0e, "FireTemple", "Obj_TBox_A_01"},
    {0x586234b3, 0x85b620f58f55317a, "FireTemple", "Obj_TBox_A_01"},
    {0x4958d1f3, 0x9203677a1637eb0b, "FireTemple", "Obj_TBox_A_01"},
    {0x393759f0, 0xae686b6127c55811, "FireTemple", "Obj_TBox_A_01"},
    {0xba01b6d8, 0xb3a812edc57747ca, "FireTemple", "Obj_TBox_A_01"},
    {0xea529806, 0xd12ebfcc345aecd9, "FireTemple", "Obj_TBox_A_01"},
    {0xe335247d, 0xe8f2b9202396f6ec, "FireTemple", "Obj_TBox_A_01"},
    {0x38581536, 0xebf2bc14b3a12fd2, "FireTemple", "Obj_TBox_A_01"},
    {0xb21e7a4e, 0x7988dc941c3d29f8, "FireTemple", "Obj_TBox_A_01_NoBake"},
    {0xe5811bd4, 0x3330422225a7368f, "FireTemple", "Obj_TBox_A_02"},
    {0xca47c799, 0xa7b05ca3e496906e, "FireTemple", "Obj_TBox_A_02"},
    {0x907a497f, 0xe7f15bebc65f9252, "FireTemple", "Obj_TBox_A_02"},
    {0x12a3b141, 0xc31f81a17795f048, "FireTemple", "Obj_TBox_A_03"},
    {0x8b9a3d0f, 0x1041dd4d83575dda, "ForestTemple", "Obj_TBox_A_01"},
    {0x6f0d153f, 0x5982ecccbb376369, "ForestTemple", "Obj_TBox_A_01"},
    {0x548fd9d1, 0x7d6fea9d6a023899, "ForestTemple", "Obj_TBox_A_01"},
    {0x4c0aa7ea, 0xd1caf4a18c8695b7, "ForestTemple", "Obj_TBox_A_01"},
    {0x9b2e155f, 0x2fa5e3dfba115737, "ForestTemple", "Obj_TBox_A_01_NoBake"},
    {0x196af396, 0x68922f37569af70b, "ForestTemple", "Obj_TBox_A_01_NoBake"},
    {0x84ee2d58, 0xcd11547d5a93dbf6, "ForestTemple", "Obj_TBox_A_01_NoBake"},
    {0xa61ae4ac, 0xdcf29a6f249b7191, "ForestTemple", "Obj_TBox_A_01_NoBake"},
    {0x0cb94292, 0x2c0f7c71d105e427, "ForestTemple", "Obj_TBox_A_02"},
    {0x3e05a646, 0x606e1ee44bb4262c, "ForestTemple", "Obj_TBox_A_02_NoBake"},
    {0xfef0aa9c, 0x99cc40ac3c369908, "ForestTemple", "Obj_TBox_A_02_NoBake"},
    {0x893e0c3a, 0x49a3d494a379b083, "ForestTemple", "Obj_TBox_A_03_NoBake"},
    {0xe5aa28a1, 0xe6a415884d4825c9, "GerudoFortress", "Obj_TBox_A_01"},
    {0x689cebe8, 0xab4dd26b0113ac53, "GerudoFortress", "Obj_TBox_A_02"},
    {0xfde0eaa2, 0x02b982471f5248d3, "GerudoTrainingGround", "Obj_TBox_A_01"},
    {0xea6cde49, 0x1a22008b43943930, "GerudoTrainingGround", "Obj_TBox_A_01"},
    {0x8602c326, 0x340b800b9af46da8, "GerudoTrainingGround", "Obj_TBox_A_01"},
    {0x7d813872, 0x3bf55116610cd9f8, "GerudoTrainingGround", "Obj_TBox_A_01"},
    {0x81ebaa2b, 0x3cadc194d595b9a3, "GerudoTrainingGround", "Obj_TBox_A_01"},
    {0x2ca9afd9, 0x719b95b90a9d76fa, "GerudoTrainingGround", "Obj_TBox_A_01"},
    {0xf33d12b3, 0x898c124383c0ad0f, "GerudoTrainingGround", "Obj_TBox_A_01"},
    {0x905525cc, 0x94720782c821ae21, "GerudoTrainingGround", "Obj_TBox_A_01"},
    {0x66f10fdd, 0xa079b1b091faf40f, "GerudoTrainingGround", "Obj_TBox_A_01"},
    {0x423b9e4d, 0x21d0c63ae1970637, "GerudoTrainingGround", "Obj_TBox_A_01_NoBake"},
    {0x2b899495, 0x3397f157a95cb936, "GerudoTrainingGround", "Obj_TBox_A_01_NoBake"},
    {0xf5dc6f20, 0x3eb89d2265745041, "GerudoTrainingGround", "Obj_TBox_A_01_NoBake"},
    {0x760940b1, 0x415c4dae2d3990cc, "GerudoTrainingGround", "Obj_TBox_A_01_NoBake"},
    {0x49443f24, 0x5fb9df4f1ab34ff5, "GerudoTrainingGround", "Obj_TBox_A_01_NoBake"},
    {0x19f4decd, 0xa32161766e5a9960, "GerudoTrainingGround", "Obj_TBox_A_01_NoBake"},
    {0xdfa226ef, 0xbb5ea265515c11c0, "GerudoTrainingGround", "Obj_TBox_A_01_NoBake"},
    {0x984cd824, 0x1af01d8f10ce451a, "GerudoTrainingGround", "Obj_TBox_A_02"},
    {0xd3b7b20f, 0x95180dd9690395e4, "GerudoTrainingGround", "Obj_TBox_A_06"},
    {0x616b6e42, 0xa223e49f14d4546f, "GerudoTrainingGround", "Obj_TBox_A_06"},
    {0x8278e92a, 0x44db1a332e1181a1, "GerudoTrainingGround", "Obj_TBox_A_06_NoBake"},
    {0x2e46de13, 0x49f747e3fdb01997, "GerudoTrainingGround", "Obj_TBox_GlassTruth_A_01"},
    {0x876b69ba, 0x9b30befed364e110, "GerudoTrainingGround", "Obj_TBox_GlassTruth_A_01"},
    {0xfac869dd, 0x59b30a4094780bae, "GerudoValley", "Obj_TBox_A_01"},
    {0x15f33ba0, 0xbceaaaf2bc02291d, "GerudoValley", "Obj_TBox_A_01"},
    {0xa1f6b6ae, 0xf6571bb2b874c05d, "GerudoValley", "Obj_TBox_A_01_NoBake"},
    {0x7001f17c, 0xdc0cbd0ac9f0b18a, "GerudoValley", "Obj_TBox_A_02"},
    {0x6a8c1bdf, 0x561cbe4b374f3739, "GerudoValley", "Obj_TBox_GlassTruth_A_01"},
    {0x7386ec19, 0xff8a367dbd828c94, "GerudoValley", "Obj_TBox_GlassTruth_A_01"},
    {0xf4bde2f0, 0x10557564b63cb936, "GoronCity", "Obj_TBox_A_01"},
    {0x930b1a9c, 0x35b54149ba5a641a, "GoronCity", "Obj_TBox_A_01"},
    {0x773742b8, 0x3d1a64837516e782, "GoronCity", "Obj_TBox_A_01"},
    {0x3ebae67c, 0xcb461988f4162350, "GoronCity", "Obj_TBox_A_01"},
    {0x9ecb3abc, 0x7f1dd488b05f0bf6, "GoronCity", "Obj_TBox_A_01_NoBake"},
    {0x37f0e791, 0x97e2ec1944316626, "GoronCity", "Obj_TBox_A_01_NoBake"},
    {0xcd9da287, 0x033f5b9324bf56aa, "Graveyard", "Obj_TBox_A_01_NoBake"},
    {0x7fe6ac15, 0x3bfd359c80644eeb, "Graveyard", "Obj_TBox_A_01_NoBake"},
    {0x9c89c565, 0x8588aaeeac14eed7, "Graveyard", "Obj_TBox_A_02"},
    {0x8331e107, 0x4ccffbd5778b67e2, "Graveyard", "Obj_TBox_A_02_NoBake"},
    {0xbf84d098, 0xde9e62394333948e, "Graveyard", "Obj_TBox_A_02_NoBake"},
    {0x39e4b7ec, 0xeab569abb9d4d59a, "Graveyard", "Obj_TBox_A_06"},
    {0xd2b4f309, 0x7a7a8d591e365abe, "HountedWasteland", "Obj_TBox_A_01_NoBake"},
    {0x76e7db96, 0x10266fe15a73cb29, "HyruleField", "Obj_TBox_A_01"},
    {0xb1c841f9, 0x89444976a912e2fa, "HyruleField", "Obj_TBox_A_01"},
    {0xf549f813, 0xec3f782c547551a1, "HyruleField", "Obj_TBox_A_01"},
    {0x900d9849, 0xf21b7ae43e7ff515, "HyruleField", "Obj_TBox_A_01"},
    {0x78751372, 0x4fdf975f9a02007c, "HyruleField", "Obj_TBox_A_01_NoBake"},
    {0xca18a6ed, 0x5fc4b82a3d4ae3c4, "HyruleField", "Obj_TBox_A_01_NoBake"},
    {0xda9dff2c, 0x292e3ed6acacbe44, "HyruleField", "Obj_TBox_A_01_memo11"},
    {0xdbd41178, 0x3949ee033ed95498, "IceCavern", "Obj_TBox_A_01"},
    {0xb494a299, 0x9f93f42b07b595af, "IceCavern", "Obj_TBox_A_02"},
    {0x27a33a9a, 0xbbeee5ed57d3abd0, "IceCavern", "Obj_TBox_A_02"},
    {0xe2650329, 0x5f311732ca6be0f4, "IceCavern", "Obj_TBox_A_02_NoBake"},
    {0xc545cfd7, 0x0e4a45a510ad6148, "InsideGanonCastle", "Obj_TBox_A_01"},
    {0xcb885192, 0x678d9ffc5389d2ff, "InsideGanonCastle", "Obj_TBox_A_01"},
    {0x326dabc5, 0xc2efa1563584d93b, "InsideGanonCastle", "Obj_TBox_A_01"},
    {0xf140f290, 0xc7e7e5400add48ae, "InsideGanonCastle", "Obj_TBox_A_01"},
    {0x56426efc, 0xfda77bc1cd57461c, "InsideGanonCastle", "Obj_TBox_A_01"},
    {0x246ff3a0, 0x41b13f78b6c5b10c, "InsideGanonCastle", "Obj_TBox_A_01_NoBake"},
    {0x8526343c, 0x6d9f85b61029b1cb, "InsideGanonCastle", "Obj_TBox_A_01_NoBake"},
    {0x965d2a40, 0xcfa53d0c7049a789, "InsideGanonCastle", "Obj_TBox_A_01_NoBake"},
    {0x16c793eb, 0xeab8ef3e0eb98f97, "InsideGanonCastle", "Obj_TBox_A_01_NoBake"},
    {0x13699922, 0xf50c7ad82f346cc6, "InsideGanonCastle", "Obj_TBox_A_02_NoBake"},
    {0xd5b8bc12, 0xed345a357fd30226, "InsideGanonCastle", "Obj_TBox_A_03_NoBake"},
    {0xf66a9af3, 0xd49f0a2e9f8f56af, "InsideGanonCastle", "Obj_TBox_GlassTruth_A_01"},
    {0xdf36e422, 0x32169a7e099404c1, "InsideJabuJabuBelly", "Obj_TBox_A_01"},
    {0x4650ce74, 0xcf3fb5f292997772, "InsideJabuJabuBelly", "Obj_TBox_A_02"},
    {0x2ae47917, 0x30b7dbf43f3e5ea0, "InsideJabuJabuBelly", "Obj_TBox_A_02_NoBake"},
    {0xe2a0a25d, 0x4cb5467d30e9d84a, "InsideJabuJabuBelly", "Obj_TBox_A_02_NoBake"},
    {0xf90b34b9, 0x9dc359c40cd5dfe7, "InsideTheDekuTree", "Obj_TBox_A_01"},
    {0x2412fffb, 0xb2df4cb474484f29, "InsideTheDekuTree", "Obj_TBox_A_01"},
    {0x4d191dc2, 0xb4ca60d52ca4d05f, "InsideTheDekuTree", "Obj_TBox_A_01"},
    {0x40f83ad5, 0x0b66198d17d6584e, "InsideTheDekuTree", "Obj_TBox_A_02"},
    {0xe4fb555d, 0x771092ae2f3b7100, "InsideTheDekuTree", "Obj_TBox_A_02"},
    {0x31a0367c, 0x80d8145de418987b, "InsideTheDekuTree", "Obj_TBox_A_02"},
    {0x8dbe7cf1, 0xa58e4896b72a2f2a, "KakarikoVillage", "Obj_TBox_A_01"},
    {0x6ce4e57a, 0xb30e25a1c5e03c1e, "KakarikoVillage", "Obj_TBox_A_01"},
    {0x1b07478f, 0xb588e3f93990af69, "KakarikoVillage", "Obj_TBox_A_01"},
    {0xe51565c5, 0x3c5bb811b4cf54dd, "KokiriForest", "Obj_TBox_A_01"},
    {0xc8643846, 0x3f38f9b9a399e19a, "KokiriForest", "Obj_TBox_A_01"},
    {0xf870d00b, 0x86f700bfe72c18fc, "KokiriForest", "Obj_TBox_A_01"},
    {0x316d8a20, 0x9652edaf3158c9b6, "KokiriForest", "Obj_TBox_A_01"},
    {0x467020fd, 0xb171aee0229fa97e, "KokiriForest", "Obj_TBox_A_01"},
    {0xb5eb0e2d, 0xcda7447b91ab4dba, "KokiriForest", "Obj_TBox_A_01_NoBake"},
    {0x15c74de1, 0xe89059b57059473a, "KokiriForest", "Obj_TBox_A_02"},
    {0x6aa4ada3, 0xf2948997ede7b25f, "LostWood", "Obj_TBox_A_01"},
    {0x9367c55c, 0x187c597584cfdb4a, "SacredForestMeadow", "Obj_TBox_A_01"},
    {0x3af5e0e3, 0x6ef4c6ed9df5f1d9, "SacredForestMeadow", "Obj_TBox_A_01_NoBake"},
    {0x93712b23, 0x0f6d9d21b4856e49, "ShadowTemple", "Obj_TBox_A_01"},
    {0x62789454, 0x503da0706624f6da, "ShadowTemple", "Obj_TBox_A_01"},
    {0x9ef7b8ca, 0x7aeee5ed95a7b2f8, "ShadowTemple", "Obj_TBox_A_01"},
    {0x612320f9, 0x88bb8d8c33b9dc83, "ShadowTemple", "Obj_TBox_A_01"},
    {0x83ba7974, 0x8f8e98673c407743, "ShadowTemple", "Obj_TBox_A_01"},
    {0xb7b0c604, 0x4ed7596563996ea6, "ShadowTemple", "Obj_TBox_A_01_NoBake"},
    {0x59d7231e, 0xf872a2d9f751b318, "ShadowTemple", "Obj_TBox_A_01_NoBake"},
    {0x5d93713b, 0x180e2cce4e8d1b35, "ShadowTemple", "Obj_TBox_A_02_NoBake"},
    {0x066a78b4, 0xbbc05231a9cd40c2, "ShadowTemple", "Obj_TBox_A_02_NoBake"},
    {0xf703ab6f, 0xc9b6fa7844300a71, "ShadowTemple", "Obj_TBox_A_02_NoBake"},
    {0x4a9a8ad6, 0x7ff8df2fa8e85da9, "ShadowTemple", "Obj_TBox_A_03"},
    {0xd38dfe04, 0x0258b24f2ac0a365, "ShadowTemple", "Obj_TBox_GlassTruth_A_01"},
    {0x7f715807, 0x339bcc54345541e7, "ShadowTemple", "Obj_TBox_GlassTruth_A_01"},
    {0x1b7261ef, 0xab54e7b25c589a22, "ShadowTemple", "Obj_TBox_GlassTruth_A_01"},
    {0xcdc08c75, 0x3b5e6ea69d7d2458, "SpiritTemple", "Obj_TBox_A_01"},
    {0x4e36e8cb, 0xa71e7700463b4bb5, "SpiritTemple", "Obj_TBox_A_01"},
    {0x7eca2517, 0x14d5a5e50bd51ac7, "SpiritTemple", "Obj_TBox_A_01_NoBake"},
    {0xc6e1ed2d, 0x1e937e4a772e9fb3, "SpiritTemple", "Obj_TBox_A_01_NoBake"},
    {0xd683aa25, 0x4f26a195e1a40726, "SpiritTemple", "Obj_TBox_A_01_NoBake"},
    {0xef6a19f7, 0xb329070612b6f1b9, "SpiritTemple", "Obj_TBox_A_01_NoBake"},
    {0xad1f9d9b, 0xbfe8a21e1a060a3a, "SpiritTemple", "Obj_TBox_A_01_NoBake"},
    {0x66741a95, 0xf19252eeb3665718, "SpiritTemple", "Obj_TBox_A_01_NoBake"},
    {0xe4d694f3, 0xc03d32a754e00f7b, "SpiritTemple", "Obj_TBox_A_02_NoBake"},
    {0x3b0228d0, 0xd8f55e36a867fa76, "SpiritTemple", "Obj_TBox_A_02_NoBake"},
    {0x4f583ba3, 0xe1d25a9ccd2acdfe, "SpiritTemple", "Obj_TBox_A_03"},
    {0x04fe28f1, 0x49adff86edcadf65, "SpiritTemple", "Obj_TBox_A_06"},
    {0x04b87f4a, 0x7ee3170a9e18d404, "SpiritTemple", "Obj_TBox_GlassTruth_A_01"},
    {0xcad2f5f6, 0xc2d8d982a99411ce, "SpiritTemple", "Obj_TBox_GlassTruth_A_01"},
    {0x7f4783e6, 0x22bf8064463f9175, "WaterTemple", "Obj_TBox_A_01"},
    {0x7ea60e52, 0x33437a1475511b03, "WaterTemple", "Obj_TBox_A_01"},
    {0x62b2da2c, 0xa22243c7d156dcce, "WaterTemple", "Obj_TBox_A_01"},
    {0xb6b6070b, 0xe4d909e591e99f8d, "WaterTemple", "Obj_TBox_A_01"},
    {0x8914a506, 0xfb2ca21db0a9d839, "WaterTemple", "Obj_TBox_A_01_NoBake"},
    {0x8169322d, 0xe362f2ae2e789aea, "WaterTemple", "Obj_TBox_A_02"},
    {0xcd8a5078, 0xe3eeb72cf74cac2c, "WaterTemple", "Obj_TBox_A_02"},
    {0x241997f9, 0xfaa0fc3748e1cbfd, "WaterTemple", "Obj_TBox_A_02"},
    {0x88b6cf77, 0x900901699af3b852, "WaterTemple", "Obj_TBox_A_03"},
    {0xa1678ed9, 0x2b702a67d25f1421, "ZoraDomain", "Obj_TBox_A_02_NoBake"},
    {0x3a2a1559, 0xd0ee852c5e8a8bea, "ZoraFountain", "Obj_TBox_A_01"},
    {0x6f484a36, 0x7aa865b1d1bc6070, "ZoraRiver", "Obj_TBox_A_01"},
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

std::span<const Opened> OpenedFlags() {
    return kOpened;
}

std::optional<std::uint64_t> KeyOfOpened(std::uint32_t flag) {
    for (const Opened& o : kOpened) {
        if (o.flag == flag) {
            return o.key;
        }
    }
    return std::nullopt;
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
    for (const Opened& o : kOpened) {
        keys.push_back(coop::host::FlagKey(o.flag, std::uint16_t{0}));
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
    LOG_INFO(Core, "coop: chests: sharing {} chest keys and opened flags{}", added,
             on ? "" : " (off: LICHII_COOP_CHESTS=0)");
#else
    (void)added;
#endif
}

} // namespace CoopChests
