// Tests for the shared enemies' hook side (lichii/hook/coop_objects_sync.cpp).
// No emulator code: each walk is a list of actors.
#include <doctest/doctest.h>

#include <ostream>  // doctest prints string_view through it

#include <cstring>
#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_hold.h"
#include "coop_objects_sync.h"

namespace proto = coop::protocol;
using CoopObjects::LifeWrite;
using CoopObjects::PlayWish;
using CoopObjects::Seen;
using CoopObjects::Sync;
using CoopObjects::IsLateEncounter;
using CoopObjects::kEncounterNearM;
using CoopObjects::kEncounterWaitUs;
using CoopObjects::kEncounterStaleUs;

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

TEST_CASE("objects: a MaxLife raised by another player raises our Link's max life (issue 88)") {
    using CoopObjects::RaisedMaxLife;
    CHECK(RaisedMaxLife(16, 20) == 20);  // a heart container: 4 hearts -> 5
    CHECK(RaisedMaxLife(16, 80) == 80);  // 20 hearts, the most
    CHECK_FALSE(RaisedMaxLife(16, 16).has_value());  // the same: nothing to write
    CHECK_FALSE(RaisedMaxLife(20, 16).has_value());  // never lowered
    CHECK_FALSE(RaisedMaxLife(16, 84).has_value());  // over 20 hearts: not a real value
    CHECK_FALSE(RaisedMaxLife(16, -1).has_value());  // the flag not readable
    CHECK_FALSE(RaisedMaxLife(0, 20).has_value());   // our Link's life not read yet
    CHECK_FALSE(RaisedMaxLife(-1, 20).has_value());
}

TEST_CASE("objects: the host's older save lowers our Link's max life only while its reset hold lasts (rows 136/137)") {
    using CoopObjects::LoweredMaxLife;
    // 16 -> 12 (a container taken, then the host loaded a save from before the boss): life held to the new max.
    const auto a = LoweredMaxLife(16, 16, 12);
    REQUIRE(a.has_value());
    CHECK(a->max == 12);
    CHECK(a->life == 12);
    const auto b = LoweredMaxLife(16, 8, 12);  // hurt: life stays
    REQUIRE(b.has_value());
    CHECK(b->max == 12);
    CHECK_FALSE(b->life.has_value());
    CHECK_FALSE(LoweredMaxLife(16, 16, std::nullopt).has_value());  // no hold: never lowered (co-op idle, solo)
    CHECK_FALSE(LoweredMaxLife(12, 12, 12).has_value());            // already there
    CHECK_FALSE(LoweredMaxLife(12, 12, 16).has_value());            // never raised here (RaisedMaxLife does)
    CHECK_FALSE(LoweredMaxLife(16, 16, 0).has_value());             // not a real maximum
    CHECK_FALSE(LoweredMaxLife(16, 16, 3).has_value());
    CHECK_FALSE(LoweredMaxLife(0, 0, 12).has_value());              // our Link's life not read yet
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

TEST_CASE("objects: a boss copy's hit place goes with its DAMAGE, and the owner's replay lands there (boss-intro)") {
    // Gohma's weak point is a headshot check against her Eyelid_Down bone: a non-owner's finishing hit must land
    // at her eye in the owner's game, not at her feet.
    const proto::Vec3 eye{18.5f, 61.5f, 67.8f};
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync copy;
    Tick(copy, 0, {Enemy(0x100, 0x11, 2, 0.0f, kBoss)});
    const proto::ObjectId id = *copy.IdOf(0x100);
    Own(copy, id, 2, 1);
    Tick(copy, 10 * kMs, {Enemy(0x100, 0x11, 2, 0.0f, kBoss)});
    Seen hit = Enemy(0x100, 0x11, 0, 0.0f, kBoss);
    hit.hit_at = eye;
    Tick(copy, 20 * kMs, {hit});
    auto damage = All<proto::Damage>(Tick(copy, 130 * kMs, {Enemy(0x100, 0x11, 0, 0.0f, kBoss)}));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].hit_at == std::optional<proto::Vec3>(eye));

    Sync owner;
    owner.SetHurt(true);
    Tick(owner, 0, {Enemy(0x200, 0x11, 2, 0.0f, kBoss)});
    const proto::ObjectId oid = *owner.IdOf(0x200);
    Own(owner, oid, 1);
    proto::Damage d = damage[0];
    d.player_number = 2;
    d.id = oid;
    owner.Receive(d);
    auto step = Tick(owner, 140 * kMs, {Enemy(0x200, 0x11, 2, 0.0f, kBoss)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].hurt == 2);
    CHECK(step.writes[0].hit_at == std::optional<proto::Vec3>(eye));
    // The boss's game took 1 of the replay (it holds at 1): no life is written for a boss, ever; the rest goes
    // to its own Life step (boss-hits), which clamps it as the game does.
    step = Tick(owner, 700 * kMs, {Enemy(0x200, 0x11, 1, 0.0f, kBoss)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].direct);
    CHECK(step.writes[0].hurt == 1);
    CHECK(owner.BossHitsStepped() == 1);
}

TEST_CASE("objects: another player's hit our boss refused as a replay goes to its own Life step (boss-hits)") {
    // A player's log as Gohma's owner: 23 DAMAGE from the other players, no replay taken ("did not take"). The
    // hitter's game judged the hit on its copy; the owner's game takes the amount through its Life step.
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync owner;
    owner.SetHurt(true);
    Tick(owner, 0, {Enemy(0x200, 0x11, 21, 0.0f, kBoss)});
    const proto::ObjectId oid = *owner.IdOf(0x200);
    Own(owner, oid, 1);
    proto::Damage d;
    d.player_number = 2;
    d.id = oid;
    d.amount = 2;
    owner.Receive(d);
    auto step = Tick(owner, 100 * kMs, {Enemy(0x200, 0x11, 21, 0.0f, kBoss)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].hurt == 2);  // the replay first (her reaction where her game takes it)
    CHECK_FALSE(step.writes[0].direct);
    CHECK(Tick(owner, 300 * kMs, {Enemy(0x200, 0x11, 21, 0.0f, kBoss)}).writes.empty());
    // Refused: after the wait the whole 2 goes to her Life step, once.
    step = Tick(owner, 600 * kMs, {Enemy(0x200, 0x11, 21, 0.0f, kBoss)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].direct);
    CHECK(step.writes[0].hurt == 2);
    CHECK(step.writes[0].life == 19);
    CHECK(step.writes[0].address == 0x200);
    // Taken: 21 -> 19 is no hit of ours (nothing sent), and nothing more is given.
    step = Tick(owner, 700 * kMs, {Enemy(0x200, 0x11, 19, 0.0f, kBoss)});
    CHECK(step.writes.empty());
    CHECK(All<proto::Damage>(step).empty());
    CHECK(Tick(owner, 1500 * kMs, {Enemy(0x200, 0x11, 19, 0.0f, kBoss)}).writes.empty());
    CHECK(owner.BossHitsStepped() == 1);

    // A replay her game takes (the eye in her own chance time): nothing more given.
    d.amount = 1;
    owner.Receive(d);
    step = Tick(owner, 1600 * kMs, {Enemy(0x200, 0x11, 19, 0.0f, kBoss)});
    REQUIRE(step.writes.size() == 1);
    CHECK_FALSE(step.writes[0].direct);
    Tick(owner, 1700 * kMs, {Enemy(0x200, 0x11, 18, 0.0f, kBoss)});
    CHECK(Tick(owner, 2200 * kMs, {Enemy(0x200, 0x11, 18, 0.0f, kBoss)}).writes.empty());
    CHECK(owner.BossHitsStepped() == 1);
}

TEST_CASE("objects: a boss copy's refused owner hit is still never given here (the owner judges)") {
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync copy;
    copy.SetHurt(true);
    Tick(copy, 0, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    const proto::ObjectId id = *copy.IdOf(0x100);
    Own(copy, id, 2, 1);
    proto::ObjectSample s;
    s.player_number = 2;
    s.id = id;
    s.epoch = 1;
    s.state.life = proto::ObjectLife{19, 21};
    copy.Receive(s);
    auto step = Tick(copy, 100 * kMs, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    REQUIRE(step.writes.size() == 1);
    CHECK_FALSE(step.writes[0].direct);
    step = Tick(copy, 600 * kMs, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    CHECK(step.writes.empty());
    CHECK(copy.BossHitsStepped() == 0);
}

namespace {
proto::ObjectSample LifeSample(const proto::ObjectId& id, std::int32_t life, std::int32_t max) {
    proto::ObjectSample s;
    s.player_number = 2;
    s.id = id;
    s.epoch = 1;
    s.state.life = proto::ObjectLife{life, max};
    return s;
}
}  // namespace

TEST_CASE("objects: our own hit on a boss copy while the owner's refused replay waits is sent, not booked as it") {
    // Probe boss-handover 20261117-114108: Gohma's copy in A's game waited all fight on B's replayed hit (her game
    // refuses it, the next sample asks again); A's own hits fell into "our own hurt landing" and 2 of 3 were never
    // sent. Our game's own hit moves the Life component's last hit id to one we did not replay (Seen::own_hit).
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    for (const bool own : {true, false}) {
        Sync copy;
        copy.SetHurt(true);
        Tick(copy, 0, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
        const proto::ObjectId id = *copy.IdOf(0x100);
        Own(copy, id, 2, 1);
        copy.Receive(LifeSample(id, 18, 21));
        REQUIRE(Tick(copy, 100 * kMs, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)}).writes.size() == 1);  // the replay
        Seen hit = Enemy(0x100, 0x11, 19, 0.0f, kBoss);
        hit.own_hit = own;
        Tick(copy, 150 * kMs, {hit});
        const auto damage = All<proto::Damage>(Tick(copy, 300 * kMs, {Enemy(0x100, 0x11, 19, 0.0f, kBoss)}));
        if (!own) {
            CHECK(damage.empty());  // (no hit id of ours moved: the fall is the replay landing, as before)
            continue;
        }
        REQUIRE(damage.size() == 1);
        CHECK(damage[0].amount == 2);
        CHECK(copy.OwnHitsKept() == 1);
        // The owner takes it (18 - 2): no new replay is asked for our own hit.
        copy.Receive(LifeSample(id, 16, 21));
        CHECK(Tick(copy, 350 * kMs, {Enemy(0x100, 0x11, 19, 0.0f, kBoss)}).writes.empty());
    }
}

TEST_CASE("objects: an enemy copy keeps our own hit made while a replay waits, also in the late window") {
    for (const bool same_walk : {false, true}) {
        Sync copy;
        copy.SetHurt(true);
        Tick(copy, 0, {Enemy(0x100, 0x11, 10)});
        const proto::ObjectId id = *copy.IdOf(0x100);
        Own(copy, id, 2, 1);
        copy.Receive(LifeSample(id, 7, 10));
        REQUIRE(Tick(copy, 100 * kMs, {Enemy(0x100, 0x11, 10)}).writes.size() == 1);  // the replay of 3
        // Our hit of 2, alone or in the same walk as the replay landing (its id reached too).
        Seen hit = Enemy(0x100, 0x11, same_walk ? 5 : 8);
        hit.own_hit = true;
        hit.replay_hit = same_walk;
        Tick(copy, 150 * kMs, {hit});
        const auto damage = All<proto::Damage>(Tick(copy, 300 * kMs, {Enemy(0x100, 0x11, same_walk ? 5 : 8)}));
        REQUIRE(damage.size() == 1);
        CHECK(damage[0].amount == 2);
        if (!same_walk) {
            // The replay never landed: at its end the life is the owner's with our hit in it (10 - 3 - 2).
            const auto end = Tick(copy, 600 * kMs, {Enemy(0x100, 0x11, 8)});
            REQUIRE(end.writes.size() == 1);
            CHECK(end.writes[0].life == 5);
        }
    }
    // The late window (the game may still take the written replay): our own hit there is ours, not put back.
    Sync copy;
    copy.SetHurt(true);
    Tick(copy, 0, {Enemy(0x100, 0x11, 10)});
    const proto::ObjectId id = *copy.IdOf(0x100);
    Own(copy, id, 2, 1);
    copy.Receive(LifeSample(id, 7, 10));
    Tick(copy, 100 * kMs, {Enemy(0x100, 0x11, 10)});
    // The replay was never taken: 7 given at its end, the late window open; its game keeps 10 for now.
    REQUIRE(Tick(copy, 600 * kMs, {Enemy(0x100, 0x11, 10)}).writes.size() == 1);
    Seen hit = Enemy(0x100, 0x11, 8);
    hit.own_hit = true;
    Tick(copy, 700 * kMs, {hit});
    const auto damage = All<proto::Damage>(Tick(copy, 850 * kMs, {Enemy(0x100, 0x11, 8)}));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].amount == 2);  // (without own_hit: booked as the late take and lost)
}

TEST_CASE("objects: our own hit and the late replay taken in the same walk: only ours is sent (late window)") {
    // review-a38-3 on 6bcc6066: in the late window (hurt_left 0, late_left > 0) the game took the given-up replay
    // late in the same walk as our own hit (own_hit and replay_hit): the whole fall went out as ours. Only a
    // reacting enemy has a late window (a pot, a crate or an animal gets its life written at once: Sync::Lower).
    Sync copy;
    copy.SetHurt(true);
    Tick(copy, 0, {Enemy(0x100, 0x11, 10)});
    const proto::ObjectId id = *copy.IdOf(0x100);
    Own(copy, id, 2, 1);
    copy.Receive(LifeSample(id, 7, 10));
    Tick(copy, 100 * kMs, {Enemy(0x100, 0x11, 10)});
    // The replay was never taken: 7 given at its end, the late window open; its game still reads 10.
    REQUIRE(Tick(copy, 600 * kMs, {Enemy(0x100, 0x11, 10)}).writes.size() == 1);
    // Now the game takes the late 3 and our own hit of 2 in one walk: 10 -> 5.
    Seen hit = Enemy(0x100, 0x11, 5);
    hit.own_hit = true;
    hit.replay_hit = true;
    CHECK(Tick(copy, 700 * kMs, {hit}).writes.empty());  // an enemy keeps what its game took (never written up)
    const auto damage = All<proto::Damage>(Tick(copy, 850 * kMs, {Enemy(0x100, 0x11, 5)}));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].amount == 2);  // (was 5: the late take sent as ours, the owner hit twice)
}

TEST_CASE("objects: our own hit on our boss while a hitter's refused DAMAGE waits keeps both") {
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync owner;
    owner.SetHurt(true);
    Tick(owner, 0, {Enemy(0x200, 0x11, 20, 0.0f, kBoss)});
    const proto::ObjectId oid = *owner.IdOf(0x200);
    Own(owner, oid, 1);
    proto::Damage d;
    d.player_number = 2;
    d.id = oid;
    d.amount = 3;
    owner.Receive(d);
    REQUIRE(Tick(owner, 100 * kMs, {Enemy(0x200, 0x11, 20, 0.0f, kBoss)}).writes.size() == 1);  // the replay
    Seen hit = Enemy(0x200, 0x11, 18, 0.0f, kBoss);
    hit.own_hit = true;
    Tick(owner, 150 * kMs, {hit});
    // Her game refused the replay: the hitter's 3 goes to her own Life step on top of our 2 (20 - 2 - 3).
    const auto step = Tick(owner, 600 * kMs, {Enemy(0x200, 0x11, 18, 0.0f, kBoss)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].direct);
    CHECK(step.writes[0].life == 15);
    CHECK(step.writes[0].hurt == 3);
}

TEST_CASE("objects: the test hit stands in for our game's own hit (TestOwnHit)") {
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync copy;
    copy.SetHurt(true);
    Tick(copy, 0, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    const proto::ObjectId id = *copy.IdOf(0x100);
    Own(copy, id, 2, 1);
    copy.Receive(LifeSample(id, 18, 21));
    Tick(copy, 100 * kMs, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    copy.TestOwnHit(0x100, 120 * kMs);
    Tick(copy, 150 * kMs, {Enemy(0x100, 0x11, 19, 0.0f, kBoss)});
    const auto damage = All<proto::Damage>(Tick(copy, 300 * kMs, {Enemy(0x100, 0x11, 19, 0.0f, kBoss)}));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].amount == 2);
}

TEST_CASE("objects: a hit place never rides on a later DAMAGE (seen while ours, or an older sum)") {
    const proto::Vec3 eye{18.5f, 61.5f, 67.8f};
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 4, 0.0f, kBoss)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 1);  // ours: our game's own hit at the eye, nothing to send
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 4, 0.0f, kBoss)});
    Seen ours = Enemy(0x100, 0x11, 3, 0.0f, kBoss);
    ours.hit_at = eye;
    Tick(sync, 20 * kMs, {ours});
    Own(sync, id, 2, 1);  // handed over: now a copy
    Tick(sync, 30 * kMs, {Enemy(0x100, 0x11, 3, 0.0f, kBoss)});
    Tick(sync, 40 * kMs, {Enemy(0x100, 0x11, 2, 0.0f, kBoss)});  // a plain drop, no place this walk
    auto damage = All<proto::Damage>(Tick(sync, 150 * kMs, {Enemy(0x100, 0x11, 2, 0.0f, kBoss)}));
    REQUIRE(damage.size() == 1);
    CHECK_FALSE(damage[0].hit_at);
    // A place with one sum, then a later sum without one: the second goes without.
    Seen at = Enemy(0x100, 0x11, 1, 0.0f, kBoss);
    at.hit_at = eye;
    Tick(sync, 160 * kMs, {at});
    damage = All<proto::Damage>(Tick(sync, 270 * kMs, {Enemy(0x100, 0x11, 1, 0.0f, kBoss)}));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].hit_at == std::optional<proto::Vec3>(eye));
    Tick(sync, 280 * kMs, {Enemy(0x100, 0x11, 0, 0.0f, kBoss)});
    damage = All<proto::Damage>(Tick(sync, 400 * kMs, {Enemy(0x100, 0x11, 0, 0.0f, kBoss)}));
    REQUIRE(damage.size() == 1);
    CHECK_FALSE(damage[0].hit_at);
}

TEST_CASE("objects: a hit place read a walk before its life drop still goes with it, not later (review)") {
    const proto::Vec3 eye{18.5f, 61.5f, 67.8f};
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 4, 0.0f, kBoss)});
    Own(sync, *sync.IdOf(0x100), 2, 1);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 4, 0.0f, kBoss)});
    Seen id_moved = Enemy(0x100, 0x11, 4, 0.0f, kBoss);  // the hit id moved, the life not yet
    id_moved.hit_at = eye;
    Tick(sync, 20 * kMs, {id_moved});
    Tick(sync, 40 * kMs, {Enemy(0x100, 0x11, 3, 0.0f, kBoss)});  // the drop, one walk later
    auto damage = All<proto::Damage>(Tick(sync, 150 * kMs, {Enemy(0x100, 0x11, 3, 0.0f, kBoss)}));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].hit_at == std::optional<proto::Vec3>(eye));
    // A place whose drop comes too late (past kHitPlaceKeepUs) goes with nothing.
    Seen old = Enemy(0x100, 0x11, 3, 0.0f, kBoss);
    old.hit_at = eye;
    Tick(sync, 160 * kMs, {old});
    Tick(sync, 500 * kMs, {Enemy(0x100, 0x11, 2, 0.0f, kBoss)});
    damage = All<proto::Damage>(Tick(sync, 620 * kMs, {Enemy(0x100, 0x11, 2, 0.0f, kBoss)}));
    REQUIRE(damage.size() == 1);
    CHECK_FALSE(damage[0].hit_at);
}

TEST_CASE("objects: whose boss it is stays known a while after the owner's boss left the walk (review boss-intro2)") {
    // The owner's Gohma is deleted by her own death event: her death must still be the owner's then.
    const std::array<std::string_view, 1> bodies{"Enemy_DungeonBoss_Gohma"};
    std::map<std::string, CoopObjects::BossOwnership, std::less<>> last;
    const std::vector<Seen> with{Enemy(0x100, 0x11, 1, 0.0f, "Enemy_DungeonBoss_Gohma")};
    const std::vector<std::uint64_t> ours{0x100};
    CHECK(CoopObjects::BossOurs(with, ours, bodies, 1'000'000, last) == std::optional(true));
    CHECK(CoopObjects::BossOurs({}, {}, bodies, 3'000'000, last) == std::optional(true));  // gone: the last reading
    CHECK_FALSE(CoopObjects::BossOurs({}, {}, bodies, 1'000'000 + CoopObjects::kBossOursKeepUs + 1, last));
    // A copy: false while seen, false after.
    std::map<std::string, CoopObjects::BossOwnership, std::less<>> copy;
    CHECK(CoopObjects::BossOurs(with, {}, bodies, 1'000'000, copy) == std::optional(false));
    CHECK(CoopObjects::BossOurs({}, {}, bodies, 2'000'000, copy) == std::optional(false));
    // Never seen: nothing.
    std::map<std::string, CoopObjects::BossOwnership, std::less<>> none;
    CHECK_FALSE(CoopObjects::BossOurs({}, {}, bodies, 1'000'000, none));
    // Ours, then handed to the other player while still seen: a copy from that walk on, also once she is gone.
    std::map<std::string, CoopObjects::BossOwnership, std::less<>> flip;
    CHECK(CoopObjects::BossOurs(with, ours, bodies, 1'000'000, flip) == std::optional(true));
    CHECK(CoopObjects::BossOurs(with, {}, bodies, 2'000'000, flip) == std::optional(false));
    CHECK(CoopObjects::BossOurs({}, {}, bodies, 3'000'000, flip) == std::optional(false));
    // And back (a stun hands her to us): ours again.
    CHECK(CoopObjects::BossOurs(with, ours, bodies, 4'000'000, flip) == std::optional(true));
}

TEST_CASE("objects: a hit place is kept only finite and near the boss") {
    const proto::Vec3 boss{18.0f, 60.0f, 67.0f};
    CHECK(CoopObjects::CheckedHitPlace({18.5f, 61.5f, 67.8f}, boss, 12.0f));
    CHECK_FALSE(CoopObjects::CheckedHitPlace({18.5f, 90.0f, 67.8f}, boss, 12.0f));
    CHECK_FALSE(CoopObjects::CheckedHitPlace({std::nanf(""), 61.0f, 67.0f}, boss, 12.0f));
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

TEST_CASE("objects: a copy takes the owner's life, but is never healed back up to it (native rule)") {
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
    // Long after, still no write: no game path raises an enemy's life, so the copy keeps its own.
    sync.Receive(s);
    step = Tick(sync, 1500 * kMs, {Enemy(0x100, 0x11, 2)});
    CHECK(step.writes.empty());
    CHECK(All<proto::Damage>(step).empty());
    // The owner's next fall below ours still comes down to the copy.
    s.state.life = proto::ObjectLife{1, 4};
    sync.Receive(s);
    step = Tick(sync, 1600 * kMs, {Enemy(0x100, 0x11, 2)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 1);
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
    // epona-ride: each game keeps its own ridable Epona (the ride sync shows the other's).
    CHECK_FALSE(CoopObjects::IsShared("Animal_Epona_Adult"));
    CHECK(CoopObjects::IsShared("Npc_Hylia_051"));
    CHECK_FALSE(CoopObjects::IsShared("CoopPuppet_Child"));
    CHECK_FALSE(CoopObjects::IsShared("Npc_Navi"));
    CHECK_FALSE(CoopObjects::IsShared("Npc_Equipment_002"));
    CHECK_FALSE(CoopObjects::IsShared("Npc_Invisible_Hylia_Dampe_001"));
}

TEST_CASE("objects: the enemies' brain path is opt-in: unset, empty or 0 is off (g4-enemies)") {
    // The first g4-enemies probe ran the path in its switch-off runs too: the switch read as on-unless-0.
    CHECK_FALSE(CoopObjects::EnemyBrainSwitch(""));
    CHECK_FALSE(CoopObjects::EnemyBrainSwitch("0"));
    CHECK(CoopObjects::EnemyBrainSwitch("1"));
}

TEST_CASE("objects: the enemies' brain path takes plain enemies only (fable-g4-enemies)") {
    CHECK(CoopObjects::OnBrainPath("Enemy_Dekubaba"));
    CHECK(CoopObjects::OnBrainPath("Enemy_Skulltula"));
    CHECK(CoopObjects::OnBrainPath("Enemy_Scrub_Deku"));
    // Bosses have their own entry; own puzzles, the picture book and everything that is no Enemy_ stay out.
    CHECK_FALSE(CoopObjects::OnBrainPath("Enemy_DungeonBoss_Gohma"));
    CHECK_FALSE(CoopObjects::OnBrainPath("Enemy_MiddleBoss_GerudoThief"));
    CHECK_FALSE(CoopObjects::OnBrainPath("Enemy_Scrub_Deku_Brothers"));
    CHECK_FALSE(CoopObjects::OnBrainPath("Enemy_Dekubaba_Dictionary"));
    CHECK_FALSE(CoopObjects::OnBrainPath("Enemy_MoblinClub_ShockWave"));  // an enemy's shot
    CHECK_FALSE(CoopObjects::OnBrainPath("Npc_Zora_Ruto_Child_001"));
}

TEST_CASE("objects: townspeople and every enemy copy are followed each game frame (jitter-b, issue 239)") {
    CHECK(CoopObjects::FollowedEachFrame("Npc_Zora_Ruto_Child_001"));
    CHECK(CoopObjects::FollowedEachFrame("Enemy_Dekubaba"));
    CHECK(CoopObjects::FollowedEachFrame("Enemy_Peahat"));
    CHECK(CoopObjects::FollowedEachFrame("Enemy_MiddleBoss_GerudoThief"));
    CHECK(CoopObjects::FollowedEachFrame("Enemy_DungeonBoss_Gohma"));
    CHECK(CoopObjects::FollowedEachFrame("Enemy_DungeonBoss_BongoBongo_LeftHand"));  // a part with its root
    CHECK_FALSE(CoopObjects::FollowedEachFrame("Enemy_Dekubaba_Dictionary"));
    CHECK_FALSE(CoopObjects::FollowedEachFrame("Obj_Kibako"));
    CHECK_FALSE(CoopObjects::FollowedEachFrame(""));
}

TEST_CASE("objects: order-puzzle scrubs are each game's own (issue 149)") {
    CHECK_FALSE(CoopObjects::IsShared("Enemy_Scrub_Deku_Brothers"));
    CHECK(CoopObjects::IsOwnPuzzle("Enemy_Scrub_Deku_Brothers"));
    CHECK_FALSE(CoopObjects::IsShared("Enemy_Scrub_ForestStage"));
    // The lone scrubs stay shared enemies.
    CHECK(CoopObjects::IsShared("Enemy_Scrub_Deku"));
    CHECK_FALSE(CoopObjects::IsOwnPuzzle("Enemy_Scrub_Deku"));
}

TEST_CASE("objects: Business Scrubs are each game's own shop (GitHub 253)") {
    for (const char* name : {"Enemy_Scrub_Business", "Enemy_Scrub_Business_Leaf", "Enemy_Scrub_Business_Seed"}) {
        CAPTURE(name);
        CHECK_FALSE(CoopObjects::IsShared(name));
        CHECK(CoopObjects::IsOwnPuzzle(name));
    }
    // Other scrubs and enemies are not caught by the prefix.
    CHECK(CoopObjects::IsShared("Enemy_Scrub_Mad"));
    CHECK(CoopObjects::IsShared("Enemy_Scrub_Deku"));
}

TEST_CASE("objects: Jabu-Jabu's child Ruto is each game's own, so a carried Ruto is never put down by a copy (#64)") {
    CHECK(CoopObjects::IsOwnPuzzle("Npc_Zora_Ruto_Child_001"));
    CHECK_FALSE(CoopObjects::IsShared("Npc_Zora_Ruto_Child_001"));
    // Other Zoras, and adult Ruto, stay shared NPCs.
    CHECK(CoopObjects::IsShared("Npc_Zora_ZU_001"));
    CHECK(CoopObjects::IsShared("Npc_Zora_Ruto_Adult_001"));
    CHECK_FALSE(CoopObjects::IsOwnPuzzle("Npc_Zora_Ruto_Adult_001"));
}

TEST_CASE("objects: the pond's owner is each game's own, its fish are shared (issue 151)") {
    for (const char* own : {"Npc_Hylia_Fishman_001_Child", "Npc_Hylia_Fishman_001_Adult"}) {
        CHECK_MESSAGE(CoopObjects::IsOwnPuzzle(own), own);
        CHECK_FALSE_MESSAGE(CoopObjects::IsShared(own), own);
    }
    for (const char* fish : {"Animal_Fish_Bass", "Animal_Fish_Loach", "Animal_Fish_Blue",
                             "Animal_Fish_Blue_TargetAttention"}) {
        CHECK_MESSAGE(CoopObjects::IsShared(fish), fish);
        CHECK_MESSAGE(CoopObjects::IsFish(fish), fish);
        CHECK_FALSE_MESSAGE(CoopObjects::IsOwnPuzzle(fish), fish);
    }
    CHECK_FALSE(CoopObjects::IsFish("Animal_Cucco"));
    CHECK_FALSE(CoopObjects::IsFish("Item_Bottle_Fish"));
    // Other animals and townspeople stay shared.
    CHECK(CoopObjects::IsShared("Animal_Cucco"));
    CHECK(CoopObjects::IsShared("Npc_Hylia_051"));
    // The rod, lure, hat and sinking lures were never shared.
    for (const char* never : {"Weapon_FishingRod", "Weapon_Lure", "Obj_SinkingLure", "Obj_Hat_FishingPondOwner"}) {
        CHECK_FALSE_MESSAGE(CoopObjects::IsShared(never), never);
    }
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

// Hookshot, patch 0007: this game's hits on a copy come from the game's own
// Life step (Sync::GameHit), not from the life read falling.
namespace {
std::int32_t DamageSent(const Step& step) {
    std::int32_t sum = 0;
    for (const auto& d : All<proto::Damage>(step)) {
        sum += d.amount;
    }
    return sum;
}
} // namespace

TEST_CASE("objects: life events: the Life step's hit on a copy goes to the owner, whichever comes first") {
    Sync sync;
    sync.SetLifeEvents(true);
    Tick(sync, 0, {Enemy(0x100, 0x11, 6)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2, 1);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 6)});

    // The hit and its fall in the same walk.
    sync.GameHit(0x100, 1);
    CHECK(DamageSent(Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 5)})) == 0);
    CHECK(DamageSent(Tick(sync, 130 * kMs, {Enemy(0x100, 0x11, 5)})) == 1);
    // The hit one walk before the life read shows it: counted once.
    sync.GameHit(0x100, 2);
    std::int32_t sent = DamageSent(Tick(sync, 200 * kMs, {Enemy(0x100, 0x11, 5)}));
    sent += DamageSent(Tick(sync, 220 * kMs, {Enemy(0x100, 0x11, 3)}));
    sent += DamageSent(Tick(sync, 400 * kMs, {Enemy(0x100, 0x11, 3)}));
    CHECK(sent == 2);
    // The fall read first, its hit drained one walk later: counted once, at the hit.
    sent = DamageSent(Tick(sync, 500 * kMs, {Enemy(0x100, 0x11, 2)}));
    sync.GameHit(0x100, 1);
    sent += DamageSent(Tick(sync, 520 * kMs, {Enemy(0x100, 0x11, 2)}));
    sent += DamageSent(Tick(sync, 700 * kMs, {Enemy(0x100, 0x11, 2)}));
    sent += DamageSent(Tick(sync, 1200 * kMs, {Enemy(0x100, 0x11, 2)}));
    CHECK(sent == 1);
    CHECK(sync.LooseDrops() == 0);
    // A hit for more than it has left: only what it had goes.
    sync.GameHit(0x100, 9);
    sent = DamageSent(Tick(sync, 1300 * kMs, {Enemy(0x100, 0x11, 0)}));
    sent += DamageSent(Tick(sync, 1500 * kMs, {Enemy(0x100, 0x11, 0)}));
    CHECK(sent == 2);
}

