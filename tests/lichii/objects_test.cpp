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

TEST_CASE("objects: animals are shared too (hits on a cucco)") {
    CHECK(CoopObjects::IsShared("Animal_Cucco"));
    CHECK_FALSE(CoopObjects::IsShared("Npc_Talon"));
}

TEST_CASE("objects: a rotation matrix goes to a quaternion and back") {
    // A turn of 90 degrees about y, then an arbitrary one.
    const CoopObjects::Mat3 turn{0, 0, 1, 0, 1, 0, -1, 0, 0};
    const auto back = CoopObjects::MatOf(CoopObjects::QuatOf(turn));
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(back[i] == doctest::Approx(turn[i]).epsilon(1e-5));
    }
    const proto::Quat q{0.2f, -0.5f, 0.3f, 0.787401f};
    const auto m = CoopObjects::MatOf(q);
    const auto q2 = CoopObjects::QuatOf(m);
    const float sign = q2[3] * q[3] < 0 ? -1.0f : 1.0f;
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(q2[i] * sign == doctest::Approx(q[i]).epsilon(1e-4));
    }
    CHECK(CoopObjects::QuatOf({}) == proto::Quat{0, 0, 0, 1});  // garbage reads as no turn
}

TEST_CASE("objects: the owner's samples carry rotation and velocity") {
    Sync sync;
    Seen e = Enemy(0x100, 0x11, 2, 0.0f);
    e.rotation = CoopObjects::Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1};
    Tick(sync, 0, {e});
    Own(sync, *sync.IdOf(0x100), 1);
    Tick(sync, 10 * kMs, {e});
    e.position[0] = 1.0f;
    e.rotation = CoopObjects::Mat3{0, 0, 1, 0, 1, 0, -1, 0, 0};
    const auto samples = All<proto::ObjectSample>(Tick(sync, 110 * kMs, {e}));
    REQUIRE(samples.size() == 1);
    REQUIRE(samples[0].state.rotation);
    CHECK((*samples[0].state.rotation)[3] == doctest::Approx(0.7071f).epsilon(1e-3));
    REQUIRE(samples[0].state.velocity);
    CHECK((*samples[0].state.velocity)[0] == doctest::Approx(10.0f));  // 1 m in 0.1 s
    // Turning on the spot is a change too.
    e.rotation = CoopObjects::Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1};
    CHECK(All<proto::ObjectSample>(Tick(sync, 170 * kMs, {e})).size() == 1);
}

TEST_CASE("objects: a copy is placed where the owner's enemy was 100 ms ago") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2, 50.0f)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    std::vector<CoopObjects::PoseWrite> poses;
    sync.Poses(10 * kMs, poses);
    CHECK(poses.empty());  // no owner, nothing received

    Own(sync, id, 2);
    const auto sample = [&](std::uint64_t t, float x, float vx = 0.0f) {
        proto::ObjectSample s;
        s.player_number = 2;
        s.id = id;
        s.state.time = {t, 0};
        s.state.position = proto::Vec3{x, 0.0f, 0.0f};
        s.state.velocity = proto::Vec3{vx, 0.0f, 0.0f};
        s.state.rotation = proto::Quat{0, 0, 0, 1};
        sync.Receive(s);
    };
    sample(1000 * kMs, 0.0f);
    sample(1050 * kMs, 1.0f, 20.0f);
    Tick(sync, 1100 * kMs, {Enemy(0x100, 0x11, 2, 50.0f)});

    poses.clear();
    sync.Poses(1125 * kMs, poses);  // shows 1025: halfway
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].address == 0x100);
    CHECK(poses[0].position[0] == doctest::Approx(0.5f));
    REQUIRE(poses[0].rotation);
    CHECK((*poses[0].rotation)[0] == doctest::Approx(1.0f));

    poses.clear();
    sync.Poses(1200 * kMs, poses);  // shows 1100: 50 ms past the last, at 20 m/s
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].position[0] == doctest::Approx(2.0f));
    poses.clear();
    sync.Poses(1900 * kMs, poses);  // never more than 150 ms ahead
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].position[0] == doctest::Approx(4.0f));

    // Long silence: the copy is left to itself.
    poses.clear();
    sync.Poses(4000 * kMs, poses);
    CHECK(poses.empty());

    // Ours now: nothing pulls it.
    sample(4000 * kMs, 3.0f);
    Own(sync, id, 1, 1);
    Tick(sync, 4010 * kMs, {Enemy(0x100, 0x11, 2, 50.0f)});
    poses.clear();
    sync.Poses(4100 * kMs, poses);
    CHECK(poses.empty());
}

