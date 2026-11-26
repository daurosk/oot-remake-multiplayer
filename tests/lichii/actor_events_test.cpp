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

TEST_CASE("actor events: a judged give is told to its step once, only an owner's death passes the life stops") {
    static Ring ring;
    static CoopActorEvents::LifeTable table;
    bool judged = true;
    CHECK(CoopActorEvents::Step(ring, table, 0x700, 0, &judged) == 0);
    CHECK_FALSE(judged);
    // Another player's hit on our own enemy: ours to judge.
    CHECK(table.Give(0x700, 4));
    CHECK(CoopActorEvents::Step(ring, table, 0x700, 0, &judged) == 4);
    CHECK_FALSE(judged);
    // The owner's life for its copy (judged there), with a hit of ours in the same frame: told, then cleared.
    CHECK(table.Give(0x700, 3, true));
    CHECK(table.Give(0x700, 2));
    CHECK(CoopActorEvents::Step(ring, table, 0x700, 1, &judged) == 6);
    CHECK(judged);
    CHECK(CoopActorEvents::Step(ring, table, 0x700, 1, &judged) == 1);
    CHECK_FALSE(judged);
    ring.Drain();
    // Gohma's copy at 21 with her own stop at 1: the owner's death (21) goes past it; anything less, or ours, not.
    using CoopActorEvents::PassStops;
    CHECK(PassStops(21, 21, true, false));
    CHECK(PassStops(21, 21, false, true));   // a copy boss whose death has begun (or whose owner's died)
    CHECK(PassStops(30, 21, true, false));
    CHECK_FALSE(PassStops(20, 21, true, true));  // not a death: her stops stay hers
    CHECK_FALSE(PassStops(21, 21, false, false)); // our own game's hit: judged here, stops and all
    CHECK_FALSE(PassStops(1, 0, true, true));     // (already dead)
    table.Give(0x700, 5, true);
    table.Clear();
    CHECK(CoopActorEvents::Step(ring, table, 0x700, 0, &judged) == 0);
    CHECK_FALSE(judged);
}

TEST_CASE("actor events: a fall given in place of a replay its damage step never took is taken once, never twice") {
    static Ring ring;
    static CoopActorEvents::LifeTable table;
    using CoopActorEvents::kNoHitId;
    using CoopActorEvents::HitIdReached;
    using CoopActorEvents::WithdrawReplay;
    // Hit ids only grow; none read or none of ours: not reached.
    CHECK(HitIdReached(0x1000, 0x1000));
    CHECK(HitIdReached(0x1002, 0x1000));
    CHECK_FALSE(HitIdReached(0x0ffe, 0x1000));
    CHECK_FALSE(HitIdReached(kNoHitId, 0x1000));
    CHECK_FALSE(HitIdReached(0x1000, kNoHitId));
    // enemy-late-kill: the Deku Baba's copy. Our replay 0x1000 waits; its hit id stays 0xffffffff. The owner's
    // death (3) is given tied to the replay and the next Life step takes it, judged, and names the replay.
    bool judged = false;
    std::uint32_t withdraw = 0;
    CHECK(table.Give(0x900, 3, true, 0x1000));
    CHECK(table.Pending(0x900) == 3);
    CHECK(CoopActorEvents::Step(ring, table, 0x900, 0, &judged, kNoHitId, &withdraw) == 3);
    CHECK(judged);
    CHECK(withdraw == 0x1000);
    CHECK(CoopActorEvents::Step(ring, table, 0x900, 0, &judged, kNoHitId, &withdraw) == 0);  // (once)
    CHECK(withdraw == kNoHitId);
    // The replay still waiting in the record (its id, present set) is withdrawn; another, or one taken, is not.
    CHECK(WithdrawReplay(0x1000, 0x1000, 1));
    CHECK_FALSE(WithdrawReplay(0x1000, 0x1000, 0));
    CHECK_FALSE(WithdrawReplay(0x1000, 0x1002, 1));
    CHECK_FALSE(WithdrawReplay(kNoHitId, kNoHitId, 1));
    // The replay landed after all (the damage step took it this frame: the id reached it, its hit dealt the fall):
    // the tied give is dropped, a plain give with it is still taken.
    CHECK(table.Give(0x900, 2, true, 0x1002));
    CHECK(table.Give(0x900, 1));
    CHECK(CoopActorEvents::Step(ring, table, 0x900, 2, &judged, 0x1002, &withdraw) == 3);
    CHECK_FALSE(judged);  // (the judged part was dropped)
    CHECK(withdraw == kNoHitId);
    CHECK(table.Pending(0x900) == 0);
    ring.Drain();
    // Taken back on our own thread (stale): all of it, tied or not, so the fall can be given again.
    CHECK(table.Give(0x900, 2, true, 0x1004));
    CHECK(table.Give(0x900, 1));
    CHECK(table.Take(0x900) == 3);
    CHECK(table.Pending(0x900) == 0);
    table.Give(0x900, 2, true, 0x1004);
    table.Clear();
    CHECK(CoopActorEvents::Step(ring, table, 0x900, 0, &judged, kNoHitId, &withdraw) == 0);
    CHECK(withdraw == kNoHitId);
}

