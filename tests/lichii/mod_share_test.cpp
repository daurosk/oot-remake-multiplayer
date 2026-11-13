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

TEST_CASE("mod share: the same mods in any order match; hashes not known yet are not compared") {
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
