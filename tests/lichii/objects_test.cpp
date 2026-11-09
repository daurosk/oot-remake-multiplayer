// Tests for the shared enemies' hook side (lichii/hook/coop_objects_sync.cpp).
// No emulator code: each walk is a list of actors.
#include <doctest/doctest.h>

#include <string>
#include <variant>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_objects_sync.h"

namespace proto = coop::protocol;
using CoopObjects::LifeWrite;
using CoopObjects::Seen;
using CoopObjects::Sync;

namespace {

constexpr std::uint64_t kMs = 1000;
const std::string kScene = "Spot_GraveYard";

Seen Enemy(std::uint64_t address, std::uint64_t hash, std::int32_t life, float x = 0.0f,
           std::string name = "Enemy_Stalchild") {
    Seen s;
    s.address = address;
    s.name = std::move(name);
    s.hash = hash;
    s.life = proto::ObjectLife{life, 2};
    s.position = {x, 0.0f, 0.0f};
    return s;
}

struct Step {
    std::vector<proto::Message> send;
    std::vector<LifeWrite> writes;
};

Step Tick(Sync& sync, std::uint64_t now, const std::vector<Seen>& seen, std::uint8_t self = 1,
          const std::string& scene = kScene) {
    Step step;
    sync.Tick(scene, self, now, seen, step.send, step.writes);
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

void Own(Sync& sync, const proto::ObjectId& id, std::uint8_t owner, std::uint16_t epoch = 0) {
    sync.Receive(proto::ObjectOwner{id, owner, epoch, 0});
}

} // namespace

TEST_CASE("objects: only enemies are shared") {
    CHECK(CoopObjects::IsShared("Enemy_Stalchild"));
    CHECK_FALSE(CoopObjects::IsShared("Item_Bomb"));
    CHECK_FALSE(CoopObjects::IsShared("Obj_Pot"));
}

TEST_CASE("objects: a scene's own enemy gets the same id on both games") {
    const auto a = CoopObjects::SceneSpawnId(kScene, "Enemy_Stalchild", 0);
    CHECK(a == CoopObjects::SceneSpawnId(kScene, "Enemy_Stalchild", 0));
    CHECK(a != CoopObjects::SceneSpawnId(kScene, "Enemy_Stalchild", 1));
    CHECK(a != CoopObjects::SceneSpawnId("Spot_Field", "Enemy_Stalchild", 0));
    CHECK(a != CoopObjects::SceneSpawnId(kScene, "Enemy_Poe", 0));
    CHECK((a >> 63) == 1);

    // Two games whose actors sit at other addresses and were walked in
    // another order still give each enemy the same id (by x within a walk).
    Sync one;
    Sync two;
    Tick(one, 0, {Enemy(0x100, ~0ULL, 2, 1.0f), Enemy(0x200, ~0ULL, 2, 5.0f)});
    Tick(two, 0, {Enemy(0x900, 0, 2, 5.0f), Enemy(0x800, ~0ULL, 2, 1.0f)});
    CHECK(*one.IdOf(0x100) == *two.IdOf(0x800));
    CHECK(*one.IdOf(0x200) == *two.IdOf(0x900));
    // A placed enemy keeps its placement hash.
    Tick(one, 10 * kMs, {Enemy(0x100, ~0ULL, 2, 1.0f), Enemy(0x200, ~0ULL, 2, 5.0f),
                         Enemy(0x300, 0x1234, 2)});
    CHECK(one.IdOf(0x300)->value == 0x1234);
    CHECK(one.IdOf(0x300)->kind == proto::ObjectKind::placed);
}

TEST_CASE("objects: OBJECT_PRESENT on a change and every 2 s, and a new scene starts afresh") {
    Sync sync;
    auto step = Tick(sync, 0, {Enemy(0x100, 0x11, 2), Enemy(0x200, 0x22, 2)});
    auto present = All<proto::ObjectPresent>(step);
    REQUIRE(present.size() == 1);
    CHECK(present[0].scene == kScene);
    CHECK(present[0].objects.size() == 2);
    CHECK(present[0].objects[0].flags == proto::object_presence::awake);

    CHECK(All<proto::ObjectPresent>(Tick(sync, 500 * kMs, {Enemy(0x100, 0x11, 2), Enemy(0x200, 0x22, 2)})).empty());
    present = All<proto::ObjectPresent>(Tick(sync, 600 * kMs, {Enemy(0x100, 0x11, 2)}));
    REQUIRE(present.size() == 1);
    CHECK(present[0].objects.size() == 1);
    CHECK(All<proto::ObjectPresent>(Tick(sync, 2700 * kMs, {Enemy(0x100, 0x11, 2)})).size() == 1);

    // Items and Link are not listed.
    Seen bomb = Enemy(0x500, ~0ULL, 0, 0.0f, "Item_Bomb");
    present = All<proto::ObjectPresent>(Tick(sync, 2800 * kMs, {Enemy(0x100, 0x11, 2), bomb}));
    CHECK(present.empty());

    present = All<proto::ObjectPresent>(Tick(sync, 2900 * kMs, {}, 1, "Spot_Field"));
    REQUIRE(present.size() == 1);
    CHECK(present[0].scene == "Spot_Field");
    CHECK(present[0].objects.empty());
    CHECK(sync.Count() == 0);
}

TEST_CASE("objects: the owner sends samples at most 20 a second, once a second when still") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    CHECK(All<proto::ObjectSample>(Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)})).empty());

    Own(sync, id, 1, 3);
    auto samples = All<proto::ObjectSample>(Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 2)}));
    REQUIRE(samples.size() == 1);
    CHECK(samples[0].id == id);
    CHECK(samples[0].epoch == 3);
    CHECK(samples[0].state.time.server_time_us == 20 * kMs);
    CHECK(samples[0].state.life == proto::ObjectLife{2, 2});

    // Moving: not before 50 ms, then again.
    CHECK(All<proto::ObjectSample>(Tick(sync, 40 * kMs, {Enemy(0x100, 0x11, 2, 1.0f)})).empty());
    samples = All<proto::ObjectSample>(Tick(sync, 70 * kMs, {Enemy(0x100, 0x11, 2, 2.0f)}));
    REQUIRE(samples.size() == 1);
    CHECK((*samples[0].state.position)[0] == doctest::Approx(2.0f));
    CHECK(samples[0].state.time.sequence == 2);
    // Still: nothing until a second has passed.
    CHECK(All<proto::ObjectSample>(Tick(sync, 500 * kMs, {Enemy(0x100, 0x11, 2, 2.0f)})).empty());
    CHECK(All<proto::ObjectSample>(Tick(sync, 1070 * kMs, {Enemy(0x100, 0x11, 2, 2.0f)})).size() == 1);
}

