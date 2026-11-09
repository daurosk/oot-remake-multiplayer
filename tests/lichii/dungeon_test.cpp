// Tests for the shared dungeon flags (lichii/hook/coop_dungeon.cpp).
#include <doctest/doctest.h>

#include <set>
#include <string>

#include "coop_dungeon.h"

namespace host = coop::host;

TEST_CASE("dungeon: every flag's hash is the hash of its name, once") {
    std::set<std::uint32_t> seen;
    std::size_t ints = 0;
    for (const auto& f : CoopDungeon::Flags()) {
        CHECK_MESSAGE(host::flag_hash(f.name) == f.hash, f.name);
        CHECK(seen.insert(f.hash).second);
        ints += f.is_int ? 1 : 0;
    }
    CHECK(CoopDungeon::Flags().size() > 700);
    CHECK(ints > 10);
}

TEST_CASE("dungeon: the Water Temple's water level and switches are in the list") {
    bool level = false;
    bool shutter = false;
    for (const auto& f : CoopDungeon::Flags()) {
        level |= std::string(f.name) == "WaterTemple_Room00_WaterLevel" && f.is_int;
        shutter |= std::string(f.name) == "WaterTemple_Room00_Switch_NearTheShutter";
        // One player's own state is not shared.
        CHECK(std::string(f.name).find("NaviHelp") == std::string::npos);
        CHECK(std::string(f.name).find("IsPlayerIn") == std::string::npos);
    }
    CHECK(level);
    CHECK(shutter);
}

TEST_CASE("dungeon: Merge adds to the list, makes one when there is none, or does nothing when off") {
    std::optional<host::SharedFlags> none;
    CHECK(CoopDungeon::Merge(none, false) == 0);
    CHECK_FALSE(none.has_value());

    const std::size_t n = CoopDungeon::Merge(none, true);
    REQUIRE(none.has_value());
    CHECK(n == CoopDungeon::Flags().size());
    CHECK(none->contains(host::FlagKey(host::flag_hash("WaterTemple_Room00_WaterLevel"), 0)));

    // An existing list keeps its own flags; a flag already there counts once.
    const auto mine = host::FlagKey(0x12345678, 0);
    const auto dup = host::FlagKey(CoopDungeon::Flags()[0].hash, 0);
    std::optional<host::SharedFlags> some = host::SharedFlags({mine, dup});
    CHECK(CoopDungeon::Merge(some, true) == CoopDungeon::Flags().size() - 1);
    CHECK(some->contains(mine));
    CHECK(some->size() == CoopDungeon::Flags().size() + 1);
}
