// Tests for enemy shots (lichii/hook/coop_enemy_shots.cpp) and how the spawn
// sync and its director use them.
#include <doctest/doctest.h>

#include <array>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_enemy_shots.h"
#include "coop_objects_sync.h"
#include "coop_spawn.h"

namespace proto = coop::protocol;
namespace es = CoopEnemyShots;
using es::Owner;
using es::Shooter;
using es::Verdict;

namespace {

constexpr std::uint64_t kMs = 1000;
const std::string kScene = "Field_Hyrule";

CoopSpawn::Seen Shot(std::uint64_t address, float x, std::string name = "Octorok_Bullet") {
    CoopSpawn::Seen s;
    s.address = address;
    s.name = std::move(name);
    s.position = {x, 1.0f, 2.0f};
    s.vtable = 0xbeef;
    return s;
}

template <typename T>
std::vector<T> Of(const std::vector<proto::Message>& send) {
    std::vector<T> out;
    for (const auto& m : send) {
        if (const auto* v = std::get_if<T>(&m)) {
            out.push_back(*v);
        }
    }
    return out;
}

class FakeGuest : public CoopPuppet::Guest {
public:
    void Map(std::uint64_t address, std::size_t size) {
        for (std::uint64_t p = address & ~0xfffULL; p < address + size; p += 0x1000) {
            pages_[p];
        }
    }
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = pages_.find((address + i) & ~0xfffULL);
            if (it == pages_.end()) return false;
            o[i] = it->second[(address + i) & 0xfff];
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = pages_.find((address + i) & ~0xfffULL);
            if (it == pages_.end()) return false;
            it->second[(address + i) & 0xfff] = p[i];
        }
        return true;
    }
    template <typename T>
    void Set(std::uint64_t address, const T& v) {
        REQUIRE(Write(address, &v, sizeof(T)));
    }

private:
    std::map<std::uint64_t, std::array<std::uint8_t, 0x1000>> pages_;
};

} // namespace

TEST_CASE("enemy shots: the names are known, with their makers") {
    for (const char* name :
         {"Octorok_Bullet", "Dodongo_Bullet_Fire", "DungeonBossDodongo_Bullet_Fire",
          "DungeonBossDodongo_Bullet_StrongFire", "ScrubDeku_DekuNut",
          "SkullKid_Bullet", "PoeHyrule_Bullet_FireBall", "FlareDancer_Bullet_FireBall",
          "DungeonBossGanondrof_Bullet_LightBall", "DungeonBossPhantomGanon_Bullet_LightBall",
          "PhantomGanon_Bullet_Thunder", "Twinrova_Bullet_FireBullet",
          "KotakeKoumeTwinrova_Bullet_GuardBreakIce", "Ganon_Bullet_Thunder",
          "Ganondorf_Bullet_Thunder_Shockwave", "Enemy_MoblinClub_ShockWave"}) {
        CAPTURE(name);
        CHECK(es::IsEnemyShot(name));
        CHECK_FALSE(es::MakersOf(name).empty());
        CHECK(CoopSpawn::IsShared(name));  // the spawn sync carries them
        CHECK(CoopSpawn::Flies(name));
        CHECK_FALSE(CoopSpawn::Explodes(name));
        CHECK_FALSE(CoopObjects::IsShared(name));  // not an enemy of the objects sync
    }
    for (const char* name : {"Enemy_Octorok", "Shootable_DekuSeed", "Item_DekuNut", "BeamosBeam",
                             "Twinrova_Bullet_FireBeam_MirrorShieldReflect",
                             "MirrorShield_Bullet_FireChargeBeam", "Enemy_Scrub_Deku",
                             "Enemy_Scrub_Business_Seed"}) {
        CAPTURE(name);
        CHECK_FALSE(es::IsEnemyShot(name));
    }
    CHECK(CoopObjects::IsShared("Enemy_Octorok"));
    CHECK(es::MakerRange("Octorok_Bullet") == doctest::Approx(10.0f));
    CHECK(es::MakerRange("DungeonBossDodongo_Bullet_Fire") == doctest::Approx(30.0f));
}

