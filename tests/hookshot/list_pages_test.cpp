// SPDX-License-Identifier: GPL-3.0-or-later
// One set of paging rules for every list of games (hookshot/src/coop_list_pages.h, player report 264).
#include <doctest/doctest.h>

#include <string>

#include "coop_list_pages.h"
#include "coop_text_fit.h"

using namespace CoopListPages;

TEST_CASE("list pages: how many pages, at least one") {
    CHECK(Pages(0, 6) == 1);   // (an empty list still shows its one page)
    CHECK(Pages(1, 6) == 1);
    CHECK(Pages(6, 6) == 1);
    CHECK(Pages(7, 6) == 2);
    CHECK(Pages(12, 6) == 2);
    CHECK(Pages(13, 6) == 3);
    CHECK(Pages(5, 0) == 5);   // (a page of none counts as one a page, never a division by zero)
}

TEST_CASE("list pages: ZL and ZR go round at both ends, one page stays") {
    CHECK(Step(0, 3, 1) == 1);
    CHECK(Step(1, 3, 1) == 2);
    CHECK(Step(2, 3, 1) == 0);    // (ZR on the last page: the first)
    CHECK(Step(0, 3, -1) == 2);   // (ZL on the first page: the last)
    CHECK(Step(2, 3, -1) == 1);
    CHECK(Step(0, 1, 1) == 0);    // (one page: nothing to turn)
    CHECK(Step(0, 1, -1) == 0);
    CHECK(Step(1, 3, 0) == 1);
    CHECK(Step(7, 3, 1) == 2);    // (a page past the end, as a list that shrank: taken round first)
    CHECK(Step(0, 2, 5) == 1);    // (any ZR is one page)
    CHECK(Wrap(-1, 4) == 3);
    CHECK(Wrap(4, 4) == 0);
    CHECK(Wrap(3, 0) == 0);
}

TEST_CASE("list pages: the page line only with more than one page, the buttons in words only where asked") {
    CHECK(Line(0, 1).empty());   // (one page: nothing said)
    CHECK(Line(0, 0).empty());
    CHECK(Line(0, 1, true).empty());
    CHECK(Line(1, 3) == "Page 2/3");    // (the screen shows the game's ZL / ZR icons beside it: the - menu's list)
    CHECK(Line(-1, 3) == "Page 3/3");   // (taken round)
    CHECK(Line(0, 2, true) == "Page 1/2 (ZL/ZR)");   // (a screen with no icon pane of the game's: the title)
    // (row 264: no private-use letter of the game's button-icon font in any words: a words pane's font has none, they
    // drew as empty boxes)
    for (const std::string& l : {Line(1, 3), Line(0, 2, true)}) {
        for (const unsigned char c : l) CHECK(c < 0x80);
    }
}

TEST_CASE("list pages: a heading keeps its page line whole, a long name gives way") {
    const std::string line = Line(1, 3);
    CHECK(Heading("Public games", "", 30) == "Public games");
    CHECK(Heading("Public games", line, 30) == "Public games  " + line);   // (the - menu's list: it fits)
    const std::string cut = Heading("A very long list name for this", line, 30);
    CHECK(cut.size() <= 30u);
    CHECK(CoopTextFit::Halves(cut) <= 60u);
    CHECK(cut.find(line) != std::string::npos);    // (the page line whole, at the end)
    CHECK(cut.substr(cut.size() - line.size()) == line);
    CHECK(cut.find("...") != std::string::npos);   // (the name cut)
    CHECK(Heading("Public games", line, 12) == CoopTextFit::Fit(line, 12));   // (no room for a name: the line alone)
    // Wide letters (a Chinese heading) count by width, not only bytes.
    const std::string zh = "\xE5\x85\xAC\xE5\xBC\x80\xE6\xB8\xB8\xE6\x88\x8F";   // (公开游戏)
    const std::string h = Heading(zh + zh + zh, line, 30);
    CHECK(h.size() <= 30u);
    CHECK(CoopTextFit::Halves(h) <= 60u);
    CHECK(h.substr(h.size() - line.size()) == line);
    // The Chinese browser heading whole (proof-01112-d: "公开游戏" cut to "公..." while the line had the letters):
    // "公开游戏  第 1/2 页" is 25 bytes and 19 half-letters, in the 30 of kListNameChars.
    const std::string page = "\xE7\xAC\xAC 1/2 \xE9\xA1\xB5";   // (第 1/2 页)
    CHECK(Heading(zh, page, 30) == zh + "  " + page);
}
