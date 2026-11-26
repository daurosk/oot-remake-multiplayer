// SPDX-License-Identifier: GPL-3.0-or-later
// Row 177: shots sent back by a player whose game does not own them (lichii/hook/coop_shot_reflect.h).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <map>

#include "../../lichii/hook/coop_shot_reflect.h"
#include "../../lichii/hook/coop_spawn.h"

namespace {

namespace addr = CoopShotReflect::addr;
using CoopShotReflect::Way;

class FakeGuest : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = bytes.find(address + i);
            o[i] = it == bytes.end() ? 0 : it->second;
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* b = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) {
            bytes[address + i] = b[i];
        }
        return true;
    }
    template <typename T>
    void Put(std::uint64_t address, T v) {
        Write(address, &v, sizeof v);
    }
    // A handle at `handle` that points to `actor` (holder +0x40).
    void Handle(std::uint64_t handle, std::uint64_t actor) {
        const std::uint64_t holder = 0x90000000 + (handle & 0xffffff) * 0x100;
        Put(handle + addr::kHandleHolder, holder);
        Put(holder + addr::kHolderActor, actor);
    }
    std::map<std::uint64_t, std::uint8_t> bytes;
};

constexpr std::uint64_t kShot = 0x1000000;
constexpr std::uint64_t kLink = 0x2000000;
constexpr std::uint64_t kPuppet = 0x3000000;
constexpr std::uint64_t kSensor = 0x4000000;
constexpr std::uint64_t kTgList = 0x4100000;
constexpr std::uint64_t kAtList = 0x4200000;
constexpr std::uint64_t kTgEntries = 0x4300000;
constexpr std::uint64_t kAtEntries = 0x4400000;

void Sensor(FakeGuest& g, std::uint32_t tg, std::uint32_t at) {
    g.Put(kShot + addr::kActorComponents, kShot + 0x8000);
    g.Put(kShot + 0x8000 + addr::kComponentSensor, kSensor);
    g.Put(kSensor + addr::kSensorTgList, kTgList);
    g.Put(kSensor + addr::kSensorAtList, kAtList);
    g.Put(kTgList + 0x8, std::uint32_t{8});
    g.Put(kTgList + 0x18, tg);
    g.Put(kTgList + 0x10, kTgEntries);
    g.Put(kAtList + 0x8, std::uint32_t{8});
    g.Put(kAtList + 0x18, at);
    g.Put(kAtList + 0x10, kAtEntries);
}

void TgHit(FakeGuest& g, std::uint32_t i, std::uint8_t kind, std::uint64_t attacker, bool second_handle = false) {
    const std::uint64_t e = kTgEntries + i * addr::kTgEntrySize;
    g.Put(e + addr::kTgKind, kind);
    g.Put(e + addr::kTgHandleFlags, static_cast<std::uint8_t>(second_handle ? 1u : 0u));
    g.Handle(e + (second_handle ? addr::kTgAttackerB : addr::kTgAttackerA), attacker);
}

void AtHit(FakeGuest& g, std::uint32_t i, bool guarded, std::uint64_t defender) {
    const std::uint64_t e = kAtEntries + i * addr::kAtEntrySize;
    g.Put(e + addr::kAtFlags, static_cast<std::uint8_t>(guarded ? 1u : 4u));
    g.Handle(e + addr::kAtDefender, defender);
}

}  // namespace

TEST_CASE("shot reflect: what the game sends back, from its ShootableParam") {
    CHECK(CoopShotReflect::ReflectedBy("DungeonBossGanondrof_Bullet_LightBall") == Way::melee);
    CHECK(CoopShotReflect::ReflectedBy("DungeonBossPhantomGanon_Bullet_LightBall") == Way::melee);
    CHECK(CoopShotReflect::ReflectedBy("Octorok_Bullet") == Way::guard);
    CHECK(CoopShotReflect::ReflectedBy("ScrubDeku_DekuNut") == Way::guard);
    CHECK(CoopShotReflect::ReflectedBy("ScrubForestStage_DekuNut") == Way::guard);
    // Not sent back by the game: never told, never asked.
    CHECK(CoopShotReflect::ReflectedBy("DungeonBossGanondrof_Bullet_ChargeLightBall") == Way::none);
    CHECK(CoopShotReflect::ReflectedBy("Dodongo_Bullet_Fire") == Way::none);
    CHECK(CoopShotReflect::ReflectedBy("Twinrova_Bullet_IceBeam") == Way::none);
    CHECK(CoopShotReflect::ReflectedBy("Twinrova_Bullet_IceBeam_MirrorShieldReflect") == Way::none);
    CHECK(CoopShotReflect::ReflectedBy("Enemy_MoblinClub_ShockWave") == Way::none);
    CHECK(CoopShotReflect::ReflectedBy("Octorok_Bullet_Other") == Way::none);
}

