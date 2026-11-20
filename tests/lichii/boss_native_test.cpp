// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>

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
    t.Receive(kActor, BossBsa{5, 4, std::nullopt, {}});
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
    t.Receive(kActor, BossBsa{5, 4, std::nullopt, {}});
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    // Already at that verb in the game: no call.
    const std::int32_t at = 6;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    t.Receive(kActor, BossBsa{6, 6, std::nullopt, {}});
    CHECK_FALSE(t.Step(g, false, 0, kMain));
}

TEST_CASE("native boss: a copy's action waits for its own node, then is asked with the owner's values") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    BossAction a = *ActionFromRequest(GameRequest(11, 0xdead));
    t.Receive(kActor, BossBsa{1, 2, a, {}});
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
    t.Receive(kActor, BossBsa{8, 4, std::nullopt, {}});
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
    brain.Receive(kActor, BossBsa{1, 2, std::nullopt, {}});
    CHECK_FALSE(brain.OnActionVerb(kBsa, GameRequest(3, kNode), true, false));
    CHECK(brain.OnBrainVerb(kBsa, 3, false, false));
}

TEST_CASE("native boss: the request block is asked again for a new game, not for a new scene") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    t.OnActionVerb(kBsa, GameRequest(12, kNode), true, false);  // (its node; no owner's state yet: runs)
    t.Receive(kActor, BossBsa{1, 2, *ActionFromRequest(GameRequest(11, 0)), {}});
    FakeGuest g;
    const std::int32_t verb = 2;
    g.Write(kBsa + addr::kBsaBrainVerb, &verb, 4);
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kMalloc);
    REQUIRE(t.Step(g, true, kBlock, kMain));  // the action, from the block
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    // A new owner's action in the same game: the same block.
    t.Receive(kActor, BossBsa{2, 2, *ActionFromRequest(GameRequest(13, 0)), {}});
    c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kRequestActionVerb);
    CHECK(c->x[1] == kBlock);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    // Another game (main moved): a block of its own first.
    t.Receive(kActor, BossBsa{3, 2, *ActionFromRequest(GameRequest(14, 0)), {}});
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
    t.Receive(kActor, BossBsa{1, 2, std::nullopt, {}});
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
    // A verb the game keeps refusing: put back kMaxDriftFixes times, then left alone.
    g.Write(kBsa + addr::kBsaBrainVerb, &other, 4);
    for (int i = 0; i < 20 * Table::kDriftSteps; ++i) {
        t.Step(g, true, 0, kMain);
    }
    CHECK(t.Totals().drift_fixes == Table::kMaxDriftFixes);
    // The owner's next state: put back again.
    t.Receive(kActor, BossBsa{2, 4, std::nullopt, {BossChange{2, BossBrain{4, true}}}});
    for (int i = 0; i < 3 * Table::kDriftSteps; ++i) {
        t.Step(g, true, 0, kMain);
    }
    CHECK(t.Totals().drift_fixes > Table::kMaxDriftFixes);
}

TEST_CASE("native boss: a new owner sends the phase its own game's boss is in, not one it had not made") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    FakeGuest g;
    const std::int32_t here = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &here, 4);
    t.Receive(kActor, BossBsa{1, 4, std::nullopt, {}});
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    // The old owner's change to 6 comes, and the boss is ours before it is made here.
    t.Receive(kActor, BossBsa{2, 6, std::nullopt, {BossChange{2, BossBrain{6, false}}}});
    t.Publish({Gohma(Role::owner)});
    // ours: nothing of the old owner's made; its AI chooses again from the verb our game's boss is in (row 149)
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSetBrainVerb);
    CHECK(c->x[1] == 4);
    CHECK_FALSE(t.OnBrainVerb(kBsa, here, true, true));
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    t.OnActionVerb(kBsa, GameRequest(11, kNode), true, false);  // a change of ours: the state goes out
    const auto s = t.OwnerState(kActor);
    REQUIRE(s);
    CHECK(s->brain_verb == 4);
    CHECK(s->seq == 4);  // (3: its re-chosen brain verb, sent to every copy)
}

// boss-fix (player reports #30, #31): the copy's own game runs its events and its own hit reactions.

TEST_CASE("native boss: during our event a copy runs its own changes, then takes the owner's state again") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    FakeGuest g;
    const std::int32_t at = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));  // (already the owner's verb)
    // The intro starts: the event's own brain verb and move run in our game.
    t.SetEvent(true);
    CHECK_FALSE(t.OnBrainVerb(kBsa, 9, true, false));
    CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(21, kNode), true, false));
    CHECK(t.Totals().copy_own_run == 2);
    CHECK(t.Totals().copy_skipped == 0);
    // The owner's changes wait: none put in, no put-back, while the event runs.
    t.Receive(kActor, BossBsa{4, 5, std::nullopt, {BossChange{4, BossBrain{5, false}}}});
    for (int i = 0; i < Table::kDriftSteps + 5; ++i) {
        CHECK_FALSE(t.Step(g, false, 0, kMain));
    }
    // Over: its own still runs for a moment (boss-copy-b: a hand-over to us
    // may be on its way), then from the owner's state (its brain verb).
    t.SetEvent(false);
    CHECK_FALSE(t.OnBrainVerb(kBsa, 9, false, false));
    CHECK(t.Totals().copy_own_run == 3);
    std::optional<CoopPuppet::Call> c;
    std::uint64_t steps = 0;
    while (!c && steps <= Table::kAfterEventSteps) {
        c = t.Step(g, false, 0, kMain);
        ++steps;
    }
    REQUIRE(c);
    CHECK(steps >= Table::kAfterEventSteps);
    CHECK(c->function == kMain + addr::kSetBrainVerb);
    CHECK(c->x[1] == 5);
    // And its own changes are skipped again.
    CHECK(t.OnBrainVerb(kBsa, 9, false, false));
}

TEST_CASE("native boss: a copy's own change skipped just before our event is made late") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    FakeGuest g;
    const std::int32_t at = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    // An old skip (long before) and one in the frame the event began.
    CHECK(t.OnBrainVerb(kBsa, 7, false, false));
    for (std::uint64_t i = 0; i < Table::kLookBackSteps + 2; ++i) {
        CHECK_FALSE(t.Step(g, false, 0, kMain));
    }
    CHECK(t.OnBrainVerb(kBsa, 9, true, false));
    t.SetEvent(true);
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSetBrainVerb);
    CHECK(c->x[1] == 9);  // only the recent one
    CHECK(c->x[2] == 1);  // with its own force
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    CHECK(t.Totals().copy_own_late == 1);
}

TEST_CASE("native boss: our own hit on a copy lets its own reaction run for a while") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    FakeGuest g;
    const std::int32_t at = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    // The stun's action was asked in the frame of the hit, before the walk saw the hit: skipped, then made late.
    CHECK(t.OnActionVerb(kBsa, GameRequest(30, kNode), true, false));
    t.NoteOwnHit(kActor);
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kMalloc);
    c = t.Step(g, true, kBlock, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kRequestActionVerb);
    CHECK(g.Get<std::int32_t>(kBlock + addr::kReqVerb) == 30);
    CHECK(g.Get<std::uint64_t>(kBlock + addr::kReqNode) == kNode);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    // What follows in the reaction runs.
    CHECK_FALSE(t.OnBrainVerb(kBsa, 12, false, false));
    CHECK(t.Totals().copy_own_run == 1);
    // A hit on a boss we own, or an unknown actor: nothing changes.
    t.NoteOwnHit(kActor + 8);
    // After the reaction time: the owner's state again, own changes skipped.
    for (std::uint64_t i = 0; i < Table::kReactSteps; ++i) {
        (void)t.Step(g, false, 0, kMain);
    }
    CHECK(t.OnBrainVerb(kBsa, 12, false, false));
}

TEST_CASE("native boss: whether our own hit took life (row 138)") {
    using CoopBossNative::OwnHitTookLife;
    CHECK(OwnHitTookLife(20, 18));
    CHECK_FALSE(OwnHitTookLife(20, 20));        // a stun, a guard, a weak-point hit: no life taken
    CHECK_FALSE(OwnHitTookLife(18, 20));        // raised meanwhile (its owner's life)
    CHECK_FALSE(OwnHitTookLife(std::nullopt, 18));  // not read the walk before: unknown, its own reaction as before
    CHECK_FALSE(OwnHitTookLife(20, std::nullopt));
}

TEST_CASE("native boss: our own hit that took life leaves the reaction to the owner (row 138 (2)(7), #238)") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    FakeGuest g;
    const std::int32_t at = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    // Its own flinch was asked in the frame of the hit: skipped, and it stays skipped (nothing made late).
    CHECK(t.OnActionVerb(kBsa, GameRequest(30, kNode), true, false));
    t.NoteOwnDamage(kActor);
    CHECK(t.Totals().copy_hits_to_owner == 1);
    CHECK(t.CopiesRunningOwn().empty());  // the objects layer keeps putting the owner's place on it
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    // What its own brain asks next is still skipped: one brain, the owner's, whoever hits.
    CHECK(t.OnBrainVerb(kBsa, 12, false, false));
    CHECK(t.OnActionVerb(kBsa, GameRequest(31, kNode), true, false));
    CHECK(t.Totals().copy_own_run == 0);
    CHECK(t.Totals().copy_own_late == 0);
    // A hit that took no life (a stun) still runs its own reaction, as before.
    t.NoteOwnHit(kActor);
    CHECK(t.CopiesRunningOwn() == std::vector<std::uint64_t>{kActor});
    // And a hit that took life inside that own time leaves it running (our stun goes on).
    t.NoteOwnDamage(kActor);
    CHECK(t.Totals().copy_hits_to_owner == 2);
    CHECK(t.CopiesRunningOwn() == std::vector<std::uint64_t>{kActor});
}

TEST_CASE("native boss: our own hit that took life on a boss we own changes nothing (solo, owner)") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::owner)});
    t.NoteOwnDamage(kActor);
    t.NoteOwnDamage(kActor + 8);  // an actor it does not know
    CHECK(t.Totals().copy_hits_to_owner == 0);
    CHECK(t.CopiesRunningOwn().empty());
    CHECK_FALSE(t.OnBrainVerb(kBsa, 12, false, false));  // an owner's own brain always runs
}

