// Tests for the shared story flags (lichii/hook/coop_story.cpp, issue 173).
#include <doctest/doctest.h>

#include <map>
#include <set>
#include <string>

#include "coop_dungeon.h"
#include "coop_story.h"
#include "coop/host/flag_watcher.hpp"
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
    // Caleb's play-test (2026-11-12): Link's house door, the Deku Tree's mouth after
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
            // a talk is one player's, the owl's is story (and the few Caleb asked for)
            CHECK_MESSAGE(name.rfind("HaveTalked_Special.Npc_Kaepora_", 0) == 0, name);
        }
        if (f.kind == CoopStory::Kind::talk) {  // only the Kokiri Shop keeper's (play-test 2026-11-12)
            CHECK_MESSAGE(name.find("Npc_Kokiri_001_Shop_Child") != std::string::npos, name);
        }
    }
    // The opening of a new game: a fresh save still wakes up (leaving the house,
    // 0045 and 0050, is shared since Caleb's play-test of 2026-11-12).
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
