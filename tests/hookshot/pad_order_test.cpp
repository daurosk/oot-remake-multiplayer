// SPDX-License-Identifier: GPL-3.0-or-later
// hos_coop_pad's order (hookshot/src/coop_pad_order.h) with stand-ins: our pause page first, the code mods see what it
// passed on and hold after it, the title's watch and down but not out always run on what the game gets, and a throw in
// the mods' part holds nothing.
#include <doctest/doctest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "coop_pad_order.h"

namespace {

constexpr std::uint64_t kA = 1, kB = 2, kR = 1u << 7, kUp = 1u << 13;

struct Chain {
    bool page_shows = false;      // our page takes A and the d-pad, passes B
    std::uint64_t hold = 0;       // the mods' holds
    bool mods_throw = false;
    std::uint64_t noted = ~std::uint64_t{0};
    std::uint64_t title_saw = ~std::uint64_t{0};
    std::uint64_t dbno_saw = ~std::uint64_t{0};
    std::int32_t title_stick = -1;
    std::vector<std::string> order;

    std::uint64_t Run(std::uint64_t buttons, std::int32_t stick[4]) {
        return CoopPadOrder::Run(
            buttons, stick,
            [this](std::uint64_t b, std::int32_t* s) {
                order.push_back("page");
                if (page_shows) {
                    s[0] = s[2] = s[3] = 0;
                    return b & kB;
                }
                return b;
            },
            [this](std::uint64_t b) {
                order.push_back("note");
                if (mods_throw) throw std::runtime_error("boom");
                noted = b;
            },
            [this] {
                order.push_back("held");
                return hold;
            },
            [this](std::uint64_t b, std::int32_t* s) {
                order.push_back("title");
                title_saw = b;
                title_stick = s[1];
                return b;
            },
            [this](std::uint64_t b, std::int32_t*) {
                order.push_back("dbno");
                dbno_saw = b;
                return b;
            });
    }
};

}  // namespace

TEST_CASE("pad order: our page first, then the mods, then the title's watch and down but not out") {
    Chain c;
    std::int32_t stick[4] = {1, 2, 3, 4};
    CHECK(c.Run(kA | kUp, stick) == (kA | kUp));
    CHECK(c.order == std::vector<std::string>{"page", "note", "held", "title", "dbno"});
    CHECK(c.noted == (kA | kUp));
}

TEST_CASE("pad order: a mod's hold of everything does not block our page, and the title and dbno still run") {
    Chain c;
    c.hold = CoopPadOrder::kHoldAll;
    std::int32_t stick[4] = {9, 9, 9, 9};
    CHECK(c.Run(kR | kA, stick) == 0);
    CHECK(c.order == std::vector<std::string>{"page", "note", "held", "title", "dbno"});   // (no early return)
    CHECK(c.noted == (kR | kA));   // (our page saw the R: it opens even with a mod's hold on)
    CHECK(c.title_saw == 0);       // (the title's edges follow what the game gets)
    CHECK(c.title_stick == 0);
    CHECK(c.dbno_saw == 0);
    CHECK(stick[0] == 0);
    CHECK(stick[1] == 0);
    CHECK(stick[2] == 0);
    CHECK(stick[3] == 0);
}

TEST_CASE("pad order: while our page shows, the mods see only what it passed on") {
    Chain c;
    c.page_shows = true;
    std::int32_t stick[4] = {5, 6, 7, 8};
    CHECK(c.Run(kA | kB | kUp, stick) == kB);
    CHECK(c.noted == kB);   // (no raw A or d-pad for a mod behind our page)
    CHECK(stick[1] == 6);   // (the left stick's up and down stay: the game's cursor over our rows)
}

TEST_CASE("pad order: a partial hold takes only its buttons") {
    Chain c;
    c.hold = kA;
    std::int32_t stick[4] = {1, 2, 3, 4};
    CHECK(c.Run(kA | kB, stick) == kB);
    CHECK(c.title_saw == kB);
    CHECK(stick[1] == 2);
}

TEST_CASE("pad order: a throw in the mods' part holds nothing and the rest still runs") {
    Chain c;
    c.mods_throw = true;
    c.hold = CoopPadOrder::kHoldAll;   // (never asked: the throw came first)
    std::int32_t stick[4] = {1, 2, 3, 4};
    CHECK(c.Run(kA, stick) == kA);
    CHECK(c.order == std::vector<std::string>{"page", "note", "title", "dbno"});
    CHECK(stick[0] == 1);
}
