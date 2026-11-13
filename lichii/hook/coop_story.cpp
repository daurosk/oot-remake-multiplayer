// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_story.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

#ifdef LICHII_COOP
#include "common/logging.h"
#endif

namespace CoopStory {
namespace {

// Read from the game's romfs, every name checked against
// GameData/GameDataList (Bool table, ResetTypeValue 8, the saved type).
// Hash = Murmur3 x86 32, seed 0, of the name. docs/flags.md, "Story and
// cutscenes", has the rules; in short:
//
// scene: `HavePlayedEvent.Dm_<area>_*` for the overworld's story scenes
//   (Kokiri, Deku_0000, HrlField, HrlCastle, LostWood, SacredForest, LonLon,
//   Kakariko, Graveyard, DeathMtTrail, DeathMtCrater, Goron, Zora, ZoraRiver,
//   ZoraFountain, LakeHrl, Gerudo, GerudoValley, DesertColossus, MelodyGet,
//   FairyFountain), and the story events of the stones' quests, the "go to"
//   prompts and the Door of Time scenes in Hyrule Field.
//   Left out: the opening of a new game (Dm_Kokiri_0000 to _0030: a new save
//   must still wake up; leaving its house, 0045/0050, is shared), time travel
//   (Dm_TimeTemple_*: a child who has not pulled the sword must still be able
//   to), the Chamber of Sages (Dm_SageRoom_*, SageRoom_*, the sages'
//   awakenings Dm_Awake_*: the first sword pull and the scenes after an adult
//   temple's blue warp; the Chamber has no walking exit, the scene moves Link
//   out; the medallions are shared items), system scenes (Dm_SY_*, DmF_*: item gets, Game Over), the title,
//   credits and ending, Ganon's Castle and the final battle, every scene in a
//   dungeon (Dm_<dungeon>_*, Demo_*; a dungeon's own state is
//   coop_dungeon.cpp's), bosses (their beaten state stays each game's own,
//   docs/sync-coverage.md D9), and Navi, talks, hints, minigames, arrivals.
// owl: `HaveTalked_Special.Npc_Kaepora_*`.
// world: named flags that an overworld placement's `Presence` tests (Banc),
//   one by one: the Kokiri who blocks the forest exit and the invisible wall
//   with it (KokiriForest_Npc_Kokiri_007_Child_Blocker_00), Mido's blockers,
//   Jabu-Jabu's mouth, Kakariko's ladder and well water, Lake Hylia's ice,
//   the royal tomb, the Death Mountain gate guard, Talon and Malon, the
//   Gerudo carpenters, the light prelude, the owl at Lake Hylia.
//   Left out: minigames, the scarecrow, Gossip Stone fairies, where one
//   player is (IsEntered, ArriveFrom), weather and age (Sequence_IsChildhood).
// talk: a talk Alex's play-test asked to be shared (2026-11-12): the Kokiri
//   Shop keeper's first talk and her Deku Shield talk. Other talks stay each
//   player's own.
constexpr std::array kFlags = std::to_array<Flag>({
    {0x007d38a2u, Kind::scene, "HavePlayedEvent.Dm_LostWood_0010"},
    {0x00a58feeu, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Fire"},
    {0x01a718c3u, Kind::scene, "HavePlayedEvent.Dm_LonLon_0010"},
    {0x069c4cc7u, Kind::scene, "HavePlayedEvent.Dm_Kakariko_0030"},
    {0x079b10a1u, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Sun_01"},
    {0x0895a9ffu, Kind::world, "Market_Adult_GetLightPrelude"},
    {0x0b450ae4u, Kind::scene, "HavePlayedEvent.Dm_Goron_0010"},
    {0x0bc9161fu, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0080"},
    {0x0bf7ce9du, Kind::world, "KakarikoVillage_LeaveKakariko_Npc_Hylia_Talon_Adult"},
    {0x0fba19e9u, Kind::scene, "HavePlayedEvent.Dm_HrlField_0072"},
    {0x132f347cu, Kind::scene, "HavePlayedEvent.Dm_ZoraFountain_0030"},
    {0x1330a253u, Kind::scene, "HavePlayedEvent.Dm_FairyFountain_Courage"},
    {0x13e870d1u, Kind::scene, "HavePlayedEvent.GoTo_DeathMountainCrater"},
    {0x13f3c149u, Kind::scene, "HavePlayedEvent.Dm_HrlField_0073"},
    // Issue 193: the Threads of Time tab is masked until this is set, and only
    // the owl's Goto_HyruleCastle event (Dm_HrlField_0010, shared) sets it.
    {0x1450c74fu, Kind::scene, "UI_IsFinishChallengeMenuOpenEvent"},
    {0x14b2d7bcu, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Sun_10"},
    {0x14d6db1eu, Kind::scene, "HavePlayedEvent.Dm_LostWood_0000"},
    {0x16f7f759u, Kind::scene, "HavePlayedEvent.Dm_Kakariko_0050"},
    {0x17072dbcu, Kind::scene, "HavePlayedEvent.Dm_DeathMtTrail_0035"},
    {0x17ecec75u, Kind::scene, "HavePlayedEvent.Dm_Zora_0020"},
    {0x18d08485u, Kind::scene, "HavePlayedEvent.Dm_DeathMtTrail_0006"},
    {0x19898d14u, Kind::world, "Npc_Hylia_Soldier_003_GateOpen"},
    {0x1c3ca514u, Kind::scene, "HavePlayedEvent.Dm_LakeHrl_0010"},
    {0x1db3a87eu, Kind::scene, "HavePlayedEvent.Dm_DeathMtTrail_0040"},
    {0x22d6ff6fu, Kind::talk, "HaveTalked.Npc_Kokiri_001_Shop_Child"},  // Alex's play-test 2026-11-12 (issue 173)
    {0x249e026eu, Kind::scene, "HavePlayedEvent.Dm_GerudoValley_0000"},
    {0x26d2802cu, Kind::scene, "HavePlayedEvent.Dm_HrlCastle_0035"},
    {0x273a73e9u, Kind::scene, "HavePlayedEvent.Dm_DeathMtTrail_0010"},
    {0x27bcb659u, Kind::world, "GerudoFortress_DestroyGerudoThief_Npc_Hylia_015_Adult"},
    {0x29076b06u, Kind::scene, "HavePlayedEvent.Dm_GerudoValley_0010"},
    {0x291f87b2u, Kind::world, "IsMalonAppear_Market"},
    {0x2adcc032u, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0160"},
    {0x2b24842au, Kind::world, "GerudoFortress_DestroyGerudoThief_Npc_Hylia_013_Adult"},
    {0x2be7fcf9u, Kind::scene, "HavePlayedEvent.Dm_DesertColossus_0030"},
    {0x2cd669e8u, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Zelda"},
    {0x2e7db943u, Kind::scene, "HavePlayedEvent.Dm_ZoraRiver_0010"},
    {0x3238c43eu, Kind::scene, "HavePlayedEvent.Dm_Kakariko_0060"},
    {0x3452914au, Kind::scene, "HavePlayedEvent.Dm_DesertColossus_0020"},
    {0x348ec6f4u, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Sun_06"},
    {0x35307febu, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Saria"},
    {0x356bd37bu, Kind::world, "Get_StoneOfForest_Complete_Mido_TalkEnd"},
    {0x35925fb8u, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Sun"},
    {0x3690868du, Kind::scene, "HavePlayedEvent.Dm_HrlField_0071"},
    {0x3725522fu, Kind::scene, "HavePlayedEvent.Dm_LakeHrl_0000"},
    {0x38f16559u, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0110"},
    {0x3b320c55u, Kind::world, "KokiriForest_Npc_Kokiri_Mido_Adult_Blocker"},
    {0x3cd8eff5u, Kind::scene, "HavePlayedEvent.Dm_Goron_0040"},
    {0x3eac8adau, Kind::world, "GraveYard_RoyalFamilyTomb_Breaked"},
    {0x4113dad1u, Kind::scene, "HavePlayedEvent.Dm_ZoraFountain_0010"},
    {0x437624edu, Kind::scene, "HavePlayedEvent.Get_StoneOfFire_GetBombBag"},
    {0x47603088u, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0090"},
    {0x4a44dfecu, Kind::scene, "HavePlayedEvent.Get_StoneOfFire_EmitLight"},
    {0x4a7376bbu, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0158"},
    {0x4c85aed3u, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Shadow"},
    {0x4da1e16au, Kind::scene, "HavePlayedEvent.Dm_DeathMtTrail_0025"},
    {0x4fb5bc83u, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0145"},
    {0x50044004u, Kind::scene, "HavePlayedEvent.Dm_Graveyard_0000"},
    {0x502f6ceau, Kind::world, "KokiriForest_Npc_Kokiri_Mido_Child_Blocker"},
    {0x503a54cfu, Kind::world, "SageOfWater_Icedelete"},
    {0x505bbf4cu, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Sun_04"},
    {0x507ccf17u, Kind::scene, "HavePlayedEvent.Dm_LakeHrl_0035"},
    {0x507d99c9u, Kind::scene, "HavePlayedEvent.Get_StoneOfFire_MeetDarnia"},
    {0x516ed011u, Kind::scene, "HavePlayedEvent.Get_StoneOfFire_DodongoCavernOpne"},
    {0x520f5421u, Kind::scene, "HavePlayedEvent.Dm_Deku_0000"},
    {0x527656c4u, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0175"},
    {0x53efe6f6u, Kind::scene, "HavePlayedEvent.Dm_HrlField_0020"},
    {0x56432857u, Kind::scene, "HavePlayedEvent.KokiriForest_WarpFromInsideTheDekuTree"},
    {0x5882c2e3u, Kind::world, "Npc_Kaepora_LakeHrl_Appeared_FromValley"},
    {0x5a4adf3fu, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Forest"},
    {0x5ab867ffu, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0075"},
    {0x5ba9783eu, Kind::scene, "HavePlayedEvent.Dm_DeathMtTrail_0020"},
    {0x603cc0d0u, Kind::scene, "HavePlayedEvent.Dm_DeathMtCrater_0000"},
    {0x60992704u, Kind::scene, "HavePlayedEvent.Dm_HrlField_0060"},
    {0x61960b30u, Kind::world, "KakarikoVillage_Npc_Hylia_009_Child_DeleteWaterPlane"},
    {0x61f46481u, Kind::scene, "HavePlayedEvent.Get_StoneOfWater_DiscoveryStone"},
    {0x6240e02cu, Kind::scene, "HavePlayedEvent.Dm_HrlCastle_0040"},
    {0x62e6144cu, Kind::scene, "HavePlayedEvent.GoTo_DeathMountainCrater_FromGoronCity"},
    {0x6359aec6u, Kind::scene, "HavePlayedEvent.Dm_ZoraRiver_0000"},
    {0x64a8681cu, Kind::scene, "HavePlayedEvent.Goto_LakeHylia"},
    {0x67f2941fu, Kind::scene, "HavePlayedEvent.Dm_Kakariko_0010"},
    {0x6a38e2acu, Kind::scene, "HavePlayedEvent.Dm_DeathMtTrail_0005"},
    {0x6c054b8eu, Kind::scene, "HavePlayedEvent.Dm_Kakariko_0025"},
    {0x6c4c8438u, Kind::scene, "HavePlayedEvent.Dm_Gerudo_0000"},
    {0x6c679d7fu, Kind::scene, "HavePlayedEvent.Dm_LostWood_0020"},
    {0x6d033b74u, Kind::scene, "HavePlayedEvent.Dm_DeathMtTrail_0000"},
    {0x6d0523e3u, Kind::scene, "HavePlayedEvent.Dm_FairyFountain_Din"},
    {0x712fffd5u, Kind::scene, "HavePlayedEvent.Goto_SacredForest"},
    {0x71fb94e3u, Kind::scene, "HavePlayedEvent.Goto_ZoraDomain"},
    {0x7275f781u, Kind::scene, "HavePlayedEvent.Dm_DeathMtTrail_0027"},
    {0x72984c4fu, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0050"},  // Alex's play-test 2026-11-12 (issue 173)
    {0x754f6228u, Kind::scene, "HavePlayedEvent.Meet_FourCarpenter_ClearEvent"},
    {0x75b6f007u, Kind::scene, "HavePlayedEvent.Dm_FairyFountain_Nayru"},
    {0x7607c748u, Kind::scene, "HavePlayedEvent.Dm_DesertColossus_0000"},
    {0x778a19e4u, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0100"},
    {0x77a28a9eu, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0060"},
    {0x78525c68u, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0063"},
    {0x78a420f6u, Kind::scene, "HavePlayedEvent.HyruleField_DoorOfTime_GetOcarinaOfTime"},
    {0x7be282fbu, Kind::scene, "HavePlayedEvent.Dm_Goron_0025"},
    {0x7d010dc8u, Kind::scene, "HavePlayedEvent.Dm_DeathMtTrail_0007"},
    {0x7d51f8ebu, Kind::scene, "HavePlayedEvent.Dm_DesertColossus_0040"},
    {0x7fa71be2u, Kind::scene, "HavePlayedEvent.Dm_LostWood_0021"},
    {0x816983e3u, Kind::scene, "HavePlayedEvent.Goto_ZoraRiver"},
    {0x81924fa3u, Kind::scene, "HavePlayedEvent.Dm_LakeHrl_0003"},
    {0x81f4c6b5u, Kind::scene, "HavePlayedEvent.Dm_LakeHrl_0020"},
    {0x843da2aau, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0170"},
    {0x880a2efau, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Sun_09"},
    {0x88163dffu, Kind::scene, "HavePlayedEvent.Dm_LonLon_0040"},
    {0x88bb5813u, Kind::world, "GerudoFortress_DestroyGerudoThief_Npc_Hylia_014_Adult"},
    {0x89fc39f9u, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Sun_07"},
    {0x8ab78984u, Kind::scene, "HavePlayedEvent.Dm_Goron_0000"},
    {0x8b591e04u, Kind::scene, "HavePlayedEvent.Goto_KakarikoWells"},
    {0x8bb76427u, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0045"},  // Alex's play-test 2026-11-12 (issue 173)
    {0x8c9a392fu, Kind::scene, "HavePlayedEvent.Dm_HrlField_0080"},
    {0x8e15ffd6u, Kind::scene, "HavePlayedEvent.Dm_HrlCastle_0060"},
    {0x8f02a43bu, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0162"},
    {0x8fa2608eu, Kind::scene, "HavePlayedEvent.Dm_FairyFountain_Power"},
    {0x9312137cu, Kind::scene, "HavePlayedEvent.Dm_Gerudo_0030"},
    {0x97137664u, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0070"},
    {0x98bf10c1u, Kind::scene, "HavePlayedEvent.HyruleField_Goto_HyruleCastle_Kaepora"},
    {0x992fb436u, Kind::owl, "HaveTalked_Special.Npc_Kaepora_DeathMountainTrail_Talk_Event2"},
    {0x9a400d41u, Kind::scene, "HavePlayedEvent.Dm_ZoraRiver_0005"},
    {0x9a60a460u, Kind::world, "KokiriForest_Meet_DekuTree_OpenDekuTreeMouth"},  // Alex's play-test 2026-11-12 (issue 173)
    {0x9b1b27d2u, Kind::world, "KokiriForest_LinkHouseDoor_Open"},  // Alex's play-test 2026-11-12 (issue 173)
    {0x9cc98b79u, Kind::scene, "HavePlayedEvent.Dm_HrlField_0050"},
    {0x9dae4eb7u, Kind::scene, "HavePlayedEvent.SageOfFire_CityIn"},
    {0x9dfb0473u, Kind::scene, "HavePlayedEvent.Dm_HrlCastle_0045"},
    {0x9e1efba1u, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Storm"},
    {0xa0c96073u, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Sun_08"},
    {0xa1d82e4fu, Kind::scene, "HavePlayedEvent.Dm_HrlField_0005"},
    {0xa1eb22e6u, Kind::world, "LostWood_Npc_Kokiri_Blocker"},
    {0xa49cb68cu, Kind::world, "JabJabOpenedMouth"},
    {0xa5d8b51du, Kind::scene, "HavePlayedEvent.Dm_LonLon_0030"},
    {0xa66e1d49u, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0120"},
    {0xa6b0c301u, Kind::scene, "HavePlayedEvent.Dm_HrlField_0070"},
    {0xa7bb21fbu, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Spilit"},
    {0xa8bf4fddu, Kind::scene, "HavePlayedEvent.Dm_Graveyard_0010"},
    {0xa9413095u, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Sun_03"},
    {0xaa5f401au, Kind::scene, "HavePlayedEvent.Dm_Graveyard_0030"},
    {0xac5d3561u, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Shine"},
    {0xadbfc26au, Kind::scene, "HavePlayedEvent.Dm_HrlCastle_0005"},
    {0xb307eda1u, Kind::scene, "HavePlayedEvent.Dm_LakeHrl_0030"},
    {0xb3585160u, Kind::scene, "HavePlayedEvent.Dm_Gerudo_0010"},
    {0xb38319abu, Kind::scene, "HavePlayedEvent.Dm_ZoraFountain_0000"},
    {0xb416bd13u, Kind::owl, "HaveTalked_Special.Npc_Kaepora_DeathMountainTrail_Talk_Event1"},
    {0xb4fc61d8u, Kind::scene, "HavePlayedEvent.Dm_LakeHrl_0011"},
    {0xb5fbb015u, Kind::scene, "HavePlayedEvent.HyruleField_DoorOfTime_SendOff"},
    {0xb649379fu, Kind::scene, "HavePlayedEvent.Goto_HyruleCastle_EntryMarket"},
    {0xb68eb220u, Kind::scene, "HavePlayedEvent.Dm_LostWood_0015"},
    {0xb6df0bf3u, Kind::scene, "HavePlayedEvent.Dm_Gerudo_0023"},
    {0xb85d902bu, Kind::scene, "HavePlayedEvent.Dm_HrlField_0040"},
    {0xb8cf4439u, Kind::scene, "HavePlayedEvent.Dm_Gerudo_0025"},
    {0xb972f383u, Kind::scene, "HavePlayedEvent.Dm_ZoraFountain_0020"},
    {0xb9d7d709u, Kind::scene, "HavePlayedEvent.Dm_HrlCastle_0020"},
    {0xba23b4eeu, Kind::scene, "HavePlayedEvent.Dm_Goron_0020"},
    {0xbba5040bu, Kind::scene, "HavePlayedEvent.Dm_DesertColossus_0025"},
    {0xbca9167bu, Kind::scene, "HavePlayedEvent.SageOfWater_KingZora_IsMelted_Ice"},
    {0xbd081d54u, Kind::scene, "HavePlayedEvent.Dm_LostWood_0030"},
    {0xbee7359bu, Kind::scene, "HavePlayedEvent.Dm_HrlField_0030"},
    {0xbf9dfc7eu, Kind::scene, "HavePlayedEvent.Dm_Zora_0010"},
    {0xbfc69399u, Kind::talk, "HaveTalked_Npc_Kokiri_001_Shop_Child_PresentDekuShield"},  // Alex's play-test 2026-11-12 (issue 173)
    {0xc06dceb8u, Kind::scene, "HavePlayedEvent.Dm_Zora_0000"},
    {0xc2f17358u, Kind::scene, "HavePlayedEvent.Get_StoneOfWater_Complete"},
    {0xc3cc0e0eu, Kind::scene, "HavePlayedEvent.Dm_Gerudo_0020"},
    {0xc416e4ecu, Kind::scene, "HavePlayedEvent.Get_StoneOfFire_Complete"},
    {0xc4cef662u, Kind::scene, "HavePlayedEvent.Dm_HrlCastle_0070"},
    {0xc6724c2eu, Kind::world, "KokiriForest_Npc_Kokiri_Blocker"},
    {0xca320c10u, Kind::scene, "HavePlayedEvent.Dm_FairyFountain_Frore"},
    {0xca55fa8bu, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0095"},
    {0xcb7ab930u, Kind::scene, "HavePlayedEvent.Dm_DeathMtCrater_0000_FromTop"},
    {0xccdeb2acu, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0130"},
    {0xce759dddu, Kind::scene, "HavePlayedEvent.Dm_SacredForest_0000"},
    {0xcf7dad5cu, Kind::world, "GraveYard_RoyalFamilyTomb_Open"},
    {0xcff5a7dfu, Kind::scene, "HavePlayedEvent.SageOfFire_CityIn_CrackWall"},
    {0xd21c4627u, Kind::scene, "HavePlayedEvent.KokiriForest_ArriveLostWood"},
    {0xd35d3f12u, Kind::scene, "HavePlayedEvent.Dm_HrlField_0000"},
    {0xd3643df1u, Kind::scene, "HavePlayedEvent.Dm_Goron_0028"},
    {0xd4ef7853u, Kind::scene, "HavePlayedEvent.Dm_HrlField_0010"},
    {0xd711a296u, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0150"},
    {0xd78d5af7u, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0155"},
    {0xdb8433f7u, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Epona"},
    {0xdbb40c10u, Kind::scene, "HavePlayedEvent.Dm_DeathMtTrail_0050"},
    {0xdcf49939u, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Sun_05"},
    {0xe0fc0275u, Kind::scene, "HavePlayedEvent.Dm_LostWood_0025"},
    {0xe2c6b142u, Kind::scene, "HavePlayedEvent.Dm_Kakariko_0040"},
    {0xe2e01efdu, Kind::scene, "HavePlayedEvent.Dm_LostWood_0000_GoronCity"},
    {0xe36744cfu, Kind::scene, "HavePlayedEvent.Dm_FairyFountain_Wisdom"},
    {0xe389ac83u, Kind::owl, "HaveTalked_Special.Npc_Kaepora_DeathMountainTrail_Talk_Event46"},
    {0xe54e7505u, Kind::scene, "HavePlayedEvent.Dm_DeathMtTrail_0030"},
    {0xe563c15bu, Kind::scene, "HavePlayedEvent.Dm_LostWood_0040"},
    {0xe5dc5674u, Kind::world, "KokiriForest_Npc_Kokiri_007_Child_Blocker_00"},
    {0xe618c0f1u, Kind::scene, "HavePlayedEvent.Dm_HrlCastle_0050"},
    {0xe6a9843du, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Water"},
    {0xe7cfb651u, Kind::scene, "HavePlayedEvent.Dm_LakeHrl_0005"},
    {0xe819f010u, Kind::scene, "HavePlayedEvent.Dm_GerudoValley_0020"},
    {0xe88f015eu, Kind::scene, "HavePlayedEvent.Dm_Goron_0030"},
    {0xe8ec37e2u, Kind::scene, "HavePlayedEvent.KokiriForest_Get_StoneOfForest_Completed_Event"},
    {0xea8d5887u, Kind::scene, "HavePlayedEvent.Dm_ZoraFountain_0040"},
    {0xebaec07fu, Kind::scene, "HavePlayedEvent.Dm_Kokiri_0140"},
    {0xedfe9d87u, Kind::world, "GerudoFortress_DestroyGerudoThief_Npc_Hylia_016_Adult"},
    {0xee47df95u, Kind::scene, "HavePlayedEvent.Dm_MelodyGet_Time"},
    {0xee842b73u, Kind::scene, "HavePlayedEvent.Dm_Gerudo_0022"},
    {0xeecaa92au, Kind::world, "KakarikoVillage_DeleteDummyLadder"},
    {0xef3af41eu, Kind::scene, "HavePlayedEvent.Get_StoneOfForest_GetSword"},
    {0xef6ebecfu, Kind::scene, "HavePlayedEvent.Dm_Gerudo_0021"},
    {0xef98d829u, Kind::scene, "HavePlayedEvent.KokiriForest_Meet_DekuTree_MeetDekuTree_01"},
    {0xf07a3be3u, Kind::scene, "HavePlayedEvent.Dm_HrlCastle_0030"},
    {0xf0fde374u, Kind::world, "KokiriForest_Npc_Kokiri_007_Child_Blocker"},
    {0xf1826d42u, Kind::scene, "HavePlayedEvent.Dm_LonLon_0020"},
    {0xf38be607u, Kind::scene, "HavePlayedEvent.Dm_Kakariko_0000"},
    {0xf5be8a94u, Kind::world, "Npc_Hylia_009_Child_IsStormSongPlayed"},
    {0xf66ceb94u, Kind::scene, "HavePlayedEvent.Dm_Goron_0045"},
    {0xf84339cbu, Kind::scene, "HavePlayedEvent.Dm_DeathMtCrater_0010"},
    {0xfd1b310eu, Kind::world, "IsMalonAppear_HyruleCastleSecondTime"},
    {0xfd925132u, Kind::scene, "HavePlayedEvent.Dm_HrlCastle_0000"},
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

std::span<const Flag> Flags() {
    return kFlags;
}

const Flag* Find(std::uint32_t hash) {
    const auto it = std::lower_bound(kFlags.begin(), kFlags.end(), hash,
                                     [](const Flag& f, std::uint32_t h) { return f.hash < h; });
    return it != kFlags.end() && it->hash == hash ? &*it : nullptr;
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
    for (const Flag& f : kFlags) {
        keys.push_back(coop::host::FlagKey(f.hash, std::uint16_t{0}));
    }
    shared = coop::host::SharedFlags(std::move(keys));  // sorts, drops repeats
    return shared->size() - before;
}

std::optional<std::uint32_t> QuestCatchUp(std::optional<std::uint32_t> step,
                                          std::optional<bool> mido_blocker,
                                          std::optional<bool> met_deku_tree) {
    const auto rank = [](std::uint32_t v) -> int {
        switch (v) {
        case quest::kReady:
            return 1;
        case quest::kStep01:
            return 2;
        case quest::kStep02:
            return 3;
        case quest::kStep03:
            return 4;
        case quest::kComplete:
            return 5;
        default:
            return 0;  // NotReady or a value we do not know
        }
    };
    if (!step || rank(*step) == 0) {
        return std::nullopt;
    }
    std::uint32_t target = 0;
    if (met_deku_tree == true) {
        target = quest::kComplete;
    } else if (mido_blocker == false) {
        target = quest::kStep03;
    } else {
        return std::nullopt;
    }
    if (rank(target) <= rank(*step)) {
        return std::nullopt;
    }
    return target;
}

bool On() {
    return !EnvOff("LICHII_COOP_STORY");
}

void AddShared(std::optional<coop::host::SharedFlags>& shared) {
    const bool on = On();
    const std::size_t added = Merge(shared, on);
#ifdef LICHII_COOP
    LOG_INFO(Core, "coop: story: sharing {} story flags{}", added,
             on ? "" : " (off: LICHII_COOP_STORY=0)");
#else
    (void)added;
#endif
}

} // namespace CoopStory
