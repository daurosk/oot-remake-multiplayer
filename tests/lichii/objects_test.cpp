// Tests for the shared enemies' hook side (lichii/hook/coop_objects_sync.cpp).
// No emulator code: each walk is a list of actors.
#include <doctest/doctest.h>

#include <cstring>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_hold.h"
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
    Tick(one, 0, {Enemy(0x100, ~0ULL, 2, 1.0f, "Enemy_Keese"), Enemy(0x200, ~0ULL, 2, 5.0f, "Enemy_Keese")});
    Tick(two, 0, {Enemy(0x900, 0, 2, 5.0f, "Enemy_Keese"), Enemy(0x800, ~0ULL, 2, 1.0f, "Enemy_Keese")});
    CHECK(*one.IdOf(0x100) == *two.IdOf(0x800));
    CHECK(*one.IdOf(0x200) == *two.IdOf(0x900));
    // A placed enemy keeps its placement hash.
    Tick(one, 10 * kMs, {Enemy(0x100, ~0ULL, 2, 1.0f, "Enemy_Keese"), Enemy(0x200, ~0ULL, 2, 5.0f, "Enemy_Keese"),
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
    // One walk without an enemy is not enough: it is gone after two.
    CHECK(All<proto::ObjectPresent>(Tick(sync, 600 * kMs, {Enemy(0x100, 0x11, 2)})).empty());
    present = All<proto::ObjectPresent>(Tick(sync, 610 * kMs, {Enemy(0x100, 0x11, 2)}));
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
    step = Tick(sync, 1500 * kMs, {Enemy(0x100, 0x11, 2)});
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

TEST_CASE("objects: animals and townspeople are shared too (hits on a cucco)") {
    CHECK(CoopObjects::IsShared("Animal_Cucco"));
    CHECK(CoopObjects::IsShared("Npc_Hylia_051"));
    CHECK_FALSE(CoopObjects::IsShared("CoopPuppet_Child"));
    CHECK_FALSE(CoopObjects::IsShared("Npc_Navi"));
    CHECK_FALSE(CoopObjects::IsShared("Npc_Equipment_002"));
    CHECK_FALSE(CoopObjects::IsShared("Npc_Invisible_Hylia_Dampe_001"));
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

TEST_CASE("objects: a pickup another game took is removed here") {
    Sync sync;
    Seen token;
    token.address = 0x300;
    token.name = "Obj_RupeeToken";
    token.hash = 0x6a8d9b2373d08fc9;
    token.position = {5.0f, 2.0f, 1.0f};
    CHECK_FALSE(sync.Taken(token.hash));
    Tick(sync, 0, {token, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());

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
    CHECK(sync.Removes() == std::vector<std::uint64_t>{0x300});
    // Gone from the list: nothing more to remove.
    Tick(sync, 30 * kMs, {Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    // A taken enemy is not shared (not in OBJECT_PRESENT).
    token.name = "Enemy_Skulltula_Gold";
    token.address = 0x400;
    const auto step = Tick(sync, 3000 * kMs, {token, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes() == std::vector<std::uint64_t>{0x400});
    const auto present = All<proto::ObjectPresent>(step);
    REQUIRE(present.size() == 1);
    CHECK(present[0].objects.size() == 1);
}

TEST_CASE("objects: the owner sends its enemy's animation; a copy is asked to play it") {
    Sync owner;
    Seen e = Enemy(0x100, 0x11, 2);
    e.animation = proto::Animation{"Wait", 3.0f, 1.0f};
    Tick(owner, 0, {e});
    const proto::ObjectId id = *owner.IdOf(0x100);
    Own(owner, id, 1);
    auto samples = All<proto::ObjectSample>(Tick(owner, 10 * kMs, {e}));
    REQUIRE(samples.size() == 1);
    REQUIRE(samples[0].state.animation);
    CHECK(samples[0].state.animation->name == "Wait");
    // A new animation alone is a change worth a sample.
    e.animation = proto::Animation{"Damage", 0.0f, 1.0f};
    samples = All<proto::ObjectSample>(Tick(owner, 70 * kMs, {e}));
    REQUIRE(samples.size() == 1);
    CHECK(samples[0].state.animation->name == "Damage");

    Sync copy;
    Seen c = Enemy(0x200, 0x11, 2);
    c.animation = proto::Animation{"Walk", 0.0f, 1.0f};
    Tick(copy, 0, {c});
    Own(copy, id, 1);
    proto::ObjectSample s = samples[0];
    s.player_number = 1;
    s.state.time = {1000 * kMs, 1};
    copy.Receive(s);
    Tick(copy, 1010 * kMs, {c}, 2);
    std::vector<CoopObjects::PlayWish> plays;
    copy.Plays(1020 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].address == 0x200);
    CHECK(plays[0].name == "Damage");
    // Still asked once it shows it: the mover compares names every game
    // frame, so the copy's own AI cannot keep its own animation.
    c.animation = proto::Animation{"Damage", 2.0f, 1.0f};
    Tick(copy, 1040 * kMs, {c}, 2);
    plays.clear();
    copy.Plays(1050 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "Damage");
    CHECK(plays[0].sample == 1000 * kMs);
}

TEST_CASE("objects: a copy's animation frame follows the owner's, shown 100 ms late") {
    using CoopObjects::FrameAfter;
    CHECK(FrameAfter(10.0f, 1.0f, 0) == doctest::Approx(10.0f));
    CHECK(FrameAfter(10.0f, 2.0f, 100'000) == doctest::Approx(16.0f));
    CHECK(FrameAfter(1.0f, 1.0f, -100'000) == doctest::Approx(0.0f));  // never below 0

    Sync owner;
    Seen e = Enemy(0x100, 0x11, 2);
    e.animation = proto::Animation{"Lifted", 10.0f, 2.0f};
    Tick(owner, 0, {e});
    const proto::ObjectId id = *owner.IdOf(0x100);
    Own(owner, id, 1);
    auto samples = All<proto::ObjectSample>(Tick(owner, 10 * kMs, {e}));
    REQUIRE(samples.size() == 1);

    Sync copy;
    Seen c = Enemy(0x200, 0x11, 2);
    Tick(copy, 0, {c});
    Own(copy, id, 1);
    proto::ObjectSample s = samples[0];
    s.player_number = 1;
    s.state.time = {1000 * kMs, 1};
    copy.Receive(s);
    Tick(copy, 1010 * kMs, {c}, 2);
    std::vector<CoopObjects::PlayWish> plays;
    // 200 ms after the sample, shown 100 ms late: 100 ms of playback at rate 2.
    copy.Plays(1200 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].frame == doctest::Approx(16.0f));
    CHECK(plays[0].rate == doctest::Approx(2.0f));
    // Long after: no further ahead than a pose extrapolates (150 ms).
    plays.clear();
    copy.Plays(2000 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].frame == doctest::Approx(19.0f));
}

namespace {
proto::PlayerSample Where(std::uint8_t player, const std::string& scene, std::uint64_t now) {
    proto::PlayerSample p;
    p.player_number = player;
    p.time = {now, 1};
    p.scene = scene;
    return p;
}
} // namespace

TEST_CASE("objects: one game rolls Stalchild, the others delete theirs and make the decided ones") {
    CHECK(CoopObjects::IsRolled("Enemy_Stalchild"));
    CHECK(CoopObjects::IsRolled("Enemy_Stalchild_Big"));
    CHECK_FALSE(CoopObjects::IsRolled("Enemy_Stalchild_Dictionary"));

    Sync a;  // player 1: decides
    Sync b;  // player 2
    a.Receive(Where(2, kScene, 0));
    b.Receive(Where(1, kScene, 0));

    // A's game rolls a Stalchild: it is shared as a made id.
    const Step rolled = Tick(a, 100 * kMs, {Enemy(0x100, ~0ULL, 2, 7.0f)}, 1);
    CHECK(a.Decider() == 1);
    const auto spawns = All<proto::ObjectSpawn>(rolled);
    REQUIRE(spawns.size() == 1);
    CHECK(spawns[0].id.kind == proto::ObjectKind::made);
    CHECK(spawns[0].id.maker == 1);
    CHECK(spawns[0].actor == "Enemy_Stalchild");
    CHECK((*spawns[0].state.position)[0] == doctest::Approx(7.0f));
    CHECK(a.OwnerOf(spawns[0].id) == 1);
    CHECK(a.Removes().empty());

    // B's game rolls its own: it is not shared, and B deletes it.
    const Step own = Tick(b, 100 * kMs, {Enemy(0x500, ~0ULL, 2, -30.0f)}, 2);
    CHECK(b.Decider() == 1);
    CHECK(All<proto::ObjectSpawn>(own).empty());
    CHECK_FALSE(b.IdOf(0x500));
    CHECK(b.Removes() == std::vector<std::uint64_t>{0x500});
    CHECK(b.Refused() == 1);

    // A's spawn reaches B: B asks the director for it, where A's one is.
    b.Receive(spawns[0]);
    Tick(b, 150 * kMs, {}, 2);
    auto creates = b.Creates();
    REQUIRE(creates.size() == 1);
    CHECK(creates[0].name == "Enemy_Stalchild");
    CHECK(creates[0].position[0] == doctest::Approx(7.0f));
    CHECK_FALSE(creates[0].removed);
    CHECK(b.OwnerOf(spawns[0].id) == 1);

    // The director's actor shows up before the director lists it: it is near
    // the decided place, so B waits instead of deleting it.
    Tick(b, 200 * kMs, {Enemy(0x600, ~0ULL, 2, 7.5f)}, 2);
    CHECK_FALSE(b.IdOf(0x600));
    CHECK(b.Removes().empty());

    // Listed: it takes A's id and follows A's samples.
    b.Bind({{creates[0].key, 0x600}});
    Tick(b, 250 * kMs, {Enemy(0x600, ~0ULL, 2, 7.5f)}, 2);
    REQUIRE(b.IdOf(0x600));
    CHECK(*b.IdOf(0x600) == spawns[0].id);
    proto::ObjectSample sample;
    sample.id = spawns[0].id;
    sample.state.time = {240 * kMs, 1};
    sample.state.position = proto::Vec3{8.0f, 0.0f, 0.0f};
    b.Receive(sample);
    b.Bind({{creates[0].key, 0x600}});
    Tick(b, 300 * kMs, {Enemy(0x600, ~0ULL, 2, 7.5f)}, 2);
    std::vector<CoopObjects::PoseWrite> poses;
    b.Poses(400 * kMs, poses);
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].position[0] == doctest::Approx(8.0f));

    // A's Stalchild dies and goes: A sends OBJECT_REMOVE; B deletes its copy.
    // Gone only after two walks in a row miss it (kMissedWalks).
    CHECK(All<proto::ObjectRemove>(Tick(a, 450 * kMs, {}, 1)).empty());
    const Step gone = Tick(a, 500 * kMs, {}, 1);
    const auto removes = All<proto::ObjectRemove>(gone);
    REQUIRE(removes.size() == 1);
    CHECK(removes[0].id == spawns[0].id);
    b.Receive(removes[0]);
    b.Bind({{creates[0].key, 0x600}});
    Tick(b, 550 * kMs, {Enemy(0x600, ~0ULL, 2, 7.5f)}, 2);
    creates = b.Creates();
    REQUIRE(creates.size() == 1);
    CHECK(creates[0].removed);
    b.Bind({});
    Tick(b, 600 * kMs, {}, 2);
    CHECK(b.Creates().empty());
}

TEST_CASE("objects: alone or first in the scene, a game rolls its own Stalchild") {
    Sync b;
    // Player 1 is elsewhere, or its scene is old news: B decides.
    b.Receive(Where(1, "Spot_Field", 0));
    const Step s = Tick(b, 0, {Enemy(0x500, ~0ULL, 2)}, 2);
    CHECK(b.Decider() == 2);
    CHECK(All<proto::ObjectSpawn>(s).size() == 1);

    Sync c;
    c.Receive(Where(1, kScene, 0));
    Tick(c, Sync::kSceneStaleUs + kMs, {Enemy(0x700, ~0ULL, 2)}, 3);
    CHECK(c.Decider() == 3);
    CHECK(c.Removes().empty());

    // A higher player in the scene does not take it from a lower one.
    Sync a;
    a.Receive(Where(3, kScene, 0));
    Tick(a, 0, {}, 1);
    CHECK(a.Decider() == 1);
}

TEST_CASE("objects: a Gold Skulltula shaken out of a tree is announced and copied where missing") {
    constexpr std::uint64_t kHash = 0x32f42f523be6425ULL;
    const proto::ObjectId id{proto::ObjectKind::placed, 0, kHash};
    Sync a;
    // Loaded with the scene: not announced, even when owned.
    Tick(a, 0, {Enemy(0x100, 0x1111, 1, 0.0f, "Enemy_Skulltula_Gold")}, 1);
    Own(a, {proto::ObjectKind::placed, 0, 0x1111}, 1);
    CHECK(All<proto::ObjectSpawn>(Tick(a, 100 * kMs, {Enemy(0x100, 0x1111, 1, 0.0f, "Enemy_Skulltula_Gold")}, 1)).empty());

    // Out of a tree 5 s later: announced once we own it, once.
    const std::vector<Seen> both = {Enemy(0x100, 0x1111, 1, 0.0f, "Enemy_Skulltula_Gold"),
                                    Enemy(0x200, kHash, 1, 9.0f, "Enemy_Skulltula_Gold")};
    CHECK(All<proto::ObjectSpawn>(Tick(a, 5000 * kMs, both, 1)).empty());
    Own(a, id, 1, 3);
    const auto spawns = All<proto::ObjectSpawn>(Tick(a, 5100 * kMs, both, 1));
    REQUIRE(spawns.size() == 1);
    CHECK(spawns[0].id == id);
    CHECK(spawns[0].actor == "Enemy_Skulltula_Gold");
    CHECK(All<proto::ObjectSpawn>(Tick(a, 5200 * kMs, both, 1)).empty());

    // B lacks it: a copy is made and takes the placed id.
    Sync b;
    Tick(b, 0, {}, 2);
    proto::ObjectSpawn relayed = spawns[0];
    relayed.player_number = 1;
    b.Receive(relayed);
    Tick(b, 5300 * kMs, {}, 2);
    auto creates = b.Creates();
    REQUIRE(creates.size() == 1);
    CHECK(creates[0].position[0] == doctest::Approx(9.0f));
    CHECK(b.OwnerOf(id) == 1);
    b.Bind({{creates[0].key, 0x900}});
    Tick(b, 5400 * kMs, {Enemy(0x900, ~0ULL, 1, 9.0f, "Enemy_Skulltula_Gold")}, 2);
    REQUIRE(b.IdOf(0x900));
    CHECK(*b.IdOf(0x900) == id);

    // B shakes the same tree: its own one comes, the copy goes.
    b.Bind({{creates[0].key, 0x900}});
    Tick(b, 5500 * kMs, {Enemy(0x900, ~0ULL, 1, 9.0f, "Enemy_Skulltula_Gold"),
                         Enemy(0x950, kHash, 1, 9.0f, "Enemy_Skulltula_Gold")}, 2);
    CHECK(*b.IdOf(0x950) == id);
    CHECK_FALSE(b.IdOf(0x900));
    REQUIRE(b.Creates().size() == 1);
    CHECK(b.Creates()[0].removed);

    // C has it loaded already: no copy.
    Sync c;
    Tick(c, 0, {Enemy(0x300, kHash, 1, 9.0f, "Enemy_Skulltula_Gold")}, 3);
    c.Receive(relayed);
    Tick(c, 100 * kMs, {Enemy(0x300, kHash, 1, 9.0f, "Enemy_Skulltula_Gold")}, 3);
    CHECK(c.Creates().empty());
}

TEST_CASE("objects: a hurt carries the hitter's place; an animal's life is written") {
    Sync sync;
    sync.SetHurt(true);
    Tick(sync, 0, {Enemy(0x100, 0x11, 2), Enemy(0x200, 0x22, 2, 5.0f, "Animal_Cucco")});
    const proto::ObjectId enemy = *sync.IdOf(0x100);
    const proto::ObjectId animal = *sync.IdOf(0x200);
    Own(sync, enemy, 1);
    Own(sync, animal, 1);
    proto::PlayerSample p = Where(2, kScene, 5 * kMs);
    p.position = proto::Vec3{7.0f, 0.0f, -3.0f};
    sync.Receive(p);
    proto::Damage d;
    d.player_number = 2;
    d.id = enemy;
    d.amount = 1;
    sync.Receive(d);
    d.id = animal;
    sync.Receive(d);
    const std::vector<Seen> seen{Enemy(0x100, 0x11, 2), Enemy(0x200, 0x22, 2, 5.0f, "Animal_Cucco")};
    const auto step = Tick(sync, 10 * kMs, seen);
    REQUIRE(step.writes.size() == 2);
    for (const auto& w : step.writes) {
        CHECK(w.life == 1);
        if (w.address == 0x100) {
            CHECK(w.hurt == 1);  // through the game: it reacts
            REQUIRE(w.from.has_value());
            CHECK((*w.from)[0] == doctest::Approx(7.0f));
            CHECK((*w.from)[2] == doctest::Approx(-3.0f));
        } else {
            CHECK(w.hurt == 0);
            CHECK(!w.from.has_value());
        }
    }
    // A place not heard of for over 2 s is not used.
    d.id = enemy;
    sync.Receive(d);
    const auto late = Tick(sync, 3000 * kMs, {Enemy(0x100, 0x11, 1), seen[1]});
    REQUIRE(late.writes.size() == 1);
    CHECK(late.writes[0].hurt == 1);
    CHECK(!late.writes[0].from.has_value());
}

TEST_CASE("objects: after our hit on a copy the owner's old animation waits for its reaction") {
    Sync owner;
    Seen e = Enemy(0x100, 0x11, 2);
    e.animation = proto::Animation{"Walk", 10.0f, 1.0f};
    Tick(owner, 0, {e});
    const proto::ObjectId id = *owner.IdOf(0x100);
    Own(owner, id, 1);
    auto samples = All<proto::ObjectSample>(Tick(owner, 10 * kMs, {e}));
    REQUIRE(samples.size() == 1);

    Sync copy;
    Seen c = Enemy(0x200, 0x11, 2);
    Tick(copy, 0, {c}, 2);
    Own(copy, id, 1);
    proto::ObjectSample s = samples[0];
    s.player_number = 1;
    s.state.time = {1000 * kMs, 1};
    copy.Receive(s);
    Tick(copy, 1010 * kMs, {c}, 2);
    std::vector<CoopObjects::PlayWish> plays;
    copy.Plays(1020 * kMs, plays);
    REQUIRE(plays.size() == 1);
    // Our sword hits the copy: its own game plays the flinch, 'Walk' waits.
    c.life = proto::ObjectLife{1, 2};
    Tick(copy, 1100 * kMs, {c}, 2);
    plays.clear();
    copy.Plays(1110 * kMs, plays);
    CHECK(plays.empty());
    // The owner's reaction to our DAMAGE is played at once.
    s.state.animation = proto::Animation{"Damage_Head", 2.0f, 1.0f};
    s.state.life = proto::ObjectLife{1, 2};
    s.state.time = {1250 * kMs, 2};
    copy.Receive(s);
    Tick(copy, 1260 * kMs, {c}, 2);
    plays.clear();
    copy.Plays(1270 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "Damage_Head");
    // And 'Walk' again once the hold is over, had the owner never reacted.
    s.state.animation = proto::Animation{"Walk", 3.0f, 1.0f};
    s.state.time = {1900 * kMs, 3};
    copy.Receive(s);
    Tick(copy, 1910 * kMs, {c}, 2);
    plays.clear();
    copy.Plays(1920 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "Walk");
}

TEST_CASE("objects: a hurt the game takes after its life was written is put back, not sent on") {
    Sync sync;
    sync.SetHurt(true);
    Tick(sync, 0, {Enemy(0x100, 0x11, 4)}, 2);
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 1);
    proto::ObjectSample s;
    s.player_number = 1;
    s.id = id;
    s.state.life = proto::ObjectLife{3, 4};
    sync.Receive(s);
    auto step = Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 4)}, 2);
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].hurt == 1);
    // Not taken in 400 ms: written as life.
    step = Tick(sync, 420 * kMs, {Enemy(0x100, 0x11, 4)}, 2);
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].hurt == 0);
    CHECK(step.writes[0].life == 3);
    Tick(sync, 450 * kMs, {Enemy(0x100, 0x11, 3)}, 2);
    // The game takes the old hit now (it was paused): 3 -> 2. Put back to 3.
    step = Tick(sync, 700 * kMs, {Enemy(0x100, 0x11, 2)}, 2);
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 3);
    CHECK(step.writes[0].hurt == 0);
    CHECK(sync.HurtsLate() == 1);
    step = Tick(sync, 900 * kMs, {Enemy(0x100, 0x11, 3)}, 2);
    CHECK(All<proto::Damage>(step).empty());
    // A real hit of ours after that still goes to the owner.
    Tick(sync, 1000 * kMs, {Enemy(0x100, 0x11, 2)}, 2);
    CHECK(All<proto::Damage>(Tick(sync, 1200 * kMs, {Enemy(0x100, 0x11, 2)}, 2)).size() == 1);
}

