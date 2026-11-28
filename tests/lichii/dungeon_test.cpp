// Tests for the shared dungeon flags (lichii/hook/coop_dungeon.cpp).
#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>

#include "coop_dungeon.h"
#include "coop_event_result.h"
#include "coop/host/flag_watcher.hpp"
#include "coop/protocol/codec.hpp"

namespace host = coop::host;

TEST_CASE("dungeon: every flag's hash is the hash of its name, once") {
    std::set<std::uint32_t> seen;
    std::size_t ints = 0;
    for (const auto& f : CoopDungeon::Flags()) {
        CHECK_MESSAGE(host::flag_hash(f.name) == f.hash, f.name);
        CHECK(seen.insert(f.hash).second);
        ints += f.is_int ? 1 : 0;
    }
    CHECK(CoopDungeon::Flags().size() > 700);
    CHECK(ints > 10);
}

TEST_CASE("dungeon: a mid-boss beaten is shared, its fight start is not (mid-boss end state)") {
    const auto has = [](std::string_view name) {
        const auto flags = CoopDungeon::Flags();
        return std::any_of(flags.begin(), flags.end(), [&](const auto& f) { return f.name == name; });
    };
    CHECK(has("Get_StoneOfWater_BigOctoDefeat"));
    CHECK(has("Enemy_MiddleBoss_IronKnuckle_Nabooru_BossDefeat"));
    CHECK(has("BottomOfTheWell_Level_Room_06_Boss_Defeat"));  // Dead Hand, the rule they follow
    CHECK(has("WaterTemple_DarkLink_Death"));
    CHECK(has("ForestTemple_DefeatMeg"));
    CHECK(has("FireTemple_Room24_FlareDancerisDead"));
    CHECK(has("Gimmick_DefeatEnemy_Open_DodongoCave_02"));  // the second Lizalfos room
    CHECK_FALSE(has("Get_StoneOfWater_BigOctoAppear"));
    CHECK_FALSE(has("Enemy_MiddleBoss_IronKnuckle_Nabooru_BossAppear"));
}

TEST_CASE("dungeon: an enemy room's clear sets our asker's result, then its flags land (rows 160/161, 336)") {
    // Dodongo's Cavern's Lizalfos rooms: the shutters are NOT(clear flag OR asker EventBool) and the room deletes its
    // Lizalfos on the clear flag (romfs Gimmick_DodongoCavern_MiddleBoss01/02). Row 336 (aetherrose 0.12.0 host log
    // 20261128-081808): the clear flag landed in the game that did not play the event (its Lizalfos were deleted and
    // its chest appeared) and the shutters stayed shut until a reload, as in rows 62 and 72. The game that played it
    // also has the asker's EventBool on that OR. So the event is class asker: our asker gets EventBool (+ EX1 and
    // EventFinish, as the event's flow sets them), and the clear flag (it deletes our asker) waits as a result guard
    // until that is set, the same order as in the game that played it. The flag itself is never in the dungeon hold
    // (row 160: a game away from the room gets it when it leaves the dungeon or loads, or once our result is set).
    for (const std::string_view room : {"01", "02"}) {
        const std::string flag = std::string("Gimmick_DefeatEnemy_Open_DodongoCave_") + std::string(room);
        const std::string event = std::string("DodongoCavern_Gimmick_DefeatLizalfos") + std::string(room);
        const std::string played = "HavePlayedEvent." + event;
        CAPTURE(flag);
        const auto flags = CoopDungeon::Flags();
        const auto find = [&](const std::string& name) {
            return std::find_if(flags.begin(), flags.end(), [&](const auto& f) { return f.name == name; });
        };
        REQUIRE(find(flag) != flags.end());
        REQUIRE(find(played) != flags.end());
        CHECK_FALSE(CoopDungeon::HeldIn(find(flag)->hash).has_value());
        CHECK_FALSE(CoopEventResult::IsLive(event));
        const auto lines = CoopEventResult::AskerLines(event, "DodongoCavern");
        REQUIRE(lines.size() == 1);
        const auto outs = CoopEventResult::SplitOutputs(lines[0]->outputs);
        CHECK(std::find(outs.begin(), outs.end(), "EventBool") != outs.end());
        CHECK(std::find(outs.begin(), outs.end(), "EventFinish") != outs.end());
        bool guarded = false;
        for (const auto& g : CoopEventResult::Guards()) {
            guarded |= g.flag == flag && g.event == event;
            CHECK(g.flag != played);
        }
        CHECK(guarded);
    }
}

TEST_CASE("dungeon: every room event whose asker drives a shutter's lock is class asker (row 336)") {
    // tools/puppet_pack/eye_switches.py LOCK_INPUTS: the enemy rooms of Fire, Forest, Ganon, Gerudo and Shadow too.
    for (const std::string_view event :
         {"DodongoCavern_Gimmick_DefeatLizalfos01", "DodongoCavern_Gimmick_DefeatLizalfos02",
          "FireTemple_Room24_FocusOnGimmick", "FocusOnGimmick_FireTemple_FlareDancer02",
          "ForestTemple_Room04_OpenDoor_AppearTBox", "ForestTemple_Room22_ShutterOpen_TBoxAppear",
          "GerudoTrainingGround_Room07_TreasureAppear", "InsideGanonCastle_Dungeon_AppaerTreasureBox_12",
          "ShadowTemple_04_EnemyAnnihilation_Clear"}) {
        CAPTURE(event);
        CHECK_FALSE(CoopEventResult::IsLive(event));
    }
}

TEST_CASE("dungeon: the Water Temple's water level and switches are in the list") {
    bool level = false;
    bool shutter = false;
    for (const auto& f : CoopDungeon::Flags()) {
        level |= std::string(f.name) == "WaterTemple_Room00_WaterLevel" && f.is_int;
        // (Room00_Switch_NearTheShutter is where our Link stands, set both ways by an area: not shared.)
        shutter |= std::string(f.name) == "WaterTemple_Room00_OpenDoor1F";
        // One player's own state is not shared.
        CHECK(std::string(f.name).find("NaviHelp") == std::string::npos);
        CHECK(std::string(f.name).find("IsPlayerIn") == std::string::npos);
    }
    CHECK(level);
    CHECK(shutter);
}

