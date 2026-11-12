// SPDX-License-Identifier: GPL-3.0-or-later
// The Hookshot module (hookshot/src) and our patch to Hookshot
// (hookshot/patches): what can be checked without the game.
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "coop_hold.h"
#include "coop_hookshot.h"
#include "coop_svc.h"

namespace {

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    std::string text = s.str();
    std::string out;  // without carriage returns
    for (const char c : text) {
        if (c != '\r') out += c;
    }
    return out;
}

const std::filesystem::path kRepo = std::filesystem::path(COOP_TEST_SOURCE_DIR) / "..";

std::string Patch() {
    return ReadFile(kRepo / "hookshot" / "patches" / "0001-coop-bridge.patch");
}

// The lines a patch adds to a new file, without their '+'.
std::string NewFile(const std::string& patch, const std::string& path) {
    const auto start = patch.find("+++ b/" + path + "\n");
    REQUIRE(start != std::string::npos);
    auto at = patch.find("@@", start);
    at = patch.find('\n', at) + 1;
    std::string out;
    while (at < patch.size()) {
        const auto end = patch.find('\n', at);
        const std::string line = patch.substr(at, end - at);
        if (line.empty() || line[0] != '+') break;
        out += line.substr(1) + "\n";
        at = end + 1;
    }
    return out;
}

}  // namespace

TEST_CASE("hookshot: the API header in our patch is hookshot/src's") {
    CHECK(NewFile(Patch(), "host/hos_coop_api.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_coop_api.h"));
}

TEST_CASE("hookshot: the patch's hook sites are the emulator cave's") {
    const std::string hooks = NewFile(Patch(), "nx2/coop_hooks.py");
    // The frame hook goes after the `mov x0, x20` before each of the cave's calls.
    for (const auto& h : CoopSvc::kHooks) {
        char site[64];
        std::snprintf(site, sizeof site, "(0x%llx, 0x%08x, ", static_cast<unsigned long long>(h.offset - 4), h.original);
        CHECK_MESSAGE(hooks.find(site) != std::string::npos, site);
    }
    char hold[48];
    std::snprintf(hold, sizeof hold, "HOLD_CALL = 0x%llx", static_cast<unsigned long long>(CoopHold::kHookOffset));
    CHECK(hooks.find(hold) != std::string::npos);
    char word[32];
    std::snprintf(word, sizeof word, "HOLD_WORDS = (0x%08x,", CoopHold::kHookOriginal);
    CHECK(hooks.find(word) != std::string::npos);
    CHECK(hooks.find("MAIN = 0x80020000") != std::string::npos);
}

TEST_CASE("hookshot: the module knows the game build by main's id as Hookshot gives it") {
    CHECK(CoopHookshot::BuildSupported("e0f3ccfaf50065ae4ea01eb5aeb13b1a"));
    CHECK(CoopHookshot::BuildSupported("E0F3CCFAF50065AE"));
    CHECK_FALSE(CoopHookshot::BuildSupported("d00e4f399e411d0613aa8d26b1c02fef"));
    CHECK_FALSE(CoopHookshot::BuildSupported("e0f3cc"));
    CHECK_FALSE(CoopHookshot::BuildSupported(""));
}

TEST_CASE("hookshot: the module builds every hook file the emulator builds, but the cave") {
    const std::string cmake = ReadFile(kRepo / "hookshot" / "CMakeLists.txt");
    CHECK(cmake.find("lichii/hook/core.cmake") != std::string::npos);
    CHECK(cmake.find("NOT name STREQUAL \"coop_svc.cpp\"") != std::string::npos);
    // The test commands are built in (the autotest driver's place, give, ...).
    CHECK(cmake.find("NOT name STREQUAL \"coop_testinput.cpp\"") == std::string::npos);
    // The stand-ins for what the cave did.
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    CHECK(module.find("std::string Refusal()") != std::string::npos);
    CHECK(module.find("CoopCallers::RunFrame") != std::string::npos);
    CHECK(module.find("namespace CoopTestInput") == std::string::npos);  // the real one is built
}

TEST_CASE("hookshot: the puppets' settings are the emulator's, not a copy") {
    // A shortened copy in the module dropped LICHII_COOP_TUNICS and
    // LICHII_COOP_SELF_TUNIC on Hookshot (issue 202): both call one function.
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    const std::string svc = ReadFile(kRepo / "lichii" / "hook" / "coop_svc.cpp");
    CHECK(module.find("CoopPuppetConfig::Configure()") != std::string::npos);
    CHECK(svc.find("CoopPuppetConfig::Configure()") != std::string::npos);
    CHECK(module.find("PlannerSettings") == std::string::npos);
    CHECK(svc.find("PlannerSettings") == std::string::npos);
    const std::string cmake = ReadFile(kRepo / "lichii" / "hook" / "core.cmake");
    CHECK(cmake.find("coop_puppet_config.cpp") != std::string::npos);
}

TEST_CASE("hookshot: test commands come from the driver's file, the pad stays the emulator's") {
    const std::string input = ReadFile(kRepo / "lichii" / "hook" / "coop_testinput.cpp");
    CHECK(input.find("\"LICHII_COOP_TEST_COMMANDS\"") != std::string::npos);
    CHECK(input.find("\"LICHII_COOP_TEST_SCRIPT\"") != std::string::npos);
    // The hid headers only for the emulator.
    const auto hid = input.find("#include \"hid_core/");
    REQUIRE(hid != std::string::npos);
    const auto guard = input.rfind("#ifndef COOP_HOOKSHOT", hid);
    CHECK(guard != std::string::npos);
    CHECK(hid - guard < 30);
    // The driver writes the same variable.
    const std::string driver = ReadFile(kRepo / "tools" / "autotest" / "hookshot.py");
    CHECK(driver.find("LICHII_COOP_TEST_COMMANDS") != std::string::npos);
}