TEST_CASE("objects: an enemy that becomes ours keeps its place and takes the lower life") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 4, 50.0f)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    proto::ObjectSample s;
    s.player_number = 2;
    s.id = id;
    s.state.time = {1000 * kMs, 0};
    s.state.position = proto::Vec3{0.0f, 0.0f, 0.0f};
    sync.Receive(s);
    Tick(sync, 1100 * kMs, {Enemy(0x100, 0x11, 4, 50.0f)});
    std::vector<CoopObjects::PoseWrite> poses;
    sync.Poses(1150 * kMs, poses);
    REQUIRE(poses.size() == 1);  // a copy: placed where player 2's is

    // The old owner's last state has more life than ours had left: no heal.
    proto::HandoverState h;
    h.player_number = 2;
    h.id = id;
    h.epoch = 1;
    h.state.life = proto::ObjectLife{4, 4};
    sync.Receive(h);
    Own(sync, id, 1, 1);
    auto step = Tick(sync, 1200 * kMs, {Enemy(0x100, 0x11, 3, 50.0f)});
    CHECK(step.writes.empty());
    // Ours where it stands here: no pose, and its sample goes out at once.
    poses.clear();
    sync.Poses(1210 * kMs, poses);
    CHECK(poses.empty());
    const auto samples = All<proto::ObjectSample>(step);
    REQUIRE(samples.size() == 1);
    CHECK(samples[0].epoch == 1);
    CHECK(samples[0].state.life->life == 3);
    CHECK((*samples[0].state.position)[0] == doctest::Approx(50.0f));
    // The hit that took it from 4 to 3 here is in its life: no DAMAGE for it,
    // and our own DAMAGE coming back to us as the owner is not taken twice.
    proto::Damage d;
    d.player_number = 1;
    d.id = id;
    d.epoch = 0;
    d.amount = 1;
    sync.Receive(d);
    step = Tick(sync, 1400 * kMs, {Enemy(0x100, 0x11, 3, 50.0f)});
    CHECK(All<proto::Damage>(step).empty());
    CHECK(step.writes.empty());
    // A late sample of the old owner does not make it a copy again.
    s.state.time = {1390 * kMs, 1};
    sync.Receive(s);
    Tick(sync, 1450 * kMs, {Enemy(0x100, 0x11, 3, 50.0f)});
    poses.clear();
    sync.Poses(1460 * kMs, poses);
    CHECK(poses.empty());
}

