// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_eye_switch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "coop/protocol/codec.hpp"
#include "coop_fairy.h"
#include "coop_live_events.h"
#include "coop_logic_set.h"

namespace CoopEyeSwitch {

namespace {
// tools/puppet_pack/eye_switches.py --places coop_live_places.cpp on the romfs of build e0f3ccfaf50065ae (1.1.1),
// 2026-11-16. 64 lines, 63 events.
// clang-format off
constexpr Switch kSwitches[] = {
    {"Dm_FireTemple_0029", "FireTemple", "Obj_HammerSwitch_A_01", {-49.6f, 179.5f, -24.8f}, "OnState", {-46.9f, 186.5f, -22.4f}},
    {"DodongoCavern_Gimmick_EyeSwitch", "DodongoCavern", "Obj_EyeSwitch_A_05", {80.5f, 121.0f, -6.6f}, "OnState", {68.2f, 127.3f, -6.6f}},
    {"DodongoCavern_Gimmick_SwitchShortcut", "DodongoCavern", "Obj_StepSwitch_A_01", {17.2f, 112.3f, -23.0f}, "FirstOn", {19.0f, 118.0f, -23.0f}},
    {"DodongoCavern_Gimmick_SwitchShutter_Room01", "DodongoCavern", "Obj_StepSwitch_A_01", {25.0f, 103.0f, -23.0f}, "FirstOn", {16.0f, 103.8f, -23.0f}},
    {"DodongoCavern_Gimmick_SwitchShutter_Room12", "DodongoCavern", "Obj_StepSwitch_A_01", {-49.0f, 114.2f, -8.0f}, "FirstOn", {-43.5f, 116.0f, -8.0f}},
    {"FireTemple_06_EyeSwitch", "FireTemple", "Obj_EyeSwitch_A_02", {34.2f, 151.6f, 0.1f}, "FirstOn", {36.7f, 150.8f, 3.9f}},
    {"FireTemple_SwitchFireCamera_Room04", "FireTemple", "Obj_SwitchHit_A_04", {87.8f, 133.1f, 16.1f}, "OnState", {93.1f, 141.0f, 16.3f}},
    {"FireTemple_SwitchFireCamera_Room12", "FireTemple", "Obj_SwitchHit_A_04", {-73.4f, 178.6f, 16.5f}, "OnState", {-67.5f, 187.3f, 14.2f}},
    {"FocusOnGimmick_InsideJabuJabuBelly_02", "InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_SwitchHit_A_03", {21.9f, 83.4f, -38.1f}, "OnState", {20.0f, 79.5f, -35.0f}},
    {"FocusOnGimmick_JabujabuLift", "InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_SwitchHit_A_01", {-4.3f, 81.0f, -42.6f}, "OnState", {-1.0f, 77.5f, -41.2f}},
    {"ForestTemple_Room02_ShutterOpen", "ForestTemple", "Obj_StepSwitch_A_01", {17.1f, 100.0f, -17.2f}, "FirstOn", {16.4f, 101.7f, -12.3f}},
    {"ForestTemple_Room11_TBoxAppear", "ForestTemple", "Obj_EyeSwitch_A_02", {-52.8f, 119.5f, 5.9f}, "OnState", {-52.8f, 115.0f, 2.0f}},
    {"ForestTemple_Room12_WaterLevelDrop", "ForestTemple", "Obj_StepSwitch_A_01", {-31.9f, 113.9f, -50.7f}, "FirstOn", {-29.1f, 107.4f, -34.9f}},
    {"ForestTemple_Room17_ShutterOpen", "ForestTemple", "Obj_EyeSwitch_A_02", {-13.8f, 118.9f, -59.5f}, "OnState", {-7.5f, 121.5f, -59.5f}},
    {"ForestTemple_Room22_ShutterOpen", "ForestTemple", "Obj_StepSwitch_A_01", {51.2f, 104.0f, -37.2f}, "FirstOn", {54.3f, 104.9f, -19.8f}},
    {"ForestTemple_Room27_CandleLight", "ForestTemple", "Obj_StepSwitch_A_01", {-15.9f, 60.7f, -19.5f}, "OnState", {-10.4f, 61.0f, 6.9f}},
    {"ForestTemple_Room27_ShutterOpen", "ForestTemple", "Obj_EyeSwitch_A_02", {24.9f, 62.1f, -3.5f}, "OnState", {0.0f, 65.7f, -19.6f}},
    {"GerudoTrainingGround_00_FallTreasureBox", "GerudoTrainingGround", "Obj_EyeSwitch_A_02", {-1.8f, 109.8f, 10.8f}, "OnState", {2.5f, 99.5f, -9.8f}},
    {"GerudoTrainingGround_03_ShutterOpen", "GerudoTrainingGround", "Obj_EyeSwitch_GlassTruth_A_02", {-47.3f, 109.2f, -110.6f}, "OnState", {-36.3f, 119.5f, -92.9f}},
    {"GerudoTrainingGround_04_ActStoneGimmick", "GerudoTrainingGround", "Obj_EyeSwitch_A_05", {24.1f, 102.5f, -82.4f}, "OnState", {13.1f, 99.9f, -87.5f}},
    {"GerudoTrainingGround_05_DisappearFire", "GerudoTrainingGround", "Obj_StepSwitch_A_04", {52.2f, 97.5f, -86.0f}, "OnState", {44.9f, 107.6f, -84.4f}},
    {"GerudoTrainingGround_05_ShutterOpen", "GerudoTrainingGround", "Obj_EyeSwitch_A_03", {46.9f, 101.9f, -92.4f}, "FirstOn", {34.4f, 108.5f, -89.8f}},
    {"GerudoTrainingGround_06_DisappearFire", "GerudoTrainingGround", "Obj_StepSwitch_A_02", {32.5f, 94.0f, -40.0f}, "OnState", {32.8f, 107.6f, -35.6f}},
    {"GerudoTrainingGround_08_FenceFallDown", "GerudoTrainingGround", "Obj_StepSwitch_A_01", {-1.9f, 98.0f, -45.1f}, "FirstOn", {0.7f, 101.1f, -45.0f}},
    {"GerudoTrainingGround_10_FallTreasureBox", "GerudoTrainingGround", "Obj_EyeSwitch_A_02", {-54.2f, 109.1f, -131.7f}, "OnState", {-51.4f, 103.5f, -131.8f}},
    {"InsideGanonCastle_Dungeon_AppaerTreasureBox_08", "InsideGanonCastle", "Obj_StepSwitch_A_01", {63.6f, 102.9f, -110.3f}, "OnState", {47.2f, 103.9f, -103.4f}},
    {"InsideGanonCastle_Dungeon_AppearExtendingPlatform_03", "InsideGanonCastle", "Obj_StepSwitch_A_01", {46.3f, 99.0f, 50.0f}, "OnState", {39.7f, 102.8f, 41.4f}},
    {"InsideGanonCastle_Dungeon_EnemyAppear01", "InsideGanonCastle", "Obj_SolarSwitch_A_03", {-38.4f, 108.2f, 39.9f}, "isLightMax", {-35.2f, 109.1f, 38.6f}},
    {"InsideGanonCastle_Dungeon_EnemyAppear02", "InsideGanonCastle", "Obj_SolarSwitch_A_03", {-28.0f, 108.2f, 46.0f}, "isLightMax", {-28.5f, 110.2f, 45.3f}},
    {"InsideGanonCastle_Dungeon_EnemyAppear03", "InsideGanonCastle", "Obj_SolarSwitch_A_03", {-22.0f, 108.2f, 35.5f}, "isLightMax", {-24.5f, 110.0f, 35.3f}},
    {"InsideGanonCastle_Dungeon_ShutterOpen_06_01", "InsideGanonCastle", "Obj_HammerSwitch_A_01", {78.8f, 102.8f, -38.4f}, "OnState", {84.6f, 107.9f, -27.2f}},
    {"InsideGanonCastle_Dungeon_ShutterOpen_08_01", "InsideGanonCastle", "Obj_StepSwitch_A_01", {45.6f, 105.5f, -123.6f}, "OnState", {46.3f, 109.1f, -126.4f}},
    {"InsideGanonCastle_Dungeon_ShutterOpen_17_01", "InsideGanonCastle", "Obj_SwitchHit_A_02", {-48.1f, 105.1f, 12.5f}, "OnState", {-46.4f, 114.6f, 24.6f}},
    {"InsideGanonCastle_Dungeon_ShutterOpen_17_02", "InsideGanonCastle", "Obj_SolarSwitch_A_02", {-32.3f, 108.2f, 29.5f}, "isLightMax", {-29.4f, 114.1f, 25.1f}},
    {"InsideJabuJabuBellyRoom01StepSwitch", "InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_StepSwitch_A_03", {8.5f, 102.5f, -51.4f}, "OnState", {7.3f, 109.7f, -48.1f}},
    {"InsideJabuJabuBellyRoom05Swich", "InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_SwitchHit_A_02", {31.1f, 111.8f, -56.8f}, "OnState", {26.8f, 106.4f, -57.4f}},
    {"InsideJabuJabuBellyRoom07StepSwitch", "InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_StepSwitch_A_03", {7.7f, 92.3f, -141.3f}, "OnState", {15.0f, 100.7f, -138.8f}},
    {"InsideJabuJabuBellyRoom14StepSwitch", "InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_StepSwitch_A_04", {29.2f, 77.5f, -66.7f}, "OnState", {32.0f, 78.0f, -69.6f}},
    {"InsideJabuJabuBellyRoom14StepSwitch_Under", "InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_StepSwitch_A_04", {28.9f, 74.5f, -58.5f}, "OnState", {26.5f, 78.0f, -69.6f}},
    {"InsideJabuJabuBelly_FocusOnGimmick", "InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_SwitchHit_A_01", {-0.0f, 97.5f, -21.5f}, "OnState", {-1.5f, 92.6f, -24.6f}},
    {"InsideTheDekuTree_Gimmick_CandleAndSpiderWeb", "InsideTheDekuTree", "Obj_StepSwitch_A_05", {-10.2f, 79.4f, -9.0f}, "FirstOn", {-10.0f, 82.5f, -9.5f}},
    {"InsideTheDekuTree_Gimmick_FallingLadder", "InsideTheDekuTree", "Obj_EyeSwitch_A_02", {-35.6f, 114.3f, 10.7f}, "OnState", {-37.5f, 117.8f, 12.5f}},
    {"InsideTheDekuTree_Gimmick_RoomD_PlatformUp", "InsideTheDekuTree", "Obj_StepSwitch_A_02", {-17.5f, 119.6f, -4.9f}, "OnState", {-22.6f, 122.6f, -3.8f}},
    {"InsideTheDekuTree_Gimmick_RoomF_EyeSwitch", "InsideTheDekuTree", "Obj_EyeSwitch_A_02", {-17.1f, 83.0f, 29.6f}, "OnState", {-14.5f, 82.7f, 27.8f}},
    {"InsideTheDekuTree_Gimmick_RoomG_WaterLevelDrop", "InsideTheDekuTree", "Obj_DiveSwitch_A_01", {-25.6f, 74.2f, 40.6f}, "OnState", {-23.5f, 81.0f, 34.5f}},
    {"SpiritTemple_Level_Room_03_BoardNetFall", "SpiritTemple", "Obj_SwitchHit_A_03", {-51.9f, 104.7f, -58.6f}, "OnState", {-55.0f, 107.3f, -51.8f}},
    {"SpiritTemple_Level_Room_04_EyeSwitchOn", "SpiritTemple", "Obj_EyeSwitch_A_02", {-33.6f, 104.0f, -86.0f}, "FirstOn", {-31.3f, 107.9f, -81.7f}},
    {"SpiritTemple_Level_Room_10_SwitchOn_01", "SpiritTemple", "Obj_SolarSwitch_A_01", {-44.5f, 131.5f, -26.0f}, "isLightMax", {-59.0f, 132.8f, -27.7f}},
    {"SpiritTemple_Level_Room_19_SwitchOn_01", "SpiritTemple", "Obj_SolarSwitch_A_02", {28.0f, 121.5f, -40.1f}, "isLightMax", {31.4f, 120.0f, -41.6f}},
    {"SpiritTemple_Level_Room_19_SwitchOn_01", "SpiritTemple", "Obj_SolarSwitch_A_02", {45.8f, 121.5f, -47.5f}, "isLightMax", {40.9f, 119.2f, -47.5f}},
    {"SpiritTemple_Level_Room_19_wallmaster", "SpiritTemple", "Obj_SolarSwitch_A_03", {42.8f, 121.5f, -40.1f}, "isLightMax", {41.9f, 125.7f, -41.9f}},
    {"SpiritTemple_Level_Room_30_BoardNetFall", "SpiritTemple", "Obj_EyeSwitch_A_03", {14.4f, 152.7f, -76.7f}, "FirstOn", {19.8f, 159.5f, -77.8f}},
    {"SpiritTemple_Level_Room_32_SwitchOn_01", "SpiritTemple", "Obj_SolarSwitch_A_02", {-21.2f, 162.9f, -39.9f}, "isLightMax", {-23.8f, 159.9f, -49.7f}},
    {"SpiritTemple_Room19_SunSwitchShutterOpen", "SpiritTemple", "Obj_SolarSwitch_A_02", {35.4f, 121.5f, -37.2f}, "isLightMax", {27.1f, 120.7f, -47.6f}},
    {"SpritTemple_Room07_AppearTreasure", "SpiritTemple", "Obj_SwitchHit_A_05", {-35.2f, 123.5f, -38.0f}, "OnState", {-39.2f, 119.2f, -41.9f}},
    {"SpritTemple_Room08_OpenShutter", "SpiritTemple", "Obj_HammerSwitch_A_01", {18.0f, 129.1f, -68.5f}, "OnState", {1.5f, 118.4f, -51.4f}},
    {"SpritTemple_Room14_OpenShutter", "SpiritTemple", "Obj_SwitchHit_A_02", {33.6f, 111.8f, -30.7f}, "OnState", {33.4f, 104.8f, -34.3f}},
    {"SpritTemple_Room22_StepSwitch", "SpiritTemple", "Obj_StepSwitch_A_01", {47.1f, 130.0f, -24.6f}, "FirstOn", {46.5f, 134.0f, -25.3f}},
    {"SpritTemple_Room23_SunSwitch", "SpiritTemple", "Obj_SolarSwitch_A_02", {55.8f, 137.3f, -41.1f}, "isLightMax", {55.9f, 133.0f, -33.6f}},
    {"SpritTemple_Room30_StepSwitch", "SpiritTemple", "Obj_StepSwitch_A_01", {19.8f, 151.0f, -82.2f}, "FirstOn", {23.1f, 150.9f, -71.4f}},
    {"SpritTemple_Room31_OpenShutter", "SpiritTemple", "Obj_SwitchHit_A_05", {0.0f, 153.1f, -46.0f}, "OnState", {-3.0f, 158.1f, -46.9f}},
    {"WaterTemple_FocusonGimmick_02", "WaterTemple", "Obj_SwitchHit_A_02", {2.0f, 90.5f, -4.5f}, "OnState", {-6.7f, 89.6f, -4.5f}},
    {"WaterTemple_Room06_Waterlevelrise_FocusOnGimmick", "WaterTemple", "Obj_SwitchHit_A_06", {-78.9f, 120.0f, -15.0f}, "OnState", {-71.8f, 121.2f, -15.1f}},
    {"WaterTemple_Waterlevelrise_FocusOnGimmick", "WaterTemple", "Obj_StepSwitch_A_01", {-27.0f, 100.0f, 20.1f}, "FirstOn", {-35.4f, 103.8f, 17.4f}},
};
// The guard flags (see the header), from the same run with --dungeon coop_dungeon.cpp: 43. A flag the room reads
// only when it loads (a gate with UpdateOnce 1: Deku room D's RoomD_StepSwitch_On_01 deletes the compass asker only
// at a load) is no guard: landing it live deletes nothing.
constexpr Guard kGuards[] = {
    {"FireTemple_FireStair_First_WasOn", "FireTemple", "FireTemple_SwitchFireCamera_Room04"},  // deletes the asker
    {"FireTemple_FireStair_Second_WasOn", "FireTemple", "FireTemple_SwitchFireCamera_Room12"},  // deletes the asker
    {"FireTemple_Room14_HammmerStair_Complete", "FireTemple", "Dm_FireTemple_0029"},  // the trigger reads it negated
    {"ForestTemple_Room02_GoldEyeSwitch_On", "ForestTemple", "ForestTemple_Room02_ShutterOpen"},  // deletes the asker
    {"ForestTemple_Room07_Cleard", "ForestTemple", "ForestTemple_Room12_WaterLevelDrop"},  // deletes the asker
    {"ForestTemple_Room08_EyeSwitch_Clear", "ForestTemple", "ForestTemple_Room11_TBoxAppear"},  // deletes the asker
    {"ForestTemple_Room15_Cleard", "ForestTemple", "ForestTemple_Room22_ShutterOpen"},  // deletes the asker
    {"GerudoTrainingGround_Dungeon_Room00_EyeSwitch", "GerudoTrainingGround", "GerudoTrainingGround_00_FallTreasureBox"},  // deletes the asker
    {"Gimmick_Insidejabujabu_Updown_MoveWater", "InsideJabuJabuBelly", "InsideJabuJabuBellyRoom14StepSwitch_Under"},  // the trigger reads it negated
    {"GerudoTrainingGround_Dungeon_Room03_StepSwitch", "GerudoTrainingGround", "GerudoTrainingGround_03_ShutterOpen"},  // the trigger reads it negated
    {"GerudoTrainingGround_Dungeon_Room04_StoneStatue", "GerudoTrainingGround", "GerudoTrainingGround_04_ActStoneGimmick"},  // the trigger reads it negated
    {"GerudoTrainingGround_Dungeon_Room05_StepSwitch", "GerudoTrainingGround", "GerudoTrainingGround_05_DisappearFire"},  // deletes the asker
    {"GerudoTrainingGround_Dungeon_Room06_Fire", "GerudoTrainingGround", "GerudoTrainingGround_06_DisappearFire"},  // the trigger reads it negated
    {"GerudoTrainingGround_Room10_EyeSwitchTBox", "GerudoTrainingGround", "GerudoTrainingGround_10_FallTreasureBox"},  // deletes the asker
    {"InsideGanonCastle_Dungeon_Room03_StepSwitch", "InsideGanonCastle", "InsideGanonCastle_Dungeon_AppearExtendingPlatform_03"},  // deletes the asker
    {"InsideGanonCastle_Dungeon_Room06_HmmerSwitch", "InsideGanonCastle", "InsideGanonCastle_Dungeon_ShutterOpen_06_01"},  // deletes the asker
    {"InsideGanonCastle_Dungeon_Room08_HummerSwitch", "InsideGanonCastle", "InsideGanonCastle_Dungeon_ShutterOpen_08_01"},  // deletes the asker
    {"InsideGanonCastle_Dungeon_Room08_StepSwitch", "InsideGanonCastle", "InsideGanonCastle_Dungeon_AppaerTreasureBox_08"},  // deletes the asker
    {"InsideGanonCastle_Dungeon_Room17_HitSwitch_Bombchu", "InsideGanonCastle", "InsideGanonCastle_Dungeon_ShutterOpen_17_01"},  // deletes the asker
    {"InsideGanonCastle_Dungeon_Room17_SolarSwitch", "InsideGanonCastle", "InsideGanonCastle_Dungeon_ShutterOpen_17_02"},  // deletes the asker
    {"InsideGanonCastle_Dungeon_Room17_SolarSwitch_Fake01", "InsideGanonCastle", "InsideGanonCastle_Dungeon_EnemyAppear01"},  // deletes the asker
    {"InsideGanonCastle_Dungeon_Room17_SolarSwitch_Fake02", "InsideGanonCastle", "InsideGanonCastle_Dungeon_EnemyAppear02"},  // deletes the asker
    {"InsideGanonCastle_Dungeon_Room17_SolarSwitch_Fake03", "InsideGanonCastle", "InsideGanonCastle_Dungeon_EnemyAppear03"},  // deletes the asker
    {"InsideJabuJabuBelly_Gimmick_Onswitch_first", "InsideJabuJabuBelly", "InsideJabuJabuBellyRoom01StepSwitch"},  // the trigger reads it negated
    {"InsideJabuJabuBelly_Room01_LiftMoveDown_GimmickClear", "InsideJabuJabuBelly", "FocusOnGimmick_JabujabuLift"},  // the trigger reads it negated
    {"InsideJabuJabuBelly_Room14_PlayedSwitchEvent_Lower", "InsideJabuJabuBelly", "InsideJabuJabuBellyRoom14StepSwitch_Under"},  // deletes the asker
    {"InsideJabuJabuBelly_Room14_PlayedSwitchEvent_Upper", "InsideJabuJabuBelly", "InsideJabuJabuBellyRoom14StepSwitch"},  // deletes the asker
    {"InsideTheDekuTree_RoomC_Cleared", "InsideTheDekuTree", "InsideTheDekuTree_Gimmick_FallingLadder"},  // deletes the asker
    {"InsideTheDekuTree_RoomF_Cleared", "InsideTheDekuTree", "InsideTheDekuTree_Gimmick_RoomF_EyeSwitch"},  // deletes the asker
    {"InsideTheDekuTree_RoomG_WaterLevel", "InsideTheDekuTree", "InsideTheDekuTree_Gimmick_RoomG_WaterLevelDrop"},  // deletes the asker
    {"Insidejabujabu_Gimmick_Room7Event", "InsideJabuJabuBelly", "InsideJabuJabuBellyRoom07StepSwitch"},  // deletes the asker + the trigger reads it negated
    {"Insidejabujabu_Room0_Cleared", "InsideJabuJabuBelly", "InsideJabuJabuBelly_FocusOnGimmick"},  // deletes the asker
    {"Insidejabujabu_Room14_Cleared", "InsideJabuJabuBelly", "FocusOnGimmick_InsideJabuJabuBelly_02"},  // the trigger reads it negated
    {"Insidejabujabu_Room5_Cleared", "InsideJabuJabuBelly", "InsideJabuJabuBellyRoom05Swich"},  // deletes the asker
    {"SpiritTemple_Room03_BoardNet_On", "SpiritTemple", "SpiritTemple_Level_Room_03_BoardNetFall"},  // deletes the asker + the trigger reads it negated
    {"SpiritTemple_Room07_TBox_On", "SpiritTemple", "SpritTemple_Room07_AppearTreasure"},  // deletes the asker
    {"SpiritTemple_Room10_Unlocked", "SpiritTemple", "SpiritTemple_Level_Room_10_SwitchOn_01"},  // deletes the asker
    {"SpiritTemple_Room19_LightSwitch_On_01", "SpiritTemple", "SpiritTemple_Level_Room_19_SwitchOn_01"},  // deletes the asker
    {"SpiritTemple_Room19_LightSwitch_On_04", "SpiritTemple", "SpiritTemple_Level_Room_19_wallmaster"},  // deletes the asker
    {"SpiritTemple_Room30_Floor_On", "SpiritTemple", "SpiritTemple_Level_Room_30_BoardNetFall"},  // deletes the asker
    {"SpiritTemple_Room32_TBox_On", "SpiritTemple", "SpiritTemple_Level_Room_32_SwitchOn_01"},  // deletes the asker
    {"WaterTemple_Room02_00", "WaterTemple", "WaterTemple_FocusonGimmick_02"},  // deletes the asker + the trigger reads it negated
    {"WaterTemple_Waterlevelrise_Room06", "WaterTemple", "WaterTemple_Room06_Waterlevelrise_FocusOnGimmick"},  // the trigger reads it negated
};
// Not covered (the tool's list; each with why):
//   ForestTemple_Room17_ShutterOpen: Enemy_Stalfos_Weak.Dead (in an OR)
//   ForestTemple_Room17_ShutterOpen: ForestTemple_After_Appearance_TreasureChest.Bool (in an OR)
//   InsideJabuJabuBellyRoom14StepSwitch: Gimmick_Insidejabujabu_Updown_MoveWater.Bool
//   InsideJabuJabuBellyRoom14StepSwitch: Gimmick_Insidejabujabu_Updown_MoveWaterSwitch.Bool
//   InsideJabuJabuBellyRoom14StepSwitch_Under: Gimmick_Insidejabujabu_Updown_MoveWaterSwitch.Bool
//   SpiritTemple_Level_Room_19_SwitchOn_01: HoldPlayerrCheckEventQueryPlayerState.IsNotState
//   SpiritTemple_Room19_SunSwitchShutterOpen: HoldPlayerrCheckEventQueryPlayerState.IsNotState
//   DodongoCavern_Gimmick_EyeSwitch: DodongoCavern_NaviHelp_14_00 (the trigger reads it negated; not shared: each game's own)
//   WaterTemple_Waterlevelrise_FocusOnGimmick: WaterTemple_Room03_FocusOnGimmick (the trigger reads it negated; not shared: each game's own)
//   -> ForestTemple_Room17: the OR's shared flag (After_Appearance_TreasureChest) comes with the other player's chest
//      event; its Stalfos are shared enemies. Spirit room 19: our Link's own state (IsNotState: not in a held state),
//      true unless he is mid-action. The not-shared flags are each game's own room state (Navi's hint, the Water
//      Temple room-3 camera): the same in both games when both play the room the same
//      way; when not, the event waits for our own switch (the old way after 2 s). Spirit room 10's box IsStop
//      (the extra actor input the tool prints) is the box at rest: true unless it is being pushed. Jabu room 14's
//      MoveWaterSwitch (both lines) is the switches' own arming flag, each game's own on purpose (g3-objects).
// deku-ladder (A #39): tools/puppet_pack/eye_switches.py --places coop_live_places.cpp --dungeon coop_dungeon.cpp
// --classify, same romfs. Class B lines whose switch's ON input is unwired: we set it (see the header). None since
// g3-objects: Jabu room 14's two lines were the only ones, and with the water flags shared (coop_dungeon.cpp) both
// are class A (the upper switch's re-arm ORs `not Updown_MoveWater`, which its event clears); driving our asker
// would now write those shared two-way flags a second time, late, and could flip the water back for everyone. The
// tool refuses such a drive (classify: "our asker would write shared two-way ... again").
constexpr std::array<Input, 0> kInputs{};
// The tool's other lines (not driven; the old way + live flags), class and why:
//   B (none) GerudoTrainingGround_06_DisappearFire (Obj_StepSwitch_A_02): Obj_GerudoTrainingGround_FireWall_A_01.StartBurnSign
//     (the fire's burn sign is an AND pulse of our asker's EventBool and Room06_Fire; the step switch has only BootUp,
//     unwired, and no ForceControl: no native input to set. Known gap: the fire stays until the room reloads.)
//   B (none) InsideTheDekuTree_Gimmick_RoomD_PlatformUp (Obj_StepSwitch_A_02): the three lifts' IsOn (OR of our
//     asker's EventBool and AND(RoomD_StepSwitch_On_01, the plate): the shared flag alone raises nothing, so the
//     result hangs on our asker; row 98(2) sets that EventBool, no replay).
//   A, 37 lines: Dm_FireTemple_0029, FireTemple_SwitchFireCamera_Room04/12, FocusOnGimmick_InsideJabuJabuBelly_02,
//     FocusOnGimmick_JabujabuLift, ForestTemple_Room27_CandleLight, GerudoTrainingGround_05_DisappearFire,
//     InsideGanonCastle_Dungeon_AppaerTreasureBox_08, _AppearExtendingPlatform_03, _EnemyAppear01/02/03,
//     _ShutterOpen_06_01/08_01/17_01/17_02, InsideJabuJabuBellyRoom01StepSwitch, Room05Swich, Room07StepSwitch,
//     Room14StepSwitch, Room14StepSwitch_Under,
//     InsideJabuJabuBelly_FocusOnGimmick, InsideTheDekuTree_Gimmick_RoomG_WaterLevelDrop,
//     SpiritTemple_Level_Room_03_BoardNetFall, _Room_10_SwitchOn_01, _Room_19_SwitchOn_01 (both suns),
//     _Room_19_wallmaster, _Room_32_SwitchOn_01, SpiritTemple_Room19_SunSwitchShutterOpen,
//     SpritTemple_Room07_AppearTreasure, _Room08_OpenShutter, _Room14_OpenShutter, _Room23_SunSwitch,
//     _Room31_OpenShutter, WaterTemple_FocusonGimmick_02, WaterTemple_Room06_Waterlevelrise_FocusOnGimmick.
// clang-format on

float Dist2(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}
}  // namespace

std::span<const Switch> Switches() { return kSwitches; }

std::span<const Guard> Guards() { return kGuards; }

namespace {
std::span<const Input> g_inputs = kInputs;  // the generated table; ScopedInputsForTest swaps it in tests only
}  // namespace

std::span<const Input> Inputs() { return g_inputs; }

ScopedInputsForTest::ScopedInputsForTest(std::span<const Input> lines) : old_(g_inputs) { g_inputs = lines; }
ScopedInputsForTest::~ScopedInputsForTest() { g_inputs = old_; }

const Input* InputOf(const Switch& s) {
    for (const auto& in : g_inputs) {
        if (in.event == s.event && in.scene == s.scene && in.actor == s.actor && in.at == s.at) return &in;
    }
    return nullptr;
}

const Switch* Find(std::string_view event, std::string_view scene) {
    for (const auto& s : kSwitches) {
        if (s.event == event && s.scene == scene) return &s;
    }
    return nullptr;
}

std::vector<const Switch*> FindAll(std::string_view event, std::string_view scene,
                                   const std::optional<std::array<float, 3>>& link) {
    std::vector<const Switch*> out;
    for (const auto& s : kSwitches) {
        if (s.event == event && s.scene == scene && Latches(s)) out.push_back(&s);  // only lines we can drive
    }
    if (link) {
        std::stable_sort(out.begin(), out.end(), [&](const Switch* x, const Switch* y) {
            return Dist2(x->at, *link) < Dist2(y->at, *link);
        });
    }
    return out;
}

std::uint32_t PlugKey(std::string_view plug) { return coop::protocol::flag_hash(plug); }

bool Latches(const Switch& s) {
    return s.actor.starts_with("Obj_EyeSwitch") || s.plug == "FirstOn" || InputOf(s) != nullptr;
}

std::vector<std::uint64_t> FindActors(CoopPuppet::Guest& guest, std::uint64_t main,
                                      std::span<const std::string_view> names, const std::array<float, 3>& at,
                                      float max_m) {
    std::vector<std::pair<float, std::uint64_t>> near;
    CoopFairy::WalkActors(guest, main, [&](std::uint64_t actor) {
        std::array<float, 3> p{};
        if (CoopFairy::ActorPresent(guest, actor) &&
            std::any_of(names.begin(), names.end(),
                        [&](std::string_view n) { return CoopFairy::ActorNamed(guest, actor, n); }) &&
            guest.Read(actor + CoopPuppet::addr::kActorPosition, p.data(), sizeof p)) {
            const float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 <= max_m * max_m) near.emplace_back(d2, actor);
        }
        return false;  // the whole list
    });
    std::sort(near.begin(), near.end());
    std::vector<std::uint64_t> out;
    for (const auto& n : near) out.push_back(n.second);
    return out;
}