TEST_CASE("native boss: our shot's end beside a copy (no hit id moved) runs its own stun, issue 150") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    FakeGuest g;
    const std::int32_t at = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    const coop::protocol::Vec3 gohma{18.5f, 60.0f, 67.8f};
    // Nothing shot yet, or a shot far off (a wall across the room): not near.
    CHECK_FALSE(t.OwnShotNear(gohma));
    t.NoteOwnShotEnd({18.5f, 60.0f, 67.8f + Table::kShotNearBoss + 1.0f});
    CHECK_FALSE(t.OwnShotNear(gohma));
    // A place the game cannot have (NaN) is not kept.
    t.NoteOwnShotEnd({std::nanf(""), 60.0f, 67.8f});
    CHECK_FALSE(t.OwnShotNear(gohma));
    // The seed in her eye: the stun's action was asked in the frame of the hit and skipped (her copy).
    CHECK(t.OnActionVerb(kBsa, GameRequest(30, kNode), true, false));
    t.NoteOwnShotEnd({19.0f, 62.5f, 68.0f});
    CHECK(t.OwnShotNear(gohma));
    // The walk's reading: the copy runs its own reaction, the skipped stun made late.
    t.NoteOwnHit(kActor, true);
    CHECK(t.Totals().copy_shot_hits == 1);
    CHECK(t.CopiesRunningOwn() == std::vector<std::uint64_t>{kActor});
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kMalloc);
    c = t.Step(g, true, kBlock, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kRequestActionVerb);
    CHECK(g.Get<std::int32_t>(kBlock + addr::kReqVerb) == 30);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    // Its reaction runs; a second walk inside it counts no second shot hit.
    CHECK_FALSE(t.OnBrainVerb(kBsa, 12, false, false));
    t.NoteOwnHit(kActor, true);
    CHECK(t.Totals().copy_shot_hits == 1);
    // The end is kept a few frames only.
    for (std::uint64_t i = 0; i <= Table::kShotEndKeepSteps; ++i) {
        (void)t.Step(g, false, 0, kMain);
    }
    CHECK_FALSE(t.OwnShotNear(gohma));
    // Forgotten with the rest.
    t.NoteOwnShotEnd(gohma);
    t.Reset();
    CHECK_FALSE(t.OwnShotNear(gohma));
}

TEST_CASE("native boss: our shot's end beside a boss we own changes nothing (solo, owner)") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::owner)});
    t.NoteOwnShotEnd({18.5f, 60.0f, 67.8f});
    CHECK(t.OwnShotNear({18.5f, 60.0f, 67.8f}));
    t.NoteOwnHit(kActor, true);  // the walk calls it for every boss: an owner's is no copy
    CHECK(t.Totals().copy_shot_hits == 0);
    CHECK(t.CopiesRunningOwn().empty());
    // Our own boss's changes are its own game's: never skipped.
    CHECK_FALSE(t.OnBrainVerb(kBsa, 9, false, false));
}

TEST_CASE("native boss: the owner keeps none of its event's changes and sends its verb when it is over") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::owner)});
    CHECK_FALSE(t.OnBrainVerb(kBsa, 3, false, false));
    REQUIRE(t.OwnerState(kActor));
    CHECK(t.OwnerState(kActor)->seq == 1);
    t.SetEvent(true);
    CHECK_FALSE(t.OnBrainVerb(kBsa, 9, true, false));  // the game runs it, nothing kept
    CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(21, kNode), true, false));
    FakeGuest g;
    std::int32_t at = 9;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    CHECK(t.OwnerState(kActor)->seq == 1);
    CHECK(t.OwnerState(kActor)->brain_verb == 3);  // not the cutscene's verb
    // The intro leaves her in her battle verb: sent as a change.
    at = 6;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    t.SetEvent(false);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    const auto s = t.OwnerState(kActor);
    REQUIRE(s);
    CHECK(s->seq == 2);
    CHECK(s->brain_verb == 6);
    REQUIRE_FALSE(s->recent.empty());
    CHECK(std::get<BossBrain>(s->recent.back().what) == BossBrain{6, true});
}

TEST_CASE("native boss: a copy's own Life step holds it at 1 until the owner's dies") {
    constexpr std::uint64_t kLife = 0x7700'0000;
    Table t;
    t.SetOn(true, true);
    Boss copy = Gohma(Role::copy);
    copy.life_component = kLife;
    t.Publish({copy});
    CHECK(t.Watches(kLife));
    CHECK_FALSE(t.Watches(kLife + 0x10));
    CHECK(t.HeldLifeStep(kLife, 30, 10) == 10);  // a hit that leaves life: as the game has it
    CHECK(t.HeldLifeStep(kLife, 5, 21) == 4);    // a killing hit of its own: life 1 stays
    CHECK(t.HeldLifeStep(kLife, 1, 3) == 0);
    CHECK(t.HeldLifeStep(kLife + 0x10, 5, 21) == 21);  // another actor's: untouched
    CHECK(t.Totals().copy_life_held == 2);
    CHECK_FALSE(t.Finishes(kLife));  // held: its own stops stay (its death is not its owner's yet)
    // The owner's died: the same step lets ours die, past its own life stops.
    copy.owner_dead = true;
    t.Publish({copy});
    CHECK(t.Finishes(kLife));
    CHECK_FALSE(t.Finishes(kLife + 0x10));
    CHECK(t.HeldLifeStep(kLife, 1, 1) == 1);
    CHECK(t.Totals().copy_killed == 1);
    const auto notes = t.TakeNotes();
    CHECK(std::any_of(notes.begin(), notes.end(),
                      [](const std::string& n) { return n.find("lets it die now") != std::string::npos; }));
}

TEST_CASE("native boss: the owner's own boss and a forgotten copy are never held") {
    constexpr std::uint64_t kLife = 0x7700'0000;
    Table t;
    t.SetOn(true, true);
    Boss owner = Gohma(Role::owner);
    owner.life_component = kLife;
    t.Publish({owner});
    CHECK_FALSE(t.Watches(kLife));
    CHECK_FALSE(t.Finishes(kLife));  // our own boss: every stop is its game's
    CHECK(t.HeldLifeStep(kLife, 5, 21) == 21);
    Boss copy = Gohma(Role::copy);
    copy.life_component = kLife;
    t.Publish({copy});
    CHECK(t.HeldLifeStep(kLife, 5, 21) == 4);
    t.Publish({});  // gone from the walk (its owner's ObjectRemove dead: nobody's now)
    CHECK_FALSE(t.Watches(kLife));
    CHECK(t.HeldLifeStep(kLife, 5, 21) == 21);
    t.Publish({copy});
    t.Reset();
    CHECK_FALSE(t.Watches(kLife));
}

TEST_CASE("native boss: a new mid-boss copy runs its own start, then takes the owner's state") {
    // Issue 62 (Dodongo's Cavern): the second Lizalfos's copy took the owner's
    // verbs at once, skipped its own appear and stayed unseen.
    CHECK(RunsOwnStart("Enemy_MiddleBoss_Lizalfos"));
    CHECK_FALSE(RunsOwnStart("Enemy_DungeonBoss_Gohma"));  // its intro is an event
    Table t;
    t.SetOn(true, true);
    t.Publish({Boss{kActor, kBsa, Role::copy, "Enemy_MiddleBoss_Lizalfos", 2}});
    t.Receive(kActor, BossBsa{3, 62, std::nullopt, {}});
    FakeGuest g;
    const std::int32_t at = 60;  // its own start's phase
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    // Its own appear runs; the owner's verbs wait.
    CHECK_FALSE(t.OnBrainVerb(kBsa, 61, false, false));
    CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(5, kNode), true, false));
    for (std::uint64_t i = 0; i + 1 < Table::kStartSteps; ++i) {
        CHECK_FALSE(t.Step(g, false, 0, kMain));
    }
    // Over: the owner's phase is put in, and its own are skipped again.
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSetBrainVerb);
    CHECK(c->x[1] == 62);
    CHECK(t.OnBrainVerb(kBsa, 61, false, false));
    // A hand-over of the same copy (not a new one) starts no own time.
    t.Publish({Boss{kActor, kBsa, Role::copy, "Enemy_MiddleBoss_Lizalfos", 3}});
    t.Receive(kActor, BossBsa{1, 62, std::nullopt, {}});
    CHECK(t.OnBrainVerb(kBsa, 61, false, false));
}

TEST_CASE("native boss: a mid-boss of ours handed over during its start runs its start on (#238)") {
    // Revirie's 0.11.0 log: our two Lizalfos were handed to player 1 while their appear still played; the copy
    // took the owner's verbs at once, stayed at 'Appear' (moved 0.00) and was unseen on our screen.
    CHECK(PlaysOwnStart("Appear"));
    CHECK_FALSE(PlaysOwnStart("Wait"));
    CHECK_FALSE(PlaysOwnStart(""));
    auto liz = [](Role role, std::uint8_t owner, std::string anim) {
        return Boss{kActor, kBsa, role, "Enemy_MiddleBoss_Lizalfos", owner, 0, false, false, false, std::move(anim)};
    };
    FakeGuest g;
    const std::int32_t at = 60;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    SUBCASE("handed over just after it was made: its start runs on") {
        Table t;
        t.SetOn(true, true);
        t.Publish({liz(Role::owner, 3, "Wait")});
        for (int i = 0; i < 20; ++i) {
            CHECK_FALSE(t.Step(g, false, 0, kMain));
        }
        t.Publish({liz(Role::copy, 1, "Wait")});
        t.Receive(kActor, BossBsa{3, 62, std::nullopt, {}});
        CHECK_FALSE(t.OnBrainVerb(kBsa, 61, false, false));  // its own appear runs
        for (std::uint64_t i = 0; i + 1 < Table::kStartSteps; ++i) {
            CHECK_FALSE(t.Step(g, false, 0, kMain));
        }
        auto c = t.Step(g, false, 0, kMain);
        REQUIRE(c);
        CHECK(c->x[1] == 62);  // then the owner's state
        const auto notes = t.TakeNotes();
        CHECK(std::any_of(notes.begin(), notes.end(),
                          [](const std::string& n) { return n.find("during its own start") != std::string::npos; }));
    }
    SUBCASE("handed over long after, its appear still playing: its start runs on") {
        Table t;
        t.SetOn(true, true);
        t.Publish({liz(Role::owner, 3, "Wait")});
        for (std::uint64_t i = 0; i < Table::kStartSteps + 10; ++i) {
            CHECK_FALSE(t.Step(g, false, 0, kMain));
        }
        t.Publish({liz(Role::copy, 1, "Appear")});
        t.Receive(kActor, BossBsa{3, 62, std::nullopt, {}});
        CHECK_FALSE(t.OnBrainVerb(kBsa, 61, false, false));
    }
    SUBCASE("handed over long after, in the fight: the owner's verbs at once (as before)") {
        Table t;
        t.SetOn(true, true);
        t.Publish({liz(Role::owner, 3, "Wait")});
        for (std::uint64_t i = 0; i < Table::kStartSteps + 10; ++i) {
            CHECK_FALSE(t.Step(g, false, 0, kMain));
        }
        t.Publish({liz(Role::copy, 1, "Walk")});
        t.Receive(kActor, BossBsa{3, 62, std::nullopt, {}});
        CHECK(t.OnBrainVerb(kBsa, 61, false, false));
    }
    SUBCASE("a new copy whose appear outlasts its start time: runs on, up to kStartMaxSteps") {
        Table t;
        t.SetOn(true, true);
        t.Publish({liz(Role::copy, 1, "Appear")});
        t.Receive(kActor, BossBsa{3, 62, std::nullopt, {}});
        std::uint64_t own = 0;
        for (std::uint64_t i = 0; i < Table::kStartMaxSteps + 50; ++i) {
            t.Publish({liz(Role::copy, 1, "Appear")});
            if (t.Step(g, false, 0, kMain)) {
                break;  // the owner's state is put in: its own time is over
            }
            ++own;
        }
        CHECK(own >= Table::kStartSteps);
        CHECK(own + 1 >= Table::kStartMaxSteps);
        CHECK(own < Table::kStartMaxSteps + 2);
    }
    SUBCASE("a new copy whose appear ended: the owner's state after kStartSteps (as before)") {
        Table t;
        t.SetOn(true, true);
        t.Publish({liz(Role::copy, 1, "Appear")});
        t.Receive(kActor, BossBsa{3, 62, std::nullopt, {}});
        std::uint64_t own = 0;
        for (std::uint64_t i = 0; i < Table::kStartMaxSteps; ++i) {
            t.Publish({liz(Role::copy, 1, "Walk")});
            if (t.Step(g, false, 0, kMain)) {
                break;
            }
            ++own;
        }
        CHECK(own + 1 == Table::kStartSteps);
    }
}