TEST_CASE("objects: an enemy that stops being ours follows the new owner at once") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 4, 50.0f)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    const auto sample = [&](std::uint8_t from, std::uint16_t epoch, std::uint64_t t, float x) {
        proto::ObjectSample s;
        s.player_number = from;
        s.id = id;
        s.epoch = epoch;
        s.state.time = {t, 0};
        s.state.position = proto::Vec3{x, 0.0f, 0.0f};
        sync.Receive(s);
    };
    sample(2, 0, 1000 * kMs, 0.0f);
    sample(2, 0, 1050 * kMs, 1.0f);
    Tick(sync, 1100 * kMs, {Enemy(0x100, 0x11, 4, 50.0f)});

    // Player 3 takes it over 30 m away: its first sample places the copy
    // there, with nothing of player 2's blended in, and a late sample of
    // player 2 (the older epoch) is dropped.
    Own(sync, id, 3, 1);
    sample(3, 1, 1150 * kMs, 30.0f);
    sample(2, 0, 1160 * kMs, 2.0f);
    Tick(sync, 1200 * kMs, {Enemy(0x100, 0x11, 4, 50.0f)});
    std::vector<CoopObjects::PoseWrite> poses;
    sync.Poses(1200 * kMs, poses);
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].position[0] == doctest::Approx(30.0f));

    // We own it, then lose it to player 2: we stop sending and follow.
    Own(sync, id, 1, 2);
    auto step = Tick(sync, 1300 * kMs, {Enemy(0x100, 0x11, 4, 50.0f)});
    CHECK(All<proto::ObjectSample>(step).size() == 1);
    Own(sync, id, 2, 3);
    sample(2, 3, 1350 * kMs, 7.0f);
    step = Tick(sync, 1400 * kMs, {Enemy(0x100, 0x11, 4, 51.0f)});
    CHECK(All<proto::ObjectSample>(step).empty());
    poses.clear();
    sync.Poses(1400 * kMs, poses);
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].position[0] == doctest::Approx(7.0f));
    // An owner change that arrives late (an older epoch) changes nothing.
    Own(sync, id, 1, 2);
    Tick(sync, 1450 * kMs, {Enemy(0x100, 0x11, 4, 51.0f)});
    CHECK(sync.OwnerOf(id) == 2);
}

