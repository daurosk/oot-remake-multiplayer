// SPDX-License-Identifier: GPL-3.0-or-later
// Longshot patch 0028's Skip title screen (hookshot/src/hos_skip_title.h, the patch's host/hos_skip_title.h): it chooses
// only the title's Continue, the load list's file and its question's first answer, in that order; any other screen, a
// known one out of order or focused on another button stops it; at most three choices (player report 133).
#include <doctest/doctest.h>

#include <string>

#include "hos_skip_title.h"

namespace {
constexpr uint64_t kTitle = HOS_SKIP_TITLE_SCREEN, kList = HOS_SKIP_LOAD_LIST_SCREEN, kCheck = HOS_SKIP_LOAD_CHECK_SCREEN;
int Next(int step, uint64_t vt, const char* name, std::string* why = nullptr) {
    const char* w = nullptr;
    const int r = hos_skip_title_next(step, vt, name, &w);
    REQUIRE(w != nullptr);
    if (why) *why = w;
    return r;
}
}  // namespace

TEST_CASE("skip title: the three screens in order, each on its one button") {
    CHECK(Next(0, kTitle, "Pa_Button_02") == HOS_SKIP_CHOOSE);       // Continue
    CHECK(Next(1, kList, "Pa_SaveData_00") == HOS_SKIP_CHOOSE);      // the file the list focuses
    CHECK(Next(2, kCheck, "Pa_CheckButton_00") == HOS_SKIP_CHOOSE);  // its first answer
    CHECK(hos_skip_title_step_of(kTitle) == 0);
    CHECK(hos_skip_title_step_of(kList) == 1);
    CHECK(hos_skip_title_step_of(kCheck) == 2);
    CHECK(HOS_SKIP_STEPS == 3);
}

TEST_CASE("skip title: an unknown screen stops it for good, at every step") {
    for (int step = 0; step < HOS_SKIP_STEPS; ++step) {
        std::string why;
        INFO("step " << step);
        CHECK(Next(step, 0x84c0bc18ULL, "Pa_Button_00", &why) == HOS_SKIP_STOP);   // (the pause menu's vtable)
        CHECK(why == "a screen that is not the title, the load list or its question");
        CHECK(Next(step, 0, "", nullptr) == HOS_SKIP_STOP);
    }
}

TEST_CASE("skip title: a known screen out of order stops it, an earlier one still updating is waited out") {
    std::string why;
    CHECK(Next(0, kList, "Pa_SaveData_00", &why) == HOS_SKIP_STOP);
    CHECK(why == "a screen out of order");
    CHECK(Next(0, kCheck, "Pa_CheckButton_00") == HOS_SKIP_STOP);
    CHECK(Next(1, kCheck, "Pa_CheckButton_00") == HOS_SKIP_STOP);
    CHECK(Next(1, kTitle, "Pa_Button_02") == HOS_SKIP_WAIT);   // the title behind the list
    CHECK(Next(2, kTitle, "Pa_Button_02") == HOS_SKIP_WAIT);
    CHECK(Next(2, kList, "Pa_SaveData_00") == HOS_SKIP_WAIT);
}

TEST_CASE("skip title: another button on a known screen stops it (New Game, Options, the second answer, no name)") {
    std::string why;
    CHECK(Next(0, kTitle, "Pa_Button_00", &why) == HOS_SKIP_STOP);   // New Game
    CHECK(why == "the title's focus is not on Continue (Pa_Button_02)");
    CHECK(Next(0, kTitle, "Pa_Button_01") == HOS_SKIP_STOP);          // Options
    CHECK(Next(0, kTitle, "Pa_Button_03") == HOS_SKIP_STOP);          // (co-op's rows)
    CHECK(Next(0, kTitle, "") == HOS_SKIP_STOP);                      // (a name that could not be read)
    CHECK(Next(0, kTitle, nullptr) == HOS_SKIP_STOP);
    CHECK(Next(1, kList, "") == HOS_SKIP_STOP);
    CHECK(Next(2, kCheck, "Pa_CheckButton_01", &why) == HOS_SKIP_STOP);
    CHECK(why == "the question's focus is not on its first answer (Pa_CheckButton_00)");
}

TEST_CASE("skip title: no fourth choice") {
    std::string why;
    for (const uint64_t vt : {kTitle, kList, kCheck, uint64_t{0x1234}}) {
        CHECK(Next(3, vt, "Pa_Button_02", &why) == HOS_SKIP_STOP);
        CHECK(why == "made its three choices");
    }
}
