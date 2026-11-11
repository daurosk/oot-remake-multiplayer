// Tests for the event hold (lichii/hook/coop_guard.h, issue 144).
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
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

TEST_CASE("guard: two held hurts both land, and a hurt behind a written life becomes that life") {
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
    // A hurt then a written life: the written life.
    held.Pass({{0x100, 3, 1}}, talking, Where, Who);
    held.Pass({{0x100, 0, 0}}, talking, Where, Who);
    r = held.Pass({}, Local{false, Vec3{0, 0, 0}}, Where, Who);
    REQUIRE(r.apply.size() == 1);
    CHECK(r.apply[0].life == 0);
    CHECK(r.apply[0].hurt == 0);
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