TEST_CASE("objects: the AI hold covers enemy copies only, and ends when one becomes ours") {
    Sync sync;
    const std::vector<Seen> seen{Enemy(0x100, 0x11, 4, 50.0f),
                                 Enemy(0x200, 0x22, 4, 60.0f, "Animal_Cucco"),
                                 Enemy(0x300, 0x33, 4, 70.0f)};
    Tick(sync, 0, seen);
    CHECK(sync.Holds(10 * kMs).empty());  // no owner yet: its own AI runs
    const auto sample = [&](std::uint64_t address, std::uint64_t t) {
        proto::ObjectSample s;
        s.player_number = 2;
        s.id = *sync.IdOf(address);
        s.state.time = {t, 0};
        s.state.position = proto::Vec3{1.0f, 0.0f, 0.0f};
        sync.Receive(s);
    };
    Own(sync, *sync.IdOf(0x100), 2);
    Own(sync, *sync.IdOf(0x200), 2);
    Own(sync, *sync.IdOf(0x300), 1);
    Tick(sync, 900 * kMs, seen);
    CHECK(sync.Holds(950 * kMs).empty());  // player 2's, but no sample to follow yet
    sample(0x100, 1000 * kMs);
    sample(0x200, 1000 * kMs);
    Tick(sync, 1050 * kMs, seen);
    // The enemy copy is held; the animal copy and our own enemy are not.
    CHECK(sync.Holds(1100 * kMs) == std::vector<std::uint64_t>{0x100});
    // The owner falls silent: the copy is let go with its pose.
    CHECK(sync.Holds(1000 * kMs + Sync::kPoseStaleUs + kMs).empty());
    // It becomes ours: not held from the next list on.
    Own(sync, *sync.IdOf(0x100), 1, 1);
    Tick(sync, 1150 * kMs, seen);
    CHECK(sync.Holds(1160 * kMs).empty());
    // And held again once it is player 2's and placed again.
    Own(sync, *sync.IdOf(0x100), 2, 2);
    proto::ObjectSample s;
    s.player_number = 2;
    s.id = *sync.IdOf(0x100);
    s.epoch = 2;
    s.state.time = {1200 * kMs, 0};
    s.state.position = proto::Vec3{1.0f, 0.0f, 0.0f};
    sync.Receive(s);
    Tick(sync, 1250 * kMs, seen);
    CHECK(sync.Holds(1260 * kMs) == std::vector<std::uint64_t>{0x100});
}