TEST_CASE("enemy shots: the nearest maker's owner decides") {
    const std::vector<Shooter> shooters{
        {"Enemy_Octorok", {0.0f, 1.0f, 2.0f}, Owner::ours},
        {"Enemy_Octorok", {6.0f, 1.0f, 2.0f}, Owner::theirs},
        {"Enemy_Keese", {1.0f, 1.0f, 2.0f}, Owner::theirs},  // nearer, but no Octorok
        {"Enemy_Scrub_Deku", {40.0f, 1.0f, 2.0f}, Owner::unknown},
    };
    CHECK(es::Decide("Octorok_Bullet", {1.5f, 1.0f, 2.0f}, shooters) == Verdict::send);
    CHECK(es::Decide("Octorok_Bullet", {5.0f, 1.0f, 2.0f}, shooters) == Verdict::hide);
    // Too far from every Octorok: no maker, left alone.
    CHECK(es::Decide("Octorok_Bullet", {20.0f, 1.0f, 2.0f}, shooters) == Verdict::keep);
    // A maker with no owner known yet: left alone, not sent.
    CHECK(es::Decide("ScrubDeku_DekuNut", {41.0f, 1.0f, 2.0f}, shooters) == Verdict::keep);
    // No shooters at all (objects sync off): each game its own.
    CHECK(es::Decide("Octorok_Bullet", {0.0f, 1.0f, 2.0f}, {}) == Verdict::keep);
    // A boss shot is given to its boss from far off.
    const std::vector<Shooter> boss{{"Enemy_DungeonBoss_Dodongo", {0.0f, 0.0f, 0.0f}, Owner::theirs}};
    CHECK(es::Decide("DungeonBossDodongo_Bullet_Fire", {20.0f, 0.0f, 0.0f}, boss) == Verdict::hide);
    // The Lost Woods stage scrubs are each game's own (not shared): kept.
    const std::vector<Shooter> stage{{"Enemy_Scrub_Deku", {0.0f, 0.0f, 0.0f}, Owner::theirs}};
    CHECK(es::Decide("ScrubForestStage_DekuNut", {1.0f, 0.0f, 0.0f}, stage) == Verdict::keep);
}

TEST_CASE("enemy shots: the shooters noted by the objects sync go stale") {
    es::NoteShooters({{"Enemy_Octorok", {}, Owner::ours}}, 10'000'000);
    CHECK(es::Shooters(10'500'000).size() == 1);
    CHECK(es::Shooters(10'000'000 + es::kFreshUs + 1).empty());
    CHECK(es::Shooters(9'000'000).empty());
    es::NoteShooters({}, 0);
}

TEST_CASE("enemy shots: a copy enemy's own shot is stopped at the game's create request (Hookshot)") {
    const std::uint64_t before = es::StoppedAtCreate();
    es::NoteShooters({{"Enemy_Octorok", {10.0f, 0.0f, 0.0f}, Owner::theirs},
                      {"Enemy_Octorok", {60.0f, 0.0f, 0.0f}, Owner::ours},
                      {"Enemy_Octorok", {120.0f, 0.0f, 0.0f}, Owner::unknown}},
                     5'000'000);
    CHECK(es::StopAtCreate("Octorok_Bullet", {11.0f, 0.0f, 0.5f}));        // the copy's: never made here
    CHECK_FALSE(es::StopAtCreate("Octorok_Bullet", {61.0f, 0.0f, 0.0f}));  // ours: made and sent
    CHECK_FALSE(es::StopAtCreate("Octorok_Bullet", {121.0f, 0.0f, 0.0f})); // no owner known: left alone
    CHECK_FALSE(es::StopAtCreate("Octorok_Bullet", {300.0f, 0.0f, 0.0f})); // no maker near
    CHECK_FALSE(es::StopAtCreate("Octorok_Bullet", {0.0f, 0.0f, 0.0f}));   // the place was not read
    CHECK_FALSE(es::StopAtCreate("Item_Bomb", {11.0f, 0.0f, 0.5f}));       // no enemy shot
    CHECK(es::StoppedAtCreate() == before + 1);
    es::NoteShooters({}, 6'000'000);  // (a new scene's walk: nobody)
    CHECK_FALSE(es::StopAtCreate("Octorok_Bullet", {11.0f, 0.0f, 0.5f}));
}