TEST_CASE("objects: an actor the game itself deleted is gone at the first walk without it") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2), Enemy(0x200, 0x22, 2, 5.0f)});
    REQUIRE(sync.IdOf(0x100));
    REQUIRE(sync.IdOf(0x200));
    sync.GameDeleted(0x100, 10 * kMs);  // (the game's delete request, patch 0007)
    sync.GameDeleted(0x999, 10 * kMs);  // (an actor we do not share: nothing)
    Tick(sync, 40 * kMs, {});
    CHECK_FALSE(sync.IdOf(0x100));
    CHECK(sync.IdOf(0x200));  // off the list for one walk only: lifted, or changing hands
    Tick(sync, 80 * kMs, {});
    CHECK_FALSE(sync.IdOf(0x200));
    // Long after the request the address is another actor's: two walks again.
    Tick(sync, 100 * kMs, {Enemy(0x300, 0x33, 2)});
    sync.GameDeleted(0x300, 100 * kMs);
    Tick(sync, 20'000 * kMs, {Enemy(0x300, 0x33, 2)});
    Tick(sync, 20'040 * kMs, {});
    CHECK(sync.IdOf(0x300));
}

namespace {
bool Listed(const Step& step, const proto::ObjectId& id) {
    for (const auto& p : All<proto::ObjectPresent>(step)) {
        for (const auto& o : p.objects) {
            if (o.id == id) {
                return true;
            }
        }
    }
    return false;
}
} // namespace

// churn-b: the game takes live actors off its list for a moment (adult
// Kakariko's townspeople and cuccos, hundreds of times in 4 minutes); with
// the game's delete requests known (Hookshot) such an actor is away, not
// gone, so the server never hands it to another game for it.
TEST_CASE("objects: delete events: an actor off the walk with no delete request keeps its id listed and its owner") {
    const std::string npc = "Npc_Hylia_019_Shop_Child";
    Sync sync;
    sync.SetDeleteEvents(true);
    Tick(sync, 0, {Enemy(0x100, 0x11, 100, 0.0f, npc), Enemy(0x200, 0x22, 2, 5.0f)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 1);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 100, 0.0f, npc), Enemy(0x200, 0x22, 2, 5.0f)});
    REQUIRE(sync.OwnerAt(0x100) == 1);

    // Off the list for several walks: away, still listed (a new enemy is the
    // change, so a presence goes out), its owner kept.
    Tick(sync, 40 * kMs, {Enemy(0x200, 0x22, 2, 5.0f)});
    const Step off = Tick(sync, 80 * kMs, {Enemy(0x200, 0x22, 2, 5.0f), Enemy(0x300, 0x33, 2, 9.0f)});
    CHECK(sync.AwayCount() == 1);
    CHECK(Listed(off, id));
    for (const auto& n : sync.TakeNotes()) {
        CHECK(n.find("Npc_Hylia_019_Shop_Child") == std::string::npos);  // no "gone ... left" line
    }
    // Back on the list: the same id, the same owner, nothing asked again.
    Tick(sync, 500 * kMs, {Enemy(0x100, 0x11, 100, 0.0f, npc)});
    CHECK(sync.AwayCount() == 0);
    CHECK(*sync.IdOf(0x100) == id);
    CHECK(sync.OwnerAt(0x100) == 1);

    // Off for longer than kAwayKeepUs: gone (left), no longer listed.
    Tick(sync, 600 * kMs, {});
    Tick(sync, 700 * kMs, {});
    CHECK(sync.AwayCount() == 1);
    const Step later = Tick(sync, 700 * kMs + Sync::kAwayKeepUs, {});
    CHECK(sync.AwayCount() == 0);
    CHECK_FALSE(Listed(later, id));
}

TEST_CASE("objects: delete events: a deleted, dead or unowned actor is gone, not away") {
    Sync sync;
    sync.SetDeleteEvents(true);
    Tick(sync, 0, {Enemy(0x100, 0x11, 2), Enemy(0x200, 0x22, 2, 5.0f), Enemy(0x300, 0x33, 2, 9.0f)});
    Own(sync, *sync.IdOf(0x100), 1);
    Own(sync, *sync.IdOf(0x200), 2);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2), Enemy(0x200, 0x22, 0, 5.0f), Enemy(0x300, 0x33, 2, 9.0f)});
    sync.GameDeleted(0x100, 20 * kMs);  // the game asked: gone at the first miss
    Tick(sync, 40 * kMs, {});
    Tick(sync, 80 * kMs, {});
    // 0x100 deleted, 0x200 dead (life 0), 0x300 with no owner yet: none is away.
    CHECK(sync.AwayCount() == 0);
    CHECK_FALSE(sync.IdOf(0x100));
    CHECK_FALSE(sync.IdOf(0x200));
    CHECK_FALSE(sync.IdOf(0x300));

    // A delete request that comes while it is away ends it at once.
    Tick(sync, 100 * kMs, {Enemy(0x400, 0x44, 2)});
    Own(sync, *sync.IdOf(0x400), 1);
    Tick(sync, 110 * kMs, {Enemy(0x400, 0x44, 2)});
    Tick(sync, 140 * kMs, {});
    Tick(sync, 180 * kMs, {});
    REQUIRE(sync.AwayCount() == 1);
    sync.GameDeleted(0x400, 190 * kMs);
    CHECK(sync.AwayCount() == 0);
}

TEST_CASE("objects: without delete events (the emulator) two missed walks still end an owned actor") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    Own(sync, *sync.IdOf(0x100), 1);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 40 * kMs, {});
    Tick(sync, 80 * kMs, {});
    CHECK(sync.AwayCount() == 0);
    CHECK_FALSE(sync.IdOf(0x100));
}

TEST_CASE("objects: life events: a hit that took no life is forgotten, a fall with no step still counts") {
    Sync sync;
    sync.SetLifeEvents(true);
    Tick(sync, 0, {Enemy(0x100, 0x11, 6)});
    Own(sync, *sync.IdOf(0x100), 2, 1);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 6)});
    // The step said 1 but the life stayed (the game's own guard): nothing goes, and a fall
    // long after it is not paired with it.
    sync.GameHit(0x100, 1);
    std::int32_t sent = 0;
    for (std::uint64_t t = 20; t <= 400; t += 20) {
        sent += DamageSent(Tick(sync, t * kMs, {Enemy(0x100, 0x11, 6)}));
    }
    CHECK(sent == 0);
    // Life gone with no step for it (something outside the Life step): a hit after the wait.
    sent = DamageSent(Tick(sync, 500 * kMs, {Enemy(0x100, 0x11, 4)}));
    sent += DamageSent(Tick(sync, 700 * kMs, {Enemy(0x100, 0x11, 4)}));
    CHECK(sent == 0);
    CHECK(sync.LooseDrops() == 0);
    sent += DamageSent(Tick(sync, 810 * kMs, {Enemy(0x100, 0x11, 4)}));
    sent += DamageSent(Tick(sync, 1000 * kMs, {Enemy(0x100, 0x11, 4)}));
    CHECK(sent == 2);
    CHECK(sync.LooseDrops() == 2);
}

TEST_CASE("objects: life events: our hit in the same frames as the owner's hurt is still ours") {
    Sync sync;
    sync.SetHurt(true);
    sync.SetLifeEvents(true);
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
    // The game takes the owner's hurt and our Link's own hit in one step: 2 gone, 1 of it ours.
    sync.GameHit(0x100, 1);
    std::int32_t sent = DamageSent(Tick(sync, 40 * kMs, {Enemy(0x100, 0x11, 2)}));
    sent += DamageSent(Tick(sync, 200 * kMs, {Enemy(0x100, 0x11, 2)}));
    sent += DamageSent(Tick(sync, 900 * kMs, {Enemy(0x100, 0x11, 2)}));
    CHECK(sent == 1);
    CHECK(sync.LooseDrops() == 0);
    CHECK(sync.HurtsTaken() == 1);  // (and its life is not written back up to the owner's 3)
    // The owner's hurt alone: nothing of ours.
    Sync other;
    other.SetHurt(true);
    other.SetLifeEvents(true);
    Tick(other, 0, {Enemy(0x100, 0x11, 4)});
    Own(other, id, 2);
    other.Receive(s);
    Tick(other, 10 * kMs, {Enemy(0x100, 0x11, 4)});
    sent = DamageSent(Tick(other, 40 * kMs, {Enemy(0x100, 0x11, 3)}));
    sent += DamageSent(Tick(other, 900 * kMs, {Enemy(0x100, 0x11, 3)}));
    CHECK(sent == 0);
    CHECK(other.HurtsTaken() == 1);
}

TEST_CASE("objects: life events: our own hit while a replay waits lowers its target once, by its step") {
    // review-a38-3 on 6bcc6066: the own-hit block lowered hurt_target by the fall no step explained yet, and the
    // step event lowered it again when it came: one hit of 2 took the target down by 4.
    Sync sync;
    sync.SetHurt(true);
    sync.SetLifeEvents(true);
    Tick(sync, 0, {Enemy(0x100, 0x11, 10)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2, 1);
    sync.Receive(LifeSample(id, 7, 10));
    REQUIRE(Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 10)}).writes.size() == 1);  // the hurt of 3
    // Our own hit of 2 read before its step event, the hurt not taken.
    Seen hit = Enemy(0x100, 0x11, 8);
    hit.own_hit = true;
    std::int32_t sent = DamageSent(Tick(sync, 40 * kMs, {hit}));
    sync.GameHit(0x100, 2);
    sent += DamageSent(Tick(sync, 60 * kMs, {Enemy(0x100, 0x11, 8)}));
    sent += DamageSent(Tick(sync, 200 * kMs, {Enemy(0x100, 0x11, 8)}));
    CHECK(sent == 2);
    // The hurt never taken: at its end the life to reach is the owner's with our hit in it (10 - 3 - 2), not 3.
    const auto end = Tick(sync, 600 * kMs, {Enemy(0x100, 0x11, 8)});
    REQUIRE(end.writes.size() == 1);
    CHECK(end.writes[0].life == 5);
}

TEST_CASE("objects: life events: life we put through the step ourselves is no hit of this game's") {
    Sync sync;
    sync.SetLifeEvents(true);
    Tick(sync, 0, {Enemy(0x100, 0x11, 5)});
    Own(sync, *sync.IdOf(0x100), 2, 1);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 5)});
    sync.Stepped(0x100, 2, 10 * kMs);  // (the owner's life, given to the Life step: no GameHit for it)
    std::int32_t sent = DamageSent(Tick(sync, 40 * kMs, {Enemy(0x100, 0x11, 3)}));
    sent += DamageSent(Tick(sync, 500 * kMs, {Enemy(0x100, 0x11, 3)}));
    sent += DamageSent(Tick(sync, 900 * kMs, {Enemy(0x100, 0x11, 3)}));
    CHECK(sent == 0);
    CHECK(sync.LooseDrops() == 0);
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

TEST_CASE("objects: a killing hit our game did not take leaves our enemy alive (native rule)") {
    // The big Skulltula hit from its armoured side in another game: our game
    // refuses the hurt. Nothing is written, so it is NOT said dead or deleted
    // while its game still has it alive.
    Sync sync;
    sync.SetHurt(true);
    const auto skull = [](std::int32_t life) { return Enemy(0x100, 0x11, life, 0.0f, "Enemy_Skulltula"); };
    Tick(sync, 0, {skull(4)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 1, 3);
    proto::Damage d;
    d.player_number = 2;
    d.id = id;
    d.amount = 4;
    sync.Receive(d);
    auto step = Tick(sync, 10 * kMs, {skull(4)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].hurt == 4);
    // Our samples tell the others only the life our game really has (never the target of a lost hit).
    std::size_t samples = 0;
    const auto real_life_only = [&samples](const Step& st) {
        for (const auto& sample : All<proto::ObjectSample>(st)) {
            if (sample.state.life) {
                ++samples;
                CHECK(sample.state.life->life == 4);
            }
        }
    };
    step = Tick(sync, 430 * kMs, {skull(4)});
    REQUIRE(step.writes.size() == 1);  // to the game's Life step (WriteLife), never a raw write
    CHECK(step.writes[0].hurt == 0);
    CHECK(step.writes[0].life == 0);
    CHECK(All<proto::ObjectRemove>(step).empty());
    real_life_only(step);
    // Its game did not take it: still alive at 4.
    for (const std::uint64_t t : {500, 1000, 1600, 3500, 4000}) {
        step = Tick(sync, t * kMs, {skull(4)});
        CHECK(All<proto::ObjectRemove>(step).empty());
        CHECK(sync.Removes().empty());  // (no dead_at: never deleted)
        real_life_only(step);
    }
    CHECK(samples > 0);
    // Nor is it said dead when it leaves the walk alive (a room change).
    for (const std::uint64_t t : {4100, 4200, 4300, 4400, 4500}) {
        for (const auto& r : All<proto::ObjectRemove>(Tick(sync, t * kMs, {}))) {
            CHECK(r.reason != proto::RemoveReason::dead);
        }
    }
}

TEST_CASE("objects: a killing hit given after the wait that its game takes is said dead and deleted (issue 133)") {
    Sync sync;
    sync.SetHurt(true);
    const auto skull = [](std::int32_t life) { return Enemy(0x100, 0x11, life, 0.0f, "Enemy_Skulltula"); };
    Tick(sync, 0, {skull(2)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 1, 3);
    proto::Damage d;
    d.player_number = 2;
    d.id = id;
    d.amount = 2;
    sync.Receive(d);
    Tick(sync, 10 * kMs, {skull(2)});
    auto step = Tick(sync, 430 * kMs, {skull(2)});
    CHECK(All<proto::ObjectRemove>(step).empty());  // not before its game took it
    // Its Life step took it: life 0, standing on (issue 133).
    step = Tick(sync, 500 * kMs, {skull(0)});
    auto removes = All<proto::ObjectRemove>(step);
    REQUIRE(removes.size() == 1);
    CHECK(removes[0].id == id);
    CHECK(removes[0].reason == proto::RemoveReason::dead);
    CHECK(removes[0].epoch == 3);
    // Off our presence list from then on (the server takes it as dropped).
    const auto presents = All<proto::ObjectPresent>(step);
    REQUIRE(presents.size() == 1);
    CHECK(presents[0].objects.empty());
    // Standing on at life 0: deleted 3 s later.
    Tick(sync, 3000 * kMs, {skull(0)});
    CHECK(sync.Removes().empty());
    Tick(sync, 3600 * kMs, {skull(0)});
    CHECK(sync.Removes() == std::vector<std::uint64_t>{0x100});
    // When it goes, it is not said dead a second time.
    Tick(sync, 3700 * kMs, {});
    CHECK(All<proto::ObjectRemove>(Tick(sync, 3800 * kMs, {})).empty());
}

TEST_CASE("objects: RouteLife: an enemy goes only through its game's own steps, else it is lost") {
    using CoopObjects::LifeAsk;
    using CoopObjects::LifeRoute;
    using CoopObjects::RouteLife;
    LifeAsk a;
    a.native = true;
    a.replay_on = true;
    a.has_player = true;
    a.hurt = 2;
    a.life = 1;
    a.old = 3;
    a.step_seen = true;
    a.has_component = true;
    CHECK(RouteLife(a).route == LifeRoute::replay);
    // The 400 ms fallback (no hurt, a life to reach): the Life step once the replay was seen.
    a.hurt = 0;
    auto r = RouteLife(a);
    CHECK(r.route == LifeRoute::step);
    CHECK(r.amount == 2);
    // A stale give is merged into the fall (not refused, not written).
    a.stale = 2;
    r = RouteLife(a);
    CHECK(r.route == LifeRoute::step);
    CHECK(r.amount == 2);
    // Our replay not seen yet by its damage step: nothing more.
    a.stale = 0;
    a.replay_seen = false;
    CHECK(RouteLife(a).route == LifeRoute::wait_replay);
    a.replay_seen = true;
    // No Life step (emulator), no component, nothing to lower, or unread: lost, never a write.
    a.step_seen = false;
    CHECK(RouteLife(a).route == LifeRoute::lost);
    a.step_seen = true;
    a.has_component = false;
    CHECK(RouteLife(a).route == LifeRoute::lost);
    a.has_component = true;
    a.life = 5;  // up: no game path
    CHECK(RouteLife(a).route == LifeRoute::lost);
    a.life = 1;
    a.old = -1;
    CHECK(RouteLife(a).route == LifeRoute::lost);
    a.old = 3;
    // A hurt past the replay (a test kill): its stale part goes with it.
    a.direct = true;
    a.hurt = 3;
    a.stale = 1;
    r = RouteLife(a);
    CHECK(r.route == LifeRoute::step);
    CHECK(r.amount == 4);
    // Not native (a pot): a stale give means the old direct write, as before.
    LifeAsk pot = a;
    pot.native = false;
    pot.direct = false;
    pot.replay_on = false;
    pot.hurt = 0;
    CHECK(RouteLife(pot).route == LifeRoute::write);
    pot.stale = 0;
    CHECK(RouteLife(pot).route == LifeRoute::step);
    pot.step_seen = false;
    CHECK(RouteLife(pot).route == LifeRoute::write);
}

TEST_CASE("objects: NativeOnlyName: enemies, bosses and unknown names are never written") {
    CHECK(CoopObjects::NativeOnlyName("Enemy_Stalchild"));
    CHECK(CoopObjects::NativeOnlyName("Enemy_DungeonBoss_Goma"));
    CHECK(CoopObjects::NativeOnlyName("?"));
    CHECK(CoopObjects::NativeOnlyName(""));
    CHECK_FALSE(CoopObjects::NativeOnlyName("Obj_Pot_A_01"));
    CHECK_FALSE(CoopObjects::NativeOnlyName("Animal_Cucco"));
}

TEST_CASE("objects: a hurt not taken on a copy, or one that leaves life, says nobody dead") {
    Sync sync;
    sync.SetHurt(true);
    Tick(sync, 0, {Enemy(0x100, 0x11, 3), Enemy(0x200, 0x22, 3, 5.0f)});
    Own(sync, *sync.IdOf(0x100), 1);
    Own(sync, *sync.IdOf(0x200), 2);
    proto::Damage d;
    d.player_number = 2;
    d.id = *sync.IdOf(0x100);
    d.amount = 1;  // 3 -> 2: not a kill
    sync.Receive(d);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 3), Enemy(0x200, 0x22, 3, 5.0f)});
    const auto step = Tick(sync, 430 * kMs, {Enemy(0x100, 0x11, 3), Enemy(0x200, 0x22, 3, 5.0f)});
    CHECK(All<proto::ObjectRemove>(step).empty());
    Tick(sync, 4000 * kMs, {Enemy(0x100, 0x11, 2), Enemy(0x200, 0x22, 3, 5.0f)});
    CHECK(sync.Removes().empty());
}