// Player report #105: a black screen after Gohma's death in the host's game. The death setting our game is asked
// (InsideTheDekuTree_BossDie) runs her own death in our copy; past the event the copy rules skipped its own
// changes again and its Life step held her at 1, so the death never finished (no fade back, no warp).
TEST_CASE("native boss: once its death has begun, a copy runs all its own and its Life is not held") {
    constexpr std::uint64_t kLife = 0x7700'0000;
    Table t;
    t.SetOn(true, true);
    Boss copy = Gohma(Role::copy);
    copy.life_component = kLife;
    t.Publish({copy});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    FakeGuest g;
    const std::int32_t at = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    CHECK(t.OnBrainVerb(kBsa, 9, true, false));  // a moment before: skipped (still a copy)
    CHECK(t.HeldLifeStep(kLife, 5, 21) == 4);
    // The owner's death is heard: its own death plays here, the skipped change made late.
    CHECK_FALSE(t.Finishes(kLife));
    t.SetDying({"Enemy_DungeonBoss_Gohma"});
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->x[1] == 9);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    CHECK(t.Watches(kLife));
    CHECK(t.Finishes(kLife));  // its death has begun: past its own stops (Gohma's at 1, hs-gohma-death 20261115)
    CHECK(t.HeldLifeStep(kLife, 1, 1) == 1);  // dies of its own
    CHECK(t.HeldLifeStep(kLife, 5, 1) == 5);  // her own rules let 1 through: the step ends her all the same
    CHECK(t.Totals().copy_finished == 1);
    // Long after our event is over and with the owner's (fight) state still coming: never skipped, never put back.
    t.SetEvent(true);
    t.SetEvent(false);
    t.Receive(kActor, BossBsa{4, 5, std::nullopt, {BossChange{4, BossBrain{5, false}}}});
    for (std::uint64_t i = 0; i < Table::kAfterEventSteps + Table::kDriftSteps + 10; ++i) {
        CHECK_FALSE(t.Step(g, false, 0, kMain));
    }
    CHECK_FALSE(t.OnBrainVerb(kBsa, 12, false, false));
    CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(21, kNode), true, false));
    // A walk's next publish keeps both (the dying list is the glue's, every frame).
    t.Publish({copy});
    CHECK_FALSE(t.OnBrainVerb(kBsa, 13, false, false));
    CHECK(t.HeldLifeStep(kLife, 5, 21) == 21);
    // The owner's dead remove takes her off the owned list: her Life step still ends her while she is dying.
    t.Publish({});
    CHECK(t.Watches(kLife));
    CHECK(t.HeldLifeStep(kLife, 5, 2) == 5);
    CHECK(t.Finishes(kLife));  // (off the owned list, still dying)
    t.SetDying({});
    CHECK_FALSE(t.Watches(kLife));
    CHECK_FALSE(t.Finishes(kLife));
    CHECK(t.HeldLifeStep(kLife, 5, 2) == 2);
    const auto notes = t.TakeNotes();
    CHECK(std::any_of(notes.begin(), notes.end(),
                      [](const std::string& n) { return n.find("its death has begun") != std::string::npos; }));
}

TEST_CASE("native boss: a death list for another boss, or none, leaves a copy a copy") {
    constexpr std::uint64_t kLife = 0x7700'0000;
    Table t;
    t.SetOn(true, true);
    Boss copy = Gohma(Role::copy);
    copy.life_component = kLife;
    t.Publish({copy});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    t.SetDying({"Enemy_DungeonBoss_Dodongo"});
    CHECK(t.OnBrainVerb(kBsa, 9, false, false));
    CHECK(t.HeldLifeStep(kLife, 5, 21) == 4);
    // Her death, then a new fight (the host loaded a save: the list empties): a copy again after a moment.
    t.SetDying({"Enemy_DungeonBoss_Gohma"});
    CHECK_FALSE(t.OnBrainVerb(kBsa, 9, false, false));
    t.SetDying({});
    t.Publish({copy});
    CHECK(t.HeldLifeStep(kLife, 5, 21) == 4);
    FakeGuest g;
    for (std::uint64_t i = 0; i <= Table::kAfterEventSteps; ++i) {
        t.Step(g, false, 0, kMain);
        t.Step(g, true, 0, kMain);
    }
    CHECK(t.OnBrainVerb(kBsa, 9, false, false));
    // Reset forgets the list too.
    t.SetDying({"Enemy_DungeonBoss_Gohma"});
    t.Reset();
    t.Publish({copy});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    CHECK(t.OnBrainVerb(kBsa, 9, false, false));
}

// The enemies' brain path (fable-g4-enemies, LICHII_COOP_ENEMY_BRAIN): a plain enemy as Boss::plain.
namespace {

Boss Baba(Role role) {
    Boss b{kActor, kBsa, role, "Enemy_Dekubaba", 2};
    b.plain = true;
    return b;
}

} // namespace

TEST_CASE("native boss: a plain enemy copy skips its own brain, takes the owner's and tells what its own wanted") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Baba(Role::copy)});
    CHECK_FALSE(t.OnBrainVerb(kBsa, 2, false, false));  // no owner's state yet: as before
    CHECK(t.TakeOwnWants().empty());
    t.Receive(kActor, BossBsa{5, 4, std::nullopt, {}});
    CHECK(t.OnBrainVerb(kBsa, 2, false, false));  // its own, another phase than the owner's: skipped and told
    CHECK(t.OnBrainVerb(kBsa, 2, false, false));  // (told once)
    CHECK(t.OnBrainVerb(kBsa, 4, false, false));  // its own, the owner's phase: skipped, nothing to tell
    const auto wants = t.TakeOwnWants();
    REQUIRE(wants.size() == 1);
    CHECK(wants[0] == kActor);
    CHECK(t.TakeOwnWants().empty());
    CHECK(t.Totals().copy_skipped == 3);
    CHECK(t.Totals().plain_wants == 2);
    FakeGuest g;
    const auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSetBrainVerb);
    CHECK(c->x[0] == kBsa);
    CHECK(c->x[1] == 4);
    // A boss's own other phase is never told: the Sync's chase rule is the plain enemies'.
    Table b;
    b.SetOn(true, true);
    b.Publish({Gohma(Role::copy)});
    b.Receive(kActor, BossBsa{5, 4, std::nullopt, {}});
    CHECK(b.OnBrainVerb(kBsa, 2, false, false));
    CHECK(b.TakeOwnWants().empty());
}

TEST_CASE("native boss: the owner's brain verbs a copy is given are counted, also the ones it was in already") {
    // g4-enemies probe: Deku Tree copies showed 0 brain verbs put in; a resync whose verb the copy is in makes
    // no call, so the count alone could not tell 'never came' from 'came, already so'.
    Table t;
    t.SetOn(true, true);
    t.Publish({Baba(Role::copy)});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});  // first state: a resync to brain 4
    FakeGuest g;
    const std::int32_t at = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));  // already in 4: no call
    CHECK(t.Totals().brain_came == 1);
    CHECK(t.Totals().brain_already == 1);
    CHECK(t.Totals().brain_applied == 0);
    // The owner's next change, joined to the last: queued and put in.
    t.Receive(kActor, BossBsa{4, 6, std::nullopt, {BossChange{4, BossBrain{6, false}}}});
    const auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->x[1] == 6);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    CHECK(t.Totals().brain_came == 2);
    CHECK(t.Totals().brain_already == 1);
    CHECK(t.Totals().brain_applied == 1);
}

TEST_CASE("native boss: a first state queued in our event and replaced after it is counted once") {
    // g4-enemies-2 probe (Deku Tree): 'given 33' for 16 copies, all 'already so' after the event: the first
    // state's resync came in our event and the one after the event replaced it; both were counted.
    Table t;
    t.SetOn(true, true);
    t.Publish({Baba(Role::copy)});
    t.SetEvent(true);
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    FakeGuest g;
    const std::int32_t at = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));  // our event: its own runs
    t.SetEvent(false);
    for (std::uint64_t i = 0; i <= Table::kAfterEventSteps + 1; ++i) {
        CHECK_FALSE(t.Step(g, false, 0, kMain));  // then the owner's state: already so
    }
    CHECK(t.Totals().brain_came == 1);
    CHECK(t.Totals().brain_already == 1);
}

TEST_CASE("native boss: a change whose call was not made is asked again, not lost") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    FakeGuest g;
    const std::int32_t at = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    t.Receive(kActor, BossBsa{4, 6, std::nullopt, {BossChange{4, BossBrain{6, false}}}});
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->x[1] == 6);
    // The runtime could not make it (the frame's calls ran out): the next frame asks the same change.
    c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSetBrainVerb);
    CHECK(c->x[1] == 6);
    CHECK(c->copy_game);  // (#189: what the copy's game makes inside it is judged as its own)
    CHECK_FALSE(t.Step(g, true, 0, kMain));  // made: out of the queue
    CHECK(t.Totals().brain_applied == 1);
    CHECK(t.Totals().brain_came == 2);  // (the first state, already so, and 6 once though asked twice)
    const std::int32_t six = 6;
    g.Write(kBsa + addr::kBsaBrainVerb, &six, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));  // nothing left
    // An action not made: asked again with the same values.
    CHECK(t.OnActionVerb(kBsa, GameRequest(5, kNode), true, false));  // its own ask, skipped: its node seen
    const BossAction a = *ActionFromRequest(GameRequest(11, 0));
    t.Receive(kActor, BossBsa{5, 6, a, {BossChange{5, a}}});
    c = t.Step(g, false, 0, kMain);  // the request block first
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kMalloc);
    CHECK_FALSE(c->copy_game);  // (our own block)
    c = t.Step(g, true, kBlock, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kRequestActionVerb);
    c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kRequestActionVerb);
    CHECK(c->copy_game);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    CHECK(t.Totals().action_applied == 1);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
}

TEST_CASE("native boss: a copy's own late change whose call was not made is asked again") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy)});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    FakeGuest g;
    const std::int32_t at = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    CHECK(t.OnBrainVerb(kBsa, 9, true, false));
    t.SetEvent(true);
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->x[1] == 9);
    c = t.Step(g, false, 0, kMain);  // not made
    REQUIRE(c);
    CHECK(c->x[1] == 9);
    CHECK(c->copy_game);
    CHECK(t.Totals().copy_own_late == 0);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    CHECK(t.Totals().copy_own_late == 1);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
}

