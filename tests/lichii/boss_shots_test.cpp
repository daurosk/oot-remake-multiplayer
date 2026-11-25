// SPDX-License-Identifier: GPL-3.0-or-later
// Row 222 part 3: a copy boss throws what its owner's boss prepared (coop_boss_shots.h).
#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "coop_boss_shots.h"

using namespace CoopBossShots;

namespace {

constexpr std::uint64_t kMain = 0x80020000;
constexpr std::uint64_t kTwinrova = 0x1000000;
constexpr std::uint64_t kKoume = 0x1100000;
constexpr std::uint64_t kOther = 0x1200000;  // an enemy that is no shared boss
constexpr std::uint32_t kFire = 0x1111aaaa;  // Twinrova_Bullet_Fire's hash (any)
constexpr std::uint32_t kIce = 0x2222bbbb;
constexpr std::uint32_t kUnknown = 0x3333cccc;  // a shootable the shooter does not hold

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
    // The actor's shooter, as the game links it: [[actor + 0x270] + 0x3d8].
    std::uint64_t Shooter(std::uint64_t actor) {
        const std::uint64_t components = actor + 0x10000;
        const std::uint64_t shooter = actor + 0x20000;
        Write(actor + addr::kActorComponents, &components, sizeof components);
        Write(components + addr::kComponentShooter, &shooter, sizeof shooter);
        return shooter;
    }
    std::map<std::uint64_t, std::uint8_t> bytes;
};

// One game: its memory, our table (the wrap) and what the game's own function main+0x374d20 was run with.
struct Game {
    FakeGuest guest;
    Table table;
    std::vector<std::pair<std::uint64_t, BossShot>> ran;  // (shooter, {0, hash, num}) in order

    Reader Read() {
        return [this](std::uint64_t a, void* out, std::size_t n) { return guest.Read(a, out, n); };
    }
    // A call of the game's function, through the wrap as the module's hos_coop_wrap does. Returns its x0.
    std::uint64_t Call(std::uint64_t shooter, std::uint32_t hash, std::uint32_t num, bool own_call = false) {
        if (table.OnRequest(shooter, hash, num, own_call, Read())) {
            return 1;  // skipped
        }
        ran.push_back({shooter, BossShot{0, hash, static_cast<std::uint8_t>(num)}});
        const bool found = Asks(hash, num) && hash != kUnknown;
        table.OnRan(shooter, hash, num, found, own_call, Read());
        return found ? 1 : 0;
    }
    // One frame of our caller, as the runtime does; returns how many calls it made.
    int Frame() {
        int made = 0;
        auto call = table.Step(guest, false, 0, kMain);
        while (call) {
            REQUIRE(call->function == kMain + addr::kRequestCreate);
            const std::uint64_t result = Call(call->x[0], static_cast<std::uint32_t>(call->x[1]),
                                              static_cast<std::uint32_t>(call->x[2]), true);
            ++made;
            call = table.Step(guest, true, result, kMain);
        }
        return made;
    }
};

Member Boss(std::uint64_t actor, Role role, std::uint8_t owner, const char* name = "Enemy_Twinrova") {
    return Member{actor, role, owner, name};
}

// The owner's samples reach the copy (coop_objects.cpp: Publish, then the owner's list).
void Sync(Game& owner, Game& copy, std::uint64_t actor = kTwinrova) {
    if (const auto shots = owner.table.OwnerShots(actor)) {
        copy.table.Receive(actor, *shots);
    }
}

}  // namespace

TEST_CASE("the function's no-ops are never kept") {
    CHECK(Asks(kFire, 0xff));
    CHECK(Asks(kFire, 0x101));
    CHECK_FALSE(Asks(0, 1));
    CHECK_FALSE(Asks(kFire, 0));
    CHECK_FALSE(Asks(kFire, 0x100));  // the game tests the low byte
}

