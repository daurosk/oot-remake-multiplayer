// SPDX-License-Identifier: GPL-3.0-or-later
// The game browser's search, filter and sort (hookshot/src/coop_browse.h).
#include <doctest/doctest.h>

#include "coop_browse.h"

using namespace CoopBrowse;

namespace {

std::vector<Entry> Games() {
    return {
        {"kokiri", "Saria", 2, 4, true},    // 0
        {"forest", "Mido", 4, 4, true},     // 1: full
        {"Lake", "Ruto", 1, 4, false},      // 2: another version
        {"desert", "Nabooru", 3, 8, true},  // 3
    };
}

}  // namespace

TEST_CASE("browse: the most players first by default, ties by name") {
    CHECK(Order(Games(), "", kAll, kMostPlayers) == std::vector<int>{1, 3, 0, 2});
    CHECK(Order(Games(), "", kAll, kFewestPlayers) == std::vector<int>{2, 0, 3, 1});
    CHECK(Order(Games(), "", kAll, kByName) == std::vector<int>{3, 1, 0, 2});   // (any case: Lake after kokiri)
    std::vector<Entry> tie = {{"b", "", 2, 4, true}, {"a", "", 2, 4, true}};
    CHECK(Order(tie, "", kAll, kMostPlayers) == std::vector<int>{1, 0});
}

TEST_CASE("browse: the filter keeps the open games, or this version's") {
    CHECK(Order(Games(), "", kOpen, kMostPlayers) == std::vector<int>{3, 0});
    CHECK(Order(Games(), "", kSameVersion, kMostPlayers) == std::vector<int>{1, 3, 0});
    CHECK(Joinable(Games()[0]));
    CHECK_FALSE(Joinable(Games()[1]));
    CHECK_FALSE(Joinable(Games()[2]));
    CHECK(Joinable(Entry{"x", "", 5, 0, true}));   // (no max known: not full)
}

TEST_CASE("browse: the search finds the name or the host's, any case") {
    CHECK(Order(Games(), "KOK", kAll, kByName) == std::vector<int>{0});
    CHECK(Order(Games(), "mido", kAll, kByName) == std::vector<int>{1});
    CHECK(Order(Games(), "e", kAll, kByName) == std::vector<int>{3, 1, 2});
    CHECK(Order(Games(), "zzz", kAll, kByName).empty());
    CHECK(std::string(FilterWords(kOpen)) == "Open games");
    CHECK(std::string(SortWords(kByName)) == "Name");
}
