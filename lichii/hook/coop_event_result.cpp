// SPDX-License-Identifier: GPL-3.0-or-later
// Row 98(2) result only: see coop_event_result.h.
#include "coop_event_result.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>

#include "coop/protocol/codec.hpp"
#include "coop_live_events.h"
#include "coop_logic_set.h"

namespace CoopEventResult {

namespace {
// Generated: tools/puppet_pack/eye_switches.py --romfs <romfs> --places lichii/hook/coop_live_places.cpp
// --dungeon lichii/hook/coop_dungeon.cpp --results (189 askers of 186 events, 119 live, 70 asker; 12 events a field
// asks). The comment says the class and why (for class asker: the result the room reads from the asker). Row 336: a
// shutter's lock input that the asker drives is never covered by a shared flag (eye_switches.py LOCK_INPUTS).
// clang-format off
constexpr Result kResults[] = {
    {"BottomOfTheWell_08_Enemy_WipedOut", "BottomOfTheWell", {-0.0f, 92.3f, -108.1f}, ""},  // live (EventRequestActor): results through shared flags
    {"BottomOfTheWell_SilverToken_Appear", "BottomOfTheWell", {-12.9f, 95.9f, -7.5f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_silverRupeeCounter.Start; Obj_silverRupeeCounter.TokenVisible; unshared flag HaveEncountered_Token
    {"BottomOfTheWell_SilverToken_Unlock", "BottomOfTheWell", {-17.6f, 95.9f, -12.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"Dm_DodongoCave_0010", "DodongoCavern", {-49.0f, 115.5f, -45.9f}, ""},  // asker (EventRequestActor): Obj_DodongoCave_BrokenStair_A_01 plays Obj_DodongoCave_BrokenStair_A_01_Demo_Anim (its room deletes it at load only)
    {"Dm_FireTemple_0029", "FireTemple", {-46.9f, 186.5f, -22.4f}, ""},  // live (EventRequestActor): results through shared flags
    {"Dm_ForestTemple_0014", "ForestTemple", {60.3f, 115.8f, 0.8f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_ForestTemple_MovableBox_A_01 plays C01_A_01 (its room deletes it at load only); Obj_ForestTemple_MovableBox_A_02 plays C01_A_02 (its room deletes it at load only); Obj_ForestTemple_MovableBox_A_03 plays C01_A_03 (its room deletes it at load only); Obj_ForestTemple_MovableBox_A_04 plays C01_A_04 (its room deletes it at load only)
    {"Dm_InsideGanonCastle_0035", "InsideGanonCastle", {-10.5f, 108.0f, -47.7f}, "EventBool"},  // asker (EventRequestActor): Obj_LiftRock_C_01 plays ToLift_0035 (its room deletes it at load only)
    {"DodongoCavern_Gimmick_CandleStickShutter", "DodongoCavern", {32.5f, 108.0f, -44.5f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_DodongoCave_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.IsDisableBlend; Obj_SwitchShutter_A_06.Open; unshared flag DodongoCavern_NaviHelp_08_00
    {"DodongoCavern_Gimmick_DefeatLizalfos01", "DodongoCavern", {84.9f, 100.5f, -54.5f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_DodongoCave_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open
    {"DodongoCavern_Gimmick_DefeatLizalfos02", "DodongoCavern", {94.3f, 114.9f, -49.0f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_DodongoCave_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open
    {"DodongoCavern_Gimmick_EyeSwitch", "DodongoCavern", {68.2f, 127.3f, -6.6f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_DodongoCave_FireWall_A_01.RequestExtinguish; Obj_DodongoCave_FireWall_A_01.StartBurnSign; unshared flag DodongoCavern_NaviHelp_14_00
    {"DodongoCavern_Gimmick_StatueSwitch", "DodongoCavern", {80.0f, 102.0f, -6.5f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_DodongoCave_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open; unshared flag DodongoCavern_NaviHelp_02_00
    {"DodongoCavern_Gimmick_SwitchShortcut", "DodongoCavern", {19.0f, 118.0f, -23.0f}, ""},  // live (EventRequestActor): results through shared flags
    {"DodongoCavern_Gimmick_SwitchShutter_Room01", "DodongoCavern", {16.0f, 103.8f, -23.0f}, "EventBool"},  // asker (EventRequestActor): Obj_DodongoCave_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open
    {"DodongoCavern_Gimmick_SwitchShutter_Room12", "DodongoCavern", {-43.5f, 116.0f, -8.0f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_DodongoCave_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open
    {"DodongoCavern_Gimmick_SwitchShutter_Room19", "DodongoCavern", {14.8f, 100.3f, -77.9f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_DodongoCave_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open; unshared flag DodongoCavern_NaviHelp_19_04
    {"FireTemple_06_EyeSwitch", "FireTemple", {36.7f, 150.8f, 3.9f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_FireTemple_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_02.IsDisableBlend; Obj_SwitchShutter_A_02.Open
    {"FireTemple_15_OpenShutter", "FireTemple", {7.3f, 103.5f, -12.0f}, "EventBool"},  // asker (EventRequestActor): Obj_EnemyCounter_A_01.Finish
    {"FireTemple_LargePillarFallCamera", "FireTemple", {-25.4f, 143.2f, -3.0f}, ""},  // live (EventRequestActor): results through shared flags
    {"FireTemple_MovingFireWallCamera_Room16", "FireTemple", {28.0f, 149.1f, 17.1f}, ""},  // live (EventRequestActor): results through shared flags
    {"FireTemple_PutOutFireWallTrap", "FireTemple", {-61.3f, 156.2f, -15.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"FireTemple_Room24_FocusOnGimmick", "FireTemple", {-72.2f, 149.9f, 3.3f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_FireTemple_SwitchShutter_A_01.LockMinusZ; Obj_SwitchShutter_A_02.Open
    {"FireTemple_SwitchFireCamera_Room04", "FireTemple", {93.1f, 141.0f, 16.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"FireTemple_SwitchFireCamera_Room12", "FireTemple", {-67.5f, 187.3f, 14.2f}, ""},  // live (EventRequestActor): results through shared flags
    {"FocusGimmick_BlueTentacleGimmick", "InsideJabuJabuBelly", {0.4f, 103.5f, -147.8f}, ""},  // live (EventRequestActor): results through shared flags
    {"FocusGimmick_GreenTentacleGimmick", "InsideJabuJabuBelly", {-2.2f, 104.4f, -85.6f}, ""},  // live (EventRequestActor): results through shared flags
    {"FocusGimmick_RedTentacleGimmick0", "InsideJabuJabuBelly", {32.8f, 101.2f, -139.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"FocusOnGimmick_FireTemple_FlareDancer02", "FireTemple", {-7.0f, 102.0f, -35.5f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_FireTemple_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_02.Open
    {"FocusOnGimmick_InsideJabuJabuBelly_02", "InsideJabuJabuBelly", {20.0f, 79.5f, -35.0f}, ""},  // live (EventRequestActor): results through shared flags
    {"FocusOnGimmick_JabujabuLift", "InsideJabuJabuBelly", {-1.0f, 77.5f, -41.2f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room02_ShutterOpen", "ForestTemple", {16.4f, 101.7f, -12.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room04_OpenDoor_AppearTBox", "ForestTemple", {0.0f, 105.7f, -60.5f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_ForestTemple_SwitchShutter_A_03.LockMinusZ; Obj_SwitchShutter_A_02.Open
    {"ForestTemple_Room11_TBoxAppear", "ForestTemple", {-52.8f, 115.0f, 2.0f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room12_WaterLevelDrop", "ForestTemple", {-29.1f, 107.4f, -34.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room17_BigTBoxAppear", "ForestTemple", {0.2f, 114.7f, -66.2f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room17_ShutterOpen", "ForestTemple", {-7.5f, 121.5f, -59.5f}, "EventBool"},  // asker (EventRequestActor): Obj_ForestTemple_SwitchShutter_A_03.LockMinusZ; Obj_SwitchShutter_A_02.Open; unshared flag ForestTemple_NaviHelp_17_03
    {"ForestTemple_Room17_TBoxAppear_CandleLight", "ForestTemple", {-19.3f, 114.7f, -63.6f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room17_TBoxAppear_CandleLight_ShutterOpen", "ForestTemple", {19.3f, 114.7f, -63.6f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_ForestTemple_SwitchShutter_A_03.LockPlusZ; Obj_SwitchShutter_A_02.Open
    {"ForestTemple_Room17_TwoStalfosAppear", "ForestTemple", {4.8f, 132.8f, -60.4f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room21_ShutterOpen", "ForestTemple", {-58.2f, 105.1f, -60.4f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room22_FallingCeiling", "ForestTemple", {55.9f, 120.3f, -43.9f}, "EventBool"},  // asker (EventRequestActor): Obj_ForestTemple_FallingCeiling_A_01.BootUp; Obj_ForestTemple_FallingCeiling_A_01.IsForceFall
    {"ForestTemple_Room22_IceMelts", "ForestTemple", {56.5f, 137.7f, -10.7f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room22_ShutterOpen", "ForestTemple", {54.3f, 104.9f, -19.8f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room22_ShutterOpen_TBoxAppear", "ForestTemple", {-68.0f, 107.0f, -26.5f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_ForestTemple_SwitchShutter_A_03.LockPlusZ; Obj_SwitchShutter_A_02.Open
    {"ForestTemple_Room25_CandleLight_ShutterOpen", "ForestTemple", {53.3f, 106.0f, -3.9f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_ForestTemple_SwitchShutter_A_03.LockPlusZ; Obj_SwitchShutter_A_02.Open
    {"ForestTemple_Room25_FirstTimeBLock", "FireTemple", {-46.9f, 153.9f, 0.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room25_FirstTimeBLock", "ForestTemple", {-16.2f, 101.4f, -14.7f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room27_CandleLight", "ForestTemple", {-10.4f, 61.0f, 6.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room27_IceMelts", "ForestTemple", {23.8f, 65.5f, -3.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"ForestTemple_Room27_ShutterOpen", "ForestTemple", {0.0f, 65.7f, -19.6f}, "EventBool"},  // asker (EventRequestActor): Obj_SwitchShutter_A_05.Open
    {"GerudoTrainingGround_00_FallTreasureBox", "GerudoTrainingGround", {2.5f, 99.5f, -9.8f}, ""},  // live (EventRequestActor): results through shared flags
    {"GerudoTrainingGround_01_AppearTreasureBox_From00", "GerudoTrainingGround", {-51.0f, 98.1f, -13.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"GerudoTrainingGround_02From03_SilverToken_Appear", "GerudoTrainingGround", {-63.8f, 105.9f, -29.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"GerudoTrainingGround_02From03_SilverToken_Unlock", "GerudoTrainingGround", {-64.1f, 105.9f, -34.6f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_silverRupeeCounter.Vanish
    {"GerudoTrainingGround_02_SilverToken_Appear", "GerudoTrainingGround", {-66.1f, 111.9f, -44.3f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_silverRupeeCounter.TokenVisible; OperateGameDataAllocAutogenGameData.IsWrite; unshared flag HaveEncountered_Token
    {"GerudoTrainingGround_02_SilverToken_Unlock", "GerudoTrainingGround", {-66.3f, 111.9f, -48.9f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_silverRupeeCounter.Vanish; OperateGameDataAllocAutogenGameData.IsWrite
    {"GerudoTrainingGround_03_AppaerTreasureBox", "GerudoTrainingGround", {-53.8f, 104.0f, -88.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"GerudoTrainingGround_03_ShutterOpen", "GerudoTrainingGround", {-36.3f, 119.5f, -92.9f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_Gerudo_SwitchShutter_A_04.LockPlusZ; Obj_SwitchShutter_A_02.IsDisableBlend; Obj_SwitchShutter_A_02.Open
    {"GerudoTrainingGround_04_ActStoneGimmick", "GerudoTrainingGround", {13.1f, 99.9f, -87.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"GerudoTrainingGround_05_AppaerTreasureBox", "GerudoTrainingGround", {43.5f, 101.9f, -89.5f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_Gerudo_SwitchShutter_A_04.LockMinusZ; Obj_SwitchShutter_A_02.Open; TriggerGameSafetyStop.IsDrive
    {"GerudoTrainingGround_05_DisappearFire", "GerudoTrainingGround", {44.9f, 107.6f, -84.4f}, ""},  // live (EventRequestActor): results through shared flags
    {"GerudoTrainingGround_05_ShutterOpen", "GerudoTrainingGround", {34.4f, 108.5f, -89.8f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_Gerudo_SwitchShutter_A_04.LockMinusZ; Obj_SwitchShutter_A_02.IsDisableBlend; Obj_SwitchShutter_A_02.Open; TriggerGameSafetyStop.IsDrive
    {"GerudoTrainingGround_06_DisappearFire", "GerudoTrainingGround", {32.8f, 107.6f, -35.6f}, "EventBool"},  // asker (EventRequestActor): Obj_GerudoTrainingGround_FireWall_A_01.StartBurnSign
    {"GerudoTrainingGround_06_SilverToken_Appear", "GerudoTrainingGround", {54.7f, 110.1f, -66.9f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_silverRupeeCounter.Start; Obj_silverRupeeCounter.TokenVisible; unshared flag HaveEncountered_Token
    {"GerudoTrainingGround_06_SilverToken_Unlock", "GerudoTrainingGround", {59.5f, 110.1f, -71.6f}, "EventBool"},  // asker (EventRequestActor): Obj_silverRupeeCounter.Vanish; OperateGameDataAllocAutogenGameData.IsWrite
    {"GerudoTrainingGround_08_FenceFallDown", "GerudoTrainingGround", {0.7f, 101.1f, -45.0f}, ""},  // live (EventRequestActor): results through shared flags
    {"GerudoTrainingGround_09_AppaerTreasureBox", "GerudoTrainingGround", {71.6f, 101.9f, -42.6f}, ""},  // live (EventRequestActor): results through shared flags
    {"GerudoTrainingGround_09_SilverToken_Unlock", "GerudoTrainingGround", {76.4f, 101.9f, -47.2f}, "EventBool"},  // asker (EventRequestActor): Obj_silverRupeeCounter.Vanish
    {"GerudoTrainingGround_10_AppearTreasureBox", "GerudoTrainingGround", {-44.2f, 104.2f, -124.7f}, ""},  // live (EventRequestActor): results through shared flags
    {"GerudoTrainingGround_10_AppearTreasureBox", "GerudoTrainingGround", {-43.6f, 109.9f, -137.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"GerudoTrainingGround_10_FallTreasureBox", "GerudoTrainingGround", {-51.4f, 103.5f, -131.8f}, ""},  // live (EventRequestActor): results through shared flags
    {"GerudoTrainingGround_Room04_StoneGimmick", "GerudoTrainingGround", {14.4f, 110.0f, -82.1f}, ""},  // live (EventRequestActor): results through shared flags
    {"GerudoTrainingGround_Room07_TreasureAppear", "GerudoTrainingGround", {44.4f, 98.7f, -1.0f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_Gerudo_SwitchShutter_A_04.LockMinusZ; Obj_SwitchShutter_A_02.Open
    {"IceCavern_Gimmick_BlueFireShowup", "IceCavern", {19.1f, 115.2f, -69.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"IceCavern_Room01_DefeatEnemy", "IceCavern", {-5.8f, 109.5f, 26.2f}, ""},  // live (EventRequestActor): results through shared flags
    {"IceCavern_Room05_SilverToken_Appear", "IceCavern", {11.0f, 107.9f, -24.0f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_silverRupeeCounter_03.Start; Obj_silverRupeeCounter_03.TokenVisible; unshared flag HaveEncountered_Token
    {"IceCavern_Room05_SilverToken_Unlock", "IceCavern", {14.9f, 107.9f, -24.0f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_IceCave_RotateBlade_A_01.IsResetAngle
    {"IceCavern_Room06_SilverToken_Appear", "IceCavern", {-49.1f, 113.0f, -8.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"IceCavern_Room06_SilverToken_Unlock", "IceCavern", {-49.2f, 107.3f, -7.7f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_silverRupeeCounter_05.Vanish
    {"InsideGanonCastle_DefeatEnemy", "InsideGanonCastle", {0.0f, 139.6f, -22.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_DefeatEnemy_02", "InsideGanonCastle", {-2.2f, 146.9f, -18.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_DefeatEnemy_03", "InsideGanonCastle", {2.3f, 149.7f, -24.7f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_AppaerTreasureBox", "InsideGanonCastle", {-39.2f, 103.9f, 15.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_AppaerTreasureBox_02", "InsideGanonCastle", {24.4f, 105.5f, 22.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_AppaerTreasureBox_08", "InsideGanonCastle", {47.2f, 103.9f, -103.4f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_AppaerTreasureBox_12", "InsideGanonCastle", {-38.2f, 99.8f, -21.0f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_InsideGanonCastle_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_02.Open
    {"InsideGanonCastle_Dungeon_AppearExtendingPlatform_03", "InsideGanonCastle", {39.7f, 102.8f, 41.4f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_BarrierRemove_Fire", "InsideGanonCastle", {-40.0f, 141.0f, -78.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_BarrierRemove_Forest", "InsideGanonCastle", {50.3f, 119.2f, 66.1f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_BarrierRemove_Light", "InsideGanonCastle", {-92.9f, 117.9f, -21.4f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_BarrierRemove_Shadow", "InsideGanonCastle", {30.8f, 129.5f, -75.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_BarrierRemove_Spirit", "InsideGanonCastle", {-37.0f, 122.6f, 45.6f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_BarrierRemove_Water", "InsideGanonCastle", {68.2f, 118.0f, -26.8f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_EnemyAppear01", "InsideGanonCastle", {-35.2f, 109.1f, 38.6f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_EnemyAppear02", "InsideGanonCastle", {-28.5f, 110.2f, 45.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_EnemyAppear03", "InsideGanonCastle", {-24.5f, 110.0f, 35.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_FocusOnGimmick_08_01", "InsideGanonCastle", {27.7f, 114.5f, -59.1f}, "EventBool"},  // asker (EventRequestActor): Obj_InsideGanonCastle_GlassTruth_A_05.IsOn
    {"InsideGanonCastle_Dungeon_ShutterOpen_02_01", "InsideGanonCastle", {26.2f, 111.2f, 25.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_ShutterOpen_06_01", "InsideGanonCastle", {84.6f, 107.9f, -27.2f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_ShutterOpen_08_01", "InsideGanonCastle", {46.3f, 109.1f, -126.4f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_ShutterOpen_17_01", "InsideGanonCastle", {-46.4f, 114.6f, 24.6f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_Dungeon_ShutterOpen_17_02", "InsideGanonCastle", {-29.4f, 114.1f, 25.1f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_SilverToken_Appear_Room03", "InsideGanonCastle", {35.9f, 109.9f, 38.8f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_SilverToken_Appear_Room10", "InsideGanonCastle", {-22.4f, 112.5f, -57.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_SilverToken_Appear_Room14", "InsideGanonCastle", {-66.2f, 108.1f, -19.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_SilverToken_Appear_Room16", "InsideGanonCastle", {-19.3f, 111.7f, 17.0f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideGanonCastle_SilverToken_Unlock_Room03", "InsideGanonCastle", {37.8f, 109.9f, 32.4f}, "EventBool"},  // asker (EventRequestActor): Obj_silverRupeeCounter.Vanish
    {"InsideGanonCastle_SilverToken_Unlock_Room10", "InsideGanonCastle", {-24.1f, 112.5f, -51.0f}, "EventBool"},  // asker (EventRequestActor): Obj_silverRupeeCounter.Vanish
    {"InsideGanonCastle_SilverToken_Unlock_Room14", "InsideGanonCastle", {-61.6f, 108.1f, -14.4f}, "EventBool"},  // asker (EventRequestActor): Obj_silverRupeeCounter.Vanish
    {"InsideGanonCastle_SilverToken_Unlock_Room16", "InsideGanonCastle", {-12.8f, 111.7f, 15.3f}, "EventBool"},  // asker (EventRequestActor): Obj_silverRupeeCounter.Vanish
    {"InsideJabuJabuBellyRoom01StepSwitch", "InsideJabuJabuBelly", {7.3f, 109.7f, -48.1f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideJabuJabuBellyRoom05Swich", "InsideJabuJabuBelly", {26.8f, 106.4f, -57.4f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideJabuJabuBellyRoom07StepSwitch", "InsideJabuJabuBelly", {15.0f, 100.7f, -138.8f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideJabuJabuBellyRoom07StepSwitch02", "InsideJabuJabuBelly", {-12.5f, 98.1f, -136.0f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideJabuJabuBellyRoom14StepSwitch", "InsideJabuJabuBelly", {32.0f, 78.0f, -69.6f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_InsideJabu_Jabu_StepSwitch_A_04.BootUp
    {"InsideJabuJabuBellyRoom14StepSwitch_Under", "InsideJabuJabuBelly", {26.5f, 78.0f, -69.6f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_InsideJabu_Jabu_StepSwitch_A_04.BootUp
    {"InsideJabuJabuBelly_FocusGimmick_Tentacle", "InsideJabuJabuBelly", {-25.4f, 94.1f, -181.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideJabuJabuBelly_FocusOnGimmick", "InsideJabuJabuBelly", {-1.5f, 92.6f, -24.6f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideJabuJabuBelly_Room12_TreasureBoxAppear", "InsideJabuJabuBelly", {23.8f, 92.2f, -174.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideJabuJabuBelly_TreasureBoxAppear", "InsideJabuJabuBelly", {-29.1f, 92.3f, -131.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideTheDekuTree_Gimmick_CandleAndSpiderWeb", "InsideTheDekuTree", {-10.0f, 82.5f, -9.5f}, "EventBool"},  // asker (EventRequestActor): Obj_InsideTheDekuTree_SpiderWeb_A_02.Burn; Obj_StepSwitch_A_05.BootUp; Obj_SwitchCandleStick_A_02.Burn; Obj_SwitchCandleStick_A_02.ForceControl; OperateGameDataAllocAutogenGameData.IsWrite
    {"InsideTheDekuTree_Gimmick_FallingLadder", "InsideTheDekuTree", {-37.5f, 117.8f, 12.5f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_InsideTheDekuTree_FallingLadder_A_01.IsOn
    {"InsideTheDekuTree_Gimmick_RoomD_PlatformUp", "InsideTheDekuTree", {-22.6f, 122.6f, -3.8f}, "EventBool"},  // asker (EventRequestActor): Obj_InsideTheDekuTree_ElevatorLift_A_01.IsOn; Obj_InsideTheDekuTree_ElevatorLift_A_02.IsOn; Obj_InsideTheDekuTree_ElevatorLift_A_03.IsOn
    {"InsideTheDekuTree_Gimmick_RoomF_EyeSwitch", "InsideTheDekuTree", {-14.5f, 82.7f, 27.8f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideTheDekuTree_Gimmick_RoomG_WaterLevelDrop", "InsideTheDekuTree", {-23.5f, 81.0f, 34.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"InsideTheDekuTree_Gimmick_RoomH_CandleStick", "InsideTheDekuTree", {-63.6f, 86.0f, 47.2f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_01_BeakStatue_Clear", "ShadowTemple", {51.7f, 107.3f, -0.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_03_EnemyAnnihilation_Clear", "ShadowTemple", {2.6f, 93.9f, -24.0f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_04_EnemyAnnihilation_Clear", "ShadowTemple", {-74.4f, 97.3f, -10.1f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_ShadowTemple_SwitchShutter_A_04.LockPlusZ; Obj_SwitchShutter_A_02.Open
    {"ShadowTemple_07_EnemyAnnihilation_Clear", "ShadowTemple", {96.5f, 90.4f, 20.6f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_10_EnemyAnnihilation_Clear", "ShadowTemple", {168.8f, 64.8f, 98.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_13_EnemyAnnihilation_DOWN_Clear", "ShadowTemple", {39.3f, 61.2f, 34.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_13_EnemyAnnihilation_UP_Clear", "ShadowTemple", {20.8f, 64.7f, 34.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_15_EnemyAnnihilation_Clear", "ShadowTemple", {122.8f, 56.6f, 6.1f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_16_EnemyAnnihilation_Clear", "ShadowTemple", {103.4f, 52.0f, -17.6f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_19_EnemyAnnihilation_Clear", "ShadowTemple", {-180.9f, 49.2f, -64.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_21_EnemyAnnihilation_Clear", "ShadowTemple", {-181.9f, 44.3f, -14.8f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_Gimmick_Focus_LargeJar_Key", "ShadowTemple", {36.3f, 56.2f, 33.4f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_Room06_SilverToken_Appear", "ShadowTemple", {91.1f, 95.1f, -25.7f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_Room06_SilverToken_Unlock", "ShadowTemple", {95.0f, 95.1f, -25.7f}, "EventBool"},  // asker (EventRequestActor): Obj_silverRupeeCounter_04.Vanish
    {"ShadowTemple_Room09_SilverToken_Appear", "ShadowTemple", {48.8f, 64.4f, 80.0f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_Room09_SilverToken_Unlock", "ShadowTemple", {44.9f, 64.4f, 80.0f}, "EventBool"},  // asker (EventRequestActor): Obj_silverRupeeCounter_04.Vanish
    {"ShadowTemple_Room12_SilverToken_Appear", "ShadowTemple", {48.5f, 60.5f, 33.1f}, ""},  // live (EventRequestActor): results through shared flags
    {"ShadowTemple_Room12_SilverToken_Unlock", "ShadowTemple", {46.2f, 60.5f, 32.6f}, "EventBool"},  // asker (EventRequestActor): Obj_silverRupeeCounter.Vanish
    {"SpiritTemple_FaceIsBroken_Effect", "SpiritTemple", {-3.2f, 141.6f, -65.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpiritTemple_Level_Room_03_BoardNetFall", "SpiritTemple", {-55.0f, 107.3f, -51.8f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpiritTemple_Level_Room_04_EyeSwitchOn", "SpiritTemple", {-31.3f, 107.9f, -81.7f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpiritTemple_Level_Room_05_SilverToken_Appear", "SpiritTemple", {-9.9f, 112.7f, -59.1f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpiritTemple_Level_Room_05_SilverToken_Unlock", "SpiritTemple", {-9.9f, 112.7f, -63.0f}, "EventBool"},  // asker (EventRequestActor): Obj_silverRupeeCounter_06.Vanish
    {"SpiritTemple_Level_Room_07_SunSwitchOn", "SpiritTemple", {-32.2f, 123.8f, -48.8f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpiritTemple_Level_Room_08_SwitchOn_01", "SpiritTemple", {-17.1f, 131.7f, -44.1f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_SpiritTemple_SwitchShutter_A_01.LockMinusZ; Obj_SwitchShutter_A_06.IsDisableBlend; Obj_SwitchShutter_A_06.Open
    {"SpiritTemple_Level_Room_10_SilverToken_Appear", "SpiritTemple", {-45.5f, 141.3f, -13.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpiritTemple_Level_Room_10_SilverToken_Unlock", "SpiritTemple", {-45.5f, 141.3f, -17.1f}, "EventBool"},  // asker (EventRequestActor): Obj_SpiritTemple_SwitchShutter_A_01.LockMinusZ; Obj_SpiritTemple_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open; Obj_silverRupeeCounter_02.Vanish
    {"SpiritTemple_Level_Room_10_SwitchOn_01", "SpiritTemple", {-59.0f, 132.8f, -27.7f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpiritTemple_Level_Room_16_SilverToken_Appear", "SpiritTemple", {47.3f, 108.2f, -48.4f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpiritTemple_Level_Room_16_SilverToken_Unlock", "SpiritTemple", {47.3f, 108.2f, -52.3f}, "EventBool"},  // asker (EventRequestActor): Obj_SpiritTemple_SwitchShutter_A_01.LockMinusZ; Obj_SpiritTemple_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open; Obj_silverRupeeCounter_02.Vanish
    {"SpiritTemple_Level_Room_19_SwitchOn_01", "SpiritTemple", {31.4f, 120.0f, -41.6f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpiritTemple_Level_Room_19_SwitchOn_01", "SpiritTemple", {40.9f, 119.2f, -47.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpiritTemple_Level_Room_19_wallmaster", "SpiritTemple", {41.9f, 125.7f, -41.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpiritTemple_Level_Room_30_BoardNetFall", "SpiritTemple", {19.8f, 159.5f, -77.8f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpiritTemple_Level_Room_32_SwitchOn_01", "SpiritTemple", {-23.8f, 159.9f, -49.7f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpiritTemple_Room19_SunSwitchShutterOpen", "SpiritTemple", {27.1f, 120.7f, -47.6f}, "EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_SpiritTemple_SwitchShutter_A_01.LockMinusZ; Obj_SpiritTemple_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open
    {"SpritTemple_Room03_OpenShutter", "SpiritTemple", {-33.6f, 104.8f, -32.7f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_SpiritTemple_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open
    {"SpritTemple_Room04_OpenShutter", "SpiritTemple", {-31.7f, 108.1f, -77.1f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpritTemple_Room05_AppearTreasure", "SpiritTemple", {-15.3f, 104.7f, -58.0f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpritTemple_Room07_AppearTreasure", "SpiritTemple", {-39.2f, 119.2f, -41.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpritTemple_Room08_AppearTreasureBox", "SpiritTemple", {0.0f, 114.2f, -55.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpritTemple_Room08_OpenShutter", "SpiritTemple", {1.5f, 118.4f, -51.4f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_SpiritTemple_SwitchShutter_A_01.LockMinusZ; Obj_SpiritTemple_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open
    {"SpritTemple_Room12_OpenShutter", "SpiritTemple", {-48.0f, 136.4f, 24.8f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_SpiritTemple_SwitchShutter_A_01.LockMinusZ; Obj_SpiritTemple_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open
    {"SpritTemple_Room14_OpenShutter", "SpiritTemple", {33.4f, 104.8f, -34.3f}, "EventBool+EventBoolEX1+EventFinish"},  // asker (EventRequestActor): Obj_SpiritTemple_SwitchShutter_A_01.LockMinusZ; Obj_SpiritTemple_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open
    {"SpritTemple_Room22_OpenShutter", "SpiritTemple", {47.2f, 131.8f, -17.8f}, "EventBool"},  // asker (EventRequestActor): Obj_SpiritTemple_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open
    {"SpritTemple_Room22_StepSwitch", "SpiritTemple", {46.5f, 134.0f, -25.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpritTemple_Room23_StepSwitch", "SpiritTemple", {56.7f, 131.6f, -16.6f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_SpiritTemple_SwitchShutter_A_01.LockMinusZ; Obj_SwitchShutter_A_06.Open
    {"SpritTemple_Room23_SunSwitch", "SpiritTemple", {55.9f, 133.0f, -33.6f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_SpiritTemple_SwitchShutter_A_01.LockMinusZ; Obj_SpiritTemple_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open
    {"SpritTemple_Room25_OpenShutter", "SpiritTemple", {55.7f, 134.1f, 23.9f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_SpiritTemple_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open
    {"SpritTemple_Room30_StepSwitch", "SpiritTemple", {23.1f, 150.9f, -71.4f}, ""},  // live (EventRequestActor): results through shared flags
    {"SpritTemple_Room31_OpenShutter", "SpiritTemple", {-3.0f, 158.1f, -46.9f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_SpiritTemple_SwitchShutter_A_01.LockMinusZ; Obj_SpiritTemple_SwitchShutter_A_01.LockPlusZ; Obj_SwitchShutter_A_06.Open
    {"StoneOfWater_ElevatedFloor", "InsideJabuJabuBelly", {-40.7f, 84.7f, -109.9f}, ""},  // live (EventRequestActor): results through shared flags
    {"WaterTempleRoom08HitSwitch", "WaterTemple", {-39.0f, 96.0f, -21.6f}, ""},  // live (EventRequestActor): results through shared flags
    {"WaterTempleRoom14StepSwitch", "WaterTemple", {1.5f, 100.0f, -109.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"WaterTempleRoom22Clear", "WaterTemple", {-83.5f, 135.5f, -69.6f}, "EventBool+EventFinish"},  // asker (EventRequestActor): TriggerChallengeSetStep.IsDrive
    {"WaterTempleRoom22Close", "WaterTemple", {-70.2f, 135.5f, -80.8f}, ""},  // live (EventRequestActor): results through shared flags
    {"WaterTemple_EyeSwitch_Focus", "WaterTemple", {-33.5f, 118.9f, -40.7f}, ""},  // live (EventRequestActor): results through shared flags
    {"WaterTemple_EyeSwitch_Focus", "WaterTemple", {0.7f, 116.1f, -29.3f}, ""},  // live (EventRequestActor): results through shared flags
    {"WaterTemple_FocusGimmick_Room18", "WaterTemple", {41.5f, 100.0f, -4.5f}, ""},  // live (EventRequestActor): results through shared flags
    {"WaterTemple_FocusonGimmick_02", "WaterTemple", {-6.7f, 89.6f, -4.5f}, "EventBool+EventFinish"},  // asker (EventRequestActor): Obj_WaterTemple_SwitchShutter_A_03.Open
    {"WaterTemple_Room06_Waterlevelrise_FocusOnGimmick", "WaterTemple", {-71.8f, 121.2f, -15.1f}, ""},  // live (EventRequestActor): results through shared flags
    {"WaterTemple_Room17_FocusOnGimmick", "WaterTemple", {25.0f, 102.3f, -4.7f}, "EventBool"},  // asker (EventRequestActor): Obj_SwitchShutter_A_02.IsDisableBlend; Obj_SwitchShutter_A_02.Open; Obj_WaterTemple_SwitchShutter_A_01.LockPlusZ
    {"WaterTemple_Waterlevelrise_FocusOnGimmick", "WaterTemple", {-35.4f, 103.8f, 17.4f}, "EventBool+EventFinish"},  // asker (EventRequestActor): unshared flag WaterTemple_Room03_FocusOnGimmick
    {"DodongoCavern_OpenShutterPlusZ_FocusOnEnemy", "DodongoCavern", {0.0f, 0.0f, 0.0f}, ""},  // live (no asker actor: a field asks it)
    {"DodongoCavern_OpenShutterPlusZ_FocusOnEnemy_02", "DodongoCavern", {0.0f, 0.0f, 0.0f}, ""},  // live (no asker actor: a field asks it)
    {"FireTemple_OpenShutterPlusZ_FocusOnEnemy", "FireTemple", {0.0f, 0.0f, 0.0f}, ""},  // live (no asker actor: a field asks it)
    {"FireTemple_OpenShutterPlusZ_FocusOnEnemy_02", "FireTemple", {0.0f, 0.0f, 0.0f}, ""},  // live (no asker actor: a field asks it)
    {"FireTemple_OpenShutterPlusZ_FocusOnEnemy_02_B", "FireTemple", {0.0f, 0.0f, 0.0f}, ""},  // live (no asker actor: a field asks it)
    {"ForestTemple_OpenDoor_Room01To02", "ForestTemple", {0.0f, 0.0f, 0.0f}, ""},  // live (no asker actor: a field asks it)
    {"ForestTemple_OpenShutterPlusZ_FocusOnEnemy", "ForestTemple", {0.0f, 0.0f, 0.0f}, ""},  // live (no asker actor: a field asks it)
    {"ForestTemple_OpenShutterPlusZ_FocusOnEnemy_02", "ForestTemple", {0.0f, 0.0f, 0.0f}, ""},  // live (no asker actor: a field asks it)
    {"InsideGanonCastle_OpenShutterPlusZ_FocusOnEnemy", "InsideGanonCastle", {0.0f, 0.0f, 0.0f}, ""},  // live (no asker actor: a field asks it)
    {"InsideGanonCastle_OpenShutterPlusZ_FocusOnEnemy_02", "InsideGanonCastle", {0.0f, 0.0f, 0.0f}, ""},  // live (no asker actor: a field asks it)
    {"InsideGanonCastle_OpenShutterPlusZ_FocusOnEnemy_03", "InsideGanonCastle", {0.0f, 0.0f, 0.0f}, ""},  // live (no asker actor: a field asks it)
    {"WaterTemple_DarkLink_DoorMovement", "WaterTemple", {0.0f, 0.0f, 0.0f}, ""},  // live (no asker actor: a field asks it)
};
// kLocks (generated with kResults: class asker only for a shutter's lock, row 336; IsLockOnly)
struct Lock {
    std::string_view event;
    std::string_view scene;
};
constexpr Lock kLocks[] = {
    {"DodongoCavern_Gimmick_DefeatLizalfos01", "DodongoCavern"},
    {"DodongoCavern_Gimmick_DefeatLizalfos02", "DodongoCavern"},
    {"DodongoCavern_Gimmick_SwitchShutter_Room01", "DodongoCavern"},
    {"DodongoCavern_Gimmick_SwitchShutter_Room12", "DodongoCavern"},
    {"FireTemple_Room24_FocusOnGimmick", "FireTemple"},
    {"FocusOnGimmick_FireTemple_FlareDancer02", "FireTemple"},
    {"ForestTemple_Room04_OpenDoor_AppearTBox", "ForestTemple"},
    {"ForestTemple_Room17_TBoxAppear_CandleLight_ShutterOpen", "ForestTemple"},
    {"ForestTemple_Room22_ShutterOpen_TBoxAppear", "ForestTemple"},
    {"ForestTemple_Room25_CandleLight_ShutterOpen", "ForestTemple"},
    {"ForestTemple_Room27_ShutterOpen", "ForestTemple"},
    {"GerudoTrainingGround_Room07_TreasureAppear", "GerudoTrainingGround"},
    {"InsideGanonCastle_Dungeon_AppaerTreasureBox_12", "InsideGanonCastle"},
    {"ShadowTemple_04_EnemyAnnihilation_Clear", "ShadowTemple"},
    {"SpiritTemple_Room19_SunSwitchShutterOpen", "SpiritTemple"},
    {"SpritTemple_Room03_OpenShutter", "SpiritTemple"},
    {"SpritTemple_Room08_OpenShutter", "SpiritTemple"},
    {"SpritTemple_Room12_OpenShutter", "SpiritTemple"},
    {"SpritTemple_Room14_OpenShutter", "SpiritTemple"},
    {"SpritTemple_Room22_OpenShutter", "SpiritTemple"},
    {"SpritTemple_Room23_StepSwitch", "SpiritTemple"},
    {"SpritTemple_Room23_SunSwitch", "SpiritTemple"},
    {"SpritTemple_Room25_OpenShutter", "SpiritTemple"},
    {"SpritTemple_Room31_OpenShutter", "SpiritTemple"},
    {"WaterTemple_FocusonGimmick_02", "WaterTemple"},
};

// Generated by the same run (// kPlays, row 165): an event flow's EventPlayAS on a room actor whose Logic_Delete reads
// the event's played state (its room deletes it, and makes its end actor, only at load).
constexpr Play kPlays[] = {
    {"Dm_DodongoCave_0010", "DodongoCavern", "Obj_DodongoCave_BrokenStair_A_01", {-49.0f, 103.7f, -43.5f}, "Obj_DodongoCave_BrokenStair_A_01_Demo_Anim"},
    {"Dm_ForestTemple_0014", "ForestTemple", "Obj_ForestTemple_MovableBox_A_01", {58.2f, 103.9f, 0.8f}, "C01_A_01"},
    {"Dm_ForestTemple_0014", "ForestTemple", "Obj_ForestTemple_MovableBox_A_02", {59.8f, 103.9f, -0.8f}, "C01_A_02"},
    {"Dm_ForestTemple_0014", "ForestTemple", "Obj_ForestTemple_MovableBox_A_03", {59.8f, 103.9f, 0.8f}, "C01_A_03"},
    {"Dm_ForestTemple_0014", "ForestTemple", "Obj_ForestTemple_MovableBox_A_04", {58.2f, 103.9f, -0.8f}, "C01_A_04"},
    {"Dm_InsideGanonCastle_0035", "InsideGanonCastle", "Obj_LiftRock_C_01", {-15.2f, 103.9f, -48.3f}, "ToLift_0035"},
};

constexpr Guard kGuards[] = {
    {"DodongoCavern_CandleStick", "DodongoCavern", "DodongoCavern_Gimmick_CandleStickShutter"},
    {"FireTemple_Room03_EventHasPlayed", "FireTemple", "FocusOnGimmick_FireTemple_FlareDancer02"},
    {"ForestTemple_Room_13_AppearTreasureChest", "ForestTemple", "ForestTemple_Room17_TBoxAppear_CandleLight_ShutterOpen"},
    {"ForestTemple_Room_18_AppearTreasureChest", "ForestTemple", "ForestTemple_Room22_ShutterOpen_TBoxAppear"},
    {"GerudoTrainingGround_Dungeon_Room05_DefeatEnemy", "GerudoTrainingGround", "GerudoTrainingGround_05_AppaerTreasureBox"},
    {"GerudoTrainingGround_Dungeon_Room07_DefeatEnemy", "GerudoTrainingGround", "GerudoTrainingGround_Room07_TreasureAppear"},
    {"Gimmick_DefeatEnemy_Open_DodongoCave_01", "DodongoCavern", "DodongoCavern_Gimmick_DefeatLizalfos01"},
    {"Gimmick_DefeatEnemy_Open_DodongoCave_02", "DodongoCavern", "DodongoCavern_Gimmick_DefeatLizalfos02"},
    {"Gimmick_DefeatEnemy_Open_FireTemple_02_CounterOff", "FireTemple", "FireTemple_15_OpenShutter"},
    {"Gimmick_DefeatEnemy_Open_ForestTemple_01", "ForestTemple", "ForestTemple_Room04_OpenDoor_AppearTBox"},
    {"HavePlayedEvent.Dm_DodongoCave_0010", "DodongoCavern", "Dm_DodongoCave_0010"},
    {"HavePlayedEvent.Dm_InsideGanonCastle_0035", "InsideGanonCastle", "Dm_InsideGanonCastle_0035"},
    {"IceCavern_Room05_Unlocked", "IceCavern", "IceCavern_Room05_SilverToken_Unlock"},
    {"IceCavern_Room06_Unlocked", "IceCavern", "IceCavern_Room06_SilverToken_Unlock"},
    {"InsideGanonCastle_Dungeon_Room12_DefeatEnemy", "InsideGanonCastle", "InsideGanonCastle_Dungeon_AppaerTreasureBox_12"},
    {"InsideJabuJabuBelly_Room14_PlayedSwitchEvent_Lower", "InsideJabuJabuBelly", "InsideJabuJabuBellyRoom14StepSwitch_Under"},
    {"InsideJabuJabuBelly_Room14_PlayedSwitchEvent_Upper", "InsideJabuJabuBelly", "InsideJabuJabuBellyRoom14StepSwitch"},
    {"InsideTheDekuTree_RoomC_Cleared", "InsideTheDekuTree", "InsideTheDekuTree_Gimmick_FallingLadder"},
    {"ShadowTemple_RoomB1FD_Cleared", "ShadowTemple", "ShadowTemple_04_EnemyAnnihilation_Clear"},
    {"SpiritTemple_Room08_Unlocked_00", "SpiritTemple", "SpiritTemple_Level_Room_08_SwitchOn_01"},
    {"WaterTemple_Room02_00", "WaterTemple", "WaterTemple_FocusonGimmick_02"},
    {"WaterTemple_Room22_Cleared", "WaterTemple", "WaterTempleRoom22Clear"},
};

// Generated by the same run (// kTriggers): what another game can raise on an asker's trigger, both classes (248
// lines: 9 shared flags, 19 keys (17 silver rupee counters, 2 ice blocks), 85 silver rupee tokens, 113 kills, 22
// followed world objects (3 hammer switches, a hammer pillar, a block's stop, 4 corners of the Forest Temple's box
// puzzle, 13 blocks, statues and rolling rocks in a press switch's room)).
constexpr Trigger kTriggers[] = {
    {Trigger::flag, "ForestTemple", "Dm_ForestTemple_0014", "ForestTemple_AmyPuzzleBoxCleard", 0x0000000000000000, 0x0000000000000000},  // a shared flag
    {Trigger::flag, "ForestTemple", "ForestTemple_Room17_ShutterOpen", "ForestTemple_After_Appearance_TreasureChest", 0x0000000000000000, 0x0000000000000000},  // a shared flag
    {Trigger::flag, "ForestTemple", "ForestTemple_Room25_CandleLight_ShutterOpen", "ForestTemple_DefeatAmy", 0x0000000000000000, 0x0000000000000000},  // a shared flag
    {Trigger::flag, "InsideJabuJabuBelly", "InsideJabuJabuBellyRoom14StepSwitch", "Gimmick_Insidejabujabu_Updown_MoveWater", 0x0000000000000000, 0x0000000000000000},  // a shared flag
    {Trigger::flag, "ShadowTemple", "ShadowTemple_01_BeakStatue_Clear", "ShadowTemple_Room_01_GimmickClear", 0x0000000000000000, 0x0000000000000000},  // a shared flag
    {Trigger::flag, "ShadowTemple", "ShadowTemple_04_EnemyAnnihilation_Clear", "ShadowTemple_Level_Room_B1F_D_Defeat_DeadHand", 0x0000000000000000, 0x0000000000000000},  // a shared flag
    {Trigger::flag, "ShadowTemple", "ShadowTemple_21_EnemyAnnihilation_Clear", "ShadowTemple_RoomB3FB4FL_DefeatFloormaster_Truth", 0x0000000000000000, 0x0000000000000000},  // a shared flag
    {Trigger::flag, "SpiritTemple", "SpritTemple_Room04_OpenShutter", "SpiritTemple_Room04_Unlocked", 0x0000000000000000, 0x0000000000000000},  // a shared flag
    {Trigger::flag, "SpiritTemple", "SpritTemple_Room22_OpenShutter", "SpiritTemple_Room22_Unlocked_01", 0x0000000000000000, 0x0000000000000000},  // a shared flag
    {Trigger::follow, "DodongoCavern", "DodongoCavern_Gimmick_StatueSwitch", "", 0x56b7b1d67ffe622b, 0x0000000000000000},  // Obj_AmosStatue_A_01 may rest on Obj_Stepswitch_A_03
    {Trigger::follow, "DodongoCavern", "DodongoCavern_Gimmick_StatueSwitch", "", 0x9b4a08419de081f6, 0x0000000000000000},  // Obj_AmosStatue_A_01 may rest on Obj_Stepswitch_A_03
    {Trigger::follow, "DodongoCavern", "DodongoCavern_Gimmick_SwitchShutter_Room12", "", 0x8342f68da2a61311, 0x0000000000000000},  // Obj_AmosStatue_A_01 may rest on Obj_StepSwitch_A_01
    {Trigger::follow, "DodongoCavern", "DodongoCavern_Gimmick_SwitchShutter_Room19", "", 0x0f6a348ed7daaef4, 0x0000000000000000},  // Obj_MovableBox_A_01 may rest on Obj_Stepswitch_A_03
    {Trigger::follow, "DodongoCavern", "DodongoCavern_Gimmick_SwitchShutter_Room19", "", 0x244fd0ef8c5e5e01, 0x0000000000000000},  // Obj_MovableBox_A_01 may rest on Obj_Stepswitch_A_03
    {Trigger::follow, "DodongoCavern", "DodongoCavern_Gimmick_SwitchShutter_Room19", "", 0x641bc828409e0c24, 0x0000000000000000},  // Obj_MovableBox_A_01 may rest on Obj_Stepswitch_A_03
    {Trigger::follow, "DodongoCavern", "DodongoCavern_Gimmick_SwitchShutter_Room19", "", 0xfc81551012767634, 0x0000000000000000},  // Obj_MovableBox_A_01 may rest on Obj_Stepswitch_A_03
    {Trigger::follow, "FireTemple", "Dm_FireTemple_0029", "", 0x036315ba2c3705b6, 0x0000000000000000},  // Obj_HammerSwitch_A_01: another game's move or fall of it
    {Trigger::follow, "FireTemple", "FireTemple_LargePillarFallCamera", "", 0x717cde568e9a8c5f, 0x0000000000000000},  // Obj_FireTemple_HammerDropPillar_A_01: another game's move or fall of it
    {Trigger::follow, "FireTemple", "FireTemple_PutOutFireWallTrap", "", 0x28d55cfe2ef60933, 0x0000000000000000},  // Obj_Firetemple_RollingRock_A_01 may rest on Obj_Stepswitch_A_03
    {Trigger::follow, "FireTemple", "FireTemple_PutOutFireWallTrap", "", 0x724c15f7f2c88c3b, 0x0000000000000000},  // Obj_Firetemple_RollingRock_A_01 may rest on Obj_Stepswitch_A_03
    {Trigger::follow, "FireTemple", "FireTemple_PutOutFireWallTrap", "", 0xaa778b8191681ec3, 0x0000000000000000},  // Obj_Firetemple_RollingRock_A_01 may rest on Obj_Stepswitch_A_03
    {Trigger::follow, "ForestTemple", "Dm_ForestTemple_0014", "", 0x1f0b64dac1bb1499, 0x55ad860a60c20c12},  // Obj_ForestTemple_MovableBox_A_04, a block ForestTemple_MovableBox_Puzzle_Manager links
    {Trigger::follow, "ForestTemple", "Dm_ForestTemple_0014", "", 0x794f8e0396597b84, 0x55ad860a60c20c12},  // Obj_ForestTemple_MovableBox_A_02, a block ForestTemple_MovableBox_Puzzle_Manager links
    {Trigger::follow, "ForestTemple", "Dm_ForestTemple_0014", "", 0x9340d4ba68f5fecc, 0x55ad860a60c20c12},  // Obj_ForestTemple_MovableBox_A_01, a block ForestTemple_MovableBox_Puzzle_Manager links
    {Trigger::follow, "ForestTemple", "Dm_ForestTemple_0014", "", 0xebddc3cef6e36708, 0x55ad860a60c20c12},  // Obj_ForestTemple_MovableBox_A_03, a block ForestTemple_MovableBox_Puzzle_Manager links
    {Trigger::follow, "InsideGanonCastle", "InsideGanonCastle_Dungeon_ShutterOpen_06_01", "", 0xae973b173365bf20, 0x0000000000000000},  // Obj_HammerSwitch_A_01: another game's move or fall of it
    {Trigger::follow, "InsideTheDekuTree", "InsideTheDekuTree_Gimmick_CandleAndSpiderWeb", "", 0x83a1177e914c9c03, 0x0000000000000000},  // Obj_MovableBox_A_01 may rest on Obj_StepSwitch_A_05
    {Trigger::follow, "SpiritTemple", "SpiritTemple_Level_Room_08_SwitchOn_01", "", 0x2dd466a59f90320e, 0x0000000000000000},  // Obj_AmosStatue_A_01 may rest on Obj_Stepswitch_A_03
    {Trigger::follow, "SpiritTemple", "SpiritTemple_Level_Room_10_SwitchOn_01", "", 0xfd9f60054b5abf0c, 0x0000000000000000},  // Obj_SpiritTemple_MovableBox_A_01: another game's move or fall of it
    {Trigger::follow, "SpiritTemple", "SpritTemple_Room08_OpenShutter", "", 0xcb61a9de736c9e1c, 0x0000000000000000},  // Obj_HammerSwitch_A_01: another game's move or fall of it
    {Trigger::follow, "WaterTemple", "WaterTempleRoom14StepSwitch", "", 0xce77a8f0da18626c, 0x0000000000000000},  // Obj_MovableBox_A_05 may rest on Obj_WaterTemple_Stepswitch_A_03
    {Trigger::key, "BottomOfTheWell", "BottomOfTheWell_SilverToken_Unlock", "", 0xe6b8841ed81f8e60, 0x0000000000000000},  // Obj_silverRupeeCounter's own key
    {Trigger::key, "ForestTemple", "ForestTemple_Room22_IceMelts", "", 0xf13da6346db2aa7a, 0x0000000000000000},  // Obj_Ice_A_01's key
    {Trigger::key, "ForestTemple", "ForestTemple_Room27_IceMelts", "", 0x62c96cba10a9da4e, 0x0000000000000000},  // Obj_Ice_A_01's key
    {Trigger::key, "GerudoTrainingGround", "GerudoTrainingGround_02From03_SilverToken_Unlock", "", 0x583974f20da1c7ca, 0x0000000000000000},  // Obj_silverRupeeCounter's own key
    {Trigger::key, "GerudoTrainingGround", "GerudoTrainingGround_02_SilverToken_Unlock", "", 0xb1f683aa91daf209, 0x0000000000000000},  // Obj_silverRupeeCounter's own key
    {Trigger::key, "GerudoTrainingGround", "GerudoTrainingGround_06_SilverToken_Unlock", "", 0xf6280b38b3266c26, 0x0000000000000000},  // Obj_silverRupeeCounter's own key
    {Trigger::key, "GerudoTrainingGround", "GerudoTrainingGround_09_SilverToken_Unlock", "", 0x12dfff252bc20a2f, 0x0000000000000000},  // Obj_silverRupeeCounter's own key
    {Trigger::key, "IceCavern", "IceCavern_Room05_SilverToken_Unlock", "", 0xaf0a7a17099f8d19, 0x0000000000000000},  // Obj_silverRupeeCounter_03's own key
    {Trigger::key, "IceCavern", "IceCavern_Room06_SilverToken_Unlock", "", 0x8513f43d7c5488e0, 0x0000000000000000},  // Obj_silverRupeeCounter_05's own key
    {Trigger::key, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room03", "", 0xe783f8096d7f4900, 0x0000000000000000},  // Obj_silverRupeeCounter's own key
    {Trigger::key, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room10", "", 0x2047042424da1d32, 0x0000000000000000},  // Obj_silverRupeeCounter's own key
    {Trigger::key, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room14", "", 0xf813ac43a8b36922, 0x0000000000000000},  // Obj_silverRupeeCounter's own key
    {Trigger::key, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room16", "", 0x0e8fe67f2c220c0a, 0x0000000000000000},  // Obj_silverRupeeCounter's own key
    {Trigger::key, "ShadowTemple", "ShadowTemple_Room06_SilverToken_Unlock", "", 0x25766be26134a2b8, 0x0000000000000000},  // Obj_silverRupeeCounter_04's own key
    {Trigger::key, "ShadowTemple", "ShadowTemple_Room09_SilverToken_Unlock", "", 0x253ba095a3818d6c, 0x0000000000000000},  // Obj_silverRupeeCounter_04's own key
    {Trigger::key, "ShadowTemple", "ShadowTemple_Room12_SilverToken_Unlock", "", 0x75980ea65b71139c, 0x0000000000000000},  // Obj_silverRupeeCounter's own key
    {Trigger::key, "SpiritTemple", "SpiritTemple_Level_Room_05_SilverToken_Unlock", "", 0x496dc2e67fd48379, 0x0000000000000000},  // Obj_silverRupeeCounter_06's own key
    {Trigger::key, "SpiritTemple", "SpiritTemple_Level_Room_10_SilverToken_Unlock", "", 0xf32a750e44c14f93, 0x0000000000000000},  // Obj_silverRupeeCounter_02's own key
    {Trigger::key, "SpiritTemple", "SpiritTemple_Level_Room_16_SilverToken_Unlock", "", 0x8e7e2c616d6f8711, 0x0000000000000000},  // Obj_silverRupeeCounter_02's own key
    {Trigger::kill, "BottomOfTheWell", "BottomOfTheWell_08_Enemy_WipedOut", "", 0x95c6281cedf26cef, 0x0000000000000000},  // Enemy_DeadHand.Dead
    {Trigger::kill, "DodongoCavern", "DodongoCavern_Gimmick_DefeatLizalfos01", "", 0x01fa0074fddbc182, 0x0000000000000000},  // Enemy_MiddleBoss_Lizalfos.Dead
    {Trigger::kill, "DodongoCavern", "DodongoCavern_Gimmick_DefeatLizalfos01", "", 0x9cbd9c524b24df87, 0x0000000000000000},  // Enemy_MiddleBoss_Lizalfos.Dead
    {Trigger::kill, "DodongoCavern", "DodongoCavern_Gimmick_DefeatLizalfos02", "", 0x155ce1b2ba5028cf, 0x0000000000000000},  // Enemy_MiddleBoss_Lizalfos.Dead
    {Trigger::kill, "DodongoCavern", "DodongoCavern_Gimmick_DefeatLizalfos02", "", 0x23456e4b5b005315, 0x0000000000000000},  // Enemy_MiddleBoss_Lizalfos.Dead
    {Trigger::kill, "FireTemple", "FireTemple_15_OpenShutter", "", 0x524d8f690c11944a, 0x0000000000000000},  // Enemy_TorchSlug.Dead
    {Trigger::kill, "FireTemple", "FireTemple_15_OpenShutter", "", 0x7955892af6fcf0a2, 0x0000000000000000},  // Enemy_TorchSlug.Dead
    {Trigger::kill, "FireTemple", "FireTemple_15_OpenShutter", "", 0x8515bd766b65e010, 0x0000000000000000},  // Enemy_TorchSlug.Dead
    {Trigger::kill, "FireTemple", "FireTemple_15_OpenShutter", "", 0xa36a08d01e93953c, 0x0000000000000000},  // Enemy_TorchSlug.Dead
    {Trigger::kill, "FireTemple", "FireTemple_15_OpenShutter", "", 0xf8119818b5e22a95, 0x0000000000000000},  // Enemy_TorchSlug.Dead
    {Trigger::kill, "FireTemple", "FireTemple_15_OpenShutter", "", 0xfc367bdaeb7524ef, 0x0000000000000000},  // Enemy_TorchSlug.Dead
    {Trigger::kill, "ForestTemple", "ForestTemple_Room04_OpenDoor_AppearTBox", "", 0x16bf797c856ddcc9, 0x0000000000000000},  // Enemy_Stalfos_Weak.Dead
    {Trigger::kill, "ForestTemple", "ForestTemple_Room04_OpenDoor_AppearTBox", "", 0x921691d2cb1ba63b, 0x0000000000000000},  // Enemy_Stalfos_Weak.Dead
    {Trigger::kill, "ForestTemple", "ForestTemple_Room17_BigTBoxAppear", "", 0x551f0ba3582714ea, 0x0000000000000000},  // Enemy_Stalfos_Weak.Dead
    {Trigger::kill, "ForestTemple", "ForestTemple_Room17_BigTBoxAppear", "", 0xd8eddfbe6a1d8c7b, 0x0000000000000000},  // Enemy_Stalfos_Weak.Dead
    {Trigger::kill, "ForestTemple", "ForestTemple_Room17_ShutterOpen", "", 0x551f0ba3582714ea, 0x0000000000000000},  // Enemy_Stalfos_Weak.Dead
    {Trigger::kill, "ForestTemple", "ForestTemple_Room17_ShutterOpen", "", 0xd8eddfbe6a1d8c7b, 0x0000000000000000},  // Enemy_Stalfos_Weak.Dead
    {Trigger::kill, "ForestTemple", "ForestTemple_Room17_TBoxAppear_CandleLight", "", 0x124c4eb93f9f58b0, 0x0000000000000000},  // Enemy_Poe_Joelle.Dead
    {Trigger::kill, "ForestTemple", "ForestTemple_Room17_TBoxAppear_CandleLight_ShutterOpen", "", 0x137683de37a18db6, 0x0000000000000000},  // Enemy_Poe_Beth.Dead
    {Trigger::kill, "ForestTemple", "ForestTemple_Room17_TwoStalfosAppear", "", 0xeb8b927a5e5f64dc, 0x0000000000000000},  // Enemy_Stalfos_Weak.Dead
    {Trigger::kill, "ForestTemple", "ForestTemple_Room22_ShutterOpen_TBoxAppear", "", 0xd43b64999c272d7c, 0x0000000000000000},  // Enemy_Floormaster.Dead
    {Trigger::kill, "ForestTemple", "ForestTemple_Room25_CandleLight_ShutterOpen", "", 0x0990b181d8f4b622, 0x0000000000000000},  // Enemy_Poe_Amy.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_01_AppearTreasureBox_From00", "", 0x616fc451aa0773c2, 0x0000000000000000},  // Enemy_Stalfos.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_01_AppearTreasureBox_From00", "", 0xaa5bb8f42e25b8ef, 0x0000000000000000},  // Enemy_Stalfos.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_01_AppearTreasureBox_From00", "", 0xae9492a5cba255ac, 0x0000000000000000},  // Enemy_Stalfos.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_01_AppearTreasureBox_From00", "", 0xfc0e11f3bbc80a83, 0x0000000000000000},  // Enemy_Stalfos.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_03_AppaerTreasureBox", "", 0x179c11d66d11584d, 0x0000000000000000},  // Enemy_Wolfos_White.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_03_AppaerTreasureBox", "", 0x4701ac950da685ba, 0x0000000000000000},  // Enemy_Wolfos.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_03_AppaerTreasureBox", "", 0x5c68565daaa00f8f, 0x0000000000000000},  // Enemy_Wolfos.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_03_AppaerTreasureBox", "", 0x75ea604bc3edb8b3, 0x0000000000000000},  // Enemy_Wolfos_White.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_03_AppaerTreasureBox", "", 0x7c4da02c124c27df, 0x0000000000000000},  // Enemy_Wolfos_White.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_03_AppaerTreasureBox", "", 0x885322e72c3749e9, 0x0000000000000000},  // Enemy_Wolfos.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_03_AppaerTreasureBox", "", 0xeb851e9f9e513d1a, 0x0000000000000000},  // Enemy_Wolfos_White.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_03_AppaerTreasureBox", "", 0xeeec48c556250c8d, 0x0000000000000000},  // Enemy_Wolfos.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_05_AppaerTreasureBox", "", 0x5dc030aa22a9ffa0, 0x0000000000000000},  // Enemy_TorchSlug.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_05_AppaerTreasureBox", "", 0xabf57f930bd44998, 0x0000000000000000},  // Enemy_Keese_Fire.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_05_AppaerTreasureBox", "", 0xc163e64f2d60393b, 0x0000000000000000},  // Enemy_TorchSlug.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_05_AppaerTreasureBox", "", 0xebd23f380f965bb7, 0x0000000000000000},  // Enemy_Keese_Fire.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_10_AppearTreasureBox", "", 0x04395edbb6af96a9, 0x0000000000000000},  // Enemy_LikeLike.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_10_AppearTreasureBox", "", 0xaa2d82a821884e6e, 0x0000000000000000},  // Enemy_LikeLike.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_10_AppearTreasureBox", "", 0xdfee12fb90c7343a, 0x0000000000000000},  // Enemy_LikeLike.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_Room07_TreasureAppear", "", 0x27cbb357d566f0a0, 0x0000000000000000},  // Enemy_Beamos.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_Room07_TreasureAppear", "", 0xa6aa0af23fc39f5a, 0x0000000000000000},  // Enemy_Dinolfos.Dead
    {Trigger::kill, "GerudoTrainingGround", "GerudoTrainingGround_Room07_TreasureAppear", "", 0xad051e44b41ff806, 0x0000000000000000},  // Enemy_Dinolfos.Dead
    {Trigger::kill, "IceCavern", "IceCavern_Gimmick_BlueFireShowup", "", 0xc8c52390f810590b, 0x0000000000000000},  // Obj_BlueFirePedestalLid_A_01.Dead
    {Trigger::kill, "IceCavern", "IceCavern_Room01_DefeatEnemy", "", 0x0d91c16e19a8d11e, 0x0000000000000000},  // Enemy_Freezard_Fixed.Dead
    {Trigger::kill, "IceCavern", "IceCavern_Room01_DefeatEnemy", "", 0x3c5e8c2264345204, 0x0000000000000000},  // Enemy_Freezard_Fixed.Dead
    {Trigger::kill, "IceCavern", "IceCavern_Room01_DefeatEnemy", "", 0x7e9ca9e397557518, 0x0000000000000000},  // Enemy_Freezard_Fixed.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_DefeatEnemy", "", 0x7630f82c1de2b52e, 0x0000000000000000},  // Enemy_Dinolfos.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_DefeatEnemy", "", 0xf4fa6f881116cdf3, 0x0000000000000000},  // Enemy_Dinolfos.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_DefeatEnemy_02", "", 0x42e984907f639cad, 0x0000000000000000},  // Enemy_Stalfos.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_DefeatEnemy_02", "", 0x667f356682c6155a, 0x0000000000000000},  // Enemy_Stalfos.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_DefeatEnemy_03", "", 0x489228920fa0060b, 0x0000000000000000},  // Enemy_IronKnuckle_White.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_DefeatEnemy_03", "", 0x6b81391bef01efe8, 0x0000000000000000},  // Enemy_IronKnuckle_Black.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_Dungeon_AppaerTreasureBox", "", 0x16ad0a48fa885da2, 0x0000000000000000},  // Enemy_TorchSlug.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_Dungeon_AppaerTreasureBox", "", 0x2dbdca0357821c33, 0x0000000000000000},  // Enemy_TorchSlug.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_Dungeon_AppaerTreasureBox_02", "", 0x57232133bebcbd40, 0x0000000000000000},  // Enemy_Wolfos.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_Dungeon_AppaerTreasureBox_02", "", 0xd8a6681ce4c4b4ea, 0x0000000000000000},  // Enemy_Wolfos.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_Dungeon_AppaerTreasureBox_12", "", 0x25df4046561d2f54, 0x0000000000000000},  // Enemy_Skulltula_Truth.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_Dungeon_AppaerTreasureBox_12", "", 0x6c076e3bb1614d72, 0x0000000000000000},  // Enemy_Keese_Truth.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_Dungeon_AppaerTreasureBox_12", "", 0x732049a8a14ceb24, 0x0000000000000000},  // Enemy_Keese_Truth.Dead
    {Trigger::kill, "InsideGanonCastle", "InsideGanonCastle_Dungeon_AppaerTreasureBox_12", "", 0xb846716cb9424adf, 0x0000000000000000},  // Enemy_Keese_Truth.Dead
    {Trigger::kill, "InsideJabuJabuBelly", "FocusGimmick_RedTentacleGimmick0", "", 0x15e6223d49a03d63, 0x0000000000000000},  // Enemy_Tentacle_Red.IsDeadNotDelay
    {Trigger::kill, "InsideJabuJabuBelly", "InsideJabuJabuBelly_Room12_TreasureBoxAppear", "", 0x21fcd12aca67cc22, 0x0000000000000000},  // Enemy_Shabom_AlwaysRespawn.Dead
    {Trigger::kill, "InsideJabuJabuBelly", "InsideJabuJabuBelly_Room12_TreasureBoxAppear", "", 0x33edf14d7e19adc1, 0x0000000000000000},  // Enemy_Shabom_AlwaysRespawn.Dead
    {Trigger::kill, "InsideJabuJabuBelly", "InsideJabuJabuBelly_Room12_TreasureBoxAppear", "", 0x3716f5d5afc4aee3, 0x0000000000000000},  // Enemy_Shabom_AlwaysRespawn.Dead
    {Trigger::kill, "InsideJabuJabuBelly", "InsideJabuJabuBelly_Room12_TreasureBoxAppear", "", 0x3fd610e2262b5ab9, 0x0000000000000000},  // Enemy_Shabom_AlwaysRespawn.Dead
    {Trigger::kill, "InsideJabuJabuBelly", "InsideJabuJabuBelly_Room12_TreasureBoxAppear", "", 0x42633b45e3e539be, 0x0000000000000000},  // Enemy_Shabom_AlwaysRespawn.Dead
    {Trigger::kill, "InsideJabuJabuBelly", "InsideJabuJabuBelly_Room12_TreasureBoxAppear", "", 0x6793e7a54ec99307, 0x0000000000000000},  // Enemy_Shabom_AlwaysRespawn.Dead
    {Trigger::kill, "InsideJabuJabuBelly", "InsideJabuJabuBelly_Room12_TreasureBoxAppear", "", 0xb4b11a85bb200f01, 0x0000000000000000},  // Enemy_Shabom_AlwaysRespawn.Dead
    {Trigger::kill, "InsideJabuJabuBelly", "InsideJabuJabuBelly_Room12_TreasureBoxAppear", "", 0xcdaa8e29999a2b15, 0x0000000000000000},  // Enemy_Shabom_AlwaysRespawn.Dead
    {Trigger::kill, "InsideJabuJabuBelly", "InsideJabuJabuBelly_Room12_TreasureBoxAppear", "", 0xf1f846db3b3ddf73, 0x0000000000000000},  // Enemy_Shabom_AlwaysRespawn.Dead
    {Trigger::kill, "InsideJabuJabuBelly", "InsideJabuJabuBelly_TreasureBoxAppear", "", 0x3810f14b8e15a6c1, 0x0000000000000000},  // Enemy_Stinger.Dead
    {Trigger::kill, "InsideJabuJabuBelly", "InsideJabuJabuBelly_TreasureBoxAppear", "", 0x472e145ca0c9f529, 0x0000000000000000},  // Enemy_Stinger.Dead
    {Trigger::kill, "InsideJabuJabuBelly", "InsideJabuJabuBelly_TreasureBoxAppear", "", 0xc496d4373a9d7f3e, 0x0000000000000000},  // Enemy_Stinger.Dead
    {Trigger::kill, "InsideJabuJabuBelly", "InsideJabuJabuBelly_TreasureBoxAppear", "", 0xe654ab3bfc05c0d9, 0x0000000000000000},  // Enemy_Stinger.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_03_EnemyAnnihilation_Clear", "", 0x1eb355d5ec718e75, 0x0000000000000000},  // Enemy_Keese.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_03_EnemyAnnihilation_Clear", "", 0x72ae1363f5f734fa, 0x0000000000000000},  // Enemy_ReDead.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_03_EnemyAnnihilation_Clear", "", 0xfe14030826f02f2e, 0x0000000000000000},  // Enemy_Keese.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_07_EnemyAnnihilation_Clear", "", 0x4093cb0e670d2c01, 0x0000000000000000},  // Enemy_Gibdo.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_07_EnemyAnnihilation_Clear", "", 0x9e3d5b6579caa457, 0x0000000000000000},  // Enemy_Gibdo.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_10_EnemyAnnihilation_Clear", "", 0x1c2e21b8307eaa25, 0x0000000000000000},  // Enemy_Keese_Fire.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_10_EnemyAnnihilation_Clear", "", 0xb8f919610656d262, 0x0000000000000000},  // Enemy_LikeLike.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_10_EnemyAnnihilation_Clear", "", 0xd6936c470ce10426, 0x0000000000000000},  // Enemy_Keese_Fire.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_10_EnemyAnnihilation_Clear", "", 0xe054114ca68f1a49, 0x0000000000000000},  // Enemy_Keese_Fire.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_13_EnemyAnnihilation_DOWN_Clear", "", 0x687d9f8fc2a66530, 0x0000000000000000},  // Enemy_Keese.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_13_EnemyAnnihilation_DOWN_Clear", "", 0x6a094e28f9f1261d, 0x0000000000000000},  // Enemy_Keese.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_13_EnemyAnnihilation_DOWN_Clear", "", 0xdaf60389b03332ec, 0x0000000000000000},  // Enemy_Keese_Fire.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_13_EnemyAnnihilation_DOWN_Clear", "", 0xeaee25f9c22524f5, 0x0000000000000000},  // Enemy_Keese_Fire.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_13_EnemyAnnihilation_UP_Clear", "", 0x687d9f8fc2a66530, 0x0000000000000000},  // Enemy_Keese.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_13_EnemyAnnihilation_UP_Clear", "", 0x6a094e28f9f1261d, 0x0000000000000000},  // Enemy_Keese.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_13_EnemyAnnihilation_UP_Clear", "", 0xdaf60389b03332ec, 0x0000000000000000},  // Enemy_Keese_Fire.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_13_EnemyAnnihilation_UP_Clear", "", 0xeaee25f9c22524f5, 0x0000000000000000},  // Enemy_Keese_Fire.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_15_EnemyAnnihilation_Clear", "", 0x08ba8b12d7df10cb, 0x0000000000000000},  // Enemy_ReDead.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_15_EnemyAnnihilation_Clear", "", 0x440a59367dbd98d3, 0x0000000000000000},  // Enemy_ReDead.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_16_EnemyAnnihilation_Clear", "", 0x9d5b52febbc87f14, 0x0000000000000000},  // Enemy_Gibdo.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_16_EnemyAnnihilation_Clear", "", 0xe212915b8d50fcc7, 0x0000000000000000},  // Enemy_Gibdo.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_19_EnemyAnnihilation_Clear", "", 0x10596beb095df717, 0x0000000000000000},  // Obj_ShadowTemple_SpikeBoard_A_01.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_19_EnemyAnnihilation_Clear", "", 0x11c786951151434d, 0x0000000000000000},  // Obj_ShadowTemple_SpikeBoard_A_01.Dead
    {Trigger::kill, "ShadowTemple", "ShadowTemple_Gimmick_Focus_LargeJar_Key", "", 0xedb79e4006511277, 0x0000000000000000},  // Obj_ShadowTemple_SkullPot_A_01.Dead
    {Trigger::kill, "SpiritTemple", "SpiritTemple_Level_Room_07_SunSwitchOn", "", 0x7d19595a56ab39af, 0x0000000000000000},  // Obj_SpiritTemple_CrackWall_A_01.Dead
    {Trigger::kill, "SpiritTemple", "SpritTemple_Room03_OpenShutter", "", 0x0dd2a1950388164c, 0x0000000000000000},  // Enemy_Keese_Fire.Dead
    {Trigger::kill, "SpiritTemple", "SpritTemple_Room03_OpenShutter", "", 0x1dd1b5f2fdfa4ebe, 0x0000000000000000},  // Enemy_Amos.Dead
    {Trigger::kill, "SpiritTemple", "SpritTemple_Room03_OpenShutter", "", 0xaebf6a6bacaad2bd, 0x0000000000000000},  // Enemy_Keese_Fire.Dead
    {Trigger::kill, "SpiritTemple", "SpritTemple_Room03_OpenShutter", "", 0xb3c932fa61b390d5, 0x0000000000000000},  // Enemy_Keese.Dead
    {Trigger::kill, "SpiritTemple", "SpritTemple_Room03_OpenShutter", "", 0xd6a640e6a41d6bfd, 0x0000000000000000},  // Enemy_Keese.Dead
    {Trigger::kill, "SpiritTemple", "SpritTemple_Room12_OpenShutter", "", 0x6e0f7c7c3a1593c4, 0x0000000000000000},  // Enemy_IronKnuckle_Silver.Dead
    {Trigger::kill, "SpiritTemple", "SpritTemple_Room22_OpenShutter", "", 0xd182cfa4dd559382, 0x0000000000000000},  // Enemy_Beamos.Dead
    {Trigger::kill, "SpiritTemple", "SpritTemple_Room25_OpenShutter", "", 0xb3407d11a768ae6c, 0x0000000000000000},  // Enemy_IronKnuckle_Silver.Dead
    {Trigger::kill, "WaterTemple", "WaterTempleRoom22Clear", "", 0x4974202072beaec6, 0x0000000000000000},  // Enemy_MiddleBoss_DarkLink.IsDeathEventPlay
    {Trigger::kill, "WaterTemple", "WaterTemple_FocusGimmick_Room18", "", 0x2ee9779f4f50fbb0, 0x0000000000000000},  // Enemy_Spike.Dead
    {Trigger::kill, "WaterTemple", "WaterTemple_FocusGimmick_Room18", "", 0x6a94d24eb4fdb673, 0x0000000000000000},  // Enemy_Spike.Dead
    {Trigger::kill, "WaterTemple", "WaterTemple_FocusGimmick_Room18", "", 0x9aadac42596b8380, 0x0000000000000000},  // Enemy_Spike.Dead
    {Trigger::token, "BottomOfTheWell", "BottomOfTheWell_SilverToken_Unlock", "", 0x688ba7ae82f7d174, 0xe6b8841ed81f8e60},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "BottomOfTheWell", "BottomOfTheWell_SilverToken_Unlock", "", 0x7e5292413b721c0a, 0xe6b8841ed81f8e60},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "BottomOfTheWell", "BottomOfTheWell_SilverToken_Unlock", "", 0xc7ac3c87b8dc3044, 0xe6b8841ed81f8e60},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "BottomOfTheWell", "BottomOfTheWell_SilverToken_Unlock", "", 0xd3fca295920aa56f, 0xe6b8841ed81f8e60},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "BottomOfTheWell", "BottomOfTheWell_SilverToken_Unlock", "", 0xda81b3878c1deae3, 0xe6b8841ed81f8e60},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_02From03_SilverToken_Unlock", "", 0x8912e1b8bd3d49dc, 0x583974f20da1c7ca},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_02From03_SilverToken_Unlock", "", 0x97634124f088e7d8, 0x583974f20da1c7ca},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_02From03_SilverToken_Unlock", "", 0xb72a84ab06a5d986, 0x583974f20da1c7ca},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_02From03_SilverToken_Unlock", "", 0xc7f53b9e70b909ba, 0x583974f20da1c7ca},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_02From03_SilverToken_Unlock", "", 0xd504cad432df6c2d, 0x583974f20da1c7ca},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_02_SilverToken_Unlock", "", 0x0e741bc26e2c0697, 0xb1f683aa91daf209},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_02_SilverToken_Unlock", "", 0x1da71f60ac02ec12, 0xb1f683aa91daf209},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_02_SilverToken_Unlock", "", 0x4ab44ec9fa62841e, 0xb1f683aa91daf209},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_02_SilverToken_Unlock", "", 0x77f9feaac7f28534, 0xb1f683aa91daf209},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_02_SilverToken_Unlock", "", 0xe09a166c192d8995, 0xb1f683aa91daf209},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_06_SilverToken_Unlock", "", 0x5ac73f3cfe149f5e, 0xf6280b38b3266c26},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_06_SilverToken_Unlock", "", 0x8d3d7bd0aff53a04, 0xf6280b38b3266c26},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_06_SilverToken_Unlock", "", 0xb3d7d0be8bee7bd0, 0xf6280b38b3266c26},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_06_SilverToken_Unlock", "", 0xd48bf013feb195ab, 0xf6280b38b3266c26},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_06_SilverToken_Unlock", "", 0xe88c4cd905220761, 0xf6280b38b3266c26},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_09_SilverToken_Unlock", "", 0x0d9ab445c687af84, 0x12dfff252bc20a2f},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_09_SilverToken_Unlock", "", 0x1eeb356140284a18, 0x12dfff252bc20a2f},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_09_SilverToken_Unlock", "", 0xcf018cf2c0d86a7e, 0x12dfff252bc20a2f},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_09_SilverToken_Unlock", "", 0xddb014e95fd519b3, 0x12dfff252bc20a2f},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "GerudoTrainingGround", "GerudoTrainingGround_09_SilverToken_Unlock", "", 0xef4878d7320239f1, 0x12dfff252bc20a2f},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "IceCavern", "IceCavern_Room05_SilverToken_Unlock", "", 0x2d106890d9d01132, 0xaf0a7a17099f8d19},  // a token of Obj_silverRupeeCounter_03
    {Trigger::token, "IceCavern", "IceCavern_Room05_SilverToken_Unlock", "", 0x7633db754459d19d, 0xaf0a7a17099f8d19},  // a token of Obj_silverRupeeCounter_03
    {Trigger::token, "IceCavern", "IceCavern_Room05_SilverToken_Unlock", "", 0xa6b41aa8827c975c, 0xaf0a7a17099f8d19},  // a token of Obj_silverRupeeCounter_03
    {Trigger::token, "IceCavern", "IceCavern_Room05_SilverToken_Unlock", "", 0xc066e1f9eb643d09, 0xaf0a7a17099f8d19},  // a token of Obj_silverRupeeCounter_03
    {Trigger::token, "IceCavern", "IceCavern_Room05_SilverToken_Unlock", "", 0xffa5887336519e97, 0xaf0a7a17099f8d19},  // a token of Obj_silverRupeeCounter_03
    {Trigger::token, "IceCavern", "IceCavern_Room06_SilverToken_Unlock", "", 0x5af593e74aea3040, 0x8513f43d7c5488e0},  // a token of Obj_silverRupeeCounter_05
    {Trigger::token, "IceCavern", "IceCavern_Room06_SilverToken_Unlock", "", 0x5f68e628665584d6, 0x8513f43d7c5488e0},  // a token of Obj_silverRupeeCounter_05
    {Trigger::token, "IceCavern", "IceCavern_Room06_SilverToken_Unlock", "", 0x648096aeed9365dd, 0x8513f43d7c5488e0},  // a token of Obj_silverRupeeCounter_05
    {Trigger::token, "IceCavern", "IceCavern_Room06_SilverToken_Unlock", "", 0x662045239dbbe63a, 0x8513f43d7c5488e0},  // a token of Obj_silverRupeeCounter_05
    {Trigger::token, "IceCavern", "IceCavern_Room06_SilverToken_Unlock", "", 0x8d204d97a565b776, 0x8513f43d7c5488e0},  // a token of Obj_silverRupeeCounter_05
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room03", "", 0x696eb2dc0f8b34e8, 0xe783f8096d7f4900},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room03", "", 0x705fb316106b0b6c, 0xe783f8096d7f4900},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room03", "", 0x8b8068232007db4d, 0xe783f8096d7f4900},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room03", "", 0xa3f9adac302829e1, 0xe783f8096d7f4900},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room03", "", 0xb4e81d00864d3e86, 0xe783f8096d7f4900},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room10", "", 0x17694182d88f582e, 0x2047042424da1d32},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room10", "", 0x347ae3e058ec62d6, 0x2047042424da1d32},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room10", "", 0x35cf343928020beb, 0x2047042424da1d32},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room10", "", 0x659c7851d834bd73, 0x2047042424da1d32},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room10", "", 0x7727295e5ac90fbc, 0x2047042424da1d32},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room14", "", 0x6bc709478dcf5743, 0xf813ac43a8b36922},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room14", "", 0x75167baf8c4eb3cd, 0xf813ac43a8b36922},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room14", "", 0x9b10adddb3cbc666, 0xf813ac43a8b36922},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room14", "", 0xa97840fb17023abc, 0xf813ac43a8b36922},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room14", "", 0xb37960fe859686fe, 0xf813ac43a8b36922},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room16", "", 0x0feb38734016a7b5, 0x0e8fe67f2c220c0a},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room16", "", 0x3faf930737b10184, 0x0e8fe67f2c220c0a},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room16", "", 0xcd4013a340fcd219, 0x0e8fe67f2c220c0a},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room16", "", 0xed8772ded0ab12de, 0x0e8fe67f2c220c0a},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "InsideGanonCastle", "InsideGanonCastle_SilverToken_Unlock_Room16", "", 0xf1fa4913c187d43a, 0x0e8fe67f2c220c0a},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room06_SilverToken_Unlock", "", 0x63889e3987a9150a, 0x25766be26134a2b8},  // a token of Obj_silverRupeeCounter_04
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room06_SilverToken_Unlock", "", 0x7ef1165429858ebc, 0x25766be26134a2b8},  // a token of Obj_silverRupeeCounter_04
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room06_SilverToken_Unlock", "", 0x85424388dd3c8590, 0x25766be26134a2b8},  // a token of Obj_silverRupeeCounter_04
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room06_SilverToken_Unlock", "", 0xb17ab77bd26f6bdf, 0x25766be26134a2b8},  // a token of Obj_silverRupeeCounter_04
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room06_SilverToken_Unlock", "", 0xfa4541fe1a2d0d44, 0x25766be26134a2b8},  // a token of Obj_silverRupeeCounter_04
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room09_SilverToken_Unlock", "", 0x1fcdf4e539750d87, 0x253ba095a3818d6c},  // a token of Obj_silverRupeeCounter_04
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room09_SilverToken_Unlock", "", 0x743cfd29ef59b171, 0x253ba095a3818d6c},  // a token of Obj_silverRupeeCounter_04
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room09_SilverToken_Unlock", "", 0xbd3f9202723b1441, 0x253ba095a3818d6c},  // a token of Obj_silverRupeeCounter_04
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room09_SilverToken_Unlock", "", 0xbd85570337f0a11d, 0x253ba095a3818d6c},  // a token of Obj_silverRupeeCounter_04
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room09_SilverToken_Unlock", "", 0xf49ad43d8a7275c9, 0x253ba095a3818d6c},  // a token of Obj_silverRupeeCounter_04
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room12_SilverToken_Unlock", "", 0x8241637fe67c146a, 0x75980ea65b71139c},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room12_SilverToken_Unlock", "", 0x88b04072b4b7644f, 0x75980ea65b71139c},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room12_SilverToken_Unlock", "", 0xc0db58037d189f1c, 0x75980ea65b71139c},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room12_SilverToken_Unlock", "", 0xe09902944c952db7, 0x75980ea65b71139c},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "ShadowTemple", "ShadowTemple_Room12_SilverToken_Unlock", "", 0xfbeaa8e3bc7e4090, 0x75980ea65b71139c},  // a token of Obj_silverRupeeCounter
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_05_SilverToken_Unlock", "", 0x123a06e00ac830d0, 0x496dc2e67fd48379},  // a token of Obj_silverRupeeCounter_06
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_05_SilverToken_Unlock", "", 0x822af9b03cfc75d6, 0x496dc2e67fd48379},  // a token of Obj_silverRupeeCounter_06
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_05_SilverToken_Unlock", "", 0xa76777d06730e9db, 0x496dc2e67fd48379},  // a token of Obj_silverRupeeCounter_06
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_05_SilverToken_Unlock", "", 0xb19312d1918376b4, 0x496dc2e67fd48379},  // a token of Obj_silverRupeeCounter_06
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_05_SilverToken_Unlock", "", 0xf61cbd8464f4f4d8, 0x496dc2e67fd48379},  // a token of Obj_silverRupeeCounter_06
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_10_SilverToken_Unlock", "", 0x02f8a8f3bf0abf64, 0xf32a750e44c14f93},  // a token of Obj_silverRupeeCounter_02
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_10_SilverToken_Unlock", "", 0x0f6a8ef57d0bbd4f, 0xf32a750e44c14f93},  // a token of Obj_silverRupeeCounter_02
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_10_SilverToken_Unlock", "", 0x5c4828587b70f44c, 0xf32a750e44c14f93},  // a token of Obj_silverRupeeCounter_02
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_10_SilverToken_Unlock", "", 0x65332cb0488e7429, 0xf32a750e44c14f93},  // a token of Obj_silverRupeeCounter_02
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_10_SilverToken_Unlock", "", 0xeeb9d8960e2c1c92, 0xf32a750e44c14f93},  // a token of Obj_silverRupeeCounter_02
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_16_SilverToken_Unlock", "", 0x175745649864338f, 0x8e7e2c616d6f8711},  // a token of Obj_silverRupeeCounter_02
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_16_SilverToken_Unlock", "", 0x188041154d2d8cf7, 0x8e7e2c616d6f8711},  // a token of Obj_silverRupeeCounter_02
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_16_SilverToken_Unlock", "", 0x3630c03fe6fa04c3, 0x8e7e2c616d6f8711},  // a token of Obj_silverRupeeCounter_02
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_16_SilverToken_Unlock", "", 0xab91261b450b353a, 0x8e7e2c616d6f8711},  // a token of Obj_silverRupeeCounter_02
    {Trigger::token, "SpiritTemple", "SpiritTemple_Level_Room_16_SilverToken_Unlock", "", 0xd12664a0540d94e8, 0x8e7e2c616d6f8711},  // a token of Obj_silverRupeeCounter_02
};
// The tool's proof lines: every other trigger leaf (of the events with a live place), and why shared state cannot
// raise it. Four leaves are switch share plugs (coop_room_objects.cpp kSwitchPlugs, g3-objects): each is held only
// behind its guard, which is on once our asker is disarmed, so the hold cannot raise our trigger ('held by the switch
// share only behind its guard').
//   BottomOfTheWell_SilverToken_Appear: flag HaveEncountered_Token: not shared (each game's own)
//   Dm_DodongoCave_0010: BombFlowerChainExplosionManager.IsSuccessSameTimeExplodeNum: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   Dm_FireTemple_0029: flag FireTemple_Room14_HammmerStair_Complete: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   Dm_InsideGanonCastle_0035: Obj_LiftRock_C_01.ReceiveDoAction: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   DodongoCavern_Gimmick_CandleStickShutter: Obj_CandleStick_TimeExtinguish_A_01.Burning: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   DodongoCavern_Gimmick_DefeatLizalfos01: flag Gimmick_DefeatEnemy_Open_DodongoCave_01: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   DodongoCavern_Gimmick_DefeatLizalfos02: flag Gimmick_DefeatEnemy_Open_DodongoCave_02: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   DodongoCavern_Gimmick_EyeSwitch: Obj_EyeSwitch_A_05.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   DodongoCavern_Gimmick_EyeSwitch: flag DodongoCavern_NaviHelp_14_00: not shared (each game's own)
//   DodongoCavern_Gimmick_SwitchShortcut: Obj_StepSwitch_A_01.FirstOn: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   DodongoCavern_Gimmick_SwitchShutter_Room01: Obj_StepSwitch_A_01.FirstOn: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   FireTemple_06_EyeSwitch: Obj_EyeSwitch_A_02.FirstOn: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   FireTemple_15_OpenShutter: HoldDungeonRoomQueryIsInCurrentRoom.IsInCurrentRoom: our own game's state (our Link, our room)
//   FireTemple_Room24_FocusOnGimmick: flag Gimmick_DefeatEnemy_Open_FireTemple_01: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   FireTemple_SwitchFireCamera_Room04: Obj_SwitchHit_A_04.OnState: each game's own (a followed block in its room cannot drive it: a block rests only on press switches; no shared flag, key, kill or switch share line)
//   FireTemple_SwitchFireCamera_Room12: Obj_SwitchHit_A_04.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   FocusGimmick_RedTentacleGimmick0: flag Insidejabujabu_Room10_Cleared: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   FocusOnGimmick_FireTemple_FlareDancer02: flag Gimmick_DefeatEnemy_Open_FireTemple_03: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   FocusOnGimmick_InsideJabuJabuBelly_02: Obj_InsideJabu_Jabu_SwitchHit_A_03.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   FocusOnGimmick_InsideJabuJabuBelly_02: flag Insidejabujabu_Room14_Cleared: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   FocusOnGimmick_JabujabuLift: Obj_InsideJabu_Jabu_SwitchHit_A_01.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   FocusOnGimmick_JabujabuLift: flag InsideJabuJabuBelly_Room01_LiftMoveDown_GimmickClear: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   ForestTemple_Room02_ShutterOpen: Obj_StepSwitch_A_01.FirstOn: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   ForestTemple_Room11_TBoxAppear: Obj_EyeSwitch_A_02.OnState: each game's own (a followed block in its room cannot drive it: a block rests only on press switches; no shared flag, key, kill or switch share line)
//   ForestTemple_Room12_WaterLevelDrop: Obj_StepSwitch_A_01.FirstOn: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   ForestTemple_Room17_ShutterOpen: Obj_EyeSwitch_A_02.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   ForestTemple_Room17_TBoxAppear_CandleLight: Enemy_Poe_Joelle.Logic_Alive: a kill or remove only lowers it
//   ForestTemple_Room17_TBoxAppear_CandleLight: flag ForestTemple_Room_12_AppearTreasureChest: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   ForestTemple_Room17_TBoxAppear_CandleLight_ShutterOpen: Enemy_Poe_Beth.Logic_Alive: a kill or remove only lowers it
//   ForestTemple_Room17_TBoxAppear_CandleLight_ShutterOpen: flag ForestTemple_Room_13_AppearTreasureChest: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   ForestTemple_Room17_TwoStalfosAppear: Enemy_Stalfos_Weak.Logic_Alive: a kill or remove only lowers it
//   ForestTemple_Room22_FallingCeiling: flag Introduction_FallingCeiling: not shared (each game's own)
//   ForestTemple_Room22_ShutterOpen: Obj_StepSwitch_A_01.FirstOn: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   ForestTemple_Room25_CandleLight_ShutterOpen: Enemy_Poe_Amy.Logic_Alive: a kill or remove only lowers it
//   ForestTemple_Room25_FirstTimeBLock: CheckAttentionActor_TimeBlock.ReceiveDoAction: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   ForestTemple_Room25_FirstTimeBLock: CheckAttentionActor_TimeBlock.ReceiveDoAction: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   ForestTemple_Room27_CandleLight: Obj_StepSwitch_A_01.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   ForestTemple_Room27_ShutterOpen: Obj_EyeSwitch_A_02.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   GerudoTrainingGround_00_FallTreasureBox: Obj_EyeSwitch_A_02.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   GerudoTrainingGround_02_SilverToken_Appear: flag HaveEncountered_Token: not shared (each game's own)
//   GerudoTrainingGround_03_ShutterOpen: Obj_EyeSwitch_GlassTruth_A_02.OnState: each game's own (a followed block in its room cannot drive it: a block rests only on press switches; no shared flag, key, kill or switch share line)
//   GerudoTrainingGround_03_ShutterOpen: flag GerudoTrainingGround_Dungeon_Room03_StepSwitch: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   GerudoTrainingGround_04_ActStoneGimmick: Obj_EyeSwitch_A_05.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   GerudoTrainingGround_04_ActStoneGimmick: flag GerudoTrainingGround_Dungeon_Room04_StoneStatue: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   GerudoTrainingGround_05_DisappearFire: Obj_StepSwitch_A_04.OnState: held by the switch share only behind its guard GerudoTrainingGround_Dungeon_Room05_StepSwitch (on: our asker is disarmed)
//   GerudoTrainingGround_05_ShutterOpen: Obj_EyeSwitch_A_03.FirstOn: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   GerudoTrainingGround_06_DisappearFire: Obj_StepSwitch_A_02.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   GerudoTrainingGround_06_DisappearFire: flag GerudoTrainingGround_Dungeon_Room06_Fire: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   GerudoTrainingGround_06_SilverToken_Appear: flag HaveEncountered_Token: not shared (each game's own)
//   GerudoTrainingGround_08_FenceFallDown: Obj_StepSwitch_A_01.FirstOn: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   GerudoTrainingGround_10_FallTreasureBox: Obj_EyeSwitch_A_02.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   GerudoTrainingGround_Room04_StoneGimmick: Obj_Gerudo_StoneStatue_B_01.IsOn_Xm: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   GerudoTrainingGround_Room04_StoneGimmick: Obj_Gerudo_StoneStatue_B_01.IsOn_Xp: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   GerudoTrainingGround_Room04_StoneGimmick: Obj_Gerudo_StoneStatue_B_01.IsOn_Zm: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   GerudoTrainingGround_Room04_StoneGimmick: Obj_Gerudo_StoneStatue_B_01.IsOn_Zp: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   IceCavern_Room01_DefeatEnemy: flag IceCavern_Room01_Unlocked: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   IceCavern_Room05_SilverToken_Appear: flag HaveEncountered_Token: not shared (each game's own)
//   InsideGanonCastle_Dungeon_AppaerTreasureBox_08: Obj_StepSwitch_A_01.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideGanonCastle_Dungeon_AppearExtendingPlatform_03: Obj_StepSwitch_A_01.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideGanonCastle_Dungeon_BarrierRemove_Fire: flag InsideGanonCastle_Dungeon_BarrierRemove_Fire: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   InsideGanonCastle_Dungeon_BarrierRemove_Forest: flag InsideGanonCastle_Dungeon_BarrierRemove_Forest: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   InsideGanonCastle_Dungeon_BarrierRemove_Light: flag InsideGanonCastle_Dungeon_BarrierRemove_Light: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   InsideGanonCastle_Dungeon_BarrierRemove_Shadow: flag InsideGanonCastle_Dungeon_BarrierRemove_Shadow: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   InsideGanonCastle_Dungeon_BarrierRemove_Spirit: flag InsideGanonCastle_Dungeon_BarrierRemove_Spirit: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   InsideGanonCastle_Dungeon_BarrierRemove_Water: flag InsideGanonCastle_Dungeon_BarrierRemove_Water: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   InsideGanonCastle_Dungeon_EnemyAppear01: Obj_SolarSwitch_A_03.isLightMax: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideGanonCastle_Dungeon_EnemyAppear02: Obj_SolarSwitch_A_03.isLightMax: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideGanonCastle_Dungeon_EnemyAppear03: Obj_SolarSwitch_A_03.isLightMax: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideGanonCastle_Dungeon_FocusOnGimmick_08_01: Obj_CandleStick_TimeExtinguish_A_01.Burning: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideGanonCastle_Dungeon_ShutterOpen_02_01: Obj_CandleStick_TimeExtinguish_A_01.Burning: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideGanonCastle_Dungeon_ShutterOpen_08_01: Obj_StepSwitch_A_01.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideGanonCastle_Dungeon_ShutterOpen_17_01: Obj_SwitchHit_A_02.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideGanonCastle_Dungeon_ShutterOpen_17_02: Obj_SolarSwitch_A_02.isLightMax: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideJabuJabuBellyRoom01StepSwitch: Obj_InsideJabu_Jabu_StepSwitch_A_03.OnState: held by the switch share only behind its guard InsideJabuJabuBelly_Gimmick_Onswitch_first (on: our asker is disarmed)
//   InsideJabuJabuBellyRoom01StepSwitch: flag InsideJabuJabuBelly_Gimmick_Onswitch_first: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   InsideJabuJabuBellyRoom05Swich: Obj_InsideJabu_Jabu_SwitchHit_A_02.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideJabuJabuBellyRoom07StepSwitch: Obj_InsideJabu_Jabu_StepSwitch_A_03.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideJabuJabuBellyRoom07StepSwitch: flag Insidejabujabu_Gimmick_Room7Event: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   InsideJabuJabuBellyRoom07StepSwitch02: Obj_InsideJabu_Jabu_Stepswitch_A_02.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideJabuJabuBellyRoom14StepSwitch: Obj_InsideJabu_Jabu_StepSwitch_A_04.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideJabuJabuBellyRoom14StepSwitch: flag Gimmick_Insidejabujabu_Updown_MoveWaterSwitch: not shared (each game's own)
//   InsideJabuJabuBellyRoom14StepSwitch_Under: Obj_InsideJabu_Jabu_StepSwitch_A_04.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideJabuJabuBellyRoom14StepSwitch_Under: flag Gimmick_Insidejabujabu_Updown_MoveWater: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   InsideJabuJabuBellyRoom14StepSwitch_Under: flag Gimmick_Insidejabujabu_Updown_MoveWaterSwitch: not shared (each game's own)
//   InsideJabuJabuBelly_FocusOnGimmick: Obj_InsideJabu_Jabu_SwitchHit_A_01.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideJabuJabuBelly_Room12_TreasureBoxAppear: flag Insidejabujabu_Room12_Cleared: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   InsideJabuJabuBelly_TreasureBoxAppear: flag Insidejabujabu_Room9_Cleared: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   InsideTheDekuTree_Gimmick_FallingLadder: Obj_EyeSwitch_A_02.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideTheDekuTree_Gimmick_RoomD_PlatformUp: Obj_StepSwitch_A_02.OnState: held by the switch share only behind its guard load:InsideTheDekuTree_RoomD_StepSwitch_On_01 (on: our asker is disarmed)
//   InsideTheDekuTree_Gimmick_RoomF_EyeSwitch: Obj_EyeSwitch_A_02.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideTheDekuTree_Gimmick_RoomG_WaterLevelDrop: Obj_DiveSwitch_A_01.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   InsideTheDekuTree_Gimmick_RoomH_CandleStick: Obj_CandleStick_TimeExtinguish_A_01.Burning: each game's own (a followed block in its room cannot drive it: a block rests only on press switches; no shared flag, key, kill or switch share line)
//   InsideTheDekuTree_Gimmick_RoomH_CandleStick: flag InsideTheDekuTree_RoomH_Cleared: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   ShadowTemple_01_BeakStatue_Clear: flag ShadowTemple_Level_Room_01_Clear_A: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   ShadowTemple_04_EnemyAnnihilation_Clear: flag ShadowTemple_RoomB1FD_Cleared: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   ShadowTemple_07_EnemyAnnihilation_Clear: flag ShadowTemple_RoomB2FB_Cleared: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   ShadowTemple_10_EnemyAnnihilation_Clear: flag ShadowTemple_RoomB3FB4FD_Cleared: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   ShadowTemple_13_EnemyAnnihilation_DOWN_Clear: flag ShadowTemple_RoomB3FB4FF_DefeatKeith: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   ShadowTemple_13_EnemyAnnihilation_UP_Clear: flag ShadowTemple_RoomB3FB4FF_DefeatKeith: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   ShadowTemple_15_EnemyAnnihilation_Clear: flag ShadowTemple_RoomB3FB4FH_Cleared: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   ShadowTemple_16_EnemyAnnihilation_Clear: flag ShadowTemple_RoomB3FB4FI_DefeatRedead: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   ShadowTemple_19_EnemyAnnihilation_Clear: HoldDungeonRoomQueryIsInCurrentRoom.IsInCurrentRoom: our own game's state (our Link, our room)
//   ShadowTemple_21_EnemyAnnihilation_Clear: flag ShadowTemple_RoomB3FB4F_OpenCounter: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   ShadowTemple_Gimmick_Focus_LargeJar_Key: flag ShadowTemple_Level_Room_13_Clear_A: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   SpiritTemple_FaceIsBroken_Effect: Obj_SpiritTemple_GoddessStatueFace_A_01.isLightMax: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpiritTemple_Level_Room_03_BoardNetFall: Obj_SwitchHit_A_03.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpiritTemple_Level_Room_03_BoardNetFall: flag SpiritTemple_Room03_BoardNet_On: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   SpiritTemple_Level_Room_04_EyeSwitchOn: Obj_EyeSwitch_A_02.FirstOn: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpiritTemple_Level_Room_07_SunSwitchOn: flag SpiritTemple_Room07_Unlocked: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   SpiritTemple_Level_Room_08_SwitchOn_01: flag SpiritTemple_Room08_Unlocked_00: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   SpiritTemple_Level_Room_10_SwitchOn_01: Obj_SolarSwitch_A_01.isLightMax: each game's own (a followed block in its room cannot drive it: a block rests only on press switches; no shared flag, key, kill or switch share line)
//   SpiritTemple_Level_Room_19_SwitchOn_01: HoldPlayerrCheckEventQueryPlayerState.IsNotState: our own game's state (our Link, our room)
//   SpiritTemple_Level_Room_19_SwitchOn_01: Obj_SolarSwitch_A_02.isLightMax: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpiritTemple_Level_Room_19_SwitchOn_01: Obj_SolarSwitch_A_02.isLightMax: held by the switch share only behind its guard SpiritTemple_Room19_LightSwitch_On_01 (on: our asker is disarmed)
//   SpiritTemple_Level_Room_19_wallmaster: Obj_SolarSwitch_A_03.isLightMax: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpiritTemple_Level_Room_30_BoardNetFall: Obj_EyeSwitch_A_03.FirstOn: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpiritTemple_Level_Room_32_SwitchOn_01: Obj_SolarSwitch_A_02.isLightMax: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpiritTemple_Room19_SunSwitchShutterOpen: HoldPlayerrCheckEventQueryPlayerState.IsNotState: our own game's state (our Link, our room)
//   SpiritTemple_Room19_SunSwitchShutterOpen: Obj_SolarSwitch_A_02.isLightMax: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpritTemple_Room05_AppearTreasure: Obj_CandleStick_A_02.Burning: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpritTemple_Room07_AppearTreasure: Obj_SwitchHit_A_05.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpritTemple_Room08_AppearTreasureBox: Obj_CandleStick_A_02.Burning: each game's own (a followed block in its room cannot drive it: a block rests only on press switches; no shared flag, key, kill or switch share line)
//   SpritTemple_Room12_OpenShutter: Enemy_IronKnuckle_Silver.Logic_Alive: a kill or remove only lowers it
//   SpritTemple_Room14_OpenShutter: Obj_SwitchHit_A_02.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpritTemple_Room22_StepSwitch: Obj_StepSwitch_A_01.FirstOn: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpritTemple_Room23_StepSwitch: Obj_Stepswitch_A_03_Amos.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpritTemple_Room23_SunSwitch: Obj_SolarSwitch_A_02.isLightMax: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpritTemple_Room25_OpenShutter: Enemy_IronKnuckle_Silver.Logic_Alive: a kill or remove only lowers it
//   SpritTemple_Room30_StepSwitch: Obj_StepSwitch_A_01.FirstOn: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   SpritTemple_Room31_OpenShutter: Obj_SwitchHit_A_05.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   WaterTempleRoom14StepSwitch: HoldPlayerrCheckEventQueryPlayerState.IsNotState: our own game's state (our Link, our room)
//   WaterTemple_FocusGimmick_Room18: flag WaterTemple_Room18_Tresure: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   WaterTemple_FocusonGimmick_02: Obj_SwitchHit_A_02.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   WaterTemple_FocusonGimmick_02: flag WaterTemple_Room02_00: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   WaterTemple_Room06_Waterlevelrise_FocusOnGimmick: Obj_SwitchHit_A_06.OnState: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   WaterTemple_Room06_Waterlevelrise_FocusOnGimmick: flag WaterTemple_Waterlevelrise_Room06: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   WaterTemple_Room17_FocusOnGimmick: Obj_CandleStick_TimeExtinguish_A_01.Burning: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   WaterTemple_Room17_FocusOnGimmick: flag WaterTemple_Room17_Cleared: read negated: another game's 1 only lowers the trigger; a saved room flag the game never clears (a 0 comes only with a save load)
//   WaterTemple_Waterlevelrise_FocusOnGimmick: Obj_StepSwitch_A_01.FirstOn: each game's own (no shared flag, key, kill, switch share line or followed world object that can drive it)
//   WaterTemple_Waterlevelrise_FocusOnGimmick: flag WaterTemple_Room03_FocusOnGimmick: not shared (each game's own)
// clang-format on

// The setter's block (one game malloc, kept: a slot may keep the AS name pointer it was given): the set-bool node
// (coop_logic_set.h), then the AS play request (0x30 bytes, coop_puppet.h), then one name cell per kPlays row.
constexpr std::uint64_t kPlayRequestAt = (CoopLogicSet::addr::kBlockSize + 0xf) & ~std::uint64_t{0xf};
constexpr std::uint64_t kPlayNamesAt = kPlayRequestAt + 0x30;
constexpr std::uint64_t kSetterBlockSize = kPlayNamesAt + kPlayNameCell * std::size(kPlays);
constexpr bool PlayNamesFit() {
    for (const auto& p : kPlays) {
        if (p.as.size() >= kPlayNameCell) return false;
    }
    return true;
}
static_assert(PlayNamesFit(), "an AS name longer than its cell");

// Slot 0's AS command name of `actor` ("" when unreadable; docs/puppet.md "Animation").
std::string SlotZeroName(CoopPuppet::Guest& guest, std::uint64_t actor) {
    namespace pa = CoopPuppet::addr;
    std::uint64_t at = actor;
    for (const auto off : pa::kControllerPath) {
        if (!guest.Read(at + off, &at, 8) || at == 0) return {};
    }
    std::uint64_t slots = 0;
    std::uint64_t slot = 0;
    std::uint64_t name = 0;
    std::array<char, kPlayNameCell> text{};
    if (!guest.Read(at + pa::kSlots, &slots, 8) || slots == 0 || !guest.Read(slots, &slot, 8) || slot == 0 ||
        !guest.Read(slot + pa::kSlotName, &name, 8) || name == 0 || !guest.Read(name, text.data(), text.size())) {
        return {};
    }
    return std::string(text.data(), strnlen(text.data(), text.size()));
}

float Dist2(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

std::uint32_t PlayedHash(std::string_view event) {
    return coop::protocol::flag_hash("HavePlayedEvent." + std::string(event));
}

bool Has(const std::vector<std::uint32_t>& list, std::uint32_t hash) {
    return std::find(list.begin(), list.end(), hash) != list.end();
}
}  // namespace

std::span<const Result> Results() { return kResults; }

std::span<const Play> Plays() { return kPlays; }

std::vector<const Play*> PlaysOf(std::string_view event, std::string_view scene) {
    std::vector<const Play*> out;
    for (const auto& p : kPlays) {
        if (p.event == event && p.scene == scene) out.push_back(&p);
    }
    return out;
}

std::span<const Guard> Guards() { return kGuards; }

std::span<const Trigger> Triggers() { return kTriggers; }

std::vector<const Result*> AskerLines(std::string_view event, std::string_view scene,
                                      const std::optional<std::array<float, 3>>& link) {
    std::vector<const Result*> out;
    for (const auto& r : kResults) {
        if (r.event == event && r.scene == scene && (!r.outputs.empty() || !PlaysOf(event, scene).empty())) {
            out.push_back(&r);
        }
    }
    if (link) {
        std::stable_sort(out.begin(), out.end(), [&](const Result* x, const Result* y) {
            return Dist2(x->at, *link) < Dist2(y->at, *link);
        });
    }
    return out;
}

bool IsLockOnly(std::string_view event) {
    return std::any_of(std::begin(kLocks), std::end(kLocks), [&](const Lock& l) { return l.event == event; });
}

bool IsLive(std::string_view event) {
    bool any = false;
    for (const auto& r : kResults) {
        if (r.event != event) continue;
        if (!r.outputs.empty() || !PlaysOf(event, r.scene).empty()) return false;
        any = true;
    }
    return any;
}

std::vector<std::string_view> SplitOutputs(std::string_view outputs) {
    std::vector<std::string_view> out;
    while (!outputs.empty()) {
        const auto plus = outputs.find('+');
        out.push_back(outputs.substr(0, plus));
        if (plus == std::string_view::npos) break;
        outputs.remove_prefix(plus + 1);
    }
    return out;
}

bool Hold::Releases(std::uint32_t hash) const { return Has(released, hash); }

bool Hold::Waits(std::uint32_t hash) const { return Has(waiting, hash); }

bool Hold::WaitsKey(std::uint64_t key) const {
    return std::find(waiting_keys.begin(), waiting_keys.end(), key) != waiting_keys.end();
}

namespace {
// Whether a trigger of `event` in `scene` still waits: our game is in that scene and has not played it.
bool TriggerWaits(const Trigger& t, std::string_view scene, const std::function<bool(const std::string&)>& played) {
    return t.scene == scene && !(played && played(std::string(t.event)));
}
}  // namespace

std::set<std::uint64_t> KillKeepers(std::string_view scene, const std::function<bool(std::uint64_t)>& alive,
                                    const std::function<bool(const std::string&)>& played,
                                    const std::function<bool(std::uint64_t)>& ours) {
    std::map<std::string_view, std::uint64_t> keeper;  // event -> its largest live kill actor
    std::set<std::string_view> own_alive;              // events with a live kill actor our game owns
    for (const auto& t : kTriggers) {
        if (t.kind != Trigger::kill || !TriggerWaits(t, scene, played) || !(alive && alive(t.id))) continue;
        if (ours && ours(t.id)) {
            own_alive.insert(t.event);  // rows 160/161: our own enemy's death is the last one, in our game
            continue;
        }
        auto& k = keeper[t.event];
        k = std::max(k, t.id);
    }
    std::set<std::uint64_t> out;
    for (const auto& [event, hash] : keeper) {
        if (!own_alive.contains(event)) out.insert(hash);
    }
    return out;
}

bool CrossingKillLands(std::string_view scene, std::uint64_t keeper, std::uint64_t kept_for_us, std::uint8_t self,
                       std::optional<std::uint8_t> keeper_owner, const std::function<bool(std::uint64_t)>& ours_died,
                       bool owner_here, bool we_decide) {
    if (kept_for_us < kCrossingUs || self == 0) {
        return false;
    }
    bool kill_line = false;
    for (const auto& t : kTriggers) {
        if (t.kind == Trigger::kill && t.scene == scene && t.id == keeper) kill_line = true;
    }
    if (!kill_line) {
        return false;
    }
    if (!owner_here) {
        return we_decide;  // its owner cannot play the event any more: the scene's decider does
    }
    if (!keeper_owner || *keeper_owner == 0 || self >= *keeper_owner || !ours_died) {
        return false;
    }
    for (const auto& t : kTriggers) {
        if (t.kind != Trigger::kill || t.scene != scene || t.id != keeper) continue;
        for (const auto& o : kTriggers) {  // another kill actor of the same event that was ours and died here
            if (o.kind == Trigger::kill && o.scene == scene && o.event == t.event && o.id != keeper && ours_died(o.id)) {
                return true;
            }
        }
    }
    return false;
}

bool AbleToPlay(bool in_our_scene, bool link_up, std::uint8_t status) {
    constexpr std::uint8_t kCannot = coop::protocol::status::dead | coop::protocol::status::loading;
    return in_our_scene && link_up && (status & kCannot) == 0;
}

std::uint8_t DeciderAmongAble(std::uint8_t self, std::span<const std::uint8_t> in_scene,
                              const std::function<bool(std::uint8_t)>& able) {
    std::uint8_t decider = 0;
    const auto consider = [&](std::uint8_t p) {
        if (p != 0 && (decider == 0 || p < decider) && able && able(p)) decider = p;
    };
    consider(self);
    for (const std::uint8_t p : in_scene) consider(p);
    return decider;
}

std::optional<std::string_view> KillEventOf(std::string_view scene, std::uint64_t hash) {
    for (const auto& t : kTriggers) {
        if (t.kind == Trigger::kill && t.scene == scene && t.id == hash) return t.event;
    }
    return std::nullopt;
}

std::set<std::uint64_t> KillsToRedo(std::string_view scene, const std::set<std::uint64_t>& dead,
                                    const std::set<std::uint64_t>& kept,
                                    const std::function<bool(const std::string&)>& played) {
    std::map<std::string_view, bool> all_dead;  // event -> every kill actor of it is dead
    std::set<std::string_view> others;          // events with another game's kill kept here
    for (const auto& t : kTriggers) {
        if (t.kind != Trigger::kill || !TriggerWaits(t, scene, played)) continue;
        const auto it = all_dead.try_emplace(t.event, true).first;
        it->second = it->second && (dead.contains(t.id) || kept.contains(t.id));
        if (kept.contains(t.id)) others.insert(t.event);
    }
    std::erase_if(all_dead, [&](const auto& e) { return !others.contains(e.first); });
    std::set<std::uint64_t> out;
    for (const auto& t : kTriggers) {
        if (t.kind != Trigger::kill || t.scene != scene) continue;
        if (const auto it = all_dead.find(t.event); it != all_dead.end() && it->second) out.insert(t.id);
    }
    return out;
}

std::optional<KeptShield::Step> KeptShield::Next(const std::set<std::uint64_t>& want,
                                                 const std::set<std::uint64_t>& live) {
    for (auto it = on_.begin(); it != on_.end();) {
        it = live.contains(*it) ? std::next(it) : on_.erase(it);  // gone, and our key with it
    }
    std::erase_if(tries_, [&](const auto& t) { return !live.contains(t.first.first); });
    for (const std::uint64_t l : on_) {
        const Step off{l, false};
        if (!want.contains(l) && !GivenUp(off)) return off;  // off first: a landing kill waits for it
    }
    for (const std::uint64_t l : want) {
        const Step on{l, true};
        if (l != 0 && live.contains(l) && !on_.contains(l) && !GivenUp(on)) return on;
    }
    return std::nullopt;
}

void KeptShield::Called(const Step& s, bool ok) {
    if (!ok) {
        ++tries_[{s.life, s.on}];
        return;
    }
    tries_.erase({s.life, s.on});
    if (s.on) {
        on_.insert(s.life);
    } else {
        on_.erase(s.life);
    }
}

bool KeptShield::GivenUp(const Step& s) const {
    const auto it = tries_.find({s.life, s.on});
    return it != tries_.end() && it->second >= kKeptShieldTries;
}

std::set<std::uint64_t> FollowKeepers(std::string_view scene, const std::function<bool(const std::string&)>& played) {
    std::set<std::uint64_t> out;
    std::map<std::pair<std::string_view, std::uint64_t>, std::uint64_t> largest;  // (event, group) -> its keeper
    for (const auto& t : kTriggers) {
        if (t.kind != Trigger::follow || !TriggerWaits(t, scene, played)) continue;
        if (t.group == 0) {
            out.insert(t.id);
        } else {
            auto& k = largest[{t.event, t.group}];
            k = std::max(k, t.id);
        }
    }
    for (const auto& [group, hash] : largest) out.insert(hash);
    return out;
}

Hold HoldFor(std::string_view scene, const std::set<std::string>& applied,
             const std::function<bool(const std::string&)>& played,
             const std::function<bool(std::uint64_t)>& have_key, const std::set<std::string>& given_up) {
    Hold h;
    // Row 336: a lock-only event given up lands as in 0.12.0 (class live): its played flag and its guards, like a set.
    std::set<std::string> done = applied;
    for (const auto& event : given_up) {
        if (IsLockOnly(event)) done.insert(event);
    }
    for (const auto& event : done) {
        h.released.push_back(PlayedHash(event));
        for (const auto& g : kGuards) {
            if (g.event == event) h.released.push_back(coop::protocol::flag_hash(g.flag));
        }
        for (const auto& g : CoopEyeSwitch::Guards()) {
            if (g.event == event) h.released.push_back(coop::protocol::flag_hash(g.flag));
        }
    }
    const auto wait = [&](std::string_view flag, std::string_view guard_scene, std::string_view guard_event) {
        const std::string event(guard_event);
        if (guard_scene != scene || done.contains(event) || (played && played(event))) return;
        // Only an event with an output of our asker to set: a class-live event's result is its flags; nothing waits.
        if (AskerLines(guard_event, guard_scene).empty()) return;
        const auto hash = coop::protocol::flag_hash(flag);
        if (!h.Releases(hash) && !h.Waits(hash)) h.waiting.push_back(hash);
    };
    for (const auto& g : kGuards) wait(g.flag, g.scene, g.event);
    // The eye switch path's guards (coop_eye_switch.h): they kept our asker for its replay; the replay is retired, so
    // they keep it for our result now, by the same rule.
    for (const auto& g : CoopEyeSwitch::Guards()) wait(g.flag, g.scene, g.event);
    // Trigger guards (coop_event_result.h, both classes): until our game has played the event. A flag released above
    // (an applied event's guard) is not a trigger of an unplayed event here, so Releases goes first as before.
    std::map<std::pair<std::string_view, std::uint64_t>, std::uint64_t> largest_missing;  // (event, counter) -> token
    for (const auto& t : kTriggers) {
        if (!TriggerWaits(t, scene, played)) continue;
        switch (t.kind) {
        case Trigger::flag: {
            const auto hash = coop::protocol::flag_hash(t.name);
            if (!h.Releases(hash) && !h.Waits(hash)) h.waiting.push_back(hash);
            break;
        }
        case Trigger::key:
            if (!h.WaitsKey(t.id)) h.waiting_keys.push_back(t.id);
            break;
        case Trigger::token:
            if (!(have_key && have_key(t.id))) {
                auto& m = largest_missing[{t.event, t.group}];
                m = std::max(m, t.id);
            }
            break;
        case Trigger::kill:
        case Trigger::follow:
            break;  // the object sync's own holds (KillKeepers, FollowKeepers)
        }
    }
    for (const auto& [group, token] : largest_missing) {
        if (!h.WaitsKey(token)) h.waiting_keys.push_back(token);
    }
    return h;
}

bool Setter::Want(const std::string& event, const std::string& scene, std::optional<std::array<float, 3>> link) {
    if (AskerLines(event, scene).empty()) return false;
    std::lock_guard lock(mutex_);
    if (applied_.contains(event) || ended_.contains(event) ||
        std::any_of(wanted_.begin(), wanted_.end(), [&](const Try& t) { return t.event == event; })) {
        return true;  // set, given up or being set this visit
    }
    Try t;
    t.event = event;
    t.scene = scene;
    t.link = link;
    wanted_.push_back(std::move(t));
    return true;
}

void Setter::Visit(const std::string& scene, const std::function<bool(const std::string&)>& played) {
    std::lock_guard lock(mutex_);
    if (!scene.empty() && scene != scene_) {  // a new visit: the room loads again, nothing of ours is set in it
        scene_ = scene;
        applied_.clear();
        ended_.clear();
        held_since_.clear();
        given_up_.clear();
    }
    // The front may be mid-set on the calc thread: it ends there (Frame sees the scene or the played flag) so its
    // execute's after-call stays paired; only the ones behind it are dropped here.
    for (auto it = wanted_.size() > 1 ? wanted_.begin() + 1 : wanted_.end(); it != wanted_.end();) {
        if (it->scene != scene_ || (played && played(it->event))) {
            notes_.push_back("result: " + it->event + ": dropped (another scene, or our game played it)");
            it = wanted_.erase(it);
        } else {
            ++it;
        }
    }
    if (!wanted_.empty() && (wanted_.front().scene != scene_ || (played && played(wanted_.front().event)))) {
        wanted_.front().until_ms = 1;  // Frame ends it as timed out ("not set here")
    }
}

std::set<std::string> Setter::Applied() {
    std::lock_guard lock(mutex_);
    return applied_;
}

std::set<std::string> Setter::GivenUp(const std::vector<std::string>& held, std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    const auto give_up = [&](const std::string& event, const char* why) {
        if (given_up_.insert(event).second) {
            notes_.push_back("result: " + event + ": " + why +
                             ": its flags land now, as a class-live event's (row 336 fallback)");
        }
    };
    for (const auto& event : ended_) {
        if (IsLockOnly(event)) give_up(event, "not set here");
    }
    for (const auto& event : held) {
        if (!IsLockOnly(event) || applied_.contains(event)) continue;
        const auto [it, fresh] = held_since_.try_emplace(event, now_ms);
        if (!fresh && now_ms >= it->second && now_ms - it->second >= kLockFallbackMs) {
            give_up(event, "not set within 10 s of another player's played flag");
        }
    }
    return given_up_;
}

std::vector<std::string> Setter::TakeNotes() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
}

bool Setter::EventRunning(CoopPuppet::Guest& guest, std::uint64_t main) {
    namespace le = CoopLiveEvents::addr;
    std::uint64_t manager = 0;
    std::uint64_t session = 0;
    if (!guest.Read(main + le::kEventStatePtr, &manager, 8) || manager == 0) return false;
    return guest.Read(manager + le::kEventSession, &session, 8) && session != 0;
}

void Setter::End(const std::string& why, bool applied) {
    const Try& t = wanted_.front();
    notes_.push_back("result: " + t.event + ": " + why);
    if (t.scene == scene_) (applied ? applied_ : ended_).insert(t.event);
    wanted_.pop_front();
}

std::optional<CoopPuppet::Call> Setter::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                             std::uint64_t main, std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    if (after_call) {
        const Next was = next_;
        next_ = Next::none;
        if (was != Next::malloc) return std::nullopt;  // the execute: one set a frame
        if (result == 0) {
            failed_ = true;
            while (!wanted_.empty()) End("the game's malloc gave nothing: not set, its flags stay held", false);
            return std::nullopt;
        }
        block_ = result;
    }
    if (main == 0 || wanted_.empty()) return std::nullopt;
    if (failed_) {
        while (!wanted_.empty()) End("no set-bool block: not set, its flags stay held", false);
        return std::nullopt;
    }
    if (block_ == 0) {
        if (after_call) return std::nullopt;
        next_ = Next::malloc;
        auto c = CoopLogicSet::Malloc(main);
        c.x[0] = kSetterBlockSize;  // the set-bool node, then the AS play request and its name cells
        return c;
    }
    return Frame(guest, main, now_ms);
}

std::optional<CoopPuppet::Call> Setter::Frame(CoopPuppet::Guest& guest, std::uint64_t main, std::uint64_t now_ms) {
    Try& t = wanted_.front();
    if (t.until_ms == 0) t.until_ms = now_ms + kTryMs;
    const auto lines = AskerLines(t.event, t.scene, t.link);
    if (now_ms > t.until_ms || lines.empty()) {
        char text[200];
        std::snprintf(text, sizeof text,
                      "not set here (%s; output %zu: %d set-bools, read 0 after a set on %d frames): its flags stay "
                      "held until we leave the dungeon or load",
                      t.asker == 0 ? "our asker not found" : "an output did not stay 1", t.output, t.sets,
                      t.zeros_after_set);
        End(text, false);
        return std::nullopt;
    }
    if (EventRunning(guest, main)) return std::nullopt;  // an event of ours runs: its flow owns the askers now
    // Our asker: the first line (nearest our Link) with an asker at its placement. Once found, only that one.
    const Result* line = nullptr;
    std::uint64_t asker = 0;
    for (const Result* l : lines) {
        const auto here = find_(guest, main, CoopLiveEvents::kAskerNames, l->at, CoopLiveEvents::kAskerNearM);
        if (here.empty()) continue;
        if (t.asker != 0 && std::find(here.begin(), here.end(), t.asker) == here.end()) continue;
        line = l;
        asker = t.asker != 0 ? t.asker : here.front();
        break;
    }
    if (line == nullptr) {
        if (t.asker != 0) {  // its graph deleted it after our set (EventFinish -> Logic_Delete): the result ran
            End("our asker is gone after our set (its room graph took the result)", true);
        }
        return std::nullopt;  // not loaded yet: wait (until kTryMs)
    }
    if (t.asker == 0) {
        t.asker = asker;
        char text[200];
        std::snprintf(text, sizeof text, "result: %s: our asker 0x%llx, setting %.*s (another player played it; no replay)",
                      t.event.c_str(), static_cast<unsigned long long>(asker), static_cast<int>(line->outputs.size()),
                      line->outputs.data());
        notes_.emplace_back(text);
    }
    // Row 165: our performers play the event's AS first (the room swaps them only at its load).
    if (auto call = PlayStep(guest, t, main)) return call;
    if (t.play < PlaysOf(t.event, t.scene).size()) return std::nullopt;  // a play shows: read it next frame
    const auto outputs = SplitOutputs(line->outputs);
    if (t.output >= outputs.size()) {
        End(line->outputs.empty() ? "set (its performers' AS)" : "set (" + std::string(line->outputs) + ")", true);
        return std::nullopt;
    }
    const std::uint32_t key = coop::protocol::flag_hash(outputs[t.output]);
    const auto plug = CoopLogicSet::BoolPlug(guest, asker, key);
    if (!plug) return std::nullopt;  // its logic not read yet: wait (until kTryMs)
    if (plug->value == std::uint8_t{1}) {
        if (++t.ones >= kApplyFrames) {
            notes_.push_back("result: " + t.event + ": our asker's " + std::string(outputs[t.output]) + " is 1");
            ++t.output;
            t.ones = 0;
            t.sets = 0;
            t.zeros_after_set = 0;
            if (t.output >= outputs.size()) End("set (" + std::string(line->outputs) + ")", true);
        }
        return std::nullopt;
    }
    t.ones = 0;
    if (t.sets > 0) ++t.zeros_after_set;
    if (!CoopLogicSet::WriteNode(guest, block_, main, asker, key)) return std::nullopt;
    ++t.sets;
    next_ = Next::execute;
    return CoopLogicSet::Execute(main, block_);
}

std::optional<CoopPuppet::Call> Setter::PlayStep(CoopPuppet::Guest& guest, Try& t, std::uint64_t main) {
    const auto plays = PlaysOf(t.event, t.scene);
    const auto next = [&](const std::string& why) {
        notes_.push_back("result: " + t.event + ": our " + std::string(plays[t.play]->actor) + " " + why);
        ++t.play;
        t.play_calls = 0;
        t.play_ones = 0;
    };
    while (t.play < plays.size()) {
        const Play& p = *plays[t.play];
        const std::string_view names[] = {p.actor};
        const auto here = find_(guest, main, names, p.at, kPerformerAtM);
        if (here.empty()) {
            next("is not at its place: skipped (our room shows its end already, or it was moved)");
            continue;
        }
        const std::uint64_t actor = here.front();
        if (SlotZeroName(guest, actor) == p.as) {
            if (++t.play_ones < kApplyFrames) return std::nullopt;  // read it again next frame
            next("plays " + std::string(p.as) + " (the event's own AS)");
            continue;
        }
        t.play_ones = 0;
        if (t.play_calls >= kMaxPlayCalls) {
            next("did not show " + std::string(p.as) + " in its slot 0 after our plays: went on");
            continue;
        }
        const auto index = static_cast<std::uint64_t>(&p - kPlays);
        const std::uint64_t cell = block_ + kPlayNamesAt + kPlayNameCell * index;
        std::array<char, kPlayNameCell> text{};
        std::memcpy(text.data(), p.as.data(), p.as.size());
        std::array<std::uint8_t, 0x30> request{};
        const float rate = 1.0f;
        const float unset = -1.0f;
        const std::int32_t slot = 0;
        std::memcpy(request.data() + 0x00, &actor, 8);
        std::memcpy(request.data() + 0x08, &cell, 8);
        std::memcpy(request.data() + 0x10, &slot, 4);
        std::memcpy(request.data() + 0x14, &rate, 4);
        std::memcpy(request.data() + 0x18, &unset, 4);
        std::memcpy(request.data() + 0x1c, &unset, 4);
        if (!guest.Write(cell, text.data(), text.size()) ||
            !guest.Write(block_ + kPlayRequestAt, request.data(), request.size())) {
            return std::nullopt;  // try again next frame (until kTryMs)
        }
        ++t.play_calls;
        next_ = Next::play;
        CoopPuppet::Call c;
        c.function = main + CoopPuppet::addr::kPlayAs;
        c.x[0] = block_ + kPlayRequestAt;
        return c;
    }
    return std::nullopt;
}

}  // namespace CoopEventResult
