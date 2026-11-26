// SPDX-License-Identifier: GPL-3.0-or-later
// No puppet pack (lichii/hook/coop_pack_notice.h): the chat says so once per
// game run, when another player is first shown, and never with a pack.
#include <doctest/doctest.h>

#include <string>

#include "coop_pack_notice.h"

TEST_CASE("pack notice: once, only without a pack and with someone shown") {
    CoopPackNotice::Once once;
    CHECK_FALSE(once.Due(false, false));  // alone: nothing to explain yet
    CHECK(once.Due(false, true));
    CHECK_FALSE(once.Due(false, true));   // once per run
    CoopPackNotice::Once with_pack;
    CHECK_FALSE(with_pack.Due(true, true));
}

TEST_CASE("pack notice: off until the module turns it on, then the line says what to do") {
    CHECK(CoopPackNotice::TakeLines(true).empty());  // the emulator never calls Enable
    CoopPackNotice::Enable(false);
    CHECK(CoopPackNotice::TakeLines(false).empty());
    const auto lines = CoopPackNotice::TakeLines(true);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("No puppet pack") == 0);
#ifdef _WIN32
    CHECK(lines[0].find("Start Squadala.bat") != std::string::npos);
#else
    CHECK(lines[0].find("start-squadala.sh") != std::string::npos);
#endif
    CHECK(CoopPackNotice::TakeLines(true).empty());
}

TEST_CASE("pack notice: a game started by hand says to use the kit's start file once, with or without a pack") {
    CoopPackNotice::EnableDirect(false);
    CoopPackNotice::Enable(true);
    CHECK(CoopPackNotice::TakeLines(false).empty());   // started by the kit: nothing to say
    CoopPackNotice::EnableDirect(true);
    const auto lines = CoopPackNotice::TakeLines(false);   // at once, nobody needs to be shown
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("not started with") != std::string::npos);
    CHECK(lines[0].find("do not need your own Longshot build") != std::string::npos);
#ifdef _WIN32
    CHECK(lines[0].find("Start Squadala.bat") != std::string::npos);
#else
    CHECK(lines[0].find("start-squadala.sh") != std::string::npos);
#endif
    CHECK(CoopPackNotice::TakeLines(false).empty());   // once per run
    CoopPackNotice::EnableDirect(false);
}

TEST_CASE("pack notice: the title's hint line (solo, no chat box) follows the same switch") {
    CoopPackNotice::EnableDirect(false);
    CHECK_FALSE(CoopPackNotice::DirectOn());
    CoopPackNotice::EnableDirect(true);
    CHECK(CoopPackNotice::DirectOn());
    CHECK(std::string(CoopPackNotice::kDirectHint).find("Start Squadala") != std::string::npos);
    CHECK(std::string(CoopPackNotice::kDirectHint).size() <= 159);   // the hint line's placeholder holds 159 bytes
    CoopPackNotice::EnableDirect(false);
}