bool Switcher::Want(const std::string& event, const std::string& scene, std::optional<std::array<float, 3>> link) {
    if (FindAll(event, scene).empty()) return false;  // not a switch event, or one whose plug its AI rewrites
    std::lock_guard lock(mutex_);
    if (failed_) return false;
    if (std::none_of(wanted_.begin(), wanted_.end(), [&](const Try& t) { return t.event == event; })) {
        Try t;
        t.event = event;
        t.scene = scene;
        t.link = link;
        wanted_.push_back(std::move(t));
    }
    return true;
}

void Switcher::Expire(const std::string& scene, const std::function<bool(const std::string&)>& played) {
    std::lock_guard lock(mutex_);
    if (!scene.empty() && scene != scene_) {  // a new visit: every event may be tried again
        scene_ = scene;
        ended_.clear();
        ran_.clear();
        if (!restores_.empty()) notes_.push_back("eye switch: input stores not taken back (we left the scene: its switches are gone)");
        restores_.clear();
    }
    for (auto it = ran_.begin(); it != ran_.end();) {  // our copy ran: its guards come once our game has it played
        if (played && played(*it)) {
            ended_.push_back(*it);
            it = ran_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = wanted_.begin(); it != wanted_.end();) {
        const bool busy = it == wanted_.begin() && next_ != Next::none;  // a call of ours is out: next Step ends it
        if (!busy && !scene.empty() && it->scene != scene) {
            notes_.push_back("eye switch: " + it->event + " dropped (we left its scene)");
            it = wanted_.erase(it);
        } else if (!busy && played && played(it->event)) {
            notes_.push_back("eye switch: " + it->event + " dropped (our game has it played)");
            if (it->restore) restores_.push_back(*it->restore);
            if (std::find(ended_.begin(), ended_.end(), it->event) == ended_.end()) ended_.push_back(it->event);
            it = wanted_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Switcher::HoldsGuard(std::uint32_t hash, std::string_view scene) {
    std::lock_guard lock(mutex_);
    if (failed_) return false;  // every event goes the old way: nothing waits
    // 0: not held (its event has no line we drive: the flag comes as before). Which events have a line depends on
    // the input lines, so the hashes follow them: a function-local static froze the first call's lines (the full
    // host test run, eye_switch_test:397: a test with no input lines ran first, the input-line test then saw 0).
    const std::span<const Input> inputs = Inputs();
    if (guard_hashes_.size() != std::size(kGuards) || guard_inputs_ != inputs.data() ||
        guard_inputs_size_ != inputs.size()) {
        guard_hashes_.clear();
        for (const Guard& g : kGuards) {
            guard_hashes_.push_back(FindAll(g.event, g.scene).empty() ? 0u : coop::protocol::flag_hash(g.flag));
        }
        guard_inputs_ = inputs.data();
        guard_inputs_size_ = inputs.size();
    }
    const std::vector<std::uint32_t>& hashes = guard_hashes_;
    for (std::size_t i = 0; i < hashes.size(); ++i) {
        const Guard& g = kGuards[i];
        if (hashes[i] != 0 && hashes[i] == hash && g.scene == scene &&
            std::find(ended_.begin(), ended_.end(), g.event) == ended_.end()) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> Switcher::TakeNotes() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
}

std::vector<std::pair<std::string, std::string>> Switcher::TakeFallbacks() {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::string, std::string>> out;
    out.swap(fallbacks_);
    return out;
}

bool Switcher::EventRunning(CoopPuppet::Guest& guest, std::uint64_t main) {
    namespace le = CoopLiveEvents::addr;
    std::uint64_t manager = 0;
    std::uint64_t session = 0;
    if (!guest.Read(main + le::kEventStatePtr, &manager, 8) || manager == 0) return false;
    return guest.Read(manager + le::kEventSession, &session, 8) && session != 0;
}

void Switcher::End(const std::string& why, bool fallback, bool ran) {
    const Try& t = wanted_.front();
    notes_.push_back("eye switch: " + t.event + ": " + why);
    // Every end is final for this visit (the decider asks an event once a visit). Our copy did not run and will not
    // (no asker, the old way): its guard flags come as before from now on. Our copy runs (an event started after our
    // plug, or our own hit): its guards stay held until our game has the event played (Expire's played()), since
    // most of them delete the asker and the other player's value would land in the middle of our event.
    auto& list = ran ? ran_ : ended_;
    if (std::find(list.begin(), list.end(), t.event) == list.end()) list.push_back(t.event);
    if (fallback) fallbacks_.emplace_back(t.event, t.scene);
    if (t.restore) restores_.push_back(*t.restore);  // taken back on the next Step
    wanted_.pop_front();
}

void Switcher::RestoreInputs(CoopPuppet::Guest& guest, std::uint64_t main) {
    for (const auto& r : restores_) {
        // Only into the same plug of the same switch, still at its place (a reload makes new actors and blocks).
        const std::string_view name[] = {r.name};
        const auto here = find_(guest, main, name, r.at, kEyeAtM);
        const auto plug = std::find(here.begin(), here.end(), r.actor) != here.end()
                              ? CoopLogicSet::InputPlug(guest, r.actor, r.key)
                              : std::nullopt;
        char text[200];
        if (plug && plug->value_word == r.value_word && CoopLogicSet::StoreInput(guest, *plug, r.old)) {
            std::snprintf(text, sizeof text, "eye switch: our %.*s 0x%llx input taken back to %u",
                          static_cast<int>(r.name.size()), r.name.data(), static_cast<unsigned long long>(r.actor),
                          static_cast<unsigned>(r.old));
        } else {
            std::snprintf(text, sizeof text, "eye switch: our %.*s 0x%llx input not taken back (the switch or its plug is gone)",
                          static_cast<int>(r.name.size()), r.name.data(), static_cast<unsigned long long>(r.actor));
        }
        notes_.emplace_back(text);
    }
    restores_.clear();
}

std::optional<CoopPuppet::Call> Switcher::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                               std::uint64_t main, std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    if (main != 0 && !restores_.empty()) RestoreInputs(guest, main);
    if (after_call) {
        const Next was = next_;
        next_ = Next::none;
        if (was != Next::malloc) return std::nullopt;  // the execute: one set a frame
        if (result == 0) {
            failed_ = true;
            while (!wanted_.empty()) End("the game's malloc gave nothing: asked the old way", true);
            return std::nullopt;
        }
        block_ = result;
    }
    if (failed_ || main == 0 || wanted_.empty()) return std::nullopt;
    if (block_ == 0) {
        if (after_call) return std::nullopt;
        next_ = Next::malloc;
        return CoopLogicSet::Malloc(main);
    }
    return Frame(guest, main, now_ms);
}

std::optional<CoopPuppet::Call> Switcher::Frame(CoopPuppet::Guest& guest, std::uint64_t main, std::uint64_t now_ms) {
    Try& t = wanted_.front();
    if (t.until_ms == 0) t.until_ms = now_ms + kTryMs;
    const auto lines = FindAll(t.event, t.scene, t.link);
    if (lines.empty()) {
        End("not in the switch table: asked the old way", true);
        return std::nullopt;
    }
    const bool running = EventRunning(guest, main);
    if (t.set && running) {
        End("an event started after our switch's plug (the room's asker asked it)", false, true);
        return std::nullopt;
    }
    if (now_ms > t.until_ms) {
        End(t.set ? "the plug set but no event started in time: asked the old way (cutscene only; plan (b) needed)"
                  : "our switch or its plug not found in time: asked the old way",
            true);
        return std::nullopt;
    }
    if (running) return std::nullopt;  // another event: wait (not set while one runs)
    // The first line (nearest our Link) whose asker is here; then only its own switch, at its placement.
    const Switch* line = nullptr;
    for (const Switch* l : lines) {
        if (!find_(guest, main, CoopLiveEvents::kAskerNames, l->asker_at, CoopLiveEvents::kAskerNearM).empty()) {
            line = l;
            break;
        }
    }
    if (line == nullptr) {
        // No asker: the room is cleared here (its asker deleted itself) or not loaded; the old way would not ask either.
        End(t.set ? "our room's asker is gone after the plug (its event ran)" : "no room asker here: not started",
            false, t.set);
        return std::nullopt;
    }
    const std::string_view switch_name[] = {line->actor};
    const auto eyes = find_(guest, main, switch_name, line->at, kEyeAtM);
    // An input line sets the switch's unwired ON input (InputOf); the others its output plug.
    const Input* input = InputOf(*line);
    const std::string_view plug_name = input ? input->input : line->plug;
    const std::uint32_t key = PlugKey(plug_name);
    std::uint64_t target = 0;
    std::optional<CoopLogicProbe::Plug> target_plug;
    bool any_plug = false;
    for (const std::uint64_t eye : eyes) {
        const auto plug = input ? CoopLogicSet::InputPlug(guest, eye, key) : CoopLogicSet::BoolPlug(guest, eye, key);
        if (!plug) continue;
        any_plug = true;
        if (input && !CoopLogicSet::CanSetInput(*plug)) {
            char text[160];
            std::snprintf(text, sizeof text, "our switch's %.*s input marks its changes (value word low bits %u): "
                          "not set, asked the old way", static_cast<int>(plug_name.size()), plug_name.data(),
                          static_cast<unsigned>(plug->value_word & 3));
            End(text, true);
            return std::nullopt;
        }
        if (plug->value != std::uint8_t{1}) {  // 1: latched, or our own shot
            target = eye;
            target_plug = plug;
        }
        break;
    }
    if (target == 0) {
        if (any_plug && !t.set) {
            End("our switch's " + std::string(plug_name) + " is 1 already (our own hit): nothing to do", false, true);
        } else if (!t.told_waiting) {
            t.told_waiting = true;
            notes_.push_back("eye switch: " + t.event + ": waiting (" + std::to_string(eyes.size()) + " " +
                             std::string(line->actor) + " at its place, " + std::string(plug_name) +
                             (t.set ? " on" : " not a live plug yet") + ")");
        }
        return std::nullopt;
    }
    if (input) {
        // The set-bool's own store into the unwired input; it stays (no source rewrites it), so no call. First the
        // address must be this plug's own on this instance (review Round 8): else the old way.
        const auto mine = CoopLogicSet::AllPlugs(guest, target);
        std::vector<std::uint64_t> others;
        for (const std::uint64_t other : find_(guest, main, switch_name, line->at, kSameKindM)) {
            if (other == target) continue;
            if (const auto o = CoopLogicSet::InputPlug(guest, other, key)) others.push_back(o->value_word);
        }
        const char* why = mine ? CoopLogicSet::InputNotOwn(*target_plug, *mine, others, main) : "its logic is gone";
        if (why != nullptr) {
            char text[256];
            std::snprintf(text, sizeof text, "our switch's %.*s input not set (value 0x%llx, plug 0x%llx: %s): "
                          "asked the old way", static_cast<int>(plug_name.size()), plug_name.data(),
                          static_cast<unsigned long long>(target_plug->value_word),
                          static_cast<unsigned long long>(target_plug->node), why);
            End(text, true);
            return std::nullopt;
        }
        if (!CoopLogicSet::SetInput(guest, *target_plug)) return std::nullopt;
        if (!t.restore) {
            t.restore = Try::Restore{target, key, target_plug->value_word, target_plug->value.value_or(0), line->actor,
                                     line->at};
        }
    } else if (!CoopLogicSet::WriteNode(guest, block_, main, target, key)) {
        return std::nullopt;
    }
    if (!t.set) {
        std::array<float, 3> p{};
        guest.Read(target + CoopPuppet::addr::kActorPosition, p.data(), sizeof p);
        const float d = std::sqrt(Dist2(p, line->asker_at));
        char text[256];
        std::snprintf(text, sizeof text,
                      "eye switch: %s: our %.*s 0x%llx (%.1f m from the asker) %.*s set %s",
                      t.event.c_str(), static_cast<int>(line->actor.size()), line->actor.data(),
                      static_cast<unsigned long long>(target), static_cast<double>(d), static_cast<int>(plug_name.size()),
                      plug_name.data(), input ? "(its unwired input, the set-bool's store)" : "through the game's set-bool");
        notes_.emplace_back(text);
        if (input) {
            std::snprintf(text, sizeof text, "eye switch: %s: input value at 0x%llx (plug 0x%llx), old byte %u",
                          t.event.c_str(), static_cast<unsigned long long>(target_plug->value_word & ~std::uint64_t{3}),
                          static_cast<unsigned long long>(target_plug->node),
                          static_cast<unsigned>(target_plug->value.value_or(0)));
            notes_.emplace_back(text);
        }
    }
    t.set = true;
    if (on_set_) {
        on_set_(t.event);
    }
    if (input) return std::nullopt;
    next_ = Next::execute;
    return CoopLogicSet::Execute(main, block_);
}

}  // namespace CoopEyeSwitch