TEST_CASE("shot reflect: our Link's sword on a light ball copy, from its own taken hits") {
    FakeGuest g;
    Sensor(g, 2, 0);
    TgHit(g, 0, 2, kPuppet);  // a reflect kind, but not ours
    TgHit(g, 1, 5, kLink);    // ours, a kind that does not send it back
    CHECK_FALSE(CoopShotReflect::OurHit(g, kShot, kLink, Way::melee));
    TgHit(g, 1, 17, kLink);  // ours, a reflect kind (0x6000c: 2, 3, 17, 18)
    CHECK(CoopShotReflect::OurHit(g, kShot, kLink, Way::melee));
    const std::string said = CoopShotReflect::Describe(g, kShot);
    CHECK(said.find("taken kind 2 by 0x3000000") != std::string::npos);
    CHECK(said.find("taken kind 17 by 0x2000000") != std::string::npos);
    FakeGuest empty;
    CHECK(CoopShotReflect::Describe(empty, kShot).empty());
    // The attacker handle by the entry's flag: +0x78 when bit 0 is set.
    FakeGuest h;
    Sensor(h, 1, 0);
    TgHit(h, 0, 3, kLink, true);
    CHECK(CoopShotReflect::OurHit(h, kShot, kLink, Way::melee));
    // A melee shot is not sent back by a guard, nor the other way round.
    CHECK_FALSE(CoopShotReflect::OurHit(h, kShot, kLink, Way::guard));
    CHECK_FALSE(CoopShotReflect::OurHit(h, kShot, kLink, Way::none));
    CHECK_FALSE(CoopShotReflect::OurHit(h, kShot, 0, Way::melee));
}

TEST_CASE("shot reflect: kinds above the game's mask are never taken") {
    FakeGuest g;
    Sensor(g, 1, 0);
    TgHit(g, 0, 0x13, kLink);
    CHECK_FALSE(CoopShotReflect::OurHit(g, kShot, kLink, Way::melee));
    TgHit(g, 0, 0xff, kLink);
    CHECK_FALSE(CoopShotReflect::OurHit(g, kShot, kLink, Way::melee));
}

TEST_CASE("shot reflect: our Link's shield on a rock copy, from its own dealt hits") {
    FakeGuest g;
    Sensor(g, 0, 2);
    AtHit(g, 0, false, kLink);  // hit our Link, not guarded
    AtHit(g, 1, true, kPuppet);  // guarded, by another
    CHECK_FALSE(CoopShotReflect::OurHit(g, kShot, kLink, Way::guard));
    AtHit(g, 1, true, kLink);
    CHECK(CoopShotReflect::OurHit(g, kShot, kLink, Way::guard));
}

TEST_CASE("shot reflect: no sensor, no list, or a count past the capacity") {
    FakeGuest g;
    CHECK_FALSE(CoopShotReflect::OurHit(g, kShot, kLink, Way::melee));
    Sensor(g, 40, 0);  // count 40, capacity 8: only 8 read
    TgHit(g, 20, 2, kLink);
    CHECK_FALSE(CoopShotReflect::OurHit(g, kShot, kLink, Way::melee));
    TgHit(g, 7, 2, kLink);
    CHECK(CoopShotReflect::OurHit(g, kShot, kLink, Way::melee));
}

TEST_CASE("shot reflect: the maker's shooter record of the shot") {
    FakeGuest g;
    constexpr std::uint64_t kMaker = 0x5000000, kShooter = 0x5100000, kMap = 0x5200000;
    constexpr std::uint64_t kRecordsA = 0x5300000, kRecordsB = 0x5400000;
    g.Put(kMaker + addr::kActorComponents, kMaker + 0x8000);
    g.Put(kMaker + 0x8000 + addr::kComponentShooter, kShooter);
    g.Put(kShooter + addr::kShooterMap, kMap);
    g.Put(kShooter + addr::kShooterSlots, std::uint32_t{4});
    // slot 1: key 0x1111, 1 record (another shot); slot 3: key 0x2222, 3 records, ours at index 2.
    g.Put(kMap + 1 * addr::kMapEntrySize, std::uint32_t{0x1111});
    g.Put(kMap + 1 * addr::kMapEntrySize + addr::kEntryCount, std::uint32_t{1});
    g.Put(kMap + 1 * addr::kMapEntrySize + addr::kEntryRecords, kRecordsA);
    g.Handle(kRecordsA + addr::kRecordShot, 0x6000000);
    g.Put(kMap + 3 * addr::kMapEntrySize, std::uint32_t{0x2222});
    g.Put(kMap + 3 * addr::kMapEntrySize + addr::kEntryCount, std::uint32_t{3});
    g.Put(kMap + 3 * addr::kMapEntrySize + addr::kEntryRecords, kRecordsB);
    g.Handle(kRecordsB + 0 * addr::kRecordSize + addr::kRecordShot, 0x6100000);
    g.Handle(kRecordsB + 2 * addr::kRecordSize + addr::kRecordShot, kShot);
    const auto r = CoopShotReflect::FindRecord(g, kMaker, kShot);
    REQUIRE(r);
    CHECK(r->shooter == kShooter);
    CHECK(r->key == 0x2222);
    CHECK(r->index == 2);
    CHECK_FALSE(CoopShotReflect::FindRecord(g, kMaker, 0x7000000));  // not this maker's
    CHECK_FALSE(CoopShotReflect::FindRecord(g, 0x8000000, kShot));   // no shooter
    const auto call = CoopShotReflect::ReflectCall(0x80020000, *r);
    CHECK(call.function == 0x80020000 + 0x2eb3bd0);
    CHECK(call.x[0] == kShooter);
    CHECK(call.x[1] == 0x2222);
    CHECK(call.x[2] == 2);
    CHECK_FALSE(call.copy_game);

    // The queue: asked once, the call made on the calc thread, its answer noted; nothing then.
    CoopShotReflect::Queue q;
    CHECK_FALSE(q.Step(g, false, 0));
    q.Ask(kShot, "Octorok_Bullet", {0x8000000, kMaker}, 0x80020000);
    const auto c = q.Step(g, false, 0);
    REQUIRE(c);
    CHECK(c->x[0] == kShooter);
    CHECK_FALSE(q.Step(g, true, 1));
    CHECK(q.Asked() == 1);
    const auto notes = q.TakeNotes();
    REQUIRE(notes.size() == 2);
    CHECK(notes[1].find("took it") != std::string::npos);
    CHECK_FALSE(q.Step(g, false, 0));
    // A shot no maker has a record of: noted, no call.
    q.Ask(0x7000000, "Octorok_Bullet", {kMaker}, 0x80020000);
    CHECK_FALSE(q.Step(g, false, 0));
    CHECK(q.TakeNotes().size() == 1);
}