TEST_CASE("native boss: a plain enemy copy's own wake runs its own brain for a while, a boss's does not") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Baba(Role::copy)});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    FakeGuest g;
    const std::int32_t at = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    // Its rise was asked a moment before the Sync saw the wake: skipped, then made late.
    CHECK(t.OnBrainVerb(kBsa, 9, false, false));
    t.NoteOwnWake(kActor);
    const auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSetBrainVerb);
    CHECK(c->x[1] == 9);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    CHECK_FALSE(t.OnBrainVerb(kBsa, 10, false, false));  // what follows in the wake runs
    CHECK(t.Totals().plain_wakes == 1);
    CHECK(t.Totals().copy_own_late == 1);
    for (std::uint64_t i = 0; i < Table::kWakeSteps; ++i) {
        (void)t.Step(g, false, 0, kMain);
    }
    CHECK(t.OnBrainVerb(kBsa, 10, false, false));  // over: the owner's state again
    // A boss has no own wake.
    Table b;
    b.SetOn(true, true);
    b.Publish({Gohma(Role::copy)});
    b.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    b.NoteOwnWake(kActor);
    CHECK(b.OnBrainVerb(kBsa, 9, false, false));
    CHECK(b.Totals().plain_wakes == 0);
}

TEST_CASE("native boss: only a plain enemy copy with the owner's state wakes by its own brain (rows 109/111)") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Baba(Role::copy)});
    CHECK_FALSE(t.WakesOwnBrain(kActor));  // no owner's state yet: its own brain runs anyway, the Mover's wake too
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    CHECK(t.WakesOwnBrain(kActor));
    CHECK_FALSE(t.WakesOwnBrain(kActor + 0x10));  // not in the Table
    // The owner's own enemy is not a copy.
    Table o;
    o.SetOn(true, true);
    o.Publish({Baba(Role::owner)});
    o.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    CHECK_FALSE(o.WakesOwnBrain(kActor));
    // A boss copy keeps the Mover's give-back.
    Table b;
    b.SetOn(true, true);
    b.Publish({Gohma(Role::copy)});
    b.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    CHECK_FALSE(b.WakesOwnBrain(kActor));
    // Brain switch off: nothing is skipped, so the pose-era give-back stays.
    Table off;
    off.SetOn(false, true);
    off.Publish({Baba(Role::copy)});
    off.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    CHECK_FALSE(off.WakesOwnBrain(kActor));
}

TEST_CASE("native boss: a plain enemy copy's Life is never held and its death runs its own") {
    constexpr std::uint64_t kLife = 0x7700'0000;
    CHECK(PlainDeath(0, false));
    CHECK(PlainDeath(-3, false));
    CHECK(PlainDeath(5, true));
    CHECK(PlainDeath(std::nullopt, true));
    CHECK_FALSE(PlainDeath(5, false));
    CHECK_FALSE(PlainDeath(std::nullopt, false));
    Table t;
    t.SetOn(true, true);
    Boss copy = Baba(Role::copy);
    copy.life_component = kLife;
    t.Publish({copy});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    CHECK_FALSE(t.Watches(kLife));
    CHECK(t.HeldLifeStep(kLife, 5, 21) == 21);  // a killing hit of ours kills it (the Sync shares the kill)
    CHECK(t.Totals().copy_life_held == 0);
    FakeGuest g;
    const std::int32_t at = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    CHECK(t.OnBrainVerb(kBsa, 20, false, false));  // its death's first change, before the walk saw life 0
    copy.own_death = true;
    t.Publish({copy});
    const auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSetBrainVerb);
    CHECK(c->x[1] == 20);  // made late
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    CHECK_FALSE(t.OnBrainVerb(kBsa, 21, false, false));  // all its own from now on
    t.Publish({copy});
    CHECK(t.Totals().plain_deaths == 1);
    // A boss list's death names leave a plain enemy alone.
    Table d;
    d.SetOn(true, true);
    d.Publish({Baba(Role::copy)});
    d.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    d.SetDying({"Enemy_Dekubaba"});
    CHECK(d.OnBrainVerb(kBsa, 9, false, false));
}

TEST_CASE("native boss: many copies take turns at the one call a frame") {
    Table t;
    t.SetOn(true, true);
    Boss a = Baba(Role::copy);
    Boss b = Baba(Role::copy);
    b.actor = kActor + 0x100;
    b.bsa = kBsa + 0x100;
    t.Publish({a, b});
    t.Receive(a.actor, BossBsa{5, 4, std::nullopt, {}});
    t.Receive(b.actor, BossBsa{5, 6, std::nullopt, {}});
    FakeGuest g;
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->x[0] == a.bsa);
    CHECK(c->x[1] == 4);
    // The first one has a change again at once: the other's turn comes first.
    t.Receive(a.actor, BossBsa{6, 7, std::nullopt, {}});
    c = t.Step(g, true, 0, kMain);
    REQUIRE(c);
    CHECK(c->x[0] == b.bsa);
    CHECK(c->x[1] == 6);
    c = t.Step(g, true, 0, kMain);
    REQUIRE(c);
    CHECK(c->x[0] == a.bsa);
    CHECK(c->x[1] == 7);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    CHECK(t.Totals().brain_applied == 3);
}

// Row 130 (two players entering Gohma's room together; only one could beat her). The second game's own appear event
// ran with a copy Gohma the objects layer kept putting into its owner's animation (C01 at frame 1 over her own
// Pre_Battle_Ceiling_Find) every frame: the event waited for her for good. The table now names the copies that run
// their own, and the objects layer puts none of their owner's motion on them meanwhile.
TEST_CASE("native boss: the copies that run their own now are named for the objects layer (row 130)") {
    constexpr std::uint64_t kOwned = kActor + 0x100;
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::copy), Boss{kOwned, kBsa + 0x100, Role::owner, "Enemy_MiddleBoss_Stalfos", 1}});
    t.Receive(kActor, BossBsa{3, 4, std::nullopt, {}});
    FakeGuest g;
    const std::int32_t at = 4;
    g.Write(kBsa + addr::kBsaBrainVerb, &at, 4);
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    using V = std::vector<std::uint64_t>;
    CHECK(t.CopiesRunningOwn().empty());  // a copy taking its owner's verbs; our own boss is never named

    SUBCASE("our event, and kAfterEventSteps after it") {
        t.SetEvent(true);
        CHECK(t.CopiesRunningOwn() == V{kActor});
        t.SetEvent(false);
        CHECK(t.CopiesRunningOwn() == V{kActor});
        for (std::uint64_t i = 0; i < Table::kAfterEventSteps + 1; ++i) {
            (void)t.Step(g, false, 0, kMain);
        }
        CHECK(t.CopiesRunningOwn().empty());
    }
    SUBCASE("our own hit's reaction") {
        t.NoteOwnHit(kActor);
        CHECK(t.CopiesRunningOwn() == V{kActor});
        t.NoteOwnHit(kOwned);  // our own boss: never named
        for (std::uint64_t i = 0; i < Table::kReactSteps + 1; ++i) {
            (void)t.Step(g, false, 0, kMain);
        }
        CHECK(t.CopiesRunningOwn().empty());
    }
    SUBCASE("its death") {
        t.SetDying({"Enemy_DungeonBoss_Gohma"});
        CHECK(t.CopiesRunningOwn() == V{kActor});
    }
    SUBCASE("a new mid-boss copy's own start") {
        Table m;
        m.SetOn(true, true);
        m.Publish({Boss{kActor, kBsa, Role::copy, "Enemy_MiddleBoss_Lizalfos", 2}});
        CHECK(m.CopiesRunningOwn() == V{kActor});
    }
    SUBCASE("native sync off: none (the owner's motion is the only sync)") {
        t.SetEvent(true);
        t.SetOn(false, false);
        CHECK(t.CopiesRunningOwn().empty());
    }
}