TEST_CASE("the planner: every request newer than the last one queued, a start takes the whole list") {
    const BossShots owner{9, {BossShot{7, kFire, 1}, BossShot{8, kIce, 2}, BossShot{9, kFire, 0xff}}};
    std::uint32_t lost = 7;
    CHECK(Plan(owner, 9u, &lost).empty());
    CHECK(lost == 0);
    CHECK(Plan(owner, 12u, &lost).empty());
    auto plan = Plan(owner, 7u, &lost);
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].seq == 8);
    CHECK(plan[1].seq == 9);
    CHECK(lost == 0);
    plan = Plan(owner, 3u, &lost);  // 4, 5, 6 are no longer in the list
    CHECK(plan.size() == 3);
    CHECK(lost == 3);
    CHECK(Plan(owner, std::nullopt, &lost) == owner.recent);
    CHECK(lost == 0);
    CHECK(Plan(BossShots{4, {}}, std::nullopt).empty());
}

TEST_CASE("row 222, run 20261124-161320: the copy's shooter gets the owner's requests, not its own AI's") {
    Game owner, copy;
    const std::uint64_t so = owner.guest.Shooter(kTwinrova);
    const std::uint64_t sc = copy.guest.Shooter(kTwinrova);
    owner.table.SetOn(true);
    copy.table.SetOn(true);
    const std::vector<Member> ours{Boss(kTwinrova, Role::owner, 2)};
    const std::vector<Member> theirs{Boss(kTwinrova, Role::copy, 2)};
    owner.table.Publish(ours);
    copy.table.Publish(theirs);
    // From the start the owner's samples carry its (empty) list: the copy knows its owner shares them.
    REQUIRE(owner.table.OwnerShots(kTwinrova));
    CHECK(owner.table.OwnerShots(kTwinrova)->seq == 0);
    Sync(owner, copy);
    CHECK(copy.Frame() == 0);

    // Attack 1: the owner's AI chose Fire; the copy's own AI chose Ice for the same move.
    CHECK(owner.Call(so, kFire, 0xff) == 1);
    CHECK(copy.Call(sc, kIce, 0xff) == 1);  // skipped, answered as found
    CHECK(copy.ran.empty());                // (before the fix: Ice was prepared and the Fire throw had no bullet)
    Sync(owner, copy);
    CHECK(copy.Frame() == 1);
    REQUIRE(copy.ran.size() == 1);
    CHECK(copy.ran[0].first == sc);
    CHECK(copy.ran[0].second == BossShot{0, kFire, 0xff});

    // Attack 2: both chose Ice. The copy's own is skipped all the same: the owner's is made once, not twice.
    CHECK(owner.Call(so, kIce, 0xff) == 1);
    CHECK(copy.Call(sc, kIce, 0xff) == 1);
    Sync(owner, copy);
    CHECK(copy.Frame() == 1);
    Sync(owner, copy);  // the same list again (every sample carries it): nothing new
    CHECK(copy.Frame() == 0);
    REQUIRE(copy.ran.size() == 2);
    CHECK(copy.ran[1].second == BossShot{0, kIce, 0xff});

    // Attack 3: the copy's own AI is not in the attack at all (no own request). Before the fix: nothing prepared.
    CHECK(owner.Call(so, kIce, 2) == 1);
    Sync(owner, copy);
    CHECK(copy.Frame() == 1);
    REQUIRE(copy.ran.size() == 3);
    CHECK(copy.ran[2].second == BossShot{0, kIce, 2});

    // Two requests in one owner frame (a bullet and its field): both, in the owner's order, in one frame.
    owner.Call(so, kFire, 1);
    owner.Call(so, kIce, 1);
    Sync(owner, copy);
    CHECK(copy.Frame() == 2);
    REQUIRE(copy.ran.size() == 5);
    CHECK(copy.ran[3].second.hash == kFire);
    CHECK(copy.ran[4].second.hash == kIce);

    const Counts oc = owner.table.GetCounts();
    const Counts cc = copy.table.GetCounts();
    CHECK(oc.owner_kept == 5);
    CHECK(cc.delivered == 5);
    CHECK(cc.copy_skipped == 2);
    CHECK(cc.copy_own_ran == 0);
    CHECK(cc.dropped == 0);
    // The owner's game ran exactly its own five; our caller's calls on the copy were not kept as the copy's.
    CHECK(owner.ran.size() == 5);
    CHECK_FALSE(copy.table.OwnerShots(kTwinrova));
}