TEST_CASE("dungeon: room state under another spelling or after Gimmick_ is shared (softlock audit)") {
    std::set<std::string> names;
    for (const auto& f : CoopDungeon::Flags()) names.insert(f.name);
    for (const char* n : {"Insidejabujabu_Room7_Cleared", "Insidejabujabu_Room12_Cleared",
                          "Firetemple_SongOfTimeBlock_A_01_Disappearance",
                          "Foresttemple_SongOfTimeBlock_A_01_Disappearance", "Gimmick_DefeatEnemy_Open_DodongoCave_01",
                          "Gimmick_DefeatEnemy_Open_ForestTemple_05", "Gimmick_DeleteFire_FireTemple"}) {
        CHECK_MESSAGE(names.contains(n), n);
    }
    // Jabu-Jabu's Room 14 water (issue 249): the two flags are the water's only input (its placement
    // groups' saved state), set once per press and never written back by a game whose switch is free.
    for (const char* n : {"Gimmick_Insidejabujabu_Updown_MoveWater", "Gimmick_Insidejabujabu_down_MoveWater"}) {
        CHECK_MESSAGE(names.contains(n), n);
        CHECK_FALSE_MESSAGE(CoopDungeon::HeldIn(host::flag_hash(n)).has_value(), n);  // live, not held
    }
    // Where one player (or Ruto) stands, state a switch or torch holds both ways, each game's own switch
    // arming, and what Link reads stay each player's own (review of da6a300).
    for (const char* n : {"Insidejabujabu_Room7_Switch1_InsideArea", "Insidejabujabu_Room7_Switch2_OnlyLinkOrLuto",
                          "Insidejabujabu_Room7_NearTheSwitch", "Insidejabujabu_Room7_NearTheSwitch_Tentacle_Gate",
                          "Gimmick_Insidejabujabu_Updown_MoveWaterSwitch", "InsidejaubjabuBelly_gimic_switchOn_Stone",
                          "Gimmick_Ignition_Open_WaterTemple_01", "Gimmick_Ignition_Open_WaterTemple_02",
                          "HavePlayedEvent.Obj_SpiritTemple_StoneTablet_A_01"}) {
        CHECK_FALSE_MESSAGE(names.contains(n), n);
    }
    // Story scenes outside the rooms stay out.
    CHECK_FALSE(names.contains("EscapeFromCrumblingGanonCastle_OpenLastGate"));
    // Second pass: the Goron cells, the room gimmick scenes;
    // not the Forest Temple corridor (one-way), a boss intro or a boss alone.
    CHECK(names.contains("Npc_Goron_001_ShutterisOpen"));
    // Jabu's Room 17 stone switch: its step-switch graph sets it both ways (review r2).
    CHECK_FALSE(names.contains("InsidejaubjabuBelly_gimic_switchOn_Stone"));
    CHECK(names.contains("HavePlayedEvent.Dm_DodongoCave_0010"));
    CHECK(names.contains("HavePlayedEvent.Dm_InsideGanonCastle_0090"));
    CHECK_FALSE(names.contains("HavePlayedEvent.Dm_ForestTemple_0007"));
    CHECK_FALSE(names.contains("HavePlayedEvent.Dm_Deku_BossIn"));
    CHECK_FALSE(names.contains("Gohma_DefeatBoss"));
    CHECK_FALSE(names.contains("InsidejaubjabuBelly_gimic_Liftposition_On"));
}

TEST_CASE("dungeon: a room gimmick's played event is shared, story scenes and bosses are not") {
    std::set<std::string> names;
    std::size_t played = 0;
    for (const auto& f : CoopDungeon::Flags()) {
        names.insert(f.name);
        const std::string name = f.name;
        // The dungeon gimmick scenes a room's own logic reads for its end state (softlock
        // audit): Dm_ by name, but no story.
        static const std::set<std::string> kRoomScenes = {
            "HavePlayedEvent.Dm_DodongoCave_0010", "HavePlayedEvent.Dm_FireTemple_0029",
            "HavePlayedEvent.Dm_ForestTemple_0010", "HavePlayedEvent.Dm_ForestTemple_0014",
            "HavePlayedEvent.Dm_InsideGanonCastle_0035", "HavePlayedEvent.Dm_InsideGanonCastle_0090",
            "HavePlayedEvent.Dm_WaterTemple_0010", "HavePlayedEvent.Dm_WaterTemple_0015"};
        if (name.rfind("HavePlayedEvent.", 0) == 0 && !kRoomScenes.contains(name)) {
            ++played;
            CHECK_FALSE(f.is_int);
            for (const char* left_out : {"Boss", "Navi", "Talk", "Meet", "Ruto", "SageOf",
                                         "HavePlayedEvent.Common_", "HavePlayedEvent.Dm_",
                                         "HavePlayedEvent.Demo_"}) {
                CHECK_MESSAGE(name.find(left_out) == std::string::npos, name);
            }
        }
    }
    // Issue 129: the Deku Tree slingshot room's ladder, and its room cleared.
    CHECK(names.count("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder") == 1);
    CHECK(names.count("InsideTheDekuTree_RoomC_Cleared") == 1);
    // The other Deku Tree rooms' gimmicks, and some of every kind elsewhere.
    CHECK(names.count("HavePlayedEvent.InsideTheDekuTree_Gimmick_RoomD_PlatformUp") == 1);
    CHECK(names.count("HavePlayedEvent.InsideTheDekuTree_Gimmick_RoomG_WaterLevelDrop") == 1);
    CHECK(names.count("HavePlayedEvent.DodongoCavern_Gimmick_EyeSwitch") == 1);
    CHECK(names.count("HavePlayedEvent.ForestTemple_Room22_ShutterOpen") == 1);
    CHECK(names.count("HavePlayedEvent.ShadowTemple_Room06_SilverToken_Unlock") == 1);
    // One-way rides and camera shots that are no room's state stay out.
    for (const char* out :
         {"HavePlayedEvent.ShadowTemple_Gimmick_Departure", "HavePlayedEvent.ForestTemple_Room11_ElevatorStart",
          "HavePlayedEvent.ForestTemple_Room11_RoomSpin", "HavePlayedEvent.ForestTemple_Room22_RoomSpin",
          "HavePlayedEvent.FocusOnGimmick_Basic", "HavePlayedEvent.FocusOnGimmick_NoSE",
          "HavePlayedEvent.TrapTreasureBoxOpen", "HavePlayedEvent.TreasureBoxAppear_Room04",
          "HavePlayedEvent.InsideGanonCastle_LookBarrierFromDoor_Fire",
          "HavePlayedEvent.GanonCastle_Dungeon_SeeTowerBarrier",
          "HavePlayedEvent.GanonCastle_Dungeon_SeeTowerBarrier_FromDoor"}) {
        CHECK_MESSAGE(names.count(out) == 0, out);
    }
    CHECK(played > 180);
}

TEST_CASE("dungeon: one player's failed or reset attempt is not shared (issue 149)") {
    // Set by a room graph when one player's try fails or a timed switch runs out;
    // shared, it would reach another player in the middle of their own try.
    std::set<std::string> names;
    for (const auto& f : CoopDungeon::Flags()) names.insert(f.name);
    for (const char* out : {"InsideTheDekuTree_RoomK_Failure", "FireTemple_Room08_SwitchWasReset",
                            "FireTemple_Room10_SwitchWasReset", "FireTemple_Room13_SwitchWasReset",
                            "InsideGanonCastle_Dungeon_Room08_CandleWasReset"}) {
        CHECK_MESSAGE(names.count(out) == 0, out);
    }
    // The puzzle solved is shared: the Scrub Brothers' room opens for everyone.
    CHECK(names.count("InsideTheDekuTree_RoomK_Cleared") == 1);
    CHECK(names.count("InsideTheDekuTree_Dungeon_EnemyScrubDekuDestroy_003") == 1);
}