TEST_CASE("enemy shots: the shooter the game's shoot code names decides, not the place") {
    // Two Octoroks side by side, ours nearer the shot's place than the copy that fires it.
    es::NoteShooters({{"Enemy_Octorok", {10.0f, 0.0f, 0.0f}, Owner::theirs, 0x1000},
                      {"Enemy_Octorok", {11.0f, 0.0f, 0.0f}, Owner::ours, 0x2000},
                      {"Enemy_Octorok", {12.0f, 0.0f, 0.0f}, Owner::unknown, 0x3000}},
                     7'000'000);
    const proto::Vec3 at{11.1f, 0.0f, 0.0f};
    CHECK_FALSE(es::StopAtCreate("Octorok_Bullet", at));     // by place: ours is nearest
    CHECK(es::StopAtCreate("Octorok_Bullet", at, 0x1000));   // the copy fired it: never made
    CHECK_FALSE(es::StopAtCreate("Octorok_Bullet", at, 0x2000));  // ours fired it
    CHECK_FALSE(es::StopAtCreate("Octorok_Bullet", at, 0x3000));  // no owner known: left alone
    // A shooter that is not a shared enemy here: the place decides, as before.
    CHECK(es::StopAtCreate("Octorok_Bullet", {9.9f, 0.0f, 0.0f}, 0x9999));
    // The copy's shot is stopped even when its place was not read.
    CHECK(es::StopAtCreate("Octorok_Bullet", {0.0f, 0.0f, 0.0f}, 0x1000));
    CHECK_FALSE(es::StopAtCreate("Item_Bomb", at, 0x1000));  // no enemy shot
    es::NoteShooters({}, 8'000'000);
}