TEST_CASE("objects: the AI hold's table and hook") {
    // bl from main+0x369df8 to the hold cave, which sits after the co-op cave.
    const std::uint32_t word = CoopHold::HookWord();
    CHECK((word >> 26) == 0x25);
    CHECK(CoopHold::kHookOffset + (static_cast<std::uint64_t>(word & 0x3ffffff) << 2) ==
          CoopHold::kCaveOffset);
    CHECK(CoopHold::kCaveOffset + CoopHold::kCave.size() <= CoopHold::kTableOffset);
    // Mode 1 sets the brain skip bit, mode 2 clears it, anything else changes nothing.
    CHECK(((0x1230u & CoopHold::MasksFor(1).and_mask) | CoopHold::MasksFor(1).or_mask) == 0x1231u);
    CHECK(((0x1231u & CoopHold::MasksFor(2).and_mask) | CoopHold::MasksFor(2).or_mask) == 0x1230u);
    CHECK(((0x1231u & CoopHold::MasksFor(0).and_mask) | CoopHold::MasksFor(0).or_mask) == 0x1231u);
    // Count, masks, then the components; empty entries and those past 64 are left out.
    std::vector<std::uint64_t> components{0x1111, 0, 0x2222};
    auto table = CoopHold::Table(components, CoopHold::MasksFor(1));
    const auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        std::memcpy(&v, table.data() + at, 4);
        return v;
    };
    const auto u64 = [&](std::size_t at) {
        std::uint64_t v = 0;
        std::memcpy(&v, table.data() + at, 8);
        return v;
    };
    CHECK(u32(0) == 2);
    CHECK(u32(4) == 0xffffffffu);
    CHECK(u32(8) == 1);
    CHECK(u64(0x10) == 0x1111);
    CHECK(u64(0x18) == 0x2222);
    components.assign(100, 0x3333);
    table = CoopHold::Table(components, CoopHold::MasksFor(2));
    CHECK(u32(0) == CoopHold::kMaxHeld);
    CHECK(u32(4) == 0xfffffffeu);
    // No enemy held: an empty table, so every brain runs as the game wants.
    table = CoopHold::Table({}, CoopHold::MasksFor(1));
    CHECK(u32(0) == 0);
}

