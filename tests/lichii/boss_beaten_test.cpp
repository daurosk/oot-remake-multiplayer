// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_boss.h"
#include "coop_boss_beaten.h"
#include "coop_boss_scenes.h"
#include "coop_dungeon.h"
#include "coop_quests.h"

namespace {

using CoopBossBeaten::Apply;
using CoopBossBeaten::BeatenSet;
using CoopBossBeaten::Write;

// A game's flags: unset ones read as nothing.
struct Flags {
    std::map<std::uint32_t, std::uint32_t> v;
    std::optional<std::uint32_t> operator()(std::uint32_t hash) const {
        const auto it = v.find(hash);
        return it == v.end() ? std::nullopt : std::optional<std::uint32_t>(it->second);
    }
};

// A game that never saw the boss die: every bool of the set at the other value, the quest at the boss step.
Flags Alive(const BeatenSet& s) {
    Flags f;
    for (const auto& b : s.bools) {
        f.v[b.hash] = b.value ? 0 : 1;
    }
    f.v[s.quest] = s.below.back();
    return f;
}

const BeatenSet& Gohma() {
    const BeatenSet* s = CoopBossBeaten::ForRoom(0);
    REQUIRE(s != nullptr);
    return *s;
}

// Our Link in the boss's area (the room and the room before its door), or not.
bool InTheArea(std::uint8_t) {
    return true;
}
bool OutOfTheArea(std::uint8_t) {
    return false;
}
const std::function<bool(std::uint8_t)> kNear = InTheArea;
const std::function<bool(std::uint8_t)> kAway = OutOfTheArea;

bool HasBool(const BeatenSet& s, std::string_view name, bool value) {
    return std::any_of(s.bools.begin(), s.bools.end(),
                       [&](const auto& b) { return b.name == name && b.value == value; });
}

} // namespace

TEST_CASE("boss beaten: the generated sets, one per boss (eight rooms, Ganondorf, Ganon), hashes from their names") {
    const auto sets = CoopBossBeaten::Sets();
    REQUIRE(sets.size() == 10);
    for (std::uint8_t room = 0; room < 10; ++room) {
        const BeatenSet* s = CoopBossBeaten::ForRoom(room);
        REQUIRE(s != nullptr);
        CAPTURE(s->boss);
        CHECK(std::string_view(s->boss) == CoopBossScenes::Bosses()[room].name);
        CHECK((CoopBoss::RoomByIndex(room) != nullptr) == (room < 8));
        REQUIRE(CoopBossBeaten::DeathScene(room).has_value());
        CHECK(s->step == coop::protocol::flag_hash(s->step_name));
        CHECK(!s->below.empty());
        CHECK(s->below.front() == coop::protocol::flag_hash("NotReady"));
        CHECK(std::find(s->below.begin(), s->below.end(), s->step) == s->below.end());
        CHECK(std::find(s->below.begin(), s->below.end(), coop::protocol::flag_hash("Complete")) == s->below.end());
        CHECK(!s->bools.empty());
        for (const auto& b : s->bools) {
            CAPTURE(b.name);
            CHECK(b.hash == coop::protocol::flag_hash(b.name));
            // the fight's own state is never given: the stale-flag mend clears it outside a boss room (issue 148)
            const auto ns = CoopDungeon::NotShared();
            CHECK(std::find(ns.begin(), ns.end(), std::string_view(b.name)) == ns.end());
        }
    }
    CHECK(CoopBossBeaten::ForRoom(10) == nullptr);
    CHECK_FALSE(CoopBossBeaten::DeathScene(10).has_value());
}

namespace {

const BeatenSet& Ganondorf() {
    const BeatenSet* s = CoopBossBeaten::ForRoom(8);
    REQUIRE(s != nullptr);
    return *s;
}

const BeatenSet& Ganon() {
    const BeatenSet* s = CoopBossBeaten::ForRoom(9);
    REQUIRE(s != nullptr);
    return *s;
}

} // namespace

TEST_CASE("boss beaten: Ganondorf and Ganon move Defeat_Ganon, each game's own quest (not shared)") {
    const BeatenSet& dorf = Ganondorf();
    const BeatenSet& ganon = Ganon();
    CHECK(std::string_view(dorf.quest_name) == "Defeat_Ganon");
    CHECK(dorf.quest == ganon.quest);
    CHECK(CoopQuests::Find(dorf.quest) == nullptr);  // coop_quests.cpp leaves the final battle out
    CHECK(std::string_view(dorf.step_name) == "Step06");
    CHECK(dorf.below.back() == coop::protocol::flag_hash("Step05"));
    CHECK(std::string_view(ganon.step_name) == "Complete");
    CHECK(ganon.below.back() == coop::protocol::flag_hash("Step09"));
    // Ganondorf's step is below Ganon's: a save past Ganondorf is still given Ganon's.
    CHECK(std::find(ganon.below.begin(), ganon.below.end(), dorf.step) != ganon.below.end());
    CHECK(HasBool(dorf, "HavePlayedEvent.Dm_InsideGanonCastle_BossOut", true));
    CHECK(HasBool(ganon, "FinalBattleField_DefeatGanonStart", true));
    CHECK(HasBool(ganon, "HavePlayedEvent.Dm_FinalBattle_BossOut", true));
}

TEST_CASE("boss beaten: Ganondorf and Ganon are given outside the end sequence's scenes, the furthest step once") {
    const BeatenSet& dorf = Ganondorf();
    const BeatenSet& ganon = Ganon();
    const std::uint32_t step05 = coop::protocol::flag_hash("Step05");
    const auto quest_writes = [&](const std::vector<Write>& w) {
        std::vector<std::uint32_t> v;
        for (const Write& x : w) {
            if (x.is_enum) {
                CHECK(x.hash == dorf.quest);
                v.push_back(x.value);
            }
        }
        return v;
    };
    // Ganondorf alone: in his castle, the escape or Ganon's field nothing; at the next load elsewhere Step06.
    for (const char* kept : {"InsideGanonCastle", "CrumblingGanonCastle", "FinalBattleField"}) {
        CAPTURE(kept);
        CHECK(CoopBossBeaten::KeptScene(8, kept));
        CHECK(CoopBossBeaten::KeptScene(9, kept));
        Flags f = Alive(dorf);
        Apply a;
        CHECK(a.Check(std::string(kept), true, std::vector<std::uint8_t>{8}, std::ref(f), kAway).empty());
    }
    CHECK_FALSE(CoopBossBeaten::KeptScene(8, "GanonCastle"));  // the castle grounds: no fight there
    CHECK_FALSE(CoopBossBeaten::KeptScene(0, "InsideGanonCastle"));
    {
        Flags f = Alive(dorf);
        Apply a;
        const auto w = a.Check(std::string("GanonCastle"), true, std::vector<std::uint8_t>{8}, std::ref(f), kAway);
        CHECK(w.size() == dorf.bools.size() + 1);
        CHECK((quest_writes(w) == std::vector<std::uint32_t>{dorf.step}));
    }
    // Both beaten (a joiner told both, in either order): one Defeat_Ganon write, Complete.
    for (const std::vector<std::uint8_t>& beaten : {std::vector<std::uint8_t>{8, 9}, std::vector<std::uint8_t>{9, 8}}) {
        Flags f = Alive(dorf);
        for (const auto& b : ganon.bools) {
            f.v[b.hash] = b.value ? 0 : 1;
        }
        f.v[dorf.quest] = step05;
        Apply a;
        const auto w = a.Check(std::string("HyruleField"), true, beaten, std::ref(f), kAway);
        CHECK((quest_writes(w) == std::vector<std::uint32_t>{ganon.step}));
        CHECK(w.size() == dorf.bools.size() + ganon.bools.size() + 1);
    }
    // Past Ganondorf already (Step06, Step07): only Ganon's step; a finished game (Complete): no step at all.
    for (const char* at : {"Step06", "Step07"}) {
        Flags f;
        f.v[dorf.quest] = coop::protocol::flag_hash(at);
        Apply a;
        CHECK((quest_writes(a.Check(std::string("HyruleField"), true, std::vector<std::uint8_t>{8, 9}, std::ref(f), kAway)) ==
               std::vector<std::uint32_t>{ganon.step}));
    }
    Flags done;
    done.v[dorf.quest] = coop::protocol::flag_hash("Complete");
    Apply a;
    CHECK(quest_writes(a.Check(std::string("HyruleField"), true, std::vector<std::uint8_t>{8, 9}, std::ref(done), kAway))
              .empty());
}