TEST_CASE("objects: with hurt on, a copy loses life through the game and it is no hit of ours") {
    Sync sync;
    sync.SetHurt(true);
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
    CHECK(step.writes[0].hurt == 1);
    CHECK(step.writes[0].life == 3);
    // The same sample again asks for nothing more.
    sync.Receive(s);
    CHECK(Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 4)}).writes.empty());
    // The game takes it: no DAMAGE goes back.
    Tick(sync, 40 * kMs, {Enemy(0x100, 0x11, 3)});
    CHECK(All<proto::Damage>(Tick(sync, 300 * kMs, {Enemy(0x100, 0x11, 3)})).empty());
    CHECK(sync.HurtsTaken() == 1);
    // Our own hit after that still goes.
    Tick(sync, 600 * kMs, {Enemy(0x100, 0x11, 2)});
    CHECK(All<proto::Damage>(Tick(sync, 800 * kMs, {Enemy(0x100, 0x11, 2)})).size() == 1);
}

TEST_CASE("objects: a hurt the game does not take is written as life after 400 ms") {
    Sync sync;
    sync.SetHurt(true);
    Tick(sync, 0, {Enemy(0x100, 0x11, 3)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 1);
    proto::Damage d;
    d.player_number = 2;
    d.id = id;
    d.amount = 2;
    sync.Receive(d);
    auto step = Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 3)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].hurt == 2);
    // A second hit while the first is pending stacks on the target.
    d.amount = 1;
    sync.Receive(d);
    step = Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 3)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].hurt == 1);
    CHECK(step.writes[0].life == 0);
    CHECK(Tick(sync, 300 * kMs, {Enemy(0x100, 0x11, 3)}).writes.empty());
    step = Tick(sync, 430 * kMs, {Enemy(0x100, 0x11, 3)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].hurt == 0);
    CHECK(step.writes[0].life == 0);
    CHECK(sync.HurtsMissed() == 1);
}

TEST_CASE("objects: a pickup another game took is hidden here") {
    Sync sync;
    Seen token;
    token.address = 0x300;
    token.name = "Obj_RupeeToken";
    token.hash = 0x6a8d9b2373d08fc9;
    token.position = {5.0f, 2.0f, 1.0f};
    CHECK_FALSE(sync.Taken(token.hash));
    Tick(sync, 0, {token, Enemy(0x100, 0x11, 2)});

    proto::FlagState state;
    state.player_number = 2;
    state.flags.push_back(proto::FlagEntry(proto::FlagKey::key64_flag(token.hash),
                                           proto::FlagKind::boolean, 1));
    state.flags.push_back(proto::FlagEntry(proto::FlagKey::key64_flag(0x77),
                                           proto::FlagKind::boolean, 0));
    sync.Receive(state);
    Tick(sync, 10 * kMs, {token, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Taken(token.hash));
    CHECK_FALSE(sync.Taken(0x77));

    Tick(sync, 20 * kMs, {token, Enemy(0x100, 0x11, 2)});
    std::vector<CoopObjects::PoseWrite> poses;
    sync.Poses(30 * kMs, poses);
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].address == 0x300);
    CHECK(poses[0].position[1] == doctest::Approx(2.0f - CoopObjects::kHideDrop));
    // Where it was is kept, not the hidden place read back.
    token.position[1] -= CoopObjects::kHideDrop;
    Tick(sync, 40 * kMs, {token, Enemy(0x100, 0x11, 2)});
    poses.clear();
    sync.Poses(50 * kMs, poses);
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].position[1] == doctest::Approx(2.0f - CoopObjects::kHideDrop));
    // A taken enemy is not shared (not in OBJECT_PRESENT).
    token.name = "Enemy_Skulltula_Gold";
    token.address = 0x400;
    const auto step = Tick(sync, 3000 * kMs, {token, Enemy(0x100, 0x11, 2)});
    const auto present = All<proto::ObjectPresent>(step);
    REQUIRE(present.size() == 1);
    CHECK(present[0].objects.size() == 1);
}
