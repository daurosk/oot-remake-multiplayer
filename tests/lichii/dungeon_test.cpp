// Tests for the shared dungeon flags (lichii/hook/coop_dungeon.cpp).
#include <doctest/doctest.h>

#include <map>
#include <set>
#include <string>

#include "coop_dungeon.h"
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

TEST_CASE("dungeon: the Water Temple's water level and switches are in the list") {
    bool level = false;
    bool shutter = false;
    for (const auto& f : CoopDungeon::Flags()) {
        level |= std::string(f.name) == "WaterTemple_Room00_WaterLevel" && f.is_int;
        shutter |= std::string(f.name) == "WaterTemple_Room00_Switch_NearTheShutter";
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
    // Where one player (or Ruto) stands, state set both ways, and what Link reads stay
    // each player's own (review of da6a300).
    for (const char* n : {"Insidejabujabu_Room7_Switch1_InsideArea", "Insidejabujabu_Room7_Switch2_OnlyLinkOrLuto",
                          "Insidejabujabu_Room7_NearTheSwitch", "Insidejabujabu_Room7_NearTheSwitch_Tentacle_Gate",
                          "Gimmick_Insidejabujabu_Updown_MoveWater", "Gimmick_Insidejabujabu_down_MoveWater",
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

TEST_CASE("dungeon: Merge adds to the list, makes one when there is none, or does nothing when off") {
    std::optional<host::SharedFlags> none;
    CHECK(CoopDungeon::Merge(none, false) == 0);
    CHECK_FALSE(none.has_value());

    const std::size_t all =
        CoopDungeon::Flags().size() + CoopDungeon::LiveFlags().size() + CoopDungeon::Keys().size();
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

} // namespace

TEST_CASE("dungeon: the ladder event waits while B is in the Deku Tree, B's own eye still works (issue 129)") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto ladder =
        host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder"), 0);
    const auto cleared = host::FlagKey(host::flag_hash("InsideTheDekuTree_RoomC_Cleared"), 0);
    REQUIRE(CoopDungeon::HeldIn(ladder.hash) == std::optional<std::string_view>("InsideTheDekuTree"));
    // deku-switches-b: the room clear deletes the ladder's asker in the room graph, so it waits with the event.
    REQUIRE(CoopDungeon::HeldIn(cleared.hash) == std::optional<std::string_view>("InsideTheDekuTree"));

    Game b;
    b.set(ladder, 0);
    b.set(cleared, 0);
    bool link = true;
    std::string scene = "InsideTheDekuTree";
    host::HoldFlagStore store(b, HoldIn(link, scene));
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

TEST_CASE("dungeon: which flags are held: gimmick events and the corridor, not switches or keys") {
    std::size_t held = 0;
    for (const auto& f : CoopDungeon::Flags()) {
        const auto in = CoopDungeon::HeldIn(f.hash);
        const std::string name = f.name;
        const bool live_on_test6 = name == "HavePlayedEvent.InsideTheDekuTree_Gimmick_RoomG_WaterLevelDrop" ||
                                   name == "HavePlayedEvent.InsideTheDekuTree_Gimmick_RoomH_CandleStick";
        if (name.starts_with("HavePlayedEvent.") && !live_on_test6) {
            CHECK_MESSAGE(in.has_value(), name);
            if (in && !in->empty()) CHECK(CoopDungeon::IsDungeonScene(*in));
        }
        if (!name.starts_with("HavePlayedEvent.") &&
            (name.find("SmallKey") != std::string::npos || name.find("WaterLevel") != std::string::npos)) {
            CHECK_FALSE(in.has_value());
        }
        held += in ? 1 : 0;
    }
    CHECK(held > 190);
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