TEST_CASE("objects: group rules for parts and bosses") {
    using CoopObjects::IsBoss;
    using CoopObjects::PartOf;
    REQUIRE(PartOf("Enemy_DungeonBoss_BongoBongo_LeftHand") != nullptr);
    CHECK(std::string(PartOf("Enemy_DungeonBoss_BongoBongo_LeftHand")->roots.front()) ==
          "Enemy_DungeonBoss_BongoBongo");
    CHECK(PartOf("Enemy_DungeonBoss_BongoBongo_LeftHand")->boss);
    REQUIRE(PartOf("Enemy_Peahat_Larva") != nullptr);
    CHECK_FALSE(PartOf("Enemy_Peahat_Larva")->boss);
    // Independent larvae, the picture book and plain enemies are no parts.
    CHECK(PartOf("Enemy_Peahat_Larva_Independent") == nullptr);
    CHECK(PartOf("Enemy_DungeonBoss_BongoBongo_LeftHand_Dictionary") == nullptr);
    CHECK(PartOf("Enemy_Stalchild") == nullptr);
    CHECK(PartOf("Enemy_DungeonBoss_BongoBongo") == nullptr);
    CHECK(IsBoss("Enemy_DungeonBoss_Volvagia"));
    CHECK(IsBoss("Enemy_MiddleBoss_DarkLink"));
    CHECK(IsBoss("Enemy_DeadHand"));
    CHECK(IsBoss("Enemy_KotakeKoumeBattleManager"));
    CHECK_FALSE(IsBoss("Enemy_DungeonBoss_Volvagia_Dictionary"));
    CHECK_FALSE(IsBoss("Enemy_Stalchild"));
    CHECK_FALSE(IsBoss("Enemy_Dodongo"));
}