namespace {

Boss Dodongo(Role role, std::string animation, std::optional<std::int32_t> life, std::uint8_t owner = 2) {
    Boss b{kActor, kBsa, role, "Enemy_DungeonBoss_Dodongo", owner};
    b.animation = std::move(animation);
    b.life = life;
    return b;
}

// Steps of our own King Dodongo in one move (its AI asking the roll's action each frame, as in the row 138 log)
// until the table asks a call or `steps` ran out; its life 20 from step `life_at` on (a hit).
std::optional<CoopPuppet::Call> HoldMove(Table& t, FakeGuest& g, std::uint64_t steps,
                                         std::string animation = "Rolling_Loop", bool asks = true,
                                         std::uint64_t life_at = ~0ull) {
    for (std::uint64_t i = 0; i < steps; ++i) {
        if (asks) {
            CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(5, kNode), false, false));
        }
        t.Publish({Dodongo(Role::owner, animation, i >= life_at ? 20 : 24)});
        if (auto c = t.Step(g, false, 0, kMain)) {
            return c;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("native boss: row 149: our boss stuck in one move with no hit chooses again, sent to every copy") {
    Table t;
    t.SetOn(true, true);
    FakeGuest g;
    const std::int32_t brain = 12;
    g.Write(kBsa + addr::kBsaBrainVerb, &brain, 4);
    t.Publish({Dodongo(Role::owner, "Rolling_Loop", 24)});
    CHECK_FALSE(t.OnBrainVerb(kBsa, brain, false, false));
    // Short of the stall: nothing (a real roll ends at a wall within seconds).
    CHECK_FALSE(HoldMove(t, g, Table::kStallSteps - 2));
    auto c = HoldMove(t, g, 4);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSetBrainVerb);
    CHECK(c->x[0] == kBsa);
    CHECK(c->x[1] == 12);  // the brain verb it is in, again
    CHECK(c->x[2] == 1);   // forced
    CHECK(t.Totals().owner_stalls == 1);
    // The call runs the wrap as our caller's: kept and sent as any change of ours.
    const std::uint32_t seq = t.OwnerState(kActor)->seq;
    CHECK_FALSE(t.OnBrainVerb(kBsa, brain, true, true));
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    CHECK(t.Totals().owner_rechosen == 1);
    const auto s = t.OwnerState(kActor);
    REQUIRE(s);
    CHECK(s->seq == seq + 1);
    REQUIRE(std::holds_alternative<BossBrain>(s->recent.back().what));
    CHECK(std::get<BossBrain>(s->recent.back().what).verb == 12);
    CHECK(std::get<BossBrain>(s->recent.back().what).force);
    // Asked once: the next frame makes no call.
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    bool noted = false;
    for (const std::string& n : t.TakeNotes()) {
        noted = noted || n.find("held 'Rolling_Loop'") != std::string::npos;
    }
    CHECK(noted);
    // Still stuck: again, at most kMaxStallRechoices times in the one move (then the server's hand-over).
    int more = 0;
    for (int i = 0; i < Table::kMaxStallRechoices + 1; ++i) {
        if (HoldMove(t, g, Table::kStallSteps + 2)) {
            ++more;
            CHECK_FALSE(t.Step(g, true, 0, kMain));
        }
    }
    CHECK(more == Table::kMaxStallRechoices - 1);
    // A new move: counted from it, re-chosen again if it sticks too.
    CHECK(HoldMove(t, g, Table::kStallSteps + 2, "Rolling_Start"));
}

TEST_CASE("native boss: row 149: a hit, an idle boss, our event, no life read or a copy never stall") {
    FakeGuest g;
    const std::int32_t brain = 12;
    g.Write(kBsa + addr::kBsaBrainVerb, &brain, 4);
    {
        // A hit that took life halfway: counted again from it.
        Table t;
        t.SetOn(true, true);
        CHECK_FALSE(t.OnBrainVerb(kBsa, brain, false, false));
        CHECK_FALSE(HoldMove(t, g, Table::kStallSteps + 2, "Rolling_Loop", true, Table::kStallSteps / 2));
    }
    {
        // Its AI asks no action (Gohma waiting on the ceiling): no stall.
        Table t;
        t.SetOn(true, true);
        CHECK_FALSE(t.OnBrainVerb(kBsa, brain, false, false));
        CHECK_FALSE(HoldMove(t, g, Table::kStallSteps * 2, "Wait", false));
        CHECK(t.Totals().owner_stalls == 0);
    }
    {
        // Our event (its cutscene holds one animation): no stall.
        Table t;
        t.SetOn(true, true);
        t.SetEvent(true);
        CHECK_FALSE(HoldMove(t, g, Table::kStallSteps * 2));
    }
    {
        // No life read: nothing to judge by.
        Table t;
        t.SetOn(true, true);
        for (std::uint64_t i = 0; i < Table::kStallSteps * 2; ++i) {
            (void)t.OnActionVerb(kBsa, GameRequest(5, kNode), false, false);
            t.Publish({Dodongo(Role::owner, "Rolling_Loop", std::nullopt)});
            CHECK_FALSE(t.Step(g, false, 0, kMain));
        }
    }
    {
        // A copy: its owner's game judges.
        Table t;
        t.SetOn(true, true);
        t.Publish({Dodongo(Role::copy, "Rolling_Loop", 24)});
        t.Receive(kActor, BossBsa{3, 12, std::nullopt, {}});
        for (std::uint64_t i = 0; i < Table::kStallSteps * 2; ++i) {
            t.Publish({Dodongo(Role::copy, "Rolling_Loop", 24)});
            CHECK_FALSE(t.Step(g, false, 0, kMain));
        }
        CHECK(t.Totals().owner_stalls == 0);
    }
    {
        // Native sync off (the switches): nothing at all.
        Table t;
        for (std::uint64_t i = 0; i < Table::kStallSteps * 2; ++i) {
            t.Publish({Dodongo(Role::owner, "Rolling_Loop", 24)});
            CHECK_FALSE(t.Step(g, false, 0, kMain));
        }
    }
}

TEST_CASE("boss-die-0116: the stall check judges only fight moves: never a death, life 0 or a cutscene") {
    using CoopBossNative::StallJudged;
    // Row 149's moves (King Dodongo's roll, Gohma's ceiling walk, a Barinade spin): judged.
    for (const char* move : {"Rolling_Loop", "Rolling_Start", "CeilingMove", "Climb_All", "Down_Loop", "Spin_Loop",
                             "Attack", "Wait", "AttackEnd_Hit", "Pre_Battle_Ceiling_Wait"}) {
        CAPTURE(std::string(move));
        CHECK(StallJudged(move, 24));
    }
    // Deaths (an own death move before the Director's outro, at any life the revive left) and cuts: never.
    for (const char* not_a_move : {"Die", "Die_Start", "Dead", "Dead_Fall", "Damage_Death", "Death2",
                                   "C01-Enemy_DungeonBoss_Gohma-A-0", "C03_00-Enemy_DungeonBoss_Gohma-A-0",
                                   "AS-C02_00-Enemy_DungeonBoss_Gohma-A-0",
                                   "AS-C10-Dm_FireTemple_BossIn_Enemy_DungeonBoss_Volvagia-A-0",
                                   "Dm_Deku_BossOut_Gohma"}) {
        CAPTURE(std::string(not_a_move));
        CHECK_FALSE(StallJudged(not_a_move, 1));
        CHECK_FALSE(StallJudged(not_a_move, 24));
    }
    // Life 0 whatever plays (a Bongo Bongo hand down), and nothing read.
    CHECK_FALSE(StallJudged("Wait", 0));
    CHECK_FALSE(StallJudged("Wait", -2));
    CHECK_FALSE(StallJudged("Wait", std::nullopt));
    CHECK_FALSE(StallJudged("", 24));
    // Words only as words: a move with them inside another word stays a move.
    CHECK(StallJudged("Diet_Wait", 24));
    CHECK(StallJudged("Undead_Walk", 24));
    CHECK(StallJudged("Cry", 24));
}

TEST_CASE("boss-die-0116: every boss and part held in its death or a cut never chooses again, a live stuck move does") {
    FakeGuest g;
    const std::int32_t brain = 8;
    g.Write(kBsa + addr::kBsaBrainVerb, &brain, 4);
    // Steps of our own boss `name` in one held animation at one life, its AI asking each frame.
    const auto hold = [&](Table& t, std::string_view name, std::string_view animation, std::int32_t life,
                          std::uint64_t steps) {
        std::optional<CoopPuppet::Call> call;
        for (std::uint64_t i = 0; i < steps && !call; ++i) {
            CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(141, kNode), false, false));
            Boss b{kActor, kBsa, Role::owner, std::string(name), 2};
            b.animation = std::string(animation);
            b.life = life;
            t.Publish({b});
            call = t.Step(g, false, 0, kMain);
        }
        return call;
    };
    for (const char* name : {"Enemy_DungeonBoss_Gohma", "Enemy_DungeonBoss_Dodongo", "Enemy_DungeonBoss_Barinade",
                             "Enemy_DungeonBoss_PhantomGanon", "Enemy_DungeonBoss_Volvagia", "Enemy_DungeonBoss_Morpha",
                             "Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_BongoBongo_LeftHand",
                             "Enemy_DungeonBoss_BongoBongo_RightHand", "Enemy_DungeonBoss_Kotake",
                             "Enemy_DungeonBoss_Koume", "Enemy_DungeonBoss_Twinrova", "Enemy_DungeonBoss_Ganon",
                             "Enemy_DungeonBoss_Ganon_SecondHalfBattle"}) {
        CAPTURE(std::string(name));
        Table t;
        t.SetOn(true, true);
        CHECK_FALSE(t.OnBrainVerb(kBsa, brain, false, false));
        // Its death move held at the life the revive left (Gohma's test kill, LANE2 06:47 UTC), at life 0, a cut.
        CHECK_FALSE(hold(t, name, "Die", 1, Table::kStallSteps * 2));
        CHECK_FALSE(hold(t, name, "Dead_Loop", 0, Table::kStallSteps * 2));
        CHECK_FALSE(hold(t, name, "Wait", 0, Table::kStallSteps * 2));
        CHECK_FALSE(hold(t, name, "C01-Enemy_DungeonBoss_Gohma-A-0", 20, Table::kStallSteps * 2));
        CHECK(t.Totals().owner_stalls == 0);
        // The skip hides no live stuck move: the same boss then stuck in a fight move chooses again.
        const auto c = hold(t, name, "Rolling_Loop", 20, Table::kStallSteps + 4);
        REQUIRE(c);
        CHECK(c->x[1] == 8);
        CHECK(t.Totals().owner_stalls == 1);
    }
}

TEST_CASE("native boss: row 149: a copy made ours chooses again unless its own AI ran its body (our stun)") {
    FakeGuest g;
    const std::int32_t brain = 12;
    g.Write(kBsa + addr::kBsaBrainVerb, &brain, 4);
    {
        // Its body ran the owner's roll while its own AI's requests were skipped: once ours, its AI chooses again.
        Table t;
        t.SetOn(true, true);
        t.Publish({Dodongo(Role::copy, "Rolling_Loop", 24)});
        t.Receive(kActor, BossBsa{3, 12, std::nullopt, {}});
        CHECK_FALSE(t.Step(g, false, 0, kMain));
        t.Publish({Dodongo(Role::owner, "Rolling_Loop", 24)});
        auto c = t.Step(g, false, 0, kMain);
        REQUIRE(c);
        CHECK(c->function == kMain + addr::kSetBrainVerb);
        CHECK(c->x[1] == 12);
        // Not made that frame (the runtime could not): asked again, not lost.
        c = t.Step(g, false, 0, kMain);
        REQUIRE(c);
        CHECK(c->function == kMain + addr::kSetBrainVerb);
        CHECK_FALSE(t.OnBrainVerb(kBsa, brain, true, true));
        CHECK_FALSE(t.Step(g, true, 0, kMain));
        CHECK(t.Totals().owner_rechosen == 1);
        CHECK_FALSE(t.Step(g, false, 0, kMain));
    }
    {
        // Handed to us at our stun (our own hit: its own reaction runs its body): left as it is.
        Table t;
        t.SetOn(true, true);
        t.Publish({Dodongo(Role::copy, "Down", 24)});
        t.Receive(kActor, BossBsa{3, 12, std::nullopt, {}});
        CHECK_FALSE(t.Step(g, false, 0, kMain));
        t.NoteOwnHit(kActor);
        t.Publish({Dodongo(Role::owner, "Down", 24)});
        CHECK_FALSE(t.Step(g, false, 0, kMain));
        CHECK(t.Totals().owner_rechosen == 0);
    }
    {
        // Ours from the start (solo, or the first owner): nothing.
        Table t;
        t.SetOn(true, true);
        t.Publish({Dodongo(Role::owner, "Wait", 24)});
        CHECK_FALSE(t.Step(g, false, 0, kMain));
    }
}

TEST_CASE("native boss: row 169: a copy's own stun is its own time, and a hand-over during it keeps the stun") {
    FakeGuest g;
    const std::int32_t brain = 12;
    g.Write(kBsa + addr::kBsaBrainVerb, &brain, 4);
    {
        // King Dodongo swallowed our bomb: his own interrupt laid the copy 'Down' with no hit and no shot read.
        // The stun's DAMAGE of 0 hands him to us: no re-choose (it cut the stun short: 'Down' for good at life 1).
        Table t;
        t.SetOn(true, true);
        t.Publish({Dodongo(Role::copy, "Down", 1)});
        t.Receive(kActor, BossBsa{3, 12, std::nullopt, {}});
        CHECK_FALSE(t.Step(g, false, 0, kMain));
        t.NoteOwnStun(kActor);
        t.NoteOwnStun(kActor);  // (every walk while it lies there: counted once)
        CHECK(t.Totals().copy_own_stuns == 1);
        // Its own time: its own changes run (the stun's end asks its next move).
        CHECK_FALSE(t.OnBrainVerb(kBsa, 13, false, false));
        t.Publish({Dodongo(Role::owner, "Down", 1)});
        CHECK_FALSE(t.Step(g, false, 0, kMain));
        CHECK(t.Totals().owner_rechosen == 0);
    }
    {
        // Not a copy (ours already, or no owner's state yet): nothing.
        Table t;
        t.SetOn(true, true);
        t.Publish({Dodongo(Role::owner, "Down", 1)});
        t.NoteOwnStun(kActor);
        Table u;
        u.SetOn(true, true);
        u.Publish({Dodongo(Role::copy, "Down", 1)});
        u.NoteOwnStun(kActor);
        CHECK(t.Totals().copy_own_stuns == 0);
        CHECK(u.Totals().copy_own_stuns == 0);
    }
    {
        // Without its own stun (the row 149 case) a copy made ours still chooses again.
        Table t;
        t.SetOn(true, true);
        t.Publish({Dodongo(Role::copy, "Rolling_Loop", 24)});
        t.Receive(kActor, BossBsa{3, 12, std::nullopt, {}});
        CHECK_FALSE(t.Step(g, false, 0, kMain));
        t.Publish({Dodongo(Role::owner, "Rolling_Loop", 24)});
        auto c = t.Step(g, false, 0, kMain);
        REQUIRE(c);
        CHECK(c->function == kMain + addr::kSetBrainVerb);
    }
}

