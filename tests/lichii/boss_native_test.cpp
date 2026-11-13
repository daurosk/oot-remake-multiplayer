// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <map>

#include "coop_boss_native.h"

using namespace CoopBossNative;

namespace {

constexpr std::uint64_t kMain = 0x80020000;
constexpr std::uint64_t kActor = 0x1000000;
constexpr std::uint64_t kBsa = 0x2000000;
constexpr std::uint64_t kBlock = 0x3000000;
constexpr std::uint64_t kNode = 0x4000000;

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
    T Get(std::uint64_t address) {
        T v{};
        Read(address, &v, sizeof v);
        return v;
    }
    std::map<std::uint64_t, std::uint8_t> bytes;
};

// A request as the game's ExecuteBSASetActionVerb node makes it.
Request GameRequest(std::int32_t verb, std::uint64_t node, bool place = true) {
    Request r{};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float p0[3] = {place ? 10.0f : nan, 2.0f, -3.0f};
    std::memcpy(r.data() + addr::kReqPlaces, p0, 12);
    std::memcpy(r.data() + addr::kReqPlaces + 12, &nan, 4);
    std::memcpy(r.data() + addr::kReqPlaces + 24, &nan, 4);
    const std::uint32_t k1 = addr::kKind, k2 = addr::kKind2, k3 = addr::kKind3;
    std::memcpy(r.data() + addr::kReqKind, &k1, 4);
    std::memcpy(r.data() + addr::kReqKind2, &k2, 4);
    std::memcpy(r.data() + addr::kReqExtra, &nan, 4);
    std::memcpy(r.data() + addr::kReqExtra + 4, &nan, 4);
    std::memcpy(r.data() + addr::kReqKind3, &k3, 4);
    const std::int32_t slot = 0;
    const std::uint16_t flags = 0x100;
    std::memcpy(r.data() + addr::kReqSlot, &slot, 4);
    std::memcpy(r.data() + addr::kReqVerb, &verb, 4);
    std::memcpy(r.data() + addr::kReqFlags, &flags, 2);
    std::memcpy(r.data() + addr::kReqNode, &node, 8);
    return r;
}

Boss Gohma(Role role, std::uint8_t owner = 2) {
    return Boss{kActor, kBsa, role, "Enemy_DungeonBoss_Gohma", owner};
}

} // namespace

TEST_CASE("native boss: a request turns into the protocol's action and back") {
    const Request r = GameRequest(7, kNode);
    bool plug = true;
    const auto a = ActionFromRequest(r, &plug);
    REQUIRE(a);
    CHECK_FALSE(plug);
    CHECK(a->verb == 7);
    CHECK(a->flags == 0x100);
    REQUIRE(a->places[0]);
    CHECK((*a->places[0])[0] == 10.0f);
    CHECK_FALSE(a->places[1]);
    CHECK_FALSE(a->extra);
    // Back: the same bytes as the game's, with the node we name.
    const Request back = RequestFromAction(*a, kNode);
    CHECK(ActionFromRequest(back) == a);
    CHECK(std::memcmp(back.data() + addr::kReqKind, r.data() + addr::kReqKind, 0x58 - addr::kReqKind) == 0);
    std::uint64_t node = 0;
    std::memcpy(&node, back.data() + addr::kReqNode, 8);
    CHECK(node == kNode);
    // Not the game's request words: nothing.
    Request odd = r;
    odd[addr::kReqKind] = 5;
    CHECK_FALSE(ActionFromRequest(odd));
}

TEST_CASE("native boss: the owner keeps its changes and sends them") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::owner)});
    CHECK_FALSE(t.OwnerState(kActor));  // nothing before its first change
    CHECK_FALSE(t.OnBrainVerb(kBsa, 3, false, false));  // the owner's game always runs its own
    auto s = t.OwnerState(kActor);
    REQUIRE(s);
    CHECK(s->brain_verb == 3);
    CHECK(s->seq == 1);
    CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(9, kNode), true, false));
    s = t.OwnerState(kActor);
    REQUIRE(s);
    CHECK(s->seq == 2);
    REQUIRE(s->action);
    CHECK(s->action->verb == 9);
    // Not a boss: left alone.
    CHECK_FALSE(t.OnBrainVerb(kBsa + 8, 1, false, false));
    CHECK(t.Totals().owner_brain == 1);
    CHECK(t.Totals().owner_action == 1);
}