TEST_CASE("objects: with hurt off, a DAMAGE that takes our enemy to 0 says it dead at once") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 1, 0.0f, "Enemy_Skullwalltula")});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 1, 2);
    proto::Damage d;
    d.player_number = 2;
    d.id = id;
    d.amount = 1;
    sync.Receive(d);
    const auto step = Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 1, 0.0f, "Enemy_Skullwalltula")});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 0);
    const auto removes = All<proto::ObjectRemove>(step);
    REQUIRE(removes.size() == 1);
    CHECK(removes[0].epoch == 2);
    // Its own death takes it: no second remove.
    Tick(sync, 100 * kMs, {});
    CHECK(All<proto::ObjectRemove>(Tick(sync, 200 * kMs, {})).empty());
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

    // The flag watcher wrote its key for another player (Take); a key never
    // written here (0x77) is not taken.
    sync.Take(token.hash);
    Tick(sync, 10 * kMs, {token, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Taken(token.hash));
    CHECK_FALSE(sync.Taken(0x77));
    CHECK(sync.Removes() == std::vector<std::uint64_t>{0x300});
    const auto notes = sync.TakeNotes();
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("removed here") != std::string::npos);

    Tick(sync, 20 * kMs, {token, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes() == std::vector<std::uint64_t>{0x300});
    // Gone from the list: nothing more to remove.
    Tick(sync, 30 * kMs, {Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
}

TEST_CASE("objects: a Gold Skulltula whose key the save already holds is removed (issue 89)") {
    // The game makes a placement's actor from its key as the scene loads and
    // never looks again (docs/items.md, "the loaded Skulltula stays"), so an
    // actor it has just made for an already-true key is deleted as the walk
    // sees it: the keys the save holds are fed to Take (Objects::Share).
    Sync sync;
    Seen skulltula;
    skulltula.address = 0x900;
    skulltula.name = "Enemy_Skulltula_Gold";
    skulltula.hash = 0x2df265292516bdf0;
    skulltula.position = {3.0f, 0.0f, 4.0f};
    CHECK(CoopObjects::RemovedWhenTaken(skulltula.name));
    Tick(sync, 0, {skulltula, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());

    // key was true in the save, key + 2 as well (a placement is both)
    sync.Take(0x2df265292516bdf0);
    sync.Take(0x2df265292516bdf0 + 2);
    Tick(sync, 10 * kMs, {skulltula, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Taken(0x2df265292516bdf0));
    CHECK(sync.Taken(0x2df265292516bdf0 + 2));
    CHECK(sync.Removes() == std::vector<std::uint64_t>{0x900});
}

TEST_CASE("objects: a chest opened elsewhere is shown open here, never removed (issue 195)") {
    Sync sync;
    Seen chest;
    chest.address = 0x500;
    chest.name = "Obj_TBox_A_01";
    chest.hash = 0x3c5bb811b4cf54dd;
    chest.position = {1.0f, 0.0f, 1.0f};
    chest.animation = proto::Animation{"CloseWait", 0.0f, 1.0f};
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    // Collect lists a chest only once its key is taken: the walk of the Tick
    // that brings the take does not have it yet, the next one does.
    sync.Take(chest.hash);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    Tick(sync, 20 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    // Its own open idle is played on it, only over its closed idle.
    std::vector<PlayWish> plays;
    sync.Plays(30 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].address == 0x500);
    // deku-visuals-b: first its own opening (the lid), only over its closed idle.
    CHECK(plays[0].name == "FrontOpenSmall");
    CHECK(plays[0].only_over == "Close");
    chest.animation->name = "OpenWait";
    Tick(sync, 100 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    // Its AI puts its closed idle back for a walk: still never removed.
    chest.animation->name = "CloseWait";
    Tick(sync, 4000 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 9000 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    bool shows = false;
    for (const auto& n : sync.TakeNotes()) shows |= n.find("shows open here") != std::string::npos;
    CHECK(shows);
    // The room loads again in the same scene (a new actor): the game made it
    // open, knowing the flag. Left alone, and nothing is played on it.
    chest.address = 0x600;
    chest.animation->name = "OpenWait";
    Tick(sync, 9100 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    plays.clear();
    sync.Plays(9110 * kMs, plays);
    CHECK(plays.empty());
    // Another scene and back: the take is forgotten, nothing goes.
    Tick(sync, 9200 * kMs, {}, 1, "Other");
    CHECK_FALSE(sync.Taken(chest.hash));
    chest.address = 0x700;
    Tick(sync, 9300 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
}

namespace {

Seen Chest(std::uint64_t address, std::uint64_t hash = 0x3c5bb811b4cf54dd) {
    Seen chest;
    chest.address = address;
    chest.name = "Obj_TBox_A_01";
    chest.hash = hash;
    chest.position = {1.0f, 0.0f, 1.0f};
    return chest;
}

} // namespace

TEST_CASE("objects: keys written while no walk ran remove nothing (title, save load, join snapshot)") {
    // Before the first Tick of the run.
    Sync sync;
    sync.Take(0x3c5bb811b4cf54dd);
    Tick(sync, 0, {Chest(0x500), Enemy(0x100, 0x11, 2)});
    Tick(sync, 10 * kMs, {Chest(0x500), Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    CHECK_FALSE(sync.Taken(0x3c5bb811b4cf54dd));

    // After a pause of the walks (a load of the same scene): dropped too.
    sync.Take(0x3c5bb811b4cf54dd);
    Tick(sync, 3000 * kMs, {Chest(0x600), Enemy(0x100, 0x11, 2)});
    Tick(sync, 3010 * kMs, {Chest(0x600), Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());

    // Across a scene change: the next scene's chest was made knowing the key.
    sync.Take(0x3c5bb811b4cf54dd);
    Tick(sync, 3020 * kMs, {Chest(0x700), Enemy(0x200, 0x22, 2)}, 1, "Other");
    Tick(sync, 3030 * kMs, {Chest(0x700), Enemy(0x200, 0x22, 2)}, 1, "Other");
    CHECK(sync.Removes().empty());

    // While the walks run, a take still removes the chest loaded then.
    sync.Take(0x3c5bb811b4cf54dd);
    Tick(sync, 3040 * kMs, {Enemy(0x200, 0x22, 2)}, 1, "Other");
    Tick(sync, 3050 * kMs, {Chest(0x700), Enemy(0x200, 0x22, 2)}, 1, "Other");
    CHECK(sync.Removes().empty());  // shown open: its open idle is played
    // It never shows open (no animation here): left closed, never removed (chest-lid-b, #195).
    for (std::uint64_t t = 3500; t <= 9500; t += 500) {
        Tick(sync, t * kMs, {Chest(0x700), Enemy(0x200, 0x22, 2)}, 1, "Other");
    }
    CHECK(sync.Removes().empty());
}

TEST_CASE("objects: a chest opened elsewhere that shows open stays, open (issues 87, 195)") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.Take(0x3c5bb811b4cf54dd);
    Seen chest = Chest(0x500);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    // Its own AI read the shared opened flag and plays the opened pose.
    chest.animation = proto::Animation{};
    chest.animation->name = "OpenWait";
    Tick(sync, 600 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 2000 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    bool said = false;
    for (const auto& n : sync.TakeNotes()) {
        said |= n.find("shows open here") != std::string::npos;
    }
    CHECK(said);
    // Still closed after the wait: left closed, never removed (chest-lid-b, #195).
    Seen closed = Chest(0x600, 0x77);
    closed.animation = proto::Animation{};
    closed.animation->name = "CloseWait";
    sync.Take(0x77);
    Tick(sync, 2010 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 2020 * kMs, {chest, closed, Enemy(0x100, 0x11, 2)});
    Tick(sync, 3600 * kMs, {chest, closed, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    // Its open idle never took (still closed kChestShowFailUs after): left closed.
    for (std::uint64_t t = 4000; t <= 9000; t += 500) {
        Tick(sync, t * kMs, {chest, closed, Enemy(0x100, 0x11, 2)});
    }
    CHECK(sync.Removes().empty());
    bool failed = false;
    for (const auto& n : sync.TakeNotes()) failed |= n.find("could not be shown open here, left closed") != std::string::npos;
    CHECK(failed);
}

TEST_CASE("objects: a pickup beside our Link is not removed (we may be taking it too)") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.SetPlayer(proto::Vec3{1.5f, 0.0f, 1.0f});  // 0.5 m from it
    Seen heart = Chest(0x500);
    heart.name = "Obj_Heart_Piece";
    sync.Take(heart.hash);
    Tick(sync, 10 * kMs, {heart, Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {heart, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    bool said = false;
    for (const auto& n : sync.TakeNotes()) said |= n.find("our Link is beside it") != std::string::npos;
    CHECK(said);
    // Walking away later does not remove it: the bind window is over.
    sync.SetPlayer(proto::Vec3{20.0f, 0.0f, 1.0f});
    Tick(sync, 30 * kMs, {heart, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    // Another of the same kind far away, in the window: removed at once.
    Seen far = heart;
    far.address = 0x900;
    far.hash = 0x1111;
    sync.Take(0x1111);
    Tick(sync, 40 * kMs, {far, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes() == std::vector<std::uint64_t>{0x900});
}

TEST_CASE("objects: a chest beside our Link is shown open too, never removed (issue 195)") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.SetPlayer(proto::Vec3{1.5f, 0.0f, 1.0f});  // 0.5 m from the chests
    // Closed beside our Link: shown open (the old rule removed it at once, and
    // Alex saw the chest vanish).
    Seen closed = Chest(0x500);
    closed.animation = proto::Animation{"CloseWait", 0.0f, 1.0f};
    sync.Take(0x3c5bb811b4cf54dd);
    Tick(sync, 10 * kMs, {closed, Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {closed, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    std::vector<PlayWish> plays;
    sync.Plays(25 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].address == 0x500);
    sync.TakeNotes();
    // Ours started opening just before the other's flag came (both at once):
    // its animation changed to "Open" with our Link beside it. It stays, and
    // the log says its content may count twice.
    Seen opening = Chest(0x600, 0x2222);
    opening.animation = proto::Animation{"CloseWait", 0.0f, 1.0f};
    Tick(sync, 25 * kMs, {opening, Enemy(0x100, 0x11, 2)});
    opening.animation = proto::Animation{"Open", 0.0f, 1.0f};
    Tick(sync, 28 * kMs, {opening, Enemy(0x100, 0x11, 2)});
    sync.Take(0x2222);
    Tick(sync, 30 * kMs, {opening, Enemy(0x100, 0x11, 2)});
    Tick(sync, 40 * kMs, {opening, Enemy(0x100, 0x11, 2)});
    closed.animation->name = "OpenWait";
    Tick(sync, 2000 * kMs, {closed, opening, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    bool said = false;
    for (const auto& n : sync.TakeNotes()) said |= n.find("at once") != std::string::npos;
    CHECK(said);
}

TEST_CASE("objects: a chest made open is never removed when its flag comes (issue 195)") {
    // B never touched the chest; it was made open ("OpenWait") and has not
    // changed since (the old rule removed it: a vanished open chest).
    for (const bool beside : {false, true}) {
        Sync sync;
        sync.SetPlayer(beside ? proto::Vec3{1.5f, 0.0f, 1.0f} : proto::Vec3{20.0f, 0.0f, 1.0f});
        Seen chest = Chest(0x500);
        chest.animation = proto::Animation{"OpenWait", 0.0f, 1.0f};
        Tick(sync, 0, {chest, Enemy(0x100, 0x11, 2)});
        Tick(sync, 5000 * kMs, {chest, Enemy(0x100, 0x11, 2)});
        sync.Take(chest.hash);
        Tick(sync, 5010 * kMs, {chest, Enemy(0x100, 0x11, 2)});
        Tick(sync, 5020 * kMs, {chest, Enemy(0x100, 0x11, 2)});
        Tick(sync, 9000 * kMs, {chest, Enemy(0x100, 0x11, 2)});
        CHECK(sync.Removes().empty());
        std::vector<PlayWish> plays;
        sync.Plays(9010 * kMs, plays);
        CHECK(plays.empty());
        bool already = false;
        for (const auto& n : sync.TakeNotes()) {
            CHECK(n.find("at once") == std::string::npos);
            already |= n.find("already shows open here") != std::string::npos;
        }
        CHECK(already);
    }
}

TEST_CASE("objects: an already open chest's log says how long it read OpenWait and if it was closed (row 35)") {
    // chest-late-b: Bean Seller's 0.11.0 log had 'already shows open here'
    // for chests the player saw closed; the note now tells the two cases apart.
    Sync sync;
    sync.SetPlayer(proto::Vec3{20.0f, 0.0f, 1.0f});
    Seen made = Chest(0x500, 0x1111);
    made.animation = proto::Animation{"OpenWait", 0.0f, 1.0f};
    Seen changed = Chest(0x600, 0x2222);
    changed.animation = proto::Animation{"CloseWait", 0.0f, 1.0f};
    Tick(sync, 0, {made, changed, Enemy(0x100, 0x11, 2)});
    changed.animation = proto::Animation{"OpenWait", 0.0f, 1.0f};
    Tick(sync, 1000 * kMs, {made, changed, Enemy(0x100, 0x11, 2)});
    for (std::uint64_t t = 2000; t <= 5000; t += 1000) Tick(sync, t * kMs, {made, changed, Enemy(0x100, 0x11, 2)});
    sync.TakeNotes();
    sync.Take(0x1111);
    sync.Take(0x2222);
    Tick(sync, 6000 * kMs, {made, changed, Enemy(0x100, 0x11, 2)});
    Tick(sync, 6010 * kMs, {made, changed, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    std::vector<PlayWish> plays;
    sync.Plays(6020 * kMs, plays);
    CHECK(plays.empty());
    bool first = false;
    bool later = false;
    for (const auto& n : sync.TakeNotes()) {
        if (n.find("already shows open here") == std::string::npos) continue;
        first |= n.find("open idle since first seen") != std::string::npos &&
                 n.find("closed here before") == std::string::npos;
        later |= n.find("open idle for 5.0 s, closed here before") != std::string::npos;
        CHECK(n.find("m away") != std::string::npos);
    }
    CHECK(first);
    CHECK(later);
}

TEST_CASE("objects: a chest away from our Link that starts opening as the flag comes shows open (issue 87)") {
    Sync sync;
    sync.SetPlayer(proto::Vec3{20.0f, 0.0f, 1.0f});
    Seen chest = Chest(0x500);
    chest.animation = proto::Animation{"OpenWait", 0.0f, 1.0f};
    Tick(sync, 0, {chest, Enemy(0x100, 0x11, 2)});
    // Its AI read the shared opened flag a frame before the key's take.
    chest.animation = proto::Animation{"Open", 0.0f, 1.0f};
    Tick(sync, 1000 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    sync.Take(chest.hash);
    Tick(sync, 1010 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 1020 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 4000 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    bool shows = false;
    bool at_once = false;
    for (const auto& n : sync.TakeNotes()) {
        shows |= n.find("shows open here") != std::string::npos;
        at_once |= n.find("at once") != std::string::npos;
    }
    CHECK(shows);
    CHECK_FALSE(at_once);
}

TEST_CASE("objects: our Link opening a chest shown open is not fought, and logged (issue 195)") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.SetPlayer(proto::Vec3{20.0f, 0.0f, 1.0f});  // far
    Seen chest = Chest(0x500);
    chest.animation = proto::Animation{"CloseWait", 0.0f, 1.0f};
    sync.Take(0x3c5bb811b4cf54dd);
    Tick(sync, 10 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    chest.animation->name = "OpenWait";
    Tick(sync, 100 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    // Our Link walks up and opens it anyway (its AI's own opening).
    sync.SetPlayer(proto::Vec3{2.0f, 0.0f, 1.0f});
    chest.animation->name = "Open";
    Tick(sync, 500 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 9000 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    std::vector<PlayWish> plays;
    sync.Plays(9010 * kMs, plays);
    CHECK(plays.empty());  // no longer played over its own animation
    bool again = false;
    for (const auto& n : sync.TakeNotes()) again |= n.find("opened here too") != std::string::npos;
    CHECK(again);
    CHECK(sync.ChestsOpenedTwice() == 1);
}

TEST_CASE("objects: a chest's show-open clock waits while its play is held in an event (chest-sync-2 review)") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    Seen chest = Chest(0x500);
    chest.animation = proto::Animation{"CloseWait", 0.0f, 1.0f};
    sync.Take(chest.hash);
    Tick(sync, 10 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    // Our Link is in a long event beside it: CoopGuard holds the play.
    for (std::uint64_t t = 500; t <= 8000; t += 500) {
        sync.ChestPlayHeld(0x500, t * kMs);
        sync.ChestPlayHeld(0x999, t * kMs);  // not a chest shown open: ignored
        Tick(sync, t * kMs + 1, {chest, Enemy(0x100, 0x11, 2)});
        CHECK(sync.Removes().empty());
    }
    // The event ends and the play is sent: it shows open, never removed.
    chest.animation->name = "OpenWait";
    Tick(sync, 8100 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    // Its AI puts the closed idle back: kept, and the flicker is logged.
    chest.animation->name = "CloseWait";
    Tick(sync, 8200 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 20000 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    bool flicker = false;
    bool removed = false;
    for (const auto& n : sync.TakeNotes()) {
        flicker |= n.find("the lid may flicker") != std::string::npos;
        removed |= n.find("removed") != std::string::npos;
    }
    CHECK(flicker);
    CHECK_FALSE(removed);
    CHECK(sync.ChestsOpenedTwice() == 0);
}

TEST_CASE("objects: a second take of a key does not start the chest's wait again") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.Take(0x3c5bb811b4cf54dd);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {Chest(0x500), Enemy(0x100, 0x11, 2)});
    sync.Take(0x3c5bb811b4cf54dd);  // the same chest's other flag, later
    Tick(sync, 1000 * kMs, {Chest(0x500), Enemy(0x100, 0x11, 2)});
    Tick(sync, 3000 * kMs, {Chest(0x500), Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    // Never shown open: left closed, never removed (chest-lid-b, #195).
    for (std::uint64_t t = 3500; t <= 8000; t += 500) {
        Tick(sync, t * kMs, {Chest(0x500), Enemy(0x100, 0x11, 2)});
    }
    CHECK(sync.Removes().empty());
}

TEST_CASE("objects: a lid that does not take gives the open idle its own full try (clock restarted)") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.Take(0x3c5bb811b4cf54dd);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {Chest(0x500), Enemy(0x100, 0x11, 2)});  // first play: the lid's command
    // No animation here: the lid gives up at the 1500 ms walk; the open idle's 3 s count from there.
    const auto gave_up = [&](std::uint64_t until) {
        bool said = false;
        for (std::uint64_t t = 500; t <= until; t += 500) {
            Tick(sync, t * kMs, {Chest(0x500), Enemy(0x100, 0x11, 2)});
            for (const auto& n : sync.TakeNotes()) said |= n.find("could not be shown open here") != std::string::npos;
        }
        return said;
    };
    CHECK_FALSE(gave_up(4000));  // the old clock (from 20 ms) gave up at 3500
    bool said = false;
    Tick(sync, 4500 * kMs, {Chest(0x500), Enemy(0x100, 0x11, 2)});
    for (const auto& n : sync.TakeNotes()) said |= n.find("could not be shown open here") != std::string::npos;
    CHECK(said);
    CHECK(sync.Removes().empty());
}

TEST_CASE("objects: a late OpenWait after the give-up is shown open, not opened twice (no take-back)") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.SetPlayer(proto::Vec3{1.5f, 0.0f, 1.0f});  // beside it
    Seen chest = Chest(0x500);
    sync.Take(chest.hash);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    for (std::uint64_t t = 500; t <= 6000; t += 500) {
        Tick(sync, t * kMs, {chest, Enemy(0x100, 0x11, 2)});
    }
    // Our own play takes late.
    chest.animation = proto::Animation{};
    chest.animation->name = "OpenWait";
    Tick(sync, 6500 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 7000 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.ChestsOpenedTwice() == 0);
    bool late = false;
    for (const auto& n : sync.TakeNotes()) late |= n.find("shows open here (late)") != std::string::npos;
    CHECK(late);
    CHECK(sync.Removes().empty());
}

TEST_CASE("objects: a chest left closed, then opened here beside our Link, counts as opened twice") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.SetPlayer(proto::Vec3{1.5f, 0.0f, 1.0f});
    Seen chest = Chest(0x500);
    sync.Take(chest.hash);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    for (std::uint64_t t = 500; t <= 9500; t += 500) {
        Tick(sync, t * kMs, {chest, Enemy(0x100, 0x11, 2)});
    }
    bool left = false;
    for (const auto& n : sync.TakeNotes()) left |= n.find("left closed") != std::string::npos;
    CHECK(left);
    CHECK(sync.ChestsOpenedTwice() == 0);
    // Our Link opens it: its own AI plays the opening.
    chest.animation = proto::Animation{};
    chest.animation->name = "FrontOpenSmall";
    Tick(sync, 9600 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 9700 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.ChestsOpenedTwice() == 1);
    CHECK(sync.Removes().empty());
}

TEST_CASE("objects: a take for a chest not loaded removes nothing that loads later") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.Take(0x1234);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 2)});
    Seen chest;
    chest.address = 0x800;
    chest.name = "Obj_TBox_A_01";
    chest.hash = 0x1234;
    Tick(sync, 30 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    sync.Take(0);  // no placement: ignored
    Tick(sync, 40 * kMs, {Enemy(0x100, 0x11, 2)});
    CHECK_FALSE(sync.Taken(0));
}

TEST_CASE("objects: the owner sends its enemy's animation, a copy is asked to play it") {
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

TEST_CASE("objects: enemies and townspeople go by the playing frame, bosses do not (enemy-anim-b)") {
    using CoopObjects::PlaysByFrame;
    CHECK(PlaysByFrame("Enemy_Scrub_Deku_Leaf"));
    CHECK(PlaysByFrame("Enemy_Skulltula"));
    CHECK(PlaysByFrame("Npc_Hylia_030_Child"));
    CHECK_FALSE(PlaysByFrame("Enemy_DungeonBoss_Gohma"));
    CHECK_FALSE(PlaysByFrame("Enemy_MiddleBoss_Dodongo"));
    CHECK_FALSE(PlaysByFrame("Obj_Plant_Grass_A_01"));
}

namespace {
// An owner's sample of an enemy with this animation, as the copy gets it.
proto::ObjectSample EnemySample(const proto::ObjectId& id, std::uint64_t at, const std::string& anim,
                                float frame) {
    proto::ObjectSample s;
    s.id = id;
    s.player_number = 1;
    s.state.time = {at, 1};
    s.state.position = proto::Vec3{0.0f, 0.0f, 0.0f};
    s.state.animation = proto::Animation{anim, frame, 1.0f};
    return s;
}

// The copy's walk at `now` (as player 2), then what it asks the mover to play.
std::vector<CoopObjects::PlayWish> EnemyPlays(Sync& copy, std::uint64_t now, const std::string& name) {
    Tick(copy, now, {Enemy(0x200, 0x11, 2, 0.0f, name)}, 2);
    std::vector<CoopObjects::PlayWish> plays;
    copy.Plays(now, plays);
    return plays;
}
} // namespace

TEST_CASE("objects: an enemy copy gets the owner's new animation once, near its start, run as the owner runs it") {
    const std::string name = "Enemy_Scrub_Deku_Leaf";
    Sync owner;
    Tick(owner, 0, {Enemy(0x100, 0x11, 2, 0.0f, name)});
    const proto::ObjectId id = *owner.IdOf(0x100);

    Sync copy;
    Seen c = Enemy(0x200, 0x11, 2, 0.0f, name);
    c.animation = proto::Animation{"Wait", 40.0f, 1.0f};
    Tick(copy, 0, {c}, 2);
    Own(copy, id, 1);

    // The owner starts "Appear": its first sample shows playing frame 1.
    copy.Receive(EnemySample(id, 1000 * kMs, "Appear", 1.0f));
    auto plays = EnemyPlays(copy, 1000 * kMs, name);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "Appear");
    CHECK(plays[0].play_frame);
    CHECK_FALSE(plays[0].alias);  // an enemy's own wake must show (issue 157)
    // Shown 100 ms late: at its start, not 8 to 15 frames in.
    CHECK(plays[0].frame == doctest::Approx(0.0f));

    // The next sample of the same animation: the same name and sample-led
    // frame (the mover plays a name only when the copy shows another, and
    // sets the frame only when it is off by more than its slack).
    copy.Receive(EnemySample(id, 1050 * kMs, "Appear", 4.0f));
    plays = EnemyPlays(copy, 1150 * kMs, name);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "Appear");
    CHECK(plays[0].sample == 1050 * kMs);
    // 3 frames in 50 ms (1.5 game frames): the owner runs it at 2.
    CHECK(plays[0].run == doctest::Approx(2.0f));
    CHECK(plays[0].frame == doctest::Approx(4.0f));

    // A loop: the owner's "Wait" climbs to 20 and starts again; the copy's
    // frame stays inside that loop.
    copy.Receive(EnemySample(id, 1100 * kMs, "Wait", 18.0f));
    copy.Receive(EnemySample(id, 1150 * kMs, "Wait", 20.0f));
    copy.Receive(EnemySample(id, 1200 * kMs, "Wait", 1.0f));
    plays = EnemyPlays(copy, 1200 * kMs + 100 * kMs + 150 * kMs, name);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "Wait");
    CHECK(plays[0].loop == doctest::Approx(20.0f));
    CHECK(plays[0].frame < 20.0f);
}

TEST_CASE("objects: a boss copy keeps the slot frame it had (issue 150)") {
    const std::string name = "Enemy_DungeonBoss_Gohma";
    Sync owner;
    Tick(owner, 0, {Enemy(0x100, 0x11, 2, 0.0f, name)});
    const proto::ObjectId id = *owner.IdOf(0x100);
    Sync copy;
    Tick(copy, 0, {Enemy(0x200, 0x11, 2, 0.0f, name)}, 2);
    Own(copy, id, 1);
    copy.Receive(EnemySample(id, 1000 * kMs, "Wait", 10.0f));
    const auto plays = EnemyPlays(copy, 1100 * kMs, name);
    REQUIRE(plays.size() == 1);
    CHECK_FALSE(plays[0].play_frame);
    CHECK_FALSE(plays[0].alias);
}

TEST_CASE("objects: an enemy copy never plays its owner's hide animation (issue 35, Navi on a hidden Deku Baba)") {
    CHECK(CoopObjects::IsHideAnimation("Upside_Visible_Off_Spine4"));
    CHECK(CoopObjects::IsHideAnimation("DownSide_Visible_Off_Withered_Spine6"));
    CHECK(CoopObjects::IsHideAnimation("Visible_Off"));
    CHECK_FALSE(CoopObjects::IsHideAnimation("Upside_Visible_On_Spine6"));
    CHECK_FALSE(CoopObjects::IsHideAnimation("Wait_Battle_Wither"));
    CHECK_FALSE(CoopObjects::IsHideAnimation("InVisible_Off"));

    const std::string name = "Enemy_Dekubaba_Withered";
    Sync owner;
    Tick(owner, 0, {Enemy(0x100, 0x11, 2, 0.0f, name)});
    const proto::ObjectId id = *owner.IdOf(0x100);
    Sync copy;
    Tick(copy, 0, {Enemy(0x200, 0x11, 2, 0.0f, name)}, 2);
    Own(copy, id, 1);

    // Before the kill the owner's animations play on the copy as usual.
    copy.Receive(EnemySample(id, 1000 * kMs, "Upside_Visible_On_Spine6", 1.0f));
    auto plays = EnemyPlays(copy, 1100 * kMs, name);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "Upside_Visible_On_Spine6");

    // The owner's dying Baba hides its spine: the copy is asked nothing (its
    // own death hides it and takes it off the target list).
    copy.Receive(EnemySample(id, 1050 * kMs, "Upside_Visible_Off_Spine6", 1.0f));
    CHECK(EnemyPlays(copy, 1150 * kMs, name).empty());

    // A boss keeps every animation of its owner's (its phases may hide parts).
    const std::string boss = "Enemy_DungeonBoss_Gohma";
    Sync boss_owner;
    Tick(boss_owner, 0, {Enemy(0x100, 0x11, 2, 0.0f, boss)});
    const proto::ObjectId boss_id = *boss_owner.IdOf(0x100);
    Sync boss_copy;
    Tick(boss_copy, 0, {Enemy(0x200, 0x11, 2, 0.0f, boss)}, 2);
    Own(boss_copy, boss_id, 1);
    boss_copy.Receive(EnemySample(boss_id, 1000 * kMs, "Visible_Off", 1.0f));
    plays = EnemyPlays(boss_copy, 1100 * kMs, boss);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "Visible_Off");
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
    CHECK(CoopObjects::IsRolled("Enemy_Poe"));  // the graveyard night ghost (owner retest R6)
    CHECK_FALSE(CoopObjects::IsRolled("Enemy_Poe_Sharp"));  // placed event Poe

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

TEST_CASE("objects: a decided Stalchild copy that sank by itself is made again, a dead one is not") {
    // Issue 124 (chunk 2 test, B's log): B made A's three Stalchild, each played
    // its own 'Disappear' 2 s later and left B's list while A's still fought.
    Sync a;  // player 1: decides
    Sync b;  // player 2
    a.Receive(Where(2, kScene, 0));
    b.Receive(Where(1, kScene, 0));
    const auto spawns = All<proto::ObjectSpawn>(
        Tick(a, 100 * kMs, {Enemy(0x100, ~0ULL, 2, 7.0f), Enemy(0x101, ~0ULL, 2, 20.0f)}, 1));
    REQUIRE(spawns.size() == 2);
    Tick(b, 100 * kMs, {}, 2);  // B is in the scene first
    b.Receive(spawns[0]);
    b.Receive(spawns[1]);
    Tick(b, 150 * kMs, {}, 2);
    auto creates = b.Creates();
    REQUIRE(creates.size() == 2);
    for (const auto& c : creates) {
        CHECK(c.remakes == Sync::kRemakes);
    }
    const auto key_at = [&](float x) {
        for (const auto& c : b.Creates()) {
            if (c.position[0] == x) return c.key;
        }
        return std::uint64_t{0};
    };
    const std::uint64_t sinks = key_at(7.0f), dies = key_at(20.0f);

    // Both made and listed.
    Seen sinking = Enemy(0x600, ~0ULL, 2, 7.0f);
    Seen dying = Enemy(0x601, ~0ULL, 2, 20.0f);
    b.Bind({{sinks, 0x600}, {dies, 0x601}});
    Tick(b, 200 * kMs, {sinking, dying}, 2);
    REQUIRE(b.IdOf(0x600));
    REQUIRE(b.IdOf(0x601));

    // One sinks by its own AI; the other is killed here (life 0, 'Die').
    sinking.animation = proto::Animation{"Disappear", 0.0f, 1.0f};
    dying.life = proto::ObjectLife{0, 2};
    dying.animation = proto::Animation{"Die", 0.0f, 1.0f};
    b.Bind({{sinks, 0x600}, {dies, 0x601}});
    Tick(b, 250 * kMs, {sinking, dying}, 2);
    b.Bind({});
    Tick(b, 300 * kMs, {}, 2);
    Tick(b, 350 * kMs, {}, 2);
    CHECK_FALSE(b.IdOf(0x600));
    CHECK_FALSE(b.IdOf(0x601));

    // A's are still alive: both stay wanted; only the sunk one may come back.
    creates = b.Creates();
    REQUIRE(creates.size() == 2);
    for (const auto& c : creates) {
        CHECK_FALSE(c.removed);
        CHECK(c.remakes == (c.key == sinks ? Sync::kRemakes : 0));
    }

    // The director makes it again near A's one: B waits for it instead of
    // deleting it as an own roll, and it takes A's id again.
    Tick(b, 1400 * kMs, {Enemy(0x700, ~0ULL, 2, 7.2f)}, 2);
    CHECK(b.Removes().empty());
    CHECK(b.Refused() == 0);
    b.Bind({{sinks, 0x700}});
    Tick(b, 1450 * kMs, {Enemy(0x700, ~0ULL, 2, 7.2f)}, 2);
    REQUIRE(b.IdOf(0x700));
    CHECK(*b.IdOf(0x700) == spawns[0].id);
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

TEST_CASE("objects: a Stalchild that rose far from player 1 is player 2's to share (issue 124)") {
    // Night field: player 1 stands at x 0, player 2 rolls one at x 100 (60 m
    // past kRollNearM) and one at x 30 (near player 1).
    Sync b;
    auto p1 = Where(1, kScene, 0);
    p1.position = proto::Vec3{0.0f, 0.0f, 0.0f};
    b.Receive(p1);
    const Step far = Tick(b, 100 * kMs, {Enemy(0x500, ~0ULL, 2, 100.0f)}, 2);
    const auto spawns = All<proto::ObjectSpawn>(far);
    REQUIRE(spawns.size() == 1);
    CHECK(spawns[0].id.kind == proto::ObjectKind::made);
    CHECK(spawns[0].id.maker == 2);
    CHECK(b.OwnerOf(spawns[0].id) == 2);
    CHECK(b.Removes().empty());

    const Step near = Tick(b, 150 * kMs, {Enemy(0x500, ~0ULL, 2, 100.0f), Enemy(0x501, ~0ULL, 2, 30.0f)}, 2);
    CHECK(All<proto::ObjectSpawn>(near).empty());
    CHECK_FALSE(b.IdOf(0x501));
    CHECK(b.Removes() == std::vector<std::uint64_t>{0x501});

    // Player 1 makes player 2's far one as a copy, owned by player 2.
    Sync a;
    auto p2 = Where(2, kScene, 0);
    p2.position = proto::Vec3{100.0f, 0.0f, 0.0f};
    a.Receive(p2);
    Tick(a, 100 * kMs, {}, 1);
    a.Receive(spawns[0]);
    Tick(a, 150 * kMs, {}, 1);
    const auto creates = a.Creates();
    REQUIRE(creates.size() == 1);
    CHECK(creates[0].position[0] == doctest::Approx(100.0f));
    CHECK(a.OwnerOf(spawns[0].id) == 2);

    // Player 1's place gone stale: the old rule (lowest number) holds again.
    Sync c;
    auto old = Where(1, kScene, Sync::kSceneStaleUs + 200 * kMs);
    c.Receive(old);
    proto::PlayerSample stale = Where(1, kScene, 0);
    stale.scene.reset();
    stale.position = proto::Vec3{0.0f, 0.0f, 0.0f};
    c.Receive(stale);
    Tick(c, Sync::kSceneStaleUs + 300 * kMs, {Enemy(0x700, ~0ULL, 2, 300.0f)}, 2);
    CHECK(c.Removes() == std::vector<std::uint64_t>{0x700});
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

TEST_CASE("objects: report 99, which placed enemies a game's own logic makes later (late encounters)") {
    // The Gerudo Fortress jail guards (CarpenterSet logic) and mini-boss or
    // ambush enemies of the same kind.
    CHECK(IsLateEncounter("Enemy_MiddleBoss_GerudoThief"));
    CHECK(IsLateEncounter("Enemy_MiddleBoss_GerudoThief_04"));
    CHECK(IsLateEncounter("Enemy_MiddleBoss_Lizalfos"));
    CHECK(IsLateEncounter("Enemy_MiddleBoss_FlareDancer"));
    CHECK(IsLateEncounter("Enemy_Wolfos"));
    // Not: the Gold Skulltula (announced at once, its own rule), bosses and
    // their parts, what the sync makes or keeps its own way, non-enemies.
    CHECK_FALSE(IsLateEncounter("Enemy_Skulltula_Gold"));
    CHECK_FALSE(IsLateEncounter("Enemy_DungeonBoss_Gohma"));
    CHECK_FALSE(IsLateEncounter("Enemy_Gohma_Larva"));
    CHECK_FALSE(IsLateEncounter("Enemy_DungeonBoss_Gohma_Egg"));
    CHECK_FALSE(IsLateEncounter("Enemy_MiddleBoss_FlareDancer_FireBody"));
    CHECK_FALSE(IsLateEncounter("Enemy_Shabom_BigOctoChild"));
    CHECK_FALSE(IsLateEncounter("Enemy_KotakeKoumeBattleManager"));
    CHECK_FALSE(IsLateEncounter("Enemy_Stalchild"));
    CHECK_FALSE(IsLateEncounter("Enemy_Poe"));
    CHECK_FALSE(IsLateEncounter("Enemy_Stalfos_Dictionary"));
    CHECK_FALSE(IsLateEncounter("Npc_Gerudo_FortressGuard"));
    CHECK_FALSE(IsLateEncounter("Obj_Dungeon_SmallKey_GerudoFortress_01"));
}

TEST_CASE("objects: report 99, a jail guard only the host's logic made is copied to the guest near it") {
    constexpr std::uint64_t kHash = 0x2e80bd2b39d06e43ULL;  // GerudoFortress cell 1's guard
    const std::string kThief = "Enemy_MiddleBoss_GerudoThief";
    const proto::ObjectId id{proto::ObjectKind::placed, 0, kHash};
    Sync a;
    // Host: the guard is made when its player walks into the cell, 8 s in.
    Tick(a, 0, {}, 1);
    const std::vector<Seen> guard = {Enemy(0x100, kHash, 4, 23.0f, kThief)};
    CHECK(All<proto::ObjectSpawn>(Tick(a, 8000 * kMs, guard, 1)).empty());
    Own(a, id, 1, 2);
    const auto spawns = All<proto::ObjectSpawn>(Tick(a, 8100 * kMs, guard, 1));
    REQUIRE(spawns.size() == 1);
    CHECK(spawns[0].id == id);
    CHECK(spawns[0].actor == kThief);
    CHECK(All<proto::ObjectSpawn>(Tick(a, 8200 * kMs, guard, 1)).empty());

    // Made at the scene's load by the host's own per-game flags (it went in
    // before, then came back): announced all the same (review, point 2).
    Sync early;
    Tick(early, 0, guard, 1);
    Own(early, id, 1);
    const auto at_load = All<proto::ObjectSpawn>(Tick(early, 100 * kMs, guard, 1));
    REQUIRE(at_load.size() == 1);
    CHECK(at_load[0].id == id);
    // A Gold Skulltula loaded with the scene is still not announced.
    Sync gold;
    const std::vector<Seen> tree = {Enemy(0x100, 0x1111, 1, 0.0f, "Enemy_Skulltula_Gold")};
    Tick(gold, 0, tree, 1);
    Own(gold, {proto::ObjectKind::placed, 0, 0x1111}, 1);
    CHECK(All<proto::ObjectSpawn>(Tick(gold, 100 * kMs, tree, 1)).empty());

    proto::ObjectSpawn relayed = spawns[0];
    relayed.player_number = 1;
    // The owner's samples, relayed to the whole scene.
    const auto heard = [&](Sync& s) {
        proto::ObjectSample smp;
        smp.player_number = 1;
        smp.id = id;
        smp.epoch = 2;
        smp.state.position = proto::Vec3{23.0f, 0.0f, 0.0f};
        s.Receive(smp);
    };

    // Guest far away (another part of the fortress): held, no copy.
    Sync b;
    b.SetPlayer(proto::Vec3{23.0f + kEncounterNearM + 10.0f, 0.0f, 0.0f});
    Tick(b, 0, {}, 2);
    b.Receive(relayed);
    Tick(b, 8300 * kMs, {}, 2);
    heard(b);
    Tick(b, 11000 * kMs, {}, 2);
    CHECK(b.Creates().empty());
    // Guest walks up to the cell: after kEncounterWaitUs without its own, a copy.
    b.SetPlayer(proto::Vec3{25.0f, 0.0f, 0.0f});
    heard(b);
    Tick(b, 12000 * kMs, {}, 2);
    CHECK(b.Creates().empty());
    Tick(b, 12000 * kMs + kEncounterWaitUs, {}, 2);
    auto creates = b.Creates();
    REQUIRE(creates.size() == 1);
    CHECK(creates[0].name == kThief);
    CHECK(creates[0].position[0] == doctest::Approx(23.0f));
    CHECK(b.OwnerOf(id) == 1);
    b.Bind({{creates[0].key, 0x900}});
    Tick(b, 14000 * kMs, {Enemy(0x900, ~0ULL, 4, 23.0f, kThief)}, 2);
    REQUIRE(b.IdOf(0x900));
    CHECK(*b.IdOf(0x900) == id);
    // The guest's own logic makes its guard too (walked into the cell): the copy goes.
    b.Bind({{creates[0].key, 0x900}});
    Tick(b, 14100 * kMs, {Enemy(0x900, ~0ULL, 4, 23.0f, kThief), Enemy(0x950, kHash, 4, 23.0f, kThief)}, 2);
    CHECK(*b.IdOf(0x950) == id);
    CHECK_FALSE(b.IdOf(0x900));
    REQUIRE(b.Creates().size() == 1);
    CHECK(b.Creates()[0].removed);

    // Guest near, its own guard made in the wait: no copy.
    Sync c;
    c.SetPlayer(proto::Vec3{25.0f, 0.0f, 0.0f});
    Tick(c, 0, {}, 2);
    c.Receive(relayed);
    Tick(c, 8300 * kMs, {}, 2);
    Tick(c, 8400 * kMs, {Enemy(0x300, kHash, 4, 23.0f, kThief)}, 2);
    Tick(c, 8400 * kMs + 2 * kEncounterWaitUs, {Enemy(0x300, kHash, 4, 23.0f, kThief)}, 2);
    CHECK(c.Creates().empty());

    // The host beat it before the guest came near: never copied.
    Sync d;
    d.SetPlayer(proto::Vec3{200.0f, 0.0f, 0.0f});
    Tick(d, 0, {}, 2);
    d.Receive(relayed);
    Tick(d, 8300 * kMs, {}, 2);
    d.Receive(proto::ObjectRemove{1, id, 2, proto::RemoveReason::gone, {}});
    Tick(d, 8400 * kMs, {}, 2);
    d.SetPlayer(proto::Vec3{25.0f, 0.0f, 0.0f});
    Tick(d, 9000 * kMs, {}, 2);
    Tick(d, 9000 * kMs + kEncounterWaitUs, {}, 2);
    CHECK(d.Creates().empty());

    // A scene change forgets what was held.
    Sync e;
    e.SetPlayer(proto::Vec3{25.0f, 0.0f, 0.0f});
    Tick(e, 0, {}, 2);
    e.Receive(relayed);
    Tick(e, 8300 * kMs, {}, 2);
    Tick(e, 8400 * kMs, {}, 2, "GerudoValley");
    Tick(e, 8400 * kMs + 2 * kEncounterWaitUs, {}, 2, "GerudoValley");
    CHECK(e.Creates().empty());

    // Review point 3: the owner left (no samples for kEncounterStaleUs) before
    // we came near: dropped, no copy that nobody drives.
    Sync f;
    f.SetPlayer(proto::Vec3{200.0f, 0.0f, 0.0f});
    Tick(f, 0, {}, 2);
    f.Receive(relayed);
    Tick(f, 8300 * kMs, {}, 2);
    Tick(f, 8300 * kMs + kEncounterStaleUs + 100 * kMs, {}, 2);
    f.SetPlayer(proto::Vec3{25.0f, 0.0f, 0.0f});
    Tick(f, 15000 * kMs, {}, 2);
    Tick(f, 15000 * kMs + kEncounterWaitUs, {}, 2);
    CHECK(f.Creates().empty());
}

TEST_CASE("objects: report 99 review, our own one of a copy killed here is dead, not a respawn (issue 133)") {
    constexpr std::uint64_t kHash = 0x2e80bd2b39d06e43ULL;
    const std::string kThief = "Enemy_MiddleBoss_GerudoThief";
    const proto::ObjectId id{proto::ObjectKind::placed, 0, kHash};
    proto::ObjectSpawn relayed;
    relayed.player_number = 1;
    relayed.id = id;
    relayed.actor = kThief;
    relayed.state.position = proto::Vec3{23.0f, 0.0f, 0.0f};

    Sync b;
    b.SetHurt(true);
    b.SetPlayer(proto::Vec3{25.0f, 0.0f, 0.0f});
    Tick(b, 0, {}, 2);
    b.Receive(relayed);
    Tick(b, 8300 * kMs, {}, 2);
    Tick(b, 8400 * kMs, {}, 2);
    Tick(b, 8400 * kMs + kEncounterWaitUs, {}, 2);
    auto creates = b.Creates();
    REQUIRE(creates.size() == 1);
    b.Bind({{creates[0].key, 0x900}});
    Tick(b, 10000 * kMs, {Enemy(0x900, ~0ULL, 4, 23.0f, kThief)}, 2);
    REQUIRE(b.IdOf(0x900));
    // The host kills it: the copy dies and goes.
    b.Receive(proto::ObjectRemove{1, id, 0, proto::RemoveReason::dead, {}});
    b.Bind({{creates[0].key, 0x900}});
    Tick(b, 10100 * kMs, {Enemy(0x900, ~0ULL, 0, 23.0f, kThief)}, 2);
    Tick(b, 12000 * kMs, {}, 2);

    // The guest then walks into the cell: its own logic makes the guard.
    const auto step = Tick(b, 15000 * kMs, {Enemy(0x950, kHash, 4, 23.0f, kThief)}, 2);
    // It dies at once through the game (its room counter counts it) ...
    bool lowered = false;
    for (const auto& w : step.writes) {
        lowered = lowered || (w.address == 0x950 && w.life == 0);
    }
    CHECK(lowered);
    // ... is never listed (the server keeps the kill: no respawn) and never
    // announced.
    for (const auto& p : All<proto::ObjectPresent>(step)) {
        for (const auto& e : p.objects) CHECK_FALSE(e.id == id);
    }
    Own(b, id, 2);
    const auto later = Tick(b, 16000 * kMs, {Enemy(0x950, kHash, 4, 23.0f, kThief)}, 2);
    CHECK(All<proto::ObjectSpawn>(later).empty());
    for (const auto& p : All<proto::ObjectPresent>(later)) {
        for (const auto& e : p.objects) CHECK_FALSE(e.id == id);
    }

    // Re-review: the kill is used once. The guest's room loads again (it left
    // and came back): its game makes the guard anew, a real respawn. It is
    // listed alive as the game made it (the server's dead_for rule decides),
    // not killed again.
    Tick(b, 17000 * kMs, {}, 2);
    Tick(b, 17100 * kMs, {}, 2);
    Tick(b, 17200 * kMs, {}, 2);
    const auto reload = Tick(b, 25000 * kMs, {Enemy(0x960, kHash, 4, 23.0f, kThief)}, 2);
    for (const auto& w : reload.writes) CHECK_FALSE((w.address == 0x960 && w.life == 0));
    const auto present = All<proto::ObjectPresent>(reload);
    REQUIRE_FALSE(present.empty());
    CHECK(std::any_of(present.back().objects.begin(), present.back().objects.end(),
                      [&](const auto& e) { return e.id == id; }));
}

TEST_CASE("objects: report 99 re-review, a new spawn after a copy was killed here is a respawn") {
    constexpr std::uint64_t kHash = 0x2e80bd2b39d06e43ULL;
    const std::string kThief = "Enemy_MiddleBoss_GerudoThief";
    const proto::ObjectId id{proto::ObjectKind::placed, 0, kHash};
    proto::ObjectSpawn relayed;
    relayed.player_number = 1;
    relayed.id = id;
    relayed.actor = kThief;
    relayed.state.position = proto::Vec3{23.0f, 0.0f, 0.0f};

    Sync g;
    g.SetHurt(true);
    g.SetPlayer(proto::Vec3{25.0f, 0.0f, 0.0f});
    Tick(g, 0, {}, 2);
    g.Receive(relayed);
    Tick(g, 8300 * kMs, {}, 2);
    Tick(g, 8400 * kMs, {}, 2);
    Tick(g, 8400 * kMs + kEncounterWaitUs, {}, 2);
    REQUIRE(g.Creates().size() == 1);
    const auto first = g.Creates()[0].key;
    g.Bind({{first, 0x900}});
    Tick(g, 10000 * kMs, {Enemy(0x900, ~0ULL, 4, 23.0f, kThief)}, 2);
    // The host kills it; the copy dies and leaves.
    g.Receive(proto::ObjectRemove{1, id, 0, proto::RemoveReason::dead, {}});
    g.Bind({{first, 0x900}});
    Tick(g, 10100 * kMs, {Enemy(0x900, ~0ULL, 0, 23.0f, kThief)}, 2);
    Tick(g, 12000 * kMs, {}, 2);
    Tick(g, 12100 * kMs, {}, 2);

    // The host's room loads again and its guard respawns: announced anew.
    // Held, then copied when near, as a new enemy.
    g.Receive(relayed);
    Tick(g, 20000 * kMs, {}, 2);
    Tick(g, 20100 * kMs, {}, 2);
    Tick(g, 20100 * kMs + kEncounterWaitUs, {}, 2);
    const auto live = [&] {
        std::size_t n = 0;
        for (const auto& cr : g.Creates()) n += cr.removed ? 0 : 1;
        return n;
    };
    REQUIRE(live() == 1);
    const auto creates = g.Creates();
    const auto copy = std::find_if(creates.begin(), creates.end(), [](const auto& cr) { return !cr.removed; });
    CHECK(copy->key != first);
    CHECK(copy->name == kThief);

    // And our own one made later is no longer taken for the dead one: it
    // takes the id from the copy, alive.
    g.Bind({{copy->key, 0x910}});
    const auto own = Tick(g, 22000 * kMs, {Enemy(0x910, ~0ULL, 4, 23.0f, kThief), Enemy(0x970, kHash, 4, 23.0f, kThief)}, 2);
    for (const auto& w : own.writes) CHECK_FALSE((w.address == 0x970 && w.life == 0));
    REQUIRE(g.IdOf(0x970));
    CHECK(*g.IdOf(0x970) == id);

    // Review nit: the respawn is announced while the killed copy is still
    // dying here (inside the delete grace): a new copy all the same.
    Sync h;
    h.SetHurt(true);
    h.SetPlayer(proto::Vec3{25.0f, 0.0f, 0.0f});
    Tick(h, 0, {}, 2);
    h.Receive(relayed);
    Tick(h, 8300 * kMs, {}, 2);
    Tick(h, 8400 * kMs, {}, 2);
    Tick(h, 8400 * kMs + kEncounterWaitUs, {}, 2);
    REQUIRE(h.Creates().size() == 1);
    const auto dying = h.Creates()[0].key;
    h.Bind({{dying, 0x900}});
    Tick(h, 10000 * kMs, {Enemy(0x900, ~0ULL, 4, 23.0f, kThief)}, 2);
    h.Receive(proto::ObjectRemove{1, id, 0, proto::RemoveReason::dead, {}});
    h.Bind({{dying, 0x900}});
    Tick(h, 10100 * kMs, {Enemy(0x900, ~0ULL, 0, 23.0f, kThief)}, 2);
    // Still on our walk, dying, when the respawn comes.
    h.Receive(relayed);
    h.Bind({{dying, 0x900}});
    Tick(h, 10200 * kMs, {Enemy(0x900, ~0ULL, 0, 23.0f, kThief)}, 2);
    h.Bind({{dying, 0x900}});
    Tick(h, 10300 * kMs, {Enemy(0x900, ~0ULL, 0, 23.0f, kThief)}, 2);
    h.Bind({{dying, 0x900}});
    Tick(h, 10300 * kMs + kEncounterWaitUs, {Enemy(0x900, ~0ULL, 0, 23.0f, kThief)}, 2);
    std::size_t fresh = 0;
    for (const auto& cr : h.Creates()) fresh += (!cr.removed && cr.key != dying) ? 1 : 0;
    CHECK(fresh == 1);
}

TEST_CASE("objects: a hurt carries the hitter's place, an animal's life is written") {
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

TEST_CASE("objects: a hurt the game takes late is no hit of ours, and an enemy is not written back up") {
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
    // Not taken in 400 ms: the rest goes to the game's Life step (WriteLife; never a raw write).
    step = Tick(sync, 420 * kMs, {Enemy(0x100, 0x11, 4)}, 2);
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].hurt == 0);
    CHECK(step.writes[0].life == 3);
    // (WriteLife gave nothing: the replay still waited for its damage step.) Our life stays the real read.
    Tick(sync, 450 * kMs, {Enemy(0x100, 0x11, 4)}, 2);
    // The game takes the replayed hit now (it was paused): 4 -> 3. No hit of ours, nothing written.
    step = Tick(sync, 700 * kMs, {Enemy(0x100, 0x11, 3)}, 2);
    CHECK(step.writes.empty());
    CHECK(sync.HurtsLate() == 1);
    CHECK(All<proto::Damage>(step).empty());
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

TEST_CASE("objects: the AI hold's host table (Hookshot, no guest table)") {
    CoopHold::HostTable t;
    // Nothing held yet: every brain runs as the game wants.
    CHECK(t.Apply(0x1111, 0x1230u) == 0x1230u);
    t.Publish({0x1111, 0, 0x2222}, CoopHold::MasksFor(1));
    CHECK(t.Count() == 2);
    CHECK(t.Apply(0x1111, 0x1230u) == 0x1231u);
    CHECK(t.Apply(0x2222, 0x1230u) == 0x1231u);
    CHECK(t.Apply(0x3333, 0x1230u) == 0x1230u);
    CHECK(t.Apply(0, 0x1230u) == 0x1230u);
    // Mode 2 clears the bit; a component no longer listed runs its own AI again.
    t.Publish({0x2222}, CoopHold::MasksFor(2));
    CHECK(t.Apply(0x2222, 0x1231u) == 0x1230u);
    CHECK(t.Apply(0x1111, 0x1231u) == 0x1231u);
    // At most 64; past them nothing is held.
    std::vector<std::uint64_t> many;
    for (std::uint64_t i = 1; i <= 100; ++i) {
        many.push_back(0x10000 + i);
    }
    t.Publish(many, CoopHold::MasksFor(1));
    CHECK(t.Count() == CoopHold::kMaxHeld);
    CHECK(t.Apply(0x10000 + CoopHold::kMaxHeld, 0) == 1u);
    CHECK(t.Apply(0x10000 + CoopHold::kMaxHeld + 1, 0) == 0u);
    t.Clear();
    CHECK(t.Count() == 0);
    CHECK(t.Apply(0x10001, 0) == 0u);
}

TEST_CASE("objects: group rules for parts and bosses") {
    using CoopObjects::IsBoss;
    using CoopObjects::PartOf;
    REQUIRE(PartOf("Enemy_DungeonBoss_BongoBongo_LeftHand") != nullptr);
    CHECK(std::string(PartOf("Enemy_DungeonBoss_BongoBongo_LeftHand")->roots.front()) ==
          "Enemy_DungeonBoss_BongoBongo");
    CHECK(PartOf("Enemy_DungeonBoss_BongoBongo_LeftHand")->boss);
    // Phantom Ganon's painting show follows him (row 114).
    for (const char* show : {"Enemy_GhostPicturePerformer_PhantomGanon", "Enemy_GhostPicturePerformer_PhantomGanonHorse",
                             "Enemy_GhostPicturePerformer_PhantomGanonImitation",
                             "Enemy_GhostPicturePerformer_PhantomGanonHorse_Imitation"}) {
        REQUIRE(PartOf(show) != nullptr);
        CHECK(std::string(PartOf(show)->roots.front()) == "Enemy_DungeonBoss_PhantomGanon");
        CHECK(PartOf(show)->boss);
    }
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
    a.group = 7;
    CHECK(log.Changed(a));  // joined a group (issue 105)
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

// Issue 133: enemy kills not always shared; issue 132: room enemies that only
// woke in their owner's game.
namespace {

Seen Acting(std::uint64_t address, std::uint64_t hash, std::optional<std::int32_t> life, float x,
            const std::string& animation, std::string name = "Enemy_Dekubaba") {
    Seen s = Enemy(address, hash, life.value_or(1), x, std::move(name));
    if (!life) {
        s.life.reset();  // the big Skulltula: no life we can read
    }
    s.animation = proto::Animation{animation, 0.0f, 1.0f};
    return s;
}

proto::ObjectSample OwnerSample(const proto::ObjectId& id, std::uint8_t owner, std::uint64_t t,
                                float x, std::int32_t life, const std::string& animation) {
    proto::ObjectSample s;
    s.player_number = owner;
    s.id = id;
    s.state.time = {t, 1};
    s.state.position = proto::Vec3{x, 0.0f, 0.0f};
    s.state.life = proto::ObjectLife{life, 2};
    s.state.animation = proto::Animation{animation, 0.0f, 1.0f};
    return s;
}

proto::ObjectRemove Dead(const proto::ObjectId& id, std::uint8_t owner,
                         proto::RemoveReason reason = proto::RemoveReason::dead) {
    proto::ObjectRemove r;
    r.player_number = owner;
    r.id = id;
    r.reason = reason;
    return r;
}

} // namespace

TEST_CASE("objects: an owner from before an enemy unloaded does not outlive it (issue 133)") {
    // The Lixyas run: the Skullwalltula went to player 2 at epoch 1, left
    // player 1's walk, and the server, with nobody holding it, made it anew
    // owned by player 1 at epoch 0. Player 1 kept "player 2, epoch 1",
    // dropped every later owner as old, and sent its own kill to itself.
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 1)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2, 1);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 1)});
    CHECK(sync.OwnerOf(id) == 2);
    Tick(sync, 20 * kMs, {});
    Tick(sync, 30 * kMs, {});
    CHECK(sync.OwnerOf(id) == 0);  // gone with it
    Own(sync, id, 2, 1);           // the server's last word on it, while unloaded
    Tick(sync, 40 * kMs, {});
    CHECK(sync.OwnerOf(id) == 0);
    // Loaded again, the server's new owner at epoch 0 is taken.
    Tick(sync, 2000 * kMs, {Enemy(0x200, 0x11, 1)});
    Own(sync, id, 1, 0);
    auto step = Tick(sync, 2010 * kMs, {Enemy(0x200, 0x11, 1)});
    CHECK(sync.OwnerOf(id) == 1);
    CHECK(All<proto::ObjectSample>(step).size() == 1);  // ours: its state goes out
    // Our own kill is no DAMAGE to ourselves; it is said dead.
    Tick(sync, 2100 * kMs, {Enemy(0x200, 0x11, 0)});
    CHECK(All<proto::Damage>(Tick(sync, 2300 * kMs, {Enemy(0x200, 0x11, 0)})).empty());

    // A reconnect: the server's epochs start again, so the owners are forgotten.
    Sync again;
    Tick(again, 0, {Enemy(0x100, 0x11, 1)});
    const proto::ObjectId id2 = *again.IdOf(0x100);
    Own(again, id2, 2, 3);
    Tick(again, 10 * kMs, {Enemy(0x100, 0x11, 1)});
    again.ForgetOwners();
    Own(again, id2, 1, 0);
    Tick(again, 20 * kMs, {Enemy(0x100, 0x11, 1)});
    CHECK(again.OwnerOf(id2) == 1);
}

TEST_CASE("objects: a hit before the owner is known is kept and sent once it is (issue 133)") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 6)});
    CHECK(All<proto::Damage>(Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 5)})).empty());
    CHECK(All<proto::Damage>(Tick(sync, 500 * kMs, {Enemy(0x100, 0x11, 5)})).empty());
    Own(sync, *sync.IdOf(0x100), 2, 3);
    auto damage = All<proto::Damage>(Tick(sync, 600 * kMs, {Enemy(0x100, 0x11, 5)}));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].amount == 1);
    CHECK(damage[0].epoch == 3);

    // Kept 2 s at most: an owner known later gets nothing.
    Sync late;
    Tick(late, 0, {Enemy(0x100, 0x11, 6)});
    Tick(late, 10 * kMs, {Enemy(0x100, 0x11, 5)});
    CHECK(All<proto::Damage>(Tick(late, 2100 * kMs, {Enemy(0x100, 0x11, 5)})).empty());
    Own(late, *late.IdOf(0x100), 2);
    CHECK(All<proto::Damage>(Tick(late, 2200 * kMs, {Enemy(0x100, 0x11, 5)})).empty());
}

TEST_CASE("objects: a copy we killed is never healed back, and the hit is sent again twice at most") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 0)});
    REQUIRE(All<proto::Damage>(Tick(sync, 130 * kMs, {Enemy(0x100, 0x11, 0)})).size() == 1);

    // The owner's enemy still has its life after the hold: no heal, DAMAGE again.
    for (const std::uint64_t t : {1200, 2300}) {
        sync.Receive(OwnerSample(id, 2, t * kMs, 0.0f, 2, "Wait"));
        auto step = Tick(sync, t * kMs, {Enemy(0x100, 0x11, 0)});
        CHECK(step.writes.empty());
        auto damage = All<proto::Damage>(step);
        REQUIRE(damage.size() == 1);
        CHECK(damage[0].amount == 2);
    }
    // After that nothing more is sent, and the copy is not written up either (the native rule: no game
    // path raises an enemy's life; its game's kill stands).
    sync.Receive(OwnerSample(id, 2, 3400 * kMs, 0.0f, 2, "Wait"));
    auto step = Tick(sync, 3400 * kMs, {Enemy(0x100, 0x11, 0)});
    CHECK(All<proto::Damage>(step).empty());
    CHECK(step.writes.empty());
}

TEST_CASE("objects: an enemy of ours that died is said dead to the others, even with no life read") {
    CHECK(CoopObjects::IsDeathAnimation("Die"));
    CHECK(CoopObjects::IsDeathAnimation("Dead_Fall"));
    CHECK(CoopObjects::IsDeathAnimation("Damage_Death"));
    CHECK(CoopObjects::IsDeathAnimation("Die02"));
    CHECK_FALSE(CoopObjects::IsDeathAnimation("Dive"));
    CHECK_FALSE(CoopObjects::IsDeathAnimation("Wait"));
    CHECK_FALSE(CoopObjects::IsDeathAnimation("Deadline"));

    Sync sync;
    const std::vector<Seen> rest = {Acting(0x100, 0x11, 2, 1.0f, "Wait"),
                                    Acting(0x200, 0x22, std::nullopt, 2.0f, "Wait", "Enemy_Skulltula"),
                                    Acting(0x300, 0x33, 2, 3.0f, "Wait")};
    Tick(sync, 0, rest);
    for (const std::uint64_t a : {0x100, 0x200, 0x300}) {
        Own(sync, *sync.IdOf(a), 1, 4);
    }
    Tick(sync, 10 * kMs, rest);
    // 0x100 read at life 0, 0x200 (no life) plays its death, 0x300 just unloads.
    Tick(sync, 100 * kMs, {Acting(0x100, 0x11, 0, 1.0f, "Die"),
                           Acting(0x200, 0x22, std::nullopt, 2.0f, "Dead_Fall", "Enemy_Skulltula"),
                           Acting(0x300, 0x33, 2, 3.0f, "Wait")});
    Tick(sync, 200 * kMs, {});
    const auto removes = All<proto::ObjectRemove>(Tick(sync, 300 * kMs, {}));
    REQUIRE(removes.size() == 2);
    std::set<std::uint64_t> said;
    for (const auto& r : removes) {
        CHECK(r.reason == proto::RemoveReason::dead);
        CHECK(r.epoch == 4);
        said.insert(r.id.value);
    }
    CHECK(said == std::set<std::uint64_t>{0x11, 0x22});
}

TEST_CASE("objects: a copy its owner said dead loses its life, and is deleted 3 s later if still here") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)}, 2);
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 1);
    // A remove that is not a death changes nothing here.
    sync.Receive(Dead(id, 1, proto::RemoveReason::gone));
    CHECK(Tick(sync, 500 * kMs, {Enemy(0x100, 0x11, 2)}, 2).writes.empty());
    CHECK(sync.OwnerOf(id) == 1);

    sync.Receive(Dead(id, 1));
    auto step = Tick(sync, 1000 * kMs, {Enemy(0x100, 0x11, 2)}, 2);
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 0);
    CHECK(sync.OwnerOf(id) == 0);  // nobody's: no samples, poses or plays
    Tick(sync, 3900 * kMs, {Enemy(0x100, 0x11, 0)}, 2);
    CHECK(sync.Removes().empty());
    Tick(sync, 4000 * kMs, {Enemy(0x100, 0x11, 0)}, 2);
    CHECK(sync.Removes() == std::vector<std::uint64_t>{0x100});
    // No DAMAGE for a fall we wrote.
    step = Tick(sync, 4100 * kMs, {Enemy(0x100, 0x11, 0)}, 2);
    CHECK(All<proto::Damage>(step).empty());
}