TEST_CASE("native boss: row 149: after our hit a copy that ran no move of its own is not asked the owner's again") {
    FakeGuest g;
    const std::int32_t brain = 12;
    g.Write(kBsa + addr::kBsaBrainVerb, &brain, 4);
    const auto roll = ActionFromRequest(GameRequest(5, kNode));
    REQUIRE(roll);
    // The owner's roll put into the copy (its node learnt from its own skipped ask).
    const auto start = [&](Table& t) {
        t.Publish({Dodongo(Role::copy, "Rolling_Loop", 24)});
        CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(8, kNode), false, false));  // (no owner's state: runs)
        t.Receive(kActor, BossBsa{3, 12, roll, {}});
        auto c = t.Step(g, false, 0, kMain);
        REQUIRE(c);
        CHECK(c->function == kMain + addr::kMalloc);
        c = t.Step(g, true, kBlock, kMain);
        REQUIRE(c);
        CHECK(c->function == kMain + addr::kRequestActionVerb);
        CHECK_FALSE(t.Step(g, true, 0, kMain));
        CHECK(t.Totals().action_applied == 1);
    };
    {
        // A sword hit with no stun: its own time passes with no move of its own: the roll goes on, not over.
        Table t;
        t.SetOn(true, true);
        start(t);
        t.NoteOwnHit(kActor);
        for (std::uint64_t i = 0; i <= Table::kReactSteps + 2; ++i) {
            CHECK_FALSE(t.Step(g, false, 0, kMain));
        }
        CHECK(t.Totals().action_applied == 1);
        CHECK(t.Totals().moves_kept == 1);
    }
    {
        // Its own AI ran a move of its own meanwhile (Inhale): the owner's roll is asked again.
        Table t;
        t.SetOn(true, true);
        start(t);
        t.NoteOwnHit(kActor);
        CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(9, kNode), true, false));  // its own runs
        std::optional<CoopPuppet::Call> c;
        for (std::uint64_t i = 0; !c && i <= Table::kReactSteps + 2; ++i) {
            c = t.Step(g, false, 0, kMain);
        }
        REQUIRE(c);
        CHECK(c->function == kMain + addr::kRequestActionVerb);
        CHECK(g.Get<std::int32_t>(kBlock + addr::kReqVerb) == 5);
        CHECK(t.Totals().moves_kept == 0);
    }
    {
        // A new owner whose state names another move: asked.
        Table t;
        t.SetOn(true, true);
        start(t);
        t.Publish({Dodongo(Role::copy, "Rolling_Loop", 24, 3)});
        const auto inhale = ActionFromRequest(GameRequest(9, kNode));
        t.Receive(kActor, BossBsa{4, 12, inhale, {}});
        auto c = t.Step(g, false, 0, kMain);
        REQUIRE(c);
        CHECK(c->function == kMain + addr::kRequestActionVerb);
        CHECK(g.Get<std::int32_t>(kBlock + addr::kReqVerb) == 9);
    }
    {
        // A new owner whose state names the move our copy runs (its old owner's): not asked again.
        Table t;
        t.SetOn(true, true);
        start(t);
        t.Publish({Dodongo(Role::copy, "Rolling_Loop", 24, 3)});
        t.Receive(kActor, BossBsa{4, 12, roll, {}});
        CHECK_FALSE(t.Step(g, false, 0, kMain));
        CHECK(t.Totals().moves_kept == 1);
    }
}

// Row 149, every native boss: the stall check is not King Dodongo's alone. Barinade, Morpha, Bongo Bongo, Volvagia
// and Gohma run the same Table path (their names only label the boss here; the animation names are test names): one
// held move with no hit is re-chosen, a move that changes animation (Barinade's spin cycle, Bongo Bongo's hand
// slams) or a hit taken is never judged stuck.
TEST_CASE("native boss: row 149: Barinade, Morpha, Bongo Bongo, Volvagia and Gohma stuck in one move choose again") {
    const std::int32_t brain = 7;
    for (const char* name : {"Enemy_DungeonBoss_Barinade", "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_BongoBongo",
                             "Enemy_DungeonBoss_Volvagia", "Enemy_DungeonBoss_Gohma"}) {
        CAPTURE(name);
        const auto boss = [name](std::string animation, std::int32_t life) {
            Boss b{kActor, kBsa, Role::owner, name, 2};
            b.animation = std::move(animation);
            b.life = life;
            return b;
        };
        // Steps of one move (its AI asking an action each frame); the call asked, if any.
        const auto hold = [&](Table& t, FakeGuest& g, std::uint64_t steps, auto&& animation_at, auto&& life_at) {
            for (std::uint64_t i = 0; i < steps; ++i) {
                CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(5, kNode), false, false));
                t.Publish({boss(animation_at(i), life_at(i))});
                if (auto c = t.Step(g, false, 0, kMain)) {
                    return std::optional<CoopPuppet::Call>(std::move(c));
                }
            }
            return std::optional<CoopPuppet::Call>{};
        };
        const auto same = [](std::uint64_t) { return std::string("Held_Loop"); };
        const auto full = [](std::uint64_t) { return 24; };
        {
            Table t;
            t.SetOn(true, true);
            FakeGuest g;
            g.Write(kBsa + addr::kBsaBrainVerb, &brain, 4);
            t.Publish({boss("Held_Loop", 24)});
            CHECK_FALSE(t.OnBrainVerb(kBsa, brain, false, false));
            const auto c = hold(t, g, Table::kStallSteps + 4, same, full);
            REQUIRE(c);
            CHECK(c->function == kMain + addr::kSetBrainVerb);
            CHECK(c->x[1] == static_cast<std::uint32_t>(brain));
            CHECK(c->x[2] == 1);
            CHECK(t.Totals().owner_stalls == 1);
        }
        {
            // A move whose animation changes every 2 s (a spin cycle, hand slams): never judged.
            Table t;
            t.SetOn(true, true);
            FakeGuest g;
            g.Write(kBsa + addr::kBsaBrainVerb, &brain, 4);
            t.Publish({boss("Phase_A", 24)});
            CHECK_FALSE(t.OnBrainVerb(kBsa, brain, false, false));
            const auto cycle = [](std::uint64_t i) { return std::string((i / 120) % 2 ? "Phase_B" : "Phase_A"); };
            CHECK_FALSE(hold(t, g, 3 * Table::kStallSteps, cycle, full));
            CHECK(t.Totals().owner_stalls == 0);
        }
        {
            // Hit every 10 s in one held animation (a long legitimate move under fire): never judged.
            Table t;
            t.SetOn(true, true);
            FakeGuest g;
            g.Write(kBsa + addr::kBsaBrainVerb, &brain, 4);
            t.Publish({boss("Held_Loop", 24)});
            CHECK_FALSE(t.OnBrainVerb(kBsa, brain, false, false));
            const auto hits = [](std::uint64_t i) { return 24 - static_cast<std::int32_t>(i / 600); };
            CHECK_FALSE(hold(t, g, 3 * Table::kStallSteps, same, hits));
            CHECK(t.Totals().owner_stalls == 0);
        }
    }
}

TEST_CASE("native boss: row 177, a copy names its own node of the owner's move (Phantom Ganon's appear)") {
    constexpr std::uint64_t kAppearNode = 0x4000100;  // its own AI's node that asks Appear (verb 4)
    constexpr std::uint64_t kRunNode = 0x4000200;     // the one that asks Run (verb 8), its last ask
    constexpr std::uint64_t kPlug = 0x5000000;
    Table t;
    t.SetOn(true, true);
    t.Publish({Boss{kActor, kBsa, Role::copy, "Enemy_DungeonBoss_PhantomGanon", 2}});
    Request appear = GameRequest(4, kAppearNode, false);
    std::memcpy(appear.data() + addr::kReqPlug, &kPlug, 8);
    appear[addr::kReqHasPlug] = 1;
    t.OnActionVerb(kBsa, appear, true, false);  // (no owner's state yet: its own runs)
    t.OnActionVerb(kBsa, GameRequest(8, kRunNode), true, false);
    // The owner's Appear asked through a plug (no place sent), then asked again.
    BossAction a = *ActionFromRequest(GameRequest(4, 0, false));
    a.changed = true;
    t.Receive(kActor, BossBsa{1, 2, a, {}});
    FakeGuest g;
    const std::int32_t verb = 2;
    g.Write(kBsa + addr::kBsaBrainVerb, &verb, 4);
    REQUIRE(t.Step(g, false, 0, kMain));  // the block
    auto c = t.Step(g, true, kBlock, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kRequestActionVerb);
    // Named as its own AI asks Appear: that node and its plug (not Run's node, its last ask).
    CHECK(g.Get<std::uint64_t>(kBlock + addr::kReqNode) == kAppearNode);
    CHECK(g.Get<std::uint64_t>(kBlock + addr::kReqPlug) == kPlug);
    CHECK(g.Get<std::uint8_t>(kBlock + addr::kReqHasPlug) == 1);
    CHECK(g.Get<std::int32_t>(kBlock + addr::kReqVerb) == 4);
}

TEST_CASE("native boss: row 177, the node for a move its own AI never asked: the slot's, else its last") {
    constexpr std::uint64_t kSlotNode = 0x4000300;
    const std::map<std::pair<std::int32_t, std::int32_t>, OwnAsk> by_move{{{0, 8}, OwnAsk{kSlotNode, 0x77, true}}};
    const std::map<std::int32_t, OwnAsk> by_slot{{0, OwnAsk{kSlotNode, 0x77, true}}};
    BossAction a;
    a.slot = 0;
    a.verb = 8;
    REQUIRE(AskFor(by_move, by_slot, kNode, a));
    CHECK(*AskFor(by_move, by_slot, kNode, a) == OwnAsk{kSlotNode, 0x77, true});
    a.verb = 3;  // another move of the slot: its node, never another move's plug
    CHECK(*AskFor(by_move, by_slot, kNode, a) == OwnAsk{kSlotNode});
    a.slot = 1;  // a slot it never asked: its last ask
    CHECK(*AskFor(by_move, by_slot, kNode, a) == OwnAsk{kNode});
    CHECK_FALSE(AskFor({}, {}, 0, a));  // none yet
    // A plug only when the owner's action names no place (its own places come from that plug).
    a.places[0] = coop::protocol::Vec3{1.0f, 2.0f, 3.0f};
    const Request placed = RequestFromAction(a, OwnAsk{kSlotNode, 0x77, true});
    CHECK(placed[addr::kReqHasPlug] == 0);
    a.places[0].reset();
    const Request plugged = RequestFromAction(a, OwnAsk{kSlotNode, 0x77, true});
    CHECK(plugged[addr::kReqHasPlug] == 1);
    std::uint64_t node = 0;
    std::memcpy(&node, plugged.data() + addr::kReqNode, 8);
    CHECK(node == kSlotNode);
}