TEST_CASE("actor events: a create keeps its name") {
    Event e;
    e.SetName("Shootable_DekuSeed");
    CHECK(e.Name() == "Shootable_DekuSeed");
    e.SetName(std::string(40, 'x'));
    CHECK(e.Name().size() == 31);
}

TEST_CASE("actor events: a create's flags (our own call, refused) come through the ring") {
    CoopActorEvents::Ring ring;
    Event e;
    e.kind = CoopActorEvents::Kind::Create;
    e.flags = CoopActorEvents::kOwn;
    ring.Push(e);
    e.flags = CoopActorEvents::kStopped;
    e.SetName("Octorok_Bullet");
    ring.Push(e);
    const auto out = ring.Drain();
    REQUIRE(out.size() == 2);
    CHECK(out[0].flags == CoopActorEvents::kOwn);
    CHECK(out[1].flags == CoopActorEvents::kStopped);
    CHECK(out[1].Name() == "Octorok_Bullet");
}

TEST_CASE("actor events: a create inside a call that runs a copy's own game code is the game's, not ours (#189)") {
    CHECK_FALSE(CoopActorEvents::Ours(false, false));  // the game's own
    CHECK(CoopActorEvents::Ours(true, false));         // a copy or a remove we asked
    // The owner's shoot action put into a copy: the copy's own shot is judged (stopped), the owner's comes.
    CHECK_FALSE(CoopActorEvents::Ours(true, true));
}

TEST_CASE("boss-die-0116: a death kept past its revive stays kept on the steps after, until it lives again") {
    using CoopActorEvents::KeptDeaths;
    using CoopActorEvents::SkipRevive;
    KeptDeaths kept;
    constexpr std::uint64_t kGohma = 0x1052a20000;
    constexpr std::uint64_t kOther = 0x1052b30000;
    // A death our game judged (a real hit outside chance time): its revive stays, every frame.
    CHECK_FALSE(SkipRevive(kept, kGohma, false));
    CHECK_FALSE(SkipRevive(kept, kGohma, false));
    // The test kill (or an owner's death on its copy): the step whose stops passed skips the revive ...
    CHECK(SkipRevive(kept, kGohma, true));
    // ... and so does every later step that ends at 0 (the check is asked on each; before, the next frame gave 1).
    for (int frame = 0; frame < 600; ++frame) {
        CHECK(SkipRevive(kept, kGohma, false));
    }
    // Only that component.
    CHECK_FALSE(SkipRevive(kept, kOther, false));
    CHECK_FALSE(SkipRevive(kept, 0, true));
    // Alive again (a heal; a new actor's Life component at the same address starts above 0): its revive is its own.
    kept.Alive(kGohma);
    CHECK_FALSE(kept.Kept(kGohma));
    CHECK_FALSE(SkipRevive(kept, kGohma, false));
    // Review C1: more deaths than slots (dead copies are deleted, never read alive): the newest are kept, the 17th
    // death still skips its revive, the oldest gave up its slot.
    for (std::uint64_t i = 1; i <= KeptDeaths::kSlots + 1; ++i) {
        CHECK(SkipRevive(kept, i * 0x1000, true));
    }
    CHECK(SkipRevive(kept, (KeptDeaths::kSlots + 1) * 0x1000, false));
    CHECK(SkipRevive(kept, 2 * 0x1000, false));
    CHECK_FALSE(kept.Kept(0x1000));
    // Many more (a long session): always the newest kSlots.
    for (std::uint64_t i = 100; i < 100 + 5 * KeptDeaths::kSlots; ++i) {
        kept.Keep(i * 0x1000);
    }
    for (std::uint64_t i = 100 + 4 * KeptDeaths::kSlots; i < 100 + 5 * KeptDeaths::kSlots; ++i) {
        CHECK(kept.Kept(i * 0x1000));
    }
    CHECK_FALSE(kept.Kept((100 + 4 * KeptDeaths::kSlots - 1) * 0x1000));
    // A scene load: none kept.
    kept.Clear();
    CHECK_FALSE(kept.Kept((100 + 5 * KeptDeaths::kSlots - 1) * 0x1000));
    CHECK(SkipRevive(kept, 0x7000, true));  // and it keeps again after
    CHECK(kept.Kept(0x7000));
    // Kept twice: one slot (freed once by Alive).
    KeptDeaths twice;
    twice.Keep(kGohma);
    twice.Keep(kGohma);
    twice.Alive(kGohma);
    CHECK_FALSE(twice.Kept(kGohma));
}

