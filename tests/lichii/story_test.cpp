// Tests for the shared story flags (lichii/hook/coop_story.cpp, issue 173).
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "coop_chests.h"
#include "coop_dungeon.h"
#include "coop_quests.h"
#include "coop_story.h"
#include "coop/host/flag_watcher.hpp"
#include "coop/host/game_items.hpp"
#include "coop/protocol/codec.hpp"

namespace host = coop::host;
namespace proto = coop::protocol;

namespace {

host::FlagKey Key(const char* name) {
    return host::FlagKey(host::flag_hash(name), 0);
}

class Game : public host::FlagStore {
public:
    std::map<host::FlagKey, proto::FlagEntry> values;
    void set(host::FlagKey key, std::uint32_t v) {
        values[key] = {key, proto::FlagKind::boolean, v};
    }
    std::uint32_t get(host::FlagKey key) {
        return values.at(key).value;
    }
    std::optional<proto::FlagEntry> read(host::FlagKey key) override {
        const auto it = values.find(key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    }
    bool write(const proto::FlagEntry& e) override {
        if (!values.contains(e.key)) return false;
        values[e.key] = e;
        return true;
    }
};

// What a player test saw (issue 173).
constexpr const char* kSaria = "HavePlayedEvent.Dm_Kokiri_0150";      // Saria at the forest exit
constexpr const char* kOwl = "HavePlayedEvent.Dm_HrlField_0010";      // the owl, Hyrule Field
constexpr const char* kDekuTree = "HavePlayedEvent.Dm_Kokiri_0140";   // the Deku Tree's last scene
constexpr const char* kWall = "KokiriForest_Npc_Kokiri_007_Child_Blocker_00";  // the forest-exit wall

} // namespace

TEST_CASE("story: every flag's hash is the hash of its name, sorted, once") {
    std::uint32_t last = 0;
    bool first = true;
    for (const auto& f : CoopStory::Flags()) {
        CHECK_MESSAGE(host::flag_hash(f.name) == f.hash, f.name);
        if (!first) CHECK_MESSAGE(last < f.hash, f.name);
        last = f.hash;
        first = false;
        CHECK(CoopStory::Find(f.hash) == &f);
    }
    CHECK(CoopStory::Flags().size() > 200);
    CHECK(CoopStory::Find(host::flag_hash("NotAFlag")) == nullptr);
}

TEST_CASE("story: Saria's goodbye, the owl, the Deku Tree and the forest-exit wall are shared (issue 173)") {
    for (const char* name : {kSaria, kOwl, kDekuTree, kWall, "KokiriForest_Npc_Kokiri_Mido_Child_Blocker",
                             "HavePlayedEvent.KokiriForest_ArriveLostWood",
                             "HaveTalked_Special.Npc_Kaepora_DeathMountainTrail_Talk_Event1",
                             "HavePlayedEvent.Get_StoneOfFire_DodongoCavernOpne", "JabJabOpenedMouth"}) {
        const auto* f = CoopStory::Find(host::flag_hash(name));
        REQUIRE_MESSAGE(f != nullptr, name);
        CHECK(std::string(f->name) == name);
    }
    CHECK(CoopStory::Find(host::flag_hash(kWall))->kind == CoopStory::Kind::world);
    // Alex's play-test (2026-11-12): Link's house door, the Deku Tree's mouth after
    // its talk, the Kokiri Shop keeper's talks.
    for (const char* name : {"HavePlayedEvent.Dm_Kokiri_0045", "HavePlayedEvent.Dm_Kokiri_0050",
                             "KokiriForest_LinkHouseDoor_Open", "KokiriForest_Meet_DekuTree_OpenDekuTreeMouth",
                             "HavePlayedEvent.Dm_Kokiri_0070", "HavePlayedEvent.Dm_Kokiri_0075",
                             "HaveTalked.Npc_Kokiri_001_Shop_Child",
                             "HaveTalked_Npc_Kokiri_001_Shop_Child_PresentDekuShield"}) {
        CHECK_MESSAGE(CoopStory::Find(host::flag_hash(name)) != nullptr, name);
    }
    CHECK(CoopStory::Find(host::flag_hash(kSaria))->kind == CoopStory::Kind::scene);
}

TEST_CASE("story: what must stay each game's own is left out") {
    std::set<std::uint32_t> dungeon;
    for (const auto& f : CoopDungeon::Flags()) dungeon.insert(f.hash);
    for (const auto& f : CoopDungeon::LiveFlags()) dungeon.insert(f.hash);
    for (const auto name : CoopDungeon::NotShared()) dungeon.insert(host::flag_hash(std::string(name)));
    for (const auto& f : CoopStory::Flags()) {
        const std::string name = f.name;
        CHECK_MESSAGE(!dungeon.contains(f.hash), name);  // the dungeons' list owns those
        // Time travel, system scenes, the title and the end, every dungeon's own scenes
        // and its boss, and one player's own (Navi, talks other than the owl's, hints).
        for (const char* out : {"Dm_TimeTemple", "Dm_SY_", "DmF_", "Dm_Title", "Dm_StaffCredit", "Dm_Ending",
                                "Dm_FinalBattle", "Dm_InsideGanonCastle", "Dm_ForestTemple", "Dm_FireTemple",
                                "Dm_WaterTemple", "Dm_ShadowTemple", "Dm_SpiritTemple", "Dm_DodongoCave",
                                "Dm_InsideJabu", "Demo_", "Boss", "Navi", "Hint", "MiniGame", "Sequence_IsChildhood",
                                "SageRoom", "Dm_Awake_"}) {
            CHECK_MESSAGE(name.find(out) == std::string::npos, name);
        }
        // The Chamber of Sages is left out (the first sword pull, the awakenings after a
        // temple's blue warp); "SageOf" names are only the overworld's own gates.
        if (name.find("SageOf") != std::string::npos) {
            static const std::set<std::string> kOverworld = {
                "HavePlayedEvent.SageOfFire_CityIn", "HavePlayedEvent.SageOfFire_CityIn_CrackWall",
                "HavePlayedEvent.SageOfWater_KingZora_IsMelted_Ice", "SageOfWater_Icedelete"};
            CHECK_MESSAGE(kOverworld.contains(name), name);
        }
        if (name.rfind("HaveTalked", 0) == 0 && f.kind != CoopStory::Kind::talk) {
            // a talk is one player's, the owl's is story (and the few Alex asked for)
            CHECK_MESSAGE(name.rfind("HaveTalked_Special.Npc_Kaepora_", 0) == 0, name);
        }
        if (f.kind == CoopStory::Kind::talk) {  // only the Kokiri Shop keeper's (play-test 2026-11-12)
            CHECK_MESSAGE(name.find("Npc_Kokiri_001_Shop_Child") != std::string::npos, name);
        }
    }
    // The opening of a new game: a fresh save still wakes up (leaving the house,
    // 0045 and 0050, is shared since Alex's play-test of 2026-11-12).
    for (const char* opening : {"HavePlayedEvent.Dm_Kokiri_0000", "HavePlayedEvent.Dm_Kokiri_0010",
                                "HavePlayedEvent.Dm_Kokiri_0030"}) {
        CHECK_MESSAGE(CoopStory::Find(host::flag_hash(opening)) == nullptr, opening);
    }
    // The Chamber of Sages: each game plays its own (time travel, the blue warps' scenes).
    for (const char* sage : {"HavePlayedEvent.SageRoom_MeetRauru", "HavePlayedEvent.Dm_SageRoom_0000",
                             "HavePlayedEvent.Dm_Awake_Forest", "HavePlayedEvent.Dm_Awake_Forest_Get",
                             "HavePlayedEvent.SageOfWater_SageRoom"}) {
        CHECK_MESSAGE(CoopStory::Find(host::flag_hash(sage)) == nullptr, sage);
    }
    // A boss beaten stays each game's own (docs/sync-coverage.md, D9).
    CHECK(CoopStory::Find(host::flag_hash("Gohma_DefeatBoss")) == nullptr);
    CHECK(CoopStory::Find(host::flag_hash("HavePlayedEvent.Dm_Deku_BossOut")) == nullptr);
}

TEST_CASE("story: Merge adds to the list, makes one when there is none, or does nothing when off") {
    std::optional<host::SharedFlags> none;
    CHECK(CoopStory::Merge(none, false) == 0);
    CHECK_FALSE(none.has_value());
    CHECK(CoopStory::Merge(none, true) == CoopStory::Flags().size());
    REQUIRE(none.has_value());
    CHECK(none->contains(Key(kWall)));

    std::optional<host::SharedFlags> some(host::SharedFlags({Key(kSaria), Key("SomeOtherFlag")}));
    CHECK(CoopStory::Merge(some, true) == CoopStory::Flags().size() - 1);
    CHECK(some->contains(Key("SomeOtherFlag")));
    CHECK(some->contains(Key(kOwl)));
    // With the dungeons' list too, nothing is there twice.
    CoopDungeon::Merge(some, true);
    const auto size = some->size();
    CHECK(CoopStory::Merge(some, true) == 0);
    CHECK(some->size() == size);
}

TEST_CASE("story: a scene one player saw counts as seen for the other (issue 173)") {
    std::optional<host::SharedFlags> list;
    CoopStory::Merge(list, true);
    const auto saria = Key(kSaria);
    const auto owl = Key(kOwl);
    Game a;
    Game b;
    for (Game* g : {&a, &b}) {
        g->set(saria, 0);
        g->set(owl, 0);
    }
    host::FlagWatcher wa(*list, host::FlagRole::player);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wa.frame(a);
    wb.frame(b);

    // A watches Saria at the forest exit, then the owl.
    a.set(saria, 1);
    a.set(owl, 1);
    const auto out = wa.frame(a);
    REQUIRE(out.size() == 1);
    proto::Message m;
    REQUIRE(proto::decode(out[0].bytes, m) == proto::Error::none);
    const auto* set = std::get_if<proto::FlagSet>(&m);
    REQUIRE(set);
    CHECK(set->flags.size() == 2);

    // The host's answer reaches B: B's game counts both scenes as played.
    wb.receive(proto::Message{proto::FlagState{1, set->flags}});
    wb.frame(b);
    CHECK(b.get(saria) == 1);
    CHECK(b.get(owl) == 1);
}

TEST_CASE("story: a guest with an early save joins a host past Gohma: no wall, no Saria again (issue 173)") {
    std::optional<host::SharedFlags> list;
    CoopStory::Merge(list, true);
    const auto wall = Key(kWall);
    const auto saria = Key(kSaria);
    const auto tree = Key(kDekuTree);
    // The host's save: Gohma beaten, the Deku Tree's scene and Saria's goodbye seen, the
    // Kokiri at the exit has stepped aside (the wall is gone with him).
    Game host_game;
    host_game.set(wall, 0);
    host_game.set(saria, 1);
    host_game.set(tree, 1);
    // The guest's save: before the Deku Tree. The wall's flag starts at 1 (its default).
    Game guest;
    guest.set(wall, 1);
    guest.set(saria, 0);
    guest.set(tree, 0);

    host::FlagWatcher wg(*list, host::FlagRole::player);
    wg.frame(guest);
    // The server sends the joiner the host's table (FLAG_SNAPSHOT).
    proto::FlagSnapshot snap;
    snap.flags = {{wall, proto::FlagKind::boolean, 0}, {saria, proto::FlagKind::boolean, 1},
                  {tree, proto::FlagKind::boolean, 1}};
    wg.receive(proto::Message{snap});
    wg.frame(guest);
    CHECK(guest.get(wall) == 0);
    CHECK(guest.get(saria) == 1);
    CHECK(guest.get(tree) == 1);
    // Nothing goes back from the guest for what the host's table gave it.
    CHECK(wg.frame(guest).empty());
}

TEST_CASE("story: the host's save load decides, also back to a wall (FLAG_RESET)") {
    std::optional<host::SharedFlags> list;
    CoopStory::Merge(list, true);
    const auto wall = Key(kWall);
    const auto saria = Key(kSaria);
    Game guest;
    guest.set(wall, 0);
    guest.set(saria, 1);
    host::FlagWatcher wg(*list, host::FlagRole::player);
    wg.frame(guest);
    // The host loads an older save, before the Deku Tree: the group follows it.
    wg.receive(proto::Message{proto::FlagReset{1, {{wall, proto::FlagKind::boolean, 1},
                                                   {saria, proto::FlagKind::boolean, 0}}}});
    wg.frame(guest);
    CHECK(guest.get(wall) == 1);
    CHECK(guest.get(saria) == 0);
}

TEST_CASE("story: the Meet_DekuTree quest's hashes are its step names and shared story flags (issue 130)") {
    namespace q = CoopStory::quest;
    CHECK(host::flag_hash("NotReady") == q::kNotReady);
    CHECK(host::flag_hash("Ready") == q::kReady);
    CHECK(host::flag_hash("Step01") == q::kStep01);
    CHECK(host::flag_hash("Step02") == q::kStep02);
    CHECK(host::flag_hash("Step03") == q::kStep03);
    CHECK(host::flag_hash("Complete") == q::kComplete);
    CHECK(host::flag_hash("KokiriForest_Npc_Kokiri_Mido_Child_Blocker") == q::kMidoBlocker);
    CHECK(host::flag_hash("HavePlayedEvent.Dm_Kokiri_0075") == q::kMetDekuTree);
    // Both inputs are shared story flags, so every game sees the same story.
    CHECK(CoopStory::Find(q::kMidoBlocker) != nullptr);
    CHECK(CoopStory::Find(q::kMetDekuTree) != nullptr);
}

TEST_CASE("story: a player whose Mido was moved by another player gets the shop's quest step (issue 130)") {
    namespace q = CoopStory::quest;
    using CoopStory::QuestCatchUp;
    // Another player got past Mido: ours never talked to him (Step01) or only
    // heard him out (Step02). Raised to Step03, so the shop sells (>= Step02).
    CHECK(QuestCatchUp(q::kStep01, false, false) == q::kStep03);
    CHECK(QuestCatchUp(q::kStep02, false, false) == q::kStep03);
    CHECK(QuestCatchUp(q::kReady, false, false) == q::kStep03);
    // The Deku Tree scene played (shared): the quest is complete here too.
    CHECK(QuestCatchUp(q::kStep01, false, true) == q::kComplete);
    CHECK(QuestCatchUp(q::kStep03, false, true) == q::kComplete);
    // Mido still blocks everyone: each player talks to him, as in one game.
    CHECK_FALSE(QuestCatchUp(q::kStep01, true, false));
    CHECK_FALSE(QuestCatchUp(q::kStep01, std::nullopt, std::nullopt));
}

TEST_CASE("story: the Meet_DekuTree catch-up never lowers the quest nor runs before the opening (issue 130)") {
    namespace q = CoopStory::quest;
    using CoopStory::QuestCatchUp;
    CHECK_FALSE(QuestCatchUp(q::kStep03, false, false));
    CHECK_FALSE(QuestCatchUp(q::kComplete, false, false));
    CHECK_FALSE(QuestCatchUp(q::kComplete, false, true));
    // A host save load that puts Mido back (blocker 1) leaves our step as it is.
    CHECK_FALSE(QuestCatchUp(q::kStep03, true, false));
    // A new game before Navi woke Link, an unknown value, or no read: left alone.
    CHECK_FALSE(QuestCatchUp(q::kNotReady, false, true));
    CHECK_FALSE(QuestCatchUp(0x12345678u, false, true));
    CHECK_FALSE(QuestCatchUp(std::nullopt, false, true));
}

TEST_CASE("story: Mido's blocker goes back to 0 in a game already past him (issue 84)") {
    namespace q = CoopStory::quest;
    using CoopStory::MidoBlockerMend;
    // After Gohma (quest Complete, the Deku Tree scene played) a shared 1 came
    // back from a save behind the group: Mido blocks the path in every game.
    CHECK(MidoBlockerMend(q::kComplete, true, true));
    CHECK(MidoBlockerMend(q::kComplete, true, false));
    CHECK(MidoBlockerMend(q::kStep03, true, false));
    // The Deku Tree scene played but our quest is unread or not caught up yet.
    CHECK(MidoBlockerMend(std::nullopt, true, true));
    CHECK(MidoBlockerMend(q::kStep01, true, true));
}

TEST_CASE("story: Mido's blocker is left alone before Link gets past him (issue 84)") {
    namespace q = CoopStory::quest;
    using CoopStory::MidoBlockerMend;
    // Mido still waits for a sword and shield, as in one game.
    CHECK_FALSE(MidoBlockerMend(q::kStep02, true, false));
    CHECK_FALSE(MidoBlockerMend(q::kStep01, true, false));
    CHECK_FALSE(MidoBlockerMend(q::kReady, true, false));
    // A host save from before Mido: its quest and scene flags came with it.
    CHECK_FALSE(MidoBlockerMend(q::kNotReady, true, false));
    // Already 0, unread, or nothing known: no write.
    CHECK_FALSE(MidoBlockerMend(q::kComplete, false, true));
    CHECK_FALSE(MidoBlockerMend(q::kComplete, std::nullopt, true));
    CHECK_FALSE(MidoBlockerMend(0x12345678u, true, false));
    CHECK_FALSE(MidoBlockerMend(std::nullopt, true, std::nullopt));
}

TEST_CASE("story: the Kakariko guard's gate and the letter scene are shared (issue 171)") {
    // The guard's flow Npc_Hylia_Soldier_003 and the letter scene Dm_Kakariko_0010
    // set Npc_Hylia_Soldier_003_GateOpen (romfs Event/EventFlow); the scene's
    // own played flag goes with it, so the other game neither keeps the gate
    // shut nor plays the scene again (its letter is already gone: shared item).
    for (const char* name : {"Npc_Hylia_Soldier_003_GateOpen", "HavePlayedEvent.Dm_Kakariko_0010"}) {
        INFO(name);
        const auto* f = CoopStory::Find(host::flag_hash(name));
        REQUIRE(f != nullptr);
        CHECK(std::string(f->name) == name);
    }
    CHECK(host::flag_hash("Npc_Hylia_Soldier_003_GateOpen") == 0x19898d14u);
}

TEST_CASE("story: Zora's River waterfall opens for every player, with its scene (issue 61)") {
    // StoneOfWater_WaterfallSplit sets the Bool, then plays the (shared) scene:
    // sharing the scene alone left the other players' falls shut.
    CHECK(host::flag_hash("Zorariver_TheWaterfallsplits") == 0xf16a3d82u);
    CHECK(host::flag_hash("Zorariver_TheWaterfallsplits_Adult") == 0xa426db4bu);
    for (const char* name : {"Zorariver_TheWaterfallsplits", "Zorariver_TheWaterfallsplits_Adult"}) {
        const auto* f = CoopStory::Find(host::flag_hash(name));
        REQUIRE_MESSAGE(f != nullptr, name);
        CHECK(f->kind == CoopStory::Kind::world);
    }
    CHECK(CoopStory::Find(host::flag_hash("HavePlayedEvent.Dm_ZoraRiver_0010")) != nullptr);
}

TEST_CASE("story: every one-way story flag is a shared world flag named by its hash, listed once (issue 84)") {
    std::set<std::uint32_t> seen;
    for (const auto& f : CoopStory::OneWays()) {
        INFO(f.name);
        CHECK(host::flag_hash(f.name) == f.hash);
        const auto* shared = CoopStory::Find(f.hash);
        REQUIRE(shared != nullptr);
        CHECK(shared->kind == CoopStory::Kind::world);
        CHECK(seen.insert(f.hash).second);
    }
    // The blockers the game only lowers; the scene gates it only raises.
    const auto way = [](const char* name) {
        for (const auto& f : CoopStory::OneWays()) {
            if (std::string(f.name) == name) return std::optional<CoopStory::Way>(f.way);
        }
        return std::optional<CoopStory::Way>();
    };
    CHECK(way("KokiriForest_Npc_Kokiri_Mido_Child_Blocker") == CoopStory::Way::down);
    CHECK(way("KokiriForest_Npc_Kokiri_Mido_Adult_Blocker") == CoopStory::Way::down);
    CHECK(way("KokiriForest_Npc_Kokiri_007_Child_Blocker_00") == CoopStory::Way::down);
    CHECK(way("KokiriForest_Meet_DekuTree_OpenDekuTreeMouth") == CoopStory::Way::up);
    CHECK(way("GraveYard_RoyalFamilyTomb_Open") == CoopStory::Way::up);
    CHECK(way("KakarikoVillage_Npc_Hylia_009_Child_DeleteWaterPlane") == CoopStory::Way::up);
    CHECK(way("Npc_Hylia_Soldier_003_GateOpen") == CoopStory::Way::up);
    // Set both ways by the game (area logic, Malon's flows): not one way.
    CHECK_FALSE(way("KokiriForest_Npc_Kokiri_007_Child_Blocker"));
    CHECK_FALSE(way("KokiriForest_Npc_Kokiri_Blocker"));
    CHECK_FALSE(way("LostWood_Npc_Kokiri_Blocker"));
    CHECK_FALSE(way("IsMalonAppear_Market"));
    CHECK_FALSE(way("IsMalonAppear_HyruleCastleSecondTime"));
}

TEST_CASE("story: the Deku Tree's mouth opens again in a game whose quest is Complete (issue 84)") {
    namespace q = CoopStory::quest;
    using CoopStory::DekuTreeMouthMend;
    CHECK(host::flag_hash("KokiriForest_Meet_DekuTree_OpenDekuTreeMouth") == q::kDekuTreeMouth);
    CHECK(DekuTreeMouthMend(q::kComplete, false));
    // Open already, or the quest not Complete (the scene can still play), or unread.
    CHECK_FALSE(DekuTreeMouthMend(q::kComplete, true));
    CHECK_FALSE(DekuTreeMouthMend(q::kStep03, false));
    CHECK_FALSE(DekuTreeMouthMend(q::kStep01, false));
    CHECK_FALSE(DekuTreeMouthMend(std::nullopt, false));
    CHECK_FALSE(DekuTreeMouthMend(q::kComplete, std::nullopt));
}

TEST_CASE("story: each mend writes the value its quest proves, on the one-way side, and only when due (issue 84)") {
    namespace q = CoopStory::quest;
    for (const auto& m : CoopStory::Mends()) {
        INFO(m.what);
        const CoopStory::OneWay* one = nullptr;
        for (const auto& f : CoopStory::OneWays()) {
            if (f.hash == m.flag) one = &f;
        }
        REQUIRE(one != nullptr);
        // A down-only flag is mended to 0, an up-only one to 1.
        CHECK(m.value == (one->way == CoopStory::Way::up));
        // Never due once it holds the mended value, whatever the quest says.
        CHECK_FALSE(m.due(q::kComplete, m.value, true));
        // Due on the wrong side once the quest is Complete and the scene played.
        CHECK(m.due(q::kComplete, !m.value, true));
        // Before Mido (Step01, the Deku Tree scene not played): never due.
        CHECK_FALSE(m.due(q::kStep01, !m.value, false));
    }
}

namespace {

// Player report 99: cell N of Gerudo Fortress holds Npc_Hylia_01(2+N)_Adult
// (Logic/GerudoFortress_Common_Level_CarpenterSet_0N_*).
std::string Carpenter(int cell) {
    return "Npc_Hylia_01" + std::to_string(2 + cell) + "_Adult";
}

std::vector<std::string> RescueDone(int cell) {
    const std::string n = "0" + std::to_string(cell);
    return {"GerudoFortress_DestroyGerudoThief_" + Carpenter(cell),
            "IsGet_SmallKey_GerudoFortress.SmallKey" + n,
            "GerudoFortress_Saved_" + Carpenter(cell),
            "GerudoFortress_IsAfterHelpCarpenter_" + Carpenter(cell),
            "GerudoFortress_WarpPoint" + n + "_ClearEvent"};
}

} // namespace

TEST_CASE("story: a Gerudo Fortress rescue is shared whole, the guard's own spawn is not (player report 99)") {
    CHECK(host::flag_hash("GerudoFortress_Saved_Npc_Hylia_013_Adult") == 0x0db0556fu);
    CHECK(host::flag_hash("GerudoFortress_WarpPoint04_ClearEvent") == 0xa4b1efb9u);
    for (int cell = 1; cell <= 4; ++cell) {
        for (const std::string& name : RescueDone(cell)) {
            const auto* f = CoopStory::Find(host::flag_hash(name));
            REQUIRE_MESSAGE(f != nullptr, name);
            CHECK_MESSAGE(f->kind == CoopStory::Kind::world, name);
        }
        // What makes the jail guard and closes the cell stays each game's own: a
        // player there before the guard is beaten still gets the fight.
        for (const std::string& name : {"Found_" + Carpenter(cell),
                                        "GerudoFortress_AppearGerudoThief_" + Carpenter(cell),
                                        "GerudoFortress_CloseShutter_" + Carpenter(cell),
                                        "InsideTheRoom_" + Carpenter(cell),
                                        "GerudoFortress_" + Carpenter(cell) + "_IsCallingHelp"}) {
            CHECK_MESSAGE(CoopStory::Find(host::flag_hash(name)) == nullptr, name);
        }
    }
    // The card and its scene were shared already (an item; Dm_Gerudo_0025).
    CHECK(CoopStory::Find(host::flag_hash("HavePlayedEvent.Dm_Gerudo_0025")) != nullptr);
}

TEST_CASE("story: the host frees a carpenter, the guest's game has the whole rescue, not half (player report 99)") {
    std::optional<host::SharedFlags> list;
    CoopStory::Merge(list, true);
    Game host_game;
    Game guest;
    for (Game* g : {&host_game, &guest}) {
        for (int cell = 1; cell <= 4; ++cell) {
            for (const std::string& name : RescueDone(cell)) g->set(Key(name.c_str()), 0);
        }
    }
    host::FlagWatcher wh(*list, host::FlagRole::player);
    host::FlagWatcher wg(*list, host::FlagRole::player);
    wh.frame(host_game);
    wg.frame(guest);

    // The host beats cell 2's guard, takes the key, opens the cell and the carpenter leaves.
    for (const std::string& name : RescueDone(2)) host_game.set(Key(name.c_str()), 1);
    const auto out = wh.frame(host_game);
    REQUIRE(out.size() == 1);
    proto::Message m;
    REQUIRE(proto::decode(out[0].bytes, m) == proto::Error::none);
    const auto* set = std::get_if<proto::FlagSet>(&m);
    REQUIRE(set);
    CHECK(set->flags.size() == RescueDone(2).size());

    wg.receive(proto::Message{proto::FlagState{1, set->flags}});
    wg.frame(guest);
    for (const std::string& name : RescueDone(2)) CHECK_MESSAGE(guest.get(Key(name.c_str())) == 1, name);
    for (const int cell : {1, 3, 4}) {
        for (const std::string& name : RescueDone(cell)) CHECK_MESSAGE(guest.get(Key(name.c_str())) == 0, name);
    }
}

// Issue 244: a Great Fairy who stays in her fountain. Read from the game's data (romfs: Event/EventFlow
// TriForcePoint and Dm_FairyFountain_*, Banc Presence, GameDataList): what lets a fairy out is each game's
// own, and none of it is shared or written by co-op:
//   - the Triforce mark is placed only while FairyFountain_OcarinaEventOff is false (Presence, IsNegation),
//     and its ReplaceEventParam has it as InvalidGameData; ResetTypeValue 9 (a scene's own);
//   - after Zelda's Lullaby, TriForcePoint (event 11, EventQueryFairyFountainSelectSpecialAbility) lets the
//     Din fountain (2) through and asks IsGet.Item_DinFire at every other one (event 3). That gate kept the
//     fountains shut in a seed where the Death Mountain fairy gave another item: the randomizer's own, opened
//     in OOTRR 0.6.0 (8a8a0bc, "Open every Great Fairy fountain ... without Din's Fire"); a solo retest on
//     2026-11-17 opened the Hyrule Castle fountain without Din's Fire. In co-op the spell is a shared item, so
//     the gate opens for everyone once anyone has it: sharing can only open it;
//   - GreatFairy_AlreadyAppeared (set after a visit; ResetTypeValue 9), GreatFairy_MeetFirst (the Din
//     fountain's first meeting), Meet_GreatFairy_RemainNumber and _01.._05, _CompleteMessage,
//     GreatFairy_HasNoMPOnDinDemo and GreatFairyFountain_In stay each game's own.
// Shared on purpose: each fountain's played flag (HavePlayedEvent.Dm_FairyFountain_*) and its reward (an
// item). The five other fountain flows ask exactly those two: not played -> the first offer; played and no
// reward -> the offer again; played and the reward -> a heal. A fountain one player emptied heals the others
// (its reward is theirs already) and is never paid out twice.
TEST_CASE("story: the Great Fairy fountains open by each game's own flags, their scenes and rewards are shared (issue 244)") {
    for (const char* played : {"HavePlayedEvent.Dm_FairyFountain_Courage", "HavePlayedEvent.Dm_FairyFountain_Din",
                               "HavePlayedEvent.Dm_FairyFountain_Frore", "HavePlayedEvent.Dm_FairyFountain_Nayru",
                               "HavePlayedEvent.Dm_FairyFountain_Power", "HavePlayedEvent.Dm_FairyFountain_Wisdom"}) {
        const CoopStory::Flag* f = CoopStory::Find(host::flag_hash(played));
        REQUIRE_MESSAGE(f != nullptr, played);
        CHECK(f->kind == CoopStory::Kind::scene);
    }
    for (const char* reward : {"IsGet.Item_DinFire", "IsGet.Item_FairyBless_MP", "IsGet.Item_FairyBless_MPDouble",
                               "IsGet.Item_FaroreWind", "IsGet.Item_NayruLove", "IsGet.Item_FairyBless_DefenseDouble"}) {
        const host::ItemFlag* item = host::find_item(host::flag_hash(reward));
        REQUIRE_MESSAGE(item != nullptr, reward);
        CHECK_MESSAGE(item->on, reward);
    }
    std::set<std::uint32_t> dungeon;
    for (const auto& f : CoopDungeon::Flags()) dungeon.insert(f.hash);
    for (const auto& f : CoopDungeon::LiveFlags()) dungeon.insert(f.hash);
    for (const char* own : {"FairyFountain_OcarinaEventOff", "GreatFairy_AlreadyAppeared", "GreatFairy_MeetFirst",
                            "GreatFairy_HasNoMPOnDinDemo", "GreatFairyFountain_In", "Meet_GreatFairy_RemainNumber",
                            "Meet_GreatFairy_01", "Meet_GreatFairy_02", "Meet_GreatFairy_03", "Meet_GreatFairy_04",
                            "Meet_GreatFairy_05", "Meet_GreatFairy_CompleteMessage"}) {
        const std::uint32_t h = host::flag_hash(own);
        CHECK_MESSAGE(CoopStory::Find(h) == nullptr, own);
        CHECK_MESSAGE(host::find_item(h) == nullptr, own);
        CHECK_MESSAGE(CoopQuests::Find(h) == nullptr, own);
        CHECK_MESSAGE(!dungeon.contains(h), own);
    }
    // The hashes of all 12 names above, each found in the game's GameDataList (a misspelt name would pass the
    // checks above for nothing).
    CHECK(host::flag_hash("FairyFountain_OcarinaEventOff") == 0x1dada1c7u);
    CHECK(host::flag_hash("GreatFairy_AlreadyAppeared") == 0x73b4cc1au);
    CHECK(host::flag_hash("GreatFairy_MeetFirst") == 0x508b472du);
    CHECK(host::flag_hash("GreatFairy_HasNoMPOnDinDemo") == 0xccb86260u);
    CHECK(host::flag_hash("GreatFairyFountain_In") == 0xaae0bd8bu);
    CHECK(host::flag_hash("Meet_GreatFairy_RemainNumber") == 0x53c5f0eau);
    CHECK(host::flag_hash("Meet_GreatFairy_01") == 0x9cfe9836u);
    CHECK(host::flag_hash("Meet_GreatFairy_02") == 0xf8106300u);
    CHECK(host::flag_hash("Meet_GreatFairy_03") == 0x3ff29ed3u);
    CHECK(host::flag_hash("Meet_GreatFairy_04") == 0x732acbf1u);
    CHECK(host::flag_hash("Meet_GreatFairy_05") == 0xbe878637u);
    CHECK(host::flag_hash("Meet_GreatFairy_CompleteMessage") == 0x2fe03182u);
}

// Row 129 (GitHub #173): what a shared story event sets in the game that plays it, given to a game whose
// played flag came from another game (CoopStory::Effects, tools/puppet_pack/story_effects.py).
namespace {

const CoopStory::Effect* FindEffect(std::string_view setting, std::string_view flag) {
    for (const auto& e : CoopStory::Effects()) {
        if (setting == e.setting && flag == e.name) return &e;
    }
    return nullptr;
}

std::vector<const CoopStory::Effect*> Due(const std::map<std::uint32_t, bool>& bools) {
    std::array<const CoopStory::Effect*, CoopStory::kEffectsMax> out{};
    const auto n = CoopStory::EffectsDue(
        [&](std::uint32_t h) -> std::optional<bool> {
            const auto it = bools.find(h);
            return it == bools.end() ? std::nullopt : std::optional<bool>(it->second);
        },
        out);
    return {out.begin(), out.begin() + n};
}

} // namespace

TEST_CASE("story effects: the owl's HyruleField_ArriveEntrance, which starts the clock, is one (row 129)") {
    const auto* owl = FindEffect("HyruleField_Goto_HyruleCastle_Kaepora", "HyruleField_ArriveEntrance");
    REQUIRE(owl != nullptr);
    CHECK(owl->played == 0x98bf10c1u);  // shared: CoopStory::Find
    CHECK(owl->flag == 0x804aec99u);    // the clock gate's flag (docs/time.md)
    CHECK(owl->value);
    REQUIRE(CoopStory::Find(owl->played) != nullptr);
}

TEST_CASE("story effects: due only once the event played, while the flag is read and not its value") {
    const auto* owl = FindEffect("HyruleField_Goto_HyruleCastle_Kaepora", "HyruleField_ArriveEntrance");
    REQUIRE(owl != nullptr);
    // A late joiner: the owl's played flag came from the host, its own flow never ran.
    auto due = Due({{owl->played, true}, {owl->flag, false}});
    CHECK(std::find(due.begin(), due.end(), owl) != due.end());
    // Its own owl (or a mend before): nothing to do.
    CHECK(Due({{owl->played, true}, {owl->flag, true}}).empty());
    // The owl not seen anywhere yet: the clock stays as the game has it.
    CHECK(Due({{owl->played, false}, {owl->flag, false}}).empty());
    // Anything unread counts as not due.
    CHECK(Due({{owl->flag, false}}).empty());
    CHECK(Due({{owl->played, true}}).empty());
}

TEST_CASE("story effects: every one comes from a shared played flag and is not shared itself") {
    std::optional<host::SharedFlags> shared = host::item_shared_flags();
    CoopDungeon::AddShared(shared);
    CoopStory::AddShared(shared);
    CoopQuests::AddShared(shared);
    CoopChests::AddShared(shared);
    REQUIRE(shared.has_value());
    REQUIRE_FALSE(CoopStory::Effects().empty());
    CHECK(CoopStory::Effects().size() <= CoopStory::kEffectsMax);
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    for (const auto& e : CoopStory::Effects()) {
        INFO(e.setting << ": " << e.name);
        CHECK(e.played == host::flag_hash(std::string("HavePlayedEvent.") + e.setting));
        CHECK(e.flag == host::flag_hash(e.name));
        CHECK(shared->contains(host::FlagKey(e.played, std::uint16_t{0})));
        CHECK_FALSE(shared->contains(host::FlagKey(e.flag, std::uint16_t{0})));
        CHECK(seen.insert({e.played, e.flag}).second);  // no row twice
    }
}

TEST_CASE("story effects: given once per flag per load, a flag set back then is left alone and said once") {
    // Review of owl-time: a flag the game's own code sets back is not fought every 3 s.
    const auto& effects = CoopStory::Effects();
    const auto* owl_row = FindEffect("HyruleField_Goto_HyruleCastle_Kaepora", "HyruleField_ArriveEntrance");
    REQUIRE(owl_row != nullptr);
    REQUIRE(owl_row->played == 0x98bf10c1u);
    const CoopStory::Effect& owl = *owl_row;
    using V = CoopStory::EffectsGiven::Verdict;
    CoopStory::EffectsGiven given;
    // A write that failed is not given: the next pass gives it again, no false "set back".
    CHECK(given.Take(owl) == V::give);
    CHECK(given.Take(owl) == V::give);
    given.Given(owl);
    // Set back in the same load: said once, then quiet however often it is due again.
    CHECK(given.Take(owl) == V::set_back);
    CHECK(given.Take(owl) == V::quiet);
    CHECK(given.Take(owl) == V::quiet);
    // Another flag is its own.
    for (const CoopStory::Effect& e : effects) {
        if (e.flag != owl.flag) {
            CHECK(given.Take(e) == V::give);
            given.Given(e);
            break;
        }
    }
    // A new load (an older save reloaded, the flag back at 0 there): given once more.
    given.NewLoad();
    CHECK(given.Take(owl) == V::give);
    given.Given(owl);
    CHECK(given.Take(owl) == V::set_back);
}

TEST_CASE("story effects: a flag set by several events is due once per pass, with one value (review of owl-time)") {
    // Every flag that more than one row sets: one value, and with all its events played it is due once.
    std::map<std::uint32_t, std::vector<const CoopStory::Effect*>> by_flag;
    for (const auto& e : CoopStory::Effects()) by_flag[e.flag].push_back(&e);
    std::size_t multi = 0;
    for (const auto& [flag, rows] : by_flag) {
        if (rows.size() < 2) continue;
        ++multi;
        INFO(rows.front()->name);
        std::map<std::uint32_t, bool> bools{{flag, !rows.front()->value}};
        for (const auto* r : rows) {
            CHECK(r->value == rows.front()->value);
            bools[r->played] = true;
        }
        const auto due = Due(bools);
        REQUIRE(due.size() == 1);
        CHECK(due.front()->flag == flag);
        // Given once in that pass: the flag is not "set back" by its own second row.
        CoopStory::EffectsGiven given;
        CHECK(given.Take(*due.front()) == CoopStory::EffectsGiven::Verdict::give);
        given.Given(*due.front());
    }
    CHECK(multi >= 2);  // DeathMtCrater_IsFirstEntry, MelodyGet_Time_EndConversationWithZelda
    // The two crater entrances played together: one due row.
    const auto* a = FindEffect("GoTo_DeathMountainCrater", "DeathMtCrater_IsFirstEntry");
    const auto* b = FindEffect("GoTo_DeathMountainCrater_FromGoronCity", "DeathMtCrater_IsFirstEntry");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(Due({{a->played, true}, {b->played, true}, {a->flag, true}}).size() == 1);
    // Only the second entrance played: still due, through its own row.
    const auto only_b = Due({{a->played, false}, {b->played, true}, {a->flag, true}});
    REQUIRE(only_b.size() == 1);
    CHECK(only_b.front() == b);
}

TEST_CASE("story: King Zora's quest floor is the hash of its names (row 145)") {
    const auto& zora = CoopStory::StepFloors()[0];
    CHECK(zora.quest == host::flag_hash("Step_Get_StoneOfWater"));
    CHECK(zora.proof == host::flag_hash("HavePlayedEvent.Dm_Zora_0010"));
    CHECK(zora.floor == host::flag_hash("Step07"));
    // Every step below Step07 but NotReady, in the quest's order (coop_quests.cpp Get_StoneOfWater).
    const std::vector<std::string> back{"Ready", "Step01", "Step02", "Step03", "Step03_Impa",
                                        "Step04", "Step05", "Step05_01", "Step06"};
    REQUIRE(zora.back.size() == back.size());
    for (std::size_t i = 0; i < back.size(); ++i) CHECK(zora.back[i] == host::flag_hash(back[i]));
    // The same steps letters::kKingWaits names, after NotReady: one list for both.
    for (std::size_t i = 0; i < back.size(); ++i) CHECK(zora.back[i] == host::letters::kKingWaits[i + 1]);
    CHECK(zora.proof == host::letters::kKingRead);
    CHECK(zora.floor == host::letters::kKingMoved[0]);
}

TEST_CASE("story: King Zora goes back aside when the game's own flow set his quest back (row 145)") {
    using CoopStory::StepFloorDue;
    const auto& zora = CoopStory::StepFloors()[0];
    const auto step07 = host::flag_hash("Step07");
    // The letter was read here (Dm_Zora_0010), then the diving game (Step05), Ruto's Letter got (Step05_01)
    // or read (Step06), or Goto_ZoraDomain's arrival (Step03, Step03_Impa) moved the quest back.
    for (const char* s : {"Step03", "Step03_Impa", "Step05", "Step05_01", "Step06", "Ready"}) {
        CHECK(StepFloorDue(zora, host::flag_hash(s), true) == step07);
    }
    // At Step07 or past it (Jabu-Jabu, Complete): nothing to do.
    for (const char* s : {"Step07", "Step09", "Step14", "Complete"}) {
        CHECK_FALSE(StepFloorDue(zora, host::flag_hash(s), true).has_value());
    }
    // The letter not read anywhere yet: King Zora still waits for it.
    CHECK_FALSE(StepFloorDue(zora, host::flag_hash("Step06"), false).has_value());
    // Unread, a new save (NotReady) or a value not in the quest: left alone.
    CHECK_FALSE(StepFloorDue(zora, host::flag_hash("Step06"), std::nullopt).has_value());
    CHECK_FALSE(StepFloorDue(zora, std::nullopt, true).has_value());
    CHECK_FALSE(StepFloorDue(zora, host::flag_hash("NotReady"), true).has_value());
    CHECK_FALSE(StepFloorDue(zora, 0x12345678u, true).has_value());
}

TEST_CASE("story: the Water Temple door never sets SageOfWater back below what was played (row 145 review)") {
    using CoopStory::StepFloorDue;
    const auto floors = CoopStory::StepFloors();
    // Highest first, all from Step06 only (WaterTemple_Arrival writes Step06 and nothing else).
    const std::vector<std::pair<const char*, const char*>> rows{
        {"HavePlayedEvent.Demo_SageOfWater_BossDefeat", "Step08"},
        {"HavePlayedEvent.Demo_SageOfWater_BossAppears", "Step07_2"},
        {"WaterTemple_DarkLink_Death", "Step07_1"},
        {"HavePlayedEvent.Demo_WaterTemple_MeetAgainRutoEvent", "Step07"},
    };
    REQUIRE(floors.size() == 1 + rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& f = floors[i + 1];
        CHECK(f.quest == host::flag_hash("Step_SageOfWater"));
        CHECK(f.proof == host::flag_hash(rows[i].first));
        CHECK(f.floor == host::flag_hash(rows[i].second));
        REQUIRE(f.back.size() == 1);
        CHECK(f.back[0] == host::flag_hash("Step06"));
        // Re-entered after that scene: back up; before it, or past Step06, nothing.
        CHECK(StepFloorDue(f, host::flag_hash("Step06"), true) == f.floor);
        CHECK_FALSE(StepFloorDue(f, host::flag_hash("Step06"), false).has_value());
        for (const char* s : {"Step05", "Step07", "Step07_1", "Step08", "Complete", "NotReady"}) {
            CHECK_FALSE(StepFloorDue(f, host::flag_hash(s), true).has_value());
        }
    }
}

TEST_CASE("story: Saria's gift is named by its hashes (rows 195/204)") {
    REQUIRE(CoopStory::Gifts().size() <= CoopStory::kGiftsMax);
    const auto& saria = CoopStory::Gifts()[0];
    const std::vector<std::string> played{"HavePlayedEvent.KokiriForest_ArriveLostWood",
                                          "HavePlayedEvent.Dm_Kokiri_0150", "HavePlayedEvent.Dm_Kokiri_0155"};
    REQUIRE(saria.played.size() == played.size());
    for (std::size_t i = 0; i < played.size(); ++i) {
        CHECK_MESSAGE(saria.played[i] == host::flag_hash(played[i]), played[i]);
        // each is a shared story flag: the reason the gift can be spent without its scene here
        CHECK_MESSAGE(CoopStory::Find(saria.played[i]) != nullptr, played[i]);
    }
    CHECK(saria.count == host::flag_hash("PouchData.Item_Ocarina_Fairy.StockNum"));
    CHECK(saria.count == host::ocarina::kFairy);
    REQUIRE(saria.held.size() == 2);
    CHECK(saria.held[0] == host::ocarina::kFairy);
    CHECK(saria.held[1] == host::ocarina::kTime);
    CHECK(std::string_view(saria.item) == host::ocarina::kFairyName);
}

TEST_CASE("story: Saria's gift spent with no ocarina is due, never with one held or unread (rows 195/204)") {
    const auto& saria = CoopStory::Gifts()[0];
    std::map<std::uint32_t, bool> bools;
    std::map<std::uint32_t, std::int32_t> ints{{host::ocarina::kFairy, 0}, {host::ocarina::kTime, 0}};
    const auto due = [&] {
        return CoopStory::GiftDue(
            saria,
            [&](std::uint32_t h) -> std::optional<bool> {
                const auto it = bools.find(h);
                return it == bools.end() ? std::nullopt : std::optional<bool>(it->second);
            },
            [&](std::uint32_t h) -> std::optional<std::int32_t> {
                const auto it = ints.find(h);
                return it == ints.end() ? std::nullopt : std::optional<std::int32_t>(it->second);
            });
    };
    for (const auto h : saria.played) bools[h] = false;
    CHECK_FALSE(due());  // the bridge scene still to come: Saria gives it herself
    // Each played flag alone spends it: the timeline joined alone (Dm_Kokiri_0150, no gift), the setting or the gift
    // scene shared in from another game whose share of the item never came.
    for (const auto h : saria.played) {
        for (const auto o : saria.played) bools[o] = o == h;
        CHECK(due());
    }
    ints[host::ocarina::kFairy] = 1;
    CHECK_FALSE(due());  // got it (from Saria or shared): nothing to give
    ints[host::ocarina::kFairy] = 0;
    ints[host::ocarina::kTime] = 1;
    CHECK_FALSE(due());  // the Ocarina of Time is better: never a fairy one beside it
    ints.erase(host::ocarina::kTime);
    CHECK_FALSE(due());  // a count not read (a load): left alone
    ints[host::ocarina::kTime] = 0;
    bools.clear();
    CHECK_FALSE(due());  // played flags not read: left alone
}

TEST_CASE("story: Saria's played flags from others wait until an ocarina is ours (rows 195/204, the cause)") {
    const auto& saria = CoopStory::Gifts()[0];
    for (const auto h : saria.played) CHECK(CoopStory::GiftOf(h) == std::optional<std::size_t>(0));
    CHECK_FALSE(CoopStory::GiftOf(host::flag_hash("HavePlayedEvent.Dm_Kokiri_0140")));
    CHECK_FALSE(CoopStory::GiftOf(host::ocarina::kFairy));

    std::array<bool, CoopStory::kGiftsMax> held{};
    for (const auto h : saria.played) CHECK(CoopStory::GiftHolds(h, held));  // no ocarina here: wait
    CHECK_FALSE(CoopStory::GiftHolds(host::flag_hash("HavePlayedEvent.Dm_Kokiri_0140"), held));  // others pass
    held[0] = true;
    for (const auto h : saria.played) CHECK_FALSE(CoopStory::GiftHolds(h, held));  // the shared ocarina came

    std::map<std::uint32_t, std::int32_t> ints;
    const auto read = [&](std::uint32_t h) -> std::optional<std::int32_t> {
        const auto it = ints.find(h);
        return it == ints.end() ? std::nullopt : std::optional<std::int32_t>(it->second);
    };
    CHECK_FALSE(CoopStory::GiftHeld(saria, read).has_value());  // unread (a load): the hold keeps waiting
    ints = {{host::ocarina::kFairy, 0}, {host::ocarina::kTime, 0}};
    CHECK(CoopStory::GiftHeld(saria, read) == std::optional<bool>(false));
    ints[host::ocarina::kFairy] = 1;
    CHECK(CoopStory::GiftHeld(saria, read) == std::optional<bool>(true));
    ints = {{host::ocarina::kTime, 1}};  // the fairy count unread, the Ocarina of Time held: held
    CHECK(CoopStory::GiftHeld(saria, read) == std::optional<bool>(true));
}

TEST_CASE("story: the Ocarina of Time's scene waits for its item, never mended (rows 195/204 sweep)") {
    REQUIRE(CoopStory::Gifts().size() == 2);
    CHECK(CoopStory::Gifts()[0].mend);
    const auto& time = CoopStory::Gifts()[1];
    CHECK_FALSE(time.mend);  // the randomizer's progressive ocarina: hold only
    REQUIRE(time.played.size() == 1);
    CHECK(time.played[0] == host::flag_hash("HavePlayedEvent.HyruleField_DoorOfTime_GetOcarinaOfTime"));
    CHECK(CoopStory::Find(time.played[0]) != nullptr);
    CHECK(time.count == host::ocarina::kTime);
    REQUIRE(time.held.size() == 1);
    CHECK(time.held[0] == host::ocarina::kTime);
    std::array<bool, CoopStory::kGiftsMax> held{};
    held[0] = true;  // the Fairy Ocarina is no Ocarina of Time
    CHECK(CoopStory::GiftHolds(time.played[0], held));
    held[1] = true;
    CHECK_FALSE(CoopStory::GiftHolds(time.played[0], held));
    for (const auto h : CoopStory::Gifts()[0].played) CHECK_FALSE(CoopStory::GiftHolds(h, held));
}
