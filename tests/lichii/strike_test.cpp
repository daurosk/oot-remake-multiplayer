// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the play-test strike (lichii/hook/coop_strike.h): which actor "kill [ID] NAME" and "hit [ID] NAME N" land on,
// and the result the walk leaves for the bridge. Names are made-up test strings.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "coop_strike.h"

using namespace CoopStrike;

namespace {
Candidate Actor(std::uint64_t address, std::string_view name, std::optional<std::int32_t> life, float metres) {
    Candidate c;
    c.address = address;
    c.name = name;
    c.life = life;
    c.metres = metres;
    return c;
}
}  // namespace

TEST_CASE("strike: a name alone strikes the nearest actor of that name with a life") {
    const std::vector<Candidate> actors{Actor(0x10, "Enemy_Test", 4, 9.0f), Actor(0x20, "Enemy_Test", 4, 3.0f),
                                        Actor(0x30, "Enemy_Test", 0, 1.0f), Actor(0x40, "Enemy_Other", 4, 0.5f)};
    const Pick pick = PickTarget(actors, "Enemy_Test", 0);
    CHECK(pick.result == kDone);
    CHECK(actors[pick.index].address == 0x20);
}

TEST_CASE("strike: an id strikes exactly that actor, even when another of the name is nearer (done)") {
    const std::vector<Candidate> actors{Actor(0x10, "Enemy_Test", 4, 9.0f), Actor(0x20, "Enemy_Test", 4, 3.0f)};
    const Pick pick = PickTarget(actors, "Enemy_Test", 0x10);
    CHECK(pick.result == kDone);
    CHECK(actors[pick.index].address == 0x10);
}

TEST_CASE("strike: an id that is no longer in the walk's list is gone, and never another actor of the name") {
    const std::vector<Candidate> actors{Actor(0x20, "Enemy_Test", 4, 3.0f)};
    CHECK(PickTarget(actors, "Enemy_Test", 0x10).result == kGone);
    CHECK(PickTarget({}, "Enemy_Test", 0x10).result == kGone);
}

TEST_CASE("strike: an id the world list shows but the walk does not share is not shared, not gone") {
    const std::vector<Candidate> actors{Actor(0x20, "Enemy_Test", 4, 3.0f)};
    const std::vector<Listed> listed{{0x50, "Enemy_Shot"}};
    CHECK(PickTarget(actors, "Enemy_Shot", 0x50, listed).result == kNotShared);
    CHECK(PickTarget(actors, "Enemy_Other", 0x50, listed).result == kGone);   // another name at that address
    CHECK(PickTarget(actors, "Enemy_Shot", 0x60, listed).result == kGone);    // not listed at all
    CHECK(PickTarget(actors, "Enemy_Shot", 0x50).result == kGone);            // no list given
    CHECK(PickTarget(actors, "Enemy_Test", 0x20, listed).result == kDone);    // shared wins
}

TEST_CASE("strike: an id whose actor now has another name is gone") {
    const std::vector<Candidate> actors{Actor(0x10, "Enemy_Other", 4, 9.0f), Actor(0x20, "Enemy_Test", 4, 3.0f)};
    CHECK(PickTarget(actors, "Enemy_Test", 0x10).result == kGone);
}

TEST_CASE("strike: the actor with no life left, or no life at all, is none with a life") {
    const std::vector<Candidate> actors{Actor(0x10, "Enemy_Test", 0, 1.0f), Actor(0x20, "Enemy_Test", std::nullopt, 2.0f),
                                        Actor(0x30, "Enemy_Test", 5, 3.0f)};
    CHECK(PickTarget(actors, "Enemy_Test", 0x10).result == kNoLife);
    CHECK(PickTarget(actors, "Enemy_Test", 0x20).result == kNoLife);
    CHECK(PickTarget(actors, "Enemy_Nobody", 0).result == kNoLife);
    const std::vector<Candidate> dead{Actor(0x10, "Enemy_Test", 0, 1.0f)};
    CHECK(PickTarget(dead, "Enemy_Test", 0).result == kNoLife);
}

TEST_CASE("strike: each strike gets the next number, and the book keeps the newest result") {
    Book book;
    CHECK_FALSE(book.Last());
    CHECK(book.LastAsked() == 0);
    CHECK(book.Asked() == 1);
    CHECK(book.Asked() == 2);
    CHECK(book.LastAsked() == 2);
    book.Record({1, "gone", "Enemy_Test", 16});
    book.Record({2, "done", "Enemy_Test", 0});
    REQUIRE(book.Last());
    CHECK(book.Last()->seq == 2);
    CHECK(book.Last()->result == "done");
    book.Clear();
    CHECK_FALSE(book.Last());
    CHECK(book.LastAsked() == 0);
}

TEST_CASE("strike: a Not shared Gold Skulltula is this game's own mob, which a test kill may kill here") {
    // Row 63, race mode: the shared list does not have it (not shared), the own-mob list does, and the kill lands
    // there, by name or by id. Shared mode, or any other unshared actor: never an own mob.
    CHECK(OwnGameMob("Enemy_Skulltula_Gold", true));
    CHECK_FALSE(OwnGameMob("Enemy_Skulltula_Gold", false));
    CHECK_FALSE(OwnGameMob("Enemy_Skulltula", true));
    CHECK_FALSE(OwnGameMob("Scrub_Deku_Brothers_Manager", true));

    const std::vector<Candidate> shared{Actor(0x10, "Enemy_Test", 4, 2.0f)};
    const std::vector<Listed> listed{{0x50, "Enemy_Skulltula_Gold"}};
    CHECK(PickTarget(shared, "Enemy_Skulltula_Gold", 0x50, listed).result == kNotShared);
    CHECK(PickTarget(shared, "Enemy_Skulltula_Gold", 0).result == kNoLife);

    const std::vector<Candidate> own{Actor(0x50, "Enemy_Skulltula_Gold", 1, 1.2f),
                                     Actor(0x60, "Enemy_Skulltula_Gold", 0, 0.5f)};
    const Pick by_name = PickTarget(own, "Enemy_Skulltula_Gold", 0);
    CHECK(by_name.result == kDone);
    CHECK(own[by_name.index].address == 0x50);  // the dead one nearer is skipped
    const Pick by_id = PickTarget(own, "Enemy_Skulltula_Gold", 0x50);
    CHECK(by_id.result == kDone);
    CHECK(own[by_id.index].address == 0x50);
}