TEST_CASE("without the relay (off, or an older owner) a copy's own requests run, as before") {
    Game copy;
    const std::uint64_t sc = copy.guest.Shooter(kTwinrova);
    // Off (solo, or a hookshot.exe without the wrap): nothing is touched.
    CHECK(copy.Call(sc, kIce, 0xff) == 1);
    CHECK(copy.ran.size() == 1);
    copy.table.SetOn(true);
    const std::vector<Member> theirs{Boss(kTwinrova, Role::copy, 2)};
    copy.table.Publish(theirs);
    // An owner that sends no list (an older build): the copy's own AI prepares its own, the old behaviour.
    CHECK(copy.Call(sc, kIce, 0xff) == 1);
    CHECK(copy.ran.size() == 2);
    CHECK(copy.table.GetCounts().copy_own_ran == 1);
    CHECK(copy.table.GetCounts().copy_skipped == 0);
}

TEST_CASE("only shared bosses' shooters: any other shooter's requests run and are not kept") {
    Game g;
    const std::uint64_t boss = g.guest.Shooter(kTwinrova);
    const std::uint64_t other = g.guest.Shooter(kOther);
    g.table.SetOn(true);
    const std::vector<Member> ours{Boss(kTwinrova, Role::owner, 1)};
    g.table.Publish(ours);
    CHECK(g.Call(other, kFire, 1) == 1);  // Link's bow, an Octorok, ...
    CHECK(g.Call(0, kFire, 1) == 1);
    CHECK(g.ran.size() == 2);
    CHECK(g.table.OwnerShots(kTwinrova)->seq == 0);
    // Ours: kept only when the function found the shootable and did something.
    CHECK(g.Call(boss, kUnknown, 1) == 0);
    CHECK(g.Call(boss, kFire, 0) == 0);
    CHECK(g.Call(boss, 0, 1) == 0);
    CHECK(g.table.OwnerShots(kTwinrova)->seq == 0);
    CHECK(g.Call(boss, kFire, 0x1ff) == 1);
    const auto mine = g.table.OwnerShots(kTwinrova);
    REQUIRE(mine->recent.size() == 1);
    CHECK(mine->recent[0] == BossShot{1, kFire, 0xff});
    CHECK_FALSE(g.table.OwnerShots(kOther));
}

TEST_CASE("each boss part has its own list (Koume and Kotake, Phantom Ganon, Ganondorf: any shared boss)") {
    Game owner, copy;
    const std::uint64_t so_t = owner.guest.Shooter(kTwinrova);
    const std::uint64_t so_k = owner.guest.Shooter(kKoume);
    const std::uint64_t sc_t = copy.guest.Shooter(kTwinrova);
    const std::uint64_t sc_k = copy.guest.Shooter(kKoume);
    owner.table.SetOn(true);
    copy.table.SetOn(true);
    const std::vector<Member> ours{Boss(kTwinrova, Role::owner, 1), Boss(kKoume, Role::owner, 1, "Enemy_Koume")};
    const std::vector<Member> theirs{Boss(kTwinrova, Role::copy, 1), Boss(kKoume, Role::copy, 1, "Enemy_Koume")};
    owner.table.Publish(ours);
    copy.table.Publish(theirs);
    Sync(owner, copy, kTwinrova);
    Sync(owner, copy, kKoume);
    owner.Call(so_k, kFire, 1);
    owner.Call(so_t, kIce, 1);
    Sync(owner, copy, kTwinrova);
    Sync(owner, copy, kKoume);
    CHECK(copy.Frame() == 2);
    REQUIRE(copy.ran.size() == 2);
    for (const auto& [shooter, shot] : copy.ran) {
        CHECK(shot.hash == (shooter == sc_k ? kFire : kIce));
        CHECK((shooter == sc_k || shooter == sc_t));
    }
}

