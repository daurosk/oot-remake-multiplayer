// SPDX-License-Identifier: GPL-3.0-or-later
// The Hookshot module (hookshot/src) and our patch to Hookshot
// (hookshot/patches): what can be checked without the game.
#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>
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

std::vector<std::string> g_log;
void LogLine(const char* line) {
    g_log.emplace_back(line);
}
int NoRead(uint64_t, void*, size_t) {
    return 0;
}
int NoWrite(uint64_t, const void*, size_t) {
    return 0;
}
int NoCas(uint64_t, uint32_t, uint32_t, uint32_t*) {
    return 0;
}
int NoCall(void*, const HosCoopCall*, uint64_t*) {
    return 0;
}

HosCoopApi FakeApi(const char* build, const char* dir) {
    HosCoopApi api{};
    api.version = HOS_COOP_API_VERSION;
    api.size = sizeof api;
    api.main_base = 0x80020000ULL;
    api.main_build_id = build;
    api.game_dir = dir;
    api.read = NoRead;
    api.write = NoWrite;
    api.cas32 = NoCas;
    api.call = NoCall;
    api.log = LogLine;
    return api;
}

bool Logged(const std::string& text) {
    for (const auto& l : g_log) {
        if (l.find(text) != std::string::npos) return true;
    }
    return false;
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

TEST_CASE("hookshot: the module stays off without a table, another build or a server") {
    const auto dir = std::filesystem::temp_directory_path() / "coop-hookshot-test-empty";
    std::filesystem::create_directories(dir);
    std::filesystem::remove(dir / "coop.ini");
    const std::string d = dir.string();

    CHECK_FALSE(CoopHookshot::Start(nullptr));
    HosCoopApi old = FakeApi("e0f3ccfaf50065ae4ea01eb5aeb13b1a", d.c_str());
    old.version = HOS_COOP_API_VERSION + 1;
    CHECK_FALSE(CoopHookshot::Start(&old));

    g_log.clear();
    HosCoopApi mkw = FakeApi("d00e4f399e411d0613aa8d26b1c02fef", d.c_str());
    CHECK_FALSE(CoopHookshot::Start(&mkw));
    CHECK(Logged("not supported"));

    if (std::getenv("LICHII_COOP_SERVER") == nullptr) {
        g_log.clear();
        HosCoopApi ok = FakeApi("e0f3ccfaf50065ae4ea01eb5aeb13b1a", d.c_str());
        CHECK_FALSE(CoopHookshot::Start(&ok));
        CHECK(Logged("game build e0f3ccfaf50065ae"));
        CHECK(Logged("off (no server"));
    }
    // Off: frames and the hold do nothing.
    CoopHookshot::Frame(nullptr, HOS_COOP_SITE_FRAME);
    CHECK(CoopHookshot::Hold(0x1234, 0x55u) == 0x55u);
    CoopHookshot::Stop();
}
