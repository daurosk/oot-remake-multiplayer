// Tests for boss rooms together (lichii/hook/coop_boss.cpp).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "coop_boss.h"
#include "coop_boss_beaten.h"

namespace proto = coop::protocol;
using CoopBoss::Actions;
using CoopBoss::Local;
using CoopBoss::Sync;
using CoopBoss::Vec;

namespace {

constexpr std::uint64_t kS = 1'000'000;

const CoopBoss::Room& Forest() {
    return *CoopBoss::RoomOfScene("ForestTemple");
}

Local At(const char* scene, Vec p, bool event = false, std::uint8_t self = 1) {
    Local l;
    l.scene = scene;
    l.position = p;
    l.event = event;
    l.self = self;
    return l;
}

// A place in the Forest Temple outside the boss room (its entrance).
constexpr Vec kForestEntrance{0.0f, 101.5f, 78.4f};

std::vector<proto::BossState> States(const Actions& a) {
    std::vector<proto::BossState> out;
    for (const auto& m : a.send) {
        out.push_back(m.state);
    }
    return out;
}

bool Has(const Actions& a, proto::BossState s) {
    const auto v = States(a);
    return std::find(v.begin(), v.end(), s) != v.end();
}

proto::BossRoom From(std::uint8_t player, const CoopBoss::Room& room, proto::BossState state) {
    return {player, std::string(room.scene), room.index, state};
}

} // namespace

TEST_CASE("boss: the eight rooms") {
    const auto rooms = CoopBoss::Rooms();
    REQUIRE(rooms.size() == 8);
    std::set<std::string_view> scenes;
    for (std::size_t i = 0; i < rooms.size(); ++i) {
        const auto& r = rooms[i];
        CHECK(r.index == i);
        CHECK(r.index <= proto::kMaxBossRoom);
        scenes.insert(r.scene);
        CHECK(CoopBoss::RoomOfScene(r.scene) == &r);
        CHECK(CoopBoss::RoomByIndex(r.index) == &r);
        // The arrival point and every player's spot around it are in the room.
        CHECK(CoopBoss::Inside(r, r.arrive));
        for (std::uint8_t n = 1; n <= proto::kMaxPlayers; ++n) {
            CHECK(CoopBoss::Inside(r, CoopBoss::ArrivalFor(r, n)));
        }
        // All 16 spots differ, at least a unit apart.
        for (std::uint8_t a = 1; a <= proto::kMaxPlayers; ++a) {
            for (std::uint8_t b = a + 1; b <= proto::kMaxPlayers; ++b) {
                const Vec pa = CoopBoss::ArrivalFor(r, a);
                const Vec pb = CoopBoss::ArrivalFor(r, b);
                CHECK(std::hypot(pa.x - pb.x, pa.z - pb.z) > 1.0f);
            }
        }
        CHECK_FALSE(r.parts.empty());
        CHECK(r.parts[0] == r.boss);
        CHECK(r.intro.ends_with("_BossIn"));
        CHECK(r.outro.ends_with("_BossOut"));
    }
    CHECK(scenes.size() == 8);
    CHECK(rooms[0].era == CoopBoss::Era::child);
    CHECK(rooms[2].era == CoopBoss::Era::child);
    CHECK(rooms[3].era == CoopBoss::Era::adult);
    CHECK(CoopBoss::RoomByIndex(8) == nullptr);
    CHECK(CoopBoss::RoomOfScene("KokiriForest_Child") == nullptr);
}

TEST_CASE("boss: in the room or not") {
    CHECK(CoopBoss::RoomAt("ForestTemple", Forest().arrive) == &Forest());
    CHECK(CoopBoss::RoomAt("ForestTemple", kForestEntrance) == nullptr);
    CHECK(CoopBoss::RoomAt("WaterTemple", Forest().arrive) == nullptr);
    // Right place on the ground, wrong floor.
    Vec above = Forest().arrive;
    above.y += 40.0f;
    CHECK_FALSE(CoopBoss::Inside(Forest(), above));
    // The Shadow Temple's StartPos just before the boss room (above it).
    CHECK(CoopBoss::RoomAt("ShadowTemple", Vec{-138.5f, 42.0f, 21.2f}) == nullptr);
    CHECK_FALSE(CoopBoss::Inside(Forest(), Vec{NAN, 80.0f, -62.0f}));
    // Spot spread: two players never on the same point.
    CHECK(std::abs(CoopBoss::ArrivalFor(Forest(), 2).x - CoopBoss::ArrivalFor(Forest(), 4).x) > 2.0f);
    CHECK(std::abs(CoopBoss::ArrivalFor(Forest(), 1).z - CoopBoss::ArrivalFor(Forest(), 3).z) > 2.0f);
}

TEST_CASE("boss: entering sends in_room at once and every 2 s, leaving sends left") {
    Sync s;
    auto a = s.Tick(At("ForestTemple", kForestEntrance), 1 * kS);
    CHECK(a.send.empty());
    a = s.Tick(At("ForestTemple", Forest().arrive), 2 * kS);
    REQUIRE(a.send.size() == 1);
    CHECK(a.send[0].state == proto::BossState::in_room);
    CHECK(a.send[0].scene == "ForestTemple");
    CHECK(a.send[0].room == Forest().index);
    CHECK(a.send[0].player_number == 0);
    CHECK(s.phase() == Sync::Phase::waiting);
    CHECK(s.Tick(At("ForestTemple", Forest().arrive), 3 * kS).send.empty());
    a = s.Tick(At("ForestTemple", Forest().arrive), 4 * kS);
    CHECK(States(a) == std::vector{proto::BossState::in_room});
    a = s.Tick(At("ForestTemple", kForestEntrance), 5 * kS);
    CHECK(States(a) == std::vector{proto::BossState::left});
    CHECK(s.phase() == Sync::Phase::none);
}