TEST_CASE("native boss: a copy runs as before until the owner's state comes, then takes only the owner's") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    CHECK_FALSE(t.OnBrainVerb(kBsa, 2, false, false));  // no owner's state yet
    t.Receive(kActor, BossBsa{5, 4, std::nullopt});
    CHECK(t.OnBrainVerb(kBsa, 2, false, false));   // its own: skipped
    CHECK_FALSE(t.OnBrainVerb(kBsa, 4, true, true));  // our call: runs
    CHECK(t.Totals().copy_skipped == 1);
    // The caller puts the owner's brain verb in through the game's function.
    FakeGuest g;
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSetBrainVerb);
    CHECK(c->x[0] == kBsa);
    CHECK(c->x[1] == 4);
    CHECK(c->x[2] == 1);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    CHECK(t.Totals().brain_applied == 1);
    // The same state again (each sample has it): nothing new.
    t.Receive(kActor, BossBsa{5, 4, std::nullopt});
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    // Already at that verb in the game: no call.
    const std::int32_t at = 6;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    t.Receive(kActor, BossBsa{6, 6, std::nullopt});
    CHECK_FALSE(t.Step(g, false, 0, kMain));
}

TEST_CASE("native boss: a copy's action waits for its own node, then is asked with the owner's values") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    BossAction a = *ActionFromRequest(GameRequest(11, 0xdead));
    t.Receive(kActor, BossBsa{1, 2, a});
    FakeGuest g;
    const std::int32_t verb = 2;
    g.Write(kBsa + addr::kBsaBrainVerb, &verb, 4);  // (brain verb already the owner's)
    CHECK_FALSE(t.Step(g, false, 0, kMain));  // no node of its own seen yet
    CHECK(t.Totals().action_waiting == 1);
    // The copy's own AI asks (skipped): its node is learnt.
    CHECK(t.OnActionVerb(kBsa, GameRequest(12, kNode), true, false));
    auto c = t.Step(g, false, 0, kMain);  // first a block for the request
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kMalloc);
    CHECK(c->x[0] == addr::kRequestSize);
    c = t.Step(g, true, kBlock, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kRequestActionVerb);
    CHECK(c->x[0] == kBsa);
    CHECK(c->x[1] == kBlock);
    CHECK(g.Get<std::int32_t>(kBlock + addr::kReqVerb) == 11);
    CHECK(g.Get<std::uint64_t>(kBlock + addr::kReqNode) == kNode);  // the copy's own node, not the owner's
    CHECK(std::isnan(g.Get<float>(kBlock + addr::kReqPlaces + 12)));
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    CHECK(t.Totals().action_applied == 1);
}

TEST_CASE("native boss: a hand-over keeps the state, and switches off leave the game alone") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    t.Receive(kActor, BossBsa{8, 4, std::nullopt});
    t.Publish({Gohma(Role::owner)});  // the boss is ours now
    CHECK_FALSE(t.OnBrainVerb(kBsa, 5, false, false));  // our game runs it
    auto s = t.OwnerState(kActor);
    REQUIRE(s);
    CHECK(s->seq == 9);  // counts on from the owner's: the others see it as new
    CHECK(s->brain_verb == 5);
    // Back to a copy: waits for the new owner's state again.
    t.Publish({Gohma(Role::copy)});
    CHECK_FALSE(t.OnBrainVerb(kBsa, 1, false, false));
    // A boss gone from the walk is forgotten.
    t.Publish({});
    CHECK_FALSE(t.OwnerState(kActor));
    // Off: nothing kept, nothing skipped.
    Table off;
    off.SetOn(false, true);
    off.Publish({Gohma(Role::owner)});
    off.OnBrainVerb(kBsa, 3, false, false);
    CHECK_FALSE(off.OwnerState(kActor));
    CHECK_FALSE(off.ActionsOn());
    // Brain verbs on, actions off: actions are neither kept nor skipped.
    Table brain;
    brain.SetOn(true, false);
    brain.Publish({Gohma(Role::copy)});
    brain.Receive(kActor, BossBsa{1, 2, std::nullopt});
    CHECK_FALSE(brain.OnActionVerb(kBsa, GameRequest(3, kNode), true, false));
    CHECK(brain.OnBrainVerb(kBsa, 3, false, false));
}

