// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the play-test bridge's world snapshot (lichii/hook/coop_world_snapshot.h): the kind table, the owner and
// alive rules, the select-and-cut order, the store and the freshness rule. Names are made-up test strings.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "coop_world_snapshot.h"

using namespace CoopWorldSnapshot;

namespace {
Candidate At(std::uint64_t id, const std::string& name, float x, float y = 0, float z = 0) {
    Candidate c;
    c.id = id;
    c.name = &name;
    c.pos = {x, y, z};
    return c;
}
}  // namespace

TEST_CASE("world snapshot: the kind of an actor comes from its name, and an unknown name is other") {
    CHECK(KindOf("Player_Child") == Kind::player);
    CHECK(KindOf("Player_Adult") == Kind::player);
    CHECK(KindOf("Enemy_Octorok") == Kind::enemy);
    CHECK(KindOf("Npc_Zelda") == Kind::npc);
    CHECK(KindOf("Obj_TBox_Wood") == Kind::chest);
    CHECK(KindOf("Item_Heart") == Kind::item);
    CHECK(KindOf("Obj_WoodDoor") == Kind::door);
    CHECK(KindOf("Obj_FloorSwitch") == Kind::switch_);
    CHECK(KindOf("Obj_Plant_Grass_A") == Kind::other);
    CHECK(KindOf("") == Kind::other);
    CHECK(KindOf("Enemy") == Kind::other);  // no underscore: not an enemy prefix
    for (const Kind k : {Kind::player, Kind::enemy, Kind::npc, Kind::chest, Kind::door, Kind::switch_, Kind::item,
                         Kind::other}) {
        CHECK(KindFromName(KindName(k)) == k);
    }
    CHECK_FALSE(KindFromName("monster"));
}

TEST_CASE("world snapshot: the owner is server, a number or null, never 0 meaning none") {
    CHECK(OwnerJson(std::nullopt) == "null");
    CHECK(OwnerJson(0) == "\"server\"");
    CHECK(OwnerJson(3) == "3");
}

TEST_CASE("world snapshot: alive is known only when life is") {
    CHECK_FALSE(AliveFrom(std::nullopt));
    CHECK(AliveFrom(4) == std::optional<bool>(true));
    CHECK(AliveFrom(0) == std::optional<bool>(false));
}

TEST_CASE("world snapshot: Select puts the known kinds first, nearest first, and cuts at the limit") {
    const std::string grass = "Obj_Plant_Grass_A", foe = "Enemy_Octorok", npc = "Npc_Zelda", chest = "Obj_TBox_Wood";
    const std::vector<Candidate> in = {At(1, grass, 1), At(2, foe, 50), At(3, npc, 10), At(4, chest, 10)};
    const std::array<float, 3> from{0, 0, 0};
    const auto all = Select(in, from, 10);
    REQUIRE(all.size() == 4);
    CHECK(all[0].id == 3);  // the tie at 10 goes to the lower id
    CHECK(all[1].id == 4);
    CHECK(all[2].id == 2);
    CHECK(all[3].id == 1);  // grass is last although it is the nearest
    CHECK(all[3].kind == Kind::other);
    CHECK(all[0].dist == doctest::Approx(10.0f));
    const auto cut = Select(in, from, 2);
    REQUIRE(cut.size() == 2);
    CHECK(cut[0].id == 3);
    CHECK(cut[1].id == 4);
    CHECK(Select({}, from, 5).empty());
    CHECK(Select(in, from, 0).empty());
}

TEST_CASE("world snapshot: with no Link place the distance is 0 and the order is by id") {
    const std::string a = "Enemy_A", b = "Enemy_B";
    const auto out = Select({At(9, a, 100), At(2, b, 1)}, std::nullopt, 5);
    REQUIRE(out.size() == 2);
    CHECK(out[0].id == 2);
    CHECK(out[0].dist == 0.0f);
}

TEST_CASE("world snapshot: Select keeps life, owner and the derived alive") {
    const std::string foe = "Enemy_Octorok";
    Candidate c = At(1, foe, 3, 4, 0);
    c.life = 0;
    c.max_life = 6;
    c.owner = 0;
    const auto out = Select({c}, std::array<float, 3>{0, 0, 0}, 5);
    REQUIRE(out.size() == 1);
    CHECK(out[0].dist == doctest::Approx(5.0f));
    CHECK(out[0].life == std::optional<int>(0));
    CHECK(out[0].max_life == std::optional<int>(6));
    CHECK(out[0].owner == std::optional<int>(0));
    CHECK(out[0].alive == std::optional<bool>(false));
    const std::string grass = "Obj_Plant_Grass_A";
    CHECK_FALSE(Select({At(2, grass, 1)}, std::nullopt, 5)[0].alive);
}

TEST_CASE("world snapshot: the freshness rule") {
    CHECK(Judge(10, 10, 0) == Freshness::current);
    CHECK(Judge(10, 12, 50) == Freshness::behind);
    CHECK(Judge(12, 10, 50) == Freshness::current);  // (a frame ahead of the count is not behind)
    CHECK(Judge(10, 10, kStaleMs) == Freshness::current);
    CHECK(Judge(10, 10, kStaleMs + 1) == Freshness::current);  // a held game does not age its list
    CHECK(Judge(10, 10 + kStaleFrames - 1, kStaleMs + 1) == Freshness::behind);
    CHECK(Judge(10, 10 + kStaleFrames, kStaleMs + 1) == Freshness::stale);
    CHECK(Judge(5, 5 + kStaleFrames, kStaleMs + 1) == Freshness::stale);  // stale wins over behind
    CHECK(Judge(10, 10 + kStaleFrames, kStaleMs) == Freshness::behind);   // frames alone do not make it stale
    CHECK(Judge(10, 10 + kStaleFrames, 0) == Freshness::behind);
}

namespace {
std::uint64_t g_frame = 77;
std::uint64_t FakeFrame() { return g_frame; }
}  // namespace

TEST_CASE("world snapshot: the store is off until enabled, holds one snapshot, and clears") {
    Store store;
    CHECK_FALSE(store.Enabled());
    CHECK(store.Frame() == 0);
    CHECK_FALSE(store.Get());
    store.Enable(&FakeFrame);
    CHECK(store.Enabled());
    CHECK(store.Frame() == 77);
    Snapshot s;
    s.frame = 5;
    s.actors.resize(2);
    store.Put(s);
    REQUIRE(store.Get());
    CHECK(store.Get()->frame == 5);
    CHECK(store.Get()->actors.size() == 2);
    store.Clear();
    CHECK_FALSE(store.Get());
    store.Put(s);
    store.Disable();
    CHECK_FALSE(store.Enabled());
    CHECK_FALSE(store.Get());
}