TEST_CASE("boss: intro, fight and the boss's death") {
    Sync s;
    s.Tick(At("ForestTemple", Forest().arrive), 1 * kS);
    auto a = s.Tick(At("ForestTemple", Forest().arrive, true), 2 * kS);
    CHECK_FALSE(Has(a, proto::BossState::intro_start));  // no shut-in flag here: past the door at its end
    CHECK(s.phase() == Sync::Phase::intro);
    a = s.Tick(At("ForestTemple", Forest().arrive, false), 20 * kS);
    CHECK(Has(a, proto::BossState::intro_end));
    CHECK(s.phase() == Sync::Phase::fight);
    a = s.Tick(At("ForestTemple", Forest().arrive, true), 60 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    CHECK(s.phase() == Sync::Phase::outro);
    // No heartbeat after the boss is beaten.
    a = s.Tick(At("ForestTemple", Forest().arrive, true), 70 * kS);
    CHECK(a.send.empty());
    s.Tick(At("ForestTemple", Forest().arrive, false), 80 * kS);
    CHECK(s.phase() == Sync::Phase::done);
    CHECK(s.Tick(At("ForestTemple", Forest().arrive), 90 * kS).send.empty());
}

TEST_CASE("boss: no intro within 10 s means the fight has begun here, but moves no one in") {
    Sync s;
    s.Tick(At("ForestTemple", Forest().arrive), 1 * kS);
    auto a = s.Tick(At("ForestTemple", Forest().arrive), 11 * kS);
    CHECK_FALSE(Has(a, proto::BossState::intro_end));  // no event said we are past the door
    CHECK(States(a) == std::vector{proto::BossState::in_room});  // the heartbeat
    CHECK(s.phase() == Sync::Phase::fight);
}

TEST_CASE("boss: our own intro and death, read with no name, are told to the area rules (Gohma on Longshot)") {
    // 0.11 runs (boss-intro): Gohma's intro is no event the game asks by name; these rules saw it anyway.
    Sync s;
    s.Tick(At("ForestTemple", Forest().arrive), 1 * kS);
    auto a = s.Tick(At("ForestTemple", Forest().arrive), 11 * kS);  // no intro in 10 s: "fight" here
    REQUIRE(s.phase() == Sync::Phase::fight);
    CHECK(a.intro_here == nullptr);
    a = s.Tick(At("ForestTemple", Forest().arrive, true), 12 * kS);  // the real intro, after the wait
    CHECK(s.phase() == Sync::Phase::intro);
    CHECK(a.intro_here == nullptr);  // no shut-in flag here: told once it has run kMinIntroUs (issue 148)
    a = s.Tick(At("ForestTemple", Forest().arrive, true), 15 * kS);
    CHECK(a.intro_here == &Forest());
    CHECK(a.beaten_here == nullptr);
    CHECK(s.Tick(At("ForestTemple", Forest().arrive, true), 16 * kS).intro_here == nullptr);  // once
    a = s.Tick(At("ForestTemple", Forest().arrive, false), 30 * kS);
    CHECK(a.intro_here == nullptr);
    {
        // The same fight, its death with no reading of the boss: beaten as before, but not told (a guess).
        Sync g;
        g.Tick(At("ForestTemple", Forest().arrive), 1 * kS);
        g.Tick(At("ForestTemple", Forest().arrive), 11 * kS);
        g.Tick(At("ForestTemple", Forest().arrive, true), 12 * kS);
        g.Tick(At("ForestTemple", Forest().arrive, false), 30 * kS);
        CHECK(g.Tick(At("ForestTemple", Forest().arrive, true), 90 * kS).beaten_here == nullptr);
        CHECK(g.phase() == Sync::Phase::outro);
    }
    Local dying = At("ForestTemple", Forest().arrive, true);
    dying.boss_alive = false;  // the walk read it at life 0
    dying.boss_read_dead = true;
    a = s.Tick(dying, 90 * kS);
    CHECK(a.beaten_here == &Forest());
    CHECK(a.intro_here == nullptr);
    // A door's event on the way (not yet in the room) tells nothing.
    Sync w;
    w.Tick(At("ForestTemple", kForestEntrance), 1 * kS);
    CHECK(w.Tick(At("ForestTemple", kForestEntrance, true), 2 * kS).intro_here == nullptr);
}

TEST_CASE("boss: a player only near the room pulls no one, one whose intro started does (issue 148)") {
    // Deku Tree, player test: A came near the boss room; B was moved in and the door shut in B's game
    // while A, still on the way to it, was locked out.
    Sync b;
    b.Tick(At("ForestTemple", kForestEntrance, false, 2), 1 * kS);
    for (std::uint64_t t = 2; t <= 20; t += 2) {
        b.Receive(From(1, Forest(), proto::BossState::in_room));  // A on the way, heartbeat
        CHECK_FALSE(b.Tick(At("ForestTemple", kForestEntrance, false, 2), t * kS).pull);
    }
    // A's intro starts: A is inside and the door has shut behind A. Now B is moved in.
    b.Receive(From(1, Forest(), proto::BossState::intro_start));
    CHECK(b.Tick(At("ForestTemple", kForestEntrance, false, 2), 21 * kS).pull);
}

TEST_CASE("boss: the heartbeat says how far we are, so a late joiner moves in only for a real fight") {
    Sync a;
    auto out = a.Tick(At("ForestTemple", Forest().arrive), 1 * kS);
    CHECK(States(out) == std::vector{proto::BossState::in_room});
    out = a.Tick(At("ForestTemple", Forest().arrive), 3 * kS);
    CHECK(States(out) == std::vector{proto::BossState::in_room});  // only near: in_room
    a.Tick(At("ForestTemple", Forest().arrive, true), 4 * kS);      // the intro starts
    out = a.Tick(At("ForestTemple", Forest().arrive, true), 6 * kS);
    // The Forest Temple has no shut-in flag: past the door only once the intro is over (issue 148).
    CHECK(States(out) == std::vector{proto::BossState::in_room});
    a.Tick(At("ForestTemple", Forest().arrive, false), 7 * kS);     // the fight
    out = a.Tick(At("ForestTemple", Forest().arrive, false), 9 * kS);
    CHECK(States(out) == std::vector{proto::BossState::intro_end});
    // A game that joins now hears only the heartbeat: it is moved in.
    Sync late;
    late.Receive(out.send[0].player_number == 0 ? From(1, Forest(), out.send[0].state) : out.send[0]);
    CHECK(late.Tick(At("ForestTemple", kForestEntrance, false, 2), 10 * kS).pull);
    // Leaving the room: the next heartbeat in another room starts from in_room again.
    out = a.Tick(At("ForestTemple", kForestEntrance), 11 * kS);
    CHECK(States(out) == std::vector{proto::BossState::left});
}

TEST_CASE("boss: another player in the boss room pulls us in, five tries at most") {
    Sync s;
    s.Tick(At("ForestTemple", kForestEntrance, false, 2), 1 * kS);
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    auto a = s.Tick(At("ForestTemple", kForestEntrance, false, 2), 2 * kS);
    REQUIRE(a.pull);
    CHECK(a.pull->room == &Forest());
    CHECK_FALSE(a.pull->by_event);
    const Vec spot = CoopBoss::ArrivalFor(Forest(), 2);
    CHECK(a.pull->to.x == doctest::Approx(spot.x));
    CHECK(a.pull->to.z == doctest::Approx(spot.z));
    // Not again within 3 s.
    CHECK_FALSE(s.Tick(At("ForestTemple", kForestEntrance, false, 2), 3 * kS).pull);
    unsigned pulls = 1;
    for (std::uint64_t t = 5; t <= 30; ++t) {
        s.Receive(From(1, Forest(), proto::BossState::intro_end));  // the fight goes on
        if (s.Tick(At("ForestTemple", kForestEntrance, false, 2), t * kS).pull) {
            ++pulls;
        }
    }
    CHECK(pulls == Sync::kPullTries);
}

TEST_CASE("boss: an event of our own lets the scene change take the move") {
    Sync s;
    s.Receive(From(1, Forest(), proto::BossState::intro_start));
    auto a = s.Tick(At("ForestTemple", kForestEntrance, true, 2), 1 * kS);
    REQUIRE(a.pull);
    CHECK(a.pull->by_event);
}

TEST_CASE("boss: no pull from another dungeon, after the fight, or once the room is left") {
    Sync s;
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    CHECK_FALSE(s.Tick(At("WaterTemple", Vec{-4.5f, 118.6f, 43.6f}), 1 * kS).pull);
    // Heard 6 s ago and nothing since: over.
    CHECK_FALSE(s.Tick(At("ForestTemple", kForestEntrance), 8 * kS).pull);
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    s.Receive(From(1, Forest(), proto::BossState::left));
    CHECK_FALSE(s.Tick(At("ForestTemple", kForestEntrance), 9 * kS).pull);
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    s.Receive(From(1, Forest(), proto::BossState::defeated));
    CHECK_FALSE(s.Tick(At("ForestTemple", kForestEntrance), 10 * kS).pull);
    // Still beaten for this visit, even if someone says in_room again.
    s.Receive(From(3, Forest(), proto::BossState::intro_end));
    CHECK_FALSE(s.Tick(At("ForestTemple", kForestEntrance), 11 * kS).pull);
}

TEST_CASE("boss: a player who comes back to the dungeon during the fight is pulled again") {
    Sync s;
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    CHECK(s.Tick(At("ForestTemple", kForestEntrance), 1 * kS).pull);
    // We die: a load (nothing readable), then the dungeon's entrance.
    s.Tick(Local{}, 2 * kS);
    s.Tick(At("ForestTemple", Forest().arrive), 2 * kS + 1);  // got in, then died
    s.Tick(At("Graveyard_Adult", Vec{}), 3 * kS);
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    CHECK(s.Tick(At("ForestTemple", kForestEntrance), 4 * kS).pull);
}

TEST_CASE("boss: our own intro played here makes us ready to own, a skipped one does not (boss-fails-0115)") {
    // big-run-0115 hs-boss-fix-gohma: the game that played Gohma's intro read unready (the game had not set the
    // intro's HavePlayedEvent flag), so the server handed her to the other game before any stun.
    Sync s;
    s.Tick(At("ForestTemple", Forest().arrive), 1 * kS);
    CHECK_FALSE(s.OwnIntroPlayed());
    s.Tick(At("ForestTemple", Forest().arrive, true), 2 * kS);
    CHECK_FALSE(s.OwnIntroPlayed());  // (still running)
    s.Tick(At("ForestTemple", Forest().arrive, false), 20 * kS);
    REQUIRE(s.phase() == Sync::Phase::fight);
    CHECK(s.OwnIntroPlayed());
    // Leaving the room forgets it.
    s.Tick(At("ForestTemple", kForestEntrance), 30 * kS);
    CHECK_FALSE(s.OwnIntroPlayed());

    Sync t;  // another player's intro is over: ours is skipped to follow, which is no intro played here
    t.Tick(At("ForestTemple", Forest().arrive, false, 2), 1 * kS);
    t.Tick(At("ForestTemple", Forest().arrive, true, 2), 2 * kS);
    t.Receive(From(1, Forest(), proto::BossState::intro_end));
    CHECK(t.Tick(At("ForestTemple", Forest().arrive, true, 2), 4 * kS).skip_intro);
    t.Tick(At("ForestTemple", Forest().arrive, false, 2), 12 * kS);
    REQUIRE(t.phase() == Sync::Phase::fight);
    CHECK_FALSE(t.OwnIntroPlayed());
}

TEST_CASE("boss: our intro is skipped once another player's intro is over") {
    Sync s;
    s.Tick(At("ForestTemple", Forest().arrive, false, 2), 1 * kS);
    s.Tick(At("ForestTemple", Forest().arrive, true, 2), 2 * kS);
    REQUIRE(s.phase() == Sync::Phase::intro);
    s.Receive(From(1, Forest(), proto::BossState::intro_start));
    CHECK_FALSE(s.Tick(At("ForestTemple", Forest().arrive, true, 2), 3 * kS).skip_intro);
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    CHECK(s.Tick(At("ForestTemple", Forest().arrive, true, 2), 4 * kS).skip_intro);
    // Asked again every 0.5 s while the event runs, for 6 s.
    CHECK_FALSE(s.Tick(At("ForestTemple", Forest().arrive, true, 2), 4 * kS + 100'000).skip_intro);
    CHECK(s.Tick(At("ForestTemple", Forest().arrive, true, 2), 4 * kS + 600'000).skip_intro);
    CHECK_FALSE(s.Tick(At("ForestTemple", Forest().arrive, true, 2), 11 * kS).skip_intro);
    auto a = s.Tick(At("ForestTemple", Forest().arrive, false, 2), 12 * kS);
    CHECK(Has(a, proto::BossState::intro_end));
    CHECK(s.phase() == Sync::Phase::fight);
    // The fight's events (the boss's death) are never skipped.
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    CHECK_FALSE(s.Tick(At("ForestTemple", Forest().arrive, true, 2), 13 * kS).skip_intro);
}

TEST_CASE("boss: a pulled player who arrives in an event treats it as the intro") {
    Sync s;
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    REQUIRE(s.Tick(At("ForestTemple", kForestEntrance, false, 2), 1 * kS).pull);
    auto a = s.Tick(At("ForestTemple", Forest().arrive, true, 2), 1 * kS + 500'000);
    CHECK(Has(a, proto::BossState::in_room));
    CHECK(Has(a, proto::BossState::intro_start));
    CHECK(a.skip_intro);
}

TEST_CASE("boss: messages for rooms this build does not know are dropped") {
    Sync s;
    s.Receive(proto::BossRoom{1, "ForestTemple", 5, proto::BossState::intro_end});  // room 5 is the Water Temple
    s.Receive(proto::BossRoom{1, "NoSuchDungeon", 9, proto::BossState::intro_end});
    s.Receive(proto::TimeOfDay{1, 10, 0});
    CHECK_FALSE(s.Tick(At("ForestTemple", kForestEntrance), 1 * kS).pull);
}

TEST_CASE("boss: Reset forgets everything") {
    Sync s;
    s.Tick(At("ForestTemple", Forest().arrive), 1 * kS);
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    s.Reset();
    CHECK(s.phase() == Sync::Phase::none);
    CHECK_FALSE(s.Tick(At("ForestTemple", kForestEntrance), 2 * kS).pull);
}

TEST_CASE("boss: a clock that steps back does not end the wait for the intro") {
    Sync s;
    s.Tick(At("ForestTemple", Forest().arrive), 20 * kS);
    // The server clock is estimated again and goes back 5 s: no wrap, no
    // false "no intro" (that needs 10 s in the room).
    auto a = s.Tick(At("ForestTemple", Forest().arrive), 15 * kS);
    CHECK_FALSE(Has(a, proto::BossState::intro_end));
    CHECK(s.phase() == Sync::Phase::waiting);
    a = s.Tick(At("ForestTemple", Forest().arrive), 29 * kS);
    CHECK(s.phase() == Sync::Phase::waiting);
    a = s.Tick(At("ForestTemple", Forest().arrive), 30 * kS);
    CHECK(s.phase() == Sync::Phase::fight);  // no intro for 10 s (it moves no one in: issue 148)
    CHECK_FALSE(Has(a, proto::BossState::intro_end));
}

TEST_CASE("boss: a player who walks out during the fight is pulled back") {
    Sync s;
    s.Tick(At("ForestTemple", Forest().arrive, false, 2), 1 * kS);
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    auto a = s.Tick(At("ForestTemple", kForestEntrance, false, 2), 2 * kS);
    CHECK(Has(a, proto::BossState::left));
    CHECK(a.pull);
}

namespace {

Local Alive(Vec p, bool event, std::optional<bool> alive, std::uint8_t self = 2) {
    Local l = At("ForestTemple", p, event, self);
    l.boss_alive = alive;
    l.boss_read_dead = alive == false;  // these fights' deaths are read at life 0
    return l;
}

// In the room with the fight begun (the intro played).
void ToFight(Sync& s, std::uint8_t self = 2) {
    s.Tick(Alive(Forest().arrive, false, true, self), 1 * kS);
    s.Tick(Alive(Forest().arrive, true, true, self), 2 * kS);
    s.Tick(Alive(Forest().arrive, false, true, self), 5 * kS);
    REQUIRE(s.phase() == Sync::Phase::fight);
}

} // namespace

TEST_CASE("boss: an event in the fight with the boss alive is not its death (issue 109)") {
    Sync s;
    ToFight(s);
    // A grab or a phase change: an event, the boss still there.
    auto a = s.Tick(Alive(Forest().arrive, true, true), 30 * kS);
    CHECK_FALSE(Has(a, proto::BossState::defeated));
    CHECK(s.phase() == Sync::Phase::outro);
    // The heartbeat goes on while it is decided (the fight: intro_end).
    a = s.Tick(Alive(Forest().arrive, true, true), 33 * kS);
    CHECK(Has(a, proto::BossState::intro_end));
    // It ends with the boss alive: back to the fight, nothing beaten.
    a = s.Tick(Alive(Forest().arrive, false, true), 36 * kS);
    CHECK_FALSE(Has(a, proto::BossState::defeated));
    CHECK(s.phase() == Sync::Phase::fight);

    // Walking out now: still a fight elsewhere, so pulled back.
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    a = s.Tick(Alive(kForestEntrance, false, true), 37 * kS);
    CHECK(Has(a, proto::BossState::left));
    CHECK(a.pull);
}

TEST_CASE("boss: the boss gone during the event is its death") {
    Sync s;
    ToFight(s);
    s.Tick(Alive(Forest().arrive, true, true), 30 * kS);
    auto a = s.Tick(Alive(Forest().arrive, true, false), 34 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    CHECK(s.phase() == Sync::Phase::outro);
    s.Tick(Alive(Forest().arrive, false, false), 50 * kS);
    CHECK(s.phase() == Sync::Phase::done);
    // Walking out of a beaten room: not pulled back (allowed).
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    a = s.Tick(Alive(kForestEntrance, false, false), 51 * kS);
    CHECK_FALSE(a.pull);
}

TEST_CASE("boss: an event that runs 15 s is the death even if the boss is still seen") {
    Sync s;
    ToFight(s);
    s.Tick(Alive(Forest().arrive, true, true), 30 * kS);
    auto a = s.Tick(Alive(Forest().arrive, true, true), 44 * kS);
    CHECK_FALSE(Has(a, proto::BossState::defeated));
    a = s.Tick(Alive(Forest().arrive, true, true), 45 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    s.Tick(Alive(Forest().arrive, false, true), 60 * kS);
    CHECK(s.phase() == Sync::Phase::done);
}

TEST_CASE("boss: a long event with the boss at full life is not its death (Gohma's ceiling intro)") {
    // The Deku Tree: a 2 s event on entry is taken as the intro; Gohma's own intro (on the
    // ceiling, then three camera shots) starts later and runs over 15 s, Gohma at full life.
    Sync s;
    ToFight(s);
    Local l = Alive(Forest().arrive, true, true);
    l.boss_hurt = false;
    s.Tick(l, 30 * kS);
    REQUIRE(s.phase() == Sync::Phase::outro);
    auto a = s.Tick(l, 46 * kS);
    CHECK_FALSE(Has(a, proto::BossState::defeated));
    a = s.Tick(l, 60 * kS);
    CHECK_FALSE(Has(a, proto::BossState::defeated));
    // The scene ends, Gohma alive: the fight goes on.
    l.event = false;
    a = s.Tick(l, 61 * kS);
    CHECK_FALSE(Has(a, proto::BossState::defeated));
    CHECK(s.phase() == Sync::Phase::fight);
    // Once hurt, a long event is the death again (a body left with its life unread).
    l.event = true;
    l.boss_hurt = true;
    s.Tick(l, 80 * kS);
    a = s.Tick(l, 96 * kS);
    CHECK(Has(a, proto::BossState::defeated));
}

TEST_CASE("boss: the boss dead when the event starts, or no reading, is the death at once") {
    Sync s;
    ToFight(s);
    auto a = s.Tick(Alive(Forest().arrive, true, false), 30 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    Sync t;
    ToFight(t);
    a = t.Tick(Alive(Forest().arrive, true, std::nullopt), 30 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    // A reading lost during the event: decided at its end, as before.
    Sync u;
    ToFight(u);
    u.Tick(Alive(Forest().arrive, true, true), 30 * kS);
    a = u.Tick(Alive(Forest().arrive, false, std::nullopt), 33 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    CHECK(u.phase() == Sync::Phase::done);
}

// G2 review: what the fight's guesses send must never reach the beaten record: neither beaten_here nor a message
// CoopBossBeaten::ProvenDeath takes. The record is written into absent players' saves, for good.
bool Recorded(const Actions& a) {
    if (a.beaten_here != nullptr) {
        return true;
    }
    return std::any_of(a.send.begin(), a.send.end(), [](const proto::BossRoom& m) {
        return CoopBossBeaten::ProvenDeath(proto::Message{m}).has_value();
    });
}

TEST_CASE("boss: a heuristic Defeat never reaches the record") {
    // 1. An event in the fight with the boss not read (coop_boss.cpp: GuessedOver at the event's start).
    Sync s;
    ToFight(s);
    auto a = s.Tick(Alive(Forest().arrive, true, std::nullopt), 30 * kS);
    CHECK(Has(a, proto::BossState::defeated));  // the pull still ends
    CHECK_FALSE(Recorded(a));
    // 2. A long event with the boss hurt and still read alive (Morpha's grab, Phantom Ganon's paintings).
    Sync t;
    ToFight(t);
    Local l = Alive(Forest().arrive, true, true);
    l.boss_hurt = true;
    t.Tick(l, 30 * kS);
    a = t.Tick(l, 46 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    CHECK_FALSE(Recorded(a));
    // 3. The event ends with the reading lost.
    Sync u;
    ToFight(u);
    u.Tick(Alive(Forest().arrive, true, true), 30 * kS);
    a = u.Tick(Alive(Forest().arrive, false, std::nullopt), 33 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    CHECK_FALSE(Recorded(a));
    // 4. Gone without any reading at life 0 (a body never read this scene): a guess too.
    Sync w;
    ToFight(w);
    Local gone = Alive(Forest().arrive, true, false);
    gone.boss_read_dead = false;
    a = w.Tick(gone, 30 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    CHECK_FALSE(Recorded(a));
    Sync x;
    ToFight(x);
    x.Tick(Alive(Forest().arrive, true, true), 30 * kS);  // provisional
    a = x.Tick(gone, 31 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    CHECK_FALSE(Recorded(a));
    // A proven death (a body read at life 0) reaches it, once.
    Sync v;
    ToFight(v);
    a = v.Tick(Alive(Forest().arrive, true, false), 30 * kS);
    CHECK(a.beaten_here == &Forest());
    CHECK(Recorded(a));
    CHECK_FALSE(Recorded(v.Tick(Alive(Forest().arrive, true, false), 31 * kS)));
}

TEST_CASE("boss: an event right after a skipped intro is not the boss's death (integrate-p8)") {
    // B in its intro; A's intro is over: B skips. B's event ends, and 0.1 s later the next one
    // starts (the skipped intro's next shot), with no reading of the boss.
    Sync s;
    s.Tick(At("ForestTemple", Forest().arrive, false, 2), 1 * kS);
    s.Tick(At("ForestTemple", Forest().arrive, true, 2), 2 * kS);
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    CHECK(s.Tick(At("ForestTemple", Forest().arrive, true, 2), 3 * kS).skip_intro);
    auto a = s.Tick(At("ForestTemple", Forest().arrive, false, 2), 4 * kS);
    CHECK(Has(a, proto::BossState::intro_end));
    REQUIRE(s.phase() == Sync::Phase::fight);
    a = s.Tick(At("ForestTemple", Forest().arrive, true, 2), 4 * kS + 100'000);
    CHECK_FALSE(Has(a, proto::BossState::defeated));
    CHECK(s.phase() == Sync::Phase::fight);
    bool said = false;
    for (const auto& line : a.log) said |= line.find("not the boss's death") != std::string::npos;
    CHECK(said);
    s.Tick(At("ForestTemple", Forest().arrive, false, 2), 8 * kS);
    CHECK(s.phase() == Sync::Phase::fight);
    // So the fight goes on: B walks out and is pulled back by A's fight.
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    a = s.Tick(At("ForestTemple", kForestEntrance, false, 2), 9 * kS);
    CHECK(Has(a, proto::BossState::left));
    CHECK(a.pull);
    // Later in a fight (past kMinFightUs), an event with no reading is the death as before.
    Sync t;
    t.Tick(At("ForestTemple", Forest().arrive), 1 * kS);
    t.Tick(At("ForestTemple", Forest().arrive, true), 2 * kS);
    t.Tick(At("ForestTemple", Forest().arrive, false), 6 * kS);  // a 4 s intro (kMinIntroUs is 3 s)
    REQUIRE(t.phase() == Sync::Phase::fight);
    a = t.Tick(At("ForestTemple", Forest().arrive, true), 6 * kS + Sync::kMinFightUs);
    CHECK(Has(a, proto::BossState::defeated));
}

TEST_CASE("boss: a shut boss door left in a save by an older build is opened once per load (issue 148)") {
    CoopBoss::StaleBossFlags stale;
    std::set<std::string> set{"InsideTheDekuTree_BossArea_ShutIn", "FireTemple_BossGroundChange"};
    const auto is_set = [&](std::string_view n) { return set.contains(std::string(n)); };
    // Loading: nothing yet. Then the dungeon, Link at its entrance: both set flags, once.
    CHECK(stale.Check(std::nullopt, std::nullopt, is_set).empty());
    CHECK(stale.Check(std::string("InsideTheDekuTree"), std::nullopt, is_set).empty());  // place not read yet
    auto out = stale.Check(std::string("InsideTheDekuTree"), Vec{0.0f, 0.0f, 0.0f}, is_set);
    CHECK(out.size() == 2);
    CHECK(stale.Check(std::string("InsideTheDekuTree"), Vec{0.0f, 0.0f, 0.0f}, is_set).empty());  // once
    // A reload into the same scene checks again.
    stale.Check(std::nullopt, std::nullopt, is_set);
    CHECK(stale.Check(std::string("InsideTheDekuTree"), Vec{0.0f, 0.0f, 0.0f}, is_set).size() == 2);
    // Loaded straight into a boss room (a real fight, a respawn there): left alone.
    stale.Check(std::nullopt, std::nullopt, is_set);
    CHECK(stale.Check(std::string("ForestTemple"), Forest().arrive, is_set).empty());
    // Nothing set: nothing to do.
    set.clear();
    stale.Check(std::nullopt, std::nullopt, is_set);
    CHECK(stale.Check(std::string("Graveyard_Child"), Vec{}, is_set).empty());
}

TEST_CASE("boss: the boss's bodies are the boss actor, and Koume and Kotake for Twinrova") {
    for (const auto& room : CoopBoss::Rooms()) {
        const auto bodies = CoopBoss::Bodies(room);
        REQUIRE_FALSE(bodies.empty());
        CHECK(bodies[0] == room.boss);
        for (const auto name : bodies) {
            CHECK(name.find("_Egg") == std::string_view::npos);
            CHECK(name.find("Baby") == std::string_view::npos);
            CHECK(name.find("Bullet") == std::string_view::npos);
        }
    }
    const auto* twinrova = CoopBoss::RoomOfScene("SpiritTemple");
    REQUIRE(twinrova != nullptr);
    CHECK(CoopBoss::Bodies(*twinrova).size() == 3);
    CHECK(CoopBoss::Bodies(Forest()).size() == 1);
}

// Issue 148: the move-in waits until the leader is really in the boss room.
namespace {

const CoopBoss::Room& Deku() {
    return *CoopBoss::RoomOfScene("InsideTheDekuTree");
}
// Inside Gohma's circle but on the way to its door (the circle takes that in).
constexpr Vec kDekuOnTheWay{7.5f, 47.0f, 97.0f};
// Elsewhere in the Deku Tree: below the room's height band.
constexpr Vec kDekuElsewhere{0.0f, 0.0f, 0.0f};

Local DekuAt(Vec p, bool event, std::optional<bool> shut_in, std::uint8_t self) {
    Local l = At("InsideTheDekuTree", p, event, self);
    l.shut_in = shut_in;
    return l;
}

// A's messages as B hears them.
void Relay(const Actions& from_a, Sync& b) {
    for (auto m : from_a.send) {
        m.player_number = 1;
        b.Receive(m);
    }
}

bool Logged(const Actions& a, std::string_view text) {
    for (const auto& line : a.log) {
        if (line.find(text) != std::string::npos) return true;
    }
    return false;
}

bool Committed(const Actions& a) {
    return Has(a, proto::BossState::intro_start) || Has(a, proto::BossState::intro_end);
}

} // namespace

TEST_CASE("boss: the Deku Tree has a shut-in flag, the other rooms none") {
    CHECK(CoopBoss::ShutInFlag(Deku()) == "InsideTheDekuTree_BossArea_ShutIn");
    for (const auto& r : CoopBoss::Rooms()) {
        if (r.index != 0) CHECK(CoopBoss::ShutInFlag(r).empty());
    }
}

TEST_CASE("boss: A on the way to Gohma's door moves no one, A shut in moves B (issue 148)") {
    Sync a;
    Sync b;
    REQUIRE(CoopBoss::Inside(Deku(), kDekuOnTheWay));
    REQUIRE_FALSE(CoopBoss::Inside(Deku(), kDekuElsewhere));
    std::uint64_t t = 1 * kS;
    const auto step = [&](Vec pa, bool event, bool shut) {
        Actions out = a.Tick(DekuAt(pa, event, shut, 1), t);
        Relay(out, b);
        Actions bo = b.Tick(DekuAt(kDekuElsewhere, false, false, 2), t);
        t += 250'000;
        return std::pair{std::move(out), std::move(bo)};
    };
    // A walks into the circle on the way; the door opening is an event of 5 s (longer than
    // kMinIntroUs) with A still outside: the game's flag says A is not shut in.
    for (int i = 0; i < 8; ++i) CHECK_FALSE(step(kDekuOnTheWay, false, false).second.pull);
    for (int i = 0; i < 20; ++i) CHECK_FALSE(step(kDekuOnTheWay, true, false).second.pull);
    {
        const auto [out, bo] = step(kDekuOnTheWay, false, false);
        CHECK_FALSE(bo.pull);
        CHECK_FALSE(Committed(out));
        CHECK(Logged(out, "not the intro"));
    }
    CHECK(a.phase() == Sync::Phase::waiting);
    // Text boxes or Navi on the way: still nothing; 20 s of heartbeats say only in_room.
    for (int i = 0; i < 80; ++i) {
        const auto [out, bo] = step(kDekuOnTheWay, i % 10 < 3, false);
        CHECK_FALSE(bo.pull);
        CHECK_FALSE(Committed(out));
    }
    // A reaches the door's cutscene: the door shuts behind A and the game sets ShutIn. Now B moves in.
    bool pulled = false;
    bool said = false;
    for (int i = 0; i < 8 && !pulled; ++i) {
        const auto [out, bo] = step(Deku().arrive, true, true);
        said = said || (Has(out, proto::BossState::intro_start) && Logged(out, "past the door (shut in)"));
        CHECK_FALSE(out.pull);  // A, in the room, is never moved
        pulled = bo.pull.has_value();
    }
    CHECK(said);
    CHECK(pulled);
}

TEST_CASE("boss: the area rules hear of our intro only once we are past the door (issue 148)") {
    // The older rules tell the area rules (coop_boss_scenes) "our game runs the intro"; they send
    // intro_start and move every other game in the area to the room's start. Told at the first event in
    // Gohma's circle (which takes in the way to her door), the door's own event pulled the others in
    // while the leader still stood outside.
    Sync a;
    std::uint64_t t = 1 * kS;
    const auto tick = [&](Vec p, bool event, std::optional<bool> shut) {
        Actions out = a.Tick(DekuAt(p, event, shut, 1), t);
        t += 250'000;
        return out;
    };
    tick(kDekuOnTheWay, false, false);
    // The door opening (and texts) on the way, 6 s, not shut in: nothing told.
    for (int i = 0; i < 24; ++i) CHECK(tick(kDekuOnTheWay, true, false).intro_here == nullptr);
    CHECK(tick(kDekuOnTheWay, false, false).intro_here == nullptr);
    // Came into the room with an event running, still not shut in: nothing told either.
    for (int i = 0; i < 8; ++i) CHECK(tick(Deku().arrive, true, false).intro_here == nullptr);
    // The door shuts behind A: told, once.
    int told = 0;
    for (int i = 0; i < 12; ++i) told += tick(Deku().arrive, true, true).intro_here == &Deku() ? 1 : 0;
    CHECK(told == 1);
}

TEST_CASE("boss: in a room with no shut-in flag a short event is not told as the intro (issue 148)") {
    Sync s;
    s.Tick(At("ForestTemple", Forest().arrive), 1 * kS);
    // A 2 s event (a door, a text): shorter than kMinIntroUs, never told.
    for (std::uint64_t t = 2 * kS; t <= 4 * kS; t += kS / 2) {
        CHECK(s.Tick(At("ForestTemple", Forest().arrive, true), t).intro_here == nullptr);
    }
    CHECK(s.Tick(At("ForestTemple", Forest().arrive, false), 4 * kS + kS / 2).intro_here == nullptr);
    CHECK(s.phase() == Sync::Phase::waiting);
    // The real intro: told while it still runs (the area rules then see it end), once.
    int told = 0;
    for (std::uint64_t t = 6 * kS; t <= 20 * kS; t += kS / 2) {
        told += s.Tick(At("ForestTemple", Forest().arrive, true), t).intro_here == &Forest() ? 1 : 0;
    }
    CHECK(told == 1);
    CHECK(s.Tick(At("ForestTemple", Forest().arrive, false), 21 * kS).intro_here == nullptr);
    CHECK(s.phase() == Sync::Phase::fight);
}

TEST_CASE("boss: in the Deku Tree an event that ends with A not shut in moves no one (issue 148)") {
    Sync a;
    a.Tick(DekuAt(kDekuOnTheWay, false, false, 1), 1 * kS);
    a.Tick(DekuAt(kDekuOnTheWay, true, false, 1), 2 * kS);
    auto out = a.Tick(DekuAt(kDekuOnTheWay, false, false, 1), 20 * kS);  // long, but not shut in
    CHECK_FALSE(Committed(out));
    CHECK(a.phase() == Sync::Phase::waiting);
    // Still not shut in 10 s later: the fight counts here, but nobody is moved in.
    out = a.Tick(DekuAt(kDekuOnTheWay, false, false, 1), 31 * kS);
    CHECK(a.phase() == Sync::Phase::fight);
    CHECK_FALSE(Committed(out));
    // The intro seen before (no event) and the flag set: past the door, the fight is on.
    out = a.Tick(DekuAt(Deku().arrive, false, true, 1), 32 * kS);
    CHECK(Has(out, proto::BossState::intro_end));
    out = a.Tick(DekuAt(Deku().arrive, false, true, 1), 35 * kS);
    CHECK(States(out) == std::vector{proto::BossState::intro_end});
}

TEST_CASE("boss: with no shut-in flag a short event near the room is not the intro (issue 148)") {
    Sync a;
    a.Tick(At("ForestTemple", Forest().arrive), 1 * kS);
    a.Tick(At("ForestTemple", Forest().arrive, true), 2 * kS);
    auto out = a.Tick(At("ForestTemple", Forest().arrive, false), 3 * kS);  // 1 s: a door, a text
    CHECK_FALSE(Committed(out));
    CHECK(a.phase() == Sync::Phase::waiting);
    a.Tick(At("ForestTemple", Forest().arrive, true), 4 * kS);
    out = a.Tick(At("ForestTemple", Forest().arrive, false), 12 * kS);  // the real intro
    CHECK(Has(out, proto::BossState::intro_end));
    CHECK(a.phase() == Sync::Phase::fight);
}

TEST_CASE("boss: shut in while the intro runs says intro_start, then intro_end at its end") {
    Sync a;
    a.Tick(DekuAt(Deku().arrive, false, false, 1), 1 * kS);
    auto out = a.Tick(DekuAt(Deku().arrive, true, false, 1), 2 * kS);
    CHECK_FALSE(Committed(out));
    out = a.Tick(DekuAt(Deku().arrive, true, true, 1), 3 * kS);  // the door shuts in the cutscene
    CHECK(Has(out, proto::BossState::intro_start));
    out = a.Tick(DekuAt(Deku().arrive, true, true, 1), 5 * kS + 1);
    CHECK(States(out) == std::vector{proto::BossState::intro_start});  // the heartbeat
    out = a.Tick(DekuAt(Deku().arrive, false, true, 1), 20 * kS);
    CHECK(Has(out, proto::BossState::intro_end));
    CHECK_FALSE(Has(out, proto::BossState::intro_start));
    CHECK(a.phase() == Sync::Phase::fight);
}

TEST_CASE("boss: a real intro long after an event that was not the intro is the intro, not the death") {
    // Review of issue 148: the rejected event started the 10 s no-intro wait again; the real
    // intro 12 s later came in the no-intro fight and was taken for the boss's death.
    Sync a;
    a.Tick(Alive(Forest().arrive, false, true, 1), 1 * kS);
    a.Tick(Alive(Forest().arrive, true, true, 1), 2 * kS);
    a.Tick(Alive(Forest().arrive, false, true, 1), 3 * kS);  // 1 s: a door, a text
    REQUIRE(a.phase() == Sync::Phase::waiting);
    a.Tick(Alive(Forest().arrive, false, true, 1), 14 * kS);  // no intro for 10 s
    REQUIRE(a.phase() == Sync::Phase::fight);
    auto out = a.Tick(Alive(Forest().arrive, true, true, 1), 15 * kS);  // the real intro, 16 s long
    CHECK(a.phase() == Sync::Phase::intro);
    for (std::uint64_t t = 16; t < 31; ++t) {
        out = a.Tick(Alive(Forest().arrive, true, true, 1), t * kS);
        CHECK_FALSE(Has(out, proto::BossState::defeated));
    }
    out = a.Tick(Alive(Forest().arrive, false, true, 1), 31 * kS);
    CHECK(Has(out, proto::BossState::intro_end));  // past the door: the others are moved in
    CHECK_FALSE(Has(out, proto::BossState::defeated));
    CHECK(a.phase() == Sync::Phase::fight);
    // Now the fight's events are the fight's again: the boss gone in one is its death.
    a.Tick(Alive(Forest().arrive, true, true, 1), 50 * kS);
    out = a.Tick(Alive(Forest().arrive, true, false, 1), 52 * kS);
    CHECK(Has(out, proto::BossState::defeated));
}

TEST_CASE("boss: in a no-intro fight nobody is past the door for, the boss dead in an event is its death") {
    Sync a;
    a.Tick(Alive(Forest().arrive, false, true, 1), 1 * kS);
    a.Tick(Alive(Forest().arrive, false, true, 1), 12 * kS);
    REQUIRE(a.phase() == Sync::Phase::fight);
    auto out = a.Tick(Alive(Forest().arrive, true, false, 1), 40 * kS);
    CHECK(Has(out, proto::BossState::defeated));
}

namespace {

// The chat notices of a Tick, as "<player> <text>".
std::vector<std::string> Said(const Actions& a) {
    std::vector<std::string> out;
    for (const auto& n : a.chat) {
        out.push_back(std::to_string(n.player) + " " + n.text);
    }
    return out;
}

} // namespace

TEST_CASE("boss: a player who leaves the beaten boss room is told to those still in it (#50)") {
    Sync s;  // we are player 1, in the room
    const auto here = [] { return At("ForestTemple", Forest().arrive); };
    s.Tick(here(), 1 * kS);
    s.Receive(From(2, Forest(), proto::BossState::intro_end));
    s.Receive(From(2, Forest(), proto::BossState::defeated));
    s.Receive(From(2, Forest(), proto::BossState::left));
    CHECK(s.Tick(here(), 20 * kS).chat.empty());  // waits a second: the group may be leaving together
    CHECK(s.Tick(here(), 20 * kS + kS / 2).chat.empty());
    CHECK(Said(s.Tick(here(), 21 * kS)) == std::vector<std::string>{"2 took the blue warp"});
    CHECK(s.Tick(here(), 22 * kS).chat.empty());
    // Once per player per fight: back in and out again says nothing more.
    s.Receive(From(2, Forest(), proto::BossState::defeated));
    s.Receive(From(2, Forest(), proto::BossState::left));
    CHECK(s.Tick(here(), 23 * kS).chat.empty());
    CHECK(s.Tick(here(), 25 * kS).chat.empty());
}

TEST_CASE("boss: a player past the door who leaves before the win is told as left, and one only near it is not (#50)") {
    Sync s;
    const auto here = [] { return At("ForestTemple", Forest().arrive); };
    s.Tick(here(), 1 * kS);
    s.Receive(From(2, Forest(), proto::BossState::intro_start));
    s.Receive(From(2, Forest(), proto::BossState::left));
    s.Receive(From(3, Forest(), proto::BossState::in_room));  // never past the door
    s.Receive(From(3, Forest(), proto::BossState::left));
    s.Tick(here(), 2 * kS);
    CHECK(Said(s.Tick(here(), 4 * kS)) == std::vector<std::string>{"2 left the boss room"});
    // The boss beaten in our own game: their warp out is a warp.
    s.Receive(From(4, Forest(), proto::BossState::intro_end));
    s.Tick(At("ForestTemple", Forest().arrive, true), 5 * kS);  // our intro
    s.Tick(here(), 20 * kS);                                    // its end: the fight
    REQUIRE(s.phase() == Sync::Phase::fight);
    s.Tick(At("ForestTemple", Forest().arrive, true), 60 * kS);  // our boss's death (outro)
    s.Tick(At("ForestTemple", Forest().arrive, true), 80 * kS);
    s.Tick(here(), 90 * kS);
    REQUIRE(s.phase() == Sync::Phase::done);
    s.Receive(From(4, Forest(), proto::BossState::left));
    s.Tick(here(), 91 * kS);
    CHECK(Said(s.Tick(here(), 93 * kS)) == std::vector<std::string>{"4 took the blue warp"});
}

TEST_CASE("boss: no leave notice for our own player, a group that leaves together, or after our own warp (#50)") {
    const auto here = [] { return At("ForestTemple", Forest().arrive); };
    {
        Sync s;  // our own BOSS_ROOM back from the server
        s.Tick(here(), 1 * kS);
        s.Receive(From(1, Forest(), proto::BossState::defeated));
        s.Receive(From(1, Forest(), proto::BossState::left));
        s.Tick(here(), 2 * kS);
        CHECK(s.Tick(here(), 4 * kS).chat.empty());
    }
    {
        Sync s;  // they warp, we warp half a second later: nobody left behind
        s.Tick(here(), 1 * kS);
        s.Receive(From(2, Forest(), proto::BossState::defeated));
        s.Receive(From(2, Forest(), proto::BossState::left));
        s.Tick(here(), 10 * kS);
        Local loading;  // the warp's load: nothing is read
        CHECK(s.Tick(loading, 10 * kS + kS / 2).chat.empty());
        CHECK(s.Tick(loading, 12 * kS).chat.empty());
        CHECK(s.Tick(At("DekuTree", Vec{}), 13 * kS).chat.empty());
    }
    {
        Sync s;  // we walk out within the second
        s.Tick(here(), 1 * kS);
        s.Receive(From(2, Forest(), proto::BossState::defeated));
        s.Receive(From(2, Forest(), proto::BossState::left));
        s.Tick(here(), 10 * kS);
        CHECK(s.Tick(At("ForestTemple", kForestEntrance), 10 * kS + kS / 2).chat.empty());
        CHECK(s.Tick(At("ForestTemple", kForestEntrance), 12 * kS).chat.empty());
    }
    {
        Sync s;  // we are not in the room when they leave it
        s.Tick(At("ForestTemple", kForestEntrance), 1 * kS);
        s.Receive(From(2, Forest(), proto::BossState::defeated));
        s.Receive(From(2, Forest(), proto::BossState::left));
        s.Tick(At("ForestTemple", kForestEntrance), 2 * kS);
        CHECK(s.Tick(here(), 4 * kS).chat.empty());
    }
}

namespace {
Local Dead(Vec p, bool event, std::optional<bool> alive, bool dead) {
    Local l = Alive(p, event, alive);
    l.boss_alive = alive;
    l.boss_hurt = true;  // the boss has lost life: the long-event guess would apply
    l.link_dead = dead;
    return l;
}
} // namespace

TEST_CASE("boss: a Game Over in the fight is never the boss's death (boss-gameover-b)") {
    // hs-boss-full 20261115-122137: A's Link died with Gohma at 17 of 21; the Game Over ran
    // over 15 s and was sent to the group as the boss beaten.
    Sync s;
    ToFight(s);
    auto a = s.Tick(Dead(Forest().arrive, true, true, true), 30 * kS);
    CHECK_FALSE(Has(a, proto::BossState::defeated));
    CHECK(s.phase() == Sync::Phase::fight);
    for (std::uint64_t t = 31; t <= 70; t += 3) {
        a = s.Tick(Dead(Forest().arrive, true, true, true), t * kS);
        CHECK_FALSE(Has(a, proto::BossState::defeated));
        CHECK_FALSE(a.beaten_here);
    }
    // The fight goes on for the others: the heartbeat says so.
    CHECK(Has(a, proto::BossState::intro_end));
    // The boss's life unread during the Game Over is no death either.
    a = s.Tick(Dead(Forest().arrive, false, std::nullopt, true), 71 * kS);
    a = s.Tick(Dead(Forest().arrive, true, std::nullopt, true), 72 * kS);
    CHECK_FALSE(Has(a, proto::BossState::defeated));
}

TEST_CASE("boss: Link's death read after the event began takes back the waiting (boss-gameover-b)") {
    Sync s;
    ToFight(s);
    s.Tick(Dead(Forest().arrive, true, true, false), 30 * kS);
    REQUIRE(s.phase() == Sync::Phase::outro);
    auto a = s.Tick(Dead(Forest().arrive, true, true, true), 31 * kS);
    CHECK(s.phase() == Sync::Phase::fight);
    a = s.Tick(Dead(Forest().arrive, true, true, true), 50 * kS);
    CHECK_FALSE(Has(a, proto::BossState::defeated));
    a = s.Tick(Dead(Forest().arrive, false, true, true), 51 * kS);
    CHECK_FALSE(Has(a, proto::BossState::defeated));
    CHECK(s.phase() == Sync::Phase::fight);
}

TEST_CASE("boss: after a continue the fight and its win count again (boss-gameover-b)") {
    Sync s;
    ToFight(s);
    s.Tick(Dead(Forest().arrive, true, true, true), 30 * kS);
    // The continue: out of the room (a reload), then back in, Link up.
    auto a = s.Tick(Dead(kForestEntrance, false, true, false), 50 * kS);
    CHECK(Has(a, proto::BossState::left));
    CHECK_FALSE(Has(a, proto::BossState::defeated));
    // Another game kept fighting: nothing was beaten, so we are pulled back.
    s.Receive(From(1, Forest(), proto::BossState::intro_end));
    a = s.Tick(Dead(kForestEntrance, false, true, false), 51 * kS);
    CHECK(a.pull);
    // Back in, the fight is on; Link up in the same room after a death counts again too.
    Sync t;
    ToFight(t);
    t.Tick(Dead(Forest().arrive, true, true, true), 30 * kS);
    a = t.Tick(Dead(Forest().arrive, false, true, false), 40 * kS);
    CHECK(t.phase() == Sync::Phase::fight);
    // The real win: the boss read gone in an event.
    t.Tick(Dead(Forest().arrive, true, true, false), 60 * kS);
    a = t.Tick(Dead(Forest().arrive, true, false, false), 62 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    CHECK(a.beaten_here);
}

TEST_CASE("boss: the boss read at life 0 while our Link is dead is still its death (boss-gameover-b)") {
    auto read_dead = [](Local l) {
        l.boss_read_dead = true;
        return l;
    };
    Sync s;
    ToFight(s);
    s.Tick(Dead(Forest().arrive, true, true, false), 30 * kS);
    auto a = s.Tick(read_dead(Dead(Forest().arrive, true, false, true)), 31 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    CHECK(a.beaten_here);
    Sync t;
    ToFight(t);
    a = t.Tick(read_dead(Dead(Forest().arrive, true, false, true)), 30 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    CHECK(a.beaten_here);
}

TEST_CASE("boss: the boss gone at our Game Over with no reading at life 0 is not its death (row 149, run 072018)") {
    // kd-twist run 20261120-072018: A's Link died in King Dodongo's fight, untouched; the Game Over took the scene
    // down, KD read gone without a body at life 0, and the group was told he was beaten (B: 'boss beaten in another
    // game', its fight over while KD lived).
    auto unread = [](Local l) {  // gone from the walk, never read at life 0
        l.boss_read_dead = false;
        return l;
    };
    for (const bool waiting_first : {false, true}) {
        Sync s;
        ToFight(s);
        if (waiting_first) {  // an event of the fight began first (the outro wait), then the Game Over
            s.Tick(Dead(Forest().arrive, true, true, false), 30 * kS);
            REQUIRE(s.phase() == Sync::Phase::outro);
        }
        auto a = s.Tick(unread(Dead(Forest().arrive, true, false, true)), 31 * kS);
        CHECK_FALSE(Has(a, proto::BossState::defeated));
        CHECK_FALSE(a.beaten_here);
        CHECK(s.phase() == Sync::Phase::fight);
        for (std::uint64_t t = 32; t <= 60; t += 4) {  // the whole Game Over, the boss still not read
            a = s.Tick(unread(Dead(Forest().arrive, true, false, true)), t * kS);
            CHECK_FALSE(Has(a, proto::BossState::defeated));
            a = s.Tick(Dead(Forest().arrive, true, std::nullopt, true), t * kS + kS);
            CHECK_FALSE(Has(a, proto::BossState::defeated));
        }
        CHECK(s.phase() == Sync::Phase::fight);
    }
    // Alive again, the boss's real death still counts.
    Sync u;
    ToFight(u);
    u.Tick(unread(Dead(Forest().arrive, true, false, true)), 30 * kS);
    u.Tick(Dead(Forest().arrive, false, true, false), 45 * kS);
    REQUIRE(u.phase() == Sync::Phase::fight);
    u.Tick(Dead(Forest().arrive, true, true, false), 60 * kS);
    Local l = Dead(Forest().arrive, true, false, false);
    l.boss_read_dead = true;
    auto a = u.Tick(l, 62 * kS);
    CHECK(Has(a, proto::BossState::defeated));
    CHECK(a.beaten_here);
}

TEST_CASE("boss: a late joiner is ready once its boss leaves the pre-battle phase, without its intro (boss-copy-b)") {
    CoopBoss::Readiness r;
    // Intro not played, boss unseen or on the ceiling: not ready.
    CHECK_FALSE(r.Step(0, false, std::nullopt));
    CHECK_FALSE(r.Step(0, false, true));
    CHECK_FALSE(r.TakeFoughtNews());
    // The copy follows an owner in the fight (Walk): ready, told once, kept.
    CHECK(r.Step(0, false, false));
    CHECK(r.TakeFoughtNews());
    CHECK_FALSE(r.TakeFoughtNews());
    CHECK(r.Step(0, false, true));          // back on the ceiling for a moment: still ready
    CHECK(r.Step(0, false, std::nullopt));
    // Another room, or out of the room (Reset): starts over.
    CHECK_FALSE(r.Step(1, false, true));
    r.Reset();
    CHECK_FALSE(r.Step(1, false, std::nullopt));
}

TEST_CASE("boss: never seen pre-battle, a boss is ready by its intro flag only (boss-copy-b)") {
    CoopBoss::Readiness r;
    CHECK_FALSE(r.Step(3, false, false));  // no pre-battle animation known for it: the flag decides
    CHECK_FALSE(r.Step(3, false, false));
    CHECK(r.Step(3, true, false));
    CHECK(r.Step(3, std::nullopt, std::nullopt));  // flag not read: as before, not held back
}
