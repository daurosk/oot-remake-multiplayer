// Tests for the test-input script parser (lichii/hook/coop_testinput.cpp).
// No emulator code.
#include <doctest/doctest.h>

#include <string>

#include "coop_testinput.h"

using namespace CoopTestInput;

TEST_CASE("testinput: wait, hold, press and marks") {
    std::string error;
    const auto s = Parse(
        "# load the save\n"
        "wait 120\n"
        "mark go\n"
        "hold LSTICK_UP+B 90   # run\n"
        "press a\n"
        "press ZR+A 10\n",
        error);
    REQUIRE_MESSAGE(s, error);
    REQUIRE(s->steps.size() == 7);
    CHECK(s->total_ticks == 120 + 90 + 12 + 20);

    CHECK(At(*s, 0)->buttons == 0);
    CHECK_FALSE(At(*s, 0)->left);
    const Step* run = At(*s, 120);
    REQUIRE(run);
    CHECK(run->buttons == kB);
    REQUIRE(run->left);
    CHECK(run->left->x == 0);
    CHECK(run->left->y == kStickMax);
    CHECK(At(*s, 209) == run);
    CHECK(At(*s, 210)->buttons == kA);
    CHECK(At(*s, 216)->buttons == 0);
    CHECK(At(*s, 222)->buttons == (kZR | kA));
    CHECK(At(*s, 232)->buttons == 0);
    CHECK(At(*s, 242) == nullptr);

    const auto marks = MarksBetween(*s, -1, 120);
    REQUIRE(marks.size() == 1);
    CHECK(marks[0]->mark == "go");
    CHECK(marks[0]->line == 3);
    CHECK(MarksBetween(*s, 120, 500).empty());
    CHECK(MarksBetween(*s, -1, 119).empty());
}

TEST_CASE("testinput: sticks") {
    std::string error;
    const auto s = Parse("stick 0.5 -1 60 B\nrstick -1 0 30\nhold LSTICK_UP+LSTICK_LEFT 5\n",
                         error);
    REQUIRE_MESSAGE(s, error);
    const Step* a = At(*s, 0);
    REQUIRE(a->left);
    CHECK(a->left->x == (kStickMax + 1) / 2);
    CHECK(a->left->y == -kStickMax);
    CHECK(a->buttons == kB);
    CHECK_FALSE(a->right);
    const Step* b = At(*s, 60);
    REQUIRE(b->right);
    CHECK(b->right->x == -kStickMax);
    CHECK_FALSE(b->left);
    const Step* c = At(*s, 90);
    REQUIRE(c->left);
    CHECK(c->left->x == -kStickMax);
    CHECK(c->left->y == kStickMax);
}

TEST_CASE("testinput: a mark at the start counts") {
    std::string error;
    const auto s = Parse("mark start\nwait 1\nmark end\n", error);
    REQUIRE_MESSAGE(s, error);
    CHECK(MarksBetween(*s, -1, 0).size() == 1);
    CHECK(MarksBetween(*s, 0, 1).size() == 1);
    CHECK(MarksBetween(*s, 0, 1)[0]->mark == "end");
}

TEST_CASE("testinput: bad lines say where") {
    std::string error;
    CHECK_FALSE(Parse("wait 10\nhold Q 5\n", error));
    CHECK(error == "line 2: unknown key Q");
    CHECK_FALSE(Parse("wait\n", error));
    CHECK(error.rfind("line 1:", 0) == 0);
    CHECK_FALSE(Parse("wait 0\n", error));
    CHECK_FALSE(Parse("wait -5\n", error));
    CHECK_FALSE(Parse("stick 2 0 10\n", error));
    CHECK_FALSE(Parse("stick 0 0 10 A Q\n", error));
    CHECK_FALSE(Parse("jump 10\n", error));
    CHECK(error == "line 1: unknown command jump");
    CHECK_FALSE(Parse("hold A+ 5\n", error));
    CHECK_FALSE(Parse("  # only a comment\n\n", error));
    CHECK(error == "no steps");
}

TEST_CASE("testinput: windows line ends") {
    std::string error;
    const auto s = Parse("wait 5\r\npress B\r\n", error);
    REQUIRE_MESSAGE(s, error);
    CHECK(s->total_ticks == 17);
}
