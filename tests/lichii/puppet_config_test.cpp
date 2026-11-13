// SPDX-License-Identifier: GPL-3.0-or-later
// The puppets' settings from the variables, the same on the emulator and
// Hookshot (lichii/hook/coop_puppet_config.h).
#include <doctest/doctest.h>

#include <map>
#include <string>
#include <vector>

#include "coop_puppet_config.h"

namespace {

CoopPuppet::PlannerSettings From(const std::map<std::string, std::string>& vars, bool pack_green,
                                 std::vector<std::string>* log = nullptr, int* asked = nullptr) {
    return CoopPuppetConfig::Settings(
        [&](const char* name) {
            const auto it = vars.find(name);
            return it == vars.end() ? std::string() : it->second;
        },
        [&] {
            if (asked != nullptr) ++*asked;
            return pack_green;
        },
        log);
}

}  // namespace

TEST_CASE("puppet config: nothing set gives coloured tunics, no green puppet, no test puppet") {
    int asked = 0;
    const auto s = From({}, true, nullptr, &asked);
    CHECK(s.tunics);
    CHECK_FALSE(s.green_puppet);
    CHECK_FALSE(s.debug);
    CHECK_FALSE(s.debug_anim);
    CHECK(s.actor.empty());
    CHECK(asked == 0);  // the pack is read only for another own colour
}

TEST_CASE("puppet config: LICHII_COOP_TUNICS=0 or off puts everyone in green") {
    CHECK_FALSE(From({{"LICHII_COOP_TUNICS", "0"}}, false).tunics);
    CHECK_FALSE(From({{"LICHII_COOP_TUNICS", "off"}}, false).tunics);
    CHECK(From({{"LICHII_COOP_TUNICS", "1"}}, false).tunics);
    std::vector<std::string> log;
    From({{"LICHII_COOP_TUNICS", "0"}}, false, &log);
    REQUIRE(log.size() == 1);
    CHECK(log[0].find("tunics off") != std::string::npos);
}

TEST_CASE("puppet config: own Link in another colour asks the pack for the green puppet") {
    std::vector<std::string> log;
    int asked = 0;
    auto s = From({{"LICHII_COOP_SELF_TUNIC", "blue"}}, true, &log, &asked);
    CHECK(s.green_puppet);
    CHECK(asked == 1);
    REQUIRE(log.size() == 2);
    CHECK(log[0] == "coop: your Link wears blue; green players get CoopPuppet_<Age>_Green");
    // An old pack: the plain puppet, and the log says to make the pack again.
    log.clear();
    s = From({{"LICHII_COOP_SELF_TUNIC", "red"}}, false, &log);
    CHECK_FALSE(s.green_puppet);
    CHECK(log[0].find("make it again") != std::string::npos);
    // Green is the plain puppet already.
    asked = 0;
    CHECK_FALSE(From({{"LICHII_COOP_SELF_TUNIC", "green"}}, true, nullptr, &asked).green_puppet);
    CHECK(asked == 0);
}

TEST_CASE("puppet config: actor, test puppet and animation log switches") {
    const auto s = From({{"LICHII_COOP_PUPPET_ACTOR", "CoopPuppet_Adult"},
                         {"LICHII_COOP_TEST_PUPPET", "1"},
                         {"LICHII_COOP_DEBUG_ANIM", "1"}},
                        false);
    CHECK(s.actor == "CoopPuppet_Adult");
    CHECK(s.debug);
    CHECK(s.debug_anim);
    CHECK_FALSE(From({{"LICHII_COOP_TEST_PUPPET", "0"}}, false).debug);
}

TEST_CASE("puppet config: the native player test is off unless asked for") {
    CHECK_FALSE(From({}, false).native_player);
    CHECK_FALSE(From({{"LICHII_COOP_NATIVE_PLAYER", "0"}}, false).native_player);
    std::vector<std::string> log;
    CHECK(From({{"LICHII_COOP_NATIVE_PLAYER", "1"}}, false, &log).native_player);
    CHECK(log.back().find("NATIVE PLAYER TEST") != std::string::npos);
}

TEST_CASE("puppet config: a pack without tunic puppets turns the tunics off (Hookshot proof 3)") {
    const auto with = [](std::optional<bool> has, std::vector<std::string>* log = nullptr, int* asked = nullptr,
                         std::map<std::string, std::string> vars = {}) {
        return CoopPuppetConfig::Settings(
            [&](const char* name) {
                const auto it = vars.find(name);
                return it == vars.end() ? std::string() : it->second;
            },
            [] { return false; }, log,
            [&]() -> std::optional<bool> {
                if (asked != nullptr) ++*asked;
                return has;
            });
    };
    // The pairs' pack from before the tunics: plain puppets only, and the log says why.
    std::vector<std::string> log;
    CHECK_FALSE(with(false, &log).tunics);
    REQUIRE(log.size() == 2);
    CHECK(log[0].find("no tunic colours") != std::string::npos);
    CHECK(log[0].find("make-puppet-pack") != std::string::npos);
    CHECK(log[1].find(", tunics off (the pack has none)") != std::string::npos);
    // A pack with them, or no manifest found: as before.
    CHECK(with(true).tunics);
    CHECK(with(std::nullopt).tunics);
    // LICHII_COOP_TUNICS=0 wins and the pack is not read for it.
    int asked = 0;
    log.clear();
    CHECK_FALSE(with(true, &log, &asked, {{"LICHII_COOP_TUNICS", "0"}}).tunics);
    CHECK(asked == 0);
    CHECK(log.back().find("(LICHII_COOP_TUNICS=0)") != std::string::npos);
}

TEST_CASE("puppet config: the manifest names tunic puppets or only the plain ones") {
    // Lines as tools/puppet_pack writes them (coop_puppet.json "actors").
    const std::string plain = R"({"actors": [{"actor": "CoopPuppet_Child", "pack": "Pack/Actor/CoopPuppet_Child.pack.zs",
        "members": ["Actor/CoopPuppet_Child.engine__actor__ActorParam.bgyml"]}, {"actor": "CoopPuppet_Adult"}]})";
    const std::string tunics = R"({"actors": [{"actor": "CoopPuppet_Child"}, {"actor": "CoopPuppet_Child_Blue"},
        {"actor": "CoopPuppet_Adult_Blue"}]})";
    CHECK_FALSE(CoopPuppet::PackHasTunicPuppets(plain));
    CHECK(CoopPuppet::PackHasTunicPuppets(tunics));
    CHECK(CoopPuppet::PackHasTunicPuppets(R"({"actor": "CoopPuppet_Adult_Red"})"));
    CHECK_FALSE(CoopPuppet::PackHasTunicPuppets(""));
}
