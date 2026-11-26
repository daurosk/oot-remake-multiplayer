// SPDX-License-Identifier: GPL-3.0-or-later
// Row 222: one battle manager decides for every game (coop_boss_orders.h).
#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <optional>
#include <vector>

#include "coop_boss_orders.h"

using namespace CoopBossOrders;

namespace {

constexpr std::uint64_t kMain = 0x80020000;
constexpr std::uint64_t kKotake = 0x1000000;
constexpr std::uint64_t kKoume = 0x1100000;
constexpr std::uint64_t kNodeKotake = 0x4000000;
constexpr std::uint64_t kNodeKoume = 0x4100000;
constexpr std::uint64_t kBlock = 0x3000000;

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
    void Put(std::uint64_t address, T value) {
        Write(address, &value, sizeof value);
    }
    template <typename T>
    T Get(std::uint64_t address) {
        T v{};
        Read(address, &v, sizeof v);
        return v;
    }
    // A receive node of `actor`, as the game links it: [[[node + 8] + 8] + 0x1f0].
    void Node(std::uint64_t node, std::uint64_t actor) {
        Put<std::uint64_t>(node + addr::kNodeContext, node + 0x100);
        Put<std::uint64_t>(node + 0x100 + addr::kContextOwner, node + 0x200);
        Put<std::uint64_t>(node + 0x200 + addr::kOwnerActor, actor);
    }
    std::map<std::uint64_t, std::uint8_t> bytes;
};

Payload PayloadOf(std::uint8_t state) {
    Payload p{};
    p[0] = state;
    p[0x14] = 1;  // IsMainMove
    return p;
}

BossOrder Order(std::uint32_t seq, std::uint32_t type, std::uint8_t state) {
    return BossOrder{seq, type, PayloadOf(state)};
}

// Runs one frame of the caller as the runtime does; returns the calls made (each answered with `result`).
std::vector<CoopPuppet::Call> Frame(Table& t, FakeGuest& g, std::uint64_t malloc_result = kBlock) {
    std::vector<CoopPuppet::Call> made;
    auto call = t.Step(g, false, 0, kMain);
    while (call) {
        made.push_back(*call);
        const std::uint64_t result = call->function == kMain + addr::kMalloc ? malloc_result : 1;
        call = t.Step(g, true, result, kMain);
    }
    return made;
}

std::vector<Member> Sisters(Role kotake, Role koume, std::uint8_t owner) {
    return {Member{kKotake, kotake, owner, "Enemy_Kotake"}, Member{kKoume, koume, owner, "Enemy_Koume"}};
}

}  // namespace

TEST_CASE("only the manager's two orders are taken") {
    CHECK(IsOrder(0x70210000u));
    CHECK(IsOrder(0x70210001u));
    CHECK_FALSE(IsOrder(0x70210002u));
    CHECK_FALSE(IsOrder(0x3ddc000eu));  // the sisters' own EnemyCoopWriteBlackboard messages
}

TEST_CASE("the owner's game runs its manager's orders and keeps them for the part's samples") {
    Table t;
    t.SetOn(true);
    const auto members = Sisters(Role::owner, Role::owner, 1);
    t.Publish(members);
    CHECK_FALSE(t.OwnerOrders(kKotake));
    CHECK_FALSE(t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(3), false));
    CHECK_FALSE(t.OnReceive(kNodeKotake, kKotake, addr::kTypeInterval, PayloadOf(0), false));
    CHECK_FALSE(t.OnReceive(kNodeKotake, kKotake, 0x3ddc000eu, PayloadOf(9), false));  // not an order: not kept
    const auto mine = t.OwnerOrders(kKotake);
    REQUIRE(mine);
    CHECK(mine->seq == 2);
    REQUIRE(mine->recent.size() == 2);
    CHECK(mine->recent[0] == Order(1, addr::kTypeState, 3));
    CHECK(mine->recent[1].type == addr::kTypeInterval);
    CHECK_FALSE(t.OwnerOrders(kKoume));  // none given yet
    // The newest kMaxBossOrders only.
    for (int i = 0; i < 10; ++i) {
        (void)t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(static_cast<std::uint8_t>(i)), false);
    }
    CHECK(t.OwnerOrders(kKotake)->seq == 12);
    CHECK(t.OwnerOrders(kKotake)->recent.size() == coop::protocol::kMaxBossOrders);
    CHECK(t.OwnerOrders(kKotake)->recent.front().seq == 12 - coop::protocol::kMaxBossOrders + 1);
    CHECK(t.GetCounts().owner_kept == 12);
}