TEST_CASE("native boss: the request block is asked again for a new game, not for a new scene") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    t.OnActionVerb(kBsa, GameRequest(12, kNode), true, false);  // (its node; no owner's state yet: runs)
    t.Receive(kActor, BossBsa{1, 2, *ActionFromRequest(GameRequest(11, 0))});
    FakeGuest g;
    const std::int32_t verb = 2;
    g.Write(kBsa + addr::kBsaBrainVerb, &verb, 4);
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kMalloc);
    REQUIRE(t.Step(g, true, kBlock, kMain));  // the action, from the block
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    // A new owner's action in the same game: the same block.
    t.Receive(kActor, BossBsa{2, 2, *ActionFromRequest(GameRequest(13, 0))});
    c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kRequestActionVerb);
    CHECK(c->x[1] == kBlock);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    // Another game (main moved): a block of its own first.
    t.Receive(kActor, BossBsa{3, 2, *ActionFromRequest(GameRequest(14, 0))});
    c = t.Step(g, false, 0, kMain + 0x100000);
    REQUIRE(c);
    CHECK(c->function == kMain + 0x100000 + addr::kMalloc);
    CHECK(addr::kActionTaken == 0);  // (a skipped request answers as a taken one: hookshot's wrap 5)
}

TEST_CASE("native boss: a copy boss's own minion is not made, the owner's own is") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Boss{kActor, kBsa, Role::copy, "Enemy_DungeonBoss_Dodongo"}});
    CHECK_FALSE(t.CopyMakes("Enemy_DungeonBoss_Dodongo"));  // no owner's verbs yet: as before
    t.Receive(kActor, BossBsa{1, 2, std::nullopt});
    CHECK(t.CopyMakes("Enemy_DungeonBoss_Dodongo"));
    CHECK_FALSE(t.CopyMakes("Enemy_DungeonBoss_Gohma"));
    CHECK_FALSE(t.CopyMakes(""));  // (no minion)
    CHECK(t.Totals().minions_stopped == 1);
    t.Publish({Boss{kActor, kBsa, Role::owner, "Enemy_DungeonBoss_Dodongo"}});
    CHECK_FALSE(t.CopyMakes("Enemy_DungeonBoss_Dodongo"));
}

namespace {

BossBsa WithRecent(std::uint32_t from, std::uint32_t to) {
    // The owner's changes from..to, brain verbs 100 + seq (the state: the last).
    BossBsa s;
    s.seq = to;
    s.brain_verb = static_cast<std::int32_t>(100 + to);
    for (std::uint32_t q = from; q <= to; ++q) {
        s.recent.push_back(BossChange{q, BossBrain{static_cast<std::int32_t>(100 + q), false}});
    }
    return s;
}

std::vector<std::uint64_t> BrainCalls(Table& t, FakeGuest& g) {
    std::vector<std::uint64_t> verbs;
    for (auto c = t.Step(g, false, 0, kMain); c; c = t.Step(g, true, 0, kMain)) {
        REQUIRE(c->function == kMain + addr::kSetBrainVerb);
        verbs.push_back(c->x[1]);
    }
    return verbs;
}

} // namespace

TEST_CASE("native boss: the owner sends its newest changes in order, the action's changed bit with them") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::owner)});
    t.OnBrainVerb(kBsa, 1, true, false);
    t.OnActionVerb(kBsa, GameRequest(5, kNode), false, false);
    auto s = t.OwnerState(kActor);
    REQUIRE(s);
    REQUIRE(s->recent.size() == 2);
    CHECK(s->recent[0].seq == 1);
    CHECK(std::get<BossBrain>(s->recent[0].what) == BossBrain{1, true});
    CHECK(s->recent[1].seq == 2);
    CHECK(std::get<BossAction>(s->recent[1].what).verb == 5);
    CHECK_FALSE(std::get<BossAction>(s->recent[1].what).changed);
    CHECK_FALSE(s->action->changed);
    // Only the newest kMaxBossChanges are held.
    for (int i = 0; i < 20; ++i) {
        t.OnBrainVerb(kBsa, 10 + i, false, false);
    }
    s = t.OwnerState(kActor);
    REQUIRE(s);
    CHECK(s->seq == 22);
    REQUIRE(s->recent.size() == coop::protocol::kMaxBossChanges);
    CHECK(s->recent.front().seq == 22 - coop::protocol::kMaxBossChanges + 1);
    CHECK(s->recent.back().seq == 22);
    CHECK(s->brain_verb == 29);
}