TEST_CASE("native boss: row 177, the action stream log says each change of kind once, with the run before") {
    Table t;
    t.SetOn(true, true);
    t.Publish({Gohma(Role::owner)});
    t.TakeNotes();
    for (int i = 0; i < 5; ++i) {
        t.OnActionVerb(kBsa, GameRequest(4, kNode), i == 0, false);
    }
    t.OnActionVerb(kBsa, GameRequest(9, kNode), true, false);
    const auto notes = t.TakeNotes();
    const auto has = [&](const std::string& s) {
        return std::any_of(notes.begin(), notes.end(), [&](const std::string& n) { return n.find(s) != std::string::npos; });
    };
    CHECK(has("action stream: ours asks slot 0 verb 4 (changed 1)"));
    CHECK(has("action stream: ours asks slot 0 verb 4 (changed 0) after 1 x ours asks slot 0 verb 4 (changed 1)"));
    CHECK(has("action stream: ours asks slot 0 verb 9 (changed 1) after 4 x ours asks slot 0 verb 4 (changed 0)"));
}

TEST_CASE("native boss: row 177 review, a brain change forgets the copy's kept nodes (its action sequence's go)") {
    constexpr std::uint64_t kAppearNode = 0x4000100;  // its own AI's node that asked Appear (verb 4), then gone
    constexpr std::uint64_t kRunNode = 0x4000200;     // its last ask
    Table t;
    t.SetOn(true, true);
    t.Publish({Boss{kActor, kBsa, Role::copy, "Enemy_DungeonBoss_PhantomGanon", 2}});
    t.OnActionVerb(kBsa, GameRequest(4, kAppearNode), true, false);  // (no owner's state yet: its own runs)
    t.OnActionVerb(kBsa, GameRequest(8, kRunNode), true, false);
    BossAction a = *ActionFromRequest(GameRequest(4, 0));
    a.changed = true;
    t.Receive(kActor, BossBsa{1, 3, a, {}});  // the owner's new brain verb 3 (ours is 2), then Appear
    FakeGuest g;
    const std::int32_t verb = 2;
    g.Write(kBsa + addr::kBsaBrainVerb, &verb, 4);
    bool brain = false;
    std::optional<CoopPuppet::Call> action;
    std::optional<CoopPuppet::Call> c = t.Step(g, false, 0, kMain);
    for (int i = 0; i < 8 && c && !action; ++i) {
        if (c->function == kMain + addr::kSetBrainVerb) {
            brain = true;
        } else if (c->function == kMain + addr::kRequestActionVerb) {
            action = c;
            break;
        }
        c = t.Step(g, true, c->function == kMain + addr::kSetBrainVerb ? 0 : kBlock, kMain);
        if (!c) {
            c = t.Step(g, false, 0, kMain);
        }
    }
    CHECK(brain);
    REQUIRE(action);
    // Appear's node was its old sequence's: not named any more; its last ask stands in until it asks again.
    CHECK(g.Get<std::uint64_t>(kBlock + addr::kReqNode) == kRunNode);
}

namespace {

Boss Phantom(Role role, std::optional<float> y = std::nullopt, std::uint8_t owner = 2) {
    Boss b{kActor, kBsa, role, "Enemy_DungeonBoss_PhantomGanon", owner};
    b.animation = "SpearAttack_Turn";
    b.life = 7;
    if (y) {
        b.position = coop::protocol::Vec3{1.0f, *y, 2.0f};
    }
    return b;
}

} // namespace

TEST_CASE("native boss: row 178: a copy made ours mid-move starts that move over as ours, not a brain re-choice") {
    FakeGuest g;
    const std::int32_t brain = 6;
    g.Write(kBsa + addr::kBsaBrainVerb, &brain, 4);
    auto rush = ActionFromRequest(GameRequest(9, 0));
    REQUIRE(rush);
    rush->flags = 0;  // (the owner's ask as it went on: the start-over bit is ours to add)
    Table t;
    t.SetOn(true, true);
    t.Publish({Phantom(Role::copy)});
    // Its own AI asks the rush itself (skipped, its node kept), then the owner's rush is put in.
    CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(9, kNode), true, false));
    t.Receive(kActor, BossBsa{3, 6, rush, {}});
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kMalloc);
    c = t.Step(g, true, kBlock, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kRequestActionVerb);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    // Its owner goes down: the server hands him to us mid-rush.
    t.Publish({Phantom(Role::owner)});
    g.bytes.erase(kBlock + addr::kReqVerb);  // (the block is written again)
    c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kRequestActionVerb);  // never kSetBrainVerb: the module starts over
    CHECK(c->x[0] == kBsa);
    CHECK(c->x[1] == kBlock);
    CHECK(c->x[2] == 1);
    CHECK(g.Get<std::int32_t>(kBlock + addr::kReqVerb) == 9);
    CHECK((g.Get<std::uint16_t>(kBlock + addr::kReqFlags) & addr::kReqStartOver) != 0);
    CHECK(g.Get<std::uint64_t>(kBlock + addr::kReqNode) == kNode);  // the node its own AI names for the rush
    // Not made that frame: asked again, not lost.
    c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kRequestActionVerb);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    CHECK(t.Totals().owner_restarted == 1);
    CHECK(t.Totals().owner_rechosen == 0);
    CHECK_FALSE(t.Step(g, false, 0, kMain));  // once
    // Our ask goes through the wrap as any of ours: every copy starts the move over too.
    CHECK_FALSE(t.OnActionVerb(kBsa, g.Get<Request>(kBlock), true, true));
    CHECK(t.Totals().owner_action == 1);
}

TEST_CASE("native boss: row 178: a hand-over during our hit or back to a copy restarts nothing") {
    FakeGuest g;
    const std::int32_t brain = 6;
    g.Write(kBsa + addr::kBsaBrainVerb, &brain, 4);
    const auto rush = ActionFromRequest(GameRequest(9, 0));
    REQUIRE(rush);
    const auto copy_in_rush = [&](Table& t) {
        t.Publish({Phantom(Role::copy)});
        CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(9, kNode), true, false));
        t.Receive(kActor, BossBsa{3, 6, rush, {}});
        REQUIRE(t.Step(g, false, 0, kMain));
        REQUIRE(t.Step(g, true, kBlock, kMain));
        CHECK_FALSE(t.Step(g, true, 0, kMain));
    };
    {
        // Our own hit: its own reaction ran its body: left as it is (row 149's rule).
        Table t;
        t.SetOn(true, true);
        copy_in_rush(t);
        t.NoteOwnHit(kActor);
        t.Publish({Phantom(Role::owner)});
        CHECK_FALSE(t.Step(g, false, 0, kMain));
        CHECK(t.Totals().owner_restarted == 0);
    }
    {
        // Made ours, then a copy again before the call was made: nothing asked as a copy's.
        Table t;
        t.SetOn(true, true);
        copy_in_rush(t);
        t.Publish({Phantom(Role::owner)});
        t.Publish({Phantom(Role::copy, std::nullopt, 3)});
        CHECK_FALSE(t.Step(g, false, 0, kMain));
        CHECK(t.Totals().owner_restarted == 0);
    }
    {
        // Actions off (brain only): no move to start over, its AI chooses again as before.
        Table t;
        t.SetOn(true, false);
        t.Publish({Phantom(Role::copy)});
        t.Receive(kActor, BossBsa{3, 6, rush, {}});
        CHECK_FALSE(t.Step(g, false, 0, kMain));
        t.Publish({Phantom(Role::owner)});
        const auto c = t.Step(g, false, 0, kMain);
        REQUIRE(c);
        CHECK(c->function == kMain + addr::kSetBrainVerb);
    }
}

TEST_CASE("native boss: row 178: a boss on the brain path gets no issue-157 animation replay, a plain enemy does") {
    Table t;
    t.Publish({Phantom(Role::copy)});
    CHECK_FALSE(t.RunsOwnMoveWhenOurs(kActor));  // (brain path off: the Mover's old way)
    t.SetOn(true, true);
    t.Publish({Phantom(Role::copy)});
    CHECK(t.RunsOwnMoveWhenOurs(kActor));
    CHECK_FALSE(t.RunsOwnMoveWhenOurs(kActor + 8));  // (not on the path)
    Boss baba{kActor, kBsa, Role::copy, "Enemy_DekuBaba", 2};
    baba.plain = true;
    t.Publish({baba});
    CHECK_FALSE(t.RunsOwnMoveWhenOurs(kActor));
}

TEST_CASE("native boss: row 178: every boss keeps our Link as a replayed hit's attacker, a plain enemy does not") {
    Table t;
    CHECK_FALSE(t.KeepsLinkAsAttacker(kActor));  // (not in the table)
    t.Publish({Phantom(Role::owner)});
    CHECK(t.KeepsLinkAsAttacker(kActor));  // (brain path off too)
    t.SetOn(true, true);
    t.Publish({Phantom(Role::owner)});
    CHECK(t.KeepsLinkAsAttacker(kActor));
    CHECK_FALSE(t.KeepsLinkAsAttacker(kActor + 8));
    Boss baba{kActor, kBsa, Role::owner, "Enemy_DekuBaba", 2};
    baba.plain = true;
    t.Publish({baba});
    CHECK_FALSE(t.KeepsLinkAsAttacker(kActor));
}

TEST_CASE("native boss: row 178: the game's own no-hit state (invincible or every body no-hit) refuses a hit") {
    using CoopBossNative::HitGuard;
    HitGuard g;
    CHECK_FALSE(g.TakesNoHit());  // (unread: nothing known)
    g.read = true;
    CHECK_FALSE(g.TakesNoHit());
    g.invincible_names = 1;  // ExecuteInvincible's name list
    CHECK(g.TakesNoHit());
    g.life_flags = 0x40;  // its damage step lets a hit through then
    CHECK_FALSE(g.TakesNoHit());
    g.life_flags = 0x3f;
    CHECK(g.TakesNoHit());
    g = {};
    g.read = true;
    g.invincible_other = 2;
    CHECK(g.TakesNoHit());
    g.invincible_other = 0;
    g.bodies = 3;
    g.no_hit_bodies = 2;  // one body still takes a hit
    CHECK_FALSE(g.TakesNoHit());
    g.no_hit_bodies = 3;  // OneshotChangeForceNoHitMode on all of them
    CHECK(g.TakesNoHit());
    g.read = false;
    CHECK_FALSE(g.TakesNoHit());
}