TEST_CASE("dungeon: where one player's Link stands in the Forest Temple is not shared (player report 108)") {
    // Set true and false by a room graph's Player-layer areas, read only by
    // that player's own event (the Song of Time block's, the fall-down one).
    std::set<std::string> names;
    for (const auto& f : CoopDungeon::Flags()) names.insert(f.name);
    for (const char* out : {"ForestTemple_IsLeftOfTimeBLock", "ForestTemple_IsInFallingCeilingRoom"}) {
        CHECK_MESSAGE(names.count(out) == 0, out);
    }
    // The block gone stays the group's: every game's room graph reads it live.
    CHECK(names.count("Foresttemple_SongOfTimeBlock_A_01_Disappearance") == 1);
    CHECK(names.count("ForestTemple_Room02_TimeBlock") == 1);
}

TEST_CASE("dungeon: where one player's Link stands is not shared in any dungeon (forest-temple-a sweep)") {
  std::set<std::string> names;
  for (const auto& f : CoopDungeon::Flags()) names.insert(f.name);
  for (const char* out : {"BottomOfTheWell_Level_Room_01_InTheCenterArea",
                          "BottomOfTheWell_Level_Room_01_InTheDivingArea",
                          "BottomOfTheWell_Level_Room_01_InTheReturnArea",
                          "BottomOfTheWell_Level_Room_09_NearRock",
                          "DodongoCavern_Room05_NearTreasureBox",
                          "DodongoCavern_Room10_PlayerOutOfRoom",
                          "FireTemple_Room04_IsAtCorrectArea",
                          "FireTemple_Room12_IsAtCorrectArea",
                          "ForestTemple_Room08_ClimbIvy_Near",
                          "GerudoTrainingGround_Dungeon_Room03_InSecretRoom",
                          "GerudoTrainingGround_Dungeon_Room08_InSecretRoom",
                          "InsideJabuJabuBelly_Room09_TBoxNearRuto",
                          "InsideJabuJabuBelly_Room12_TBoxNearRuto",
                          "InsideJabuJabuBelly_Room14_IsNearGimmick",
                          "InsideJabuJabuBelly_Room14_PlayerIsUnder",
                          "InsideJabuJabuBelly_Room18_IsInHiddenArea",
                          "InsideTheDekuTree_RoomA_NearScaffold",
                          "ShadowTemple_Level_Room_09_Away",
                          "ShadowTemple_Level_Room_13_PlayerPosCheck",
                          "SpiritTemple_LevelRoom_02_PlayerPosChange1_2",
                          "SpiritTemple_LevelRoom_02_PlayerPosChange2",
                          "SpiritTemple_LevelRoom_02_PlayerPosChange2_2",
                          "SpiritTemple_Level_Room_02_PlayerPosChange1",
                          "SpiritTemple_Room16_PlayerPosCheck",
                          "WaterTemple_Room00_Switch_NearThePlatform",
                          "WaterTemple_Room00_Switch_NearTheShutter",
                          "IceCavern_Room01Demo_Far"}) {
    CHECK_MESSAGE(names.count(out) == 0, out);
  }
  // Room state set both ways by a graph stays shared (a switch, a fire, an air wall).
  CHECK(names.count("FireTemple_Room04_SwitchHit") == 1);
  CHECK(names.count("FireTemple_Room14_AirWallEnemyMove00") == 1);
  CHECK(names.count("ForestTemple_TwistCorridor01_IsConstraint") == 1);
}

TEST_CASE("dungeon: Merge adds to the list, makes one when there is none, or does nothing when off") {
    std::optional<host::SharedFlags> none;
    CHECK(CoopDungeon::Merge(none, false) == 0);
    CHECK_FALSE(none.has_value());

    const std::size_t all =
        CoopDungeon::Flags().size() + CoopDungeon::LiveFlags().size() + CoopDungeon::AutogenFlags().size() +
        CoopDungeon::Keys().size() + CoopDungeon::WorldKeys().size();
    const std::size_t n = CoopDungeon::Merge(none, true);
    REQUIRE(none.has_value());
    CHECK(n == all);
    CHECK(none->contains(host::FlagKey::key64_flag(CoopDungeon::Keys()[0].key)));
    CHECK(none->contains(host::FlagKey(host::flag_hash("WaterTemple_Room00_WaterLevel"), 0)));

    // An existing list keeps its own flags; a flag already there counts once.
    const auto mine = host::FlagKey(0x12345678, 0);
    const auto dup = host::FlagKey(CoopDungeon::Flags()[0].hash, 0);
    std::optional<host::SharedFlags> some = host::SharedFlags({mine, dup});
    CHECK(CoopDungeon::Merge(some, true) == all - 1);
    CHECK(some->contains(mine));
    CHECK(some->size() == all + 1);
}

namespace {

bool HasKey(std::uint64_t key, const char* dungeon, const char* actor) {
    for (const auto& k : CoopDungeon::Keys()) {
        if (k.key == key) {
            return std::string(k.dungeon) == dungeon && std::string(k.actor) == actor;
        }
    }
    return false;
}

} // namespace

TEST_CASE("dungeon: the placement keys are one each, and only things that stay done") {
    std::set<std::uint64_t> seen;
    std::map<std::string, std::size_t> per_dungeon;
    std::size_t chests = 0;
    for (const auto& k : CoopDungeon::Keys()) {
        CHECK(k.key != 0);
        CHECK(k.key != ~std::uint64_t{0});
        CHECK_MESSAGE(seen.insert(k.key).second, k.actor);
        const std::string actor = k.actor;
        // Pots, enemies and grass come back on a scene load: not in the list.
        CHECK_MESSAGE(!actor.starts_with("Obj_Pot_"), actor);
        CHECK_MESSAGE(actor.find("Grass") == std::string::npos, actor);
        CHECK_MESSAGE((!actor.starts_with("Enemy_") || actor == "Enemy_Skulltula_Gold" ||
                       actor == "Enemy_Anubis" || actor.starts_with("Enemy_MiddleBoss")),
                      actor);
        chests += actor.starts_with("Obj_TBox") ? 1 : 0;
        ++per_dungeon[k.dungeon];
    }
    CHECK(CoopDungeon::Keys().size() == 500);
    CHECK(chests == 140);
    CHECK(per_dungeon.size() == 13);
    CHECK(per_dungeon["InsideTheDekuTree"] == 37);
    CHECK(per_dungeon["DodongoCavern"] == 36);
}