TEST_CASE("off, solo or an unknown actor: every order runs, nothing kept") {
    Table t;
    CHECK_FALSE(t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(1), false));  // off
    t.SetOn(true);
    CHECK_FALSE(t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(1), false));  // not listed
    CHECK(t.GetCounts().unknown == 1);
    CHECK_FALSE(t.OwnerOrders(kKotake));
}

TEST_CASE("a copy runs its own orders until the owner's come, then skips them and gets the owner's in order") {
    Table t;
    FakeGuest g;
    g.Node(kNodeKotake, kKotake);
    g.Put<std::uint32_t>(kNodeKotake + addr::kNodeLastSender, 0x1234);
    t.SetOn(true);
    const auto members = Sisters(Role::copy, Role::copy, 2);
    t.Publish(members);
    // Before the owner's: its own manager's order runs (and names its node).
    CHECK_FALSE(t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(1), false));
    CHECK(t.GetCounts().copy_own_ran == 1);
    CHECK(Frame(t, g).empty());
    // The owner's come: delivered through the game's handler, the oldest first, each once.
    t.Receive(kKotake, BossOrders{2, {Order(1, addr::kTypeState, 5), Order(2, addr::kTypeInterval, 0)}});
    CHECK(t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(1), false));  // its own: skipped now
    CHECK(t.GetCounts().copy_skipped == 1);
    auto calls = Frame(t, g);
    REQUIRE(calls.size() == 3);  // the block's malloc, then the two orders
    CHECK(calls[0].function == kMain + addr::kMalloc);
    CHECK(calls[1].function == kMain + addr::kReceive);
    CHECK(calls[1].x[0] == kNodeKotake);
    CHECK(calls[1].x[1] == kBlock);
    CHECK(calls[1].copy_game);
    // The message as the game's handler reads it (the last one written: the interval order).
    CHECK(g.Get<std::uint32_t>(kBlock + addr::kMessageSender) == 0x1234);
    CHECK(g.Get<std::uint32_t>(kBlock + addr::kMessageType) == addr::kTypeInterval);
    CHECK(g.Get<std::uint64_t>(kBlock + addr::kMessagePayload) == kBlock + addr::kBlockPayload);
    CHECK(g.Get<std::uint8_t>(kBlock + addr::kBlockPayload + 0x14) == 1);
    CHECK(t.GetCounts().delivered == 2);
    // The same samples again: nothing more. A newer one: only it.
    t.Receive(kKotake, BossOrders{2, {Order(1, addr::kTypeState, 5), Order(2, addr::kTypeInterval, 0)}});
    CHECK(Frame(t, g).empty());
    t.Receive(kKotake, BossOrders{3, {Order(2, addr::kTypeInterval, 0), Order(3, addr::kTypeState, 7)}});
    calls = Frame(t, g);
    REQUIRE(calls.size() == 1);
    CHECK(g.Get<std::uint8_t>(kBlock + addr::kBlockPayload) == 7);
    CHECK(g.Get<std::uint32_t>(kBlock + addr::kMessageType) == addr::kTypeState);
    // Our own call goes through the wrap and runs.
    CHECK_FALSE(t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(7), true));
}

TEST_CASE("a copy whose node is not seen yet waits, one whose node is gone drops its queue") {
    Table t;
    FakeGuest g;
    t.SetOn(true);
    const auto members = Sisters(Role::copy, Role::copy, 2);
    t.Publish(members);
    t.Receive(kKoume, BossOrders{1, {Order(1, addr::kTypeState, 4)}});
    CHECK(Frame(t, g).empty());  // no node yet
    g.Node(kNodeKoume, kKoume);
    CHECK(t.OnReceive(kNodeKoume, kKoume, addr::kTypeState, PayloadOf(1), false));  // names it, skipped
    CHECK(Frame(t, g).size() == 2);  // malloc + the order
    t.Receive(kKoume, BossOrders{2, {Order(1, addr::kTypeState, 4), Order(2, addr::kTypeState, 6)}});
    g.Put<std::uint64_t>(kNodeKoume + 0x200 + addr::kOwnerActor, 0x9999);  // the node now belongs to another actor
    CHECK(Frame(t, g).empty());
    CHECK(t.GetCounts().dropped == 1);
}

