// Tests for the event hold (lichii/hook/coop_guard.h, issue 144).
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "coop_guard.h"

using CoopGuard::Local;
using CoopGuard::Vec3;

namespace {

struct Change {
    std::uint64_t address = 0;
    int value = 0;
};

// A life write as coop_objects_sync.h has it (for MergeLife).
struct Life {
    std::uint64_t address = 0;
    std::int32_t life = 0;
    std::int32_t hurt = 0;
};

// Who is at each address in the walk; a test changes it to reuse an address.
std::map<std::uint64_t, std::uint64_t> who{{0x100, 0xa}, {0x200, 0xb}, {0x300, 0xc}};
std::optional<std::uint64_t> Who(std::uint64_t a) {
    const auto it = who.find(a);
    if (it == who.end()) return std::nullopt;
    return it->second;
}

// The Deku Scrub of the report at 0x100, 2 m from our Link; another enemy at 0x200, 30 m away.
std::optional<Vec3> Where(std::uint64_t a) {
    if (a == 0x100) return Vec3{2.0f, 0.0f, 0.0f};
    if (a == 0x200) return Vec3{30.0f, 0.0f, 0.0f};
    return std::nullopt;
}

} // namespace

TEST_CASE("guard: near is a plain distance, unknown or broken places are never near") {
    CHECK(CoopGuard::Near(Vec3{0, 0, 0}, Vec3{2, 0, 2}, 3.0f));
    CHECK_FALSE(CoopGuard::Near(Vec3{0, 0, 0}, Vec3{3, 0, 0}, 3.0f));
    CHECK_FALSE(CoopGuard::Near(std::nullopt, Vec3{0, 0, 0}, 3.0f));
    CHECK_FALSE(CoopGuard::Near(Vec3{0, 0, 0}, Vec3{NAN, 0, 0}, 3.0f));
    CHECK(CoopGuard::kChestNearM < CoopGuard::kEventNearM);
}

TEST_CASE("guard: during our event, what is near our Link (or of unknown place) waits") {
    const Local idle{false, Vec3{0, 0, 0}};
    const Local talking{true, Vec3{0, 0, 0}};
    CHECK_FALSE(CoopGuard::Waits(idle, Where(0x100)));
    CHECK(CoopGuard::Waits(talking, Where(0x100)));
    CHECK_FALSE(CoopGuard::Waits(talking, Where(0x200)));
    CHECK(CoopGuard::Waits(talking, std::nullopt));
    CHECK(CoopGuard::Waits(Local{true, std::nullopt}, Where(0x200)));  // Link's place unknown
}

TEST_CASE("guard: a kill of the scrub we talk to waits for the talk's end, others go now (#144)") {
    who = {{0x100, 0xa}, {0x200, 0xb}, {0x300, 0xc}};
    CoopGuard::Held<Change> held;
    const Local talking{true, Vec3{0, 0, 0}};
    // A's kill of the scrub (0x100) and a hurt to the far enemy (0x200) arrive mid-talk.
    auto r = held.Pass({{0x100, 0}, {0x200, 5}}, talking, Where, Who);
    REQUIRE(r.apply.size() == 1);
    CHECK(r.apply[0].address == 0x200);
    CHECK(r.released == 0);
    CHECK(held.Size() == 1);
    // A newer change to the same actor replaces the held one (no merge given); an actor of
    // unknown place (0x300) waits too.
    r = held.Pass({{0x100, 1}, {0x300, 7}}, talking, Where, Who);
    CHECK(r.apply.empty());
    CHECK(held.Size() == 2);
    // The talk ends: the held changes first, in the order they came, then the new ones.
    r = held.Pass({{0x200, 4}}, Local{false, Vec3{0, 0, 0}}, Where, Who);
    REQUIRE(r.apply.size() == 3);
    CHECK(r.released == 2);
    CHECK(r.dropped == 0);
    CHECK(r.apply[0].address == 0x100);
    CHECK(r.apply[0].value == 1);
    CHECK(r.apply[1].address == 0x300);
    CHECK(r.apply[2].address == 0x200);
    CHECK(held.Size() == 0);
    CHECK(held.HeldCount() == 3);
    // A scene change drops what waits (those actors are gone) and says how many.
    held.Pass({{0x100, 0}}, talking, Where, Who);
    CHECK(held.Forget() == 1);
    CHECK(held.Pass({}, Local{false, std::nullopt}, Where, Who).apply.empty());
}