TEST_CASE("objects: GroupsOf picks the root by name, then the nearest in reach") {
    using CoopObjects::GroupInput;
    const auto id = [](std::uint64_t v) { return proto::ObjectId{proto::ObjectKind::placed, 0, v}; };
    constexpr std::uint8_t kBoss = proto::object_group::boss;
    std::vector<GroupInput> walk{
        {id(1), "Enemy_DungeonBoss_BongoBongo", {0, 0, 0}},
        {id(2), "Enemy_DungeonBoss_BongoBongo_LeftHand", {500, 0, 0}},  // any reach
        {id(3), "Enemy_Dodongo", {0, 0, 0}},
        {id(4), "Enemy_Dodongo", {20, 0, 0}},
        {id(5), "Enemy_Dodongo_Attention", {19, 0, 0}},  // nearest: 4
        {id(6), "Enemy_Dodongo_Attention", {200, 0, 0}},  // none within 10 m, never linked: no entry
        {id(7), "Enemy_Stalchild", {0, 0, 0}},            // no group, no entry
    };
    auto groups = CoopObjects::GroupsOf(walk);
    REQUIRE(groups.size() == 3);
    CHECK(groups[0] == proto::ObjectGroupEntry{id(1), id(1), kBoss});
    CHECK(groups[1] == proto::ObjectGroupEntry{id(2), id(1), kBoss});
    CHECK(groups[2] == proto::ObjectGroupEntry{id(5), id(4), 0});
    // A part linked before keeps its root out to 1.25 x the reach (12.5 m)...
    std::map<proto::ObjectId, proto::ObjectId> linked{{id(5), id(4)}, {id(6), id(3)}};
    walk[4].position = {32, 0, 0};  // 12 m from 4, 4 is still the only one in 12.5 m
    groups = CoopObjects::GroupsOf(walk, linked);
    REQUIRE(groups.size() == 4);
    CHECK(groups[2] == proto::ObjectGroupEntry{id(5), id(4), 0});
    // ...and one whose root is out of reach goes on its own, once.
    CHECK(groups[3] == proto::ObjectGroupEntry{id(6), id(6), 0});
    walk[4].position = {33, 0, 0};  // 13 m: past 12.5, and no other root within 10 m
    groups = CoopObjects::GroupsOf(walk, linked);
    CHECK(groups[2] == proto::ObjectGroupEntry{id(5), id(5), 0});
    // The battle manager wins over Twinrova for the witches.
    walk = {
        {id(10), "Enemy_DungeonBoss_Twinrova", {0, 0, 0}},
        {id(11), "Enemy_DungeonBoss_Kotake", {0, 0, 0}},
        {id(12), "Enemy_KotakeKoumeBattleManager", {90, 0, 0}},
    };
    groups = CoopObjects::GroupsOf(walk);
    REQUIRE(groups.size() == 3);
    CHECK(groups[0] == proto::ObjectGroupEntry{id(10), id(12), kBoss});
    CHECK(groups[1] == proto::ObjectGroupEntry{id(11), id(12), kBoss});
    CHECK(groups[2] == proto::ObjectGroupEntry{id(12), id(12), kBoss});
    // Without the manager the witches follow Twinrova.
    walk.pop_back();
    groups = CoopObjects::GroupsOf(walk);
    REQUIRE(groups.size() == 2);
    CHECK(groups[1] == proto::ObjectGroupEntry{id(11), id(10), kBoss});
    // The entries pass the codec.
    proto::ObjectGroup g;
    g.entries = groups;
    const auto bytes = proto::encode(g);
    REQUIRE(bytes.has_value());
    proto::Message back;
    CHECK(proto::decode(*bytes, back) == proto::Error::none);
    CHECK(std::get<proto::ObjectGroup>(back) == g);
}

TEST_CASE("objects: OBJECT_GROUP follows the presence, on a change and with each presence") {
    Sync sync;
    std::vector<Seen> walk{Enemy(0x100, 0x11, 4, 0.0f, "Enemy_DeadHand"),
                           Enemy(0x200, 0x22, 1, 5.0f, "Enemy_DeadHand_Hand")};
    auto step = Tick(sync, 1000 * kMs, walk);
    // Presence first, then the group.
    REQUIRE(step.send.size() >= 2);
    std::size_t present_at = step.send.size(), group_at = step.send.size();
    for (std::size_t i = 0; i < step.send.size(); ++i) {
        if (std::holds_alternative<proto::ObjectPresent>(step.send[i])) present_at = i;
        if (std::holds_alternative<proto::ObjectGroup>(step.send[i])) group_at = i;
    }
    REQUIRE(group_at < step.send.size());
    CHECK(present_at < group_at);
    const auto group = All<proto::ObjectGroup>(step).at(0);
    REQUIRE(group.entries.size() == 2);
    const auto boss = sync.IdOf(0x100);
    const auto hand = sync.IdOf(0x200);
    REQUIRE(boss);
    REQUIRE(hand);
    for (const auto& e : group.entries) {
        CHECK(e.root == *boss);
        CHECK(e.flags == proto::object_group::boss);
    }
    // Nothing changed, no presence due: no group.
    step = Tick(sync, 1100 * kMs, walk);
    CHECK(All<proto::ObjectGroup>(step).empty());
    // With the next presence (2 s), again.
    step = Tick(sync, 3100 * kMs, walk);
    CHECK(All<proto::ObjectGroup>(step).size() == 1);
    // The boss leaves the walk: the hand follows nobody.
    walk.erase(walk.begin());
    bool alone = false;
    for (std::uint64_t t = 3200; t < 6000 && !alone; t += 100) {
        step = Tick(sync, t * kMs, walk);
        for (const auto& g : All<proto::ObjectGroup>(step)) {
            alone = alone || (g.entries.size() == 1 && g.entries[0].root == *hand);
        }
    }
    CHECK(alone);
    // Sent once: the next presence has no group left to send.
    for (std::uint64_t t = 6000; t < 9000; t += 100) {
        step = Tick(sync, t * kMs, walk);
        CHECK(All<proto::ObjectGroup>(step).empty());
    }
    // A walk with no group sends none.
    Sync plain;
    step = Tick(plain, 1000 * kMs, {Enemy(0x300, 0x33, 2)});
    CHECK(All<proto::ObjectGroup>(step).empty());
    // A stray part whose root is not loaded, never linked here: nothing
    // (no boss lock for it, and no undoing another game's link).
    Sync stray;
    step = Tick(stray, 1000 * kMs, {Enemy(0x400, 0x44, 1, 0.0f, "Enemy_Gohma_Larva")});
    CHECK(All<proto::ObjectGroup>(step).empty());
}

TEST_CASE("objects: OBJECT_OWNER is logged on a change, not on every repeat") {
    CoopObjects::OwnerLog log;
    proto::ObjectOwner a;
    a.id.value = 0x1234;
    a.owner = 2;
    a.epoch = 1;
    CHECK(log.Changed(a));
    CHECK_FALSE(log.Changed(a));  // the 10 s repeat
    CHECK_FALSE(log.Changed(a));
    proto::ObjectOwner b = a;
    b.id.value = 0x5678;
    CHECK(log.Changed(b));  // another object
    a.epoch = 2;
    CHECK(log.Changed(a));  // a new epoch
    a.owner = 1;
    CHECK(log.Changed(a));  // a new owner
    CHECK_FALSE(log.Changed(a));
    log.Clear();
    CHECK(log.Changed(a));
}

