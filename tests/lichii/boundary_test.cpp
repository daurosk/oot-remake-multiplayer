// SPDX-License-Identifier: GPL-3.0-or-later
// coop_boundary.h (save-crash-0118): nothing co-op throws crosses into the game; a C export's text is read inside its
// buffer only.
#include <doctest/doctest.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include "coop_boundary.h"

namespace {
struct Logged {
    std::string where, what;
    std::uint64_t n;
};
}  // namespace

TEST_CASE("boundary: EndedText reads up to the first NUL inside the buffer") {
    char buf[16] = {};
    CHECK(CoopBoundary::EndedText(buf, sizeof buf).empty());
    std::memcpy(buf, "C:\\saves\\slot_01", sizeof buf - 1);   // (cut to 15 chars and ended, as patch 0025's snprintf writes;
    buf[sizeof buf - 1] = '\0';                                // memcpy, not snprintf: gcc -Werror=format-truncation flags the cut)
    CHECK(CoopBoundary::EndedText(buf, sizeof buf) == "C:\\saves\\slot_0");
    CHECK(CoopBoundary::EndedText(nullptr, 16).empty());
    CHECK(CoopBoundary::EndedText(buf, 0).empty());
}

TEST_CASE("boundary: EndedText of a buffer with no NUL is empty, never a read past it") {
    char buf[8];
    for (char& c : buf) c = 'x';
    CHECK(CoopBoundary::EndedText(buf, sizeof buf).empty());
    buf[7] = '\0';
    CHECK(CoopBoundary::EndedText(buf, sizeof buf) == "xxxxxxx");
}

TEST_CASE("boundary: a call that returns passes its value and logs nothing") {
    CoopBoundary::Site site{"test-ok"};
    std::vector<Logged> log;
    const int v = CoopBoundary::Call(site, -1, [] { return 7; },
                                     [&](const char* w, const char* x, std::uint64_t n) { log.push_back({w, x, n}); });
    CHECK(v == 7);
    CHECK(log.empty());
    CHECK(site.thrown.load() == 0);
}

TEST_CASE("boundary: a std::bad_alloc gives the fallback and logs once, named") {
    CoopBoundary::Site site{"CoopHook::OnFrame"};
    std::vector<Logged> log;
    auto logger = [&](const char* w, const char* x, std::uint64_t n) { log.push_back({w, x, n}); };
    for (int i = 0; i < 5; ++i) {
        const int v = CoopBoundary::Call(site, -1, []() -> int { throw std::bad_alloc(); }, logger);
        CHECK(v == -1);
    }
    REQUIRE(log.size() == 1);
    CHECK(log[0].where == "CoopHook::OnFrame");
    CHECK(log[0].what.find("bad_alloc") != std::string::npos);
    CHECK(log[0].n == 1);
    CHECK(site.thrown.load() == 5);
}

TEST_CASE("boundary: other throws are caught too, with their text") {
    CoopBoundary::Site site{"test-runtime"};
    std::vector<Logged> log;
    auto logger = [&](const char* w, const char* x, std::uint64_t n) { log.push_back({w, x, n}); };
    CoopBoundary::Run(site, [] { throw std::runtime_error("path not in the code page"); }, logger);
    CoopBoundary::Run(site, [] { throw 42; }, logger);
    REQUIRE(log.size() == 1);   // (one site: the first throw only)
    CHECK(log[0].what == "path not in the code page");
    CoopBoundary::Site other{"test-unknown"};
    CoopBoundary::Run(other, [] { throw 42; }, logger);
    REQUIRE(log.size() == 2);
    CHECK(log[1].what == "an unknown exception");
}

TEST_CASE("boundary: a log that throws as well is dropped, the call still returns") {
    CoopBoundary::Site site{"test-log-throws"};
    const unsigned v = CoopBoundary::Call(
        site, 3u, []() -> unsigned { throw std::bad_alloc(); },
        [](const char*, const char*, std::uint64_t) { throw std::bad_alloc(); });
    CHECK(v == 3u);
    CHECK(site.thrown.load() == 1);
}

TEST_CASE("boundary: a site keeps logging rarely: the first throw, then each 1024th") {
    CHECK(CoopBoundary::ShouldLog(1));
    CHECK_FALSE(CoopBoundary::ShouldLog(2));
    CHECK_FALSE(CoopBoundary::ShouldLog(1023));
    CHECK(CoopBoundary::ShouldLog(1024));
    CHECK(CoopBoundary::ShouldLog(2048));
    CoopBoundary::Site site{"test-rare"};
    int logged = 0;
    for (int i = 0; i < 2048; ++i) {
        CoopBoundary::Run(site, [] { throw std::bad_alloc(); }, [&](const char*, const char*, std::uint64_t) { ++logged; });
    }
    CHECK(logged == 3);
}
