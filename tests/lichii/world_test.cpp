// Tests for the shared world objects (lichii/hook/coop_world.cpp) and the
// break that is read just before an object leaves the list
// (coop_objects_sync.cpp, Flush). No emulator code.
#include <doctest/doctest.h>

#include <string>
#include <variant>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_objects_sync.h"
#include "coop_world.h"

namespace proto = coop::protocol;
using CoopObjects::LifeWrite;
using CoopObjects::Seen;
using CoopObjects::Sync;

namespace {

constexpr std::uint64_t kMs = 1000;
const std::string kScene = "KakarikoVillage_Child";

Seen Pot(std::uint64_t address, std::uint64_t hash, std::int32_t life,
         std::string name = "Obj_Pot_A_01") {
    Seen s;
    s.address = address;
    s.name = std::move(name);
    s.hash = hash;
    s.life = proto::ObjectLife{life, 1};
    s.position = {1.0f, 2.0f, 3.0f};
    return s;
}

struct Step {
    std::vector<proto::Message> send;
    std::vector<LifeWrite> writes;
};

Step Tick(Sync& sync, std::uint64_t now, const std::vector<Seen>& seen, std::uint8_t self = 1) {
    Step step;
    sync.Tick(kScene, self, now, seen, step.send, step.writes);
    return step;
}

template <typename T>
std::vector<T> All(const Step& step) {
    std::vector<T> out;
    for (const auto& m : step.send) {
        if (const auto* t = std::get_if<T>(&m)) {
            out.push_back(*t);
        }
    }
    return out;
}

} // namespace

TEST_CASE("world: the breakables are shared, other world objects are not") {
    for (const char* name : {"Obj_Pot_A_01", "Obj_Pot_A_02_Respawn", "Obj_BrokenBox_A_02",
                             "Obj_LiftRock_A_03", "Obj_BrokenRock_A_01", "Obj_Plant_Grass_A_01",
                             "Obj_SignBoard_A_01", "Obj_Kokiri_Signboard_A_01",
                             "Obj_DodongoCave_CrackWall_A_01", "Obj_ShadowTemple_SkullPot_A_01",
                             "Obj_Kokiri_Beehive_A_01", "Obj_GuidePost_A_02",
                             "Obj_InsideGanonCastle_SpiderWeb_A_01", "Obj_Goron_BrokenRock_A_01"}) {
        CAPTURE(name);
        CHECK(CoopWorld::IsBreakable(name));
        CHECK(CoopObjects::IsShared(name));
    }
    for (const char* name : {"Obj_TBox_A_04", "Obj_Kakariko_Door_A_01", "Obj_StepSwitch_A_01",
                             "Obj_Kakariko_CookingPot_A_01", "Obj_GoronCity_LargePot_A_01",
                             "Land_Graveyard_Plants_A_10", "Item_Bomb", "Player_Child",
                             "Obj_Pot"}) {
        CAPTURE(name);
        CHECK_FALSE(CoopWorld::IsBreakable(name));
    }
    CHECK(CoopObjects::IsShared("Enemy_Stalchild"));
}

TEST_CASE("world: a pot keeps its placement id") {
    Sync sync;
    Tick(sync, 0, {Pot(0x100, 0x8796851d6bbe37c, 1)});
    const auto id = sync.IdOf(0x100);
    REQUIRE(id);
    CHECK(id->kind == proto::ObjectKind::placed);
    CHECK(id->value == 0x8796851d6bbe37c);
}

TEST_CASE("world: the owner's pot broken, then gone two walks later, still sends life 0") {
    Sync sync;
    Tick(sync, 0, {Pot(0x100, 0x11, 1)});
    const auto id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 0, 0});
    auto first = All<proto::ObjectSample>(Tick(sync, 10 * kMs, {Pot(0x100, 0x11, 1)}));
    REQUIRE(first.size() == 1);
    // Broken: life 0 read, then removed before a sample went (within 50 ms).
    CHECK(All<proto::ObjectSample>(Tick(sync, 20 * kMs, {Pot(0x100, 0x11, 0)})).empty());
    CHECK(All<proto::ObjectSample>(Tick(sync, 25 * kMs, {})).empty());  // missed once
    const auto last = All<proto::ObjectSample>(Tick(sync, 30 * kMs, {}));
    REQUIRE(last.size() == 1);
    CHECK(last[0].id == id);
    REQUIRE(last[0].state.life);
    CHECK(last[0].state.life->life == 0);
    CHECK(sync.Count() == 0);
}

