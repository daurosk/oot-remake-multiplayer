// Tests for the game's actor events on Hookshot (lichii/hook/coop_actor_events.h).
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "coop_actor_events.h"

using CoopActorEvents::Event;
using CoopActorEvents::Kind;
using CoopActorEvents::Ring;

namespace {
Event Make(Kind k, std::uint64_t a, std::uint64_t b) {
    Event e;
    e.kind = k;
    e.a = a;
    e.b = b;
    return e;
}
} // namespace

TEST_CASE("actor events: drained in push order, each once") {
    static Ring ring;  // (big: not on the stack)
    ring.Push(Make(Kind::Create, 0x100, 0x200));
    ring.Push(Make(Kind::Delete, 0x300, 1));
    ring.Push(Make(Kind::Damage, 0x400, 0xfffffffeu));
    std::size_t lost = 9;
    auto got = ring.Drain(&lost);
    REQUIRE(got.size() == 3);
    CHECK(lost == 0);
    CHECK(got[0].kind == Kind::Create);
    CHECK(got[0].b == 0x200);
    CHECK(got[1].kind == Kind::Delete);
    CHECK(got[1].a == 0x300);
    CHECK(got[2].kind == Kind::Damage);
    CHECK(CoopActorEvents::Amount(got[2].b) == -2);
    CHECK(ring.Drain().empty());
    CHECK(ring.Pushed() == 3);
}

TEST_CASE("actor events: more than the ring holds between two drains counts the oldest as lost") {
    static Ring ring;
    const std::size_t n = Ring::kSlots + 10;
    for (std::size_t i = 0; i < n; ++i) {
        ring.Push(Make(Kind::Damage, i, 1));
    }
    std::size_t lost = 0;
    auto got = ring.Drain(&lost);
    CHECK(got.size() == Ring::kSlots);
    CHECK(lost == 10);
    CHECK(got.front().a == 10);
    CHECK(got.back().a == n - 1);
}

TEST_CASE("actor events: a new boot forgets what was pushed before") {
    static Ring ring;
    ring.Push(Make(Kind::Delete, 1, 0));
    ring.Forget();
    CHECK(ring.Drain().empty());
    ring.Push(Make(Kind::Delete, 2, 0));
    auto got = ring.Drain();
    REQUIRE(got.size() == 1);
    CHECK(got[0].a == 2);
}

TEST_CASE("actor events: several game threads push at once, nothing is torn") {
    static Ring ring;
    constexpr int kThreads = 4;
    constexpr int kEach = 500;  // (4 x 500 < kSlots: none lost)
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kEach; ++i) {
                const std::uint64_t v = (static_cast<std::uint64_t>(t) << 32) | static_cast<std::uint64_t>(i);
                ring.Push(Make(Kind::Damage, v, v ^ 0xabcdefu));
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    std::size_t lost = 0;
    auto got = ring.Drain(&lost);
    CHECK(lost == 0);
    CHECK(got.size() == static_cast<std::size_t>(kThreads * kEach));
    for (const auto& e : got) {
        CHECK(e.b == (e.a ^ 0xabcdefu));
    }
}

TEST_CASE("actor events: a hit given goes in through the next Life step once, and the game's own is reported") {
    static Ring ring;
    static CoopActorEvents::LifeTable table;
    CHECK(CoopActorEvents::Step(ring, table, 0x500, 0) == 0);  // (nothing given, no hit)
    CHECK(ring.Drain().empty());
    CHECK(table.Give(0x500, 3));
    CHECK(table.Give(0x500, 2));
    CHECK(table.Pending(0x500) == 5);
    // The game's own hit of 1 is pushed; the step takes 1 + 5.
    CHECK(CoopActorEvents::Step(ring, table, 0x500, 1) == 6);
    auto got = ring.Drain();
    REQUIRE(got.size() == 1);
    CHECK(got[0].kind == Kind::Damage);
    CHECK(CoopActorEvents::Amount(got[0].b) == 1);
    CHECK(CoopActorEvents::Step(ring, table, 0x500, 0) == 0);  // (taken once)
    CHECK(table.Take(0x600) == 0);
    CHECK_FALSE(table.Give(0, 1));
    table.Clear();
    CHECK(table.Pending(0x500) == 0);
}

TEST_CASE("actor events: a create keeps its name") {
    Event e;
    e.SetName("Shootable_DekuSeed");
    CHECK(e.Name() == "Shootable_DekuSeed");
    e.SetName(std::string(40, 'x'));
    CHECK(e.Name().size() == 31);
}