TEST_CASE("boss beaten: Gohma's set is what a save that beat her holds (hs-gohma-death's save prep)") {
    const BeatenSet& s = Gohma();
    CHECK(std::string_view(s.quest_name) == "Get_StoneOfForest");
    CHECK(std::string_view(s.step_name) == "GoToWarpPoint");
    CHECK(HasBool(s, "Gohma_DefeatBoss", true));
    CHECK(HasBool(s, "HavePlayedEvent.Dm_Deku_BossIn", true));
    CHECK(HasBool(s, "HavePlayedEvent.Dm_Deku_BossOut", true));
    CHECK(HasBool(s, "HavePlayedEvent.InsideTheDekuTree_BossDie", true));
    CHECK(!HasBool(s, "InsideTheDekuTree_BossArea_ShutIn", true));
    CHECK(HasBool(*CoopBossBeaten::ForRoom(1), "Enemy_DungeonBoss_Dodongo_BossAppear", false));
    CHECK(HasBool(*CoopBossBeaten::ForRoom(5), "Morpha_DeleteWater", true));
}

TEST_CASE("boss beaten: the shared quests' steps agree with CoopQuests (the step after the boss, the ones before)") {
    int checked = 0;
    for (const BeatenSet& s : CoopBossBeaten::Sets()) {
        const CoopQuests::Quest* q = CoopQuests::Find(s.quest);
        if (q == nullptr) {
            continue;  // SageOfWater, SageOfShadow, SageOfSpirit are not shared (coop_quests.cpp)
        }
        CAPTURE(s.quest_name);
        CHECK(std::string_view(q->name) == s.quest_name);
        const auto at = CoopQuests::Rank(*q, s.step);
        REQUIRE(at.has_value());
        REQUIRE(s.below.size() == static_cast<std::size_t>(*at));
        for (std::size_t i = 0; i < s.below.size(); ++i) {
            CHECK(s.below[i] == coop::protocol::flag_hash(q->steps[i]));
        }
        ++checked;
    }
    CHECK(checked == 5);
}

TEST_CASE("boss beaten: record") {
    CoopBossBeaten::Record r;
    CHECK(r.Add(0));
    CHECK(!r.Add(0));
    CHECK(r.Add(9));    // Ganon: no dungeon room, a set of its own
    CHECK(!r.Add(10));  // no boss
    CHECK(r.Add(3));
    CHECK((r.Rooms() == std::vector<std::uint8_t>{0, 9, 3}));
    r.Clear();
    CHECK(r.Rooms().empty());
}

TEST_CASE("boss beaten: given one way, once per scene load, never in the boss's area") {
    const BeatenSet& s = Gohma();
    const std::vector<std::uint8_t> beaten{0};
    Flags f = Alive(s);
    Apply a;
    const std::string deku(CoopBoss::RoomByIndex(0)->scene);

    // In the boss's area of the Deku Tree: D9, its own death runs there; nothing given.
    CHECK(a.Check(deku, true, beaten, std::ref(f), kNear).empty());
    // Loading, then a field: nothing before Link's place reads.
    CHECK(a.Check(std::nullopt, false, beaten, std::ref(f), kAway).empty());
    CHECK(a.Check(std::string("KokiriForest"), false, beaten, std::ref(f), kAway).empty());
    std::vector<Write> w = a.Check(std::string("KokiriForest"), true, beaten, std::ref(f), kAway);
    REQUIRE(w.size() == s.bools.size() + 1);
    for (const auto& b : s.bools) {
        CHECK(std::any_of(w.begin(), w.end(), [&](const Write& x) {
            return !x.is_enum && x.hash == b.hash && x.value == (b.value ? 1u : 0u);
        }));
    }
    CHECK(w.back().is_enum);
    CHECK(w.back().hash == s.quest);
    CHECK(w.back().value == s.step);
    // Once per scene load.
    CHECK(a.Check(std::string("KokiriForest"), true, beaten, std::ref(f), kAway).empty());
    // The writes took: the next scene load finds nothing to give.
    for (const Write& x : w) {
        f.v[x.hash] = x.value;
    }
    CHECK(a.Check(std::nullopt, false, beaten, std::ref(f), kAway).empty());
    CHECK(a.Check(std::string("KokiriForest"), true, beaten, std::ref(f), kAway).empty());
    // A reload of an older save of ours (the boss alive in it again): the next scene load gives it again (#241).
    f = Alive(s);
    CHECK(a.Check(std::nullopt, false, beaten, std::ref(f), kAway).empty());
    CHECK(a.Check(std::string("HyruleField"), true, beaten, std::ref(f), kAway).size() == s.bools.size() + 1);
}

TEST_CASE("boss beaten: a finished dungeon is never stepped back, unread flags are left alone") {
    const BeatenSet& s = Gohma();
    const std::vector<std::uint8_t> beaten{0};
    Flags f = Alive(s);
    f.v[s.quest] = coop::protocol::flag_hash("Complete");
    f.v.erase(s.bools.front().hash);
    Apply a;
    const std::vector<Write> w = a.Check(std::string("KokiriForest"), true, beaten, std::ref(f), kAway);
    CHECK(w.size() == s.bools.size() - 1);
    CHECK(std::none_of(w.begin(), w.end(), [](const Write& x) { return x.is_enum; }));
    CHECK(std::none_of(w.begin(), w.end(), [&](const Write& x) { return x.hash == s.bools.front().hash; }));
}

TEST_CASE("boss beaten: a boss beaten while we stand in a scene is given at its next load, never mid-scene") {
    // boss-away-crash (review of c78d17c4): Kokiri Forest's Npc_Kokiri_011 reads Gohma_DefeatBoss live, and in the game
    // alone it never changes while Kokiri Forest is loaded (the kill is in the Deku Tree).
    const BeatenSet& s = Gohma();
    Flags f = Alive(s);
    Apply a;
    CHECK(a.Check(std::string("KokiriForest"), true, {}, std::ref(f), kAway).empty());  // co-op idle or nothing beaten
    const std::vector<std::uint8_t> beaten{0};
    // The guests beat Gohma while we stand in Kokiri Forest: nothing written in this scene, however long we stay.
    CHECK(a.Check(std::string("KokiriForest"), true, beaten, std::ref(f), kAway).empty());
    CHECK(a.Check(std::string("KokiriForest"), true, beaten, std::ref(f), kAway).empty());
    // The next load (the same scene again too): given then, once.
    CHECK(a.Check(std::nullopt, false, beaten, std::ref(f), kAway).empty());
    CHECK(a.Check(std::string("KokiriForest"), true, beaten, std::ref(f), kAway).size() == s.bools.size() + 1);
    CHECK(a.Check(std::string("KokiriForest"), true, beaten, std::ref(f), kAway).empty());
    // A scene reached without a loading frame between (a new scene name): that is a load too.
    Apply b;
    CHECK(b.Check(std::string("KokiriForest"), true, {}, std::ref(f), kAway).empty());
    CHECK(b.Check(std::string("KokiriForest"), true, beaten, std::ref(f), kAway).empty());
    CHECK(b.Check(std::string("LostWoods"), true, beaten, std::ref(f), kAway).size() == s.bools.size() + 1);
}