namespace {

// A copy at x whose own AI plays `ours` while the owner's samples say
// `owners`, walked every 66 ms from `from` to `to` ms. The flags of the last
// OBJECT_PRESENT sent, if any.
std::optional<std::uint8_t> Wake(Sync& sync, const proto::ObjectId& id, std::uint64_t from,
                                 std::uint64_t to, float x, const std::string& ours,
                                 const std::string& owners) {
    std::optional<std::uint8_t> flags;
    for (std::uint64_t t = from; t < to; t += 66) {
        sync.Receive(OwnerSample(id, 2, t * kMs, x, 2, owners));
        for (const auto& p :
             All<proto::ObjectPresent>(Tick(sync, t * kMs, {Acting(0x100, 0x11, 2, x, ours)}))) {
            flags = p.objects[0].flags;
        }
    }
    return flags;
}

} // namespace

TEST_CASE("objects: a copy whose own AI acts near our Link is flagged targeting (issue 132)") {
    auto run = [](float x, const std::string& ours, bool wake) {
        Sync sync;
        sync.SetWake(wake);
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
        Tick(sync, 0, {Acting(0x100, 0x11, 2, x, "Wait_Ground")});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, 2);
        const auto flags = Wake(sync, id, 10, 1500, x, ours, "Wait_Ground");
        return flags && (*flags & kTargeting) != 0;
    };
    CHECK(run(5.0f, "Appear", true));             // rises for our Link, the owner's does not
    CHECK_FALSE(run(5.0f, "Wait_Ground", true));  // plays what the owner's plays
    CHECK_FALSE(run(15.0f, "Appear", true));      // over 10 m from our Link
    CHECK_FALSE(run(5.0f, "Appear", false));      // LICHII_COOP_OBJECTS_WAKE=0

    // Under 1 s of it is not enough.
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
    Tick(sync, 0, {Acting(0x100, 0x11, 2, 5.0f, "Wait_Ground")});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    const auto flags = Wake(sync, id, 10, 800, 5.0f, "Appear", "Wait_Ground");
    CHECK_FALSE(flags.has_value());
}

TEST_CASE("objects: a ceiling Skulltula copy with our Link under it is flagged targeting (issue 35)") {
    CHECK(CoopObjects::IsCeilingWait("CeilingWait"));
    CHECK(CoopObjects::IsCeilingWait("CeilingWait_ModelOnlyLOD"));
    CHECK_FALSE(CoopObjects::IsCeilingWait("Wait"));
    CHECK_FALSE(CoopObjects::IsCeilingWait("Fall"));

    // The owner's (player 2's) Skulltula hangs 12 m up, over the 10 m wake
    // reach; its copy's own AI shows no wake (it plays CeilingWait too).
    auto run = [](float link_x, const std::string& owners, std::uint8_t owner = 2, bool wake = true,
                  std::uint64_t until = 1500) {
        Sync sync;
        sync.SetWake(wake);
        sync.SetPlayer(proto::Vec3{link_x, 0.0f, 0.0f});
        auto seen = [] {
            Seen s = Acting(0x100, 0x11, 2, 0.0f, "CeilingWait", "Enemy_Skulltula");
            s.position[1] = 12.0f;
            return s;
        };
        Tick(sync, 0, {seen()});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, owner);
        std::optional<std::uint8_t> flags;
        for (std::uint64_t t = 10; t < until; t += 66) {
            proto::ObjectSample s = OwnerSample(id, owner, t * kMs, 0.0f, 2, owners);
            s.state.position = proto::Vec3{0.0f, 12.0f, 0.0f};
            sync.Receive(s);
            for (const auto& p : All<proto::ObjectPresent>(Tick(sync, t * kMs, {seen()}))) {
                flags = p.objects[0].flags;
            }
        }
        return flags && (*flags & kTargeting) != 0;
    };
    CHECK(run(1.0f, "CeilingWait"));              // under it: the server gives it to us
    CHECK_FALSE(run(6.0f, "CeilingWait"));        // 6 m across: not under it
    CHECK_FALSE(run(1.0f, "Walk"));               // already down: the usual rules
    CHECK_FALSE(run(1.0f, "CeilingWait", 1));     // ours already
    CHECK_FALSE(run(1.0f, "CeilingWait", 2, false));  // LICHII_COOP_OBJECTS_WAKE=0
    CHECK_FALSE(run(1.0f, "CeilingWait", 2, true, 800));  // under 1 s: not yet
}

TEST_CASE("objects: a copy whose wake the Mover put back once is flagged targeting (issue 157)") {
    // A buried Peahat's AI asks for its rise once; the Mover plays the
    // owner's ground animation over it on the next game frame, so every walk
    // sees the owner's name. The one put-back is enough.
    auto run = [](float x, std::uint64_t put_back_at, bool wake = true) {
        Sync sync;
        sync.SetWake(wake);
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
        Tick(sync, 0, {Acting(0x100, 0x11, 2, x, "Wait_Ground")});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, 2);
        std::optional<std::uint8_t> flags;
        for (std::uint64_t t = 10; t < 1500; t += 66) {
            if (put_back_at != 0 && t <= put_back_at && put_back_at < t + 66) {
                sync.NoteTakeover(0x100, t * kMs);
            }
            sync.Receive(OwnerSample(id, 2, t * kMs, x, 2, "Wait_Ground"));
            for (const auto& p : All<proto::ObjectPresent>(
                     Tick(sync, t * kMs, {Acting(0x100, 0x11, 2, x, "Wait_Ground")}))) {
                flags = p.objects[0].flags;
            }
        }
        return flags && (*flags & kTargeting) != 0;
    };
    CHECK(run(5.0f, 100));
    CHECK_FALSE(run(5.0f, 0));             // never put back: the owner's rest is ours too
    CHECK_FALSE(run(15.0f, 100));          // over 10 m from our Link
    CHECK_FALSE(run(5.0f, 100, false));    // LICHII_COOP_OBJECTS_WAKE=0
    CHECK_FALSE(run(5.0f, 1400));          // under 1 s since: not yet

    // An enemy we own is not a copy: a put-back note (stale) does nothing.
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
    Tick(sync, 0, {Acting(0x100, 0x11, 2, 5.0f, "Wait_Ground")});
    Own(sync, *sync.IdOf(0x100), 1);
    std::optional<std::uint8_t> flags;
    for (std::uint64_t t = 10; t < 1500; t += 66) {
        sync.NoteTakeover(0x100, t * kMs);
        for (const auto& p : All<proto::ObjectPresent>(
                 Tick(sync, t * kMs, {Acting(0x100, 0x11, 2, 5.0f, "Wait_Ground")}))) {
            flags = p.objects[0].flags;
        }
    }
    CHECK_FALSE((flags && (*flags & kTargeting) != 0));
}

TEST_CASE("objects: a plain copy whose own brain wants another phase near our Link is flagged targeting (issue 101)") {
    // The enemies' brain path: the copy's own brain is skipped and it plays the owner's phase, so its animation
    // never differs and it is never pulled towards our Link; the brain's own ask (TakeOwnWants) is what shows it.
    auto run = [](float x, std::uint64_t from, std::uint64_t until, std::uint8_t owner = 2, bool wake = true) {
        Sync sync;
        sync.SetWake(wake);
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
        Tick(sync, 0, {Acting(0x100, 0x11, 2, x, "Wait")});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, owner);
        std::optional<std::uint8_t> flags;
        for (std::uint64_t t = 10; t < 1500; t += 66) {
            if (t >= from && t < until) {
                sync.NoteOwnBrainWant(0x100, t * kMs);
            }
            sync.Receive(OwnerSample(id, owner, t * kMs, x, 2, "Wait"));
            for (const auto& p : All<proto::ObjectPresent>(Tick(sync, t * kMs, {Acting(0x100, 0x11, 2, x, "Wait")}))) {
                flags = p.objects[0].flags;
            }
        }
        return flags && (*flags & kTargeting) != 0;
    };
    CHECK(run(5.0f, 100, 1500));             // wants to act on our Link for over a second: the server gets the chase
    CHECK_FALSE(run(5.0f, 0, 0));            // its brain wants the owner's phase: nothing
    CHECK_FALSE(run(15.0f, 100, 1500));      // over kWakeM from our Link
    CHECK_FALSE(run(5.0f, 100, 1500, 2, false));  // LICHII_COOP_OBJECTS_WAKE=0
    CHECK_FALSE(run(5.0f, 1200, 1500));      // under a second: not yet
    CHECK_FALSE(run(5.0f, 100, 1500, 1));    // ours: not a copy (its own brain runs anyway)
    // An unknown actor: nothing (no crash, no entry).
    Sync sync;
    sync.NoteOwnBrainWant(0x999, 10 * kMs);
    CHECK_FALSE(sync.IdOf(0x999));
}

TEST_CASE("objects: a copy's own wake for our Link runs, the owner's rest is not put back (report 109)") {
    // Player report 109: a Deku Baba copy rose for our Link ("GetUp"); the
    // owner's buried "Wait" was put back over it, so it came out late and
    // then stood frozen in the rise's pose.
    struct Run {
        std::vector<std::string> plays;  // the owner's animations wished for the copy at the end
        std::vector<std::pair<std::uint64_t, std::string>> given_back;
        bool targeting = false;
    };
    // `own`: what the copy's own AI plays from 100 ms on (the walk sees it);
    // `taken`: the Mover put that play back at 100 ms (the walk sees "Wait").
    auto run = [](float x, const std::string& own, bool taken, const std::string& owners,
                  std::uint64_t until = 1500, bool owners_new = false) {
        Sync sync;
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
        Tick(sync, 0, {Acting(0x100, 0x11, 2, x, "Wait")});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, 2);
        Run r;
        for (std::uint64_t t = 10; t <= until; t += 66) {
            if (taken && t >= 100 && t < 166) {
                sync.NoteTakeover(0x100, t * kMs, own);
            }
            // owners_new: the owner's enemy changed animation just now (it fights).
            const std::string theirs = owners_new && t >= 1000 ? owners : (t < 1000 ? "Wait" : owners);
            sync.Receive(OwnerSample(id, 2, t * kMs, x, 2, theirs));
            const std::string shown = t >= 100 && !taken ? own : "Wait";
            for (const auto& p : All<proto::ObjectPresent>(Tick(sync, t * kMs, {Acting(0x100, 0x11, 2, x, shown)}))) {
                r.targeting = (p.objects[0].flags & kTargeting) != 0;
            }
            for (auto& g : sync.TakeOwnWakes()) {
                r.given_back.push_back(std::move(g));
            }
        }
        std::vector<PlayWish> plays;
        sync.Plays(until * kMs + 5 * kMs, plays);
        for (const PlayWish& p : plays) {
            r.plays.push_back(p.name);
        }
        return r;
    };
    // Its rise seen by the walk: the owner's "Wait" is not wished over it, and
    // the wake still hands it to us.
    Run seen = run(5.0f, "GetUp", false, "Wait");
    CHECK(seen.plays.empty());
    CHECK(seen.given_back.empty());
    CHECK(seen.targeting);
    // Its rise already put back by the Mover: given back once, "Wait" not wished.
    Run taken = run(5.0f, "GetUp", true, "Wait");
    CHECK(taken.plays.empty());
    REQUIRE(taken.given_back.size() == 1);
    CHECK(taken.given_back[0] == std::pair<std::uint64_t, std::string>{0x100, "GetUp"});
    // The owner's next animation (its own Baba rose too) is played as usual.
    CHECK(run(5.0f, "GetUp", false, "GetUp", 1500).plays == std::vector<std::string>{"GetUp"});
    // Over 10 m from our Link: no wake, the owner's rest is played.
    CHECK(run(15.0f, "GetUp", false, "Wait").plays == std::vector<std::string>{"Wait"});
    // After kOwnWakeUs the owner's animation is played again (the old rule).
    CHECK(run(5.0f, "GetUp", false, "Wait", 3500).plays == std::vector<std::string>{"Wait"});
}

TEST_CASE("objects: a copy's own wake gives its rise back once, never over the owner's new animation (report 109)") {
    // The review's case: own wake starts over the owner's "Wait"; the owner's
    // Link then reaches its Baba, which rises ("GetUp"). Plays wishes that, the
    // Mover puts it over the copy's own play: no give-back (no ping-pong).
    auto start = [](Sync& sync) {
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
        Tick(sync, 0, {Acting(0x100, 0x11, 2, 5.0f, "Wait")});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, 2);
        for (std::uint64_t t = 10; t <= 300; t += 66) {
            sync.Receive(OwnerSample(id, 2, t * kMs, 5.0f, 2, "Wait"));
            Tick(sync, t * kMs, {Acting(0x100, 0x11, 2, 5.0f, t >= 100 ? "GetUp" : "Wait")});
        }
        CHECK(sync.TakeOwnWakes().empty());  // seen by the walk: nothing to give back
        return id;
    };
    {
        Sync sync;
        const proto::ObjectId id = start(sync);
        sync.Receive(OwnerSample(id, 2, 400 * kMs, 5.0f, 2, "Attack"));
        Tick(sync, 400 * kMs, {Acting(0x100, 0x11, 2, 5.0f, "Wait")});  // the put-back shows
        sync.NoteTakeover(0x100, 420 * kMs, "GetUp");
        CHECK(sync.TakeOwnWakes().empty());
        sync.NoteTakeover(0x100, 490 * kMs, "GetUp");
        CHECK(sync.TakeOwnWakes().empty());
    }
    // While the owner's "Wait" is still the latest: given back once, not twice.
    {
        Sync sync;
        const proto::ObjectId id = start(sync);
        sync.Receive(OwnerSample(id, 2, 400 * kMs, 5.0f, 2, "Wait"));
        Tick(sync, 400 * kMs, {Acting(0x100, 0x11, 2, 5.0f, "Wait")});  // the put-back shows
        sync.NoteTakeover(0x100, 420 * kMs, "GetUp");
        CHECK(sync.TakeOwnWakes().size() == 1);
        sync.NoteTakeover(0x100, 490 * kMs, "GetUp");
        CHECK(sync.TakeOwnWakes().empty());
    }
}

