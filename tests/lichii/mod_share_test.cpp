// SPDX-License-Identifier: GPL-3.0-or-later
// Mods in co-op (lichii/hook/coop_mod_share.*): how a guest's mod list is
// compared with the host's, and the host's values handed to the loader.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "coop_mod_share.h"

using namespace coop::protocol;

namespace {
ModEntry Mod(const char* id, const char* version, std::uint8_t hash = 0) {
    ModEntry e{id, version, {}};
    e.hash[0] = hash;
    return e;
}
}  // namespace

TEST_CASE("mod share: the same mods in any order match, and hashes not known yet are not compared") {
    const ModList host{1, {Mod("rando", "1.0", 5), Mod("hud", "2", 7)}};
    CHECK(CoopModShare::Compare(host, ModList{0, {Mod("hud", "2", 7), Mod("rando", "1.0", 5)}}).empty());
    CHECK(CoopModShare::Compare(host, ModList{0, {Mod("hud", "2", 0), Mod("rando", "1.0", 5)}}).empty());
}

TEST_CASE("mod share: missing, extra, another version and other files each get a line") {
    const ModList host{1, {Mod("rando", "1.0", 5), Mod("hud", "2", 7), Mod("tunic", "", 1)}};
    const ModList mine{0, {Mod("rando", "1.1", 5), Mod("hud", "2", 8), Mod("camera", "3", 2)}};
    CHECK(CoopModShare::Compare(host, mine) ==
          std::vector<std::string>{"rando: you have 1.1, the host 1.0", "hud 2: your files differ from the host's",
                                   "missing tunic (the host has it)", "extra camera 3 (the host does not have it)"});
    CHECK(CoopModShare::Compare(host, mine, 2) ==
          std::vector<std::string>{"rando: you have 1.1, the host 1.0", "hud 2: your files differ from the host's",
                                   "and 2 more"});
    CHECK(CoopModShare::Compare(ModList{}, ModList{0, {Mod("x", "")}}) ==
          std::vector<std::string>{"extra x (the host does not have it)"});
}

TEST_CASE("mod share: with no session the loader is told at once there are no host values") {
    std::vector<int> told;
    CoopModShare::Provider p;
    p.host_values = [&](std::optional<std::vector<ModSetting>> v) { told.push_back(v ? 1 : 0); };
    CoopModShare::SetProvider(p);
    CoopModShare::Frame(nullptr);
    CoopModShare::Frame(nullptr);
    // Nothing came from a host before the first frame: told once, "none"; never again until something changes.
    CHECK(told.size() <= 1);
    CHECK(CoopModShare::TakeLines().empty());
    CHECK_FALSE(CoopModShare::Now().connected);
    CoopModShare::SetProvider({});
}

TEST_CASE("mod share: a player without the host's randomizer seed is found, and the lines name the player and seed (#59)") {
    CHECK(CoopModShare::IsSeedId("randomizer-n2bteykm"));
    CHECK_FALSE(CoopModShare::IsSeedId("randomizer-"));
    CHECK_FALSE(CoopModShare::IsSeedId("oot-remake-randomizer"));
    CHECK_FALSE(CoopModShare::IsSeedId("randomizer-N2BTEYKM"));  // ids are in lower case
    const ModList host{1, {Mod("hud", "2", 7), Mod("randomizer-n2bteykm", "0", 5)}};
    CHECK(CoopModShare::SeedMissing(host, ModList{2, {Mod("hud", "2", 7)}}) == "N2BTEYKM");
    CHECK(CoopModShare::SeedMissing(host, ModList{2, {}}) == "N2BTEYKM");
    CHECK(CoopModShare::SeedMissing(host, ModList{2, {Mod("randomizer-n2bteykm", "0", 5)}}).empty());
    CHECK(CoopModShare::SeedMissing(host, ModList{2, {Mod("randomizer-other111", "0", 5)}}) == "N2BTEYKM");
    CHECK(CoopModShare::SeedMissing(ModList{1, {Mod("hud", "2", 7)}}, ModList{2, {}}).empty());  // no seed: nothing
    const std::string line = CoopModShare::SeedlessLine(2, "Rev", "N2BTEYKM");
    CHECK(line.find("player 2 (Rev) runs WITHOUT the randomizer seed N2BTEYKM") != std::string::npos);
    CHECK(line.size() <= 200);
    CHECK(CoopModShare::SeedlessLine(3, "", "X").find("player 3 runs WITHOUT") != std::string::npos);
    CHECK(CoopModShare::SeedlessMineLine("N2BTEYKM").find("you run WITHOUT the host's randomizer seed N2BTEYKM") !=
          std::string::npos);
    CHECK(CoopModShare::SeedBackLine(2, "Rev") == "Mods: player 2 (Rev) runs the host's randomizer seed now");
}