TEST_CASE("Plan: everything newer when the list reaches back, else the newest of each type") {
    const BossOrders owner{9, {Order(5, addr::kTypeState, 1), Order(6, addr::kTypeInterval, 0),
                               Order(7, addr::kTypeState, 2), Order(8, addr::kTypeState, 3),
                               Order(9, addr::kTypeInterval, 0)}};
    auto p = Plan(owner, 6);
    REQUIRE(p.size() == 3);
    CHECK(p[0].seq == 7);
    CHECK(p[2].seq == 9);
    p = Plan(owner, 4);  // (reaches back exactly: 5 follows 4)
    CHECK(p.size() == 5);
    p = Plan(owner, 0);  // a start: the newest state (8) and the newest interval (9), in order
    REQUIRE(p.size() == 2);
    CHECK(p[0].seq == 8);
    CHECK(p[1].seq == 9);
    p = Plan(owner, 2);  // a gap (3 and 4 are gone): the same
    REQUIRE(p.size() == 2);
    CHECK(p[0].seq == 8);
    CHECK(Plan(owner, 9).empty());
    CHECK(Plan(BossOrders{}, 0).empty());
}

TEST_CASE("hand-over: made ours, its own manager decides, made a copy, it starts from the new owner's newest") {
    Table t;
    FakeGuest g;
    g.Node(kNodeKotake, kKotake);
    t.SetOn(true);
    auto members = Sisters(Role::copy, Role::copy, 2);
    t.Publish(members);
    CHECK_FALSE(t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(1), false));
    t.Receive(kKotake, BossOrders{1, {Order(1, addr::kTypeState, 5)}});
    CHECK(Frame(t, g).size() == 2);
    // Ours now: our own manager's orders run and are kept for our samples.
    members = Sisters(Role::owner, Role::owner, 1);
    t.Publish(members);
    CHECK_FALSE(t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(2), false));
    REQUIRE(t.OwnerOrders(kKotake));
    CHECK(t.OwnerOrders(kKotake)->seq == 1);
    t.Receive(kKotake, BossOrders{4, {Order(4, addr::kTypeState, 9)}});  // (an old owner's late sample: ignored)
    CHECK(Frame(t, g).empty());
    // Player 3's now: our own run until its orders come, then its newest of each type.
    members = Sisters(Role::copy, Role::copy, 3);
    t.Publish(members);
    CHECK_FALSE(t.OwnerOrders(kKotake));
    CHECK_FALSE(t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(3), false));
    t.Receive(kKotake, BossOrders{30, {Order(28, addr::kTypeInterval, 0), Order(29, addr::kTypeState, 1),
                                       Order(30, addr::kTypeState, 8)}});
    const auto calls = Frame(t, g);
    REQUIRE(calls.size() == 2);  // the interval (28), then the newest state (30)
    CHECK(g.Get<std::uint8_t>(kBlock + addr::kBlockPayload) == 8);
    CHECK(t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(3), false));
    // The owner's count going back (another game's, before the walk saw the hand-over): it starts again.
    t.Receive(kKotake, BossOrders{2, {Order(1, addr::kTypeInterval, 0), Order(2, addr::kTypeState, 6)}});
    CHECK(t.GetCounts().restarts == 1);
    CHECK(Frame(t, g).size() == 2);
    CHECK(g.Get<std::uint8_t>(kBlock + addr::kBlockPayload) == 6);
}

TEST_CASE("a malloc that gave nothing is asked again next frame, a frame whose call was not made keeps the order") {
    Table t;
    FakeGuest g;
    g.Node(kNodeKotake, kKotake);
    t.SetOn(true);
    const auto members = Sisters(Role::copy, Role::copy, 2);
    t.Publish(members);
    (void)t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(1), false);
    t.Receive(kKotake, BossOrders{1, {Order(1, addr::kTypeState, 5)}});
    auto calls = Frame(t, g, 0);  // the malloc failed
    CHECK(calls.size() == 1);
    // The deliver asked but not made (the frame's calls ran out): asked again next frame.
    calls = Frame(t, g);
    REQUIRE(calls.size() == 2);
    auto again = t.Step(g, false, 0, kMain);
    CHECK_FALSE(again);  // (delivered already)
    t.Receive(kKotake, BossOrders{2, {Order(1, addr::kTypeState, 5), Order(2, addr::kTypeState, 6)}});
    auto call = t.Step(g, false, 0, kMain);
    REQUIRE(call);
    // Not made: the next frame asks it again.
    call = t.Step(g, false, 0, kMain);
    REQUIRE(call);
    CHECK(call->function == kMain + addr::kReceive);
    CHECK_FALSE(t.Step(g, true, 1, kMain));
    CHECK(t.GetCounts().delivered == 2);
}