TEST_CASE("guard: a held change is dropped when its address holds another actor at the event's end") {
    who = {{0x100, 0xa}, {0x200, 0xb}};
    CoopGuard::Held<Change> held;
    const Local talking{true, Vec3{0, 0, 0}};
    held.Pass({{0x100, 0}}, talking, Where, Who);
    // During a long event the scrub goes and another actor is made at the same address.
    who[0x100] = 0xdead;
    auto r = held.Pass({}, Local{false, Vec3{0, 0, 0}}, Where, Who);
    CHECK(r.apply.empty());
    CHECK(r.released == 0);
    CHECK(r.dropped == 1);
    // Gone with nothing in its place: dropped too.
    who = {{0x200, 0xb}};
    held.Pass({{0x100, 0}}, talking, Where, Who);  // held with no actor known there
    r = held.Pass({}, Local{false, Vec3{0, 0, 0}}, Where, Who);
    CHECK(r.apply.empty());
    CHECK(r.dropped == 1);
    // The same actor still there: made.
    who = {{0x100, 0xa}};
    held.Pass({{0x100, 3}}, talking, Where, Who);
    r = held.Pass({}, Local{false, Vec3{0, 0, 0}}, Where, Who);
    REQUIRE(r.apply.size() == 1);
    CHECK(r.apply[0].value == 3);
    CHECK(r.released == 1);
}

TEST_CASE("guard: two held hurts both land, a hurt behind a written life becomes that life, a written life behind a hurt keeps the hurt") {
    who = {{0x100, 0xa}};
    CoopGuard::Held<Life> held{[](Life& kept, Life&& newer) { CoopGuard::MergeLife(kept, std::move(newer)); }};
    const Local talking{true, Vec3{0, 0, 0}};
    held.Pass({{0x100, 3, 1}}, talking, Where, Who);  // hit: 4 -> 3
    held.Pass({{0x100, 2, 1}}, talking, Where, Who);  // hit: 3 -> 2
    auto r = held.Pass({}, Local{false, Vec3{0, 0, 0}}, Where, Who);
    REQUIRE(r.apply.size() == 1);
    CHECK(r.apply[0].life == 2);
    CHECK(r.apply[0].hurt == 2);
    // A written life (a kill to 0) then a hurt: the hurt's target, written.
    held.Pass({{0x100, 1, 0}}, talking, Where, Who);
    held.Pass({{0x100, 0, 1}}, talking, Where, Who);
    r = held.Pass({}, Local{false, Vec3{0, 0, 0}}, Where, Who);
    REQUIRE(r.apply.size() == 1);
    CHECK(r.apply[0].life == 0);
    CHECK(r.apply[0].hurt == 0);
    // A hurt then a written life (row 172: the Sync's own fallback for that hurt, which was never
    // given): the hurt stays a hurt, so it lands as the replayed hit, aiming at the lower life.
    held.Pass({{0x100, 3, 1}}, talking, Where, Who);
    held.Pass({{0x100, 3, 0}}, talking, Where, Who);
    r = held.Pass({}, Local{false, Vec3{0, 0, 0}}, Where, Who);
    REQUIRE(r.apply.size() == 1);
    CHECK(r.apply[0].life == 3);
    CHECK(r.apply[0].hurt == 1);
    held.Pass({{0x100, 3, 1}}, talking, Where, Who);
    held.Pass({{0x100, 0, 0}}, talking, Where, Who);
    r = held.Pass({}, Local{false, Vec3{0, 0, 0}}, Where, Who);
    REQUIRE(r.apply.size() == 1);
    CHECK(r.apply[0].life == 0);
    CHECK(r.apply[0].hurt == 4);
    // A hurt then a higher written life (a raise): the written life.
    held.Pass({{0x100, 1, 1}}, talking, Where, Who);
    held.Pass({{0x100, 4, 0}}, talking, Where, Who);
    r = held.Pass({}, Local{false, Vec3{0, 0, 0}}, Where, Who);
    REQUIRE(r.apply.size() == 1);
    CHECK(r.apply[0].life == 4);
    CHECK(r.apply[0].hurt == 0);
}

// A life write with the fields of coop_objects_sync.h's LifeWrite that MergeLife reads.
struct FullLife {
    std::uint64_t address = 0;
    std::int32_t life = 0;
    std::int32_t hurt = 0;
    std::uint8_t hitter = 0;
    bool direct = false;
    bool heal = false;
    bool test_kill = false;
};

