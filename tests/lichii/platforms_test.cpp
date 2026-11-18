// Tests for moving platforms in step (lichii/hook/coop_platforms.cpp).
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coop_platforms.h"

using namespace CoopPlatforms;
namespace proto = coop::protocol;

namespace {

std::uint64_t Bits(float lo, float hi = 0.0f) {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::memcpy(&a, &lo, 4);
    std::memcpy(&b, &hi, 4);
    return std::uint64_t{a} | (std::uint64_t{b} << 32);
}

constexpr std::uint64_t kAsKey = Key(kRegionAsFrame, 0);
constexpr std::uint64_t kClockKey = kAsKey;  // the clock: the AS slot 0 frame (room-objects: no guessed word)
constexpr std::uint64_t kHash = 0x7f36b7404e6b5890ull;
constexpr std::uint64_t kMs = 1000;

// One game's driver: a clock word stepping at `rate` a second, a counter
// word that steps unevenly, a still word and a heap pointer.
struct Game {
    std::uint8_t self = 1;
    std::uint64_t steady_offset = 0;  // this game's steady clock = time + offset
    float clock = 0.0f;
    float rate = 1.0f;
    float cycle = 0.0f;  // the game loops its clock here (0: never)
    float uneven = 0.0f;
    int steps = 0;
    PlatformSync sync;
    std::vector<proto::Message> sent;
    std::vector<std::string> notes;
    bool probe_only = false;
    bool paused = false;  // a pause menu: the game's clocks stand still
    // Issue 194: the driver and our Link (by default a controller 1 m away, nobody riding).
    std::string driver_name = "GroundControlActor_WaveLoop";
    proto::Vec3 driver_at{1.0f, 0.0f, 0.0f};
    std::optional<bool> ride_plug;
    proto::Vec3 link{0.0f, 0.0f, 0.0f};

    void Step(float dt) {
        if (paused) {
            return;
        }
        clock += rate * dt;
        if (cycle > 0.0f && clock >= cycle) {
            clock -= cycle;
        }
        uneven += (++steps % 3 == 0) ? 0.5f : 0.01f;
    }

    std::map<std::uint64_t, std::uint64_t> Words() const {
        return {
            {kClockKey, Bits(clock, 0.0f)},
            {Key(4, 0x38), Bits(uneven, 7.0f)},
            {Key(0, 0x10), 0x000000214e24aae8ull},  // a pointer
        };
    }

    // `time_us`: the one true time of the test; the server is time + 10 s.
    void Tick(std::uint64_t time_us, const std::string& scene = "DodongoCavern") {
        const std::vector<PlatformSync::Driver> drivers{{0x1000, kHash, driver_name, driver_at, ride_plug}};
        PlatformSync::Out out;
        sync.Tick(scene, self, time_us + steady_offset, time_us + 10'000'000, drivers, link,
                  [this](const PlatformSync::Driver&) { return Words(); }, probe_only, out);
        for (auto& m : out.send) {
            sent.push_back(std::move(m));
        }
        for (const auto& w : out.writes) {
            CHECK(w.key == kClockKey);
            CHECK(w.hash == kHash);
            clock = w.value;
        }
        notes.insert(notes.end(), out.notes.begin(), out.notes.end());
    }
};

// Plays two games for `ms`, 50 ms a step; what A sends reaches B `delay_ms` later (and B's A).
void Play(Game& a, Game& b, std::uint64_t& time_us, std::uint64_t ms, std::uint64_t delay_ms = 80) {
    struct InFlight {
        std::uint64_t at_us;
        proto::PlatformClock m;
        bool to_b;
    };
    std::vector<InFlight> flight;
    for (std::uint64_t t = 0; t < ms; t += 50) {
        time_us += 50 * kMs;
        a.Step(0.05f);
        b.Step(0.05f);
        a.Tick(time_us);
        b.Tick(time_us);
        for (auto& m : a.sent) {
            auto pc = std::get<proto::PlatformClock>(m);
            pc.player_number = a.self;
            flight.push_back({time_us + delay_ms * kMs, pc, true});
        }
        for (auto& m : b.sent) {
            auto pc = std::get<proto::PlatformClock>(m);
            pc.player_number = b.self;
            flight.push_back({time_us + delay_ms * kMs, pc, false});
        }
        a.sent.clear();
        b.sent.clear();
        for (auto it = flight.begin(); it != flight.end();) {
            if (it->at_us <= time_us) {
                (it->to_b ? b : a).sync.Receive(it->m);
                it = flight.erase(it);
            } else {
                ++it;
            }
        }
    }
}

}  // namespace