TEST_CASE("shot reflect: only DAMAGE cause reflected on our own shot asks the owner's game") {
    namespace proto = coop::protocol;
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<CoopSpawn::Copy> copies;
    // Our Octorok's rock: a shot of ours (our enemy fired it).
    sync.SetShooters({{"Enemy_Octorok", {0.0f, 1.0f, 2.0f}, CoopEnemyShots::Owner::ours, 0x5000000}});
    CoopSpawn::Seen shot;
    shot.address = kShot;
    shot.name = "Octorok_Bullet";
    shot.position = {1.0f, 1.0f, 2.0f};
    shot.vtable = 0xbeef;
    const std::vector<CoopSpawn::Seen> seen{shot};
    sync.Tick("Field", 1, 1'000'000, {}, {}, send, copies);  // first walk: nothing new
    sync.Tick("Field", 1, 1'033'000, seen, {}, send, copies);
    proto::ObjectId id;
    for (const auto& m : send) {
        if (const auto* s = std::get_if<proto::ObjectSpawn>(&m)) {
            id = s->id;
        }
    }
    REQUIRE(id.value != 0);
    proto::Damage hit;
    hit.player_number = 2;
    hit.id = id;
    hit.amount = 1;  // an ordinary DAMAGE: the objects sync's, not ours
    sync.Receive(hit);
    sync.Tick("Field", 1, 1'066'000, seen, {}, send, copies);
    CHECK(sync.TakeReflectAsks().empty());
    hit.amount = 0;
    hit.cause = static_cast<std::uint8_t>(proto::DamageCause::reflected);
    sync.Receive(hit);
    sync.Tick("Field", 1, 1'099'000, seen, {}, send, copies);
    const auto asks = sync.TakeReflectAsks();
    REQUIRE(asks.size() == 1);
    CHECK(asks[0].address == kShot);
    CHECK(asks[0].name == "Octorok_Bullet");
    CHECK(asks[0].player == 2);
    CHECK(sync.TakeReflectAsks().empty());
}

TEST_CASE("shot reflect: a copy our Link sent back goes to its owner as DAMAGE cause reflected") {
    namespace proto = coop::protocol;
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<CoopSpawn::Copy> copies;
    sync.Tick("Boss", 1, 1'000'000, {}, {}, send, copies);  // the scene first
    proto::ObjectSpawn s;
    s.player_number = 2;
    s.id = {proto::ObjectKind::made, 2, 0x77};
    s.actor = "DungeonBossPhantomGanon_Bullet_LightBall";
    s.flags = proto::object_spawn::projectile;
    s.state.time = {1'010'000, 0};
    s.state.position = proto::Vec3{1.0f, 2.0f, 3.0f};
    s.state.velocity = proto::Vec3{10.0f, 0.0f, 0.0f};
    sync.Receive(s);
    copies.clear();
    sync.Tick("Boss", 1, 1'033'000, {}, {}, send, copies);
    REQUIRE_FALSE(copies.empty());
    const std::uint64_t key = copies.front().key;
    send.clear();
    sync.SendReflected({key + 100}, send);  // not a copy of ours
    CHECK(send.empty());
    sync.SendReflected({key}, send);
    REQUIRE(send.size() == 1);
    const auto* d = std::get_if<proto::Damage>(&send[0]);
    REQUIRE(d);
    CHECK(d->id == s.id);
    CHECK(d->amount == 0);
    CHECK(d->cause == static_cast<std::uint8_t>(proto::DamageCause::reflected));
}