TEST_CASE("boss beaten: every boss's set waits for the next load when beaten mid-scene") {
    // Every set, not only Gohma's: Dodongo's BossOut is read by Death Mountain Trail, Phantom Ganon's
    // SageOfForest_IsBefore_Step08 by Kokiri Forest. A field none of the bosses is in.
    for (const BeatenSet& set : CoopBossBeaten::Sets()) {
        CAPTURE(set.boss);
        Flags f = Alive(set);
        Apply a;
        const std::vector<std::uint8_t> beaten{set.room};
        CHECK(a.Check(std::string("HyruleField"), true, {}, std::ref(f), kAway).empty());
        CHECK(a.Check(std::string("HyruleField"), true, beaten, std::ref(f), kAway).empty());
        CHECK(a.Check(std::nullopt, false, beaten, std::ref(f), kAway).empty());
        CHECK_FALSE(a.Check(std::string("HyruleField"), true, beaten, std::ref(f), kAway).empty());
    }
}

TEST_CASE("boss beaten: a record cleared mid-scene (the host loaded a save) gives nothing in that scene") {
    const BeatenSet& s = Gohma();
    Flags f = Alive(s);
    Apply a;
    const std::vector<std::uint8_t> beaten{0};
    CHECK(a.Check(std::string("KokiriForest"), false, beaten, std::ref(f), kAway).empty());  // loaded with it, place unread
    CHECK(a.Check(std::string("KokiriForest"), true, {}, std::ref(f), kAway).empty());       // cleared before it was given
}

TEST_CASE("boss beaten: only a proven death is one (a game took the boss's death event itself)") {
    namespace proto = coop::protocol;
    const std::string deku(CoopBoss::RoomByIndex(0)->scene);
    const auto scene = [](std::uint8_t boss, const std::string& where, proto::BossSceneKind kind) {
        return proto::Message{proto::BossScene{2, where, boss, kind, {}}};
    };
    CHECK(CoopBossBeaten::ProvenDeath(scene(0, deku, proto::BossSceneKind::outro_start)) == std::uint8_t{0});
    CHECK_FALSE(CoopBossBeaten::ProvenDeath(scene(0, "GohmaStage", proto::BossSceneKind::outro_start)));  // boss rush
    CHECK_FALSE(CoopBossBeaten::ProvenDeath(scene(0, deku, proto::BossSceneKind::intro_start)));
    CHECK(CoopBossBeaten::ProvenDeath(scene(0, deku, proto::BossSceneKind::outro_end)) == std::uint8_t{0});
    // Ganondorf in his castle, Ganon on his field; never their boss rush stages or another scene.
    CHECK(CoopBossBeaten::ProvenDeath(scene(8, "InsideGanonCastle", proto::BossSceneKind::outro_start)) ==
          std::uint8_t{8});
    CHECK(CoopBossBeaten::ProvenDeath(scene(9, "FinalBattleField", proto::BossSceneKind::outro_end)) == std::uint8_t{9});
    CHECK_FALSE(CoopBossBeaten::ProvenDeath(scene(8, "GanondorfStage", proto::BossSceneKind::outro_start)));
    CHECK_FALSE(CoopBossBeaten::ProvenDeath(scene(9, "GanonStage", proto::BossSceneKind::outro_start)));
    CHECK_FALSE(CoopBossBeaten::ProvenDeath(scene(9, "InsideGanonCastle", proto::BossSceneKind::outro_start)));
    CHECK_FALSE(CoopBossBeaten::ProvenDeath(scene(10, "InsideGanonCastle", proto::BossSceneKind::outro_start)));
    // BOSS_ROOM defeated is maybe a guess (a long event, no reading): never one, from anyone.
    for (const std::uint8_t player : {std::uint8_t{0}, std::uint8_t{2}}) {
        CHECK_FALSE(
            CoopBossBeaten::ProvenDeath(proto::Message{proto::BossRoom{player, deku, 0, proto::BossState::defeated}}));
    }
}

TEST_CASE("boss beaten: BOSS_SCENE's boss index is the room for the eight dungeon bosses") {
    const auto bosses = CoopBossScenes::Bosses();
    for (std::uint8_t i = 0; i < 8; ++i) {
        CHECK(bosses[i].room == static_cast<int>(i));
    }
}

TEST_CASE("boss beaten: in the boss's own dungeon, out of its area, given live (rows 136/137)") {
    // Every dungeon boss (child and adult): a player elsewhere in the dungeon when the others win. The room's logic
    // reads the set live (Gohma: Create_WarpPrefab <- Gohma_DefeatBoss), so it is given at once, not at another
    // scene's load: else that player's boss room has the boss deleted by the shared quest and no warp.
    for (const BeatenSet& set : CoopBossBeaten::Sets()) {
        const CoopBoss::Room* room = CoopBoss::RoomByIndex(set.room);
        if (room == nullptr) {
            continue;  // Ganondorf and Ganon: no room, their end sequence's scenes wait (next case)
        }
        CAPTURE(set.boss);
        const std::string dungeon(room->scene);
        CHECK(CoopBossBeaten::InDungeon(set.room, dungeon));
        CHECK_FALSE(CoopBossBeaten::InDungeon(set.room, "HyruleField"));
        Flags f = Alive(set);
        Apply a;
        // The scene loaded before the death: nothing yet.
        CHECK(a.Check(dungeon, true, {}, std::ref(f), kAway).empty());
        // The death is recorded while we stand elsewhere in the dungeon: given now, mid-scene.
        const std::vector<std::uint8_t> beaten{set.room};
        const std::vector<Write> w = a.Check(dungeon, true, beaten, std::ref(f), kAway);
        CHECK(w.size() == set.bools.size() + 1);
        // Once.
        CHECK(a.Check(dungeon, true, beaten, std::ref(f), kAway).empty());
    }
}

TEST_CASE("boss beaten: in the boss's area it waits (D9) and is given once we are out of it") {
    const BeatenSet& s = Gohma();
    const std::string deku(CoopBoss::RoomByIndex(0)->scene);
    Flags f = Alive(s);
    Apply a;
    const std::vector<std::uint8_t> beaten{0};
    bool near = true;
    const std::function<bool(std::uint8_t)> where = [&](std::uint8_t room) { return room == 0 && near; };
    CHECK(a.Check(deku, true, beaten, std::ref(f), where).empty());
    CHECK(a.Check(deku, true, beaten, std::ref(f), where).empty());
    // Our game's own death played (D9): nothing left to give when we walk out.
    Flags played = f;
    for (const auto& b : s.bools) {
        played.v[b.hash] = b.value ? 1 : 0;
    }
    played.v[s.quest] = s.step;
    near = false;
    CHECK(a.Check(deku, true, beaten, std::ref(played), where).empty());
    // Not played (we came in after it, or its event was refused): given out of the area.
    Apply b;
    near = true;
    CHECK(b.Check(deku, true, beaten, std::ref(f), where).empty());
    near = false;
    CHECK(b.Check(deku, true, beaten, std::ref(f), where).size() == s.bools.size() + 1);
}