TEST_CASE("platforms: the drivers and platform actors, not other actors") {
    CHECK(IsDriver("GroundControlActor_WaveLoop"));
    CHECK(IsDriver("GroundControlActor_Rotate"));
    CHECK(IsDriver("GroundControlActor_ResidentMoveTo"));
    CHECK(IsDriver("GrouondControlActor_DiffentSpeedMoveTo"));  // the game's spelling
    CHECK(IsDriver("Obj_DodongoCave_MoveFloor_A_01"));
    CHECK(IsDriver("Obj_InsideTheDekuTree_ElevatorLift_A_02"));
    CHECK(IsDriver("Obj_InsideJabu_Jabu_ElevatorLift_A_01"));
    CHECK(IsDriver("Obj_ForestTemple_Elevator_A_01"));
    CHECK(IsDriver("Obj_Gerudo_RotateFloor_A_01"));
    CHECK(IsDriver("Land_GerudoValley_lift_A_01"));
    CHECK_FALSE(IsDriver("Obj_LiftRock_A_01"));  // a rock Link lifts
    CHECK_FALSE(IsDriver("Enemy_Dodongo"));
    CHECK_FALSE(IsDriver("Player_Child"));
}

TEST_CASE("platforms: a look's floats leave out pointers, the table and the actor's own place") {
    const std::map<std::uint64_t, std::uint64_t> words{
        {Key(0, 0x10), Bits(1.5f, 2.5f)},
        {Key(0, 0x320), Bits(9.0f, 9.0f)},       // the place: left out
        {Key(0, 0x350), Bits(3.0f, 0.0f)},       // after it
        {Key(1, 0x0), Bits(4.0f, 4.0f)},         // the table
        {Key(3, 0x8), 0x000000214e24aae8ull},    // a heap pointer
        {Key(kRegionAsFrame, 0), Bits(12.0f, 99.0f)},
    };
    const Floats f = FloatsOf(words);
    CHECK(f.at(Key(0, 0x10)) == 1.5f);
    CHECK(f.at(Key(0, 0x14)) == 2.5f);
    CHECK(f.at(Key(0, 0x350)) == 3.0f);
    CHECK_FALSE(f.contains(Key(0, 0x320)));
    CHECK_FALSE(f.contains(Key(0, 0x324)));
    CHECK_FALSE(f.contains(Key(1, 0x0)));
    CHECK_FALSE(f.contains(Key(3, 0x8)));
    CHECK_FALSE(f.contains(Key(3, 0xc)));
    CHECK(f.at(Key(kRegionAsFrame, 0)) == 12.0f);
    CHECK_FALSE(f.contains(Key(kRegionAsFrame, 4)));
    CHECK(Where(Key(4, 0x30)) == "c+0x10+0x30");
    CHECK(Where(Key(0, 0x44)) == "actor+0x44");
    CHECK(Where(Key(kRegionAsFrame, 0)) == "as-frame");
}

