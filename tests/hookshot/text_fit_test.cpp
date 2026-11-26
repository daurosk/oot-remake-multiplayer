// SPDX-License-Identifier: GPL-3.0-or-later
// How much text a native row holds (hookshot/src/coop_text_fit.h): by width, never inside a letter.
#include <doctest/doctest.h>

#include "coop_text_fit.h"

using namespace CoopTextFit;

namespace {
bool ValidUtf8(const std::string& s) {
    for (std::size_t at = 0, n = 1; at < s.size(); at += n) {
        const unsigned char c = static_cast<unsigned char>(s[at]);
        n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || at + n > s.size()) return false;
        for (std::size_t i = 1; i < n; ++i) {
            if ((static_cast<unsigned char>(s[at + i]) & 0xC0) != 0x80) return false;
        }
    }
    return true;
}
}  // namespace

TEST_CASE("text fit: a text that fitted before is left as it is") {
    CHECK(Fit("Leave (not in a session)", 24) == "Leave (not in a session)");
    CHECK(Fit("", 24).empty());
}

TEST_CASE("text fit: ASCII is cut as the byte rule cut it, never longer") {
    const std::string t(40, 'a');
    CHECK(Fit(t, 24) == std::string(21, 'a') + "...");
}

TEST_CASE("text fit: wide letters count wider than narrow ones") {
    const std::string w(24, 'W');   // 24 bytes: fitted by the byte rule, 36 letters wide
    const std::string cut = Fit(w, 24);
    CHECK(cut != w);
    CHECK(Halves(cut) <= 48);
    CHECK(cut.size() <= 24);
    CHECK(cut.substr(cut.size() - 3) == "...");
}

TEST_CASE("text fit: a wide script's letters are cut whole") {
    std::string zh;
    for (int i = 0; i < 20; ++i) zh += "\xE4\xB8\xAD";   // 20 x U+4E2D
    for (const std::size_t n : {10u, 24u, 40u, 63u}) {
        const std::string cut = Fit(zh, n);
        CHECK(ValidUtf8(cut));
        CHECK(cut.size() <= n);
        CHECK(Halves(cut) <= 2 * n);
    }
}

TEST_CASE("text fit: a wide script's name is cut by its width, not as one letter each") {
    std::string ko;
    for (int i = 0; i < 28; ++i) ko += "\xED\x95\x9C";   // 28 x U+D55C
    CHECK(Halves(ko) == 28 * 4);
    const std::string cut = ko.substr(0, PrefixEnd(ko, ko.size(), 2 * 28 - 6));
    CHECK(ValidUtf8(cut));
    CHECK(Halves(cut) <= 50);
    CHECK(cut.size() < ko.size());
}

TEST_CASE("text fit: LineEnd breaks at a space, and never inside a letter of a text with none") {
    const std::string en = "Two players on this PC with one game each";
    const std::size_t end = LineEnd(en, 0, 24);
    CHECK(end <= 24);
    CHECK(en[end] == ' ');
    std::string zh;
    for (int i = 0; i < 30; ++i) zh += "\xE4\xB8\xAD";
    std::size_t at = 0;
    int lines = 0;
    while (at < zh.size() && lines < 40) {
        const std::size_t e = LineEnd(zh, at, 24);
        REQUIRE(e > at);
        CHECK(ValidUtf8(zh.substr(at, e - at)));
        CHECK(Halves(zh.substr(at, e - at)) <= 48);
        at = e;
        ++lines;
    }
    CHECK(at == zh.size());
}

TEST_CASE("text fit: a text the game drew wider than its box is cut to the width that fits") {
    // (menu-polish-01113: "Co-op came with the game", 24 letters, ran past its button)
    const std::string row = "Co-op came with the game";
    CHECK(CoopTextFit::LettersForWidth(row, 200.0f, 220.0f) == 0);   // (fits: nothing cut)
    CHECK(CoopTextFit::LettersForWidth(row, 220.0f, 220.0f) == 0);
    const std::size_t n = CoopTextFit::LettersForWidth(row, 264.0f, 220.0f);
    CHECK(n == 20);                                                   // (24 letters at 220/264)
    CHECK(CoopTextFit::Fit(row, n).size() <= 20);
    CHECK(CoopTextFit::Fit(row, n).find("...") != std::string::npos);
    CHECK(CoopTextFit::LettersForWidth(row, 221.0f, 220.0f) == 23);   // (a hair too wide: always a letter shorter)
    CHECK(CoopTextFit::LettersForWidth(row, 2000.0f, 220.0f) == 4);   // (never below one letter and "...")
    CHECK(CoopTextFit::LettersForWidth("Back", 500.0f, 100.0f) == 0); // (nothing to cut)
    CHECK(CoopTextFit::LettersForWidth(row, 300.0f, 0.0f) == 0);      // (no box read: left as it is)
}