TEST_CASE("made ours: our manager's skipped orders go into its sister first, and are sent as ours") {
    Table t;
    FakeGuest g;
    g.Node(kNodeKotake, kKotake);
    t.SetOn(true);
    auto members = Sisters(Role::copy, Role::copy, 2);
    t.Publish(members);
    (void)t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(1), false);  // its own, before the owner's
    t.Receive(kKotake, BossOrders{1, {Order(1, addr::kTypeState, 5)}});
    CHECK(Frame(t, g).size() == 2);
    // Our manager goes on in our game; its orders are skipped (the newest of each type kept).
    CHECK(t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(2), false));
    CHECK(t.OnReceive(kNodeKotake, kKotake, addr::kTypeInterval, PayloadOf(0), false));
    CHECK(t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(3), false));
    members = Sisters(Role::owner, Role::owner, 1);
    t.Publish(members);
    const auto calls = Frame(t, g);
    REQUIRE(calls.size() == 2);  // the interval, then the newest state (the order our manager gave them)
    CHECK(calls[0].x[0] == kNodeKotake);
    CHECK(g.Get<std::uint32_t>(kBlock + addr::kMessageType) == addr::kTypeState);
    CHECK(g.Get<std::uint8_t>(kBlock + addr::kBlockPayload) == 3);
    CHECK(t.GetCounts().replayed == 2);
    const auto mine = t.OwnerOrders(kKotake);
    REQUIRE(mine);
    CHECK(mine->seq == 2);
    CHECK(mine->recent[0].type == addr::kTypeInterval);
    CHECK(mine->recent[1].payload[0] == 3);
    CHECK(Frame(t, g).empty());
    // A copy made a copy of another owner keeps nothing of it.
    members = Sisters(Role::copy, Role::copy, 3);
    t.Publish(members);
    CHECK(Frame(t, g).empty());
}

TEST_CASE("a new game drops the block, the parts and a call in flight, and Reset does the same") {
    constexpr std::uint64_t kMain2 = 0x90020000;
    Table t;
    FakeGuest g;
    g.Node(kNodeKotake, kKotake);
    t.SetOn(true);
    t.Publish(Sisters(Role::copy, Role::copy, 2));
    (void)t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(1), false);
    t.Receive(kKotake, BossOrders{1, {Order(1, addr::kTypeState, 5)}});
    REQUIRE(Frame(t, g).size() == 2);  // the block's malloc, then the order
    // A newer order is being put in when the game changes: its result is the old game's, not counted.
    t.Receive(kKotake, BossOrders{2, {Order(1, addr::kTypeState, 5), Order(2, addr::kTypeState, 6)}});
    auto call = t.Step(g, false, 0, kMain);
    REQUIRE(call);
    CHECK(call->function == kMain + addr::kReceive);
    CHECK_FALSE(t.Step(g, true, 1, kMain2));
    CHECK(t.GetCounts().delivered == 1);
    // The new game's sisters: the old block is never written again, a new one is asked first.
    g.bytes.clear();
    g.Node(kNodeKotake, kKotake);
    t.Publish(Sisters(Role::copy, Role::copy, 2));
    (void)t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(1), false);
    t.Receive(kKotake, BossOrders{1, {Order(1, addr::kTypeState, 9)}});
    call = t.Step(g, false, 0, kMain2);
    REQUIRE(call);
    CHECK(call->function == kMain2 + addr::kMalloc);
    constexpr std::uint64_t kBlock2 = 0x3100000;
    call = t.Step(g, true, kBlock2, kMain2);
    REQUIRE(call);
    CHECK(call->function == kMain2 + addr::kReceive);
    CHECK(call->x[1] == kBlock2);
    CHECK(g.Get<std::uint32_t>(kBlock + addr::kMessageType) == 0);  // (nothing written to the old block)
    // Reset (a new game process): the same, and the counts start again.
    t.Reset();
    CHECK(t.GetCounts().delivered == 0);
    t.Publish(Sisters(Role::copy, Role::copy, 2));
    (void)t.OnReceive(kNodeKotake, kKotake, addr::kTypeState, PayloadOf(1), false);
    t.Receive(kKotake, BossOrders{1, {Order(1, addr::kTypeState, 9)}});
    call = t.Step(g, false, 0, kMain2);
    REQUIRE(call);
    CHECK(call->function == kMain2 + addr::kMalloc);
}