TEST_CASE("dungeon: row 220: the overworld's obstacles that stay done are shared, each player's own are not") {
    std::set<std::uint64_t> dungeon;
    for (const auto& k : CoopDungeon::Keys()) dungeon.insert(k.key);
    std::set<std::uint64_t> seen;
    std::map<std::string, std::size_t> kinds;
    for (const auto& k : CoopDungeon::WorldKeys()) {
        CHECK(k.key != 0);
        CHECK_MESSAGE(seen.insert(k.key).second, k.actor);
        CHECK_MESSAGE(!dungeon.contains(k.key), k.actor);
        const std::string scene = k.dungeon;
        const std::string actor = k.actor;
        CHECK_MESSAGE(!CoopDungeon::IsDungeonScene(scene), scene);
        CHECK_MESSAGE(scene != "DreamWorld", actor);  // Boss Rush and the battle arenas: each player's own
        CHECK_MESSAGE(actor.find("HitTarget") == std::string::npos, actor);  // the Market's shooting gallery
        CHECK_MESSAGE(actor.find("ArcheryTarget") == std::string::npos, actor);  // horseback archery
        CHECK_MESSAGE(actor.find("TBox") == std::string::npos, actor);  // chests: CoopChests
        CHECK_MESSAGE(!actor.starts_with("Obj_Rupee"), actor);  // pickups: CoopPickups
        CHECK_MESSAGE(actor.find("Heart") == std::string::npos, actor);
        CHECK_MESSAGE(actor != "Enemy_Skulltula_Gold", actor);
        ++kinds[actor.substr(0, actor.rfind('_'))];
    }
    CHECK(CoopDungeon::WorldKeys().size() == 158);
    CHECK(kinds["Obj_BrokenRock_A"] == 57);
    CHECK(kinds["Obj_LiftRock_B"] == 46);
    CHECK(kinds["WallCrack"] == 15);
    CHECK(kinds["Obj_EnemyCounter_A"] == 5);
    bool cover = false, rock = false, counter = false;
    for (const auto& k : CoopDungeon::WorldKeys()) {
        cover |= k.key == 0x2031e5ac12ff9ab4ull && std::string(k.actor) == "Land_HrlField_HidingHoleCover_A_01";
        rock |= k.key == 0x8a4e791b05223269ull && std::string(k.actor) == "Obj_DeathMtTrail_BrokenRock_A_01";
        counter |= k.key == 0x0d23bcff35eca9eaull && std::string(k.dungeon) == "Graveyard";
    }
    CHECK(cover);
    CHECK(rock);
    CHECK(counter);
}

TEST_CASE("dungeon: the Deku Tree's and Dodongo's Cavern's chests are in the list") {
    CHECK(HasKey(0x9dc359c40cd5dfe7ull, "InsideTheDekuTree", "Obj_TBox_A_01"));
    CHECK(HasKey(0x0b66198d17d6584eull, "InsideTheDekuTree", "Obj_TBox_A_02"));
    CHECK(HasKey(0x77750b6072f44277ull, "DodongoCavern", "Obj_TBox_A_01"));
    CHECK(HasKey(0x686c7d83d07add16ull, "DodongoCavern", "Obj_TBox_A_06"));
}

namespace {

class Game : public host::FlagStore {
public:
    std::map<host::FlagKey, coop::protocol::FlagEntry> values;
    void set(host::FlagKey key, std::uint32_t v) {
        values[key] = {key, coop::protocol::FlagKind::boolean, v};
    }
    std::optional<coop::protocol::FlagEntry> read(host::FlagKey key) override {
        const auto it = values.find(key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    }
    bool write(const coop::protocol::FlagEntry& e) override {
        if (!values.contains(e.key)) return false;
        values[e.key] = e;
        return true;
    }
};

} // namespace

TEST_CASE("dungeon: a chest opened in one game opens in the other and never closes again") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    REQUIRE(list.has_value());
    const auto chest = host::FlagKey::key64_flag(0x9dc359c40cd5dfe7ull);  // Deku Tree
    const auto level = host::FlagKey(host::flag_hash("WaterTemple_Room00_WaterLevel"), 0);

    Game a;
    Game b;
    for (Game* g : {&a, &b}) {
        g->set(chest, 0);
        g->set(level, 2);
    }
    host::FlagWatcher wa(*list, host::FlagRole::player);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    for (host::FlagWatcher* w : {&wa, &wb}) {  // as the hook does with pickups on
        for (const auto& k : CoopDungeon::Keys()) w->share_pickup(host::FlagKey::key64_flag(k.key));
    }
    wa.frame(a);
    wb.frame(b);

    a.set(chest, 1);
    const auto out = wa.frame(a);
    REQUIRE(out.size() == 1);
    proto::Message m;
    REQUIRE(proto::decode(out[0].bytes, m) == proto::Error::none);
    const auto* set = std::get_if<proto::FlagSet>(&m);
    REQUIRE(set);
    REQUIRE(set->flags.size() == 1);
    CHECK(set->flags[0].key == chest);
    CHECK(set->flags[0].value == 1);

    // The host's answer reaches B: B's chest is open.
    wb.receive(proto::Message{proto::FlagState{1, set->flags}});
    wb.frame(b);
    CHECK(b.values[chest].value == 1);

    // B leaves the dungeon and the game reads 0 for a moment: not sent, and
    // a 0 coming in is not written over it.
    b.set(chest, 0);
    CHECK(wb.frame(b).empty());
    b.set(chest, 1);
    wb.frame(b);
    wb.receive(proto::Message{proto::FlagState{2, {{chest, proto::FlagKind::boolean, 0}}}});
    wb.frame(b);
    CHECK(b.values[chest].value == 1);
}

TEST_CASE("dungeon: row 220: a boulder blown up in one game is gone in the other and stays gone") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    REQUIRE(list.has_value());
    const auto rock = host::FlagKey::key64_flag(0x8a4e791b05223269ull);  // Death Mountain Trail's big boulder
    REQUIRE(list->contains(rock));

    Game a;
    Game b;
    a.set(rock, 0);
    b.set(rock, 0);
    host::FlagWatcher wa(*list, host::FlagRole::player);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    for (host::FlagWatcher* w : {&wa, &wb}) {  // as the hook does with pickups on
        for (const auto& k : CoopDungeon::WorldKeys()) w->share_pickup(host::FlagKey::key64_flag(k.key));
    }
    wa.frame(a);
    wb.frame(b);

    a.set(rock, 1);
    const auto out = wa.frame(a);
    REQUIRE(out.size() == 1);
    proto::Message m;
    REQUIRE(proto::decode(out[0].bytes, m) == proto::Error::none);
    const auto* set = std::get_if<proto::FlagSet>(&m);
    REQUIRE(set);
    REQUIRE(set->flags.size() == 1);
    CHECK(set->flags[0].key == rock);
    wb.receive(proto::Message{proto::FlagState{1, set->flags}});
    wb.frame(b);
    CHECK(b.values[rock].value == 1);

    // B's game reads 0 for a moment (a scene load): not sent; and a 0 coming in is never written over its 1.
    b.set(rock, 0);
    CHECK(wb.frame(b).empty());
    b.set(rock, 1);
    wb.frame(b);
    wb.receive(proto::Message{proto::FlagState{2, {{rock, proto::FlagKind::boolean, 0}}}});
    wb.frame(b);
    CHECK(b.values[rock].value == 1);
}