TEST_CASE("native boss: a copy makes every owner's change once, in order, across lost samples") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    FakeGuest g;
    t.Receive(kActor, WithRecent(1, 1));
    CHECK(BrainCalls(t, g) == std::vector<std::uint64_t>{101});  // first: the state now
    t.Receive(kActor, WithRecent(1, 3));
    CHECK(BrainCalls(t, g) == std::vector<std::uint64_t>{102, 103});
    // Samples with 4 and 5 were lost; this one still holds them.
    t.Receive(kActor, WithRecent(2, 6));
    CHECK(BrainCalls(t, g) == std::vector<std::uint64_t>{104, 105, 106});
    // An older sample after a newer one: nothing again.
    t.Receive(kActor, WithRecent(1, 5));
    CHECK(BrainCalls(t, g).empty());
    CHECK(t.Totals().resyncs == 0);
    // More lost than a sample holds: from the state now, once.
    t.Receive(kActor, WithRecent(12, 19));
    CHECK(BrainCalls(t, g) == std::vector<std::uint64_t>{119});
    CHECK(t.Totals().resyncs == 1);
    // A new owner (player 3 after 2), its count smaller: from its state now.
    t.Publish({Gohma(Role::copy, 3)});
    t.Receive(kActor, WithRecent(1, 2));
    CHECK(BrainCalls(t, g) == std::vector<std::uint64_t>{102});
    CHECK(t.Totals().resyncs == 1);  // (a new owner's first state, not a resync)
}

TEST_CASE("native boss: an action waits for the copy's node and the changes after it wait with it") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    FakeGuest g;
    const std::int32_t verb = 2;
    g.Write(kBsa + addr::kBsaBrainVerb, &verb, 4);
    BossBsa s;
    s.seq = 3;
    s.brain_verb = 7;
    BossAction a = *ActionFromRequest(GameRequest(11, 0));
    a.changed = false;
    s.action = a;
    s.recent = {BossChange{1, BossBrain{2, false}}, BossChange{2, a}, BossChange{3, BossBrain{7, false}}};
    t.Receive(kActor, BossBsa{0, 2, std::nullopt, {}});  // (first: the state now, verb 2: already so)
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    t.Receive(kActor, s);
    // The owner's own call of verb 2 is made though the copy has it (it was a call there too).
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSetBrainVerb);
    CHECK(c->x[1] == 2);
    CHECK_FALSE(t.Step(g, true, 0, kMain));  // the action: no node of its own yet, and 7 waits behind it
    CHECK(t.OnActionVerb(kBsa, GameRequest(12, kNode), true, false));
    c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kMalloc);
    c = t.Step(g, true, kBlock, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kRequestActionVerb);
    CHECK(c->x[2] == 0);  // the owner's "changed" bit
    CHECK(g.Get<std::int32_t>(kBlock + addr::kReqVerb) == 11);
    c = t.Step(g, true, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSetBrainVerb);
    CHECK(c->x[1] == 7);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
}

TEST_CASE("native boss: a copy's phase that differs for long is put back to the owner's") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    FakeGuest g;
    const std::int32_t owner = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &owner, 4);
    t.Receive(kActor, BossBsa{1, 4, std::nullopt, {}});
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    const std::int32_t other = 9;  // the game changed it where no wrap sees it
    g.Write(kBsa + addr::kBsaBrainVerb, &other, 4);
    for (int i = 1; i < Table::kDriftSteps; ++i) {
        CHECK_FALSE(t.Step(g, false, 0, kMain));
    }
    CHECK_FALSE(t.Step(g, false, 0, kMain));  // the step that queues it
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->x[1] == 4);
    CHECK(t.Totals().drift_fixes == 1);
    // Back to the owner's: no more.
    g.Write(kBsa + addr::kBsaBrainVerb, &owner, 4);
    for (int i = 0; i < 2 * Table::kDriftSteps; ++i) {
        CHECK_FALSE(t.Step(g, true, 0, kMain));
    }
    CHECK(t.Totals().drift_fixes == 1);
}