TEST_CASE("guard: a held kill keeps its hitter through the Sync's fallback write, a heal, a direct give or a test kill still win (row 172)") {
    who = {{0x100, 0xa}};
    CoopGuard::Held<FullLife> held{[](FullLife& kept, FullLife&& newer) { CoopGuard::MergeLife(kept, std::move(newer)); }};
    const Local dying{true, Vec3{0, 0, 0}};  // our Link's own death event, the Little 2 m away
    const Local after{false, Vec3{0, 0, 0}};
    // Another player's killing blow (2 -> 0, P2), then 400 ms later the Sync's plain write of that target.
    held.Pass({FullLife{0x100, 0, 2, 2}}, dying, Where, Who);
    held.Pass({FullLife{0x100, 0, 0, 0}}, dying, Where, Who);
    auto r = held.Pass({}, after, Where, Who);
    REQUIRE(r.apply.size() == 1);
    CHECK(r.released == 1);
    CHECK(r.apply[0].hurt == 2);
    CHECK(r.apply[0].hitter == 2);
    CHECK(r.apply[0].life == 0);
    // Not a plain write: the newer one wins as before.
    for (const FullLife newer : {FullLife{0x100, 0, 2, 0, true}, FullLife{0x100, 2, 0, 0, false, true},
                                 FullLife{0x100, 0, 0, 0, false, false, true}}) {
        held.Pass({FullLife{0x100, 0, 2, 2}}, dying, Where, Who);
        FullLife copy = newer;
        held.Pass({std::move(copy)}, dying, Where, Who);
        r = held.Pass({}, after, Where, Who);
        REQUIRE(r.apply.size() == 1);
        CHECK(r.apply[0].direct == newer.direct);
        CHECK(r.apply[0].heal == newer.heal);
        CHECK(r.apply[0].test_kill == newer.test_kill);
        CHECK(r.apply[0].hitter == 0);
    }
}

TEST_CASE("guard: removes, places and plays near our Link are left out while the event runs") {
    std::vector<std::uint64_t> removes{0x100, 0x200};
    const auto where = [](std::uint64_t a) { return Where(a); };
    CHECK(CoopGuard::Skip(removes, Local{true, Vec3{0, 0, 0}}, where, [](std::uint64_t a) { return a; }) == 1);
    CHECK(removes == std::vector<std::uint64_t>{0x200});
    removes = {0x100, 0x200};
    CHECK(CoopGuard::Skip(removes, Local{false, Vec3{0, 0, 0}}, where, [](std::uint64_t a) { return a; }) == 0);
    CHECK(removes.size() == 2);
    std::vector<Change> plays{{0x100, 1}};
    CoopGuard::Skip(plays, Local{true, Vec3{0, 0, 0}}, where, [](const Change& c) { return c.address; });
    CHECK(plays.empty());
}

TEST_CASE("guard: a world object another game moves follows during our event (#194)") {
    // 0x100 is near our Link (as above): it waits, unless it is a world object.
    const auto where = [](std::uint64_t a) { return Where(a); };
    const auto world = [](std::uint64_t a) { return a == 0x100; };
    std::vector<std::uint64_t> poses{0x100, 0x200};
    CHECK(CoopGuard::Skip(poses, Local{true, Vec3{0, 0, 0}}, where, [](std::uint64_t a) { return a; }, world) == 0);
    CHECK(poses.size() == 2);
    // Something else near (not a world object) still waits.
    const auto none = [](std::uint64_t) { return false; };
    poses = {0x100, 0x200};
    CHECK(CoopGuard::Skip(poses, Local{true, Vec3{0, 0, 0}}, where, [](std::uint64_t a) { return a; }, none) == 1);
    CHECK(poses == std::vector<std::uint64_t>{0x200});
}