TEST_CASE("dungeon: the Deku torch plate's unnamed autogen bool is shared (deku-test, torch flame)") {
    // B stepped on room E's plate and set 0x0f8f3fa3; A got the web (RoomE_KillSpiderWave)
    // but not the torch, because the bool has no name and Flags() lists by name.
    std::set<std::uint32_t> named;
    for (const auto& f : CoopDungeon::Flags()) named.insert(f.hash);
    for (const auto& f : CoopDungeon::LiveFlags()) named.insert(f.hash);
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    REQUIRE(list);
    bool plate = false;
    std::set<std::uint32_t> seen;
    for (const auto& f : CoopDungeon::AutogenFlags()) {
        CHECK(seen.insert(f.hash).second);
        CHECK_FALSE(named.contains(f.hash));  // a named flag belongs in Flags()
        CHECK(CoopDungeon::IsDungeonScene(f.dungeon));
        CHECK(list->contains(host::FlagKey(f.hash, std::uint16_t{0})));
        CHECK_FALSE(CoopDungeon::HeldIn(f.hash).has_value());  // written at once, as the web's flag
        plate |= f.hash == 0x0f8f3fa3;
    }
    CHECK(plate);
    CHECK(list->contains(host::FlagKey(host::flag_hash("InsideTheDekuTree_RoomE_KillSpiderWave"), 0)));

    // Through watchers, as the hook shares it: B's 1 reaches A's game.
    namespace proto = coop::protocol;
    const auto key = host::FlagKey(0x0f8f3fa3, std::uint16_t{0});
    Game a;
    Game b;
    a.set(key, 0);
    b.set(key, 0);
    host::FlagWatcher wa(*list, host::FlagRole::player);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wa.frame(a);
    wb.frame(b);
    b.set(key, 1);
    const auto out = wb.frame(b);
    REQUIRE(out.size() == 1);
    proto::Message m;
    REQUIRE(proto::decode(out[0].bytes, m) == proto::Error::none);
    const auto* set = std::get_if<proto::FlagSet>(&m);
    REQUIRE(set);
    REQUIRE(set->flags.size() == 1);
    CHECK(set->flags[0].key == key);
    wa.receive(proto::Message{proto::FlagState{1, set->flags}});
    wa.frame(a);
    CHECK(a.values[key].value == 1);
}

TEST_CASE("dungeon: the Fire Temple pillar's fallen state and every room's unnamed bool are shared (#116)") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    REQUIRE(list);
    std::set<std::uint32_t> autogen;
    std::map<std::string, int> per_dungeon;
    for (const auto& f : CoopDungeon::AutogenFlags()) {
        autogen.insert(f.hash);
        ++per_dungeon[f.dungeon];
    }
    // Generated from the romfs (tools/puppet_pack/autogen_flags.py): 47 room bools in 6 dungeons.
    CHECK(autogen.size() == 47);
    CHECK(per_dungeon["FireTemple"] == 18);
    CHECK(per_dungeon["GerudoTrainingGround"] == 22);
    // The big pillar's Buried (Logic/FireTemple_Level_Room_10_Prefab_3284; its event flow waits on the name),
    // the two hammer floors under it, and the Deku torch plate seen in a game.
    for (const std::uint32_t h : {0xa82cc7a2u, 0xce19219cu, 0x03a335cbu, 0x0f8f3fa3u}) {
        CHECK(autogen.contains(h));
    }
    // One player's own stay out: the refill respawns (pots, grass), a gossip stone's fairy, Ganon's Castle
    // room 24's "walked in".
    for (const std::uint32_t h : {0x21958973u, 0xfe6d1779u, 0x58445aa7u, 0x186adf43u, 0xaecf3cfeu, 0x9de51737u,
                                  0x0fa9d971u, 0xbd902a5bu}) {
        CHECK_FALSE(autogen.contains(h));
        CHECK_FALSE(list->contains(host::FlagKey(h, std::uint16_t{0})));
    }

    // A hammered the pillar from its top (it rode down, its game buried it); B stood on top too. B's game
    // gets the bool, so its room graph makes the fallen pillar and the respawn swap at the bottom.
    const auto key = host::FlagKey(0xa82cc7a2, std::uint16_t{0});
    REQUIRE(list->contains(key));
    Game a;
    Game b;
    a.set(key, 0);
    b.set(key, 0);
    host::FlagWatcher wa(*list, host::FlagRole::player);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wa.frame(a);
    wb.frame(b);
    a.set(key, 1);
    const auto out = wa.frame(a);
    REQUIRE(out.size() == 1);
    proto::Message m;
    REQUIRE(proto::decode(out[0].bytes, m) == proto::Error::none);
    const auto* set = std::get_if<proto::FlagSet>(&m);
    REQUIRE(set);
    REQUIRE(set->flags.size() == 1);
    wb.receive(proto::Message{proto::FlagState{1, set->flags}});
    wb.frame(b);
    CHECK(b.values[key].value == 1);
}

TEST_CASE("dungeon: the slingshot room's ladder event played in A is set in B (issue 129)") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    REQUIRE(list.has_value());
    const auto ladder =
        host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder"), 0);
    const auto cleared = host::FlagKey(host::flag_hash("InsideTheDekuTree_RoomC_Cleared"), 0);

    Game a;
    Game b;
    for (Game* g : {&a, &b}) {
        g->set(ladder, 0);
        g->set(cleared, 0);
    }
    host::FlagWatcher wa(*list, host::FlagRole::player);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wa.frame(a);
    wb.frame(b);

    // A shoots the eye switch: its game plays the event and clears the room.
    a.set(ladder, 1);
    a.set(cleared, 1);
    const auto out = wa.frame(a);
    REQUIRE(out.size() == 1);
    proto::Message m;
    REQUIRE(proto::decode(out[0].bytes, m) == proto::Error::none);
    const auto* set = std::get_if<proto::FlagSet>(&m);
    REQUIRE(set);
    CHECK(set->flags.size() == 2);

    wb.receive(proto::Message{proto::FlagState{1, set->flags}});
    wb.frame(b);
    CHECK(b.values[ladder].value == 1);
    CHECK(b.values[cleared].value == 1);
}