TEST_CASE("platforms: the finder takes the AS frame, never a guessed word that steps with time (room-objects)") {
    ClockFinder f;
    ClockFinder guessed;  // the same words without the AS frame: a seconds word, a frame counter, a slow place
    float clock = 2.0f;
    float frames = 100.0f;
    float uneven = 0.0f;
    float slow_linear = 0.0f;  // steps steadily but not like a clock (0.37 a second)
    for (int i = 0; i < 8; ++i) {
        const std::uint64_t t = 1'000'000 + static_cast<std::uint64_t>(i) * 250'000;
        Floats look{{Key(2, 0x0), clock},     {Key(2, 0x4), 5.0f},
                    {Key(2, 0x8), uneven},    {Key(2, 0xc), frames},
                    {Key(2, 0x10), slow_linear}};
        guessed.Add(t, look);
        look[kAsKey] = frames;
        f.Add(t, look);
        if (i < 4) {
            CHECK_FALSE(f.Found());  // three agreeing checks first
        }
        clock += 0.25f;
        frames += 7.5f;  // 30 a second
        slow_linear += 0.0925f;
        uneven += (i % 2 == 0) ? 1.0f : 0.1f;
    }
    const auto found = f.Found();
    REQUIRE(found);
    CHECK(found->key == kAsKey);
    CHECK(found->rate == doctest::Approx(30.0f).epsilon(0.01));
    CHECK(found->cycle == 0.0f);
    // What the clock path took in runs (the Deku lifts' frame counter, the water-room controller's place) is never
    // the clock: nothing to send or write.
    CHECK_FALSE(guessed.Found());
    CHECK_FALSE(ClockFinder::Trusted(Key(2, 0xc), false));
    CHECK(ClockFinder::Trusted(Key(2, 0xc), true));  // a wave phase
    CHECK(ClockFinder::Trusted(kAsKey, false));
}

TEST_CASE("platforms: a looping clock gives its cycle and counts as a jump") {
    ClockFinder f;
    float clock = 0.0f;
    bool jumped = false;
    for (int i = 0; i < 20; ++i) {
        f.Add(1'000'000 + static_cast<std::uint64_t>(i) * 250'000, Floats{{kAsKey, clock}});
        jumped = jumped || f.Jumped();
        clock += 0.25f;
        if (clock >= 3.0f) {
            clock -= 3.0f;  // loops every 3 s
        }
    }
    const auto found = f.Found();
    REQUIRE(found);
    CHECK(found->key == kAsKey);
    CHECK(found->cycle == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(jumped);
}

TEST_CASE("platforms: the advance by server time, looped, and the drift the short way") {
    CHECK(Advance(2.0f, 1.0f, 0.0f, 10'000'000, 10'500'000) == doctest::Approx(2.5f));
    CHECK(Advance(2.0f, 30.0f, 0.0f, 10'000'000, 10'100'000) == doctest::Approx(5.0f));
    CHECK(Advance(2.5f, 1.0f, 3.0f, 10'000'000, 11'000'000) == doctest::Approx(0.5f));  // looped
    CHECK(Advance(1.0f, -1.0f, 3.0f, 10'000'000, 12'000'000) == doctest::Approx(2.0f));  // backwards, looped
    CHECK(Advance(2.0f, 1.0f, 0.0f, 10'000'000, 9'990'000) == doctest::Approx(2.0f));  // from the future: 0
    CHECK(DriftSeconds(0.1f, 2.9f, 1.0f, 3.0f) == doctest::Approx(0.2f));  // round the loop
    CHECK(DriftSeconds(1.0f, 2.0f, 1.0f, 0.0f) == doctest::Approx(1.0f));
    CHECK(DriftSeconds(0.0f, 15.0f, 30.0f, 0.0f) == doctest::Approx(0.5f));  // frames: half a second
}

TEST_CASE("platforms: the owner's clock reaches the other game in step, whatever its own clock says") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    a.clock = 1.0f;
    b.clock = 40.0f;               // far out of step
    b.steady_offset = 777'000'000;  // B's own clock is far from A's: only server time counts
    b.sync.Receive({});             // empty message: ignored
    std::uint64_t time = 0;
    // B has nothing from A yet and would send its own; A begins first.
    a.Tick(time);
    Play(a, b, time, 1500);
    Play(a, b, time, 3000, 120);
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    CHECK(std::fabs(a.clock - b.clock) < PlatformSync::kDriftS + 0.06f);
    CHECK(b.sync.Writes() >= 1);
    CHECK(a.sync.Writes() == 0);
    bool followed = false;
    for (const auto& n : b.notes) {
        followed = followed || n.find("following player 1's clock as-frame") != std::string::npos;
    }
    CHECK(followed);
    // In step: no more writes.
    const std::size_t writes = b.sync.Writes();
    Play(a, b, time, 3000);
    CHECK(b.sync.Writes() == writes);
}

TEST_CASE("platforms: a small drift is left alone, and probe mode never writes") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    a.clock = 5.0f;
    b.clock = 5.03f;  // 30 ms off: under the limit
    std::uint64_t time = 0;
    Play(a, b, time, 4000, 30);
    CHECK(b.sync.Writes() == 0);

    Game c;
    Game d;
    c.self = 1;
    d.self = 2;
    d.clock = 20.0f;
    d.probe_only = true;
    time = 0;
    Play(c, d, time, 4000);
    CHECK(d.sync.Writes() == 0);
    CHECK(std::fabs(c.clock - d.clock) > 10.0f);
}