TEST_CASE("objects: a copy's own wake never plays over the owner's fight (report 109)") {
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
    Tick(sync, 0, {Acting(0x100, 0x11, 2, 5.0f, "Wait")});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    // The owner's Baba attacks its own Link ("Attack" since 1 s ago); our
    // copy's AI turns to ours. The owner's "Attack" stays on the copy.
    for (std::uint64_t t = 10; t <= 2000; t += 66) {
        sync.Receive(OwnerSample(id, 2, t * kMs, 5.0f, 2, t < 1000 ? "GetUp" : "Attack"));
        Tick(sync, t * kMs, {Acting(0x100, 0x11, 2, 5.0f, t < 1000 ? "GetUp" : "Turn")});
    }
    std::vector<PlayWish> plays;
    sync.Plays(2005 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "Attack");
    CHECK(sync.TakeOwnWakes().empty());

    // The owner's Baba then stays in one animation (no new one for 3 s:
    // it idles): our copy's own AI acts on our Link.
    for (std::uint64_t t = 2010; t <= 5500; t += 66) {
        sync.Receive(OwnerSample(id, 2, t * kMs, 5.0f, 2, "Attack"));
        Tick(sync, t * kMs, {Acting(0x100, 0x11, 2, 5.0f, "Turn")});
    }
    plays.clear();
    sync.Plays(5505 * kMs, plays);
    CHECK(plays.empty());
}

TEST_CASE("objects: an enemy of ours woken near our Link is flagged too, so it stays ours") {
    auto run = [](float x, const std::string& now, std::uint64_t until = 1500) {
        Sync sync;
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
        Tick(sync, 0, {Acting(0x100, 0x11, 2, x, "Wait_Ground")});
        Own(sync, *sync.IdOf(0x100), 1);
        std::optional<std::uint8_t> flags;
        for (std::uint64_t t = 66; t <= until; t += 66) {
            for (const auto& p :
                 All<proto::ObjectPresent>(Tick(sync, t * kMs, {Acting(0x100, 0x11, 2, x, now)}))) {
                flags = p.objects[0].flags;
            }
        }
        return flags && (*flags & kTargeting) != 0;
    };
    CHECK(run(5.0f, "Attack"));
    CHECK_FALSE(run(5.0f, "Wait_Ground"));
    CHECK_FALSE(run(5.0f, "Wait_Ground_ModelOnlyLOD"));  // its far variant is still rest
    CHECK_FALSE(run(15.0f, "Attack"));
    // One that keeps flying or patrolling near us is not held for good: the
    // flag ends 4 s after it woke (plus the 2 s it is kept), so a chase by
    // another game can still take it.
    CHECK_FALSE(run(5.0f, "Fly", 9000));
}

TEST_CASE("objects: animation names without their LOD suffix") {
    CHECK(std::string(CoopObjects::AnimationBase("Wait_ModelOnlyLOD")) == "Wait");
    CHECK(std::string(CoopObjects::AnimationBase("Fly_LOD")) == "Fly");
    CHECK(std::string(CoopObjects::AnimationBase("Wait")) == "Wait");
    CHECK(std::string(CoopObjects::AnimationBase("_LOD")) == "_LOD");

    // A copy in its LOD variant of the owner's animation is not woken.
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
    Tick(sync, 0, {Acting(0x100, 0x11, 2, 5.0f, "Wait")});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    const auto flags = Wake(sync, id, 10, 1500, 5.0f, "Wait_ModelOnlyLOD", "Wait");
    CHECK_FALSE((flags && (*flags & kTargeting) != 0));
}

TEST_CASE("objects: a copy brought to 0 by its owner and alive again is neither hit nor written up") {
    // We hit it once, long before; then the owner's own samples took it to
    // 0 and back (a revive). No DAMAGE for the owner's whole life.
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 1)});
    REQUIRE(All<proto::Damage>(Tick(sync, 130 * kMs, {Enemy(0x100, 0x11, 1)})).size() == 1);
    sync.Receive(OwnerSample(id, 2, 5000 * kMs, 0.0f, 0, "Die"));
    REQUIRE(Tick(sync, 5000 * kMs, {Enemy(0x100, 0x11, 1)}).writes.size() == 1);  // to 0
    sync.Receive(OwnerSample(id, 2, 8000 * kMs, 0.0f, 2, "Wait"));
    auto step = Tick(sync, 8000 * kMs, {Enemy(0x100, 0x11, 0)});
    CHECK(All<proto::Damage>(step).empty());
    CHECK(step.writes.empty());  // (an enemy's life is never written up: the native rule)
}

TEST_CASE("objects: owners queued from before a reconnect are not applied after the forget") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 1)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2, 3);  // the old server's, not yet taken by a Tick
    sync.ForgetOwners();
    Own(sync, id, 1, 0);  // the new server's
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 1)});
    CHECK(sync.OwnerOf(id) == 1);
}

TEST_CASE("objects: only chests, pickups and things that go away are removed when taken (issue 138)") {
    // Names from the dungeons' keyed placements (coop_dungeon.cpp).
    for (const char* name : {"Obj_TBox_A_01", "Obj_TBox_A_02_NoBake", "Obj_TBox_GlassTruth_A_01",
                             "Obj_RupeeToken_Silver", "Obj_RupeeToken", "Enemy_Skulltula_Gold", "Obj_Token_Gold",
                             "Obj_Heart_Container", "Obj_Heart", "Obj_Heart_InWater",
                             "Obj_Kakariko_Heart_Piece", "Obj_Rupee_Green", "Obj_RedIce_A_01",
                             "Obj_Ice_A_01", "Obj_BrokenRock_A_01", "WallCrack",
                             "Obj_DodongoCave_CrackWall_A_01", "Obj_FireTemple_CrackFloor_A_01",
                             "Obj_InsideTheDekuTree_SpiderWeb_A_01",
                             "Obj_InsideTheDekuTree_InteractiveSpiderWebParts_A_01",
                             "Obj_InsideTheDekuTree_FallingFloor_A_01"}) {
        CAPTURE(name);
        CHECK(CoopObjects::RemovedWhenTaken(name));
    }
    for (const char* name : {"Obj_EnemyCounter_A_01", "Obj_silverRupeeCounter",
                             "Obj_FireTemple_HammerSlipPillar_A_01", "Obj_Well_CandleStick_A_01",
                             "Land_Well_Waterfall_A_01", "Obj_ForestTemple_GhostPicture_A_01",
                             "Enemy_Anubis", "Obj_ShadowTemple_SkullPot_A_01",
                             "Obj_ShadowTemple_SpikeBoard_A_01", "Obj_ShadowTemple_FallingFloor_A_01",
                             "Enemy_MiddleBoss_IronKnuckle_Nabooru", "Enemy_Stalchild",
                             "Obj_MiniGame_SmallKey",
                             // no 64-bit key: taking one is a WORLD_BREAK (breakables-2)
                             "Obj_Dungeon_SmallKey", "Obj_Dungeon_SmallKey_GerudoFortress_03"}) {
        CAPTURE(name);
        CHECK_FALSE(CoopObjects::RemovedWhenTaken(name));
    }
}

TEST_CASE("objects: a taken enemy counter or hammer pillar stays, and so does a chest beside it (issues 138, 195)") {
    Sync sync;
    Seen counter = Chest(0x510, 0x1111);
    counter.name = "Obj_EnemyCounter_A_01";
    Seen pillar = Chest(0x520, 0x2222);
    pillar.name = "Obj_FireTemple_HammerSlipPillar_A_01";
    const Seen chest = Chest(0x530, 0x3333);
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.Take(counter.hash);
    sync.Take(pillar.hash);
    sync.Take(chest.hash);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {counter, pillar, chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());  // shown open: its open idle is played
    // It never shows open (no animation here): left closed, never removed (chest-lid-b).
    for (std::uint64_t t = 500; t <= 8000; t += 500) {
        Tick(sync, t * kMs, {counter, pillar, chest, Enemy(0x100, 0x11, 2)});
    }
    CHECK(sync.Removes().empty());
}

TEST_CASE("objects: a boss is alive while a body actor has life left, unknown without life (issue 109)") {
    constexpr std::array<std::string_view, 1> kMorpha{"Enemy_DungeonBoss_Morpha"};
    Seen boss = Chest(0x900, 0);
    boss.name = "Enemy_DungeonBoss_Morpha";
    Seen part = Chest(0x910, 0);
    part.name = "Enemy_DungeonBoss_Morpha_Tentacle";
    part.life = coop::protocol::ObjectLife{5, 5};
    CHECK(CoopObjects::BossAlive({part}, kMorpha) == std::optional<bool>(false));  // gone; a part is not the body
    CHECK_FALSE(CoopObjects::BossAlive({part, boss}, kMorpha).has_value());       // no life read: unknown
    boss.life = coop::protocol::ObjectLife{12, 20};
    CHECK(CoopObjects::BossAlive({boss}, kMorpha) == std::optional<bool>(true));
    boss.life = coop::protocol::ObjectLife{0, 20};
    CHECK(CoopObjects::BossAlive({boss}, kMorpha) == std::optional<bool>(false));

    // Twinrova's first phase is Koume and Kotake: alive while either is.
    constexpr std::array<std::string_view, 3> kTwinrova{
        "Enemy_DungeonBoss_Twinrova", "Enemy_DungeonBoss_Koume", "Enemy_DungeonBoss_Kotake"};
    Seen koume = Chest(0x920, 0);
    koume.name = "Enemy_DungeonBoss_Koume";
    koume.life = coop::protocol::ObjectLife{8, 8};
    Seen kotake = koume;
    kotake.address = 0x930;
    kotake.name = "Enemy_DungeonBoss_Kotake";
    kotake.life = coop::protocol::ObjectLife{0, 8};
    CHECK(CoopObjects::BossAlive({koume, kotake}, kTwinrova) == std::optional<bool>(true));
    koume.life = coop::protocol::ObjectLife{0, 8};
    CHECK(CoopObjects::BossAlive({koume, kotake}, kTwinrova) == std::optional<bool>(false));
}

TEST_CASE("objects: a boss off the walk reads by its last life and its delete request, never by time (row 114)") {
    constexpr std::array<std::string_view, 1> kGanon{"Enemy_DungeonBoss_PhantomGanon"};
    using CoopObjects::BossAliveKept;
    CoopObjects::BossLives last;
    Seen boss = Chest(0x900, 0);
    boss.name = "Enemy_DungeonBoss_PhantomGanon";
    boss.life = coop::protocol::ObjectLife{15, 42};
    // Never seen this scene (a save that beat him): none there is dead, as before.
    CHECK(BossAliveKept({}, kGanon, last) == std::optional<bool>(false));
    CoopObjects::NoteBossLives({boss}, last);
    CHECK(BossAliveKept({boss}, kGanon, last) == std::optional<bool>(true));
    // Off the walk with life left and no delete request: away, alive, however long (no 30 s guess).
    CHECK(BossAliveKept({}, kGanon, last) == std::optional<bool>(true));
    // Another actor's delete request changes nothing.
    CoopObjects::NoteBossRemoved(0x1234, last);
    CHECK(BossAliveKept({}, kGanon, last) == std::optional<bool>(true));
    // His own delete request with life left (removed alive): still no death (a Game Over is never his death).
    CoopObjects::NoteBossRemoved(0x900, last);
    CHECK(BossAliveKept({}, kGanon, last) == std::optional<bool>(true));
    // Back on the walk (a new reading): as read.
    CoopObjects::NoteBossLives({boss}, last);
    CHECK_FALSE(last.at("Enemy_DungeonBoss_PhantomGanon").removed);
    // His death is read at life 0 while it runs; deleted after it: dead, before and after its delete request.
    boss.life = coop::protocol::ObjectLife{0, 42};
    CoopObjects::NoteBossLives({boss}, last);
    CHECK(BossAliveKept({boss}, kGanon, last) == std::optional<bool>(false));
    CHECK(BossAliveKept({}, kGanon, last) == std::optional<bool>(false));
    CoopObjects::NoteBossRemoved(0x900, last);
    CHECK(BossAliveKept({}, kGanon, last) == std::optional<bool>(false));
    // A body there with its life unread stays unknown.
    Seen unread = boss;
    unread.life.reset();
    CHECK_FALSE(BossAliveKept({unread}, kGanon, last).has_value());
}

TEST_CASE("objects: a boss's death is read only at life 0, never from a body not read here (G2 review)") {
    constexpr std::array<std::string_view, 1> kGanon{"Enemy_DungeonBoss_PhantomGanon"};
    using CoopObjects::BossReadDead;
    CoopObjects::BossLives last;
    Seen boss = Chest(0x900, 0);
    boss.name = "Enemy_DungeonBoss_PhantomGanon";
    boss.life = coop::protocol::ObjectLife{15, 42};
    // Never read this scene: dead for the pull (BossAliveKept), no reading of a death.
    CHECK_FALSE(BossReadDead({}, kGanon, last));
    CoopObjects::NoteBossLives({boss}, last);
    CHECK_FALSE(BossReadDead({boss}, kGanon, last));  // alive
    CoopObjects::NoteBossRemoved(0x900, last);
    CHECK_FALSE(BossReadDead({}, kGanon, last));  // removed alive
    boss.life = coop::protocol::ObjectLife{0, 42};
    CHECK(BossReadDead({boss}, kGanon, last));  // read at life 0 on this walk
    CoopObjects::NoteBossLives({boss}, last);
    CHECK(BossReadDead({}, kGanon, last));  // its last reading at life 0, then gone
}

TEST_CASE("objects: a body removed alive keeps no boss of several bodies alive, one away does (row 114, Twinrova)") {
    constexpr std::array<std::string_view, 3> kTwinrova{"Enemy_DungeonBoss_Twinrova", "Enemy_DungeonBoss_Koume",
                                                        "Enemy_DungeonBoss_Kotake"};
    using CoopObjects::BossAliveKept;
    CoopObjects::BossLives last;
    Seen koume = Chest(0x910, 0);
    koume.name = "Enemy_DungeonBoss_Koume";
    koume.life = coop::protocol::ObjectLife{8, 8};
    Seen twin = Chest(0x920, 0);
    twin.name = "Enemy_DungeonBoss_Twinrova";
    twin.life = coop::protocol::ObjectLife{24, 24};
    CoopObjects::NoteBossLives({koume, twin}, last);
    // The sister deleted alive (merged), Twinrova then dies and is deleted after her death: dead.
    CoopObjects::NoteBossRemoved(0x910, last);
    twin.life = coop::protocol::ObjectLife{0, 24};
    CoopObjects::NoteBossLives({twin}, last);
    CoopObjects::NoteBossRemoved(0x920, last);
    CHECK(BossAliveKept({}, kTwinrova, last) == std::optional<bool>(false));
    // The sister only away (off the list, no delete request): she still keeps the boss alive.
    CoopObjects::BossLives away;
    koume.life = coop::protocol::ObjectLife{8, 8};
    CoopObjects::NoteBossLives({koume, twin}, away);
    CHECK(BossAliveKept({}, kTwinrova, away) == std::optional<bool>(true));
}

TEST_CASE("objects: a boss is hurt once a body's life is below its maximum (Gohma's intro)") {
    constexpr std::array<std::string_view, 1> kGohma{"Enemy_DungeonBoss_Gohma"};
    Seen boss = Chest(0x940, 0);
    boss.name = "Enemy_DungeonBoss_Gohma";
    CHECK_FALSE(CoopObjects::BossHurt({}, kGohma).has_value());      // not seen: unknown
    CHECK_FALSE(CoopObjects::BossHurt({boss}, kGohma).has_value());  // no life read: unknown
    boss.life = coop::protocol::ObjectLife{21, 21};
    CHECK(CoopObjects::BossHurt({boss}, kGohma) == std::optional<bool>(false));
    boss.life = coop::protocol::ObjectLife{17, 21};
    CHECK(CoopObjects::BossHurt({boss}, kGohma) == std::optional<bool>(true));
    Seen egg = Chest(0x950, 0);
    egg.name = "Enemy_Gohma_Egg";
    egg.life = coop::protocol::ObjectLife{0, 1};
    boss.life = coop::protocol::ObjectLife{21, 21};
    CHECK(CoopObjects::BossHurt({egg, boss}, kGohma) == std::optional<bool>(false));  // not a body
}

TEST_CASE("objects: a breakable taken elsewhere plays its own break, and is deleted only if it stands") {
    // breakables-2 (player test): the Deku Tree web burnt in A just vanished
    // in B. One with a life (a bomb web, a cracked wall) gets life 0 first, so
    // the game breaks it; the delete is the fallback.
    Sync sync;
    Seen web;
    web.address = 0x700;
    web.name = "Obj_InsideTheDekuTree_SpiderWeb_A_02";
    web.hash = 0x2016a1ad3261af2a;
    web.position = {5.0f, 2.0f, 1.0f};
    web.life = proto::ObjectLife{1, 1};
    Tick(sync, 0, {web});
    sync.Take(web.hash);
    // deku-visuals-b: a web first gets its own Burn played; this one never
    // shows it (no animation here), so after kWebBurnTryUs it breaks as before.
    auto step = Tick(sync, 10 * kMs, {web});
    CHECK(step.writes.empty());
    step = Tick(sync, 1100 * kMs, {web});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].address == 0x700);
    CHECK(step.writes[0].life == 0);
    CHECK(step.writes[0].hurt == 0);
    CHECK(sync.Removes().empty());
    // Its break plays: not deleted, not written again.
    step = Tick(sync, 1900 * kMs, {web});
    CHECK(step.writes.empty());
    CHECK(sync.Removes().empty());
    // Still standing 1.5 s on: deleted after all.
    Tick(sync, 2700 * kMs, {web});
    CHECK(sync.Removes() == std::vector<std::uint64_t>{0x700});

    // One with no life (the floor web you fall through) goes once its burn
    // had its try (deku-visuals-b; before: at once).
    Sync other;
    Seen floor = web;
    floor.name = "Obj_InsideTheDekuTree_SpiderWeb_A_01";
    floor.hash = 0x3de66423437a955;
    floor.life.reset();
    Tick(other, 0, {floor});
    other.Take(floor.hash);
    step = Tick(other, 10 * kMs, {floor});
    CHECK(step.writes.empty());
    CHECK(other.Removes().empty());
    step = Tick(other, 1100 * kMs, {floor});
    CHECK(step.writes.empty());
    CHECK(other.Removes() == std::vector<std::uint64_t>{0x700});
}

TEST_CASE("objects: a taken falling floor or web part stays 4 s so its fall shows, then goes (issue 176)") {
    // Issue 177: the slingshot room's bridge and the webs vanished in the other game.
    for (const char* name : {"Obj_InsideTheDekuTree_FallingFloor_A_01",
                             "Obj_InsideTheDekuTree_InteractiveSpiderWebParts_A_02"}) {
        CAPTURE(name);
        Sync sync;
        Seen part;
        part.address = 0x710;
        part.name = name;
        part.hash = 0xe9f286aa47fef9b1;
        part.position = {5.0f, 2.0f, 1.0f};
        Tick(sync, 0, {part});
        sync.Take(part.hash);
        auto step = Tick(sync, 10 * kMs, {part});
        CHECK(step.writes.empty());  // no life to write
        CHECK(sync.Removes().empty());
        Tick(sync, 3000 * kMs, {part});
        CHECK(sync.Removes().empty());
        Tick(sync, 4100 * kMs, {part});
        CHECK(sync.Removes() == std::vector<std::uint64_t>{0x710});
    }
}

TEST_CASE("objects: a kill from before we came dies through our game, deleted only as a fallback (issue 133)") {
    // The server tells a late entrant: no owner was ever named to us. Our
    // game must kill it itself, so its room counter and kill key count.
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)}, 2);
    const proto::ObjectId id = *sync.IdOf(0x100);
    sync.Receive(Dead(id, 1));
    auto step = Tick(sync, 50 * kMs, {Enemy(0x100, 0x11, 2)}, 2);
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 0);
    CHECK(sync.Removes().empty());
    CHECK(All<proto::Damage>(step).empty());
    // Still standing after the grace: deleted.
    Tick(sync, 3100 * kMs, {Enemy(0x100, 0x11, 0)}, 2);
    CHECK(sync.Removes() == std::vector<std::uint64_t>{0x100});

    // One whose life we cannot read goes at once.
    Sync blind;
    Seen e = Enemy(0x100, 0x11, 2, 0.0f, "Enemy_Skulltula");
    e.life.reset();
    Tick(blind, 0, {e}, 2);
    blind.Receive(Dead(*blind.IdOf(0x100), 1));
    CHECK(Tick(blind, 50 * kMs, {e}, 2).writes.empty());
    CHECK(blind.Removes() == std::vector<std::uint64_t>{0x100});
}

TEST_CASE("objects: a life write held through our event lands later and is no hit of ours (#144)") {
    // The owner's hit asks for life 4 -> 3; the write waits 2 s (our Link in a talk with it),
    // so the walk reads 4 all that time. Then it lands: with Written the drop is ours, no DAMAGE.
    const auto run = [](bool told) {
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
        std::size_t asked = 0;
        for (std::uint64_t t = 10; t < 2000; t += 50) {
            asked += Tick(sync, t * kMs, {Enemy(0x100, 0x11, 4)}).writes.size();  // held: not made
        }
        CHECK(asked >= 1);
        if (told) {
            sync.Written(0x100, 3, 2000 * kMs);
        }
        std::size_t damage = 0;
        for (std::uint64_t t = 2000; t < 3000; t += 50) {
            damage += All<proto::Damage>(Tick(sync, t * kMs, {Enemy(0x100, 0x11, 3)})).size();
        }
        return damage;
    };
    CHECK(run(true) == 0);
    CHECK(run(false) >= 1);  // the bug the review found: the late drop went back as our hit
}

TEST_CASE("objects: Written for an actor the Sync does not know, or a life not lower, does nothing") {
    Sync sync;
    sync.Written(0x999, 0, 0);
    Tick(sync, 0, {Enemy(0x100, 0x11, 3)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    sync.Written(0x100, 5, 0);  // not a drop
    // Our own hit afterwards still goes to the owner.
    Tick(sync, 100 * kMs, {Enemy(0x100, 0x11, 2)});
    CHECK(All<proto::Damage>(Tick(sync, 400 * kMs, {Enemy(0x100, 0x11, 2)})).size() == 1);
}

TEST_CASE("objects: a released write the game ignores is written as life, and our next hit still goes") {
    Sync sync;
    sync.SetHurt(true);
    Tick(sync, 0, {Enemy(0x100, 0x11, 4)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    // A held write to 3 lands at 2000 ms, but the game never takes it (life stays 4).
    sync.Written(0x100, 3, 2000 * kMs);
    std::size_t rewritten = 0;
    for (std::uint64_t t = 2000; t < 2600; t += 50) {
        // Until the Sync writes the life itself the game keeps 4; that write lands at once.
        for (const auto& w : Tick(sync, t * kMs, {Enemy(0x100, 0x11, rewritten ? 3 : 4)}).writes) {
            rewritten += w.life == 3 ? 1 : 0;
        }
    }
    CHECK(rewritten == 1);  // after kHurtWaitUs the life is written as the owner has it
    // Our Link's real hit afterwards (3 -> 2) goes to the owner: nothing left over swallows it.
    Tick(sync, 2700 * kMs, {Enemy(0x100, 0x11, 3)});
    Tick(sync, 4000 * kMs, {Enemy(0x100, 0x11, 2)});
    CHECK(All<proto::Damage>(Tick(sync, 4300 * kMs, {Enemy(0x100, 0x11, 2)})).size() == 1);
}

TEST_CASE("objects: stun animations (issue 150)") {
    using CoopObjects::IsStunAnimation;
    CHECK(IsStunAnimation("Stun_Lp"));
    CHECK(IsStunAnimation("Stun_St_Attack_Chance"));
    CHECK(IsStunAnimation("Down_Start"));
    CHECK(IsStunAnimation("Fall_St"));
    CHECK(IsStunAnimation("Stun"));
    CHECK(IsStunAnimation("Down_Loop_ModelOnlyLOD"));
    CHECK_FALSE(IsStunAnimation("Walk"));
    CHECK_FALSE(IsStunAnimation("Damage"));
    CHECK_FALSE(IsStunAnimation("Attack_Chance"));
    CHECK_FALSE(IsStunAnimation("Mat_Stun"));
    CHECK_FALSE(IsStunAnimation("Downward"));
    CHECK_FALSE(IsStunAnimation(""));
}

TEST_CASE("objects: a boss copy stunned here sends a DAMAGE of 0 and keeps its stun (issue 150)") {
    const std::string gohma = "Enemy_DungeonBoss_Gohma";
    Sync owner;
    Seen e = Enemy(0x100, 0x11, 12, 0.0f, gohma);
    e.animation = proto::Animation{"Walk", 10.0f, 1.0f};
    Tick(owner, 0, {e});
    const proto::ObjectId id = *owner.IdOf(0x100);
    Own(owner, id, 1, 4);
    auto samples = All<proto::ObjectSample>(Tick(owner, 10 * kMs, {e}));
    REQUIRE(samples.size() == 1);

    Sync copy;
    Seen c = Enemy(0x200, 0x11, 12, 0.0f, gohma);
    c.animation = proto::Animation{"Walk", 10.0f, 1.0f};
    Tick(copy, 0, {c}, 2);
    Own(copy, id, 1, 4);
    proto::ObjectSample s = samples[0];
    s.player_number = 1;
    s.state.time = {1000 * kMs, 1};
    copy.Receive(s);
    CHECK(All<proto::Damage>(Tick(copy, 1010 * kMs, {c}, 2)).empty());

    // A seed in its eye here: our copy's own game stuns it, no life lost.
    c.animation = proto::Animation{"Stun_St_Attack_Chance", 0.0f, 1.0f};
    auto damage = All<proto::Damage>(Tick(copy, 1100 * kMs, {c}, 2));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].id == id);
    CHECK(damage[0].epoch == 4);
    CHECK(damage[0].amount == 0);
    // The owner's 'Walk' is not played over our stun.
    std::vector<CoopObjects::PlayWish> plays;
    copy.Plays(1110 * kMs, plays);
    CHECK(plays.empty());
    // Still stunned: said once.
    c.animation = proto::Animation{"Stun_Lp", 0.0f, 1.0f};
    CHECK(All<proto::Damage>(Tick(copy, 1200 * kMs, {c}, 2)).empty());
    // Stunned again soon after: not said again within 2 s.
    c.animation = proto::Animation{"Walk", 0.0f, 1.0f};
    Tick(copy, 1300 * kMs, {c}, 2);
    c.animation = proto::Animation{"Stun_Lp", 0.0f, 1.0f};
    CHECK(All<proto::Damage>(Tick(copy, 1400 * kMs, {c}, 2)).empty());
    // Later it is.
    c.animation = proto::Animation{"Walk", 0.0f, 1.0f};
    Tick(copy, 3200 * kMs, {c}, 2);
    c.animation = proto::Animation{"Down_Start", 0.0f, 1.0f};
    CHECK(All<proto::Damage>(Tick(copy, 3300 * kMs, {c}, 2)).size() == 1);

    // The owner takes a DAMAGE of 0 as no hit at all.
    proto::Damage zero = damage[0];
    zero.player_number = 2;
    owner.Receive(zero);
    auto step = Tick(owner, 1200 * kMs, {e});
    CHECK(step.writes.empty());
}

TEST_CASE("objects: a boss's other AS slots go with its samples and are played on the copy (gohma-sync, issue 150)") {
    const std::string gohma = "Enemy_DungeonBoss_Gohma";
    Sync owner;
    Seen e = Enemy(0x100, 0x11, 12, 0.0f, gohma);
    e.animation = proto::Animation{"Stun_Lp", 10.0f, 1.0f};
    e.slots = {{1, {"Eyelid_Up", 3.0f, 1.0f}}, {2, {"", 0.0f, 0.0f}}};
    Tick(owner, 0, {e});
    const proto::ObjectId id = *owner.IdOf(0x100);
    Own(owner, id, 1, 4);
    auto samples = All<proto::ObjectSample>(Tick(owner, 10 * kMs, {e}));
    REQUIRE(samples.size() == 1);
    REQUIRE(samples[0].state.slots);
    CHECK(*samples[0].state.slots == e.slots);
    // Only the eye slot changes (the guard comes down): a new sample at once.
    e.slots[0].animation = proto::Animation{"GuardArm", 0.0f, 1.0f};
    auto changed = All<proto::ObjectSample>(Tick(owner, 200 * kMs, {e}));
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].state.slots->at(0).animation.name == "GuardArm");

    Sync copy;
    Seen c = Enemy(0x200, 0x11, 12, 0.0f, gohma);
    c.animation = proto::Animation{"Walk", 0.0f, 1.0f};
    c.slots = {{1, {"GuardWait", 0.0f, 1.0f}}};  // its own AI's guard
    Tick(copy, 0, {c}, 2);
    Own(copy, id, 1, 4);
    proto::ObjectSample s = samples[0];
    s.player_number = 1;
    s.state.time = {1000 * kMs, 1};
    copy.Receive(s);
    Tick(copy, 1010 * kMs, {c}, 2);  // the copy takes the sample in
    std::vector<CoopObjects::PlayWish> plays;
    copy.Plays(1020 * kMs, plays);
    REQUIRE(plays.size() == 2);  // slot 0 and the eyelid; the empty slot 2 is left alone
    CHECK(plays[0].slot == 0);
    CHECK(plays[0].name == "Stun_Lp");
    CHECK(plays[1].address == 0x200);
    CHECK(plays[1].slot == 1);
    CHECK(plays[1].name == "Eyelid_Up");
    CHECK(plays[1].sample == plays[0].sample);

    // A plain enemy's samples carry no slots.
    Sync plain;
    Seen p = Enemy(0x300, 0x33, 2, 0.0f, "Enemy_Stalchild");
    p.animation = proto::Animation{"Walk", 0.0f, 1.0f};
    Tick(plain, 0, {p});
    Own(plain, *plain.IdOf(0x300), 1);
    auto ps = All<proto::ObjectSample>(Tick(plain, 10 * kMs, {p}));
    REQUIRE(ps.size() == 1);
    CHECK_FALSE(ps[0].state.slots);
}