TEST_CASE("dungeon: the room switch flags that are not saved are shared live too") {
    std::set<std::uint32_t> saved;
    for (const auto& f : CoopDungeon::Flags()) saved.insert(f.hash);
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    REQUIRE(list);
    bool switch_on = false;
    for (const auto& f : CoopDungeon::LiveFlags()) {
        CHECK_MESSAGE(host::flag_hash(f.name) == f.hash, f.name);
        CHECK(saved.count(f.hash) == 0);
        CHECK(list->contains(host::FlagKey(f.hash, std::uint16_t{0})));
        // Never one player's own state.
        CHECK(std::string(f.name).find("OnThe") == std::string::npos);
        CHECK(std::string(f.name).find("Navi") == std::string::npos);
        switch_on |= std::string(f.name) == "ForestTemple_Room17_Switch_On_02";
    }
    CHECK(switch_on);
}

namespace {

// B's hold, as the hook makes it: held while B's Link is in that dungeon.
host::HoldFlagStore::Hold HoldIn(const bool& link, const std::string& scene) {
    return [&link, &scene](host::FlagKey key) {
        if (!link) return false;
        const auto in = CoopDungeon::HeldIn(key.hash);
        return in && (in->empty() ? CoopDungeon::IsDungeonScene(scene) : scene == *in);
    };
}

// The same with the row 98(2) result hold in front, as coop_hook.cpp orders them: the flags of an event whose result
// we set here (`applied`) land; a result guard of a class-asker event not set and not played (`played`) waits.
host::HoldFlagStore::Hold HookHold(const bool& link, const std::string& scene, const std::set<std::string>& applied,
                                   const std::set<std::string>& played) {
    return [&link, &scene, &applied, &played](host::FlagKey key) {
        if (!link) return false;
        const auto rh = CoopEventResult::HoldFor(scene, applied, [&](const std::string& e) { return played.contains(e); });
        if (rh.Releases(key.hash)) return false;
        if (rh.Waits(key.hash)) return true;
        const auto in = CoopDungeon::HeldIn(key.hash);
        return in && (in->empty() ? CoopDungeon::IsDungeonScene(scene) : scene == *in);
    };
}

} // namespace

TEST_CASE("dungeon: the ladder event waits while B is in the Deku Tree, B's own eye still works (issue 129)") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto ladder =
        host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder"), 0);
    const auto cleared = host::FlagKey(host::flag_hash("InsideTheDekuTree_RoomC_Cleared"), 0);
    REQUIRE(CoopDungeon::HeldIn(ladder.hash) == std::optional<std::string_view>("InsideTheDekuTree"));
    // Row 98(2): the room clear (it deletes the ladder's asker) is no longer held by hand: it is the event's
    // generated result guard, waiting in the Deku Tree until our result is set or we played it.
    REQUIRE_FALSE(CoopDungeon::HeldIn(cleared.hash).has_value());

    Game b;
    b.set(ladder, 0);
    b.set(cleared, 0);
    bool link = true;
    std::string scene = "InsideTheDekuTree";
    const std::set<std::string> applied;
    const std::set<std::string> played;
    host::HoldFlagStore store(b, HookHold(link, scene, applied, played));
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wb.frame(store);

    // A shot the eye: the room clear is written, the ladder event is held.
    const std::vector<proto::FlagEntry> from_a{{ladder, proto::FlagKind::boolean, 1},
                                              {cleared, proto::FlagKind::boolean, 1}};
    wb.receive(proto::Message{proto::FlagState{1, from_a}});
    wb.frame(store);
    CHECK(b.values[cleared].value == 0);  // B's asker stays for B's event (or B's own eye shot)
    CHECK(b.values[ladder].value == 0);   // B's eye switch still plays the event
    wb.frame(store);
    CHECK(b.values[ladder].value == 0);

    // B leaves the Deku Tree (or loads): both are written, so the room
    // loads with the ladder down and the room cleared.
    scene = "KokiriForest";
    wb.frame(store);
    CHECK(b.values[ladder].value == 1);
    CHECK(b.values[cleared].value == 1);
}

TEST_CASE("dungeon: row 98(2): once our asker's ladder result is set, its flags land in the Deku Tree (no leaving)") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto ladder =
        host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder"), 0);
    const auto cleared = host::FlagKey(host::flag_hash("InsideTheDekuTree_RoomC_Cleared"), 0);
    Game b;
    b.set(ladder, 0);
    b.set(cleared, 0);
    bool link = true;
    std::string scene = "InsideTheDekuTree";
    std::set<std::string> applied;
    const std::set<std::string> played;
    host::HoldFlagStore store(b, HookHold(link, scene, applied, played));
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wb.frame(store);
    wb.receive(proto::Message{proto::FlagState{1, {{ladder, proto::FlagKind::boolean, 1},
                                                   {cleared, proto::FlagKind::boolean, 1}}}});
    wb.frame(store);
    CHECK(b.values[ladder].value == 0);   // held: our result not set yet
    CHECK(b.values[cleared].value == 0);  // waits: it would delete our asker first
    applied.insert("InsideTheDekuTree_Gimmick_FallingLadder");  // our asker's EventBool + EventFinish read 1
    wb.frame(store);
    CHECK(b.values[ladder].value == 1);
    CHECK(b.values[cleared].value == 1);
}

TEST_CASE("dungeon: row 98(2): the room clear lands at once in the Deku Tree once our game played the ladder itself") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto cleared = host::FlagKey(host::flag_hash("InsideTheDekuTree_RoomC_Cleared"), 0);
    Game b;
    b.set(cleared, 0);
    bool link = true;
    std::string scene = "InsideTheDekuTree";
    const std::set<std::string> applied;
    const std::set<std::string> played{"InsideTheDekuTree_Gimmick_FallingLadder"};
    host::HoldFlagStore store(b, HookHold(link, scene, applied, played));
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wb.frame(store);
    wb.receive(proto::Message{proto::FlagState{1, {{cleared, proto::FlagKind::boolean, 1}}}});
    wb.frame(store);
    CHECK(b.values[cleared].value == 1);
}

TEST_CASE("dungeon: only the slingshot room's clear waits with its event, other Deku Tree rooms stay live") {
    for (const char* live : {"InsideTheDekuTree_RoomD_Cleared", "InsideTheDekuTree_RoomD_StepSwitch_On_01",
                             "InsideTheDekuTree_RoomE_Cleared", "InsideTheDekuTree_RoomG_WaterLevel"}) {
        CAPTURE(live);
        CHECK_FALSE(CoopDungeon::HeldIn(host::flag_hash(live)).has_value());
    }
}

TEST_CASE("dungeon: a held event B plays itself is not written again, and is sent as B's") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto ladder =
        host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder"), 0);
    Game b;
    b.set(ladder, 0);
    bool link = true;
    std::string scene = "InsideTheDekuTree";
    host::HoldFlagStore store(b, HoldIn(link, scene));
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wb.frame(store);
    wb.receive(proto::Message{proto::FlagState{1, {{ladder, proto::FlagKind::boolean, 1}}}});
    wb.frame(store);
    CHECK(b.values[ladder].value == 0);
    b.set(ladder, 1);  // B shoots its own eye: B's game plays the event
    const auto out = wb.frame(store);
    CHECK(out.size() == 1);  // B's change goes out as usual
    scene = "KokiriForest";
    wb.frame(store);
    CHECK(b.values[ladder].value == 1);
    CHECK(store.held() == 1);  // tried once; dropped once B played it
}