// Issue 101: an enemy that chases a player from afar in that player's game.
namespace {

// Runs a copy at x from `from` ms to `to` ms: each 33 ms the game's frame
// pulls it `step` metres towards x = 0 from where the Mover put it, each
// 66 ms a walk. The flags of the last OBJECT_PRESENT sent, if any.
std::optional<std::uint8_t> Drift(Sync& sync, std::uint64_t from, std::uint64_t to, float x,
                                  float step, std::uint8_t self = 1) {
    std::optional<std::uint8_t> flags;
    for (std::uint64_t t = from; t < to; t += 33) {
        sync.NoteDrift(0x100, {x, 0.0f, 0.0f}, {x - step, 0.0f, 0.0f}, t * kMs);
        if ((t - from) % 66 == 0) {
            for (const auto& p : All<proto::ObjectPresent>(
                     Tick(sync, t * kMs, {Enemy(0x100, 0x11, 2, x, "Enemy_Peahat_Larva_Independent")},
                          self))) {
                REQUIRE(p.objects.size() == 1);
                flags = p.objects[0].flags;
            }
        }
    }
    return flags;
}

constexpr std::uint8_t kTargeting = proto::object_presence::targeting;

} // namespace

TEST_CASE("objects: a copy whose own AI pulls it at our Link for 1 s is flagged as chasing us") {
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
    Tick(sync, 0, {Enemy(0x100, 0x11, 2, 20.0f, "Enemy_Peahat_Larva_Independent")});
    Own(sync, *sync.IdOf(0x100), 2);
    Tick(sync, 1 * kMs, {Enemy(0x100, 0x11, 2, 20.0f, "Enemy_Peahat_Larva_Independent")});

    // Under 1 s: nothing yet.
    auto flags = Drift(sync, 10, 900, 20.0f, 0.1f);
    CHECK_FALSE(flags.has_value());
    flags = Drift(sync, 900, 1200, 20.0f, 0.1f);
    REQUIRE(flags);
    CHECK((*flags & kTargeting) != 0);
    // Held still again: the flag stays 2 s, then goes.
    flags = Drift(sync, 1200, 3000, 20.0f, 0.0f);
    CHECK_FALSE(flags.has_value());  // no change, no repeat within 2 s
    flags = Drift(sync, 3000, 3600, 20.0f, 0.0f);
    REQUIRE(flags);
    CHECK((*flags & kTargeting) == 0);
}

TEST_CASE("objects: no chase when the copy goes away, is too far, ours, or the run has gaps") {
    auto run = [](float x, float step, std::uint8_t owner) {
        Sync sync;
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
        Tick(sync, 0, {Enemy(0x100, 0x11, 2, x, "Enemy_Peahat_Larva_Independent")});
        Own(sync, *sync.IdOf(0x100), owner);
        const auto flags = Drift(sync, 10, 3000, x, step);
        return flags && (*flags & kTargeting) != 0;
    };
    CHECK(run(20.0f, 0.1f, 2));        // the chase itself
    CHECK_FALSE(run(20.0f, -0.1f, 2)); // pulled away from our Link
    CHECK_FALSE(run(45.0f, 0.1f, 2));  // over 40 m away
    CHECK_FALSE(run(20.0f, 0.01f, 2)); // under 2 cm a frame: the Mover's own noise
    CHECK_FALSE(run(20.0f, 0.1f, 1));  // ours: the Mover does not place it

    // A step every 500 ms is no run.
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
    Tick(sync, 0, {Enemy(0x100, 0x11, 2, 20.0f, "Enemy_Peahat_Larva_Independent")});
    Own(sync, *sync.IdOf(0x100), 2);
    for (std::uint64_t t = 10; t < 3000; t += 500) {
        sync.NoteDrift(0x100, {20.0f, 0.0f, 0.0f}, {19.0f, 0.0f, 0.0f}, t * kMs);
    }
    const auto present =
        All<proto::ObjectPresent>(Tick(sync, 3000 * kMs, {Enemy(0x100, 0x11, 2, 20.0f, "Enemy_Peahat_Larva_Independent")}));
    for (const auto& p : present) {
        CHECK((p.objects[0].flags & kTargeting) == 0);
    }
}

TEST_CASE("objects: an enemy of ours that comes at our Link is flagged too (a fight to the server)") {
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
    Tick(sync, 0, {Enemy(0x100, 0x11, 2, 20.0f)});
    Own(sync, *sync.IdOf(0x100), 1);
    std::optional<std::uint8_t> flags;
    float x = 20.0f;
    for (std::uint64_t t = 66; t <= 1500; t += 66) {
        x -= 0.2f;  // 3 m/s towards our Link
        for (const auto& p : All<proto::ObjectPresent>(Tick(sync, t * kMs, {Enemy(0x100, 0x11, 2, x)}))) {
            flags = p.objects[0].flags;
        }
    }
    REQUIRE(flags);
    CHECK((*flags & kTargeting) != 0);

    // Walking away is no chase.
    Sync away;
    away.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
    Tick(away, 0, {Enemy(0x100, 0x11, 2, 20.0f)});
    Own(away, *away.IdOf(0x100), 1);
    x = 20.0f;
    for (std::uint64_t t = 66; t <= 3000; t += 66) {
        x += 0.2f;
        for (const auto& p : All<proto::ObjectPresent>(Tick(away, t * kMs, {Enemy(0x100, 0x11, 2, x)}))) {
            CHECK((p.objects[0].flags & kTargeting) == 0);
        }
    }
}