TEST_CASE("native boss: row 178: a boss of ours whose game takes no hit now is out of reach of another's hit") {
    HitGuard refused;
    refused.read = true;
    refused.invincible_names = 1;
    const HitGuard takes{true};
    Table t;
    t.SetOn(true, true);
    CHECK_FALSE(t.OutOfReach(kActor, refused));  // (not in the table)
    CHECK_FALSE(t.OurBossMove(kActor));
    Boss riding = Phantom(Role::owner);
    riding.animation = "Run";
    t.Publish({riding});
    CHECK(t.OutOfReach(kActor, refused));
    CHECK_FALSE(t.OutOfReach(kActor, takes));  // the move's name alone says nothing: its game does
    CHECK_FALSE(t.OutOfReach(kActor + 8, refused));
    REQUIRE(t.OurBossMove(kActor));
    CHECK(*t.OurBossMove(kActor) == "Run");
    for (const char* other : {"Enemy_DungeonBoss_Gohma", "Enemy_DungeonBoss_Dodongo", "Enemy_DungeonBoss_Ganondorf"}) {
        Boss b{kActor, kBsa, Role::owner, other, 2};
        t.Publish({b});
        CHECK(t.OutOfReach(kActor, refused));  // every boss of the table, no move list
        CHECK_FALSE(t.OutOfReach(kActor, takes));
    }
    Boss copy = Phantom(Role::copy);
    t.Publish({copy});
    CHECK_FALSE(t.OutOfReach(kActor, refused));  // a copy's hits go to its owner, who judges them
    CHECK_FALSE(t.OurBossMove(kActor));
    Boss baba{kActor, kBsa, Role::owner, "Enemy_DekuBaba", 2};
    baba.plain = true;
    t.Publish({baba});
    CHECK_FALSE(t.OutOfReach(kActor, refused));  // a plain enemy: as before
    t.Publish({riding});
    t.SetDying({"Enemy_DungeonBoss_PhantomGanon"});
    t.Publish({riding});
    CHECK_FALSE(t.OutOfReach(kActor, refused));  // his death: as before
}

TEST_CASE("native boss: row 178: Phantom Ganon and Ganondorf fight the Player only, the puppets are unseen while here") {
    CHECK(Table::FightsPlayerOnly("Enemy_DungeonBoss_PhantomGanon"));
    CHECK(Table::FightsPlayerOnly("Enemy_DungeonBoss_Ganondorf"));
    for (const char* other : {"Enemy_DungeonBoss_Gohma", "Enemy_DungeonBoss_Dodongo", "Enemy_DungeonBoss_Barinade",
                              "Enemy_DungeonBoss_Volvagia", "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_BongoBongo",
                              "Enemy_DungeonBoss_Twinrova", "Enemy_DungeonBoss_Ganon", "Enemy_DekuBaba"}) {
        CHECK_FALSE(Table::FightsPlayerOnly(other));
    }
    Table t;
    CHECK_FALSE(t.PlayerOnlyBoss());  // (no boss)
    t.Publish({Phantom(Role::copy)});
    REQUIRE(t.PlayerOnlyBoss());  // a copy too: its game's perception picks a target as well
    CHECK(*t.PlayerOnlyBoss() == "Enemy_DungeonBoss_PhantomGanon");
    t.SetDying({"Enemy_DungeonBoss_PhantomGanon"});
    CHECK_FALSE(t.PlayerOnlyBoss());  // his death: the puppets are seen again
    t.SetDying({});
    t.Publish({Boss{kActor, kBsa, Role::owner, "Enemy_DungeonBoss_Gohma", 2}});
    CHECK_FALSE(t.PlayerOnlyBoss());  // Gohma still turns to the nearer player (issue 150)
    t.Publish({});
    CHECK_FALSE(t.PlayerOnlyBoss());  // he left our walk (another scene)
}

TEST_CASE("native boss: row 178: our boss fallen far below its fight start chooses again, once per fall") {
    FakeGuest g;
    const std::int32_t brain = 6;
    g.Write(kBsa + addr::kBsaBrainVerb, &brain, 4);
    Table t;
    t.SetOn(true, true);
    t.Publish({Phantom(Role::owner, 10.0f)});
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    // A dive within its room: nothing.
    t.Publish({Phantom(Role::owner, 10.0f - Table::kFallBelowStart + 1.0f)});
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    // Out of its room: its brain verb set again through the game's own function.
    t.Publish({Phantom(Role::owner, -40.0f)});
    auto c = t.Step(g, false, 0, kMain);
    REQUIRE(c);
    CHECK(c->function == kMain + addr::kSetBrainVerb);
    CHECK(c->x[1] == 6);
    CHECK_FALSE(t.Step(g, true, 0, kMain));
    CHECK(t.Totals().owner_fell == 1);
    // Still falling: once per fall.
    t.Publish({Phantom(Role::owner, -400.0f)});
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    CHECK(t.Totals().owner_fell == 1);
    // Back up, fallen again: again, up to kMaxFallRechoices; then left.
    for (int i = 1; i < Table::kMaxFallRechoices; ++i) {
        t.Publish({Phantom(Role::owner, 10.0f)});
        t.Publish({Phantom(Role::owner, -40.0f)});
        REQUIRE(t.Step(g, false, 0, kMain));
        CHECK_FALSE(t.Step(g, true, 0, kMain));
    }
    t.Publish({Phantom(Role::owner, 10.0f)});
    t.Publish({Phantom(Role::owner, -40.0f)});
    CHECK_FALSE(t.Step(g, false, 0, kMain));
    CHECK(t.Totals().owner_rechosen == static_cast<std::uint64_t>(Table::kMaxFallRechoices));
    {
        // A copy below (the owner's place): its owner's game guards it.
        Table c2;
        c2.SetOn(true, true);
        c2.Publish({Phantom(Role::copy, 10.0f)});
        c2.Publish({Phantom(Role::copy, -400.0f)});
        CHECK(c2.Totals().owner_fell == 0);
    }
    {
        // No place read (solo walk without one): nothing.
        Table s;
        s.SetOn(true, true);
        s.Publish({Phantom(Role::owner)});
        s.Publish({Phantom(Role::owner)});
        CHECK_FALSE(s.Step(g, false, 0, kMain));
        CHECK(s.Totals().owner_fell == 0);
    }
}

TEST_CASE("native boss: boss turns: the attributes of the BSA's action verbs, as the game's own method reads them") {
    // main+0x507be0: five action slots' s32 verbs from +0x54, each verb's u64 at +0x1a40 + 16 x verb + 8 (below 282).
    CHECK(addr::ActionVerbAt(kBsa, 0) == kBsa + 0x54);
    CHECK(addr::ActionVerbAt(kBsa, 4) == kBsa + 0x64);
    CHECK(addr::ActionAttributesAt(kBsa, 0) == kBsa + 0x1a40 + 8);
    CHECK(addr::ActionAttributesAt(kBsa, 103) == kBsa + 0x1a40 + 103 * 16 + 8);
    CHECK(addr::ActionAttributesAt(kBsa, 281) == kBsa + 0x1a40 + 281 * 16 + 8);
    CHECK(addr::ActionAttributesAt(kBsa, 282) == kBsa + 0x1a40 + 8);  // (past the table: the first entry)
    CHECK(addr::ActionAttributesAt(kBsa, -1) == kBsa + 0x1a40 + 8);
    // The brain verb table (63 x 16 from +0x1650, bit 0 Battle) ends where the action verb table starts.
    CHECK(0x1650 + 63 * 16 == addr::kBsaActionAttributes);
    // Attack is attribute 0 (AttributeFlag 1; 513 = Attack and attribute 9 together).
    CHECK(addr::IsAttack(1));
    CHECK(addr::IsAttack(513));
    CHECK_FALSE(addr::IsAttack(0));
    CHECK_FALSE(addr::IsAttack(2));  // Interrupt alone
    CHECK(TurnsSwitch(""));
    CHECK(TurnsSwitch("1"));
    CHECK_FALSE(TurnsSwitch("0"));
}

TEST_CASE("native boss: boss turns: a copy made ours between moves resumes, mid-attack or unread it restarts as before") {
    using CoopBossNative::MadeOurs;
    CHECK(PlanMadeOurs(true, false, true, true, true) == MadeOurs::resume);
    CHECK(PlanMadeOurs(true, false, true, true, false) == MadeOurs::resume);  // (its own ask is the same move)
    CHECK(PlanMadeOurs(true, true, true, true, true) == MadeOurs::restart_move);  // mid-attack: row 178
    CHECK(PlanMadeOurs(true, std::nullopt, true, true, true) == MadeOurs::restart_move);  // not read
    CHECK(PlanMadeOurs(true, false, true, false, true) == MadeOurs::restart_move);  // its own AI is elsewhere
    CHECK(PlanMadeOurs(true, false, false, false, false) == MadeOurs::rechoose);   // row 149
    CHECK(PlanMadeOurs(false, false, true, true, true) == MadeOurs::restart_move);  // turns off: as before
    CHECK(PlanMadeOurs(false, false, false, false, true) == MadeOurs::rechoose);
}

TEST_CASE("native boss: boss turns: handed to us between moves, its own brain goes on (no brain verb, no start over)") {
    FakeGuest g;
    const std::int32_t brain = 6;
    g.Write(kBsa + addr::kBsaBrainVerb, &brain, 4);
    auto fly = ActionFromRequest(GameRequest(7, 0));
    REQUIRE(fly);
    fly->flags = 0;
    const auto copy_in_fly = [&](Table& t) {
        t.SetOn(true, true);
        t.SetTurns(true);
        t.Publish({Phantom(Role::copy)});
        // Its own AI asks the same move (Fly) itself; the owner's Fly is put in.
        CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(7, kNode), true, false));
        t.Receive(kActor, BossBsa{3, 6, fly, {}});
        REQUIRE(t.Step(g, false, 0, kMain));
        REQUIRE(t.Step(g, true, kBlock, kMain));
        CHECK_FALSE(t.Step(g, true, 0, kMain));
    };
    {
        Table t;
        copy_in_fly(t);
        Boss ours = Phantom(Role::owner);
        ours.attacking = false;  // its game: the move its body runs is no attack
        t.Publish({ours});
        CHECK_FALSE(t.Step(g, false, 0, kMain));  // nothing asked: its own AI goes on from its phase and move
        CHECK(t.Totals().owner_restarted == 0);
        CHECK(t.Totals().owner_rechosen == 0);
        // Its own next ask is ours now: it goes through and is sent to every copy.
        CHECK_FALSE(t.OnActionVerb(kBsa, GameRequest(103, kNode), true, true));
        CHECK(t.Totals().owner_action == 1);
    }
    {
        // The same hand-over mid-attack: the row 178 start-over, unchanged.
        Table t;
        copy_in_fly(t);
        Boss ours = Phantom(Role::owner);
        ours.attacking = true;
        t.Publish({ours});
        g.bytes.erase(kBlock + addr::kReqVerb);
        const auto c = t.Step(g, false, 0, kMain);
        REQUIRE(c);
        CHECK(c->function == kMain + addr::kRequestActionVerb);
        CHECK((g.Get<std::uint16_t>(kBlock + addr::kReqFlags) & addr::kReqStartOver) != 0);
    }
}

TEST_CASE("native boss: boss turns: another player's hit names their puppet on a boss, not on Phantom Ganon or Ganondorf") {
    Table t;
    t.SetOn(true, true);
    t.SetTurns(true);
    t.Publish({Phantom(Role::owner)});
    CHECK(t.KeepsLinkAsAttacker(kActor));  // fights the Player only (row 178): our Link still
    Boss gohma{kActor, kBsa, Role::owner, "Enemy_DungeonBoss_Gohma", 2};
    t.Publish({gohma});
    CHECK_FALSE(t.KeepsLinkAsAttacker(kActor));  // the hitter's puppet: the game's own aggro
    t.SetTurns(false);
    CHECK(t.KeepsLinkAsAttacker(kActor));  // off: as before
}