TEST_CASE("dungeon: loading (no Link) writes what was held, the corridor waits in the Forest Temple") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto twist = host::FlagKey(host::flag_hash("ForestTemple_TwistCorridor_IsReverse"), 0);
    REQUIRE(CoopDungeon::HeldIn(twist.hash) == std::optional<std::string_view>("ForestTemple"));
    Game b;
    b.set(twist, 0);
    bool link = true;
    std::string scene = "ForestTemple";
    host::HoldFlagStore store(b, HoldIn(link, scene));
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wb.frame(store);
    wb.receive(proto::Message{proto::FlagState{1, {{twist, proto::FlagKind::boolean, 1}}}});
    wb.frame(store);
    CHECK(b.values[twist].value == 0);  // never turns under B
    link = false;                       // a load: the room is built again
    wb.frame(store);
    CHECK(b.values[twist].value == 1);
}

TEST_CASE("dungeon: the host answers a held write with the asked value") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto ladder =
        host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder"), 0);
    Game h;
    h.set(ladder, 0);
    bool link = true;
    std::string scene = "InsideTheDekuTree";
    host::HoldFlagStore store(h, HoldIn(link, scene));
    host::FlagWatcher wh(*list, host::FlagRole::host);
    wh.frame(store);
    wh.receive(proto::Message{proto::FlagSet{2, {{ladder, proto::FlagKind::boolean, 1}}}});
    const auto out = wh.frame(store);
    bool answered = false;
    for (const auto& o : out) {
        proto::Message m;
        REQUIRE(proto::decode(o.bytes, m) == proto::Error::none);
        if (const auto* st = std::get_if<proto::FlagState>(&m)) {
            for (const auto& e : st->flags) answered |= e.key == ladder && e.value == 1;
        }
    }
    CHECK(answered);
    CHECK(h.values[ladder].value == 0);
    scene = "KokiriForest";
    wh.frame(store);
    CHECK(h.values[ladder].value == 1);
}