TEST_CASE("guard: who is at an address tells apart same-named game-made copies by object id") {
    constexpr std::uint64_t kNoHash = ~std::uint64_t{0};
    const auto keese1 = CoopGuard::Who(kNoHash, "Enemy_Keese", 7);
    const auto keese2 = CoopGuard::Who(kNoHash, "Enemy_Keese", 8);
    CHECK(keese1 != keese2);
    CHECK(keese1 == CoopGuard::Who(kNoHash, "Enemy_Keese", 7));
    CHECK(CoopGuard::Who(kNoHash, "Enemy_Keese", std::nullopt) != keese1);
    CHECK(CoopGuard::Who(0x11, "Enemy_Keese", 7) != keese1);
    CHECK(CoopGuard::Who(kNoHash, "Enemy_Keese_B", 7) != keese1);
    // In Held: a held kill of Keese 7 is dropped when Keese 8 has its address at the end.
    who = {{0x100, keese1}};
    CoopGuard::Held<Change> held;
    held.Pass({{0x100, 0}}, Local{true, Vec3{0, 0, 0}}, Where, Who);
    who = {{0x100, keese2}};
    const auto r = held.Pass({}, Local{false, Vec3{0, 0, 0}}, Where, Who);
    CHECK(r.apply.empty());
    CHECK(r.dropped == 1);
}

TEST_CASE("guard: an actor our event is with waits at any distance (row 97)") {
    who = {{0x100, 0xa}, {0x200, 0xb}};
    // Our event is with the far actor 0x200 (30 m): a scrub whose entrance it waits on.
    Local talking{true, Vec3{0, 0, 0}, [](std::uint64_t a) { return a == 0x200; }};
    CHECK(CoopGuard::WaitsFor(talking, 0x200, Where(0x200)));
    CHECK(CoopGuard::WaitsFor(talking, 0x100, Where(0x100)));        // near: as before
    CHECK_FALSE(CoopGuard::WaitsFor(talking, 0x300, Vec3{50, 0, 0})); // far and not in it
    talking.in_event = false;
    CHECK_FALSE(CoopGuard::WaitsFor(talking, 0x200, Where(0x200)));   // no event: nothing waits
    // A kill of it from another game waits for our event's end too.
    CoopGuard::Held<Change> held;
    talking.in_event = true;
    auto r = held.Pass({{0x200, 0}}, talking, Where, Who);
    CHECK(r.apply.empty());
    CHECK(held.Size() == 1);
    talking.in_event = false;
    r = held.Pass({}, talking, Where, Who);
    REQUIRE(r.apply.size() == 1);
    CHECK(r.apply[0].address == 0x200);
    // Skip (places, plays, removes made every frame) leaves it out while the event runs.
    talking.in_event = true;
    std::vector<std::uint64_t> removes{0x200, 0x300};
    const std::function<std::optional<Vec3>(std::uint64_t)> where = [](std::uint64_t a) -> std::optional<Vec3> {
        return a == 0x300 ? std::optional<Vec3>(Vec3{50, 0, 0}) : Where(a);
    };
    CHECK(CoopGuard::Skip(removes, talking, where, [](std::uint64_t a) { return a; }) == 1);
    REQUIRE(removes.size() == 1);
    CHECK(removes[0] == 0x300);
}

TEST_CASE("guard: another player's event here holds only its own actors, nothing near our Link (G1, issue 251)") {
    who = {{0x100, 0xa}, {0x200, 0xb}};
    // Our game plays another player's event; it binds the far actor 0x200. Our Link is not in it.
    Local replay{false, Vec3{0, 0, 0}, [](std::uint64_t a) { return a == 0x200; }, true};
    CHECK(CoopGuard::WaitsFor(replay, 0x200, Where(0x200)));         // the event's actor waits
    CHECK_FALSE(CoopGuard::WaitsFor(replay, 0x100, Where(0x100)));   // near our Link: goes through
    CHECK_FALSE(CoopGuard::Waits(replay, Where(0x100)));
    // Held: a kill of the event's actor waits, one of the near actor is made at once.
    CoopGuard::Held<Change> held;
    auto r = held.Pass({{0x200, 0}, {0x100, 0}}, replay, Where, Who);
    REQUIRE(r.apply.size() == 1);
    CHECK(r.apply[0].address == 0x100);
    CHECK(held.Size() == 1);
    replay.others_event = false;  // the event ended
    r = held.Pass({}, replay, Where, Who);
    REQUIRE(r.apply.size() == 1);
    CHECK(r.apply[0].address == 0x200);
    // Skip: only the event's actor is left out while it runs.
    replay.others_event = true;
    std::vector<std::uint64_t> removes{0x100, 0x200};
    CHECK(CoopGuard::Skip(removes, replay, Where, [](std::uint64_t a) { return a; }) == 1);
    REQUIRE(removes.size() == 1);
    CHECK(removes[0] == 0x100);
}
