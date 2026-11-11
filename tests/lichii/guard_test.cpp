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
    CoopGuard::Held<Change> held;
    const Local talking{true, Vec3{0, 0, 0}};
    // A's kill of the scrub (0x100) and a hurt to the far enemy (0x200) arrive mid-talk.
    auto now = held.Pass({{0x100, 0}, {0x200, 5}}, talking, Where);
    REQUIRE(now.size() == 1);
    CHECK(now[0].address == 0x200);
    CHECK(held.Size() == 1);
    // A newer change to the same actor replaces the held one; an unknown actor waits too.
    now = held.Pass({{0x100, 1}, {0x300, 7}}, talking, Where);
    CHECK(now.empty());
    CHECK(held.Size() == 2);
    // The talk ends: the held changes first, in the order they came, then the new ones.
    now = held.Pass({{0x200, 4}}, Local{false, Vec3{0, 0, 0}}, Where);
    REQUIRE(now.size() == 3);
    CHECK(now[0].address == 0x100);
    CHECK(now[0].value == 1);
    CHECK(now[1].address == 0x300);
    CHECK(now[2].address == 0x200);
    CHECK(held.Size() == 0);
    CHECK(held.HeldCount() == 3);
    // A scene change drops what waits (those actors are gone).
    held.Pass({{0x100, 0}}, talking, Where);
    held.Forget();
    CHECK(held.Pass({}, Local{false, std::nullopt}, Where).empty());
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