TEST_CASE("objects: a boss's brain and action verbs go with its samples, a new one at once (native boss sync)") {
    const std::string gohma = "Enemy_DungeonBoss_Gohma";
    Sync owner;
    Seen e = Enemy(0x100, 0x11, 12, 0.0f, gohma);
    e.animation = proto::Animation{"Wait", 0.0f, 1.0f};
    e.bsa = proto::BossBsa{1, 2, std::nullopt, {}};
    Tick(owner, 0, {e});
    const proto::ObjectId id = *owner.IdOf(0x100);
    Own(owner, id, 1, 4);
    auto samples = All<proto::ObjectSample>(Tick(owner, 10 * kMs, {e}));
    REQUIRE(samples.size() == 1);
    REQUIRE(samples[0].state.bsa);
    CHECK(samples[0].state.bsa->brain_verb == 2);
    // Nothing new, standing still: no sample (until the still one, 1 s).
    CHECK(All<proto::ObjectSample>(Tick(owner, 100 * kMs, {e})).empty());
    // A new verb (seq 2), nothing else changed: a sample at the next 50 ms step.
    e.bsa = proto::BossBsa{2, 3, std::nullopt, {}};
    auto changed = All<proto::ObjectSample>(Tick(owner, 200 * kMs, {e}));
    REQUIRE(changed.size() == 1);
    CHECK(changed[0].state.bsa->seq == 2);

    // The copy keeps the owner's latest for the boss module, and its AI is never held.
    Sync copy;
    Seen c = Enemy(0x200, 0x11, 12, 0.0f, gohma);
    c.animation = proto::Animation{"Walk", 0.0f, 1.0f};
    Tick(copy, 0, {c}, 2);
    Own(copy, id, 1, 4);
    CHECK(copy.BossVerbsIn().empty());
    proto::ObjectSample s = changed[0];
    s.player_number = 1;
    s.state.time = {1000 * kMs, 1};
    copy.Receive(s);
    Tick(copy, 1010 * kMs, {c}, 2);
    const auto in = copy.BossVerbsIn();
    REQUIRE(in.size() == 1);
    CHECK(in[0].first == 0x200);
    CHECK(in[0].second.brain_verb == 3);
    CHECK(copy.Holds(1020 * kMs).empty());

    // A new owner (player 3): the old owner's state is not handed on as the
    // new owner's; the AI still runs; the new owner's first sample is taken,
    // even with a lower seq (it lagged the old one).
    Own(copy, id, 3, 5);
    Tick(copy, 1100 * kMs, {c}, 2);
    CHECK(copy.BossVerbsIn().empty());
    CHECK(copy.Holds(1110 * kMs).empty());
    s.player_number = 3;
    s.epoch = 5;
    s.state.bsa = proto::BossBsa{1, 7, std::nullopt, {}};
    s.state.time = {1200 * kMs, 1};
    copy.Receive(s);
    Tick(copy, 1210 * kMs, {c}, 2);
    const auto taken = copy.BossVerbsIn();
    REQUIRE(taken.size() == 1);
    CHECK(taken[0].second.brain_verb == 7);
    CHECK(taken[0].second.seq == 1);

    // A plain enemy's samples carry no verbs.
    Sync plain;
    Seen p = Enemy(0x300, 0x33, 2, 0.0f, "Enemy_Stalchild");
    Tick(plain, 0, {p});
    Own(plain, *plain.IdOf(0x300), 1);
    auto ps = All<proto::ObjectSample>(Tick(plain, 10 * kMs, {p}));
    REQUIRE(ps.size() == 1);
    CHECK_FALSE(ps[0].state.bsa);
}

TEST_CASE("objects: no stun DAMAGE from a plain enemy, our own boss, or when the owner is stunned too") {
    const std::string gohma = "Enemy_DungeonBoss_Gohma";
    Seen plain = Enemy(0x300, 0x33, 2, 0.0f, "Enemy_Stalchild");
    plain.animation = proto::Animation{"Walk", 0.0f, 1.0f};
    Seen boss = Enemy(0x400, 0x44, 12, 0.0f, gohma);
    boss.animation = proto::Animation{"Walk", 0.0f, 1.0f};
    Sync sync;
    Tick(sync, 0, {plain, boss}, 2);
    Own(sync, *sync.IdOf(0x300), 1);
    Own(sync, *sync.IdOf(0x400), 2);  // ours
    plain.animation = proto::Animation{"Stun_Lp", 0.0f, 1.0f};
    boss.animation = proto::Animation{"Stun_Lp", 0.0f, 1.0f};
    CHECK(All<proto::Damage>(Tick(sync, 100 * kMs, {plain, boss}, 2)).empty());

    // A copy whose owner's boss is stunned already: its stun is the owner's.
    Sync owner;
    Seen e = Enemy(0x100, 0x11, 12, 0.0f, gohma);
    e.animation = proto::Animation{"Stun_Lp", 0.0f, 1.0f};
    Tick(owner, 0, {e});
    const proto::ObjectId id = *owner.IdOf(0x100);
    Own(owner, id, 1);
    auto samples = All<proto::ObjectSample>(Tick(owner, 10 * kMs, {e}));
    REQUIRE(samples.size() == 1);
    Sync copy;
    Seen c = Enemy(0x200, 0x11, 12, 0.0f, gohma);
    c.animation = proto::Animation{"Walk", 0.0f, 1.0f};
    Tick(copy, 0, {c}, 2);
    Own(copy, id, 1);
    proto::ObjectSample s = samples[0];
    s.player_number = 1;
    s.state.time = {1000 * kMs, 1};
    copy.Receive(s);
    Tick(copy, 1010 * kMs, {c}, 2);
    c.animation = proto::Animation{"Stun_Lp", 0.0f, 1.0f};
    CHECK(All<proto::Damage>(Tick(copy, 1100 * kMs, {c}, 2)).empty());
}

TEST_CASE("objects: a copy that becomes ours is listed once for the release (issue 150)") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 6, 0.0f, "Enemy_DungeonBoss_Gohma"), Enemy(0x200, 0x22, 2)});
    const proto::ObjectId boss = *sync.IdOf(0x100);
    const proto::ObjectId other = *sync.IdOf(0x200);
    Own(sync, boss, 2, 1);
    Own(sync, other, 1, 1);  // ours from the start: never a copy
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 6, 0.0f, "Enemy_DungeonBoss_Gohma"), Enemy(0x200, 0x22, 2)});
    CHECK(sync.TakeOursNow().empty());
    Own(sync, boss, 1, 2);  // handed to us
    Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 6, 0.0f, "Enemy_DungeonBoss_Gohma"), Enemy(0x200, 0x22, 2)});
    CHECK(sync.TakeOursNow() == std::vector<std::uint64_t>{0x100});
    CHECK(sync.TakeOursNow().empty());
}

// Issue 191: guards. A copy that reacts to our Link goes to us; its patrol never does.
TEST_CASE("objects: guard names and their reactions (issue 191)") {
    CHECK(CoopObjects::IsGuard("Npc_Hylia_Soldier_SecurityGuard"));
    CHECK(CoopObjects::IsGuard("Npc_Hylia_Soldier_004_Child"));
    CHECK(CoopObjects::IsGuard("Npc_Hylia_ImperialSoldier_001_Child"));
    CHECK(CoopObjects::IsGuard("Npc_Gerudo_GEN_SecurityGuard"));
    CHECK(CoopObjects::IsGuard("Npc_Gerudo_FortressGuard"));
    CHECK_FALSE(CoopObjects::IsGuard("Npc_Gerudo_001"));
    CHECK_FALSE(CoopObjects::IsGuard("Npc_Hylia_Fishman_001_Child"));
    CHECK_FALSE(CoopObjects::IsGuard("Enemy_Dekubaba"));
    CHECK(CoopObjects::IsShared("Npc_Hylia_Soldier_SecurityGuard"));
    CHECK(CoopObjects::IsGuardReaction("Notice"));
    CHECK(CoopObjects::IsGuardReaction("NoticeTurn_Quick"));
    CHECK(CoopObjects::IsGuardReaction("Discover"));
    CHECK(CoopObjects::IsGuardReaction("Surprise"));
    CHECK(CoopObjects::IsGuardReaction("Salute"));
    CHECK_FALSE(CoopObjects::IsGuardReaction("Walk"));
    CHECK_FALSE(CoopObjects::IsGuardReaction("Wait"));
    CHECK_FALSE(CoopObjects::IsGuardReaction("Turn"));
}

TEST_CASE("objects: a guard copy that notices our Link is flagged targeting, its patrol is not (issue 191)") {
    constexpr const char* kGuard = "Npc_Hylia_Soldier_SecurityGuard";
    // A guard copy at x owned by player 2, whose game plays `owners`; our
    // game's own AI plays `ours` (seen by the walk) or, with put_back, asks
    // for it once and the Mover puts the owner's back (NoteTakeover).
    auto run = [&](float x, const std::string& ours, const std::string& owners, bool put_back) {
        Sync sync;
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
        Tick(sync, 0, {Acting(0x100, 0x11, 100, x, owners, kGuard)});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, 2);
        std::optional<std::uint8_t> flags;
        for (std::uint64_t t = 10; t < 1500; t += 66) {
            if (put_back && t <= 100 && 100 < t + 66) {
                sync.NoteTakeover(0x100, t * kMs, ours);
            }
            sync.Receive(OwnerSample(id, 2, t * kMs, x, 100, owners));
            const std::string seen = put_back ? owners : ours;
            for (const auto& p : All<proto::ObjectPresent>(
                     Tick(sync, t * kMs, {Acting(0x100, 0x11, 100, x, seen, kGuard)}))) {
                flags = p.objects[0].flags;
            }
        }
        return flags && (*flags & kTargeting) != 0;
    };
    CHECK(run(5.0f, "Notice", "Walk", false));
    CHECK(run(20.0f, "Discover", "Walk", false));    // a guard sees further than 10 m
    CHECK(run(20.0f, "Discover", "Walk", true));     // put back by the Mover once
    CHECK_FALSE(run(5.0f, "Wait", "Walk", false));   // only the patrol's timing differs
    CHECK_FALSE(run(5.0f, "Wait", "Walk", true));
    CHECK_FALSE(run(30.0f, "Notice", "Walk", false)); // over 25 m
    CHECK_FALSE(run(5.0f, "Notice", "Notice", false)); // the owner's guard does it too
}

TEST_CASE("objects: a guard of ours noticing our Link stays ours, walking near us does not hold it (issue 191)") {
    auto run = [](float x, const std::string& now) {
        Sync sync;
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
        Tick(sync, 0, {Acting(0x100, 0x11, 100, x, "Walk", "Npc_Hylia_Soldier_SecurityGuard")});
        Own(sync, *sync.IdOf(0x100), 1);
        std::optional<std::uint8_t> flags;
        for (std::uint64_t t = 66; t <= 1500; t += 66) {
            for (const auto& p : All<proto::ObjectPresent>(Tick(
                     sync, t * kMs, {Acting(0x100, 0x11, 100, x, now, "Npc_Hylia_Soldier_SecurityGuard")}))) {
                flags = p.objects[0].flags;
            }
        }
        return flags && (*flags & kTargeting) != 0;
    };
    CHECK(run(15.0f, "Discover"));
    CHECK_FALSE(run(15.0f, "Wait"));      // another patrol animation: no fight
    CHECK_FALSE(run(30.0f, "Discover"));  // over 25 m: noticing someone else
}

TEST_CASE("objects: a guard copy that becomes ours is listed for the release and its replay (issue 191)") {
    Sync sync;
    Tick(sync, 0, {Acting(0x100, 0x11, 100, 0.0f, "Walk", "Npc_Hylia_Soldier_SecurityGuard"),
                   Acting(0x200, 0x22, 100, 0.0f, "Walk", "Npc_Gerudo_001")});
    const proto::ObjectId guard = *sync.IdOf(0x100);
    const proto::ObjectId other = *sync.IdOf(0x200);
    Own(sync, guard, 2, 1);
    Own(sync, other, 2, 1);
    Tick(sync, 10 * kMs, {Acting(0x100, 0x11, 100, 0.0f, "Walk", "Npc_Hylia_Soldier_SecurityGuard"),
                          Acting(0x200, 0x22, 100, 0.0f, "Walk", "Npc_Gerudo_001")});
    Own(sync, guard, 1, 2);
    Own(sync, other, 1, 2);
    Tick(sync, 20 * kMs, {Acting(0x100, 0x11, 100, 0.0f, "Walk", "Npc_Hylia_Soldier_SecurityGuard"),
                          Acting(0x200, 0x22, 100, 0.0f, "Walk", "Npc_Gerudo_001")});
    CHECK(sync.TakeOursNow() == std::vector<std::uint64_t>{0x100});
}

// Issue 151 (fishing-2): every fish shared, any player's lure works on it.
namespace {

Seen Fish(std::uint64_t address, std::uint64_t hash, float x) {
    Seen s;
    s.address = address;
    s.name = "Animal_Fish_Bass";
    s.hash = hash;
    s.position = {x, 0.0f, 0.0f};
    return s;
}

// What our games says about one fish (fishing-3): its last FISH_LURE entry,
// and whether any OBJECT_PRESENT flagged it targeting or fighting (never).
struct FishSaid {
    std::optional<proto::FishLureEntry> lure;
    bool flagged = false;
};

void Note(FishSaid& said, const Step& sent) {
    for (const auto& p : All<proto::ObjectPresent>(sent)) {
        for (const auto& o : p.objects) {
            if (o.flags & (proto::object_presence::targeting | proto::object_presence::in_fight)) {
                said.flagged = true;
            }
        }
    }
    for (const auto& f : All<proto::FishLure>(sent)) {
        REQUIRE(f.fish.size() == 1);
        said.lure = f.fish[0];
    }
}

// A fish copy at x, owned by player 2; each 33 ms our game's own frame swims
// it `step` metres towards x = 0 (our lure) from where the Mover put it, each
// 66 ms a walk.
FishSaid Swim(Sync& sync, std::uint64_t from, std::uint64_t to, float x, float step) {
    FishSaid said;
    for (std::uint64_t t = from; t < to; t += 33) {
        sync.NoteDrift(0x100, {x, 0.0f, 0.0f}, {x - step, 0.0f, 0.0f}, t * kMs);
        if ((t - from) % 66 == 0) {
            Note(said, Tick(sync, t * kMs, {Fish(0x100, 0x11, x)}));
        }
    }
    return said;
}

bool Has(const FishSaid& said, std::uint8_t flag) {
    return said.lure && (said.lure->flags & flag) != 0;
}

} // namespace

TEST_CASE("objects: a fish copy that swims at our lure for 1 s is drawn to it (issue 151)") {
    auto run = [](float x, float step, std::uint8_t owner, bool lure, std::uint64_t to = 1500) {
        Sync sync;
        // Our Link on the dock, far from the fish: the lure is what counts.
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 30.0f});
        if (lure) {
            sync.SetLures({proto::Vec3{0.0f, 0.0f, 0.0f}});
        }
        Tick(sync, 0, {Fish(0x100, 0x11, x)});
        Own(sync, *sync.IdOf(0x100), owner);
        const FishSaid said = Swim(sync, 10, to, x, step);
        CHECK_FALSE(said.flagged);  // never a chase or a fight to the server
        return Has(said, proto::fish_lure::drawn);
    };
    CHECK(run(4.0f, 0.02f, 2, true));         // swims at our lure
    CHECK_FALSE(run(4.0f, 0.02f, 2, true, 900));  // under 1 s: not yet
    CHECK(run(0.5f, 0.0f, 2, true));          // holds on it: a bite
    CHECK_FALSE(run(4.0f, -0.02f, 2, true));  // swims away
    CHECK_FALSE(run(8.0f, 0.02f, 2, true));   // over 6 m from it
    CHECK_FALSE(run(4.0f, 0.002f, 2, true));  // the Mover's noise
    CHECK_FALSE(run(4.0f, 0.02f, 2, false));  // no lure of ours cast
}

TEST_CASE("objects: every fish is listed with how near our lure is (issue 151)") {
    auto said = [](float x, bool lure, std::uint8_t owner = 2) {
        Sync sync;
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 30.0f});
        if (lure) {
            sync.SetLures({proto::Vec3{0.0f, 0.0f, 0.0f}});
        }
        FishSaid out;
        Note(out, Tick(sync, 1000 * kMs, {Fish(0x100, 0x11, x)}));
        Own(sync, *sync.IdOf(0x100), owner);
        Note(out, Tick(sync, 1066 * kMs, {Fish(0x100, 0x11, x)}));
        return out;
    };
    const FishSaid far = said(3.0f, false);
    REQUIRE(far.lure);  // listed with no lure, so the server knows it is a fish
    CHECK(far.lure->distance_cm == proto::kFishLureNone);
    CHECK(far.lure->flags == 0);
    const FishSaid near = said(3.1f, true);
    REQUIRE(near.lure);
    CHECK(near.lure->distance_cm == 300);  // 310 cm in 25 cm steps
    CHECK(near.lure->flags == 0);          // a copy resting near it
    CHECK(said(7.0f, true).lure->distance_cm == proto::kFishLureNone);  // out of reach
    CHECK(Has(said(0.5f, true, 1), proto::fish_lure::hooked));  // ours, on the lure
    CHECK_FALSE(Has(said(0.5f, true, 2), proto::fish_lure::hooked));  // a copy is never hooked
    // A fish that is not a fish is not listed.
    Sync sync;
    sync.SetLures({proto::Vec3{0.0f, 0.0f, 0.0f}});
    CHECK(All<proto::FishLure>(Tick(sync, 1000 * kMs, {Enemy(0x200, 0x22, 2)})).empty());
}

TEST_CASE("objects: a fish of ours at our lure is hooked or drawn, never a fight (issue 151)") {
    auto run = [](float x, float dx, bool lure) {
        Sync sync;
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 30.0f});
        if (lure) {
            sync.SetLures({proto::Vec3{0.0f, 0.0f, 0.0f}});
        }
        Tick(sync, 0, {Fish(0x100, 0x11, x)});
        Own(sync, *sync.IdOf(0x100), 1);
        FishSaid said;
        for (std::uint64_t t = 66; t <= 1500; t += 66) {
            x += dx;
            Note(said, Tick(sync, t * kMs, {Fish(0x100, 0x11, x)}));
        }
        CHECK_FALSE(said.flagged);
        return said;
    };
    CHECK(Has(run(0.3f, 0.0f, true), proto::fish_lure::hooked));   // held at the lure
    CHECK(Has(run(5.5f, -0.05f, true), proto::fish_lure::drawn));  // comes at it, 0.75 m/s
    const FishSaid resting = run(3.0f, 0.0f, true);
    CHECK(resting.lure->flags == 0);
    CHECK(resting.lure->distance_cm == 300);
    CHECK(run(3.0f, 0.05f, true).lure->flags == 0);  // swims off
    CHECK(run(0.3f, 0.0f, false).lure->distance_cm == proto::kFishLureNone);  // no lure of ours
}

TEST_CASE("objects: the fish list goes once when it changes, and once more when empty (issue 151)") {
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 30.0f});
    sync.SetLures({proto::Vec3{0.0f, 0.0f, 0.0f}});
    auto lists = [&](std::uint64_t ms, std::vector<Seen> seen) {
        return All<proto::FishLure>(Tick(sync, ms * kMs, std::move(seen)));
    };
    REQUIRE(lists(1000, {Fish(0x100, 0x11, 3.0f)}).size() == 1);
    Own(sync, *sync.IdOf(0x100), 2);
    CHECK(lists(1066, {Fish(0x100, 0x11, 3.0f)}).empty());   // the same
    CHECK(lists(1132, {Fish(0x100, 0x11, 2.6f)}).empty());   // under 250 ms since
    CHECK(lists(1300, {Fish(0x100, 0x11, 2.6f)}).size() == 1);  // 250 cm now, due
    // Gone from the walk: one empty list, then nothing.
    std::size_t empties = 0;
    for (std::uint64_t t = 1400; t <= 2400; t += 100) {
        for (const auto& f : lists(t, {})) {
            CHECK(f.fish.empty());
            ++empties;
        }
    }
    CHECK(empties == 1);
}

TEST_CASE("objects: a fish its owner landed is gone for everyone (issue 151)") {
    // The owner: the fish leaves the walk in Link's hands.
    auto gone = [](float link_x) {
        Sync sync;
        sync.SetPlayer(proto::Vec3{link_x, 0.0f, 0.0f});
        Tick(sync, 0, {Fish(0x100, 0x11, 1.0f)});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, 1, 3);
        Tick(sync, 10 * kMs, {Fish(0x100, 0x11, 1.0f)});
        // Gone after kMissedWalks empty walks: the remove may come at any of them.
        std::vector<proto::ObjectRemove> out;
        for (const std::uint64_t t : {100, 200, 300}) {
            for (const auto& r : All<proto::ObjectRemove>(Tick(sync, t * kMs, {}))) {
                out.push_back(r);
            }
        }
        return out;
    };
    const auto landed = gone(0.0f);
    REQUIRE(landed.size() == 1);
    CHECK(landed[0].reason == proto::RemoveReason::dead);
    CHECK(landed[0].epoch == 3);
    CHECK(gone(40.0f).empty());  // unloaded far from our Link: not caught

    // Another game: its copy is deleted at once, with no life to lower.
    Sync other;
    Tick(other, 0, {Fish(0x200, 0x11, 1.0f)}, 2);
    const proto::ObjectId id = *other.IdOf(0x200);
    Own(other, id, 1, 3);
    Tick(other, 10 * kMs, {Fish(0x200, 0x11, 1.0f)}, 2);
    CHECK(other.Removes().empty());
    proto::ObjectRemove r;
    r.player_number = 1;
    r.id = id;
    r.epoch = 3;
    r.reason = proto::RemoveReason::dead;
    other.Receive(r);
    const auto step = Tick(other, 20 * kMs, {Fish(0x200, 0x11, 1.0f)}, 2);
    CHECK(step.writes.empty());
    CHECK(other.Removes() == std::vector<std::uint64_t>{0x200});
}