TEST_CASE("objects: a hit on a copy goes to the owner as DAMAGE, summed over 100 ms") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 6)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2, 1);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 6)});

    auto step = Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 5)});
    CHECK(All<proto::Damage>(step).empty());
    step = Tick(sync, 60 * kMs, {Enemy(0x100, 0x11, 3)});
    CHECK(All<proto::Damage>(step).empty());
    step = Tick(sync, 130 * kMs, {Enemy(0x100, 0x11, 3)});
    auto damage = All<proto::Damage>(step);
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].id == id);
    CHECK(damage[0].epoch == 1);
    CHECK(damage[0].amount == 3);
    CHECK(damage[0].player_number == proto::kPlayerSelf);
    // The presence now says it is in a fight.
    auto present = All<proto::ObjectPresent>(step);
    if (present.empty()) {
        present = All<proto::ObjectPresent>(Tick(sync, 2200 * kMs, {Enemy(0x100, 0x11, 3)}));
    }
    REQUIRE(present.size() == 1);
    CHECK((present[0].objects[0].flags & proto::object_presence::in_fight) != 0);
    // Copies send no samples.
    CHECK(All<proto::ObjectSample>(step).empty());
}

TEST_CASE("objects: no owner yet or our own enemy: a life drop sends no DAMAGE") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 6)});
    CHECK(All<proto::Damage>(Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 5)})).empty());
    CHECK(All<proto::Damage>(Tick(sync, 200 * kMs, {Enemy(0x100, 0x11, 5)})).empty());
    Own(sync, *sync.IdOf(0x100), 1);
    Tick(sync, 210 * kMs, {Enemy(0x100, 0x11, 5)});
    CHECK(All<proto::Damage>(Tick(sync, 220 * kMs, {Enemy(0x100, 0x11, 4)})).empty());
    CHECK(All<proto::Damage>(Tick(sync, 400 * kMs, {Enemy(0x100, 0x11, 4)})).empty());
}