TEST_CASE("a hand-over: the new owner's AI asks, the new copy starts with the new owner's fresh list") {
    Game a, b;
    const std::uint64_t sa = a.guest.Shooter(kTwinrova);
    const std::uint64_t sb = b.guest.Shooter(kTwinrova);
    a.table.SetOn(true);
    b.table.SetOn(true);
    std::vector<Member> ma{Boss(kTwinrova, Role::copy, 2)};
    std::vector<Member> mb{Boss(kTwinrova, Role::owner, 2)};
    a.table.Publish(ma);
    b.table.Publish(mb);
    Sync(b, a);
    b.Call(sb, kFire, 1);
    b.Call(sb, kIce, 1);
    Sync(b, a);
    CHECK(a.Frame() == 2);
    b.Call(sb, kFire, 1);  // sent, but the hand-over comes before A makes it
    Sync(b, a);
    // B's Link enters an event: the server hands Twinrova to A (player 1), mid-move.
    ma = {Boss(kTwinrova, Role::owner, 1)};
    mb = {Boss(kTwinrova, Role::copy, 1)};
    a.table.Publish(ma);
    b.table.Publish(mb);
    CHECK(a.Frame() == 0);  // the old owner's queued request is dropped: A's own AI decides now
    CHECK(a.table.GetCounts().dropped == 1);
    REQUIRE(a.table.OwnerShots(kTwinrova));
    CHECK(a.table.OwnerShots(kTwinrova)->recent.empty());
    CHECK_FALSE(b.table.OwnerShots(kTwinrova));
    // A restarts the move as its own and asks at once; B's first list from A already holds it: made on B.
    CHECK(a.Call(sa, kIce, 0xff) == 1);
    CHECK(a.ran.size() == 3);
    b.ran.clear();
    CHECK(b.Call(sb, kFire, 1) == 1);  // B's own, before A's list came: runs (as with an older owner)
    Sync(a, b);
    CHECK(b.Call(sb, kFire, 1) == 1);  // skipped from then on
    CHECK(b.Frame() == 1);
    REQUIRE(b.ran.size() == 2);
    CHECK(b.ran[1].second == BossShot{0, kIce, 0xff});
    // A's count (1) is below what B had queued from... nothing: B was the owner. And back again: B owns, A copies;
    // B's list starts empty, its count goes on from 3, A starts with it.
    ma = {Boss(kTwinrova, Role::copy, 2)};
    mb = {Boss(kTwinrova, Role::owner, 2)};
    a.table.Publish(ma);
    b.table.Publish(mb);
    CHECK(b.table.OwnerShots(kTwinrova)->seq == 3);
    CHECK(b.table.OwnerShots(kTwinrova)->recent.empty());
    Sync(b, a);
    CHECK(a.Frame() == 0);
    b.Call(sb, kFire, 1);
    Sync(b, a);
    a.ran.clear();
    CHECK(a.Frame() == 1);
    CHECK(a.ran[0].second.hash == kFire);
}

TEST_CASE("the owner's list holds only fresh requests: a copy that starts later makes nothing old") {
    Game owner, late;
    const std::uint64_t so = owner.guest.Shooter(kTwinrova);
    late.guest.Shooter(kTwinrova);
    owner.table.SetOn(true);
    late.table.SetOn(true);
    const std::vector<Member> ours{Boss(kTwinrova, Role::owner, 1)};
    const std::vector<Member> theirs{Boss(kTwinrova, Role::copy, 1)};
    owner.table.Publish(ours);
    owner.Call(so, kFire, 1);
    for (std::uint64_t i = 0; i < Table::kFreshRounds; ++i) {
        owner.table.Publish(ours);
    }
    CHECK(owner.table.OwnerShots(kTwinrova)->recent.size() == 1);  // still fresh
    owner.table.Publish(ours);
    const auto mine = owner.table.OwnerShots(kTwinrova);
    CHECK(mine->recent.empty());
    CHECK(mine->seq == 1);  // the count goes on
    // A player joins: its copy's first list is empty; the next request is made.
    late.table.Publish(theirs);
    Sync(owner, late);
    CHECK(late.Frame() == 0);
    owner.Call(so, kIce, 1);
    Sync(owner, late);
    CHECK(late.Frame() == 1);
    CHECK(late.ran[0].second.hash == kIce);
    // A join during a move: the fresh request in the first list is made (the move it prepares is still playing).
    Game mid;
    mid.guest.Shooter(kTwinrova);
    mid.table.SetOn(true);
    mid.table.Publish(theirs);
    Sync(owner, mid);
    CHECK(mid.Frame() == 1);
    CHECK(mid.ran[0].second.hash == kIce);
}