// gohma-larva: a Gohma egg hatches into a larva, another actor, only in the
// game that runs the egg (its owner); the others hold their egg copy.
namespace {

const char* const kEgg = "Enemy_DungeonBoss_Gohma_Egg";
const char* const kLarva = "Enemy_Gohma_Larva";
const std::string kDeku = "InsideTheDekuTree";

bool Lists(const Step& step, const proto::ObjectId& id) {
    for (const auto& p : All<proto::ObjectPresent>(step)) {
        for (const auto& e : p.objects) {
            if (e.id == id) return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("objects: an egg that hatches here shares its larva and ends the egg (gohma-larva)") {
    CHECK(CoopObjects::IsHatchingEgg("Enemy_Gohma_Egg"));
    CHECK(CoopObjects::IsHatchingEgg(kEgg));
    CHECK_FALSE(CoopObjects::IsHatchingEgg(kLarva));
    CHECK(CoopObjects::IsHatchling(kLarva));
    CHECK_FALSE(CoopObjects::IsHatchling("Enemy_Dekubaba"));

    Sync a;
    Tick(a, 0, {Enemy(0x100, 0, 1, 0.0f, kEgg)}, 1, kDeku);
    const proto::ObjectId egg = *a.IdOf(0x100);
    CHECK(egg.kind == proto::ObjectKind::placed);
    Own(a, egg, 1, 3);
    Tick(a, 100 * kMs, {Enemy(0x100, 0, 1, 0.0f, kEgg)}, 1, kDeku);

    // The larva comes while the egg is still on the walk: a made enemy.
    const auto born = Tick(a, 200 * kMs, {Enemy(0x200, 0, 1, 1.0f, kLarva)}, 1, kDeku);
    const auto spawns = All<proto::ObjectSpawn>(born);
    REQUIRE(spawns.size() == 1);
    CHECK(spawns[0].actor == kLarva);
    CHECK(spawns[0].id.kind == proto::ObjectKind::made);
    CHECK(spawns[0].id.maker == 1);
    CHECK(*a.IdOf(0x200) == spawns[0].id);
    CHECK(a.OwnerOf(spawns[0].id) == 1);
    CHECK(All<proto::ObjectRemove>(born).empty());

    // The egg leaves the walk: OBJECT_REMOVE gone with its epoch, once.
    const auto removes = All<proto::ObjectRemove>(Tick(a, 300 * kMs, {Enemy(0x200, 0, 1, 1.0f, kLarva)}, 1, kDeku));
    REQUIRE(removes.size() == 1);
    CHECK(removes[0].id == egg);
    CHECK(removes[0].epoch == 3);
    CHECK(removes[0].reason == proto::RemoveReason::gone);
    CHECK(All<proto::ObjectRemove>(Tick(a, 400 * kMs, {Enemy(0x200, 0, 1, 1.0f, kLarva)}, 1, kDeku)).empty());
    CHECK(a.Hatched() == 1);
}

TEST_CASE("objects: a larva that comes after its egg left still ends the egg (gohma-larva)") {
    Sync a;
    Tick(a, 0, {Enemy(0x100, 0, 1, 0.0f, kEgg)}, 1, kDeku);
    const proto::ObjectId egg = *a.IdOf(0x100);
    Own(a, egg, 1, 2);
    Tick(a, 100 * kMs, {Enemy(0x100, 0, 1, 0.0f, kEgg)}, 1, kDeku);
    Tick(a, 200 * kMs, {}, 1, kDeku);
    const auto left = Tick(a, 300 * kMs, {}, 1, kDeku);
    CHECK(All<proto::ObjectRemove>(left).empty());
    // Still listed, so the server keeps it ours meanwhile.
    CHECK(Lists(Tick(a, 2100 * kMs, {}, 1, kDeku), egg));

    const auto born = Tick(a, 2200 * kMs, {Enemy(0x200, 0, 1, 2.0f, kLarva)}, 1, kDeku);
    const auto removes = All<proto::ObjectRemove>(born);
    REQUIRE(removes.size() == 1);
    CHECK(removes[0].id == egg);
    CHECK(removes[0].epoch == 2);
    REQUIRE(All<proto::ObjectSpawn>(born).size() == 1);
}

TEST_CASE("objects: a larva matched to its sibling egg still ends the egg it came from (gohma-sync, issue 178)") {
    Sync a;
    const auto both = [](float larva_x) {
        std::vector<Seen> seen{Enemy(0x100, 0, 1, 0.0f, kEgg), Enemy(0x101, 0, 1, 2.5f, kEgg)};
        if (larva_x >= 0.0f) seen.push_back(Enemy(0x200, 0, 1, larva_x, kLarva));
        return seen;
    };
    Tick(a, 0, both(-1.0f), 1, kDeku);
    const proto::ObjectId first = *a.IdOf(0x100);
    const proto::ObjectId second = *a.IdOf(0x101);
    Own(a, first, 1, 3);
    Own(a, second, 1, 5);
    Tick(a, 100 * kMs, both(-1.0f), 1, kDeku);
    // The first egg's larva comes out nearer the second egg: matched to it.
    REQUIRE(All<proto::ObjectSpawn>(Tick(a, 200 * kMs, both(1.5f), 1, kDeku)).size() == 1);
    // The first egg leaves: it is the one that hatched, ended at once.
    const std::vector<Seen> after{Enemy(0x101, 0, 1, 2.5f, kEgg), Enemy(0x200, 0, 1, 1.5f, kLarva)};
    std::vector<proto::ObjectRemove> removes;
    for (std::uint64_t t = 300; t <= 500; t += 100) {
        for (const auto& r : All<proto::ObjectRemove>(Tick(a, t * kMs, after, 1, kDeku))) removes.push_back(r);
    }
    REQUIRE(removes.size() == 1);
    CHECK(removes[0].id == first);
    CHECK(removes[0].epoch == 3);
    CHECK(removes[0].reason == proto::RemoveReason::gone);
    // The second egg hatches later: its own larva is shared and it is ended too.
    std::vector<Seen> later = after;
    later.push_back(Enemy(0x201, 0, 1, 3.5f, kLarva));
    REQUIRE(All<proto::ObjectSpawn>(Tick(a, 600 * kMs, later, 1, kDeku)).size() == 1);
    const std::vector<Seen> larvae{Enemy(0x200, 0, 1, 1.5f, kLarva), Enemy(0x201, 0, 1, 3.5f, kLarva)};
    removes.clear();
    for (std::uint64_t t = 700; t <= 900; t += 100) {
        for (const auto& r : All<proto::ObjectRemove>(Tick(a, t * kMs, larvae, 1, kDeku))) removes.push_back(r);
    }
    REQUIRE(removes.size() == 1);
    CHECK(removes[0].id == second);
    CHECK(removes[0].epoch == 5);
}

TEST_CASE("objects: two eggs hatch one after the other, the second leaving first: both end, both larvae shared (gohma-sync review)") {
    Sync a;
    const Seen e1 = Enemy(0x100, 0, 1, 0.0f, kEgg);
    const Seen e2 = Enemy(0x101, 0, 1, 2.5f, kEgg);
    Tick(a, 0, {e1, e2}, 1, kDeku);
    const proto::ObjectId first = *a.IdOf(0x100);
    const proto::ObjectId second = *a.IdOf(0x101);
    Own(a, first, 1, 3);
    Own(a, second, 1, 5);
    Tick(a, 100 * kMs, {e1, e2}, 1, kDeku);
    // E1's larva comes beside E1: matched to it, rightly.
    const Seen l1 = Enemy(0x200, 0, 1, -0.5f, kLarva);
    REQUIRE(All<proto::ObjectSpawn>(Tick(a, 200 * kMs, {e1, e2, l1}, 1, kDeku)).size() == 1);
    // E2 leaves (hatching) while E1 is still listed: ended at once, E1 unmatched.
    std::vector<proto::ObjectRemove> removes;
    std::vector<proto::ObjectSpawn> spawns;
    const auto step = [&](std::uint64_t t, const std::vector<Seen>& seen) {
        const auto out = Tick(a, t * kMs, seen, 1, kDeku);
        for (const auto& r : All<proto::ObjectRemove>(out)) removes.push_back(r);
        for (const auto& sp : All<proto::ObjectSpawn>(out)) spawns.push_back(sp);
    };
    for (std::uint64_t t = 300; t <= 500; t += 100) step(t, {e1, l1});
    REQUIRE(removes.size() == 1);
    CHECK(removes[0].id == second);
    // E1 leaves, then E2's larva comes out where E2 was: it matches E1 (gone
    // alive within 2 s), which ends E1. Both eggs ended once, both larvae shared.
    for (std::uint64_t t = 600; t <= 800; t += 100) step(t, {l1});
    const Seen l2 = Enemy(0x201, 0, 1, 2.0f, kLarva);
    for (std::uint64_t t = 900; t <= 1100; t += 100) step(t, {l1, l2});
    REQUIRE(removes.size() == 2);
    CHECK(removes[1].id == first);
    CHECK(removes[1].epoch == 3);
    CHECK(spawns.size() == 1);  // l2 (l1 was shared at 200 ms)
    CHECK(a.Hatched() == 2);
}

TEST_CASE("objects: no larva, a dead egg, or another game's egg: nothing hatched here (gohma-larva)") {
    // Gone and no larva within 2 s: no remove, and it leaves the list.
    Sync a;
    Tick(a, 0, {Enemy(0x100, 0, 1, 0.0f, kEgg)}, 1, kDeku);
    const proto::ObjectId egg = *a.IdOf(0x100);
    Own(a, egg, 1);
    Tick(a, 100 * kMs, {}, 1, kDeku);
    Tick(a, 200 * kMs, {}, 1, kDeku);
    CHECK_FALSE(Lists(Tick(a, 4500 * kMs, {}, 1, kDeku), egg));
    const auto late = Tick(a, 4600 * kMs, {Enemy(0x200, 0, 1, 0.5f, kLarva)}, 1, kDeku);
    CHECK(All<proto::ObjectRemove>(late).empty());
    CHECK(All<proto::ObjectSpawn>(late).empty());
    CHECK(a.IdOf(0x200)->kind == proto::ObjectKind::placed);

    // A killed egg is no hatch.
    Sync k;
    Tick(k, 0, {Enemy(0x100, 0, 1, 0.0f, kEgg)}, 1, kDeku);
    Own(k, *k.IdOf(0x100), 1);
    Tick(k, 100 * kMs, {Enemy(0x100, 0, 0, 0.0f, kEgg)}, 1, kDeku);
    Tick(k, 200 * kMs, {}, 1, kDeku);
    Tick(k, 300 * kMs, {}, 1, kDeku);
    const auto after = Tick(k, 400 * kMs, {Enemy(0x200, 0, 1, 0.0f, kLarva)}, 1, kDeku);
    CHECK(All<proto::ObjectSpawn>(after).empty());
    CHECK(k.Hatched() == 0);

    // Another game owns the egg: our larva (its AI got loose) is no share.
    Sync b;
    Tick(b, 0, {Enemy(0x100, 0, 1, 0.0f, kEgg)}, 2, kDeku);
    Own(b, *b.IdOf(0x100), 1);
    const auto own = Tick(b, 100 * kMs, {Enemy(0x100, 0, 1, 0.0f, kEgg), Enemy(0x200, 0, 1, 0.5f, kLarva)}, 2, kDeku);
    CHECK(All<proto::ObjectSpawn>(own).empty());
    CHECK(b.Hatched() == 0);
}

TEST_CASE("objects: a copy's egg goes and the owner's larva is made here (gohma-larva)") {
    Sync b;
    Tick(b, 0, {Enemy(0x100, 0, 1, 0.0f, kEgg), Enemy(0x110, 0, 1, 8.0f, "Enemy_Dekubaba")}, 2, kDeku);
    const proto::ObjectId egg = *b.IdOf(0x100);
    const proto::ObjectId baba = *b.IdOf(0x110);
    Own(b, egg, 1, 1);
    Own(b, baba, 1);
    const std::vector<Seen> both = {Enemy(0x100, 0, 1, 0.0f, kEgg), Enemy(0x110, 0, 1, 8.0f, "Enemy_Dekubaba")};

    // An older epoch and a gone for a plain enemy change nothing.
    b.Receive(proto::ObjectRemove{1, egg, 0, proto::RemoveReason::gone, {}});
    b.Receive(proto::ObjectRemove{1, baba, 0, proto::RemoveReason::gone, {}});
    Tick(b, 100 * kMs, both, 2, kDeku);
    CHECK(b.Removes().empty());

    b.Receive(proto::ObjectRemove{1, egg, 1, proto::RemoveReason::gone, {}});
    Tick(b, 200 * kMs, both, 2, kDeku);
    CHECK(b.Removes() == std::vector<std::uint64_t>{0x100});

    proto::ObjectSpawn spawn;
    spawn.player_number = 1;
    spawn.id = {proto::ObjectKind::made, 1, 7};
    spawn.actor = kLarva;
    spawn.state.position = proto::Vec3{1.0f, 0.0f, 0.0f};
    b.Receive(spawn);
    Tick(b, 300 * kMs, {Enemy(0x110, 0, 1, 8.0f, "Enemy_Dekubaba")}, 2, kDeku);
    const auto creates = b.Creates();
    REQUIRE(creates.size() == 1);
    CHECK(creates[0].name == kLarva);
    CHECK(b.OwnerOf(spawn.id) == 1);

    // The director's larva, seen before it is bound, is not taken for ours.
    Tick(b, 400 * kMs, {Enemy(0x110, 0, 1, 8.0f, "Enemy_Dekubaba"), Enemy(0x300, 0, 1, 1.0f, kLarva)}, 2, kDeku);
    CHECK_FALSE(b.IdOf(0x300));
    b.Bind({{creates[0].key, 0x300}});
    Tick(b, 500 * kMs, {Enemy(0x110, 0, 1, 8.0f, "Enemy_Dekubaba"), Enemy(0x300, 0, 1, 1.0f, kLarva)}, 2, kDeku);
    REQUIRE(b.IdOf(0x300));
    CHECK(*b.IdOf(0x300) == spawn.id);
}
// boss-sync-2 (issue 178): a copy of another game's egg runs its own AI, so
// our game hatches it through its own path (the game's perception sees the
// players' puppets too). Our own larva then is the owner's larva's copy.
namespace {

proto::ObjectSpawn OwnersLarva(float x) {
    proto::ObjectSpawn spawn;
    spawn.player_number = 1;
    spawn.id = {proto::ObjectKind::made, 1, 9};
    spawn.actor = kLarva;
    spawn.state.position = proto::Vec3{x, 0.0f, 0.0f};
    return spawn;
}

} // namespace

TEST_CASE("objects: our game hatches its copy of an egg, the owner's larva takes our own larva (issue 178)") {
    for (const bool egg_left : {true, false}) {
        CAPTURE(egg_left);
        Sync b;
        Tick(b, 0, {Enemy(0x100, 0, 1, 0.0f, kEgg)}, 2, kDeku);
        const proto::ObjectId egg = *b.IdOf(0x100);
        Own(b, egg, 1, 1);
        Tick(b, 100 * kMs, {Enemy(0x100, 0, 1, 0.0f, kEgg)}, 2, kDeku);
        std::vector<Seen> seen;
        if (egg_left) {
            Tick(b, 200 * kMs, {}, 2, kDeku);
            Tick(b, 300 * kMs, {}, 2, kDeku);
            CHECK_FALSE(b.IdOf(0x100));
        } else {
            seen.push_back(Enemy(0x100, 0, 1, 0.0f, kEgg));
        }
        seen.push_back(Enemy(0x300, 0, 1, 1.0f, kLarva));
        // Our own larva: kept, not shared (it is not out of an egg of ours).
        const auto born = Tick(b, 400 * kMs, seen, 2, kDeku);
        CHECK(All<proto::ObjectSpawn>(born).empty());
        REQUIRE(b.IdOf(0x300));
        CHECK(b.IdOf(0x300)->kind == proto::ObjectKind::placed);

        // The owner's larva comes: ours is its copy, nothing is made.
        const proto::ObjectSpawn spawn = OwnersLarva(1.5f);
        b.Receive(spawn);
        Tick(b, 500 * kMs, seen, 2, kDeku);
        CHECK(b.Creates().empty());
        REQUIRE(b.IdOf(0x300));
        CHECK(*b.IdOf(0x300) == spawn.id);
        CHECK(b.OwnerOf(spawn.id) == 1);
        CHECK(b.Removes().empty());
    }
}

TEST_CASE("objects: a larva of ours far from any copy egg is not the owner's (issue 178)") {
    Sync b;
    Tick(b, 0, {Enemy(0x100, 0, 1, 0.0f, kEgg)}, 2, kDeku);
    Own(b, *b.IdOf(0x100), 1, 1);
    const std::vector<Seen> seen{Enemy(0x100, 0, 1, 0.0f, kEgg), Enemy(0x300, 0, 1, 20.0f, kLarva)};
    Tick(b, 100 * kMs, seen, 2, kDeku);
    b.Receive(OwnersLarva(1.0f));
    Tick(b, 200 * kMs, seen, 2, kDeku);
    REQUIRE(b.Creates().size() == 1);  // the director makes the owner's
    CHECK(b.IdOf(0x300)->kind == proto::ObjectKind::placed);
}

TEST_CASE("objects: our own larva out of a copy egg waits only so long for the owner's, then is ended: one larva per game (issue 178)") {
    Sync b;
    Tick(b, 0, {Enemy(0x100, 0, 1, 0.0f, kEgg)}, 2, kDeku);
    Own(b, *b.IdOf(0x100), 1, 1);
    Tick(b, 100 * kMs, {Enemy(0x100, 0, 1, 0.0f, kEgg)}, 2, kDeku);  // the owner is known
    Step sent;
    const auto keep = [&sent](const Step& m) {
        sent.send.insert(sent.send.end(), m.send.begin(), m.send.end());
    };
    for (std::uint64_t t = 200; t <= 300; t += 100) {
        keep(Tick(b, t * kMs, {Enemy(0x100, 0, 1, 0.0f, kEgg), Enemy(0x300, 0, 1, 1.0f, kLarva)}, 2, kDeku));
    }
    REQUIRE(b.IdOf(0x300));
    CHECK(b.IdOf(0x300)->kind == proto::ObjectKind::placed);
    // Still waiting just before the wait runs out: kept.
    const std::uint64_t wait_end = 200 * kMs + Sync::kOwnHatchWaitUs;
    keep(Tick(b, wait_end, {Enemy(0x300, 0, 1, 1.0f, kLarva)}, 2, kDeku));
    CHECK(b.Removes().empty());
    // The wait runs out: ours is ended through the game's removal, never shared.
    const std::uint64_t late = wait_end + kMs;
    keep(Tick(b, late, {Enemy(0x300, 0, 1, 1.0f, kLarva)}, 2, kDeku));
    CHECK(b.Removes() == std::vector<std::uint64_t>{0x300});
    keep(Tick(b, late + 50 * kMs, {}, 2, kDeku));  // it went
    CHECK(All<proto::ObjectSpawn>(sent).empty());  // the owner's game gets no second larva
    // The owner's egg hatches later: its larva is the only one made here.
    b.Receive(OwnersLarva(1.0f));
    Tick(b, late + 100 * kMs, {}, 2, kDeku);
    const auto creates = b.Creates();
    REQUIRE(creates.size() == 1);
    CHECK(creates[0].name == kLarva);
    b.Bind({{creates[0].key, 0x310}});
    Tick(b, late + 200 * kMs, {Enemy(0x310, 0, 1, 1.0f, kLarva)}, 2, kDeku);
    REQUIRE(b.IdOf(0x310));
    CHECK(*b.IdOf(0x310) == OwnersLarva(1.0f).id);
    CHECK(b.Removes().empty());
}

TEST_CASE("objects: townspeople near our Link in an event are flagged targeting and wait (issue 200)") {
    constexpr const char* kNpc = "Npc_Kokiri_Mido";
    // A copy at x owned by player 2; our Link at 0 is in an event or not.
    auto run = [&](float x, bool event, const std::string& name, bool ours) {
        Sync sync;
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
        Tick(sync, 0, {Acting(0x100, 0x11, 100, x, "Wait", name)});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, ours ? 1 : 2);
        sync.SetEvent(event);
        std::optional<std::uint8_t> flags;
        for (std::uint64_t t = 10; t < 700; t += 66) {
            if (!ours) {
                sync.Receive(OwnerSample(id, 2, t * kMs, x, 100, "Wait"));
            }
            for (const auto& p : All<proto::ObjectPresent>(
                     Tick(sync, t * kMs, {Acting(0x100, 0x11, 100, x, "Talk", name)}))) {
                flags = p.objects[0].flags;
            }
        }
        const bool waits = sync.EventPartner(0x100, 700 * kMs);
        CHECK(waits == (flags && (*flags & kTargeting) != 0));
        return flags && (*flags & kTargeting) != 0;
    };
    CHECK(run(5.0f, true, kNpc, false));
    CHECK(run(25.0f, true, kNpc, false));  // further than the 8 m event rule
    CHECK(run(5.0f, true, kNpc, true));    // ours: held (a fight to the server)
    CHECK_FALSE(run(5.0f, false, kNpc, false));             // no event
    CHECK_FALSE(run(35.0f, true, kNpc, false));             // over 30 m
    CHECK_FALSE(run(5.0f, true, "Animal_Cucco", false));    // not a townsperson
}

TEST_CASE("objects: an event's townsperson is let go kEventKeepUs after the event (issue 200)") {
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
    const Seen npc = Acting(0x100, 0x11, 100, 3.0f, "Wait", "Npc_Kokiri_Saria");
    Tick(sync, 0, {npc});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    sync.SetEvent(true);
    Tick(sync, 100 * kMs, {npc});  // applies the OBJECT_OWNER too
    CHECK(sync.OwnerAt(0x100) == 2);
    CHECK(sync.EventPartner(0x100, 100 * kMs));
    CHECK(sync.EventPartners(100 * kMs) == 1);
    CHECK(sync.EventPartnersOurs(100 * kMs) == 0);  // claimed, not handed over yet
    Own(sync, id, 1, 1);                         // the server gives it to us (P1)
    Tick(sync, 120 * kMs, {npc});
    CHECK(sync.OwnerAt(0x100) == 1);
    CHECK(sync.EventPartnersOurs(120 * kMs) == 1);
    sync.SetEvent(false);
    std::optional<std::uint8_t> flags;
    for (std::uint64_t t = 166; t <= 4000; t += 66) {
        for (const auto& p : All<proto::ObjectPresent>(Tick(sync, t * kMs, {npc}))) {
            flags = p.objects[0].flags;
        }
    }
    CHECK_FALSE(sync.EventPartner(0x100, 4000 * kMs));
    CHECK(sync.EventPartners(4000 * kMs) == 0);
    REQUIRE(flags);
    CHECK((*flags & kTargeting) == 0);
    CHECK(sync.OwnerAt(0x999) == 0);
}

TEST_CASE("objects: every actor our event is with is claimed, any kind and any distance (row 97)") {
    // Mr. Shorty's softlock: a Deku Scrub copy owned by P2 (who had left the room)
    // stood further than the 8 m event rule from our Link while our event waited on
    // it; the owner's 'Sleep' was put on it every frame and the event never ended.
    constexpr const char* kScrub = "Enemy_Scrub_Deku";
    struct Case {
        float x = 20.0f;
        bool event = true;
        std::optional<bool> member;            // what the walk read of its performer
        std::optional<std::uint64_t> own_at;   // NoteTakeover at this ms
    };
    auto run = [&](const Case& c) {
        Sync sync;
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
        Seen scrub = Acting(0x100, 0x11, 100, c.x, "Sleep", kScrub);
        Tick(sync, 0, {scrub});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, 2);
        if (c.own_at && *c.own_at < 10) {
            sync.NoteTakeover(0x100, *c.own_at * kMs, "Appear");  // before the event
        }
        sync.SetEvent(c.event, 10 * kMs);
        scrub.event_member = c.member;
        std::optional<std::uint8_t> flags;
        for (std::uint64_t t = 10; t < 700; t += 66) {
            sync.Receive(OwnerSample(id, 2, t * kMs, c.x, 100, "Sleep"));
            if (c.own_at && *c.own_at >= 10 && t >= *c.own_at) {
                sync.NoteTakeover(0x100, t * kMs, "Appear");
            }
            for (const auto& p : All<proto::ObjectPresent>(Tick(sync, t * kMs, {scrub}))) {
                flags = p.objects[0].flags;
            }
        }
        const bool claimed = flags && (*flags & kTargeting) != 0;
        CHECK(sync.EventPartner(0x100, 700 * kMs) == claimed);
        return claimed;
    };
    CHECK(run({20.0f, true, true, {}}));                 // bound by our event, beyond 8 m
    CHECK(run({80.0f, true, true, {}}));                 // bound, far across the room
    CHECK_FALSE(run({20.0f, false, true, {}}));          // no event of ours
    CHECK_FALSE(run({20.0f, true, false, {}}));          // the event does not bind it
    CHECK(run({20.0f, true, std::nullopt, 200}));        // performer unknown: its own AI acts in our event
    CHECK_FALSE(run({20.0f, true, std::nullopt, 5}));    // ... but only since the event began
    CHECK_FALSE(run({40.0f, true, std::nullopt, 200}));  // ... and within 30 m
    CHECK_FALSE(run({20.0f, true, false, 200}));         // its performer says it is not in it
    CHECK_FALSE(run({20.0f, true, std::nullopt, {}}));   // an enemy nothing ties to the event

    // The log names them, with why.
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
    Seen scrub = Acting(0x100, 0x11, 100, 20.0f, "Sleep", kScrub);
    Tick(sync, 0, {scrub});
    Own(sync, *sync.IdOf(0x100), 2);
    sync.SetEvent(true, 10 * kMs);
    scrub.event_member = true;
    Tick(sync, 50 * kMs, {scrub});
    const auto list = sync.EventPartnerList(50 * kMs);
    REQUIRE(list.size() == 1);
    CHECK(list[0].name == kScrub);
    CHECK(list[0].owner == 2);
    CHECK(list[0].why == Sync::PartnerWhy::member);
    CHECK(list[0].metres == doctest::Approx(20.0f));
    // kEventKeepUs after the event it is let go.
    sync.SetEvent(false, 100 * kMs);
    Tick(sync, 100 * kMs, {scrub});
    CHECK(sync.EventPartner(0x100, 100 * kMs + Sync::kEventKeepUs - 60 * kMs));
    CHECK_FALSE(sync.EventPartner(0x100, 100 * kMs + Sync::kEventKeepUs));
}

TEST_CASE("objects: another player's event here keeps its bound actors acting, claims none (G1, issues 251, 200)") {
    // Our game plays another player's event (a live dungeon event, a shared scene joined):
    // the actors it binds here must act for it (or it waits on them: issue 251's softlock),
    // but they stay with their owner (no targeting: two games never pull one NPC to and fro,
    // issue 200), and nothing is guessed in by our Link's nearness or its own AI.
    constexpr const char* kScrub = "Enemy_Scrub_Deku";
    struct Out {
        bool partner = false;
        bool targeting = false;
    };
    auto run = [&](bool ours, std::optional<bool> member, std::optional<std::uint64_t> own_at) {
        Sync sync;
        sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
        Seen scrub = Acting(0x100, 0x11, 100, 20.0f, "Sleep", kScrub);
        Tick(sync, 0, {scrub});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, 2);
        sync.SetEvent(true, 10 * kMs, ours);
        scrub.event_member = member;
        std::optional<std::uint8_t> flags;
        for (std::uint64_t t = 10; t < 700; t += 66) {
            sync.Receive(OwnerSample(id, 2, t * kMs, 20.0f, 100, "Sleep"));
            if (own_at && t >= *own_at) {
                sync.NoteTakeover(0x100, t * kMs, "Appear");
            }
            for (const auto& p : All<proto::ObjectPresent>(Tick(sync, t * kMs, {scrub}))) {
                flags = p.objects[0].flags;
            }
        }
        return Out{sync.EventPartner(0x100, 700 * kMs), flags && (*flags & kTargeting) != 0};
    };
    const Out bound_theirs = run(false, true, {});
    CHECK(bound_theirs.partner);          // acts for the event here: no owner's motion, changes wait
    CHECK_FALSE(bound_theirs.targeting);  // not claimed: stays with its owner
    const Out bound_ours = run(true, true, {});
    CHECK(bound_ours.partner);
    CHECK(bound_ours.targeting);          // our Link's event claims it, as row 97
    const Out guessed_theirs = run(false, std::nullopt, 200);
    CHECK_FALSE(guessed_theirs.partner);  // its own AI in another player's event: not ours to take
    CHECK_FALSE(guessed_theirs.targeting);
    CHECK(run(true, std::nullopt, 200).targeting);  // in our Link's event it is, as before
}

TEST_CASE("objects: a boss or a fish our event binds stays unclaimed (row 97)") {
    // A boss runs its own changes in our event through CoopBossNative::SetEvent and keeps its
    // fight's owner; a fish keeps the lure rules (issue 151). Neither is an event partner, even
    // bound by the event and near, nor through its own AI acting.
    for (const std::string name : {"Enemy_DungeonBoss_Goma", "Enemy_MiddleBoss_Test", "Animal_Fish_Bass"}) {
        CAPTURE(name);
        for (const bool member : {true, false}) {
            Sync sync;
            sync.SetPlayer(proto::Vec3{0.0f, 0.0f, 0.0f});
            Seen actor = Acting(0x100, 0x11, 100, 5.0f, "Wait", name);
            Tick(sync, 0, {actor});
            const proto::ObjectId id = *sync.IdOf(0x100);
            Own(sync, id, 2);
            sync.SetEvent(true, 10 * kMs);
            actor.event_member = member ? std::optional<bool>(true) : std::nullopt;
            std::optional<std::uint8_t> flags;
            for (std::uint64_t t = 10; t < 700; t += 66) {
                sync.Receive(OwnerSample(id, 2, t * kMs, 5.0f, 100, "Wait"));
                sync.NoteTakeover(0x100, t * kMs, "Appear");
                for (const auto& p : All<proto::ObjectPresent>(Tick(sync, t * kMs, {actor}))) {
                    flags = p.objects[0].flags;
                }
            }
            CHECK_FALSE(sync.EventPartner(0x100, 700 * kMs));
            CHECK(sync.EventPartnerList(700 * kMs).empty());
            CHECK(sync.EventPartners(700 * kMs) == 0);
        }
    }
}

TEST_CASE("objects: a boss's minion is made by the game that owns the boss, the others make that one (native boss sync)") {
    const std::string dodongo = "Enemy_DungeonBoss_Dodongo";
    const std::string baby = "Enemy_DungeonBoss_Dodongo_SpawnBabyDodongo";
    CHECK(CoopObjects::MinionMaker(baby) == dodongo);
    CHECK(CoopObjects::MinionMaker("Enemy_Shabom_BigOctoChild") == "Enemy_MiddleBoss_BigOcto");
    CHECK(CoopObjects::MinionMaker("Enemy_DungeonBoss_Dodongo_Attention").empty());  // made by each game's boss
    CHECK(CoopObjects::MinionMaker("Enemy_Stalchild").empty());

    // A owns King Dodongo: its baby is shared as a made id.
    Sync a;
    Seen boss_a = Enemy(0x100, 0x77, 40, 0.0f, dodongo);
    Tick(a, 0, {boss_a}, 1);
    const proto::ObjectId boss_id = *a.IdOf(0x100);
    Own(a, boss_id, 1);
    Tick(a, 50 * kMs, {boss_a}, 1);  // (the owner is known long before a baby comes)
    const Step made = Tick(a, 100 * kMs, {boss_a, Enemy(0x110, ~0ULL, 4, 5.0f, baby)}, 1);
    const auto spawns = All<proto::ObjectSpawn>(made);
    REQUIRE(spawns.size() == 1);
    CHECK(spawns[0].id.kind == proto::ObjectKind::made);
    CHECK(spawns[0].id.maker == 1);
    CHECK(spawns[0].actor == baby);
    CHECK(a.Removes().empty());

    // B's copy of the boss (owned by A) makes its own baby too: deleted, not shared.
    Sync b;
    Seen boss_b = Enemy(0x200, 0x77, 40, 0.0f, dodongo);
    Tick(b, 0, {boss_b}, 2);
    Own(b, boss_id, 1);
    Tick(b, 50 * kMs, {boss_b}, 2);
    const Step own = Tick(b, 100 * kMs, {boss_b, Enemy(0x210, ~0ULL, 4, 5.2f, baby)}, 2);
    CHECK(All<proto::ObjectSpawn>(own).empty());
    CHECK_FALSE(b.IdOf(0x210));
    CHECK(b.Removes() == std::vector<std::uint64_t>{0x210});
    // A's spawn reaches B: B makes A's baby.
    b.Receive(spawns[0]);
    Tick(b, 150 * kMs, {boss_b}, 2);
    const auto creates = b.Creates();
    REQUIRE(creates.size() == 1);
    CHECK(creates[0].name == baby);
    CHECK(b.OwnerOf(spawns[0].id) == 1);

    // No owner known for the boss: as before (matched by order, a placed id).
    Sync c;
    const Step lone = Tick(c, 100 * kMs, {Enemy(0x310, ~0ULL, 4, 5.0f, baby)}, 1);
    CHECK(All<proto::ObjectSpawn>(lone).empty());
    REQUIRE(c.IdOf(0x310));
    CHECK(c.IdOf(0x310)->kind == proto::ObjectKind::placed);
}

// deku-visuals-b (0.10.1 playtest: the lid did not move for the other player).
TEST_CASE("objects: a chest opened elsewhere opens its lid here, then shows its open idle") {
    CHECK(CoopObjects::ChestOpening("Obj_TBox_A_01") == "FrontOpenSmall");
    CHECK(CoopObjects::ChestOpening("Obj_TBox_A_02") == "FrontOpen");
    CHECK(CoopObjects::ChestOpening("Obj_TBox_A_03_NoBake") == "FrontOpen");
    CHECK(CoopObjects::ChestOpening("Obj_TBox_A_04") == "FrontOpenSmall");
    CHECK(CoopObjects::ChestOpening("Obj_TBox_A_06") == "FrontOpenSmall");
    CHECK(CoopObjects::ChestOpening("Obj_TBox_GlassTruth_A_01") == "FrontOpenSmall");
    CHECK(CoopObjects::ChestOpening("Obj_HrlCity_Shop_TBox_A_01").empty());
    CHECK(CoopObjects::PlaysOver("CloseWait", "Close"));
    CHECK(CoopObjects::PlaysOver("FrontOpen", "FrontOpen|Close"));
    CHECK(CoopObjects::PlaysOver("CloseWait", "FrontOpen|Close"));
    CHECK_FALSE(CoopObjects::PlaysOver("Rattle", "FrontOpen|Close"));
    CHECK(CoopObjects::PlaysOver("Rattle", ""));

    Sync sync;
    Seen chest = Chest(0x500);
    chest.name = "Obj_TBox_A_02";
    chest.animation = proto::Animation{"CloseWait", 0.0f, 1.0f};
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.Take(chest.hash);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    std::vector<PlayWish> plays;
    sync.Plays(30 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "FrontOpen");
    CHECK(plays[0].only_over == "Close");
    CHECK(plays[0].frame == 0.0f);
    // The lid opens; still the opening while it runs.
    chest.animation->name = "FrontOpen";
    Tick(sync, 100 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 1000 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    plays.clear();
    sync.Plays(1000 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "FrontOpen");
    // After kChestLidUs: the open idle, over the finished opening (or the closed idle).
    Tick(sync, 1700 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    plays.clear();
    sync.Plays(1700 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "OpenWait");
    CHECK(plays[0].only_over == "FrontOpen|Close");
    // Still the opening's last frame for a walk: not "opened twice", not removed.
    Tick(sync, 1800 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    chest.animation->name = "OpenWait";
    Tick(sync, 1900 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    plays.clear();
    sync.Plays(1900 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].only_over == "Close");
    Tick(sync, 9000 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
    CHECK(sync.ChestsOpenedTwice() == 0);
    bool lid = false, shows = false;
    for (const auto& n : sync.TakeNotes()) {
        lid |= n.find("the lid opens here ('FrontOpen')") != std::string::npos;
        shows |= n.find("shows open here") != std::string::npos;
    }
    CHECK(lid);
    CHECK(shows);
}

TEST_CASE("objects: a chest whose opening never takes gets the open idle after a second") {
    Sync sync;
    Seen chest = Chest(0x500);
    chest.animation = proto::Animation{"CloseWait", 0.0f, 1.0f};
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.Take(chest.hash);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 900 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    std::vector<PlayWish> plays;
    sync.Plays(900 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "FrontOpenSmall");
    Tick(sync, 1100 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    plays.clear();
    sync.Plays(1100 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].name == "OpenWait");
    CHECK(plays[0].only_over == "Close");
    chest.animation->name = "OpenWait";
    Tick(sync, 1200 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    Tick(sync, 5000 * kMs, {chest, Enemy(0x100, 0x11, 2)});
    CHECK(sync.Removes().empty());
}

// deku-visuals-b (0.10.1 playtest: webs went in the other game, but did not burn).
TEST_CASE("objects: a web burnt elsewhere burns here before it goes") {
    CHECK(CoopObjects::IsWeb("Obj_InsideTheDekuTree_SpiderWeb_A_01"));
    CHECK(CoopObjects::IsWeb("Obj_InsideTheDekuTree_InteractiveSpiderWebParts_A_02"));
    CHECK_FALSE(CoopObjects::IsWeb("Obj_TBox_A_01"));
    Sync sync;
    Seen web;
    web.address = 0x800;
    web.name = "Obj_InsideTheDekuTree_SpiderWeb_A_01";
    web.hash = 0x6b1c0e44a1f0d2c3;
    web.position = {30.0f, 0.0f, 30.0f};
    web.life = proto::ObjectLife{1, 1};
    web.animation = proto::Animation{"Wait", 0.0f, 1.0f};
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.Take(web.hash);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {web, Enemy(0x100, 0x11, 2)});
    std::vector<PlayWish> plays;
    sync.Plays(30 * kMs, plays);
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].address == 0x800);
    CHECK(plays[0].name == "Burn");
    CHECK(plays[0].frame == 0.0f);
    // No life 0 and no remove while it burns.
    web.animation->name = "Burn";
    auto step = Tick(sync, 100 * kMs, {web, Enemy(0x100, 0x11, 2)});
    CHECK(step.writes.empty());
    CHECK(sync.Removes().empty());
    plays.clear();
    sync.Plays(100 * kMs, plays);
    CHECK(plays.empty());  // asked once: it never restarts
    step = Tick(sync, 2000 * kMs, {web, Enemy(0x100, 0x11, 2)});
    CHECK(step.writes.empty());
    // Burnt: then its own break (life 0) as before.
    step = Tick(sync, 2700 * kMs, {web, Enemy(0x100, 0x11, 2)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].address == 0x800);
    CHECK(step.writes[0].life == 0);
    bool said = false;
    for (const auto& n : sync.TakeNotes()) said |= n.find("burns here ('Burn' played)") != std::string::npos;
    CHECK(said);
}

TEST_CASE("objects: a web whose burn never takes is broken as before") {
    Sync sync;
    Seen web;
    web.address = 0x800;
    web.name = "Obj_InsideTheDekuTree_SpiderWeb_A_02";
    web.hash = 0x6b1c0e44a1f0d2c4;
    web.position = {30.0f, 0.0f, 30.0f};
    web.life = proto::ObjectLife{1, 1};
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    sync.Take(web.hash);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {web, Enemy(0x100, 0x11, 2)});
    CHECK(Tick(sync, 500 * kMs, {web, Enemy(0x100, 0x11, 2)}).writes.empty());
    const auto step = Tick(sync, 1100 * kMs, {web, Enemy(0x100, 0x11, 2)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 0);
    std::vector<PlayWish> plays;
    sync.Plays(1100 * kMs, plays);
    CHECK(plays.empty());
}

TEST_CASE("objects: a copy dead here whose owner still lives after the resends is counted once") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 0)});
    Tick(sync, 130 * kMs, {Enemy(0x100, 0x11, 0)});
    for (const std::uint64_t t : {1200, 2300, 3400, 4500}) {
        sync.Receive(OwnerSample(id, 2, t * kMs, 0.0f, 2, "Wait"));
        CHECK(Tick(sync, t * kMs, {Enemy(0x100, 0x11, 0)}).writes.empty());
    }
    CHECK(sync.SplitKills() == 1);
}

TEST_CASE("objects: a boss heal goes only through the game's Life step, as a negative sum") {
    using CoopObjects::LifeAsk;
    using CoopObjects::LifeRoute;
    using CoopObjects::RouteLife;
    LifeAsk a;
    a.native = true;
    a.heal = true;
    a.life = 21;
    a.old = 1;
    a.step_seen = true;
    a.has_component = true;
    auto r = RouteLife(a);
    CHECK(r.route == LifeRoute::step);
    CHECK(r.amount == -20);
    a.stale = -20;  // the earlier raise was never taken: taken back, the whole raise again
    r = RouteLife(a);
    CHECK(r.route == LifeRoute::step);
    CHECK(r.amount == -20);
    a.old = 21;  // already there: nothing
    CHECK(RouteLife(a).route == LifeRoute::lost);
    a.old = 1;
    a.step_seen = false;  // no Life step hook: never a write
    CHECK(RouteLife(a).route == LifeRoute::lost);
}

TEST_CASE("objects: a boss copy held at 1 that becomes ours takes its old owner's life") {
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 21, 50.0f, kBoss)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    // Held at 1 here (HeldLifeStep) while the owner's boss is at 21. Our hits that took it there were SENT
    // (the second Tick, kDamageSumUs later) and the owner's game refused them: its life 21 is the life to take.
    // (Still unsent at the hand-over, they would come off the old owner's life instead: H3.)
    Tick(sync, 100 * kMs, {Enemy(0x100, 0x11, 1, 50.0f, kBoss)});
    const auto sent = Tick(sync, 200 * kMs, {Enemy(0x100, 0x11, 1, 50.0f, kBoss)});
    const auto hits = All<proto::Damage>(sent);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].amount == 20);
    proto::HandoverState h;
    h.player_number = 2;
    h.id = id;
    h.epoch = 1;
    h.state.life = proto::ObjectLife{21, 21};
    sync.Receive(h);
    Own(sync, id, 1, 1);
    const auto step = Tick(sync, 300 * kMs, {Enemy(0x100, 0x11, 1, 50.0f, kBoss)});
    const auto raise = std::find_if(step.writes.begin(), step.writes.end(),
                                    [](const CoopObjects::LifeWrite& w) { return w.heal; });
    REQUIRE(raise != step.writes.end());
    CHECK(raise->address == 0x100);
    CHECK(raise->life == 21);
    CHECK(raise->hurt == 0);
    CHECK_FALSE(raise->direct);
}

TEST_CASE("objects: at the session's end a boss copy held at 1 takes its owner's last life (row 127)") {
    // Navi's report: the host's game crashed, the server dropped, and the guests' Gohma copies, held at 1 for their
    // owner, died of the next hit. Before the Reset each live copy takes its owner's last life (no network data).
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 21, 50.0f, kBoss)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    // Our hits on the copy went to the owner; HeldLifeStep held it at 1 here. The owner's boss is at 12.
    Tick(sync, 50 * kMs, {Enemy(0x100, 0x11, 1, 50.0f, kBoss)});
    Tick(sync, 200 * kMs, {Enemy(0x100, 0x11, 1, 50.0f, kBoss)});  // (sent: none left unsent for H3)
    proto::ObjectSample s;
    s.player_number = 2;
    s.id = id;
    s.state.life = proto::ObjectLife{12, 21};
    sync.Receive(s);
    Tick(sync, 250 * kMs, {Enemy(0x100, 0x11, 1, 50.0f, kBoss)});
    std::vector<CoopObjects::LifeWrite> keep;
    sync.KeepBossLives(1, keep);
    REQUIRE(keep.size() == 1);
    CHECK(keep[0].address == 0x100);
    CHECK(keep[0].heal);
    CHECK(keep[0].life == 12);
    CHECK_FALSE(keep[0].direct);
}

