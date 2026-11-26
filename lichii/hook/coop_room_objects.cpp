// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_room_objects.h"

#include <cstddef>

namespace CoopRoomObjects {

namespace {
// tools/puppet_pack/room_objects.py on the romfs of build e0f3ccfaf50065ae (1.1.1), 2026-11-16.
// 132 placements, 24 driven.
// clang-format off
constexpr Placement kPlacements[] = {
    {"BottomOfTheWell", "GroundControlActor_ResidentMoveTo", {-36.7f, 124.0f, -35.5f}, ""},
    {"BottomOfTheWell", "GroundControlActor_ResidentMoveTo", {13.6f, 129.1f, -69.6f}, ""},
    {"BottomOfTheWell", "GroundControlActor_ResidentMoveTo", {13.6f, 133.3f, 7.1f}, ""},
    {"BottomOfTheWell", "GroundControlActor_ResidentMoveTo", {13.6f, 137.4f, -42.4f}, ""},
    {"DesertColossus", "GroundControlActor_ResidentMoveTo", {-83.4f, 102.3f, 76.1f}, ""},
    {"DodongoCavern", "GroundControlActor_SimpleMoveTo", {10.5f, 101.0f, -19.0f}, "MoveToPosX"},
    {"DodongoCavern", "Obj_DodongoCave_MoveFloor_A_01", {-10.5f, 96.5f, -23.0f}, ""},
    {"DodongoCavern", "Obj_DodongoCave_MoveFloor_A_01", {-0.0f, 96.5f, -12.6f}, ""},
    {"DodongoCavern", "Obj_DodongoCave_MoveFloor_A_01", {10.5f, 96.5f, -23.0f}, "IsShortCutMove"},
    {"DodongoCavern", "Obj_DodongoCave_MoveFloor_A_01_Airwall", {-10.5f, 96.5f, -23.0f}, ""},
    {"DodongoCavern", "Obj_DodongoCave_MoveFloor_A_01_Airwall", {-0.0f, 96.5f, -12.6f}, ""},
    {"DodongoCavern", "Obj_DodongoCave_MoveFloor_A_01_Airwall", {10.5f, 96.5f, -23.0f}, "with Obj_DodongoCave_MoveFloor_A_01"},
    {"DodongoCavern", "Obj_DodongoCave_MoveFloor_A_02", {10.5f, 100.0f, -23.0f}, ""},
    {"DodongoCavern", "Obj_MoveSpike_A_01", {40.0f, 110.8f, -4.7f}, ""},
    {"DodongoCavern", "Obj_MoveSpike_A_01", {45.9f, 110.8f, -17.1f}, ""},
    {"DreamWorld", "GroundControlActor_ResidentMoveTo", {-0.6f, 52.0f, 58.2f}, ""},
    {"DreamWorld", "Obj_ForestTemple_Elevator_A_01", {0.0f, 94.5f, -3.5f}, "RideOnPlayer"},
    {"DreamWorld", "Obj_InsideJabu_Jabu_ElevatorLift_A_01", {-32.4f, 83.0f, -93.3f}, "RideOnPlayer"},
    {"DreamWorld", "Obj_ShadowTemple_MoveFloor_A_03", {99.4f, 73.6f, -43.6f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {-47.2f, 148.3f, -0.7f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {-41.8f, 181.0f, -26.4f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {-40.6f, 148.3f, -1.1f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {-39.4f, 181.0f, -23.1f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {-36.6f, 181.0f, -19.7f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {-35.4f, 107.5f, 24.8f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {-34.1f, 181.0f, -16.8f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {-30.9f, 181.0f, -14.1f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {-20.5f, 149.5f, -29.3f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {-9.9f, 101.5f, -13.2f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {-9.9f, 103.2f, -5.3f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {33.7f, 107.8f, -42.6f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {39.3f, 107.8f, 46.5f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {47.0f, 153.0f, 28.6f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {48.5f, 149.5f, -39.1f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {74.9f, 145.0f, -1.6f}, ""},
    {"FireTemple", "GroundControlActor_ResidentMoveTo", {80.6f, 145.0f, -1.6f}, ""},
    {"FireTemple", "GroundControlActor_Rotate", {-37.7f, 147.0f, -18.4f}, ""},
    {"FireTemple", "GroundControlActor_Rotate", {-29.3f, 147.0f, 10.2f}, ""},
    {"FireTemple", "GroundControlActor_Rotate", {-22.9f, 147.0f, -6.7f}, ""},
    {"FireTemple", "GrouondControlActor_DiffentSpeedMoveTo", {40.4f, 196.6f, -1.3f}, "MoveToPosX+UseFastLinearVel"},
    {"FireTemple", "GrouondControlActor_DiffentSpeedMoveTo", {43.5f, 196.6f, 2.4f}, "MoveToPosX+UseFastLinearVel"},
    {"FireTemple", "Obj_FireTemple_MoveFloor_A_01", {19.3f, 104.5f, 27.8f}, ""},
    {"FireTemple", "Obj_FireTemple_MoveFloor_A_01", {39.0f, 104.5f, -5.9f}, ""},
    {"FireTemple", "Obj_FireTemple_MoveFloor_A_01", {48.5f, 104.5f, -6.4f}, ""},
    {"FireTemple", "Obj_FireTemple_MoveFloor_A_01", {57.0f, 104.5f, 26.8f}, ""},
    {"FireTemple", "Obj_FireTemple_MoveFloor_A_02", {19.2f, 104.5f, 21.5f}, ""},
    {"FireTemple", "Obj_FireTemple_MoveFloor_A_02", {48.0f, 104.5f, -24.5f}, ""},
    {"ForestTemple", "GroundControlActor_ResidentMoveTo", {-55.5f, 124.4f, -79.1f}, ""},
    {"ForestTemple", "GroundControlActor_ResidentMoveTo", {-24.3f, 110.1f, -32.6f}, ""},
    {"ForestTemple", "Obj_ForestTemple_Elevator_A_01", {-77.2f, 120.0f, -1.0f}, "IsInPlayer+IsTargetFloor+RideOnPlayer"},
    {"ForestTemple", "Obj_ForestTemple_Elevator_A_01", {-77.2f, 125.5f, -1.0f}, "IsInPlayer+IsTargetFloor+RideOnPlayer"},
    {"ForestTemple", "Obj_ForestTemple_Elevator_A_01", {0.0f, 94.5f, -3.5f}, "RideOnPlayer"},
    {"ForestTemple", "Obj_ForestTemple_Elevator_A_01", {0.0f, 100.0f, -3.5f}, "IsInPlayer+IsTargetFloor+RideOnPlayer"},
    {"GerudoFortress", "Land_GerudoValley_lift_A_01", {43.6f, 122.1f, -70.9f}, ""},
    {"GerudoTrainingGround", "GroundControlActor_ResidentMoveTo", {-2.0f, 103.8f, -105.0f}, ""},
    {"GerudoTrainingGround", "GroundControlActor_Rotate", {-2.2f, 107.9f, -82.1f}, "RotateAxisX"},
    {"GerudoTrainingGround", "Obj_Gerudo_RotateFloor_A_01", {-2.4f, 95.4f, -82.4f}, ""},
    {"GerudoTrainingGround", "Obj_MoveSpike_A_02", {67.3f, 75.9f, -42.1f}, ""},
    {"GerudoValley", "Land_GerudoValley_lift_A_01", {-7.4f, 100.9f, -11.6f}, ""},
    {"IceCavern", "Obj_IceCave_RotateBlade_A_01", {7.5f, 100.1f, -13.4f}, "BootUp+IsPreEvent+IsResetAngle"},
    {"IceCavern", "Obj_MoveSpike_A_01", {-7.1f, 99.8f, 35.2f}, ""},
    {"IceCavern", "Obj_MoveSpike_A_01", {6.7f, 99.8f, 28.1f}, ""},
    {"IceCavern", "Obj_MoveSpike_A_01", {37.5f, 104.0f, 1.3f}, ""},
    {"IceCavern", "Obj_MoveSpike_A_01", {45.5f, 103.7f, 7.3f}, ""},
    {"IceCavern", "Obj_MoveSpike_A_02", {15.2f, 103.2f, -58.8f}, ""},
    {"InsideGanonCastle", "GroundControlActor_ResidentMoveTo", {47.0f, 99.6f, 37.9f}, ""},
    {"InsideGanonCastle", "GroundControlActor_Rotate", {-39.4f, 103.0f, -61.7f}, ""},
    {"InsideGanonCastle", "Obj_InsideGanonCastle_ExtendingPlatform_A_01", {42.0f, 100.9f, 40.8f}, ""},
    {"InsideGanonCastle", "Obj_MoveSpike_A_01", {-29.6f, 103.9f, 20.3f}, ""},
    {"InsideGanonCastle", "Obj_MoveSpike_A_01", {-23.9f, 103.9f, 10.4f}, ""},
    {"InsideGanonCastle", "Obj_MoveSpike_A_01", {-21.3f, 103.9f, 28.5f}, ""},
    {"InsideGanonCastle", "Obj_MoveSpike_A_01", {-18.2f, 103.9f, 22.8f}, ""},
    {"InsideGanonCastle", "Obj_MoveSpike_A_01", {-15.2f, 103.9f, 17.7f}, ""},
    {"InsideJabuJabuBelly", "GroundControlActor_ResidentMoveTo", {42.4f, 84.2f, -67.9f}, ""},
    {"InsideJabuJabuBelly", "GroundControlActor_ResidentMoveTo", {45.7f, 83.9f, -56.6f}, ""},
    {"InsideJabuJabuBelly", "GroundControlActor_ResidentMoveTo", {46.1f, 83.9f, -51.7f}, ""},
    {"InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_ElevatorLift_A_01", {-32.4f, 83.0f, -93.3f}, "IsTargetFloor+RideOnPlayer"},
    {"InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_ElevatorLift_A_02", {-32.4f, 81.8f, -93.3f}, "IsInPlayer+IsTargetFloor+RideOnPlayer"},
    {"InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_MoveFloor_A_01", {0.3f, 91.0f, -42.6f}, "IsInPlayer+IsTargetFloor+RideOnPlayer"},
    {"InsideTheDekuTree", "GroundControlActor_ResidentMoveTo", {-27.5f, 87.5f, 28.1f}, ""},
    {"InsideTheDekuTree", "GroundControlActor_ResidentMoveTo", {-0.6f, 52.0f, 58.2f}, ""},
    {"InsideTheDekuTree", "GroundControlActor_Rotate", {-29.7f, 80.0f, 40.7f}, ""},
    {"InsideTheDekuTree", "GroundControlActor_WaveLoop", {-26.2f, 82.3f, 38.0f}, ""},
    {"InsideTheDekuTree", "Obj_Deku_MoveFloor_A_01", {-30.2f, 75.4f, 37.0f}, ""},
    {"InsideTheDekuTree", "Obj_Deku_MoveFloor_A_02", {-27.9f, 74.0f, 36.3f}, ""},
    {"InsideTheDekuTree", "Obj_InsideTheDekuTree_ElevatorLiftLim_A_01", {-25.4f, 114.4f, -1.6f}, "with Obj_InsideTheDekuTree_ElevatorLift_A_01"},
    {"InsideTheDekuTree", "Obj_InsideTheDekuTree_ElevatorLiftLim_A_02", {-29.9f, 114.4f, 3.2f}, "with Obj_InsideTheDekuTree_ElevatorLift_A_02"},
    {"InsideTheDekuTree", "Obj_InsideTheDekuTree_ElevatorLiftLim_A_03", {-32.7f, 114.0f, -2.0f}, "with Obj_InsideTheDekuTree_ElevatorLift_A_03"},
    {"InsideTheDekuTree", "Obj_InsideTheDekuTree_ElevatorLift_A_01", {-25.4f, 114.4f, -1.6f}, "IsOn"},
    {"InsideTheDekuTree", "Obj_InsideTheDekuTree_ElevatorLift_A_02", {-29.9f, 114.4f, 3.2f}, "IsOn"},
    {"InsideTheDekuTree", "Obj_InsideTheDekuTree_ElevatorLift_A_03", {-32.7f, 114.0f, -2.0f}, "IsOn"},
    {"KakarikoVillage", "GroundControlActor_ResidentMoveTo", {-47.5f, 118.9f, -12.3f}, ""},
    {"KakarikoVillage", "GroundControlActor_ResidentMoveTo", {-30.6f, 119.5f, -13.6f}, ""},
    {"KakarikoVillage", "GroundControlActor_ResidentMoveTo", {-23.6f, 101.2f, 46.4f}, ""},
    {"Market", "GroundControlActor_SimpleMoveTo", {-43.1f, 102.2f, 1.4f}, "MoveToPosX"},
    {"ShadowTemple", "Obj_MoveSpike_A_01", {20.6f, 56.4f, 87.4f}, ""},
    {"ShadowTemple", "Obj_MoveSpike_A_01", {33.4f, 52.5f, 33.1f}, ""},
    {"ShadowTemple", "Obj_MoveSpike_A_01", {61.0f, 56.0f, 18.4f}, ""},
    {"ShadowTemple", "Obj_MoveSpike_A_01", {65.5f, 56.0f, 18.4f}, ""},
    {"ShadowTemple", "Obj_MoveSpike_A_01", {106.1f, 52.0f, -13.5f}, ""},
    {"ShadowTemple", "Obj_MoveSpike_A_01", {112.1f, 52.0f, -21.9f}, ""},
    {"ShadowTemple", "Obj_MoveSpike_A_02", {49.8f, 56.4f, 80.0f}, ""},
    {"ShadowTemple", "Obj_MoveSpike_A_02", {53.4f, 56.4f, 80.0f}, ""},
    {"ShadowTemple", "Obj_ShadowTemple_MoveFloor_A_01", {56.9f, 51.0f, 57.9f}, ""},
    {"ShadowTemple", "Obj_ShadowTemple_MoveFloor_A_02", {71.2f, 58.2f, 81.2f}, ""},
    {"ShadowTemple", "Obj_ShadowTemple_MoveFloor_A_02", {71.2f, 59.1f, 92.3f}, ""},
    {"ShadowTemple", "Obj_ShadowTemple_MoveFloor_A_02", {81.8f, 60.0f, 95.3f}, ""},
    {"ShadowTemple", "Obj_ShadowTemple_MoveFloor_A_03", {94.4f, 73.2f, 47.4f}, ""},
    {"ShadowTemple", "Obj_ShadowTemple_MoveFloor_A_03", {109.7f, 49.8f, -36.7f}, ""},
    {"ShadowTemple", "Obj_ShadowTemple_MoveFloor_A_04", {99.4f, 78.0f, -43.6f}, ""},
    {"ShadowTemple", "Obj_ShadowTemple_RotateBlade_A_01", {83.9f, 87.2f, -29.7f}, ""},
    {"ShadowTemple", "Obj_ShadowTemple_RotateBlade_GlassTruth_A_01", {145.4f, 61.4f, 96.7f}, ""},
    {"ShadowTemple", "Obj_ShadowTemple_RotateFloor_A_01", {-215.4f, 41.3f, -43.6f}, "IsOn"},
    {"SpiritTemple", "GroundControlActor_ResidentMoveTo", {-0.0f, 157.0f, -54.7f}, ""},
    {"SpiritTemple", "Land_SpiritTemple_ElevatorMirror_A_01", {0.0f, 157.0f, -56.7f}, ""},
    {"SpiritTemple", "Land_SpiritTemple_ElevatorPit_A_01", {0.0f, 153.1f, -54.7f}, ""},
    {"SpiritTemple", "Obj_MoveSpike_A_01", {-31.3f, 103.3f, -23.5f}, ""},
    {"SpiritTemple", "Obj_MoveSpike_A_02", {-33.6f, 104.8f, -34.3f}, ""},
    {"SpiritTemple", "Obj_MoveSpike_A_02", {33.6f, 104.8f, -34.8f}, ""},
    {"WaterTemple", "GroundControlActor_ResidentMoveTo", {-80.0f, 119.5f, 7.5f}, ""},
    {"WaterTemple", "GroundControlActor_ResidentMoveTo", {-50.5f, 119.5f, 7.5f}, ""},
    {"WaterTemple", "GroundControlActor_ResidentMoveTo", {-39.2f, 114.6f, 8.6f}, ""},
    {"WaterTemple", "GroundControlActor_ResidentMoveTo", {-35.0f, 170.0f, -7.0f}, ""},
    {"WaterTemple", "GroundControlActor_ResidentMoveTo", {-13.2f, 111.3f, -114.6f}, ""},
    {"WaterTemple", "GroundControlActor_ResidentMoveTo", {-12.0f, 170.0f, 46.9f}, ""},
    {"WaterTemple", "GroundControlActor_ResidentMoveTo", {-4.0f, 111.3f, -114.6f}, ""},
    {"WaterTemple", "GroundControlActor_ResidentMoveTo", {25.7f, 170.0f, 18.8f}, ""},
    {"WaterTemple", "GroundControlActor_ResidentMoveTo", {55.5f, 170.0f, 22.1f}, ""},
    {"WaterTemple", "Obj_MoveSpike_A_03", {-5.0f, 128.3f, -56.8f}, ""},
    {"WaterTemple", "Obj_MoveSpike_A_03", {-4.5f, 125.9f, -48.0f}, ""},
    {"WaterTemple", "Obj_MoveSpike_A_03", {-3.0f, 130.0f, -60.2f}, ""},
    {"ZoraRiver", "GroundControlActor_ResidentMoveTo", {12.7f, 109.6f, -44.1f}, ""},
};

constexpr std::string_view kRiderNames[] = {
    "Obj_FireTemple_RiseFloor_A_02",
    "Obj_ForestTemple_Elevator_A_01",
    "Obj_InsideGanonCastle_ElevatedFloor_A_01",
    "Obj_InsideJabu_Jabu_ElevatorLift_A_01",
    "Obj_InsideJabu_Jabu_ElevatorLift_A_02",
    "Obj_InsideJabu_Jabu_MoveFloor_A_01",
};

// Placement groups with a saved flag (room_objects.py --groups; fable-g3-objects): the scene's PlgGdTable gives the
// group's flag, its GroundControlActor_ResidentMoveTo (at `place`) moves the members when the flag changes.
// 46 groups.
constexpr GroupFlag kGroupFlags[] = {
    {"BottomOfTheWell", "BottomOfTheWell_Room_01_Blow_Ocarina", false, {-36.7f, 124.0f, -35.5f}, "Obj_WaterPlaneDynamic_A_01"},
    {"BottomOfTheWell", "BottomOfTheWell_Room_01_Blow_Ocarina", false, {13.6f, 129.1f, -69.6f}, "Obj_Well_WaterPlane_A_03"},
    {"BottomOfTheWell", "BottomOfTheWell_Room_01_Blow_Ocarina", false, {13.6f, 133.3f, 7.1f}, "Obj_Well_WaterPlane_A_04"},
    {"BottomOfTheWell", "BottomOfTheWell_Room_01_Blow_Ocarina", false, {13.6f, 137.4f, -42.4f}, "Obj_Well_WaterPlane_A_02"},
    {"DesertColossus", "DessertClossus_IsStormSongPlayed", false, {-83.4f, 102.3f, 76.1f}, "Obj_WaterPlane_Field_A_01"},
    {"DreamWorld", "DreamWorld_BossAppear", false, {-0.6f, 52.0f, 58.2f}, "EventRequestActor+Obj_InsideTheDekuTree_SwitchShutter_A_01"},
    {"FireTemple", "FireTemple_Room14_AirWallEnemyMove00", false, {-41.8f, 181.0f, -26.4f}, "FldObj_AirWall_DedicatedEnemy"},
    {"FireTemple", "FireTemple_Room14_AirWallEnemyMove01", false, {-39.4f, 181.0f, -23.1f}, "FldObj_AirWall_DedicatedEnemy"},
    {"FireTemple", "FireTemple_Room14_AirWallEnemyMove02", false, {-36.6f, 181.0f, -19.7f}, "FldObj_AirWall_DedicatedEnemy"},
    {"FireTemple", "FireTemple_Room14_AirWallEnemyMove03", false, {-34.1f, 181.0f, -16.8f}, "FldObj_AirWall_DedicatedEnemy"},
    {"FireTemple", "FireTemple_Room14_AirWallEnemyMove04", false, {-30.9f, 181.0f, -14.1f}, "FldObj_AirWall_DedicatedEnemy"},
    {"FireTemple", "Npc_Goron_001_ShutterisOpen", false, {33.7f, 107.8f, -42.6f}, "Obj_FireTemple_PrisonCage_A_01"},
    {"FireTemple", "Npc_Goron_002_ShutterisOpen", false, {74.9f, 145.0f, -1.6f}, "Obj_FireTemple_PrisonCage_A_01"},
    {"FireTemple", "Npc_Goron_002_ShutterisOpen", false, {80.6f, 145.0f, -1.6f}, "Obj_FireTemple_PrisonCage_A_01"},
    {"FireTemple", "Npc_Goron_003_ShutterisOpen", false, {39.3f, 107.8f, 46.5f}, "Obj_FireTemple_PrisonCage_A_01"},
    {"FireTemple", "Npc_Goron_004_ShutterisOpen", false, {-20.5f, 149.5f, -29.3f}, "Obj_FireTemple_PrisonCage_A_01"},
    {"FireTemple", "Npc_Goron_005_ShutterisOpen", false, {-35.4f, 107.5f, 24.8f}, "Obj_FireTemple_PrisonCage_A_01"},
    {"FireTemple", "Npc_Goron_007_ShutterisOpen", false, {48.5f, 149.5f, -39.1f}, "Obj_FireTemple_PrisonCage_A_01"},
    {"FireTemple", "Npc_Goron_008_ShutterisOpen", false, {47.0f, 153.0f, 28.6f}, "Obj_FireTemple_PrisonCage_A_01"},
    {"FireTemple", "Npc_Goron_009_ShutterisOpen", false, {-47.2f, 148.3f, -0.7f}, "Obj_FireTemple_PrisonCage_A_01"},
    {"FireTemple", "Npc_Goron_009_ShutterisOpen", false, {-40.6f, 148.3f, -1.1f}, "Obj_FireTemple_PrisonCage_A_01"},
    {"FireTemple", "Npc_Goron_010_ShutterisOpen", false, {-9.9f, 101.5f, -13.2f}, "Obj_FireTemple_PrisonCage_A_01"},
    {"FireTemple", "Npc_Goron_010_ShutterisOpen", false, {-9.9f, 103.2f, -5.3f}, "Obj_FireTemple_PrisonCage_A_01"},
    {"ForestTemple", "ForestTemple_Room07_Cleard", false, {-24.3f, 110.1f, -32.6f}, "EventRequestActor+Obj_ForestTemple_WaterPlaneDynamic_A_01"},
    {"ForestTemple", "ForestTemple_SageOfForest_TwistedCorridor_A_03", false, {-55.5f, 124.4f, -79.1f}, "EventRequestActor+Obj_TBox_A_03_NoBake"},
    {"GerudoTrainingGround", "GerudoTrainingGround_Dungeon_Room04_StoneStatue", false, {-2.0f, 103.8f, -105.0f}, "Land_GerudoTrainingGround_Lava_A_01"},
    {"InsideGanonCastle", "InsideGanonCastle_ExtendingPlatform_ON", false, {47.0f, 99.6f, 37.9f}, "EventRequestActor+Obj_HookShotTarget_A_09+Obj_InsideGanonCastle_ExtendingPlatform_A_01"},
    {"InsideJabuJabuBelly", "Gimmick_Insidejabujabu_Updown_MoveWater", false, {42.4f, 84.2f, -67.9f}, "FldObj_AirWall_DedicatedEnemy+Obj_WaterPlaneDynamic_A_01"},
    {"InsideJabuJabuBelly", "Gimmick_Insidejabujabu_down_MoveWater", false, {45.7f, 83.9f, -56.6f}, "FldObj_AirWall_DedicatedEnemy"},
    {"InsideJabuJabuBelly", "Gimmick_Insidejabujabu_down_MoveWater", false, {46.1f, 83.9f, -51.7f}, "FldObj_AirWall_DedicatedEnemy"},
    {"InsideTheDekuTree", "InsideTheDekuTree_BossArea_ShutIn", false, {-0.6f, 52.0f, 58.2f}, "EventRequestActor+Obj_InsideTheDekuTree_SwitchShutter_A_01"},
    {"InsideTheDekuTree", "InsideTheDekuTree_RoomG_WaterLevel_timing", false, {-27.5f, 87.5f, 28.1f}, "Obj_WaterPlaneDynamic_A_01"},
    {"KakarikoVillage", "KakarikoVillage_Npc_Hylia_009_Child_OcarinaPlayedFirsttime", false, {-23.6f, 101.2f, 46.4f}, "Obj_WaterPlaneCircle_A_01"},
    {"KakarikoVillage", "Npc_Hylia_Soldier_003_GateClose", false, {-30.6f, 119.5f, -13.6f}, "Obj_Kakariko_SwitchShutter_A_01"},
    {"KakarikoVillage", "Npc_Hylia_Soldier_003_GateOpen", false, {-47.5f, 118.9f, -12.3f}, "EventProxyRequestActor+Obj_Kakariko_SwitchShutter_A_01"},
    {"SpiritTemple", "SpiritTemple_Room31_Lift_On", false, {-0.0f, 157.0f, -54.7f}, "EventRequestActor+Obj_HookShotTarget_A_14+Obj_SpiritTemple_HangingFloor_A_01"},
    {"WaterTemple", "WaterTemple_Room00_WaterLevel", true, {-35.0f, 170.0f, -7.0f}, "Obj_WaterTemple_WaterPlane_A_01"},
    {"WaterTemple", "WaterTemple_Room00_WaterLevel", true, {-12.0f, 170.0f, 46.9f}, "Obj_WaterTemple_WaterPlane_A_01"},
    {"WaterTemple", "WaterTemple_Room00_WaterLevel", true, {25.7f, 170.0f, 18.8f}, "Obj_WaterTemple_WaterPlane_A_01"},
    {"WaterTemple", "WaterTemple_Room00_WaterLevel", true, {55.5f, 170.0f, 22.1f}, "Obj_WaterTemple_WaterPlane_A_01"},
    {"WaterTemple", "WaterTemple_Room03_00", false, {-39.2f, 114.6f, 8.6f}, "EventRequestActor+Obj_HookShotTarget_A_01+Obj_WaterTemple_HookShotDecoration_A_04+Obj_WaterTemple_HookShotTargetStand_A_03+Obj_WaterTemple_HookShotTarget_A_03"},
    {"WaterTemple", "WaterTemple_Room06_00", false, {-80.0f, 119.5f, 7.5f}, "Obj_HookShotTarget_A_13+Obj_WaterTemple_HookShotDecoration_A_04+Obj_WaterTemple_HookShotTarget_B_01"},
    {"WaterTemple", "WaterTemple_Room06_00", false, {-50.5f, 119.5f, 7.5f}, "Obj_WaterTemple_WaterPlane_A_01"},
    {"WaterTemple", "WaterTemple_Room14_00", false, {-13.2f, 111.3f, -114.6f}, "FldObj_AirWall_DedicatedEnemy"},
    {"WaterTemple", "WaterTemple_Room14_00", false, {-4.0f, 111.3f, -114.6f}, "FldObj_AirWall_DedicatedEnemy+Obj_WaterTemple_WaterPlane_A_01"},
    {"ZoraRiver", "MiniGame_FrogTenors_StormSong", false, {12.7f, 109.6f, -44.1f}, "Obj_WaterPlaneDynamic_A_01"},
};

// Shared switch plugs (the third table), each with the room objects that read it. A line with a fifth field
// (g3-objects) is a plug that also raises an event asker (any asker in the romfs wiring, not only the live-place ones
// coop_eye_switch.cpp lists; review-a38-3): held only once those flags are all on in our game, so our asker can no
// longer fire and the hold runs the room objects only (the result; owner 07:40 UTC 2026-11-17: no replay of a gimmick
// event). A guard is shared when the asker has a shared one; else the asker's own flag (Dodongo 17's NaviHelp_17_0x,
// on after that player's own first camera focus). "load:<flag>": the asker reads it only at the room's load
// (UpdateOnce 1), so it counts only if it was on when our room loaded (coop_room_objects.h).
constexpr SwitchPlug kSwitchPlugs[] = {
    {"DodongoCavern", "Obj_EyeSwitch_A_05", {65.7f, 121.8f, -23.0f}, "OnState", "DodongoCavern_NaviHelp_17_00"},  // Obj_DodongoCave_FireWall_A_01; asks FocusOnGimmick_Basic
    {"DodongoCavern", "Obj_EyeSwitch_A_05", {75.4f, 118.3f, -15.0f}, "OnState", "DodongoCavern_NaviHelp_17_01"},  // Obj_DodongoCave_FireWall_A_01; asks FocusOnGimmick_Basic
    {"DodongoCavern", "Obj_StepSwitch_A_01", {-49.0f, 114.2f, -8.0f}, "OnState", ""},  // Obj_DodongoCave_SwitchShutter_A_01+Obj_SwitchShutter_A_06
    {"DodongoCavern", "Obj_StepSwitch_A_01", {17.2f, 112.3f, -23.0f}, "OnState", ""},  // GroundControlActor_SimpleMoveTo+Obj_DodongoCave_MoveFloor_A_01
    {"ForestTemple", "Obj_StepSwitch_A_01", {-74.0f, 125.5f, -1.0f}, "OnState", "ForestTemple_SageOfForest_ElevatorSwitch_01"},  // LocationalLight_Amplifier_ParamChangeActor+Obj_ForestTemple_SwitchLamp_A_01+PushAirWall_Cone; asks ForestTemple_Room11_ElevatorStart
    {"ForestTemple", "Obj_StepSwitch_A_01", {56.5f, 124.6f, 2.1f}, "OnState", ""},  // Obj_SwitchCandleStick_A_01
    {"GerudoTrainingGround", "Obj_EyeSwitch_A_03", {46.9f, 101.9f, -92.4f}, "OnState", ""},  // Obj_Gerudo_SwitchShutter_A_04+Obj_SwitchShutter_A_02
    {"GerudoTrainingGround", "Obj_StepSwitch_A_04", {52.2f, 97.5f, -86.0f}, "OnState", "GerudoTrainingGround_Dungeon_Room05_StepSwitch"},  // Obj_TBox_A_01; asks GerudoTrainingGround_05_DisappearFire
    {"InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_StepSwitch_A_03", {8.5f, 102.5f, -51.4f}, "OnState", "InsideJabuJabuBelly_Gimmick_Onswitch_first"},  // Obj_InsideJabu_Jabu_SwitchShutter_A_01+Obj_InsideJabu_Jabu_SwitchShutter_A_02; asks InsideJabuJabuBellyRoom01StepSwitch
    {"InsideTheDekuTree", "Obj_StepSwitch_A_02", {-17.5f, 119.6f, -4.9f}, "OnState", "load:InsideTheDekuTree_RoomD_StepSwitch_On_01"},  // Obj_InsideTheDekuTree_ElevatorLift_A_01+Obj_InsideTheDekuTree_ElevatorLift_A_02+Obj_InsideTheDekuTree_ElevatorLift_A_03; asks InsideTheDekuTree_Gimmick_RoomD_PlatformUp
    {"SpiritTemple", "Obj_SolarSwitch_A_02", {45.8f, 121.5f, -47.5f}, "isLightMax", "SpiritTemple_Room19_LightSwitch_On_01"},  // Obj_SpiritTemple_CapstanMirror_A_01; asks SpiritTemple_Level_Room_19_SwitchOn_01
    {"WaterTemple", "Obj_EyeSwitch_A_05", {-33.2f, 115.8f, -40.2f}, "OnState", ""},  // Obj_WaterTemple_SwitchShutter_A_02
    {"WaterTemple", "Obj_EyeSwitch_A_05", {-4.5f, 116.6f, -26.3f}, "OnState", ""},  // Obj_WaterTemple_SwitchShutter_A_02
    {"WaterTemple", "Obj_SwitchHit_A_02", {-38.6f, 94.0f, -15.8f}, "OnState", ""},  // Obj_WaterTemple_SwitchShutter_A_07
    {"WaterTemple", "Obj_SwitchHit_A_06", {-28.5f, 111.5f, 3.5f}, "OnState", ""},  // Obj_WaterTemple_Fountain_A_01
    {"WaterTemple", "Obj_SwitchHit_A_06", {52.1f, 110.0f, -23.2f}, "OffState", ""},  // Obj_WaterTemple_Fountain_A_01
};
// g3-objects (room_objects.py --eye-switches coop_eye_switch.cpp --dungeon coop_dungeon.cpp): read by room objects,
// but the plug also raises an event asker that no flag stops (no flag deletes it or gates its trigger), so a
// hold could start our own copy of the event: never held. Each says how the result still reaches a follower: the
// switch's own ON input reads a shared flag the event sets (its own AI turns it on: the native path).
// read by room objects, but the plug also raises an event asker that no flag stops: not held
//   SpiritTemple Obj_HammerSwitch_A_01 {18.0, 129.1, -68.5} OnState: Obj_SpiritTemple_SwitchShutter_A_01+Obj_SwitchShutter_A_06; asks SpritTemple_Room08_OpenShutter; BootUp <- SpiritTemple_Room08_Unlocked_01, ForceControl <- SpiritTemple_Room08_Unlocked_01
//   SpiritTemple Obj_SolarSwitch_A_02 {28.0, 121.5, -40.1} isLightMax: Obj_SpiritTemple_CapstanMirror_A_01; asks SpiritTemple_Level_Room_19_SwitchOn_01; isCleared <- SpiritTemple_Room19_LightSwitch_On_02
//   SpiritTemple Obj_SolarSwitch_A_02 {35.4, 121.5, -37.2} isLightMax: Obj_SpiritTemple_CapstanMirror_A_01; asks SpiritTemple_Room19_SunSwitchShutterOpen; isCleared <- SpiritTemple_Room19_LightSwitch_On_03
//   SpiritTemple Obj_SwitchHit_A_02 {33.6, 111.8, -30.7} OnState: Obj_SpiritTemple_SwitchShutter_A_01+Obj_SwitchShutter_A_06; asks SpritTemple_Room14_OpenShutter; BootUp <- SpiritTemple_Room14_Unlocked_01, ForceControl <- SpiritTemple_Room14_Unlocked_01
//   SpiritTemple Obj_SwitchHit_A_05 {0.0, 153.1, -46.0} OnState: Obj_SpiritTemple_SwitchShutter_A_01+Obj_SwitchShutter_A_06; asks SpritTemple_Room31_OpenShutter; BootUp <- SpiritTemple_Room31_Unlocked, ForceControl <- SpiritTemple_Room31_Unlocked
// clang-format on
}  // namespace

std::span<const SwitchPlug> SwitchPlugs() { return kSwitchPlugs; }

std::vector<GuardFlag> GuardFlags(const SwitchPlug& line) {
    std::vector<GuardFlag> out;
    std::string_view rest = line.guard;
    while (!rest.empty()) {
        const std::size_t plus = rest.find('+');
        std::string_view one = rest.substr(0, plus);
        const bool at_load = one.starts_with(kLoadGuard);
        if (at_load) one.remove_prefix(kLoadGuard.size());
        out.push_back(GuardFlag{one, at_load});
        rest = plus == std::string_view::npos ? std::string_view() : rest.substr(plus + 1);
    }
    return out;
}

std::span<const GroupFlag> GroupFlags() { return kGroupFlags; }

std::vector<const SwitchPlug*> SwitchPlugsOf(std::string_view scene, std::string_view name,
                                             const std::array<float, 3>& at) {
    std::vector<const SwitchPlug*> out;
    for (const SwitchPlug& s : kSwitchPlugs) {
        if (s.scene != scene || s.name != name) continue;
        float d2 = 0.0f;
        for (std::size_t i = 0; i < 3; ++i) d2 += (s.place[i] - at[i]) * (s.place[i] - at[i]);
        if (d2 <= kSwitchMatchM * kSwitchMatchM) out.push_back(&s);
    }
    return out;
}

bool SharedSwitchName(std::string_view name) {
    for (const SwitchPlug& s : kSwitchPlugs) {
        if (s.name == name) return true;
    }
    return false;
}

std::span<const Placement> Placements() { return kPlacements; }

const Placement* Nearest(std::string_view scene, std::string_view name, const std::array<float, 3>& at) {
    const Placement* best = nullptr;
    float best_d2 = 0.0f;
    for (const Placement& p : kPlacements) {
        if (p.scene != scene || p.name != name) continue;
        float d2 = 0.0f;
        for (std::size_t i = 0; i < 3; ++i) d2 += (p.place[i] - at[i]) * (p.place[i] - at[i]);
        if (best == nullptr || d2 < best_d2) {
            best = &p;
            best_d2 = d2;
        }
    }
    return best;
}

bool Driven(std::string_view scene, std::string_view name, const std::array<float, 3>& at) {
    const Placement* p = Nearest(scene, name, at);
    return p != nullptr && !p->why.empty();
}

bool PlayerDriven(std::string_view why) {
    for (const std::string_view input : {std::string_view("RideOnPlayer"), std::string_view("RideOnCharacter"),
                                         std::string_view("IsInPlayer"), std::string_view("IsTargetFloor")}) {
        if (why.find(input) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool RiderName(std::string_view name) {
    for (const std::string_view n : kRiderNames) {
        if (n == name) return true;
    }
    return false;
}

}  // namespace CoopRoomObjects