TEST_CASE("boss beaten: another boss's dungeon or a field still waits for the next load") {
    // Gohma beaten while we stand in Dodongo's Cavern: Kokiri Forest's readers are not loaded, but the rule for
    // scenes that are not the boss's own stays (47b2bd06): the next load.
    const BeatenSet& s = Gohma();
    Flags f = Alive(s);
    Apply a;
    const std::string dodongo(CoopBoss::RoomByIndex(1)->scene);
    REQUIRE(dodongo != std::string(CoopBoss::RoomByIndex(0)->scene));
    const std::vector<std::uint8_t> beaten{0};
    CHECK(a.Check(dodongo, true, {}, std::ref(f), kAway).empty());
    CHECK(a.Check(dodongo, true, beaten, std::ref(f), kAway).empty());
    CHECK(a.Check(std::nullopt, false, beaten, std::ref(f), kAway).empty());
    CHECK(a.Check(dodongo, true, beaten, std::ref(f), kAway).size() == s.bools.size() + 1);
}

TEST_CASE("boss beaten: a record cleared in the dungeon (the host loaded a save) gives nothing live") {
    const BeatenSet& s = Gohma();
    Flags f = Alive(s);
    Apply a;
    const std::string deku(CoopBoss::RoomByIndex(0)->scene);
    CHECK(a.Check(deku, true, {}, std::ref(f), kAway).empty());  // co-op idle or solo: the record is empty
    CHECK(a.Check(deku, true, {}, std::ref(f), kAway).empty());
}

namespace {

// A game that held the boss beaten (its own kill, D9 or the group's set) whose shared quest the host's older save
// just set back to `step`.
Flags BeatenThenReset(const BeatenSet& s, std::uint32_t step) {
    Flags f;
    for (const auto& b : s.bools) {
        f.v[b.hash] = b.value ? 1 : 0;
    }
    f.v[s.quest] = step;
    return f;
}

bool Shared(const BeatenSet& s) {
    return CoopQuests::Find(s.quest) != nullptr;
}

} // namespace

TEST_CASE("boss beaten: the host's older save, a boss alive in it: the Bools its death raised go back (row 138)") {
    int shared = 0;
    for (const BeatenSet& set : CoopBossBeaten::Sets()) {
        CAPTURE(set.boss);
        const CoopBoss::Room* room = CoopBoss::RoomByIndex(set.room);
        if (!Shared(set) || room == nullptr) {
            continue;
        }
        ++shared;
        const std::string dungeon(room->scene);
        Flags f = BeatenThenReset(set, set.below.back());
        Apply a;
        a.HostReset(true);
        const std::vector<Write> w = a.Check(dungeon, true, {}, std::ref(f), kAway);
        for (const auto& b : set.bools) {
            const std::string_view name(b.name);
            const auto it = std::find_if(w.begin(), w.end(), [&](const Write& x) { return x.hash == b.hash; });
            CAPTURE(name);
            if (b.value && name.ends_with("_BossIn")) {
                CHECK(it == w.end());  // the intro: a save from after it has it
            } else if (b.value) {
                REQUIRE(it != w.end());
                CHECK(it->revert);
                CHECK_FALSE(it->is_enum);
                CHECK(it->value == 0);
            }
        }
        CHECK(std::none_of(w.begin(), w.end(), [](const Write& x) { return x.is_enum || !x.revert; }));
        // Once.
        CHECK(a.Check(dungeon, true, {}, std::ref(f), kAway).empty());
    }
    CHECK(shared == 5);  // Gohma, King Dodongo, Barinade, Phantom Ganon, Volvagia
}

TEST_CASE("boss beaten: after the host's reset Gohma's warp goes, her intro stays") {
    const BeatenSet& s = Gohma();
    Flags f = BeatenThenReset(s, s.below.back());
    Apply a;
    a.HostReset(true);
    const std::vector<Write> w = a.Check(std::string("KokiriForest"), true, {}, std::ref(f), kAway);
    std::vector<std::string_view> names;
    for (const Write& x : w) {
        names.emplace_back(x.name);
    }
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string_view>{"Gohma_DefeatBoss", "HavePlayedEvent.Dm_Deku_BossOut",
                                                 "HavePlayedEvent.InsideTheDekuTree_BossDie"});
}

TEST_CASE("boss beaten: a reset's put-back waits in the boss's area, then is given out of it") {
    const BeatenSet& s = Gohma();
    const std::string deku(CoopBoss::RoomByIndex(0)->scene);
    Flags f = BeatenThenReset(s, s.below.back());
    Apply a;
    a.HostReset(true);
    bool near = true;
    const std::function<bool(std::uint8_t)> where = [&](std::uint8_t room) { return room == 0 && near; };
    CHECK(a.Check(deku, true, {}, std::ref(f), where).empty());
    CHECK(a.Check(std::nullopt, false, {}, std::ref(f), where).empty());  // a load keeps it waiting
    CHECK(a.Check(deku, false, {}, std::ref(f), where).empty());          // Link's place not read yet
    CHECK(a.Check(deku, true, {}, std::ref(f), where).empty());
    near = false;
    CHECK(a.Check(deku, true, {}, std::ref(f), where).size() == 3);
}

TEST_CASE("boss beaten: SageOfForest_IsBefore_Step08 follows the quest the host's save set") {
    const BeatenSet* s = CoopBossBeaten::ForRoom(3);
    REQUIRE(s != nullptr);
    REQUIRE(HasBool(*s, "SageOfForest_IsBefore_Step08", false));
    const std::uint32_t is_before = coop::protocol::flag_hash("SageOfForest_IsBefore_Step08");
    const std::string forest(CoopBoss::RoomByIndex(3)->scene);
    {  // the host's save at Step07: before Step08, so true
        Flags f = BeatenThenReset(*s, coop::protocol::flag_hash("Step07"));
        Apply a;
        a.HostReset(true);
        const std::vector<Write> w = a.Check(forest, true, {}, std::ref(f), kAway);
        const auto it = std::find_if(w.begin(), w.end(), [&](const Write& x) { return x.hash == is_before; });
        REQUIRE(it != w.end());
        CHECK(it->value == 1);
    }
    {  // at Step08 (the fight): not before it, stays false
        Flags f = BeatenThenReset(*s, coop::protocol::flag_hash("Step08"));
        Apply a;
        a.HostReset(true);
        const std::vector<Write> w = a.Check(forest, true, {}, std::ref(f), kAway);
        CHECK(std::none_of(w.begin(), w.end(), [&](const Write& x) { return x.hash == is_before; }));
        CHECK_FALSE(w.empty());  // the death's own Bools still go back
    }
}