TEST_CASE("actor events: a Life component the walk no longer sees gives its slot back (row 270)") {
    using CoopActorEvents::LifeTable;
    static LifeTable table;
    table.Clear();
    // One scene, never left: wave after wave of enemies, each hit by another player. Far more components than
    // slots pass through; every give finds room, and the step takes it.
    const std::uint64_t kAlive = 30;
    for (std::uint64_t wave = 0; wave < 100; ++wave) {  // 3000 components through 256 slots
        const std::uint64_t base = 0x7100'0000 + wave * 0x10000;
        for (std::uint64_t i = 0; i < kAlive; ++i) {
            REQUIRE(table.Give(base + i * 0x200, 2));
        }
        CHECK(table.Take(base) == 2);
        const auto alive = [base](std::uint64_t c) { return c >= base && c < base + 0x10000; };
        for (std::uint64_t walk = 0; walk < LifeTable::kKeepSweeps + 1; ++walk) {
            table.Sweep(alive);
        }
        CHECK(table.Pending(base + 0x200) == 2);  // alive: kept, with what it was given
        if (wave > 0) {
            CHECK(table.Pending(base - 0x10000 + 0x200) == 0);  // the wave before: gone, with what it held
        }
    }
    table.Clear();
}

TEST_CASE("actor events: a give waits through walks that miss its component, and a freed slot holds nothing") {
    using CoopActorEvents::LifeTable;
    static LifeTable table;
    table.Clear();
    const auto nothing = [](std::uint64_t) { return false; };
    CHECK(table.Give(0x500, 3, true));
    for (std::uint64_t walk = 0; walk + 1 < LifeTable::kKeepSweeps; ++walk) {
        table.Sweep(nothing);  // (a load, a missed read)
    }
    CHECK(table.Pending(0x500) == 3);
    // Seen again (or a kept give still waiting on it: the caller's `alive`): the count starts over.
    table.Sweep([](std::uint64_t c) { return c == 0x500; });
    for (std::uint64_t walk = 0; walk + 1 < LifeTable::kKeepSweeps; ++walk) {
        table.Sweep(nothing);
    }
    CHECK(table.Pending(0x500) == 3);
    table.Sweep(nothing);
    CHECK(table.Pending(0x500) == 0);  // gone for good
    bool judged = true;
    CHECK(table.Take(0x500, &judged) == 0);
    CHECK_FALSE(judged);
    // The same address again (another actor's component): a slot with nothing of the old one's.
    CHECK(table.Give(0x500, 1));
    CHECK(table.Take(0x500, &judged) == 1);
    CHECK_FALSE(judged);
    // A give counts as seen: a new component is never freed before its first walks.
    CHECK(table.Give(0x900, 4));
    table.Sweep(nothing);
    CHECK(table.Pending(0x900) == 4);
    table.Clear();
}