TEST_CASE("objects: at the session's end nothing is given to our own boss, a dead one, a plain enemy or an unheard one") {
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    std::vector<CoopObjects::LifeWrite> keep;
    {  // ours: our game judged it all along
        Sync sync;
        Tick(sync, 0, {Enemy(0x100, 0x11, 5, 50.0f, kBoss)});
        Own(sync, *sync.IdOf(0x100), 1);
        sync.KeepBossLives(1, keep);
        CHECK(keep.empty());
    }
    {  // the owner said it died (life 0): it dies here too
        Sync sync;
        Tick(sync, 0, {Enemy(0x100, 0x11, 1, 50.0f, kBoss)});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, 2);
        proto::ObjectSample s;
        s.player_number = 2;
        s.id = id;
        s.state.life = proto::ObjectLife{0, 21};
        sync.Receive(s);
        Tick(sync, 50 * kMs, {Enemy(0x100, 0x11, 1, 50.0f, kBoss)});
        sync.KeepBossLives(1, keep);
        CHECK(std::none_of(keep.begin(), keep.end(), [](const CoopObjects::LifeWrite& w) { return w.heal; }));
        keep.clear();
    }
    {  // no owner's life heard
        Sync sync;
        Tick(sync, 0, {Enemy(0x100, 0x11, 1, 50.0f, kBoss)});
        Own(sync, *sync.IdOf(0x100), 2);
        sync.KeepBossLives(1, keep);
        CHECK(keep.empty());
    }
    {  // not a boss: an enemy copy is never healed (the native rule)
        Sync sync;
        Tick(sync, 0, {Enemy(0x100, 0x11, 1)});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, 2);
        proto::ObjectSample s;
        s.player_number = 2;
        s.id = id;
        s.state.life = proto::ObjectLife{3, 4};
        sync.Receive(s);
        Tick(sync, 50 * kMs, {Enemy(0x100, 0x11, 1)});
        sync.KeepBossLives(1, keep);
        CHECK(keep.empty());
    }
}

TEST_CASE("objects: at the session's end our boss copy's hit not sent yet comes off its owner's life (H3)") {
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    std::vector<CoopObjects::LifeWrite> keep;
    {  // owner at 12, our unsent hit of 2 already in our copy's 10: nothing given back
        Sync sync;
        Tick(sync, 0, {Enemy(0x100, 0x11, 12, 0.0f, kBoss)});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, 2, 1);
        sync.Receive(LifeSample(id, 12, 21));
        Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 12, 0.0f, kBoss)});
        Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 10, 0.0f, kBoss)});  // our hit of 2, summed, not sent yet
        sync.KeepBossLives(1, keep);
        CHECK(keep.empty());  // (was: raised back to 12)
    }
    {  // owner at 2 (another player's hit), our unsent hit of 2: its death, through its own Life step, once
        Sync sync;
        Tick(sync, 0, {Enemy(0x100, 0x11, 3, 0.0f, kBoss)});
        const proto::ObjectId id = *sync.IdOf(0x100);
        Own(sync, id, 2, 1);
        sync.Receive(LifeSample(id, 3, 21));
        Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 3, 0.0f, kBoss)});
        Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 1, 0.0f, kBoss)});  // our hit of 2 (held at 1), not sent yet
        sync.Receive(LifeSample(id, 2, 21));
        Tick(sync, 50 * kMs, {Enemy(0x100, 0x11, 1, 0.0f, kBoss)});
        keep.clear();
        sync.KeepBossLives(1, keep);
        REQUIRE(keep.size() == 1);
        CHECK(keep[0].direct);
        CHECK_FALSE(keep[0].heal);
        CHECK(keep[0].life == 0);
        CHECK(keep[0].hurt == 1);
        keep.clear();
        sync.KeepBossLives(1, keep);
        CHECK(keep.empty());  // given once
    }
}

TEST_CASE("objects: a boss copy with more life than its old owner's is lowered through its Life step (boss-hits)") {
    // hs-boss-full 20261115-122137: B took Gohma at 21 while A's was 17; the plain lower waited for a replay
    // that never came ("nothing given"), so the hand-over healed her.
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 21, 50.0f, kBoss)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    proto::HandoverState h;
    h.player_number = 2;
    h.id = id;
    h.epoch = 1;
    h.state.life = proto::ObjectLife{17, 21};
    sync.Receive(h);
    Own(sync, id, 1, 1);
    auto step = Tick(sync, 200 * kMs, {Enemy(0x100, 0x11, 21, 50.0f, kBoss)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].direct);
    CHECK(step.writes[0].hurt == 4);
    CHECK(step.writes[0].life == 17);
    CHECK_FALSE(step.writes[0].heal);
    // Taken: no hit of ours goes out for it.
    step = Tick(sync, 300 * kMs, {Enemy(0x100, 0x11, 17, 50.0f, kBoss)});
    CHECK(All<proto::Damage>(step).empty());
    CHECK(step.writes.empty());
    // A kill in the hand-over (its old owner's life 0): never a write on a boss either; its own Life step to 0,
    // given once, runs its own death here (fable-g2-bosses; only the owner's life 0 is a death).
    Sync dead;
    Tick(dead, 0, {Enemy(0x100, 0x11, 21, 50.0f, kBoss)});
    const proto::ObjectId did = *dead.IdOf(0x100);
    Own(dead, did, 2);
    h.id = did;
    h.state.life = proto::ObjectLife{0, 21};
    dead.Receive(h);
    Own(dead, did, 1, 1);
    step = Tick(dead, 200 * kMs, {Enemy(0x100, 0x11, 21, 50.0f, kBoss)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].direct);
    CHECK(step.writes[0].life == 0);
    CHECK(step.writes[0].hurt == 21);
    CHECK(All<proto::ObjectRemove>(step).empty());
}

TEST_CASE("objects: an enemy that is not a boss still never heals across a hand-over") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 1, 50.0f)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    proto::HandoverState h;
    h.player_number = 2;
    h.id = id;
    h.epoch = 1;
    h.state.life = proto::ObjectLife{2, 2};
    sync.Receive(h);
    Own(sync, id, 1, 1);
    const auto step = Tick(sync, 200 * kMs, {Enemy(0x100, 0x11, 1, 50.0f)});
    CHECK(std::none_of(step.writes.begin(), step.writes.end(), [](const CoopObjects::LifeWrite& w) { return w.heal; }));
}

// Hand-over life (fable-g2-bosses, rows 104/118, issue 150): the life the new owner takes has every hit in it.
namespace {
proto::HandoverState Handed(const proto::ObjectId& id, std::int32_t life, std::int32_t max, std::uint16_t epoch = 1) {
    proto::HandoverState h;
    h.player_number = 2;
    h.id = id;
    h.epoch = epoch;
    h.state.life = proto::ObjectLife{life, max};
    return h;
}
proto::Damage HitBy(const proto::ObjectId& id, std::uint8_t player, std::int32_t amount, std::uint16_t epoch = 0) {
    proto::Damage d;
    d.player_number = player;
    d.id = id;
    d.epoch = epoch;
    d.amount = amount;
    return d;
}
bool NoPlainWrite(const std::vector<LifeWrite>& writes) {
    return std::all_of(writes.begin(), writes.end(),
                       [](const LifeWrite& w) { return w.direct || w.heal || w.hurt > 0; });
}
}  // namespace

TEST_CASE("objects: an owner asked for its boss hands over the life with its pending DAMAGE in it (H1)") {
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync owner;
    owner.SetHurt(true);
    Tick(owner, 0, {Enemy(0x200, 0x11, 21, 0.0f, kBoss)});
    const proto::ObjectId id = *owner.IdOf(0x200);
    Own(owner, id, 1);
    owner.Receive(HitBy(id, 2, 2));
    REQUIRE(Tick(owner, 100 * kMs, {Enemy(0x200, 0x11, 21, 0.0f, kBoss)}).writes.size() == 1);  // the replay
    owner.Receive(proto::HandoverAsk{id, 0, 2});  // before its game took it
    const auto answer = All<proto::HandoverState>(Tick(owner, 150 * kMs, {Enemy(0x200, 0x11, 21, 0.0f, kBoss)}));
    REQUIRE(answer.size() == 1);
    REQUIRE(answer[0].state.life);
    CHECK(answer[0].state.life->life == 19);
    // Nothing pending: the life as read.
    owner.Receive(proto::HandoverAsk{id, 0, 2});
    const auto later = All<proto::HandoverState>(Tick(owner, 900 * kMs, {Enemy(0x200, 0x11, 19, 0.0f, kBoss)}));
    REQUIRE(later.size() == 1);
    CHECK(later[0].state.life->life == 19);
}

TEST_CASE("objects: a boss copy's own hit not sent yet comes off the life it is handed (H3)") {
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 19, 0.0f, kBoss)});  // our hit of 2, summed, not sent yet
    sync.Receive(Handed(id, 10, 21));
    Own(sync, id, 1, 1);
    auto step = Tick(sync, 50 * kMs, {Enemy(0x100, 0x11, 19, 0.0f, kBoss)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].direct);
    CHECK(step.writes[0].life == 8);
    CHECK(step.writes[0].hurt == 11);
    CHECK(All<proto::Damage>(step).empty());
    step = Tick(sync, 300 * kMs, {Enemy(0x100, 0x11, 8, 0.0f, kBoss)});
    CHECK(All<proto::Damage>(step).empty());
    CHECK(step.writes.empty());

    // An enemy with less life than its old owner's keeps its own (our hit is in it): nothing written.
    Sync enemy;
    Tick(enemy, 0, {Enemy(0x100, 0x11, 2)});
    const proto::ObjectId eid = *enemy.IdOf(0x100);
    Own(enemy, eid, 2);
    Tick(enemy, 10 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(enemy, 20 * kMs, {Enemy(0x100, 0x11, 1)});
    enemy.Receive(Handed(eid, 2, 2));
    Own(enemy, eid, 1, 1);
    step = Tick(enemy, 50 * kMs, {Enemy(0x100, 0x11, 1)});
    CHECK(step.writes.empty());
    CHECK(All<proto::Damage>(step).empty());
}

TEST_CASE("objects: a boss whose own unsent hit ends it in the hand-over dies through its Life step, once") {
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 19, 0.0f, kBoss)});  // our 2 = all she had left at her owner's
    sync.Receive(Handed(id, 2, 21));
    Own(sync, id, 1, 1);
    auto step = Tick(sync, 50 * kMs, {Enemy(0x100, 0x11, 19, 0.0f, kBoss)});
    CHECK(NoPlainWrite(step.writes));
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].direct);
    CHECK(step.writes[0].life == 0);
    CHECK(step.writes[0].hurt == 19);
    CHECK(All<proto::ObjectRemove>(step).empty());  // its own game's death, not a removal
    // Her game holds her (a stop): our hit that crossed the hand-over gives no second death.
    sync.Receive(HitBy(id, 1, 1));
    step = Tick(sync, 100 * kMs, {Enemy(0x100, 0x11, 1, 0.0f, kBoss)});
    CHECK(step.writes.empty());
}

TEST_CASE("objects: our own DAMAGE that crossed the hand-over is taken off the life we were handed (H4)") {
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    sync.Receive(Handed(id, 21, 21));
    Own(sync, id, 1, 1);
    CHECK(Tick(sync, 50 * kMs, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)}).writes.empty());
    sync.Receive(HitBy(id, 1, 3));  // given back by the server
    auto step = Tick(sync, 100 * kMs, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].direct);
    CHECK(step.writes[0].life == 18);
    CHECK(step.writes[0].hurt == 3);
    // After the window: nothing.
    sync.Receive(HitBy(id, 1, 3));
    CHECK(Tick(sync, 2500 * kMs, {Enemy(0x100, 0x11, 18, 0.0f, kBoss)}).writes.empty());

    // An enemy that took its old owner's life: written, to 0 a death.
    Sync enemy;
    Tick(enemy, 0, {Enemy(0x100, 0x11, 2)});
    const proto::ObjectId eid = *enemy.IdOf(0x100);
    Own(enemy, eid, 2);
    enemy.Receive(Handed(eid, 1, 2));
    Own(enemy, eid, 1, 1);
    step = Tick(enemy, 50 * kMs, {Enemy(0x100, 0x11, 2)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 1);
    enemy.Receive(HitBy(eid, 1, 1));
    step = Tick(enemy, 100 * kMs, {Enemy(0x100, 0x11, 1)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].life == 0);
    CHECK_FALSE(step.writes[0].direct);
    CHECK(All<proto::ObjectRemove>(step).size() == 1);

    // An enemy that kept its own life (lower than the old owner's): the hit is in it already.
    Sync kept;
    Tick(kept, 0, {Enemy(0x100, 0x11, 1)});
    const proto::ObjectId kid = *kept.IdOf(0x100);
    Own(kept, kid, 2);
    kept.Receive(Handed(kid, 2, 2));
    Own(kept, kid, 1, 1);
    Tick(kept, 50 * kMs, {Enemy(0x100, 0x11, 1)});
    kept.Receive(HitBy(kid, 1, 1));
    CHECK(Tick(kept, 100 * kMs, {Enemy(0x100, 0x11, 1)}).writes.empty());
}

TEST_CASE("objects: after a hand-over our hits on a copy count again once it moves on (D1)") {
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 2)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    Own(sync, id, 2);
    sync.Receive(Handed(id, 2, 2));
    Own(sync, id, 1, 1);
    Tick(sync, 50 * kMs, {Enemy(0x100, 0x11, 2)});
    Own(sync, id, 3, 2);  // a stun hands it on within the 2 s
    Tick(sync, 100 * kMs, {Enemy(0x100, 0x11, 2)});
    Tick(sync, 200 * kMs, {Enemy(0x100, 0x11, 1)});  // our hit on the copy
    const auto damage = All<proto::Damage>(Tick(sync, 350 * kMs, {Enemy(0x100, 0x11, 1)}));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].amount == 1);
    CHECK(damage[0].epoch == 2);
}

TEST_CASE("objects: a dying copy boss gets its remaining life once when its owner's is dead (review-boss-0114)") {
    // The real order: the owner's killing hit reaches our copy as a replay, our game refuses it (never written)
    // and the 400 ms wait ends; only then does the owner's outro_start come and the glue mark the boss dying.
    // Nothing else would give our copy its death: its whole life goes to its own Life step once (direct).
    const std::string boss = "Enemy_DungeonBoss_Barinade";
    const auto body = [&boss](std::int32_t life) { return Enemy(0x100, 0x11, life, 0.0f, boss); };
    Sync copy;
    copy.SetHurt(true);
    Tick(copy, 0, {body(5)});
    const proto::ObjectId id = *copy.IdOf(0x100);
    Own(copy, id, 2);  // player 2's; we are player 1
    Tick(copy, 10 * kMs, {body(5)});
    copy.Receive(Dead(id, 2));
    auto step = Tick(copy, 20 * kMs, {body(5)});
    REQUIRE(step.writes.size() == 1);
    CHECK_FALSE(step.writes[0].direct);
    step = Tick(copy, 500 * kMs, {body(5)});  // refused, the wait over: never written
    CHECK(step.writes.empty());
    step = Tick(copy, 800 * kMs, {body(5)});  // not dying yet: nothing
    CHECK(step.writes.empty());
    copy.SetDying({boss});
    step = Tick(copy, 900 * kMs, {body(5)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].direct);
    CHECK(step.writes[0].life == 0);
    CHECK(step.writes[0].hurt == 5);
    CHECK(copy.DyingFinished() == 1);
    CHECK(Tick(copy, 1000 * kMs, {body(5)}).writes.empty());  // once
    CHECK(copy.DyingFinished() == 1);

    // Dying while its owner's still lives (our own game's death event, a copy): nothing.
    Sync early;
    early.SetHurt(true);
    Tick(early, 0, {body(5)});
    Own(early, *early.IdOf(0x100), 2);
    early.SetDying({boss});
    CHECK(Tick(early, 10 * kMs, {body(5)}).writes.empty());
    // Our own boss dying: its own game's death, nothing given.
    Sync ours;
    ours.SetHurt(true);
    Tick(ours, 0, {body(5)});
    const proto::ObjectId own_id = *ours.IdOf(0x100);
    Own(ours, own_id, 1);
    ours.SetDying({boss});
    CHECK(Tick(ours, 10 * kMs, {body(5)}).writes.empty());
    CHECK(ours.DyingFinished() == 0);
}

TEST_CASE("objects: a boss is pre-battle while a body plays Sleep or Pre_Battle_* (boss-copy-b)") {
    constexpr std::array<std::string_view, 1> kGohma{"Enemy_DungeonBoss_Gohma"};
    CHECK(CoopObjects::IsPreBattleAnimation("Sleep"));
    CHECK(CoopObjects::IsPreBattleAnimation("Pre_Battle_Ceiling_Wait"));
    CHECK_FALSE(CoopObjects::IsPreBattleAnimation("Walk_Start"));
    CHECK_FALSE(CoopObjects::IsPreBattleAnimation("C01-Enemy_DungeonBoss_Gohma-A-0"));
    Seen boss = Chest(0x960, 0);
    boss.name = "Enemy_DungeonBoss_Gohma";
    CHECK_FALSE(CoopObjects::BossPreBattle({}, kGohma).has_value());      // not seen: unknown
    CHECK_FALSE(CoopObjects::BossPreBattle({boss}, kGohma).has_value());  // no animation read: unknown
    boss.animation = proto::Animation{"Pre_Battle_Ceiling_Find", 0.0f, 1.0f};
    CHECK(CoopObjects::BossPreBattle({boss}, kGohma) == std::optional<bool>(true));
    boss.animation = proto::Animation{"Walk", 0.0f, 1.0f};
    CHECK(CoopObjects::BossPreBattle({boss}, kGohma) == std::optional<bool>(false));
    Seen egg = Chest(0x970, 0);
    egg.name = "Enemy_Gohma_Egg";
    egg.animation = proto::Animation{"Sleep", 0.0f, 1.0f};
    CHECK(CoopObjects::BossPreBattle({boss, egg}, kGohma) == std::optional<bool>(false));  // not a body
}

TEST_CASE("objects: a copy boss whose death has begun gets its owner's refused killing hit in its Life step (#105)") {
    // The owner's Gohma died (its dead remove reaches our copy as a hurt to life 0); her game refuses it outside
    // her eye. In a fight a boss copy is never written; once her death has begun (the glue's SetDying) the rest
    // goes into her own Life step (direct), which CoopBossNative lets end her, so the death setting finds her dead.
    const std::string boss = "Enemy_DungeonBoss_Gohma";
    for (const bool dying : {false, true}) {
        CAPTURE(dying);
        Sync copy;
        copy.SetHurt(true);
        if (dying) {
            copy.SetDying({boss});
        }
        const auto gohma = [&boss](std::int32_t life) { return Enemy(0x100, 0x11, life, 0.0f, boss); };
        Tick(copy, 0, {gohma(3)});
        const proto::ObjectId id = *copy.IdOf(0x100);
        Own(copy, id, 2);  // player 2's; we are player 1
        Tick(copy, 10 * kMs, {gohma(3)});
        copy.Receive(Dead(id, 2));
        auto step = Tick(copy, 20 * kMs, {gohma(3)});
        REQUIRE(step.writes.size() == 1);
        CHECK(step.writes[0].hurt == 3);
        CHECK_FALSE(step.writes[0].direct);
        step = Tick(copy, 500 * kMs, {gohma(3)});
        if (dying) {
            REQUIRE(step.writes.size() == 1);
            CHECK(step.writes[0].direct);
            CHECK(step.writes[0].hurt == 3);
            CHECK(step.writes[0].life == 0);
        } else {
            CHECK(step.writes.empty());
        }
        CHECK(copy.HurtsMissed() == 1);
        CHECK(copy.BossHitsStepped() == 0);  // (not boss-hits' own-boss rule)
    }
}

TEST_CASE("objects: Ganondorf and Volvagia are single bosses, Volvagia's other actors are each game's own") {
    // From the game's Pack/Actor (fable-g2-bosses): Ganondorf has no part actors; Volvagia's appear sign, breath,
    // breath field and cutscene actor are not enemies, so never shared and never owned.
    using CoopObjects::IsBoss;
    using CoopObjects::IsShared;
    using CoopObjects::PartOf;
    for (const char* boss : {"Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBoss_Volvagia"}) {
        CHECK(IsBoss(boss));
        CHECK(IsShared(boss));
        CHECK(PartOf(boss) == nullptr);
    }
    CHECK_FALSE(IsBoss("Enemy_DungeonBoss_Ganondorf_Dictionary"));
    for (const char* own : {"Dm_Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_Appear_Sign",
                            "DungeonBossVolvagia_FireBreath", "DungeonBossVolvagia_FireBreathField"}) {
        CHECK_FALSE(IsShared(own));
        CHECK(PartOf(own) == nullptr);
    }
}

TEST_CASE("objects: the test stun makes our boss copy say a stun to its owner, never for our own boss") {
    const std::string kBoss = "Enemy_DungeonBoss_Gohma";
    Sync sync;
    Tick(sync, 0, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    const proto::ObjectId id = *sync.IdOf(0x100);
    CHECK_FALSE(sync.TestStun(kBoss, std::nullopt, 1));  // no owner known yet
    Own(sync, id, 1);
    Tick(sync, 10 * kMs, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    CHECK_FALSE(sync.TestStun(kBoss, std::nullopt, 1));  // ours
    Own(sync, id, 2, 1);
    Tick(sync, 20 * kMs, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)});
    CHECK_FALSE(sync.TestStun("Enemy_DungeonBoss_Barinade", std::nullopt, 1));
    REQUIRE(sync.TestStun(kBoss, proto::Vec3{1.0f, 0.0f, 0.0f}, 1));
    const auto damage = All<proto::Damage>(Tick(sync, 30 * kMs, {Enemy(0x100, 0x11, 21, 0.0f, kBoss)}));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].amount == 0);
    CHECK(damage[0].epoch == 1);
}

TEST_CASE("objects: the real hit goes everywhere: copy's DAMAGE, the owner's replay, its samples, a third copy (protocol 14)") {
    // Every hit shows on every screen: each game's enemy takes the hit that happened (its weapon, element,
    // place) through its own damage step, not a stand-in sword hit it may refuse (King Dodongo, Gohma's eye).
    proto::HitRecord rec{};
    rec[4] = 0x00090003;  // a bomb's attack kind, say
    rec[15] = 0x3f800000;
    const std::string kName = "Enemy_Bokoblin";
    const auto E = [](std::uint64_t a, std::uint64_t h, std::int32_t life, float x, const std::string& name) {
        Seen s = Enemy(a, h, life, x, name);
        s.life = proto::ObjectLife{life, 8};
        return s;
    };
    // The hitter: its copy's own hit (a moved hit id, own_hit) with its record goes with the DAMAGE.
    Sync copy;
    Tick(copy, 0, {E(0x100, 0x11, 6, 0.0f, kName)});
    const proto::ObjectId id = *copy.IdOf(0x100);
    Own(copy, id, 2, 1);
    Tick(copy, 10 * kMs, {E(0x100, 0x11, 6, 0.0f, kName)});
    Seen hit = E(0x100, 0x11, 4, 0.0f, kName);
    hit.own_hit = true;
    hit.hit_record = rec;
    Tick(copy, 20 * kMs, {hit});
    auto damage = All<proto::Damage>(Tick(copy, 130 * kMs, {E(0x100, 0x11, 4, 0.0f, kName)}));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].amount == 2);
    CHECK(damage[0].hit_record == std::optional<proto::HitRecord>(rec));
    // (the next DAMAGE without a new own hit has none)
    Tick(copy, 140 * kMs, {E(0x100, 0x11, 3, 0.0f, kName)});
    damage = All<proto::Damage>(Tick(copy, 260 * kMs, {E(0x100, 0x11, 3, 0.0f, kName)}));
    REQUIRE(damage.size() == 1);
    CHECK_FALSE(damage[0].hit_record);

    // The owner: the DAMAGE's record goes into its replay; the hit its game then took goes in its samples.
    Sync owner;
    owner.SetHurt(true);
    Tick(owner, 0, {E(0x200, 0x11, 6, 0.0f, kName)});
    const proto::ObjectId oid = *owner.IdOf(0x200);
    Own(owner, oid, 1);
    proto::Damage d{};
    d.player_number = 2;
    d.id = oid;
    d.amount = 2;
    d.hit_record = rec;
    owner.Receive(d);
    auto step = Tick(owner, 10 * kMs, {E(0x200, 0x11, 6, 0.0f, kName)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].hurt == 2);
    CHECK(step.writes[0].record == std::optional<proto::HitRecord>(rec));
    Seen took = E(0x200, 0x11, 4, 0.0f, kName);
    took.replay_hit = true;
    took.hit_record = rec;
    // (its next sample is due kSampleEveryUs after the one at 10 ms)
    const std::uint64_t took_at = 10 * kMs + Sync::kSampleEveryUs;
    auto samples = All<proto::ObjectSample>(Tick(owner, took_at, {took}));
    REQUIRE_FALSE(samples.empty());
    REQUIRE(samples.back().state.hit);
    CHECK(samples.back().state.hit->seq == 1);
    CHECK(samples.back().state.hit->record == rec);
    // Past kHitShowUs its samples carry it no more.
    samples = All<proto::ObjectSample>(Tick(owner, took_at + Sync::kHitShowUs, {E(0x200, 0x11, 3, 0.0f, kName)}));
    REQUIRE_FALSE(samples.empty());
    CHECK_FALSE(samples.back().state.hit);

    // A third game's copy: the owner's sample with a fall and a new hit seq replays that hit, once.
    Sync third;
    third.SetHurt(true);
    Tick(third, 0, {E(0x300, 0x11, 6, 0.0f, kName)});
    const proto::ObjectId tid = *third.IdOf(0x300);
    Own(third, tid, 2, 3);
    Tick(third, 10 * kMs, {E(0x300, 0x11, 6, 0.0f, kName)});
    proto::ObjectSample s{2, tid, 3, {}};
    s.state.time = {15 * kMs, 1};
    s.state.position = proto::Vec3{0.0f, 0.0f, 0.0f};
    s.state.life = proto::ObjectLife{4, 8};
    s.state.hit = proto::ObjectHit{1, rec};
    third.Receive(s);
    step = Tick(third, 20 * kMs, {E(0x300, 0x11, 6, 0.0f, kName)});
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].hurt == 2);
    CHECK(step.writes[0].record == std::optional<proto::HitRecord>(rec));
}

TEST_CASE("objects: row 149: IsBoss is the real bosses only, their parts are IsBossPart and follow their root") {
    using CoopObjects::BossPartRoot;
    using CoopObjects::IsBoss;
    using CoopObjects::IsBossOrPart;
    using CoopObjects::IsBossPart;
    // The roots of every boss fight stay bosses.
    for (const char* root : {"Enemy_DungeonBoss_Gohma", "Enemy_DungeonBoss_Dodongo", "Enemy_DungeonBoss_Barinade",
                             "Enemy_DungeonBoss_PhantomGanon", "Enemy_DungeonBoss_Volvagia", "Enemy_DungeonBoss_Morpha",
                             "Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_Twinrova",
                             "Enemy_KotakeKoumeBattleManager", "Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBoss_Ganon",
                             "Enemy_MiddleBoss_DarkLink", "Enemy_MiddleBoss_BigOcto", "Enemy_DeadHand"}) {
        CAPTURE(root);
        CHECK(IsBoss(root));
        CHECK(IsBossOrPart(root));
        CHECK_FALSE(IsBossPart(root));
    }
    // Their parts are no bosses of their own (King Dodongo's roll centre had its own owner and boss lock).
    for (const char* part : {"Enemy_DungeonBoss_Dodongo_Attention_RollingCenter", "Enemy_DungeonBoss_Dodongo_Attention",
                             "Enemy_DungeonBoss_Dodongo_SpawnBabyDodongo", "Enemy_DungeonBoss_Gohma_Egg",
                             "Enemy_DungeonBoss_Barinade_Arm", "Enemy_DungeonBoss_Barinade_Jellyfish",
                             "Enemy_DungeonBoss_Morpha_Attention_Spin", "Enemy_DungeonBoss_Morpha_Tentacle",
                             "Enemy_DungeonBoss_BongoBongo_LeftHand", "Enemy_DungeonBoss_BongoBongo_RightHand",
                             "Enemy_DungeonBoss_Kotake", "Enemy_DungeonBoss_Koume", "Enemy_DungeonBoss_Ganon_Crystal",
                             "Enemy_MiddleBoss_BigOcto_Attention"}) {
        CAPTURE(part);
        CHECK_FALSE(IsBoss(part));
        CHECK(IsBossPart(part));
        CHECK(IsBossOrPart(part));  // (the rules every boss actor shares: life, hits, slots)
    }
    // A part not named as a boss (Gohma's larva, a baby Dodongo) is a part, not IsBossOrPart; plain enemies neither.
    CHECK(IsBossPart("Enemy_Gohma_Larva"));
    CHECK_FALSE(IsBossOrPart("Enemy_Gohma_Larva"));
    CHECK_FALSE(IsBossPart("Enemy_Dodongo_Attention"));  // (a plain enemy's part)
    CHECK_FALSE(IsBossPart("Enemy_Dodongo"));
    CHECK_FALSE(IsBossPart("Enemy_DungeonBoss_Dodongo_Attention_RollingCenter_Dictionary"));

    // Its root in a walk: by name, the nearest in reach, else the next root name (Twinrova's witches).
    using coop::protocol::Vec3;
    const std::vector<std::string_view> names{"Enemy_Dodongo", "Enemy_DungeonBoss_Dodongo",
                                              "Enemy_DungeonBoss_Dodongo_Attention_RollingCenter"};
    const std::vector<Vec3> places{Vec3{1, 0, 0}, Vec3{50, 0, 0}, Vec3{0, 0, 0}};
    CHECK(BossPartRoot("Enemy_DungeonBoss_Dodongo_Attention_RollingCenter", places[2], names, places) == 1u);
    CHECK_FALSE(BossPartRoot("Enemy_DungeonBoss_Dodongo", places[1], names, places));  // a root follows none
    CHECK_FALSE(BossPartRoot("Enemy_Dodongo_Attention", places[0], names, places));  // no boss part
    CHECK_FALSE(BossPartRoot("Enemy_DungeonBoss_BongoBongo_LeftHand", places[0], names, places));  // no root here
    const std::vector<std::string_view> witches{"Enemy_DungeonBoss_Twinrova", "Enemy_DungeonBoss_Kotake"};
    const std::vector<Vec3> at{Vec3{0, 0, 0}, Vec3{5, 0, 0}};
    CHECK(BossPartRoot("Enemy_DungeonBoss_Kotake", at[1], witches, at) == 0u);
    // Out of reach (a larva far from Gohma): none.
    const std::vector<std::string_view> gohma{"Enemy_DungeonBoss_Gohma", "Enemy_Gohma_Larva"};
    const std::vector<Vec3> far{Vec3{0, 0, 0}, Vec3{500, 0, 0}};
    CHECK_FALSE(BossPartRoot("Enemy_Gohma_Larva", far[1], gohma, far));
}

TEST_CASE("objects: row 149: a boss part never linked sends no boss lock of its own, a root still does") {
    using CoopObjects::GroupInput;
    using CoopObjects::GroupsOf;
    const proto::ObjectId centre{proto::ObjectKind::placed, 0, 8};
    const proto::ObjectId twinrova{proto::ObjectKind::placed, 0, 9};
    // King Dodongo not in this walk: his roll centre sends nothing (no group lock of its own).
    const std::vector<GroupInput> walk{{centre, "Enemy_DungeonBoss_Dodongo_Attention_RollingCenter", {0, 0, 0}},
                                       {twinrova, "Enemy_DungeonBoss_Twinrova", {0, 0, 0}}};
    const auto groups = GroupsOf(walk);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].member == twinrova);  // Twinrova, a part with no battle manager here, keeps its own boss lock
    CHECK(groups[0].flags == proto::object_group::boss);
}