TEST_CASE("enemy shots: the owner's game sends its enemy's shot") {
    CoopSpawn::Sync sync;
    sync.SetShooters({{"Enemy_Octorok", {0.0f, 1.0f, 2.0f}, Owner::ours}});
    std::vector<proto::Message> send;
    std::vector<CoopSpawn::Copy> copies;
    sync.Tick(kScene, 1, 1'000 * kMs, {}, {}, send, copies);  // first walk: nothing new
    sync.Tick(kScene, 1, 1'033 * kMs, {Shot(0x1000, 1.0f)}, {}, send, copies);
    const auto spawns = Of<proto::ObjectSpawn>(send);
    REQUIRE(spawns.size() == 1);
    CHECK(spawns[0].actor == "Octorok_Bullet");
    CHECK((spawns[0].flags & proto::object_spawn::projectile) != 0);
    REQUIRE(sync.EnemyShotsDecided().size() == 1);
    CHECK(sync.EnemyShotsDecided()[0].verdict == Verdict::send);
    CHECK(sync.Mine() == 1);
    CHECK(sync.Doom().empty());

    // It flies: samples follow, and its end is a remove.
    send.clear();
    sync.Tick(kScene, 1, 1'066 * kMs, {Shot(0x1000, 2.0f)}, {}, send, copies);
    CHECK(Of<proto::ObjectSample>(send).size() == 1);
    CHECK(sync.EnemyShotsDecided().empty());  // decided once
    send.clear();
    for (int i = 0; i < 3; ++i) {
        sync.Tick(kScene, 1, (1'100 + 33 * i) * kMs, {}, {}, send, copies);
    }
    CHECK(Of<proto::ObjectRemove>(send).size() == 1);
}

TEST_CASE("enemy shots: a copy enemy's own shot is not sent and is deleted") {
    CoopSpawn::Sync sync;
    sync.SetShooters({{"Enemy_Octorok", {0.0f, 1.0f, 2.0f}, Owner::theirs}});
    std::vector<proto::Message> send;
    std::vector<CoopSpawn::Copy> copies;
    sync.Tick(kScene, 2, 1'000 * kMs, {}, {}, send, copies);
    sync.Tick(kScene, 2, 1'033 * kMs, {Shot(0x2000, 1.0f)}, {}, send, copies);
    CHECK(send.empty());
    REQUIRE(sync.EnemyShotsDecided().size() == 1);
    CHECK(sync.EnemyShotsDecided()[0].verdict == Verdict::hide);
    CHECK(sync.Doom().empty());  // one walk only: could be a copy not reported yet
    sync.Tick(kScene, 2, 1'066 * kMs, {Shot(0x2000, 1.5f)}, {}, send, copies);
    REQUIRE(sync.Doom().size() == 1);
    CHECK(sync.Doom()[0].address == 0x2000);
    CHECK(sync.Doom()[0].vtable == 0xbeef);
    sync.Tick(kScene, 2, 1'100 * kMs, {Shot(0x2000, 2.0f)}, {}, send, copies);
    CHECK(sync.Doom().empty());  // once
    CHECK(send.empty());
    CHECK(sync.Mine() == 0);

    // With the hide off it stays, still not sent.
    CoopSpawn::Sync keep;
    keep.SetHide(false);
    keep.SetShooters({{"Enemy_Octorok", {0.0f, 1.0f, 2.0f}, Owner::theirs}});
    keep.Tick(kScene, 2, 1'000 * kMs, {}, {}, send, copies);
    for (int i = 1; i < 4; ++i) {
        keep.Tick(kScene, 2, (1'000 + 33 * i) * kMs, {Shot(0x2000, 1.0f)}, {}, send, copies);
        CHECK(keep.Doom().empty());
    }
    CHECK(send.empty());
}

TEST_CASE("enemy shots: with no owner known a shot is left alone") {
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<CoopSpawn::Copy> copies;
    sync.Tick(kScene, 1, 1'000 * kMs, {}, {}, send, copies);
    for (int i = 1; i < 4; ++i) {
        sync.Tick(kScene, 1, (1'000 + 33 * i) * kMs, {Shot(0x3000, 1.0f)}, {}, send, copies);
        CHECK(sync.Doom().empty());
    }
    CHECK(send.empty());
    CHECK(sync.Mine() == 0);
}

TEST_CASE("enemy shots: the owner's shot is shown as a copy, and our own shot is not taken for it") {
    CoopSpawn::Sync sync;
    sync.SetShooters({{"Enemy_Octorok", {0.0f, 1.0f, 2.0f}, Owner::theirs}});
    std::vector<proto::Message> send;
    std::vector<CoopSpawn::Copy> copies;
    sync.Tick(kScene, 2, 1'000 * kMs, {}, {}, send, copies);

    proto::ObjectSpawn spawn;
    spawn.player_number = 1;
    spawn.id = {proto::ObjectKind::made, 1, 77};
    spawn.actor = "Octorok_Bullet";
    spawn.flags = proto::object_spawn::projectile;
    spawn.state.time = {1'010 * kMs, 0};
    spawn.state.position = proto::Vec3{1.0f, 1.0f, 2.0f};
    spawn.state.velocity = proto::Vec3{10.0f, 0.0f, 0.0f};
    sync.Receive(spawn);
    copies.clear();
    // Our copy Octorok fired too, at the same spot: not claimed as the copy.
    sync.Tick(kScene, 2, 1'033 * kMs, {Shot(0x2000, 1.1f)}, {}, send, copies);
    REQUIRE(copies.size() == 1);
    CHECK(copies[0].actor == "Octorok_Bullet");
    CHECK(copies[0].act == CoopSpawn::Act::show);
    CHECK(copies[0].position[0] >= 1.0f);  // on the owner's path
    const std::uint64_t key = copies[0].key;
    copies.clear();
    // The director made the copy at 0x4000 and says so: ours is still doomed,
    // the copy never is.
    sync.Tick(kScene, 2, 1'066 * kMs, {Shot(0x2000, 1.2f), Shot(0x4000, 1.5f)}, {{key, 0x4000}},
              send, copies);
    REQUIRE(sync.Doom().size() == 1);
    CHECK(sync.Doom()[0].address == 0x2000);
    CHECK(send.empty());
}

TEST_CASE("enemy shots: the director deletes a doomed shot only if it is still that actor") {
    namespace addr = CoopPuppet::addr;
    constexpr std::uint64_t base = 0x8000000;
    constexpr std::uint64_t shot = 0x40000000, other = 0x41000000;
    FakeGuest guest;
    guest.Map(shot, 0x600);
    guest.Map(other, 0x600);
    guest.Set(shot, std::uint64_t{0xbeef});
    guest.Set(shot + 0x538 + 0x10, shot);  // on the list: its node's owner is itself
    guest.Set(other, std::uint64_t{0xcafe});  // another actor took the address
    guest.Set(other + 0x538 + 0x10, other);

    std::vector<std::string> said;
    CoopSpawn::Director d([&](const std::string& s) { said.push_back(s); });
    d.SetInput(base, {});
    d.SetDoomed({{shot, 0xbeef}, {other, 0xbeef}, {0x50000000, 0xbeef}});
    auto c = d.Step(guest, false, 0, 0);
    REQUIRE(c);
    CHECK(c->function == base + addr::kRequestDelete);
    CHECK(c->x[0] == shot);
    CHECK_FALSE(d.Step(guest, true, 0, 0));  // the others are not that actor any more
    CHECK(d.Deleted() == 1);
    // Not twice.
    d.SetDoomed({{shot, 0xbeef}});
    CHECK_FALSE(d.Step(guest, false, 0, 16));
    CHECK(d.Deleted() == 1);
}
