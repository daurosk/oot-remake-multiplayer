// Tests for looping platforms' pause counters (lichii/hook/coop_counters.cpp).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include "coop_counters.h"

using namespace CoopCounters;
namespace proto = coop::protocol;

namespace {

constexpr std::uint64_t kMs = 1000;
constexpr std::uint64_t kHash = 0x51802f1732232227ull;

// One game's lift: a pause of `pause` frames, then a move of `move` s, again.
// `phase_s` is where in the cycle it is at time 0.
struct Lift {
    std::uint8_t self = 1;
    float pause = 60.0f;  // frames (1 s)
    float move = 2.0f;    // seconds
    double t_in = 0.0;    // seconds into the cycle
    float remaining = 0.0f;
    std::uint64_t node = 0x2140001000;
    std::uint64_t graph = 0x2140000f00;
    std::string name = "Obj_DodongoCave_MoveFloor_A_01";
    CounterSync sync;
    std::vector<proto::PlatformCounter> sent;
    std::vector<std::string> notes;
    bool frozen = false;  // the game paused
    bool probe = false;   // LICHII_COOP_PLATFORMS=probe

    double Cycle() const { return pause / 60.0 + move; }
    // Where the lift is (0 at the bottom pause, 1 at the top), for the check.
    double Phase() const { return std::fmod(t_in, Cycle()) / Cycle(); }

    void Step(double dt) {
        if (frozen) {
            return;
        }
        t_in += dt;
        const double into = std::fmod(t_in, Cycle());
        remaining = into < pause / 60.0 ? pause - static_cast<float>(into * 60.0) : 0.0f;
    }