TEST_CASE("platforms: a late tick older than one we have is not used") {
    PlatformSync s;
    // Join the scene first (a Tick sets it).
    PlatformSync::Out out;
    s.Tick("DodongoCavern", 2, 1, 20'000'000, {}, proto::Vec3{}, {}, false, out);
    proto::PlatformClock newer;
    newer.player_number = 1;
    newer.scene = "DodongoCavern";
    newer.server_time_us = 20'000'000;
    newer.ticks.push_back({kHash, kClockKey, 9.0f, 1.0f, 0.0f});
    proto::PlatformClock older = newer;
    older.server_time_us = 19'000'000;
    older.ticks[0].value = 1.0f;
    s.Receive(newer);
    s.Receive(older);  // came late: dropped
    // B's clock at 9.2 (in step with the newer one): no write.
    Game b;
    b.self = 2;
    b.sync = s;
    b.clock = 9.0f;
    std::uint64_t time = 10'000'000;  // server time = time + 10 s = 20 s
    for (int i = 0; i < 30; ++i) {
        time += 50 * kMs;
        b.Step(0.05f);
        b.Tick(time);
        if (i % 10 == 0) {
            newer.server_time_us = time + 10'000'000;
            newer.ticks[0].value = b.clock;
            b.sync.Receive(newer);
        }
    }
    CHECK(b.sync.Writes() == 0);
    // Another scene's tick is not taken.
    proto::PlatformClock other = newer;
    other.scene = "KokiriForest";
    other.player_number = 3;
    b.sync.Receive(other);
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
}

TEST_CASE("platforms: when the owner goes quiet the next game takes over, and a later lower number follows") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    std::uint64_t time = 0;
    Play(a, b, time, 3000);
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    // A leaves (nothing from it any more): after kOwnerQuietUs B sends.
    for (int i = 0; i < 100; ++i) {
        time += 50 * kMs;
        b.Step(0.05f);
        b.Tick(time);
    }
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 2);
    CHECK_FALSE(b.sent.empty());
    b.sent.clear();
    // A comes back with its clock far off: B has sent the longest, A follows B.
    Game a2;
    a2.self = 1;
    a2.clock = 50.0f;
    Play(a2, b, time, 4000);
    CHECK(a2.sync.OwnerOf(kHash, 1, time + 10'000'000) == 2);
    CHECK(std::fabs(a2.clock - b.clock) < PlatformSync::kDriftS + 0.06f);
    CHECK(b.sync.Writes() == 0);
}

TEST_CASE("platforms: two that begin together: the lower number keeps it") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    a.clock = 3.0f;
    b.clock = 30.0f;
    std::uint64_t time = 0;
    Play(a, b, time, 5000);
    CHECK(a.sync.OwnerOf(kHash, 1, time + 10'000'000) == 1);
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    CHECK(std::fabs(a.clock - b.clock) < PlatformSync::kDriftS + 0.06f);
}

TEST_CASE("platforms: a looping clock follows round the loop") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    a.cycle = 4.0f;
    b.cycle = 4.0f;
    a.clock = 0.5f;
    b.clock = 2.5f;
    std::uint64_t time = 0;
    Play(a, b, time, 8000);
    CHECK(DriftSeconds(b.clock, a.clock, 1.0f, 4.0f) < PlatformSync::kDriftS + 0.06f);
}

