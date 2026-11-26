// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <map>
#include <string>

#include "coop_presence_gates.h"

using CoopPresenceGates::Gates;
using CoopPresenceGates::Probe;

namespace {
constexpr std::uint32_t kWaterLevel01 = 0xa16f6696;  // WaterTemple_WaterLevelChange01 (coop_dungeon.cpp)
}

TEST_CASE("presence gates: the Water Temple's water planes are gated by its shared water level flag (row 233)") {
    bool planes = false;
    for (const auto& g : Gates()) {
        if (g.flag == kWaterLevel01 && std::string(g.actor) == "Obj_WaterTemple_WaterPlane_A_02") {
            planes = true;
            CHECK(g.negation);
            CHECK(std::string(g.area) == "WaterTemple");
            CHECK(std::string(g.flag_name) == "WaterTemple_WaterLevelChange01");
        }
    }
    CHECK(planes);
}

TEST_CASE("presence gates: a change of a gate's flag is logged now and 3 s later, with the pieces our game has") {
    std::map<std::uint32_t, bool> flags;
    std::map<std::string, std::size_t> actors{{"Obj_WaterTemple_WaterPlane_A_02", 1}};
    const auto read = [&](std::uint32_t h) -> std::optional<bool> {
        const auto it = flags.find(h);
        return it == flags.end() ? std::nullopt : std::optional(it->second);
    };
    const auto count = [&](std::string_view n) {
        const auto it = actors.find(std::string(n));
        return it == actors.end() ? std::size_t{0} : it->second;
    };
    Probe probe;
    flags[kWaterLevel01] = false;
    CHECK(probe.Step(1'000'000, read, count).empty());  // the first read: nothing to compare
    CHECK(probe.Step(2'000'000, read, count).empty());  // no change
    flags[kWaterLevel01] = true;
    CHECK(probe.Step(2'500'000, read, count).empty());  // (read once a second)
    auto lines = probe.Step(3'000'000, read, count);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("WaterTemple_WaterLevelChange01 (WaterTemple) went 1, now") != std::string::npos);
    CHECK(lines[0].find("Obj_WaterTemple_WaterPlane_A_02 unwanted (1 of 1 here)") != std::string::npos);
    actors.clear();  // the game took it away
    CHECK(probe.Step(4'000'000, read, count).empty());
    lines = probe.Step(6'000'000, read, count);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("3 s later") != std::string::npos);
    CHECK(lines[0].find("Obj_WaterTemple_WaterPlane_A_02 unwanted (0 of 1 here)") != std::string::npos);
    probe.Reset();  // a new scene
    flags[kWaterLevel01] = false;
    CHECK(probe.Step(7'000'000, read, count).empty());
}