TEST_CASE("boss beaten: a reset leaves a boss beaten in the host's save, a boss beaten again, unshared quests") {
    const BeatenSet& s = Gohma();
    const std::string deku(CoopBoss::RoomByIndex(0)->scene);
    {  // the host's save beat her too: the quest past DefeatBoss
        Flags f = BeatenThenReset(s, s.step);
        Apply a;
        a.HostReset(true);
        CHECK(a.Check(deku, true, {}, std::ref(f), kAway).empty());
    }
    {  // beaten again after the reset (in the record): nothing put back; the group's set (G2) raises the quest the
       // reset set back, its Bools already beaten
        Flags f = BeatenThenReset(s, s.below.back());
        Apply a;
        a.HostReset(true);
        const std::vector<std::uint8_t> beaten{0};
        const std::vector<Write> w = a.Check(deku, true, beaten, std::ref(f), kAway);
        REQUIRE(w.size() == 1);
        CHECK(w[0].is_enum);
        CHECK(w[0].hash == s.quest);
        CHECK(w[0].value == s.step);
        CHECK_FALSE(w[0].revert);
    }
    {  // LICHII_COOP_QUESTS=0: no quest follows the host
        Flags f = BeatenThenReset(s, s.below.back());
        Apply a;
        a.HostReset(false);
        CHECK(a.Check(deku, true, {}, std::ref(f), kAway).empty());
    }
    // Each game's own quests (SageOfWater, SageOfShadow, SageOfSpirit, Defeat_Ganon): the host does not reset them.
    for (const BeatenSet& set : CoopBossBeaten::Sets()) {
        if (Shared(set)) {
            continue;
        }
        CAPTURE(set.boss);
        Flags f = BeatenThenReset(set, set.below.back());
        Apply a;
        a.HostReset(true);
        CHECK(a.Check(std::string("HyruleField"), true, {}, std::ref(f), kAway).empty());
    }
}

TEST_CASE("boss beaten: solo, co-op idle and the host's own load write nothing") {
    const BeatenSet& s = Gohma();
    const std::string deku(CoopBoss::RoomByIndex(0)->scene);
    {  // no host reset ever (solo / co-op idle): a beaten save stays beaten
        Flags f = BeatenThenReset(s, s.below.back());
        Apply a;
        CHECK(a.Check(deku, true, {}, std::ref(f), kAway).empty());
    }
    {  // the host's own save load: its game already holds the alive values
        Flags f = Alive(s);
        Apply a;
        a.HostReset(true);
        CHECK(a.Check(deku, true, {}, std::ref(f), kAway).empty());
    }
    {  // Reset (out of the session) drops a waiting put-back
        Flags f = BeatenThenReset(s, s.below.back());
        Apply a;
        a.HostReset(true);
        a.Reset();
        CHECK(a.Check(deku, true, {}, std::ref(f), kAway).empty());
    }
}

TEST_CASE("boss beaten: in the boss's area, given once no death of ours can run there (row 136)") {
    // A game that came into the area after the death (walked in during the others' cutscene, fell into the room
    // later, joined there, Game Over / a load into the room) or whose ask was refused (the death flow alone): D9
    // runs nothing more, so the room's logic gets the flags its warp and container read, as the death setting
    // writes them in that room.
    for (std::uint8_t room = 0; room < 8; ++room) {
        CAPTURE(room);
        const BeatenSet* s = CoopBossBeaten::ForRoom(room);
        REQUIRE(s != nullptr);
        const std::string scene(CoopBoss::RoomByIndex(room)->scene);
        Flags f = Alive(*s);
        const std::vector<std::uint8_t> beaten{room};
        bool runs = true;
        const std::function<bool(std::uint8_t)> death = [&](std::uint8_t r) { return r == room && runs; };
        Apply a;
        CHECK(a.Check(scene, true, beaten, std::ref(f), kNear, death).empty());  // our death may run: D9
        runs = false;
        const std::vector<Write> w = a.Check(scene, true, beaten, std::ref(f), kNear, death);
        CHECK(w.size() == s->bools.size() + 1);
        CHECK(std::all_of(w.begin(), w.end(), [&](const Write& x) { return x.room == room && !x.revert; }));
        CHECK(a.Check(scene, true, beaten, std::ref(f), kNear, death).empty());  // once per scene load
        // Our own death ran there (its setting wrote the set): nothing left to give.
        Flags played = f;
        for (const auto& b : s->bools) {
            played.v[b.hash] = b.value ? 1 : 0;
        }
        played.v[s->quest] = s->step;
        Apply c;
        CHECK(c.Check(scene, true, beaten, std::ref(played), kNear, death).empty());
    }
}

TEST_CASE("boss beaten: with no death_here the area still waits, a reset's put-back too") {
    const BeatenSet& s = Gohma();
    const std::string deku(CoopBoss::RoomByIndex(0)->scene);
    Flags f = Alive(s);
    Apply a;
    CHECK(a.Check(deku, true, std::vector<std::uint8_t>{0}, std::ref(f), kNear).empty());
    Flags beatenFlags = BeatenThenReset(s, s.below.back());
    Apply r;
    r.HostReset(true);
    CHECK(r.Check(deku, true, {}, std::ref(beatenFlags), kNear).empty());
    CHECK_FALSE(r.Check(deku, true, {}, std::ref(beatenFlags), kAway).empty());
}

TEST_CASE("boss beaten: the host reloads before the warp: a guest in the boss room loses the warp too (row 159)") {
    // Row 159 (0.11.6, Gohma): the host beat her with the guests, then loaded its save from before the fight. Its
    // FLAG_RESET put the shared quest back (her room's logic showed her again, live) while the guests' own
    // Gohma_DefeatBoss stayed (the put-back waited for a walk out of the room): the warp AND Gohma, the host with
    // neither warp nor kill. Now, in the area once no death of ours can run, the Bools go back there too: every game
    // fights her again, as the host's save has it. Every boss whose quest the host's save sets.
    int shared = 0;
    for (const BeatenSet& set : CoopBossBeaten::Sets()) {
        CAPTURE(set.boss);
        const CoopBoss::Room* room = CoopBoss::RoomByIndex(set.room);
        if (!Shared(set) || room == nullptr) {
            continue;
        }
        ++shared;
        const std::string dungeon(room->scene);
        Flags f = BeatenThenReset(set, set.below.back());
        bool runs = true;
        const std::function<bool(std::uint8_t)> death = [&](std::uint8_t r) { return r == set.room && runs; };
        Apply a;
        a.HostReset(true);
        CHECK(a.Check(dungeon, true, {}, std::ref(f), kNear, death).empty());  // our own death event still plays
        runs = false;
        const std::vector<Write> w = a.Check(dungeon, true, {}, std::ref(f), kNear, death);
        REQUIRE_FALSE(w.empty());
        CHECK(std::all_of(w.begin(), w.end(), [](const Write& x) { return x.revert && !x.is_enum; }));
        for (const auto& b : set.bools) {
            const std::string_view name(b.name);
            CAPTURE(name);
            const bool written =
                std::any_of(w.begin(), w.end(), [&](const Write& x) { return x.hash == b.hash; });
            if (b.value && !name.ends_with("_BossIn")) {
                CHECK(written);  // the warp's flag (Gohma_DefeatBoss) and the death's events: alive again
            } else if (b.value) {
                CHECK_FALSE(written);  // the intro stays
            }
        }
        CHECK(a.Check(dungeon, true, {}, std::ref(f), kNear, death).empty());  // once
    }
    CHECK(shared == 5);
}

TEST_CASE("boss beaten: row 159, a host save that kept the kill or a boss beaten again leaves the guest's room") {
    const BeatenSet& s = Gohma();
    const std::string deku(CoopBoss::RoomByIndex(0)->scene);
    const std::function<bool(std::uint8_t)> none = [](std::uint8_t) { return false; };
    {  // the host's save has her beaten (its quest past DefeatBoss): the warp stays for everyone
        Flags f = BeatenThenReset(s, s.step);
        Apply a;
        a.HostReset(true);
        CHECK(a.Check(deku, true, {}, std::ref(f), kNear, none).empty());
        CHECK(a.Check(deku, true, {}, std::ref(f), kAway, none).empty());
    }
    {  // beaten again since the reset (a proven death in the record): nothing goes back
        Flags f = BeatenThenReset(s, s.step);
        Apply a;
        a.HostReset(true);
        CHECK(a.Check(deku, true, std::vector<std::uint8_t>{0}, std::ref(f), kNear, none).empty());
    }
    {  // unshared quests (LICHII_COOP_QUESTS=0): each game's own boss, nothing goes back
        Flags f = BeatenThenReset(s, s.below.back());
        Apply a;
        a.HostReset(false);
        CHECK(a.Check(deku, true, {}, std::ref(f), kNear, none).empty());
    }
}

