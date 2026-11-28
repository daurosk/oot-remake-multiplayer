// Tests for the fortress carpenters' count (lichii/hook/coop_carpenters.h, row 333).
#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "coop_carpenters.h"
#include "coop_story.h"
#include "coop/protocol/codec.hpp"

namespace {

struct Game {
    std::map<std::uint32_t, bool> bools;
    std::map<std::uint32_t, std::int32_t> ints{{CoopCarpenters::kHelped, 0}, {CoopCarpenters::kHelpedLeft, 4}};
    Game() {
        for (const auto& c : CoopCarpenters::kCellFlags) {
            bools[c.after_help] = false;
            bools[c.warp_clear] = false;
        }
        bools[CoopCarpenters::kCardScene] = false;
    }
    CoopCarpenters::ReadBool flag() const {
        return [this](std::uint32_t h) -> std::optional<bool> {
            const auto it = bools.find(h);
            return it == bools.end() ? std::nullopt : std::optional<bool>(it->second);
        };
    }
    CoopCarpenters::ReadInt value() const {
        return [this](std::uint32_t h) -> std::optional<std::int32_t> {
            const auto it = ints.find(h);
            return it == ints.end() ? std::nullopt : std::optional<std::int32_t>(it->second);
        };
    }
    void Free(int cell) {  // another player's rescue: its shared flags only, no count of ours
        bools[CoopCarpenters::kCellFlags[cell].after_help] = true;
        bools[CoopCarpenters::kCellFlags[cell].warp_clear] = true;
    }
};

}  // namespace

TEST_CASE("carpenters: the flag hashes are the game's names and the shared rescue's") {
    namespace p = coop::protocol;
    CHECK(CoopCarpenters::kHelped == p::flag_hash("GerudoFortress_Meet_FourCarpenter_NumOfCarpenterHelped"));
    CHECK(CoopCarpenters::kHelpedLeft == p::flag_hash("GerudoFortress_Meet_FourCarpenter_NumOfCarpenterHelped_02"));
    CHECK(CoopCarpenters::kFirstPlace == p::flag_hash("GerudoFortress_Npc_Gerudo_001_IsInFirstPlace"));
    CHECK(CoopCarpenters::kCardScene == p::flag_hash("HavePlayedEvent.Meet_FourCarpenter_ClearEvent"));
    for (int n = 0; n < CoopCarpenters::kCells; ++n) {
        const std::string npc = "Npc_Hylia_01" + std::to_string(3 + n) + "_Adult";
        const auto& c = CoopCarpenters::kCellFlags[n];
        CHECK(c.after_help == p::flag_hash("GerudoFortress_IsAfterHelpCarpenter_" + npc));
        CHECK(c.warp_clear == p::flag_hash("GerudoFortress_WarpPoint0" + std::to_string(n + 1) + "_ClearEvent"));
        // Both are shared (report 99): the count can follow them in every game.
        CHECK(CoopStory::Find(c.after_help) != nullptr);
        CHECK(CoopStory::Find(c.warp_clear) != nullptr);
    }
    CHECK(CoopStory::Find(CoopCarpenters::kCardScene) != nullptr);
    // The counts stay each game's own: they follow the shared flags, never sent (no double count).
    CHECK(CoopStory::Find(CoopCarpenters::kHelped) == nullptr);
    CHECK(CoopStory::Find(CoopCarpenters::kHelpedLeft) == nullptr);
}

TEST_CASE("carpenters: a rescue another player did counts here (row 333)") {
    Game host;
    host.Free(0);  // the client freed the first carpenter
    auto f = CoopCarpenters::FollowRescues(host.flag(), host.value(), false);
    CHECK(f.helped == 1);
    CHECK(f.left == 3);
    host.ints[CoopCarpenters::kHelped] = 1;
    host.ints[CoopCarpenters::kHelpedLeft] = 3;
    f = CoopCarpenters::FollowRescues(host.flag(), host.value(), false);
    CHECK_FALSE(f.helped);  // already right: nothing written
    CHECK_FALSE(f.left);
    // Split rescues: two by the host (its own adds), two by the client: the host is at 4, as the game needs for the
    // card scene's Helped > 3; the last rescuer's game reaches 4 by its own add from 3.
    host.Free(1);
    host.Free(2);
    f = CoopCarpenters::FollowRescues(host.flag(), host.value(), false);
    CHECK(f.helped == 3);
    CHECK(f.left == 1);
}

TEST_CASE("carpenters: never in an event, never without a save, and a count above the rescues comes down") {
    Game g;
    g.Free(0);
    CHECK_FALSE(CoopCarpenters::FollowRescues(g.flag(), g.value(), true).helped);  // our own rescue adds in its event
    Game none;
    none.bools.erase(CoopCarpenters::kCellFlags[3].warp_clear);  // unread: nothing
    CHECK_FALSE(CoopCarpenters::FollowRescues(none.flag(), none.value(), false).helped);
    Game twice;  // the same cell freed in two games at once: both added 1
    twice.Free(0);
    twice.ints[CoopCarpenters::kHelped] = 2;
    twice.ints[CoopCarpenters::kHelpedLeft] = 2;
    const auto f = CoopCarpenters::FollowRescues(twice.flag(), twice.value(), false);
    CHECK(f.helped == 1);
    CHECK(f.left == 3);
    // One end flag of a cell is enough (an old save with only one of them).
    Game half;
    half.bools[CoopCarpenters::kCellFlags[2].warp_clear] = true;
    CHECK(CoopCarpenters::Rescued(half.flag()) == 1);
}

TEST_CASE("carpenters: a stuck save gets the card scene once, in the fortress, out of events, after the wait") {
    Game g;
    for (int n = 0; n < CoopCarpenters::kCells; ++n) g.Free(n);
    CoopCarpenters::Mender m;
    const std::uint64_t t0 = 1'000'000;
    const std::uint64_t wait = CoopCarpenters::kStuckUs;
    CHECK_FALSE(m.Tick(g.flag(), "HyruleField", false, t0));             // not in the fortress
    CHECK_FALSE(m.Tick(g.flag(), "GerudoFortress", false, t0));           // the wait starts
    CHECK_FALSE(m.Tick(g.flag(), "GerudoFortress", false, t0 + wait - 1));
    CHECK_FALSE(m.Tick(g.flag(), "GerudoFortress", true, t0 + wait));     // an event runs: the wait starts again
    CHECK_FALSE(m.Tick(g.flag(), "GerudoFortress_Adult", false, t0 + wait + 1));
    CHECK(m.Tick(g.flag(), "GerudoFortress_Adult", false, t0 + 2 * wait + 1));
    CHECK_FALSE(m.Tick(g.flag(), "GerudoFortress_Adult", false, t0 + 3 * wait));  // asked once
    // Played: never again. Three rescued: nothing (the 4th rescue plays it itself).
    g.bools[CoopCarpenters::kCardScene] = true;
    CHECK_FALSE(m.Tick(g.flag(), "GerudoFortress", false, t0 + 9 * wait));
    Game three;
    for (int n = 0; n < 3; ++n) three.Free(n);
    CoopCarpenters::Mender m3;
    CHECK_FALSE(m3.Tick(three.flag(), "GerudoFortress", false, t0));
    CHECK_FALSE(m3.Tick(three.flag(), "GerudoFortress", false, t0 + 2 * wait));
}