TEST_CASE("objects: the owner writes DAMAGE into its enemy, never below 0") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 3)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 1);
    proto::Damage d;
    d.player_number = 2;
    d.id = id;
    d.amount = 1;
    sync.Receive(d);
    auto step = Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 3)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].address == 0x100);
    CHECK(step.writes[0].life == 2);
    // The first sample carries the written life, and it is no hit of ours.
    auto samples = All<proto::ObjectSample>(step);
    REQUIRE(samples.size() == 1);
    CHECK(samples[0].state.life->life == 2);
    CHECK(All<proto::Damage>(Tick(sync, 80 * kMs, {Enemy(0x100, 0x11, 2)})).empty());

    d.amount = 9;
    sync.Receive(d);
    step = Tick(sync, 90 * kMs, {Enemy(0x100, 0x11, 2)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 0);
    // Damage for an enemy we do not have is dropped.
    d.id.value = 0x99;
    sync.Receive(d);
    CHECK(Tick(sync, 100 * kMs, {Enemy(0x100, 0x11, 0)}).writes.empty());
}

TEST_CASE("objects: a copy takes the owner's life, but does not heal back over our own hit") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 4)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    proto::ObjectSample s;
    s.player_number = 2;
    s.id = id;
    s.state.life = proto::ObjectLife{3, 4};
    sync.Receive(s);
    auto step = Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 4)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 3);
    // Our write is not our hit.
    Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 3)});
    CHECK(All<proto::Damage>(Tick(sync, 200 * kMs, {Enemy(0x100, 0x11, 3)})).empty());

    // We hit it (3 -> 2); a sample the owner sent before our DAMAGE says 3.
    Tick(sync, 300 * kMs, {Enemy(0x100, 0x11, 2)});
    sync.Receive(s);
    CHECK(Tick(sync, 310 * kMs, {Enemy(0x100, 0x11, 2)}).writes.empty());
    CHECK(All<proto::Damage>(Tick(sync, 400 * kMs, {Enemy(0x100, 0x11, 2)})).size() == 1);
    sync.Receive(s);
    CHECK(Tick(sync, 500 * kMs, {Enemy(0x100, 0x11, 2)}).writes.empty());
    // Long after, the owner's word wins.
    sync.Receive(s);
    step = Tick(sync, 1000 * kMs, {Enemy(0x100, 0x11, 2)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 3);
}

TEST_CASE("objects: HANDOVER_STATE gives the new owner the last life") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 4)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    proto::HandoverState h;
    h.player_number = 2;
    h.id = id;
    h.epoch = 1;
    h.state.life = proto::ObjectLife{1, 4};
    sync.Receive(h);
    Own(sync, id, 1, 1);
    auto step = Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 4)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 1);
    CHECK(sync.OwnerOf(id) == 1);
}

TEST_CASE("objects: every message it sends passes the client's own check") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 4)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 4)});
    auto step = Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 3)});
    auto more = Tick(sync, 200 * kMs, {Enemy(0x100, 0x11, 3)});
    step.send.insert(step.send.end(), more.send.begin(), more.send.end());
    Own(sync, id, 1);
    more = Tick(sync, 300 * kMs, {Enemy(0x100, 0x11, 3)});
    step.send.insert(step.send.end(), more.send.begin(), more.send.end());
    REQUIRE(step.send.size() >= 3);
    for (const auto& m : step.send) {
        CHECK(proto::check_received(m, proto::Sender::client, proto::channel_of(m)) ==
              proto::Error::none);
    }
}