TEST_CASE("dungeon: which flags are held: class-asker gimmick events and the corridor, not switches or keys") {
    std::size_t held = 0;
    std::size_t live = 0;
    for (const auto& f : CoopDungeon::Flags()) {
        const auto in = CoopDungeon::HeldIn(f.hash);
        const std::string name = f.name;
        constexpr std::string_view kPlayed = "HavePlayedEvent.";
        if (name.starts_with(kPlayed)) {
            // Row 98(2): a class-live event (every result by shared flags, coop_event_result.cpp) is not held.
            const bool is_live = CoopEventResult::IsLive(std::string_view(name).substr(kPlayed.size()));
            CHECK_MESSAGE(in.has_value() == !is_live, name);
            if (in && !in->empty()) CHECK(CoopDungeon::IsDungeonScene(*in));
            live += is_live ? 1 : 0;
        }
        if (!name.starts_with("HavePlayedEvent.") &&
            (name.find("SmallKey") != std::string::npos || name.find("WaterLevel") != std::string::npos)) {
            CHECK_FALSE(in.has_value());
        }
        held += in ? 1 : 0;
    }
    // 202 played events in the list (11-17): about 155 class live, about 47 held, plus the 7 corridor flags.
    CHECK(live > 100);
    CHECK(held > 40);
    CHECK(held < 100);
    CHECK(CoopDungeon::HeldIn(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder")).has_value());
    CHECK(CoopDungeon::HeldIn(host::flag_hash("HavePlayedEvent.SpritTemple_Room01_Gimmick")) == std::nullopt);
    CHECK(CoopDungeon::IsDungeonScene("InsideJabuJabuBelly"));
    CHECK_FALSE(CoopDungeon::IsDungeonScene("KokiriForest"));
}

TEST_CASE("dungeon: a room switch flag crosses one way, the game's own reset stays its own") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto gate = host::FlagKey(host::flag_hash("GerudoTrainingGround_Gate"), 0);
    for (const auto& f : CoopDungeon::LiveFlags()) {
        CHECK(std::string(f.name).find("AppearWolfos") == std::string::npos);  // a player sensor
    }
    Game a;
    Game b;
    a.set(gate, 0);
    b.set(gate, 0);
    host::FlagWatcher wa(*list, host::FlagRole::player);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    for (auto* w : {&wa, &wb}) CHECK(w->share_one_way(gate));
    CHECK_FALSE(wa.share_one_way(gate));  // once
    wa.frame(a);
    wb.frame(b);

    // A opens the gate: a 1 goes out and is written in B.
    a.set(gate, 1);
    auto out = wa.frame(a);
    REQUIRE(out.size() == 1);
    proto::Message m;
    REQUIRE(proto::decode(out[0].bytes, m) == proto::Error::none);
    wb.receive(proto::Message{proto::FlagState{1, std::get<proto::FlagSet>(m).flags}});
    wb.frame(b);
    CHECK(b.values[gate].value == 1);

    // A leaves the room: its game resets the flag. Nothing goes out.
    a.set(gate, 0);
    CHECK(wa.frame(a).empty());
    // A 0 from elsewhere is never written over B's 1.
    wb.receive(proto::Message{proto::FlagState{1, {{gate, proto::FlagKind::boolean, 0}}}});
    wb.frame(b);
    CHECK(b.values[gate].value == 1);

    // B loads (settling): its game resets the flag and it is not written back.
    wb.set_loading(true);
    b.set(gate, 0);
    CHECK(wb.frame(b).empty());
    wb.set_loading(false);
    wb.frame(b);
    CHECK(b.values[gate].value == 0);
    CHECK(wb.stats().load_reverts == 0);
}

TEST_CASE("dungeon: a held flag names its event, for the live start") {
    CHECK(CoopDungeon::EventOf(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder")) ==
          "InsideTheDekuTree_Gimmick_FallingLadder");
    CHECK(CoopDungeon::EventOf(host::flag_hash("ForestTemple_TwistCorridor_IsReverse")).empty());
    CHECK(CoopDungeon::EventOf(0).empty());
    Game g;
    const auto ladder =
        host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder"), 0);
    g.set(ladder, 0);
    host::HoldFlagStore store(g, [](host::FlagKey) { return true; });
    CHECK_FALSE(store.write({ladder, coop::protocol::FlagKind::boolean, 1}));
    REQUIRE(store.held_keys().size() == 1);
    CHECK(store.held_keys()[0] == ladder);
}

TEST_CASE("dungeon: what played live in a player test stays live (basement water, torch room)") {
    namespace proto = coop::protocol;
    const auto water = host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_RoomG_WaterLevelDrop"), 0);
    const auto torches = host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_RoomH_CandleStick"), 0);
    const auto level = host::FlagKey(host::flag_hash("InsideTheDekuTree_RoomG_WaterLevel"), 0);
    CHECK_FALSE(CoopDungeon::HeldIn(water.hash).has_value());
    CHECK_FALSE(CoopDungeon::HeldIn(torches.hash).has_value());
    CHECK_FALSE(CoopDungeon::HeldIn(level.hash).has_value());
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    Game b;
    for (const auto& k : {water, torches, level}) b.set(k, 0);
    bool link = true;
    std::string scene = "InsideTheDekuTree";
    host::HoldFlagStore store(b, HoldIn(link, scene));
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wb.frame(store);
    wb.receive(proto::Message{proto::FlagState{1, {{water, proto::FlagKind::boolean, 1},
                                                   {torches, proto::FlagKind::boolean, 1},
                                                   {level, proto::FlagKind::boolean, 1}}}});
    wb.frame(store);
    CHECK(b.values[water].value == 1);  // written at once, B in the Deku Tree: as in the player test
    CHECK(b.values[torches].value == 1);
    CHECK(b.values[level].value == 1);
    CHECK(store.held() == 0);
}

TEST_CASE("dungeon: a held flag that a load puts back to 0 is kept for later, never sent as 0") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto ladder =
        host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder"), 0);
    Game b;
    b.set(ladder, 0);
    bool link = false;  // the title screen or a load: nothing held
    std::string scene = "InsideTheDekuTree";
    host::HoldFlagStore store(b, HoldIn(link, scene));
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wb.frame(store);
    wb.set_loading(true);
    wb.receive(proto::Message{proto::FlagState{1, {{ladder, proto::FlagKind::boolean, 1}}}});
    wb.frame(store);
    CHECK(b.values[ladder].value == 1);

    // The save loads over it with 0, and our Link appears in the Deku Tree.
    wb.set_loading(false);
    link = true;
    b.set(ladder, 0);
    const auto zero_sent = [&](const std::vector<host::Outgoing>& out) {
        for (const auto& o : out) {
            proto::Message m;
            if (proto::decode(o.bytes, m) != proto::Error::none) continue;
            if (const auto* set = std::get_if<proto::FlagSet>(&m)) {
                for (const auto& e : set->flags) {
                    if (e.key == ladder && e.value == 0) return true;
                }
            }
        }
        return false;
    };
    CHECK_FALSE(zero_sent(wb.frame(store)));
    CHECK_FALSE(zero_sent(wb.frame(store)));
    CHECK(b.values[ladder].value == 0);  // held in our game while in the Deku Tree

    scene = "KokiriForest";  // out of the dungeon: the group's 1 is written
    CHECK_FALSE(zero_sent(wb.frame(store)));
    CHECK(b.values[ladder].value == 1);
}

TEST_CASE("dungeon: the Dark Link rooms' passage flags and the Shadow boat's crush flag are each player's own") {
    std::set<std::uint32_t> listed;
    for (const auto& f : CoopDungeon::Flags()) listed.insert(f.hash);
    for (const char* name : {"WaterTemple_Room23_DeleteBackRoom", "WaterTemple_Room23_DeleteFrontRoom",
                             "WaterTemple_Room23_ResetDeletedRoom", "WaterTemple_SwitchDarkLinkDome",
                             "WaterTemple_Room22_CreateDome", "ShadowTemple_Level_PaddelShip_Elevator_AbyssTriggered"}) {
        CAPTURE(name);
        CHECK_FALSE(listed.contains(host::flag_hash(name)));
    }
    CHECK(listed.contains(host::flag_hash("WaterTemple_Room22_Cleared")));  // the room beaten stays shared
    CHECK(listed.contains(host::flag_hash("WaterTemple_Room23_Cleared")));
}

TEST_CASE("dungeon: a player's place in a boss fight is never shared, the group's progress is (issue 148)") {
    // Player test, Deku Tree: the boss door shut for a player still outside, and every reload got
    // it back from the group (the join snapshot). Those flags are left out.
    std::set<std::uint32_t> listed;
    for (const auto& f : CoopDungeon::Flags()) listed.insert(f.hash);
    REQUIRE(CoopDungeon::NotShared().size() == 6);  // with Dodongo's and the Fire Temple's arena floors
    for (const auto name : CoopDungeon::NotShared()) {
        CHECK_FALSE(listed.contains(host::flag_hash(std::string(name))));
    }
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    CHECK_FALSE(list->contains(host::FlagKey(host::flag_hash("InsideTheDekuTree_BossArea_ShutIn"), 0)));
    // What the group did stays shared: the boss beaten, a boss lock opened with the key.
    CHECK(list->contains(host::FlagKey(host::flag_hash("InsideTheDekuTree_Heart_Container_Obtained"), 0)));
    CHECK(list->contains(host::FlagKey(host::flag_hash("ForestTemple_BossRoom_LockOpne"), 0)));
}

TEST_CASE("dungeon: a player still behind the boss door does not shut it for one who reloads (issue 148)") {
    // The testers' sequence: A stays in the newer game, with the boss door shut behind A; B
    // loads an older save, where it is open. A's game keeps its ShutIn at 1; B's must stay 0.
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto shut = host::FlagKey(host::flag_hash("InsideTheDekuTree_BossArea_ShutIn"), 0);
    const auto beaten = host::FlagKey(host::flag_hash("InsideTheDekuTree_Heart_Container_Obtained"), 0);
    Game a;
    Game b;
    for (Game* g : {&a, &b}) {
        g->set(shut, 0);
        g->set(beaten, 0);
    }
    host::FlagWatcher wa(*list, host::FlagRole::host);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wa.frame(a);
    wb.frame(b);
    // A walks in: the door shuts behind A. Nothing goes out for it.
    a.set(shut, 1);
    CHECK(wa.frame(a).empty());
    // B reloads its older save and joins again: the snapshot and any state A sends (an old
    // build, or a host table from before) never write the shut door into B's game.
    proto::FlagSnapshot snap;
    snap.flags = {{shut, proto::FlagKind::boolean, 1}};
    wb.receive(proto::Message{snap});
    wb.receive(proto::Message{proto::FlagState{1, {{shut, proto::FlagKind::boolean, 1}}}});
    wb.frame(b);
    CHECK(b.values[shut].value == 0);
    // What the group did still reaches B: A beats the boss.
    a.set(beaten, 1);
    const auto out = wa.frame(a);
    REQUIRE(out.size() == 1);
    proto::Message m;
    REQUIRE(proto::decode(out[0].bytes, m) == proto::Error::none);
    wb.receive(m);
    wb.frame(b);
    CHECK(b.values[beaten].value == 1);
}
