// SPDX-License-Identifier: GPL-3.0-or-later
// After a crash, the title's ask to send /bugreport (hookshot/src/coop_problem_ask.h).
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "coop_i18n.h"
#include "coop_problem_ask.h"
#include "coop_title_native.h"

using namespace CoopProblemAsk;

namespace {
// 2026-11-21 12:00:00 UTC
constexpr std::int64_t kNow = 1795262400;
}  // namespace

TEST_CASE("problem ask: a crash record's time from its name") {
    CHECK(CrashTime("crash-19700101T000000Z.txt") == 0);
    CHECK(CrashTime("crash-20261121T120000Z.txt") == kNow);
    CHECK(CrashTime("crash-20261121T115959Z-4812-2.txt") == kNow - 1);
    CHECK(CrashTime("crash-20240229T000000Z.txt") == 1709164800);   // (a leap day)
    // not crash records: an unclean end, Longshot's ledger, ours, broken names
    CHECK_FALSE(CrashTime("unclean-20261121T120000Z.txt"));
    CHECK_FALSE(CrashTime("shown.txt"));
    CHECK_FALSE(CrashTime("crash-20261121T120000Z.log"));
    CHECK_FALSE(CrashTime("crash-20261121X120000Z.txt"));
    CHECK_FALSE(CrashTime("crash-20261121T120000Z-.txt"));
    CHECK_FALSE(CrashTime("crash-20261121T120000Z-12.txt"));
    CHECK_FALSE(CrashTime("crash-20261321T120000Z.txt"));
    CHECK_FALSE(CrashTime("crash-2026112"));
}

TEST_CASE("problem ask: the newest crash not asked about, once") {
    const std::vector<std::string> pending{"unclean-20261121T115000Z.txt", "crash-20261120T120000Z.txt",
                                           "crash-20261121T110000Z-77-1.txt", "notes.txt"};
    CHECK(Due(pending, "", kNow) == "crash-20261121T110000Z-77-1.txt");
    // after the ask, every crash pending is listed: no second ask for the older one
    const std::string asked = AskedAfter(pending);
    CHECK(asked == "crash-20261120T120000Z.txt\ncrash-20261121T110000Z-77-1.txt\n");
    CHECK_FALSE(Due(pending, asked, kNow));
    // a new crash later: asked about again; a Windows file's line ends still match
    std::vector<std::string> later = pending;
    later.push_back("crash-20261121T113000Z.txt");
    CHECK(Due(later, asked, kNow) == "crash-20261121T113000Z.txt");
    CHECK_FALSE(Due(later, "crash-20261121T113000Z.txt\r\n", kNow) == "crash-20261121T113000Z.txt");
}

TEST_CASE("problem ask: old crashes, a clock set wrong, no crash") {
    CHECK_FALSE(Due(std::vector<std::string>{"crash-20261118T115959Z.txt"}, "", kNow));   // (over three days)
    CHECK(Due(std::vector<std::string>{"crash-20261118T120001Z.txt"}, "", kNow));
    CHECK_FALSE(Due(std::vector<std::string>{"crash-20261121T121000Z.txt"}, "", kNow));   // (10 minutes ahead)
    CHECK(Due(std::vector<std::string>{"crash-20261121T120400Z.txt"}, "", kNow));
    CHECK_FALSE(Due(std::vector<std::string>{"unclean-20261121T115000Z.txt"}, "", kNow));
    CHECK_FALSE(Due(std::vector<std::string>{}, "", kNow));
}

TEST_CASE("problem ask: the words fit the title's buttons") {
    for (const char* w : kWords) {
        CHECK(std::string(w).size() <= CoopTitleNative::kTextBytes);
    }
}

TEST_CASE("problem ask: the words fit the title's buttons in Chinese too") {
    for (const char* code : {"zh-Hans", "zh-Hant"}) {
        const CoopI18n::Lang* lang = nullptr;
        for (int i = 0; i < CoopI18n::kLangCount; ++i) {
            if (std::string(CoopI18n::kLangs[i].code) == code) lang = &CoopI18n::kLangs[i];
        }
        REQUIRE(lang != nullptr);
        for (const char* w : kWords) {
            const std::string text = CoopI18n::Tr(lang, w);
            CHECK_MESSAGE(text != w, w);   // (each has its line)
            CHECK(text.size() <= CoopTitleNative::kTextBytes);
        }
    }
}

TEST_CASE("problem ask: the game folder's files") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
                         ("coop-problem-ask-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CHECK(PendingNames(dir).empty());   // (no folder)
    CHECK(ReadAsked(dir).empty());
    fs::create_directories(dir / "bug-reports" / "pending");
    std::ofstream(dir / "bug-reports" / "pending" / "crash-20261121T110000Z.txt") << "run_id: x\n";
    const auto names = PendingNames(dir);
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "crash-20261121T110000Z.txt");
    CHECK(WriteAsked(dir, AskedAfter(names)));
    CHECK(ReadAsked(dir) == "crash-20261121T110000Z.txt\n");
    CHECK_FALSE(Due(PendingNames(dir), ReadAsked(dir), kNow));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