TEST_CASE("mod share: the host's seed built into other files (another kit's OOTRR: another world) is found and said") {
    const ModList host{1, {Mod("hud", "2", 7), Mod("randomizer-n2bteykm", "0", 5)}};
    CHECK(CoopModShare::SeedFilesOther(host, ModList{2, {Mod("randomizer-n2bteykm", "0", 6)}}) == "N2BTEYKM");
    CHECK(CoopModShare::SeedFilesOther(host, ModList{2, {Mod("randomizer-n2bteykm", "0", 5)}}).empty());
    CHECK(CoopModShare::SeedFilesOther(host, ModList{2, {Mod("randomizer-n2bteykm", "0", 0)}}).empty());  // not hashed yet
    CHECK(CoopModShare::SeedFilesOther(ModList{1, {Mod("randomizer-n2bteykm", "0", 0)}},
                                       ModList{2, {Mod("randomizer-n2bteykm", "0", 6)}})
              .empty());
    CHECK(CoopModShare::SeedFilesOther(host, ModList{2, {}}).empty());  // missing: SeedMissing says it
    CHECK(CoopModShare::SeedFilesOther(host, ModList{2, {Mod("hud", "2", 9)}}).empty());  // not a seed
    const std::string line = CoopModShare::SeedOtherLine(2, "Rev-the-long-name", "N2BTEYKM");
    CHECK(line.find("player 2 (Rev-the-long-name) has the randomizer seed N2BTEYKM built unlike the host's") !=
          std::string::npos);
    CHECK(line.find("another OOTRR") != std::string::npos);
    CHECK(line.size() <= 200);
    const std::string mine = CoopModShare::SeedOtherMineLine("N2BTEYKM");
    CHECK(mine.find("update to the host's kit") != std::string::npos);
    CHECK(mine.size() <= 200);
}

namespace {
ModEntry Marked(const char* id, std::uint8_t hash) {
    ModEntry e = Mod(id, "0", hash);
    CoopModShare::MarkHashWay(e.hash);
    return e;
}
}  // namespace

TEST_CASE("mod share: hashes carry how they were made, and two ways are not compared as files (row 274)") {
    ModHash h{};
    h[0] = 5;
    CHECK(CoopModShare::HashWay(h) == 1);  // unmarked: a kit before 0.11.13
    CoopModShare::MarkHashWay(h);
    CHECK(CoopModShare::HashWay(h) == CoopModShare::kHashWay);
    CHECK(CoopModShare::kHashWay == 2);
    CHECK(h[0] == 5);
    // The same way: other files are other files (another OOTRR: another world).
    const ModList host{1, {Marked("randomizer-n2bteykm", 5), Marked("hud", 7)}};
    CHECK(CoopModShare::SeedFilesOther(host, ModList{2, {Marked("randomizer-n2bteykm", 6)}}) == "N2BTEYKM");
    CHECK(CoopModShare::SeedFilesOther(host, ModList{2, {Marked("randomizer-n2bteykm", 5)}}).empty());
    std::string seed = "untouched";
    CHECK(CoopModShare::SeedKitOther(host, ModList{2, {Marked("randomizer-n2bteykm", 6)}}, &seed) == 0);
    CHECK(seed == "untouched");
    // A guest on an older kit (unmarked hash): not 'built unlike', but which side is older.
    const ModList old_guest{2, {Mod("randomizer-n2bteykm", "0", 5), Mod("hud", "0", 7)}};
    CHECK(CoopModShare::SeedFilesOther(host, old_guest).empty());
    CHECK(CoopModShare::SeedKitOther(host, old_guest, &seed) < 0);
    CHECK(seed == "N2BTEYKM");
    // A host on an older kit: the guest (newer) is told the host's is older.
    const ModList old_host{1, {Mod("randomizer-n2bteykm", "0", 5)}};
    CHECK(CoopModShare::SeedKitOther(old_host, ModList{2, {Marked("randomizer-n2bteykm", 5)}}, &seed) > 0);
    CHECK(CoopModShare::SeedKitOther(old_host, ModList{2, {Mod("randomizer-n2bteykm", "0", 0)}}, &seed) == 0);
    // The Mods tab: one line for the kits, not one 'your files differ' per mod.
    CHECK(CoopModShare::Compare(host, old_guest) ==
          std::vector<std::string>{
              "your kit is older than the host's: your mod files can't be checked against its (update to its kit)"});
    CHECK(CoopModShare::Compare(ModList{1, {Mod("hud", "0", 7)}}, ModList{2, {Marked("hud", 8)}}) ==
          std::vector<std::string>{
              "the host's kit is older than yours: its mod files can't be checked against yours (the host updates)"});
    // The chat lines name the side that is older and fit the chat.
    const std::string older = CoopModShare::SeedKitLine(2, "Rev-the-long-name", "N2BTEYKM", false);
    CHECK(older.find("player 2 (Rev-the-long-name) is on a older kit") == std::string::npos);
    CHECK(older.find("is on an older kit than the host's") != std::string::npos);
    CHECK(older.find("it updates to the host's kit") != std::string::npos);
    CHECK(older.size() <= 200);
    const std::string newer = CoopModShare::SeedKitLine(2, "Rev-the-long-name", "N2BTEYKM", true);
    CHECK(newer.find("is on a newer kit than the host's") != std::string::npos);
    CHECK(newer.find("the host updates") != std::string::npos);
    CHECK(newer.size() <= 200);
    const std::string mine_newer = CoopModShare::SeedKitMineLine("N2BTEYKM", true);
    CHECK(mine_newer.find("the host's kit is older than yours") != std::string::npos);
    CHECK(mine_newer.size() <= 200);
    const std::string mine_older = CoopModShare::SeedKitMineLine("N2BTEYKM", false);
    CHECK(mine_older.find("update to the host's kit") != std::string::npos);
    CHECK(mine_older.size() <= 200);
}