TEST_CASE("platforms: writes the game takes back each time are stopped") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    b.clock = 30.0f;
    std::uint64_t time = 0;
    // B's game puts its own clock back after every write (a word that only
    // looks like the clock).
    for (int i = 0; i < 400; ++i) {
        time += 50 * kMs;
        a.Step(0.05f);
        b.Step(0.05f);
        a.Tick(time);
        const float before = b.clock;
        b.Tick(time);
        if (b.clock != before) {
            b.clock = before;  // taken back
        }
        for (auto& m : a.sent) {
            auto pc = std::get<proto::PlatformClock>(m);
            pc.player_number = 1;
            b.sync.Receive(pc);
        }
        a.sent.clear();
        b.sent.clear();
    }
    CHECK(b.sync.Writes() == PlatformSync::kMaxNoHold);
    bool stopped = false;
    for (const auto& n : b.notes) {
        stopped = stopped || n.find("do not hold") != std::string::npos;
    }
    CHECK(stopped);
}

TEST_CASE("platforms: a follower whose game runs slower keeps following (#196)") {
    // room-actor-replay-0116, bigrun-0115: the Deku Tree water room's floor stepped 1.155 a second in one game and
    // 0.932 in the other (a per-frame phase, frame rates differ). Every write held, yet 0.5 s later the follower was
    // more than kDriftS off again, and four of those were taken for "the game takes it back".
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    a.cycle = b.cycle = 6.2831853f;
    b.rate = 0.6f;  // 40% slower: 0.2 s off again 0.5 s after each write (over kDriftS)
    b.clock = 3.0f;
    std::uint64_t time = 0;
    float worst_late = 0.0f;
    for (int i = 0; i < 400; ++i) {
        time += 50 * kMs;
        a.Step(0.05f);
        b.Step(0.05f);
        a.Tick(time);
        b.Tick(time);
        for (auto& m : a.sent) {
            auto pc = std::get<proto::PlatformClock>(m);
            pc.player_number = 1;
            b.sync.Receive(pc);
        }
        a.sent.clear();
        b.sent.clear();
        if (i > 200) {
            float d = std::remainder(a.clock - b.clock, a.cycle);
            worst_late = std::max(worst_late, std::fabs(d));
        }
    }
    bool stopped = false;
    bool held = false;
    for (const auto& n : b.notes) {
        stopped = stopped || n.find("do not hold") != std::string::npos;
        held = held || n.find("hold; our game runs at another pace") != std::string::npos;
    }
    CHECK_FALSE(stopped);
    CHECK(held);
    CHECK(b.sync.Writes() > PlatformSync::kMaxNoHold);
    // Put in step every 0.5 s: at most about 0.5 s x 40% of the rate apart (plus a look's step).
    CHECK(worst_late < 0.35f);
}

TEST_CASE("platforms: a new scene forgets the clocks and owners") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    std::uint64_t time = 0;
    Play(a, b, time, 3000);
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    b.Tick(time + 50 * kMs, "KokiriForest");
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 2);
}

TEST_CASE("platforms: a stall right after a loop is no cycle and no jump (review)") {
    ClockFinder f;
    float clock = 0.0f;
    std::uint64_t t = 1'000'000;
    const auto look = [&] {
        f.Add(t, Floats{{kAsKey, clock}});
        t += 250'000;
    };
    // Steps at 1 a second and loops at 3: the cycle is learned.
    for (int i = 0; i < 14; ++i) {
        look();
        clock += 0.25f;
        if (clock >= 3.0f) {
            clock -= 3.0f;
        }
    }
    REQUIRE(f.Found());
    REQUIRE(f.Found()->cycle == doctest::Approx(3.0f).epsilon(0.01));
    // Loop again, then the game stalls (a pause) just after it.
    while (clock > 0.3f) {
        look();
        clock += 0.25f;
        if (clock >= 3.0f) {
            clock -= 3.0f;
        }
    }
    look();  // just past the loop
    clock += 0.25f;
    look();  // one step on, then it stands still
    bool jumped = false;
    for (int i = 0; i < 3; ++i) {  // three looks with the clock standing still
        look();
        jumped = jumped || f.Jumped();
        CHECK_FALSE(f.Found());  // standing still: nothing frozen to send
    }
    CHECK_FALSE(jumped);
    // It goes on: found again with the same cycle.
    for (int i = 0; i < 8; ++i) {
        clock += 0.25f;
        look();
    }
    REQUIRE(f.Found());
    CHECK(f.Found()->cycle == doctest::Approx(3.0f).epsilon(0.01));
}