TEST_CASE("more than the list holds, a lost sample, a missing shooter, an unknown shootable, a new game") {
    Game owner, copy;
    const std::uint64_t so = owner.guest.Shooter(kTwinrova);
    const std::uint64_t sc = copy.guest.Shooter(kTwinrova);
    owner.table.SetOn(true);
    copy.table.SetOn(true);
    const std::vector<Member> ours{Boss(kTwinrova, Role::owner, 1)};
    const std::vector<Member> theirs{Boss(kTwinrova, Role::copy, 1)};
    owner.table.Publish(ours);
    copy.table.Publish(theirs);
    Sync(owner, copy);
    // The copy misses samples while the owner asks more than the list holds: the newest are made, the rest counted.
    for (std::size_t i = 0; i < coop::protocol::kMaxBossShots + 3; ++i) {
        owner.Call(so, kFire, static_cast<std::uint32_t>(i + 1));
    }
    const auto mine = owner.table.OwnerShots(kTwinrova);
    CHECK(mine->recent.size() == coop::protocol::kMaxBossShots);
    CHECK(mine->recent.front().seq == 4);
    Sync(owner, copy);
    CHECK(copy.Frame() == static_cast<int>(coop::protocol::kMaxBossShots));
    CHECK(copy.table.GetCounts().dropped == 3);
    CHECK(copy.ran.front().second.num == 4);
    // A shootable the copy's shooter does not hold: asked, counted, the next one still made.
    owner.Call(so, kIce, 1);
    Sync(owner, copy);
    copy.ran.clear();
    // (the copy's game answers 0 for this one)
    auto call = copy.table.Step(copy.guest, false, 0, kMain);
    REQUIRE(call);
    CHECK(call->x[0] == sc);
    CHECK(call->x[1] == kIce);
    CHECK(call->x[2] == 1);
    CHECK_FALSE(copy.table.Step(copy.guest, true, 0, kMain));
    CHECK(copy.table.GetCounts().missed == 1);
    // A call the runtime did not make this frame stays queued and is asked again.
    owner.Call(so, kFire, 1);
    Sync(owner, copy);
    REQUIRE(copy.table.Step(copy.guest, false, 0, kMain));
    CHECK(copy.Frame() == 1);
    CHECK(copy.Frame() == 0);
    // No shooter on the copy (its components are gone): dropped, no call.
    owner.Call(so, kFire, 1);
    Sync(owner, copy);
    const std::uint64_t none = 0;
    copy.guest.Write(kTwinrova + addr::kActorComponents, &none, sizeof none);
    const std::uint64_t dropped = copy.table.GetCounts().dropped;
    CHECK(copy.Frame() == 0);
    CHECK(copy.table.GetCounts().dropped == dropped + 1);
    // A new game process: everything of the old one is dropped.
    copy.guest.Shooter(kTwinrova);
    owner.Call(so, kFire, 1);
    Sync(owner, copy);
    CHECK_FALSE(copy.table.Step(copy.guest, false, 0, kMain + 0x1000));
    CHECK(copy.table.GetCounts().delivered == coop::protocol::kMaxBossShots + 1);
    // Off: nothing kept, nothing skipped.
    owner.table.SetOn(false);
    CHECK_FALSE(owner.table.OwnerShots(kTwinrova));
    owner.ran.clear();
    owner.Call(so, kFire, 1);
    CHECK(owner.ran.size() == 1);
}