    void Tick(std::uint64_t time_us, const std::string& scene = "DodongoCavern") {
        Seen s;
        s.node = node;
        s.owner_hash = kHash;
        s.owner_name = name;
        s.remaining = remaining;
        s.duration = pause;
        s.key = KeyOf(kHash, pause, node, graph);
        CounterSync::Out out;
        sync.Tick(scene, self, time_us + 50'000'000, {s}, probe, out);
        for (auto& m : out.send) {
            sent.push_back(std::get<proto::PlatformCounter>(m));
        }
        for (const auto& w : out.writes) {
            CHECK(w.node == node);
            // The game's pause is that much shorter or longer: the cycle moves.
            t_in += static_cast<double>(w.was - w.remaining) / 60.0;
            remaining = w.remaining;
        }
        notes.insert(notes.end(), out.notes.begin(), out.notes.end());
    }
};

void Play(Lift& a, Lift& b, std::uint64_t& time_us, std::uint64_t ms, std::uint64_t delay_ms = 80) {
    struct InFlight {
        std::uint64_t at_us;
        proto::PlatformCounter m;
        bool to_b;
    };
    std::vector<InFlight> flight;
    for (std::uint64_t t = 0; t < ms; t += 33) {
        time_us += 33 * kMs;
        a.Step(0.033);
        b.Step(0.033);
        a.Tick(time_us);
        b.Tick(time_us);
        for (auto& m : a.sent) {
            m.player_number = a.self;
            flight.push_back({time_us + delay_ms * kMs, m, true});
        }
        for (auto& m : b.sent) {
            m.player_number = b.self;
            flight.push_back({time_us + delay_ms * kMs, m, false});
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

double PhaseApart(const Lift& a, const Lift& b) {
    double d = std::fabs(a.Phase() - b.Phase());
    return std::min(d, 1.0 - d) * a.Cycle();  // seconds
}

}  // namespace

TEST_CASE("counters: the cave's jump and its bytes") {
    CHECK(HookWord() == 0x14d97fe8u);  // b 0x3c46700 at main+0x5e6760
    CHECK(kCave[0] == 0x7f);           // pacibsp first, as the game had it
    CHECK(kCave.size() == 40);
    CHECK(kRingPointer >= kCaveOffset + kCave.size());
}

TEST_CASE("counters: ring entries, keys and owners") {
    std::array<std::uint64_t, kRingNodes> e{};
    e[0] = 5;
    e[1] = 7;
    e[2] = 5;
    CHECK(RingNodes(e) == std::vector<std::uint64_t>{5, 7});
    // The same placement, length and place in the graph: the same key, whatever the addresses.
    CHECK(KeyOf(kHash, 60.0f, 0x2140001000, 0x2140000f00) == KeyOf(kHash, 60.0f, 0x3170001000, 0x3170000f00));
    CHECK(KeyOf(kHash, 60.0f, 0x2140001000, 0x2140000f00) != KeyOf(kHash, 90.0f, 0x2140001000, 0x2140000f00));
    CHECK(KeyOf(kHash, 60.0f, 0x2140001000, 0x2140000f00) != KeyOf(kHash, 60.0f, 0x2140001100, 0x2140000f00));
    CHECK(KeyOf(kHash, 60.0f, 0x2140001000, 0x2140000f00) != KeyOf(kHash + 1, 60.0f, 0x2140001000, 0x2140000f00));
    CHECK(IsPlatformOwner("Obj_DodongoCave_MoveFloor_A_01"));
    CHECK_FALSE(IsPlatformOwner("Enemy_Dodongo"));
    CHECK_FALSE(IsPlatformOwner("Npc_Kokiri"));
    CHECK_FALSE(IsPlatformOwner("Player_Child"));
}

TEST_CASE("counters: the gap the short way round the period") {
    CHECK(PhaseGap(10'500'000, 10'000'000, 3'000'000) == doctest::Approx(0.5f));
    CHECK(PhaseGap(10'000'000, 10'500'000, 3'000'000) == doctest::Approx(-0.5f));
    CHECK(PhaseGap(12'800'000, 10'000'000, 3'000'000) == doctest::Approx(-0.2f));  // round the loop
    CHECK(PhaseGap(16'100'000, 10'000'000, 3'000'000) == doctest::Approx(0.1f));   // two loops on
}

TEST_CASE("counters: two lifts out of phase come into phase once and stay") {
    Lift a;
    Lift b;
    a.self = 1;
    b.self = 2;
    b.t_in = 1.6;  // 1.6 s out of a 3 s cycle
    std::uint64_t time = 0;
    Play(a, b, time, 3000);  // a cycle: both start counting
    a.Step(0.0);
    CHECK(PhaseApart(a, b) > 1.0);
    Play(a, b, time, 15000);
    CHECK(PhaseApart(a, b) < 0.1);
    CHECK(a.sync.Writes() + b.sync.Writes() == 1);  // one write, by one game
    CHECK(b.sync.Writes() == 1);                     // a tie of starts: the higher number moves
    CHECK(a.sync.Looping(KeyOf(kHash, 60.0f, a.node, a.graph)));
    bool logged = false;
    for (const auto& n : b.notes) {
        logged = logged || n.find("off player 1's: frames left") != std::string::npos;
    }
    CHECK(logged);
    const std::size_t writes = b.sync.Writes();
    Play(a, b, time, 20000);
    CHECK(b.sync.Writes() == writes);  // in phase: nothing more
    CHECK(PhaseApart(a, b) < 0.1);
    CHECK(a.sync.LoopingOwners().contains(kHash));
}

TEST_CASE("counters: a game that came later moves, not the one with more starts") {
    Lift a;
    Lift b;
    a.self = 2;  // the higher number, but here first
    b.self = 1;
    b.t_in = 0.9;
    std::uint64_t time = 0;
    // A alone for 12 s: many starts.
    for (int i = 0; i < 360; ++i) {
        time += 33 * kMs;
        a.Step(0.033);
        a.Tick(time);
        a.sent.clear();
    }
    Play(a, b, time, 20000);
    CHECK(a.sync.Writes() == 0);
    CHECK(b.sync.Writes() == 1);
    CHECK(PhaseApart(a, b) < 0.1);
}

TEST_CASE("counters: a pause menu puts one out of phase, and the next pause puts it back") {
    Lift a;
    Lift b;
    a.self = 1;
    b.self = 2;
    std::uint64_t time = 0;
    Play(a, b, time, 12000);
    CHECK(PhaseApart(a, b) < 0.1);
    CHECK(a.sync.Writes() + b.sync.Writes() == 0);  // in phase from the start
    b.frozen = true;  // B's player opens the menu for 0.8 s
    Play(a, b, time, 800);
    b.frozen = false;
    CHECK(PhaseApart(a, b) > 0.5);
    Play(a, b, time, 15000);
    CHECK(PhaseApart(a, b) < 0.1);
}

TEST_CASE("counters: a counter that never loops (a switch's one move) is never sent or written") {
    Lift a;
    a.self = 1;
    std::uint64_t time = 0;
    a.remaining = 30.0f;  // one pause, then it stays still
    a.frozen = true;
    for (int i = 0; i < 300; ++i) {
        time += 33 * kMs;
        a.remaining = std::max(0.0f, a.remaining - 2.0f);
        a.Tick(time);
    }
    CHECK(a.sent.empty());
    CHECK(a.sync.LoopingOwners().empty());
}

TEST_CASE("counters: a late or other scene's line is not used, and a scene change forgets") {
    CounterSync s;
    CounterSync::Out out;
    s.Tick("DodongoCavern", 2, 1'000'000, {}, false, out);
    proto::PlatformCounter m;
    m.player_number = 1;
    m.scene = "KokiriForest";
    m.server_time_us = 900'000;
    m.ticks.push_back({1234, 3'000'000, 100'000, 5});
    s.Receive(m);  // another scene: dropped
    Lift b;
    b.self = 2;
    b.sync = s;
    b.t_in = 1.0;
    std::uint64_t time = 0;
    for (int i = 0; i < 400; ++i) {
        time += 33 * kMs;
        b.Step(0.033);
        b.Tick(time);
    }
    CHECK(b.sync.Writes() == 0);
    CHECK_FALSE(b.sent.empty());
    b.Tick(time + 33 * kMs, "KokiriForest");
    CHECK(b.sync.LoopingOwners().empty());
}

TEST_CASE("counters: two of our nodes with one key are neither sent nor followed") {
    CounterSync s;
    std::uint64_t time = 50'000'000;
    float r = 0.0f;
    for (int i = 0; i < 600; ++i) {
        time += 33 * kMs;
        r = (i % 90 == 0) ? 60.0f : std::max(0.0f, r - 2.0f);
        Seen x{0x10, 99, kHash, "Obj_X", r, 60.0f};
        Seen y{0x20, 99, kHash, "Obj_X", r, 60.0f};
        CounterSync::Out out;
        s.Tick("DodongoCavern", 1, time, {x, y}, false, out);
        CHECK(out.send.empty());
    }
}

TEST_CASE("counters: probe mode says what it would write, writes nothing, keeps its own loop (review)") {
    Lift a;
    Lift b;
    a.self = 1;
    b.self = 2;
    b.probe = true;
    b.t_in = 1.6;
    std::uint64_t time = 0;
    Play(a, b, time, 20000);
    CHECK(b.sync.Writes() == 0);
    CHECK(PhaseApart(a, b) > 1.0);  // still out of phase
    int said = 0;
    for (const auto& n : b.notes) {
        said += n.find("(probe: not written)") != std::string::npos ? 1 : 0;
    }
    CHECK(said >= 2);  // once a pause, every pause: its loop is still seen
    CHECK(b.sync.Looping(KeyOf(kHash, 60.0f, b.node, b.graph)));
}

TEST_CASE("counters: other actors' nodes are skipped only while they are still theirs (review)") {
    OtherNodes o;
    o.Add(0x100, 0xa0);  // an enemy's counter
    o.Add(0x200, 0xb0);  // an NPC's
    CHECK(o.Skip(0x100, 0xa0));
    CHECK_FALSE(o.Skip(0x300, 0xa0));  // never kept
    // The heap gave the enemy's node memory to a lift's node: read again.
    CHECK_FALSE(o.Skip(0x100, 0xc0));
    CHECK_FALSE(o.Contains(0x100));
    CHECK_FALSE(o.Skip(0x100, 0xa0));  // and forgotten for good
    // The NPC leaves the walk: forgotten.
    o.Keep([](std::uint64_t actor) { return actor != 0xb0; });
    CHECK_FALSE(o.Contains(0x200));
    // A scene change clears all; a full set starts again.
    o.Add(0x400, 0xd0);
    o.Clear();
    CHECK(o.Size() == 0);
    for (std::uint64_t i = 0; i < OtherNodes::kMax; ++i) {
        o.Add(0x1000 + 8 * i, 0xe0);
    }
    CHECK(o.Size() == OtherNodes::kMax);
    o.Add(0x9, 0xe0);
    CHECK(o.Size() == 1);
}

TEST_CASE("counters: the loop from the periods: one, or two that alternate (2026-11-12 run)") {
    using D = std::deque<std::uint64_t>;
    CHECK(LoopOf(D{3'000'000, 3'050'000}, 0.08f) == 3'050'000);
    CHECK(LoopOf(D{3'000'000}, 0.08f) == 0);
    // One node times both waits; up 2 s, down 4 s, 1 s waits: 3 s, 5 s, 3 s, 5 s.
    CHECK(LoopOf(D{3'000'000, 5'000'000, 3'000'000, 5'000'000}, 0.08f) == 8'000'000);
    CHECK(LoopOf(D{5'000'000, 3'000'000, 5'000'000, 3'000'000}, 0.08f) == 8'000'000);
    CHECK(LoopOf(D{3'000'000, 5'000'000, 3'000'000}, 0.08f) == 0);  // not yet
    CHECK(LoopOf(D{3'000'000, 5'000'000, 4'000'000, 6'500'000}, 0.08f) == 0);
}

namespace {

// A lift whose one counter times both of its waits: wait, up, wait, down.
struct TwoWaitLift {
    std::uint8_t self = 1;
    float pause = 60.0f;   // frames, both waits
    double up = 2.0;       // seconds
    double down = 4.0;
    double t_in = 0.0;
    float remaining = 0.0f;
    std::uint64_t node = 0x2140002000;
    std::uint64_t graph = 0x2140001f00;
    CounterSync sync;
    std::vector<proto::PlatformCounter> sent;
    std::vector<std::string> notes;

    double Cycle() const { return 2 * pause / 60.0 + up + down; }
    void Step(double dt) {
        t_in += dt;
        const double into = std::fmod(t_in, Cycle());
        const double w = pause / 60.0;
        if (into < w) {
            remaining = pause - static_cast<float>(into * 60.0);
        } else if (into >= w + up && into < 2 * w + up) {
            remaining = pause - static_cast<float>((into - w - up) * 60.0);
        } else {
            remaining = 0.0f;
        }
    }
    void Tick(std::uint64_t time_us) {
        Seen s{node, KeyOf(kHash, pause, node, graph), kHash, "Obj_DodongoCave_MoveFloor_A_01", remaining, pause};
        CounterSync::Out out;
        sync.Tick("DodongoCavern", self, time_us + 50'000'000, {s}, false, out);
        for (auto& m : out.send) {
            sent.push_back(std::get<proto::PlatformCounter>(m));
        }
        for (const auto& w : out.writes) {
            t_in += static_cast<double>(w.was - w.remaining) / 60.0;
            remaining = w.remaining;
        }
        notes.insert(notes.end(), out.notes.begin(), out.notes.end());
    }
    double Apart(const TwoWaitLift& o) const {
        double d = std::fabs(std::fmod(t_in, Cycle()) - std::fmod(o.t_in, Cycle()));
        return std::min(d, Cycle() - d);
    }
};

}  // namespace

TEST_CASE("counters: a lift timing both waits with one counter loops, and comes into phase") {
  for (const double offset : {0.5, 1.5, 3.3, 4.2, 5.0, 7.2}) {
    CAPTURE(offset);
    TwoWaitLift a;
    TwoWaitLift b;
    a.self = 1;
    b.self = 2;
    b.t_in = offset;  // seconds into its 8 s loop
    std::uint64_t time = 0;
    std::vector<std::pair<std::uint64_t, proto::PlatformCounter>> to_a, to_b;
    for (int i = 0; i < 1500; ++i) {  // 50 s
        time += 33 * kMs;
        a.Step(0.033);
        b.Step(0.033);
        a.Tick(time);
        b.Tick(time);
        for (auto& m : a.sent) {
            m.player_number = 1;
            b.sync.Receive(m);
        }
        for (auto& m : b.sent) {
            m.player_number = 2;
            a.sync.Receive(m);
        }
        a.sent.clear();
        b.sent.clear();
    }
    const auto key = KeyOf(kHash, 60.0f, a.node, a.graph);
    CHECK(a.sync.CycleOf(key) == doctest::Approx(8'000'000).epsilon(0.02));
    CHECK(a.Apart(b) < 0.1);
    CHECK(a.sync.Writes() + b.sync.Writes() <= 1);
    bool two = false;
    for (const auto& n : a.notes) {
        two = two || n.find("(two waits)") != std::string::npos;
    }
    CHECK(two);
  }
}

TEST_CASE("counters: every platform actor with a counter leaves the clock path at once, looping or not") {
    CounterSync s;
    CounterSync::Out out;
    Seen x{0x10, 7, kHash, "Obj_DodongoCave_MoveFloor_A_01", 40.0f, 60.0f};
    Seen enemy{0x20, 8, 0x99, "Enemy_Dodongo", 40.0f, 60.0f};
    s.Tick("DodongoCavern", 1, 1'000'000, {x, enemy}, false, out);
    CHECK(s.CounterOwners().contains(kHash));
    CHECK_FALSE(s.CounterOwners().contains(0x99));
    CHECK(s.LoopingOwners().empty());
    // Still kept when its node is out of sight a while (the walk lost it), until the scene changes.
    s.Tick("DodongoCavern", 1, 2'000'000, {}, false, out);
    CHECK(s.CounterOwners().contains(kHash));
    s.Tick("KokiriForest", 1, 3'000'000, {}, false, out);
    CHECK(s.CounterOwners().empty());
}

TEST_CASE("counters: one odd period does not stop a loop (hysteresis)") {
    Lift a;
    a.self = 1;
    std::uint64_t time = 0;
    for (int i = 0; i < 400; ++i) {
        time += 33 * kMs;
        a.Step(0.033);
        a.Tick(time);
    }
    const auto key = KeyOf(kHash, 60.0f, a.node, a.graph);
    REQUIRE(a.sync.Looping(key));
    a.t_in += 0.7;  // a hitch: one period 0.7 s short
    for (int i = 0; i < 120; ++i) {
        time += 33 * kMs;
        a.Step(0.033);
        a.Tick(time);
        CHECK(a.sync.Looping(key));
    }
}

namespace {

// One game's lift node as the 2026-11-12 10:50 run logged it
// (coop-kit/shots/platforms-dodongo/20261112-105044, the first 180-frame
// Obj_DodongoCave_MoveFloor_A_01 node of each game): its starts (seconds,
// from the 10 s "pause counters:" lines: time - (180 - left) / 60), and
// below zero (-0.6) between pauses, as the game leaves it.
struct LogLift {
    std::uint8_t self = 1;
    std::vector<double> starts;
    double shift = 0.0;  // our writes: the pause ends this much sooner (s)
    float remaining = -0.6f;
    std::uint64_t node = 0x2140001000;
    std::uint64_t graph = 0x2140000f00;
    CounterSync sync;
    std::vector<proto::PlatformCounter> sent;
    std::vector<std::string> notes;

    void At(double t) {
        remaining = -0.6f;
        for (const double s : starts) {
            const double into = t - (s - shift);
            if (into >= 0.0 && into * 60.0 < 180.0) {
                remaining = 180.0f - static_cast<float>(into * 60.0);
            }
        }
    }
    void Tick(double t) {
        Seen s;
        s.node = node;
        s.owner_hash = kHash;
        s.owner_name = "Obj_DodongoCave_MoveFloor_A_01";
        s.remaining = remaining;
        s.duration = 180.0f;
        s.key = KeyOf(kHash, 180.0f, node, graph);
        CounterSync::Out out;
        sync.Tick("DodongoCavern", self, static_cast<std::uint64_t>(t * 1e6), {s}, false, out);
        for (auto& m : out.send) {
            auto pc = std::get<proto::PlatformCounter>(m);
            pc.player_number = self;
            sent.push_back(pc);
        }
        for (const auto& w : out.writes) {
            shift += static_cast<double>(w.was - w.remaining) / 60.0;
            remaining = w.remaining;
        }
        notes.insert(notes.end(), out.notes.begin(), out.notes.end());
    }
    // Our next start after t (s), with our writes.
    double NextStart(double t) const {
        for (const double s : starts) {
            if (s - shift > t) {
                return s - shift;
            }
        }
        return 0.0;
    }
};

bool Said(const std::vector<std::string>& notes, const char* what) {
    return std::any_of(notes.begin(), notes.end(),
                       [&](const std::string& n) { return n.find(what) != std::string::npos; });
}

}  // namespace

TEST_CASE("counters: the Dodongo lift starts of the 2026-11-12 run loop, though the counter goes below zero") {
    LogLift a;
    LogLift b;
    a.self = 1;
    b.self = 2;
    // From the log (A at 79.85 10.7 left, 99.92 65.9, 119.99 117.0, 140.01
    // 172.6; B at 79.17 9.6, 99.21 67.6, 119.24 122.9, 139.29 177.3), then
    // on at the same period so the test can see that it stays in phase.
    a.starts = {77.03, 98.02, 118.94, 139.88, 160.81, 181.74, 202.67};
    b.starts = {76.33, 97.34, 118.29, 139.24, 160.17, 181.10, 202.03};
    struct InFlight {
        double at;
        proto::PlatformCounter m;
        bool to_b;
    };
    std::vector<InFlight> flight;
    for (double t = 70.0; t < 215.0; t += 0.033) {
        a.At(t);
        b.At(t);
        a.Tick(t);
        b.Tick(t);
        for (auto& m : a.sent) {
            flight.push_back({t + 0.08, m, true});
        }
        for (auto& m : b.sent) {
            flight.push_back({t + 0.08, m, false});
        }
        a.sent.clear();
        b.sent.clear();
        for (auto it = flight.begin(); it != flight.end();) {
            if (it->at <= t) {
                (it->to_b ? b : a).sync.Receive(it->m);
                it = flight.erase(it);
            } else {
                ++it;
            }
        }
    }
    const auto key = KeyOf(kHash, 180.0f, a.node, a.graph);
    CHECK(a.sync.Looping(key));  // the old code: "0 looping" the whole run
    CHECK(b.sync.Looping(key));
    CHECK(std::llabs(static_cast<long long>(a.sync.CycleOf(key)) - 20'930'000) < 100'000);
    CHECK(Said(a.notes, "loops every 20.9"));
    CHECK(Said(b.notes, "loops every 20.9"));
    CHECK(a.sync.Writes() + b.sync.Writes() == 1);  // one write, by one game
    CHECK(b.sync.Writes() == 1);                     // a tie of starts: the higher number moves
    CHECK(Said(b.notes, "s off player 1's: frames left"));
    // In phase from then on: the next starts within 0.3 s of each other.
    CHECK(std::fabs(a.NextStart(185.0) - b.NextStart(185.0)) < 0.3);
}