TEST_CASE("platforms: a wave mover's phase is taken before seconds and frames, with its loop 2 pi at once") {
    // The Deku Tree water room's floor: GroundControlActor_WaveLoop, WaveCycle_Linear 18 s (Banc), so its phase
    // steps 2 pi / 18 = 0.349 a second. Its wave block is at component + 192: the cycle at +8 (+0xc8), the phase
    // at +56 (+0xf8). A seconds word and a frames word step beside it.
    ClockFinder f;
    const std::uint64_t cycle_key = Key(3, 0xc8);
    const std::uint64_t phase_key = Key(3, 0xc8 + kWavePhaseFromCycle);
    float phase = 1.0f;
    float seconds = 50.0f;
    float frames = 3000.0f;
    for (int i = 0; i < 8; ++i) {
        const std::uint64_t t = 1'000'000 + static_cast<std::uint64_t>(i) * 250'000;
        f.Add(t, Floats{{Key(3, 0x0), seconds}, {Key(3, 0x4), frames}, {cycle_key, 18.0f},
                        {cycle_key + 12, 4.0f}, {phase_key, phase}});
        seconds += 0.25f;
        frames += 15.0f;
        phase += kTwoPi / 18.0f * 0.25f;
    }
    const auto found = f.Found();
    REQUIRE(found);
    CHECK(found->key == phase_key);
    CHECK(found->rate == doctest::Approx(kTwoPi / 18.0f).epsilon(0.01));
    CHECK(found->cycle == doctest::Approx(kTwoPi));
}

TEST_CASE("platforms: a word that steps but not at 2 pi over the word before it is no wave phase") {
    const Floats look{{Key(3, 0x10), 18.0f}, {Key(3, 0x40), 1.0f}, {Key(3, 0x20), 1.0f}};
    CHECK(WavePhaseCycle(look, Key(3, 0x40), kTwoPi / 18.0f).has_value());
    CHECK_FALSE(WavePhaseCycle(look, Key(3, 0x40), 1.0f).has_value());   // a seconds clock
    CHECK_FALSE(WavePhaseCycle(look, Key(3, 0x20), kTwoPi / 18.0f).has_value());  // no cycle 48 bytes before
    CHECK_FALSE(WavePhaseCycle(look, Key(3, 0x10), kTwoPi / 18.0f).has_value());  // first words of a region
    CHECK_FALSE(WavePhaseCycle(look, Key(3, 0x40), -kTwoPi / 18.0f).has_value());
}

TEST_CASE("platforms: a follower's pause menu does not stop it following, and it is in step after (#35)") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    b.clock = 30.0f;
    std::uint64_t time = 0;
    Play(a, b, time, 4000);
    REQUIRE(std::fabs(b.clock - a.clock) < PlatformSync::kDriftS + 0.06f);
    const std::size_t before = b.sync.Writes();
    b.paused = true;
    Play(a, b, time, 4000);
    CHECK(b.sync.Writes() == before);  // nothing written into a game that stands still
    b.paused = false;
    Play(a, b, time, 3000);
    CHECK(std::fabs(b.clock - a.clock) < PlatformSync::kDriftS + 0.06f);
    for (const auto& n : b.notes) {
        CHECK(n.find("do not hold") == std::string::npos);
    }
}

