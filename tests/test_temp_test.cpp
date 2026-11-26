// SPDX-License-Identifier: GPL-3.0-or-later
// The shared temp-folder helper (tests/test_temp.h): every call gets a folder of its own.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <set>

#include "test_temp.h"

TEST_CASE("test temp: the same name gives a different folder each time") {
    std::set<std::filesystem::path> seen;
    for (int i = 0; i < 50; ++i) CHECK(seen.insert(ModTestDir("coop-test-temp-same")).second);
}

TEST_CASE("test temp: removing one folder leaves another one with the same name alone") {
    namespace fs = std::filesystem;
    const fs::path a = ModTestDir("coop-test-temp-pair"), b = ModTestDir("coop-test-temp-pair");
    CHECK(a != b);
    fs::create_directories(a);
    fs::create_directories(b);
    std::ofstream(b / "keep.txt") << "still here";
    fs::remove_all(a);
    CHECK(fs::exists(b / "keep.txt"));
    fs::remove_all(b);
}