TEST_CASE("boss beaten: solo and co-op idle stay unchanged in the boss's room (row 136)") {
    const BeatenSet& s = Gohma();
    const std::string deku(CoopBoss::RoomByIndex(0)->scene);
    const std::function<bool(std::uint8_t)> none = [](std::uint8_t) { return false; };
    Flags f = Alive(s);
    Apply a;
    CHECK(a.Check(deku, true, {}, std::ref(f), kNear, none).empty());  // empty record: nothing
    CHECK(a.Check(deku, true, {}, std::ref(f), kAway, none).empty());
}

namespace {

std::uint16_t Bit(std::uint8_t room) {
    return static_cast<std::uint16_t>(1u << room);
}

bool OwnQuest(const BeatenSet& s) {
    return CoopQuests::Find(s.quest) == nullptr;
}

const Write* EnumWrite(const std::vector<Write>& w, std::uint32_t quest) {
    const auto it = std::find_if(w.begin(), w.end(), [&](const Write& x) { return x.is_enum && x.hash == quest; });
    return it == w.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("boss beaten: BOSS_ALIVE, a boss whose quest is each game's own: quest and Bools back (row 159)") {
    // B #38: Morpha, Bongo Bongo and Twinrova (SageOfWater, SageOfShadow, SageOfSpirit: not in the host's
    // FLAG_RESET) stayed beaten in a guest's game while the host fought them again, and their rooms make the boss,
    // the warp and the container from that quest (romfs Logic: WaterTemple_Level_Room_23, ShadowTemple_Level_Room_
    // B3FB4F_Q, SpiritTemple_Level_Room_34), the container's taken key back to 0 with the host's save: a second
    // container. Now the quest goes back to the fight's step with its Bools: no warp, no container, the boss again.
    int own = 0;
    for (std::uint8_t room = 0; room < 8; ++room) {
        const BeatenSet* set = CoopBossBeaten::ForRoom(room);
        REQUIRE(set != nullptr);
        CAPTURE(set->boss);
        if (!OwnQuest(*set)) {
            continue;
        }
        ++own;
        const std::string dungeon(CoopBoss::RoomByIndex(room)->scene);
        Flags f = BeatenThenReset(*set, set->step);
        bool runs = true;
        const std::function<bool(std::uint8_t)> death = [&](std::uint8_t r) { return r == room && runs; };
        Apply a;
        a.HostAlive(Bit(room), Bit(room), true);
        CHECK(a.Check(dungeon, true, {}, std::ref(f), kNear, death).empty());  // our own death may still run
        runs = false;
        const std::vector<Write> w = a.Check(dungeon, true, {}, std::ref(f), kNear, death);
        const Write* quest = EnumWrite(w, set->quest);
        REQUIRE(quest != nullptr);
        CHECK(quest->revert);
        CHECK(quest->value == set->below.back());  // the fight's own step: the boss there again
        for (const auto& b : set->bools) {
            const std::string_view name(b.name);
            CAPTURE(name);
            const bool written = std::any_of(w.begin(), w.end(), [&](const Write& x) {
                return !x.is_enum && x.hash == b.hash && x.revert && x.value == 0;
            });
            CHECK(written == (b.value && !name.ends_with("_BossIn")));
        }
        CHECK(a.Check(dungeon, true, {}, std::ref(f), kNear, death).empty());  // once
    }
    CHECK(own == 3);  // Morpha, Bongo Bongo, Twinrova
}

TEST_CASE("boss beaten: BOSS_ALIVE leaves a boss alive in ours, a quest gone on past it, a boss beaten again") {
    const BeatenSet* morpha = CoopBossBeaten::ForRoom(5);
    REQUIRE(morpha != nullptr);
    REQUIRE(OwnQuest(*morpha));
    const std::string water(CoopBoss::RoomByIndex(5)->scene);
    {  // alive in our save too: nothing
        Flags f = Alive(*morpha);
        Apply a;
        a.HostAlive(Bit(5), Bit(5), true);
        CHECK(a.Check(water, true, {}, std::ref(f), kAway).empty());
    }
    {  // our quest went on past the boss (the warp, the sage): ours stays
        Flags f = BeatenThenReset(*morpha, 0x12345678u);
        Apply a;
        a.HostAlive(Bit(5), Bit(5), true);
        CHECK(a.Check(water, true, {}, std::ref(f), kAway).empty());
    }
    {  // beaten again since (a proven death in the record): nothing goes back
        Flags f = BeatenThenReset(*morpha, morpha->step);
        Apply a;
        a.HostAlive(Bit(5), Bit(5), true);
        CHECK(a.Check(water, true, std::vector<std::uint8_t>{5}, std::ref(f), kAway).empty());
    }
    {  // the host's save has it beaten (known, not alive), or never read: nothing
        Flags f = BeatenThenReset(*morpha, morpha->step);
        Apply a;
        a.HostAlive(Bit(5), 0, true);
        CHECK(a.Check(water, true, {}, std::ref(f), kAway).empty());
        a.HostAlive(0, 0, true);
        CHECK(a.Check(water, true, {}, std::ref(f), kAway).empty());
    }
    {  // a shared quest's boss (Gohma) is HostReset's, never BOSS_ALIVE's while the quests are shared
        const BeatenSet& g = Gohma();
        Flags f = BeatenThenReset(g, g.step);
        Apply a;
        a.HostAlive(Bit(0), Bit(0), true);
        CHECK(a.Check(std::string("KokiriForest"), true, {}, std::ref(f), kAway).empty());
        // LICHII_COOP_QUESTS=0: every quest is each game's own, so BOSS_ALIVE rewinds it too
        Apply b;
        b.HostAlive(Bit(0), Bit(0), false);
        const std::vector<Write> w = b.Check(std::string("KokiriForest"), true, {}, std::ref(f), kAway);
        const Write* quest = EnumWrite(w, g.quest);
        REQUIRE(quest != nullptr);
        CHECK(quest->value == g.below.back());
    }
    {  // Reset (out of the session) drops it
        Flags f = BeatenThenReset(*morpha, morpha->step);
        Apply a;
        a.HostAlive(Bit(5), Bit(5), true);
        a.Reset();
        CHECK(a.Check(water, true, {}, std::ref(f), kAway).empty());
    }
}

TEST_CASE("boss beaten: BOSS_ALIVE for Ganondorf and Ganon waits out the end sequence, one quest write, the earlier") {
    const BeatenSet* dorf = CoopBossBeaten::ForRoom(8);
    const BeatenSet* ganon = CoopBossBeaten::ForRoom(9);
    REQUIRE(dorf != nullptr);
    REQUIRE(ganon != nullptr);
    REQUIRE(dorf->quest == ganon->quest);
    Flags f;
    for (const auto& b : dorf->bools) {
        f.v[b.hash] = 1;
    }
    for (const auto& b : ganon->bools) {
        f.v[b.hash] = 1;
    }
    f.v[ganon->quest] = ganon->step;  // Complete: both beaten in ours
    Apply a;
    a.HostAlive(Bit(8) | Bit(9), Bit(8) | Bit(9), true);  // both alive in the host's save
    CHECK(a.Check(std::string("CrumblingGanonCastle"), true, {}, std::ref(f), kAway).empty());
    CHECK(a.Check(std::string("FinalBattleField"), true, {}, std::ref(f), kAway).empty());
    const std::vector<Write> w = a.Check(std::string("HyruleField"), true, {}, std::ref(f), kAway);
    CHECK(std::count_if(w.begin(), w.end(), [](const Write& x) { return x.is_enum; }) == 1);
    const Write* quest = EnumWrite(w, dorf->quest);
    REQUIRE(quest != nullptr);
    CHECK(quest->value == dorf->below.back());  // before Ganondorf's death, not only Ganon's
    // Only Ganon alive in the host's save (it beat Ganondorf): back to before Ganon's death.
    Apply b;
    b.HostAlive(Bit(8) | Bit(9), Bit(9), true);
    const std::vector<Write> w2 = b.Check(std::string("HyruleField"), true, {}, std::ref(f), kAway);
    const Write* q2 = EnumWrite(w2, ganon->quest);
    REQUIRE(q2 != nullptr);
    CHECK(q2->value == ganon->below.back());
    // Ganondorf alive in the host's, ours mid Ganon (a step after Ganondorf's death, before Complete): back too.
    Flags mid = f;
    mid.v[ganon->quest] = ganon->below.back();
    Apply c;
    c.HostAlive(Bit(8) | Bit(9), Bit(8) | Bit(9), true);
    const std::vector<Write> w3 = c.Check(std::string("HyruleField"), true, {}, std::ref(mid), kAway);
    const Write* q3 = EnumWrite(w3, dorf->quest);
    REQUIRE(q3 != nullptr);
    CHECK(q3->value == dorf->below.back());
}

TEST_CASE("boss beaten: the host's AliveInSave reads each boss's quest (row 159)") {
    Flags f;
    for (const BeatenSet& s : CoopBossBeaten::Sets()) {
        f.v[s.quest] = s.step;  // all beaten
    }
    const BeatenSet* morpha = CoopBossBeaten::ForRoom(5);
    f.v[morpha->quest] = morpha->below.back();  // Morpha alive
    const CoopBossBeaten::AliveBits bits = CoopBossBeaten::AliveInSave(std::ref(f));
    CHECK(bits.known == coop::protocol::kBossAliveMask);
    CHECK(bits.alive == Bit(5));
    // Ganondorf and Ganon share Defeat_Ganon: Complete beats both; Step05 holds both alive.
    const BeatenSet* dorf = CoopBossBeaten::ForRoom(8);
    f.v[dorf->quest] = dorf->below.back();
    CHECK(CoopBossBeaten::AliveInSave(std::ref(f)).alive == (Bit(5) | Bit(8) | Bit(9)));
    // Unread: not known, never alive.
    Flags none;
    const CoopBossBeaten::AliveBits empty = CoopBossBeaten::AliveInSave(std::ref(none));
    CHECK(empty.known == 0);
    CHECK(empty.alive == 0);
}

TEST_CASE("boss beaten: the record drops the bosses BOSS_ALIVE says are alive") {
    CoopBossBeaten::Record r;
    CHECK(r.Add(5));
    CHECK(r.Add(0));
    r.Remove(Bit(5));
    CHECK(r.Rooms() == std::vector<std::uint8_t>{0});
    r.Remove(0);
    CHECK(r.Rooms() == std::vector<std::uint8_t>{0});
    // What died this session outlives the host's reset (Clear), not the session (ForgetSession).
    CHECK(r.SeenMask() == (Bit(0) | Bit(5)));
    r.Clear();
    CHECK(r.Rooms().empty());
    CHECK(r.SeenMask() == (Bit(0) | Bit(5)));
    CHECK(r.Add(5));  // beaten again after the reset
    r.ForgetSession();
    CHECK(r.Rooms().empty());
    CHECK(r.SeenMask() == 0);
}

TEST_CASE("boss beaten: the host alive box keeps the newest, once") {
    CoopBossBeaten::HostAliveBox box;
    CHECK_FALSE(box.Take());
    box.Put(coop::protocol::BossAlive{1, 0x0020, 0x0020});
    box.Put(coop::protocol::BossAlive{1, 0x0060, 0x0040});
    const auto m = box.Take();
    REQUIRE(m);
    CHECK(m->alive == 0x0040);
    CHECK_FALSE(box.Take());
    box.Put(coop::protocol::BossAlive{1, 1, 1});
    box.Clear();
    CHECK_FALSE(box.Take());
}

namespace {

// The game's flags as the hook sees them: GameFlagStore reads Bools (and Ints, shared UInts) but no Enum; the game's
// Enum getter reads the quests. Writes counted per kind.
struct GameFlagsSeen {
    Flags bools;
    Flags enums;
    int bool_writes = 0;
    int enum_writes = 0;
    CoopBossBeaten::GameIo Io() {
        return CoopBossBeaten::GameIo{
            [this](std::uint32_t h) { return bools(h); },
            [this](std::uint32_t h) { return enums(h); },
            [this](std::uint32_t h, bool v) {
                ++bool_writes;
                bools.v[h] = v ? 1 : 0;
                return true;
            },
            [this](std::uint32_t h, std::uint32_t v) {
                ++enum_writes;
                enums.v[h] = v;
                return true;
            }};
    }
    // What the hook read before (the flag store alone): no quest.
    std::function<std::optional<std::uint32_t>(std::uint32_t)> StoreOnly() {
        return [this](std::uint32_t h) { return bools(h); };
    }
};

GameFlagsSeen Seen(const Flags& f, const BeatenSet& s) {
    GameFlagsSeen g;
    for (const auto& [h, v] : f.v) {
        (h == s.quest ? g.enums : g.bools).v[h] = v;
    }
    return g;
}

const char* Elsewhere(const BeatenSet& s) {
    return s.room == 8 || s.room == 9 ? "KakarikoVillage" : "Graveyard_Child";
}

} // namespace

TEST_CASE("boss beaten: the host's own save load clears the record when the save is in, a guest's and Try Again not") {
    CHECK(CoopBossBeaten::OwnLoadClears(true, true, false));
    CHECK_FALSE(CoopBossBeaten::OwnLoadClears(true, true, true));    // Game Over's Try Again: the group's values back
    CHECK_FALSE(CoopBossBeaten::OwnLoadClears(false, true, false));  // a guest's load: the group's set reaches it
    CHECK_FALSE(CoopBossBeaten::OwnLoadClears(true, false, false));  // no save load
}

TEST_CASE("boss beaten: row 159 run 022754, the idle host loads its save after a guest beat the boss: nothing given") {
    // The host (not in the fight) loaded its save after B killed Gohma. The save was in at once, the FLAG_RESET only
    // when the load window ended; in between the new scene (Graveyard_Child) was read and Apply gave the host
    // Gohma's beaten set. Now the record is cleared when the save is in: every boss, any scene of the new save.
    for (const BeatenSet& set : CoopBossBeaten::Sets()) {
        CAPTURE(set.boss);
        CoopBossBeaten::Record r;
        r.Add(set.room);
        Flags f = Alive(set);
        Apply a;
        CHECK(a.Check(std::nullopt, false, r.Rooms(), std::ref(f), kAway).empty());  // loading
        if (CoopBossBeaten::OwnLoadClears(true, true, false)) {
            r.Clear();  // the save is in
        }
        CHECK(a.Check(std::string(Elsewhere(set)), true, r.Rooms(), std::ref(f), kAway).empty());
        if (const CoopBoss::Room* room = CoopBoss::RoomByIndex(set.room)) {
            CHECK(a.Check(std::nullopt, false, r.Rooms(), std::ref(f), kAway).empty());
            CHECK(a.Check(std::string(room->scene), true, r.Rooms(), std::ref(f), kAway).empty());
        }
        // A guest's own load keeps the record: its new scene gets the set (the group beat it).
        CoopBossBeaten::Record g;
        g.Add(set.room);
        Apply b;
        CHECK_FALSE(b.Check(std::string(Elsewhere(set)), true, g.Rooms(), std::ref(f), kAway).empty());
    }
}

TEST_CASE("boss beaten: the quests are Enums: read and written through the game's Enum getter and setter (row 159)") {
    for (const BeatenSet& set : CoopBossBeaten::Sets()) {
        CAPTURE(set.boss);
        GameFlagsSeen g = Seen(Alive(set), set);
        const CoopBossBeaten::GameIo io = g.Io();
        CHECK(CoopBossBeaten::Read(io, set.quest) == set.below.back());
        CHECK_FALSE(g.StoreOnly()(set.quest));  // the flag store alone: never read
        for (const auto& b : set.bools) {
            CHECK(CoopBossBeaten::Read(io, b.hash) == (b.value ? 0u : 1u));
        }
        // The beaten set given: the quest step through the Enum setter, the Bools through the store.
        Apply a;
        const auto read = [&](std::uint32_t h) { return CoopBossBeaten::Read(io, h); };
        const std::vector<std::uint8_t> beaten{set.room};
        const std::vector<Write> w = a.Check(std::string(Elsewhere(set)), true, beaten, read, kAway);
        REQUIRE(std::any_of(w.begin(), w.end(), [&](const Write& x) { return x.is_enum && x.hash == set.quest; }));
        for (const Write& x : w) {
            CHECK(CoopBossBeaten::WriteTo(io, x));
        }
        CHECK(g.enums(set.quest) == set.step);
        CHECK(g.enum_writes == 1);
        CHECK(g.bool_writes == static_cast<int>(w.size()) - 1);
        CHECK_FALSE(g.bools(set.quest));  // never as a Bool
    }
    // A Bool write of a value past 1 is refused; nothing to write with: false.
    GameFlagsSeen g;
    CHECK_FALSE(CoopBossBeaten::WriteTo(g.Io(), Write{0, 1, false, 2, "x"}));
    CHECK_FALSE(CoopBossBeaten::WriteTo(CoopBossBeaten::GameIo{}, Write{0, 1, true, 2, "x"}));
    CHECK_FALSE(CoopBossBeaten::Read(CoopBossBeaten::GameIo{}, 1));
}

TEST_CASE("boss beaten: with the quests read, the host sends BOSS_ALIVE and the guest's put-back runs (row 159)") {
    // Run 022754: the host never logged 'our save holds bosses' (AliveInSave read no quest through the store) and
    // the guest's HostReset put-back waited for its quest to read, for ever. Through Read both run.
    GameFlagsSeen host;
    for (const BeatenSet& s : CoopBossBeaten::Sets()) {
        host.enums.v[s.quest] = s.below.back();  // all alive in the host's save
    }
    CHECK(CoopBossBeaten::AliveInSave(host.StoreOnly()).known == 0);  // before: nothing to send
    const CoopBossBeaten::GameIo hio = host.Io();
    const CoopBossBeaten::AliveBits bits =
        CoopBossBeaten::AliveInSave([&](std::uint32_t h) { return CoopBossBeaten::Read(hio, h); });
    CHECK(bits.known == coop::protocol::kBossAliveMask);
    CHECK(bits.alive != 0);
    // The guest: Gohma beaten in its game, the host's FLAG_RESET put the shared quest back; in the boss room once no
    // death of ours can run.
    const BeatenSet& set = Gohma();
    const CoopBoss::Room* room = CoopBoss::RoomByIndex(set.room);
    REQUIRE(room != nullptr);
    Flags beaten;
    for (const auto& b : set.bools) {
        beaten.v[b.hash] = b.value ? 1 : 0;
    }
    beaten.v[set.quest] = set.below.back();
    GameFlagsSeen guest = Seen(beaten, set);
    const std::function<bool(std::uint8_t)> no_death = [](std::uint8_t) { return false; };
    Apply before;
    before.HostReset(true);
    for (int i = 0; i < 3; ++i) {
        CHECK(before.Check(std::string(room->scene), true, {}, guest.StoreOnly(), kNear, no_death).empty());
    }
    const CoopBossBeaten::GameIo gio = guest.Io();
    Apply after;
    after.HostReset(true);
    const std::vector<Write> w = after.Check(std::string(room->scene), true, {},
                                             [&](std::uint32_t h) { return CoopBossBeaten::Read(gio, h); }, kNear,
                                             no_death);
    REQUIRE_FALSE(w.empty());
    for (const Write& x : w) {
        CHECK(CoopBossBeaten::WriteTo(gio, x));
    }
    for (const auto& b : set.bools) {
        if (b.value && !std::string_view(b.name).ends_with("_BossIn")) {
            CAPTURE(b.name);
            CHECK(guest.bools(b.hash) == 0u);  // Gohma_DefeatBoss and her death's events: alive again (no warp)
        }
    }
}

TEST_CASE("boss beaten: the host's step reaching our quest a frame after its reset still puts the Bools back (row 159)") {
    // Run 20261120-034533: the guest read its quest in the frame the reset was seen, still at the beaten step (the
    // host's step comes through the flag watcher's store a moment later): the put-back was dropped there, the warp
    // stayed while the shared quest made Gohma again. Now it waits until the quest reads alive. Every shared boss.
    for (const BeatenSet& set : CoopBossBeaten::Sets()) {
        const CoopBoss::Room* room = CoopBoss::RoomByIndex(set.room);
        if (!Shared(set) || room == nullptr) {
            continue;
        }
        CAPTURE(set.boss);
        Flags f = BeatenThenReset(set, set.step);  // the reset seen, our quest not yet moved back
        const std::function<bool(std::uint8_t)> no_death = [](std::uint8_t) { return false; };
        Apply a;
        a.HostReset(true);
        CHECK(a.Check(std::string(room->scene), true, {}, std::ref(f), kNear, no_death).empty());
        const std::vector<std::string> notes = a.TakeNotes();
        CHECK(std::any_of(notes.begin(), notes.end(),
                          [](const std::string& n) { return n.find("its quest reads beaten") != std::string::npos; }));
        CHECK(a.Check(std::string(room->scene), true, {}, std::ref(f), kNear, no_death).empty());
        CHECK(a.TakeNotes().empty());  // once
        f.v[set.quest] = set.below.back();  // the host's step arrives
        const std::vector<Write> w = a.Check(std::string(room->scene), true, {}, std::ref(f), kNear, no_death);
        REQUIRE_FALSE(w.empty());
        CHECK(std::all_of(w.begin(), w.end(), [](const Write& x) { return x.revert && !x.is_enum; }));
        CHECK(a.Check(std::string(room->scene), true, {}, std::ref(f), kNear, no_death).empty());  // once
    }
    // A host save that really has the boss beaten: nothing written, however long.
    const BeatenSet& g = Gohma();
    Flags kept = BeatenThenReset(g, g.step);
    Apply b;
    b.HostReset(true);
    for (int i = 0; i < 5; ++i) {
        CHECK(b.Check(std::string("KokiriForest"), true, {}, std::ref(kept), kAway).empty());
    }
}