TEST_CASE("platforms: an owner's short pause menu does not pull the others back (#35)") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    b.clock = 30.0f;
    std::uint64_t time = 0;
    Play(a, b, time, 4000);
    REQUIRE(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    const float shared = a.clock;  // where the shared clock is now; it goes on at 1 a second
    a.paused = true;
    Play(a, b, time, 1500);  // A stays the owner (it was quiet less than kOwnerQuietUs)
    CHECK(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    a.paused = false;
    Play(a, b, time, 3000);
    CHECK(std::fabs(a.clock - (shared + 4.5f)) < PlatformSync::kDriftS + 0.06f);
    CHECK(std::fabs(b.clock - (shared + 4.5f)) < PlatformSync::kDriftS + 0.06f);
    bool caught_up = false;
    for (const auto& n : a.notes) {
        caught_up = caught_up || n.find("stood still") != std::string::npos;
    }
    CHECK(caught_up);
    for (const auto& n : b.notes) {
        CHECK(n.find("do not hold") == std::string::npos);
    }
}

TEST_CASE("platforms: our Link rides by the game's RideOnPlayer and the platform's footprint (#194)") {
    PlatformSync::Driver lift{0x1000, kHash, "Obj_InsideTheDekuTree_ElevatorLift_A_02", {10.0f, 5.0f, 0.0f}, {}};
    const proto::Vec3 on{10.5f, 5.2f, 0.5f};
    const proto::Vec3 beside{13.0f, 5.0f, 0.0f};
    const proto::Vec3 under{10.0f, 3.0f, 0.0f};
    CHECK(PlatformSync::OurLinkRides(lift, on));  // no plug: the footprint
    CHECK_FALSE(PlatformSync::OurLinkRides(lift, beside));
    CHECK_FALSE(PlatformSync::OurLinkRides(lift, under));
    lift.ride_plug = true;
    CHECK(PlatformSync::OurLinkRides(lift, on));
    CHECK(PlatformSync::OurLinkRides(lift, beside));  // a wide platform: our Link on its far edge (review)
    lift.other_on = true;
    CHECK_FALSE(PlatformSync::OurLinkRides(lift, beside));  // another player's puppet on our copy
    CHECK(PlatformSync::OurLinkRides(lift, on));            // both of us on it
    lift.other_on = false;
    lift.ride_plug = false;
    CHECK_FALSE(PlatformSync::OurLinkRides(lift, on));  // the game says nobody is on it
    PlatformSync::Driver mover{0x2000, kHash, "GroundControlActor_ResidentMoveTo", {10.0f, 5.0f, 0.0f}, {}};
    CHECK_FALSE(PlatformSync::OurLinkRides(mover, on));  // a controller: no footprint, only its plug
    mover.ride_plug = true;
    CHECK(PlatformSync::OurLinkRides(mover, beside));
}

TEST_CASE("platforms: no clock write while our Link rides it, in step once he is off (#194)") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    b.clock = 30.0f;  // far out of step: a write would make the lift jump
    for (Game* g : {&a, &b}) {
        g->driver_name = "Obj_Deku_MoveFloor_A_01";  // a free floor (not in this scene's table): its footprint
        g->driver_at = {1.0f, 0.0f, 0.0f};
    }
    a.link = {6.0f, 0.0f, 0.0f};  // A beside its floor
    b.link = {1.2f, 0.3f, 0.0f};  // B stands on its floor
    std::uint64_t time = 0;
    a.Tick(time);
    Play(a, b, time, 4000);
    CHECK(b.sync.Writes() == 0);
    CHECK(std::fabs(a.clock - b.clock) > 10.0f);
    const auto has = [&b](const char* text) {
        for (const auto& n : b.notes) {
            if (n.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    CHECK(has("our Link rides it (footprint)"));
    // Off the floor: put in step, and the held writes count as none the game took back.
    b.link = {6.0f, 0.0f, 0.0f};
    Play(a, b, time, 3000);
    CHECK(b.sync.Writes() >= 1);
    CHECK(std::fabs(a.clock - b.clock) < PlatformSync::kDriftS + 0.06f);
    CHECK(has("our Link is off it"));
    CHECK_FALSE(has("do not hold"));
}

TEST_CASE("platforms: a jump on the platform keeps the hold, a longer time off it does not (#194 review)") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    b.clock = 30.0f;
    for (Game* g : {&a, &b}) {
        g->driver_name = "Obj_Deku_MoveFloor_A_01";
    }
    a.link = {6.0f, 0.0f, 0.0f};
    b.link = {1.2f, 0.3f, 0.0f};
    std::uint64_t time = 0;
    a.Tick(time);
    Play(a, b, time, 3000);
    REQUIRE(b.sync.Writes() == 0);
    // A jump: Link is over the footprint's top for half a second.
    b.link = {1.2f, 3.0f, 0.0f};
    Play(a, b, time, 500);
    CHECK(b.sync.Writes() == 0);
    // Landed again: still held.
    b.link = {1.2f, 0.3f, 0.0f};
    Play(a, b, time, 1000);
    CHECK(b.sync.Writes() == 0);
    // Off it for good: in step after the grace.
    b.link = {6.0f, 0.0f, 0.0f};
    Play(a, b, time, 3000);
    CHECK(b.sync.Writes() >= 1);
    CHECK(std::fabs(a.clock - b.clock) < PlatformSync::kDriftS + 0.06f);
}

TEST_CASE("platforms: an owner riding its platform does not put its clock back after a pause (#194 review)") {
    Game a;
    Game b;
    a.self = 1;
    b.self = 2;
    b.clock = 30.0f;
    for (Game* g : {&a, &b}) {
        g->driver_name = "Obj_Deku_MoveFloor_A_01";
        g->link = {6.0f, 0.0f, 0.0f};
    }
    std::uint64_t time = 0;
    Play(a, b, time, 4000);
    REQUIRE(b.sync.OwnerOf(kHash, 2, time + 10'000'000) == 1);
    a.link = {1.2f, 0.3f, 0.0f};  // A stands on its floor
    a.paused = true;
    Play(a, b, time, 1500);
    a.paused = false;
    Play(a, b, time, 3000);
    CHECK(a.sync.Writes() == 0);
    for (const auto& n : a.notes) {
        CHECK(n.find("stood still") == std::string::npos);
    }
}

TEST_CASE("platforms: an object its own logic drives is neither sent nor written (room-objects)") {
    struct Result {
        std::size_t sent = 0;
        std::size_t writes = 0;
        bool told = false;
    };
    const auto run = [](const char* name, const char* scene, proto::Vec3 at, std::optional<bool> plug) {
        Game a;
        Game b;
        a.self = 1;
        b.self = 2;
        b.clock = 30.0f;
        for (Game* g : {&a, &b}) {
            g->driver_name = name;
            g->driver_at = at;
            g->ride_plug = plug;
            g->link = {at[0] + 6.0f, at[1], at[2]};
        }
        Result r;
        std::uint64_t time = 0;
        for (std::uint64_t t = 0; t < 4000; t += 50) {
            time += 50 * kMs;
            a.Step(0.05f);
            b.Step(0.05f);
            a.Tick(time, scene);
            b.Tick(time, scene);
            r.sent += a.sent.size() + b.sent.size();
            for (auto& m : a.sent) {
                auto pc = std::get<proto::PlatformClock>(m);
                pc.player_number = a.self;
                b.sync.Receive(pc);
            }
            a.sent.clear();
            b.sent.clear();
        }
        r.writes = a.sync.Writes() + b.sync.Writes();
        for (const auto& n : b.notes) {
            r.told = r.told || n.find("driven by its own logic") != std::string::npos;
        }
        return r;
    };
    // The Deku Tree compass-room lift: IsOn from its plate (CoopEyeSwitch sets the plate's plug).
    const Result lift = run("Obj_InsideTheDekuTree_ElevatorLift_A_02", "InsideTheDekuTree", {-29.9f, 116.0f, 3.2f}, {});
    CHECK(lift.sent == 0);
    CHECK(lift.writes == 0);
    CHECK(lift.told);
    // Its limit part placed on it moves with it.
    const Result lim = run("Obj_InsideTheDekuTree_ElevatorLiftLim_A_02", "InsideTheDekuTree", {-29.9f, 118.0f, 3.2f}, {});
    CHECK(lim.sent == 0);
    // A rider lift by its live plug (CoopLiftRide sets it for the other players), in any scene.
    const Result rider = run("Obj_InsideJabu_Jabu_MoveFloor_A_01", "DodongoCavern", {0.0f, 0.0f, 0.0f}, false);
    CHECK(rider.sent == 0);
    CHECK(rider.told);
    // The water room's wave runs free: shared and followed.
    const Result wave = run("GroundControlActor_WaveLoop", "InsideTheDekuTree", {-26.2f, 82.3f, 38.0f}, {});
    CHECK(wave.sent > 0);
    CHECK(wave.writes > 0);
    CHECK_FALSE(wave.told);
}