TEST_CASE("world: a pot that leaves with life left (unloaded) sends nothing") {
    Sync sync;
    Tick(sync, 0, {Pot(0x100, 0x11, 1)});
    sync.Receive(proto::ObjectOwner{*sync.IdOf(0x100), 1, 0, 0});
    Tick(sync, 10 * kMs, {Pot(0x100, 0x11, 1)});
    Tick(sync, 15 * kMs, {});
    const auto step = Tick(sync, 20 * kMs, {});
    CHECK(All<proto::ObjectSample>(step).empty());
    CHECK(All<proto::Damage>(step).empty());
}

TEST_CASE("world: a copy broken and gone before 100 ms sends its DAMAGE at once") {
    Sync sync;
    Tick(sync, 0, {Pot(0x100, 0x11, 1)}, 2);
    const auto id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 3, 0});
    Tick(sync, 10 * kMs, {Pot(0x100, 0x11, 1)}, 2);
    CHECK(All<proto::Damage>(Tick(sync, 20 * kMs, {Pot(0x100, 0x11, 0)}, 2)).empty());
    CHECK(All<proto::Damage>(Tick(sync, 25 * kMs, {}, 2)).empty());  // missed once
    const auto damage = All<proto::Damage>(Tick(sync, 30 * kMs, {}, 2));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].id == id);
    CHECK(damage[0].epoch == 3);
    CHECK(damage[0].amount == 1);
}

TEST_CASE("world: the owner's life 0 breaks the copy") {
    Sync sync;
    Tick(sync, 0, {Pot(0x100, 0x11, 1)}, 2);
    const auto id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 0, 0});
    Tick(sync, 10 * kMs, {Pot(0x100, 0x11, 1)}, 2);
    proto::ObjectSample sample;
    sample.id = id;
    sample.epoch = 0;
    sample.state.time.server_time_us = 20 * kMs;
    sample.state.time.sequence = 1;
    sample.state.life = proto::ObjectLife{0, 1};
    sync.Receive(sample);
    const auto step = Tick(sync, 30 * kMs, {Pot(0x100, 0x11, 1)}, 2);
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].address == 0x100);
    CHECK(step.writes[0].life == 0);
}

namespace {

Seen At(Seen s, float x, float z) {
    s.position = {x, 2.0f, z};
    return s;
}

} // namespace

TEST_CASE("world: a rock lifted, thrown and gone near our Link is broken, with no life 0 read") {
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 2.0f, 0.0f});
    Tick(sync, 0, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), 1.0f, 1.0f)});
    const auto id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 0, 0});
    Tick(sync, 10 * kMs, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), 1.0f, 1.0f)});
    // Lifted (it moves with Link), thrown 3 m ahead, then removed.
    Tick(sync, 20 * kMs, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), 0.2f, 0.3f)});
    Tick(sync, 30 * kMs, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), 3.0f, 0.5f)});
    Tick(sync, 35 * kMs, {});  // missed once
    const auto last = All<proto::ObjectSample>(Tick(sync, 40 * kMs, {}));
    REQUIRE(last.size() == 1);
    REQUIRE(last[0].state.life);
    CHECK(last[0].state.life->life == 0);
}

TEST_CASE("world: a copy thrown and gone near our Link sends its whole life as DAMAGE") {
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 2.0f, 0.0f});
    Tick(sync, 0, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), 1.0f, 1.0f)}, 2);
    const auto id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 4, 0});
    Tick(sync, 10 * kMs, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), 3.0f, 0.5f)}, 2);
    Tick(sync, 15 * kMs, {}, 2);  // missed once
    const auto damage = All<proto::Damage>(Tick(sync, 20 * kMs, {}, 2));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].id == id);
    CHECK(damage[0].epoch == 4);
    CHECK(damage[0].amount == 1);
}

TEST_CASE("world: a breakable gone unmoved, far away, or with no Link seen is not broken") {
    for (const int which : {0, 1, 2}) {
        CAPTURE(which);
        Sync sync;
        if (which != 2) {
            sync.SetPlayer(proto::Vec3{which == 1 ? 50.0f : 0.0f, 2.0f, 0.0f});
        }
        Tick(sync, 0, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), 1.0f, 1.0f)});
        sync.Receive(proto::ObjectOwner{*sync.IdOf(0x100), 1, 0, 0});
        const float moved = which == 0 ? 1.2f : 4.0f;  // 0: under 0.5 m, unmoved
        Tick(sync, 10 * kMs, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), moved, 1.0f)});
        Tick(sync, 15 * kMs, {});
        const auto step = Tick(sync, 20 * kMs, {});
        CHECK(All<proto::ObjectSample>(step).empty());
        CHECK(All<proto::Damage>(step).empty());
    }
}
