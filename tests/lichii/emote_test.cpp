// Tests for emotes and pings (lichii/hook/coop_emote.cpp).
#include <doctest/doctest.h>

#include <string>

#include "coop_emote.h"

using CoopEmote::Command;
using CoopEmote::Holds;
using CoopEmote::Limiter;

TEST_CASE("emote: the table has unique ids and names, all with a game command") {
    const auto table = CoopEmote::Table();
    REQUIRE(!table.empty());
    for (std::size_t i = 0; i < table.size(); ++i) {
        CHECK(table[i].id >= 1);
        CHECK(table[i].id <= coop::protocol::kMaxEmoteId);
        CHECK(std::string(table[i].command) != "");
        CHECK(table[i].hold_ms > 0);
        CHECK(table[i].hold_ms <= CoopEmote::kMaxHoldMs);
        for (std::size_t j = i + 1; j < table.size(); ++j) {
            CHECK(table[i].id != table[j].id);
            CHECK(std::string(table[i].name) != table[j].name);
            CHECK(std::string(table[i].name) != table[j].alias);
        }
        CHECK(CoopEmote::ById(table[i].id) == &table[i]);
    }
    CHECK(CoopEmote::ById(0) == nullptr);
    CHECK(CoopEmote::ById(200) == nullptr);
}

TEST_CASE("emote: Parse takes the names, /emote <name> and /emote, and leaves chat and other commands") {
    CHECK(!CoopEmote::Parse("hello"));
    CHECK(!CoopEmote::Parse("/"));
    CHECK(!CoopEmote::Parse("/pvp on"));
    CHECK(!CoopEmote::Parse("/tp Link"));
    CHECK(!CoopEmote::Parse("/help"));
    CHECK(!CoopEmote::Parse("/wavey"));

    const auto wave = CoopEmote::Parse("/wave");
    REQUIRE(wave);
    CHECK(wave->kind == Command::Kind::play);
    CHECK(wave->def == CoopEmote::ByName("wave"));

    const auto same = CoopEmote::Parse("  /WAVE  ");
    REQUIRE(same);
    CHECK(same->def == wave->def);

    const auto alias = CoopEmote::Parse("/hi");
    REQUIRE(alias);
    CHECK(alias->def == wave->def);

    const auto via = CoopEmote::Parse("/emote here");
    REQUIRE(via);
    CHECK(via->kind == Command::Kind::play);
    CHECK(via->def == CoopEmote::ByName("here"));

    const auto list = CoopEmote::Parse("/emote");
    REQUIRE(list);
    CHECK(list->kind == Command::Kind::list);

    const auto bad = CoopEmote::Parse("/emote dance");
    REQUIRE(bad);
    CHECK(bad->kind == Command::Kind::unknown);
    CHECK(bad->name == "dance");
}

TEST_CASE("emote: the list line names every emote") {
    const std::string line = CoopEmote::ListLine();
    for (const auto& d : CoopEmote::Table()) {
        CHECK(line.find(std::string("/") + d.name) != std::string::npos);
    }
}

TEST_CASE("emote: the sender is limited to one per cooldown") {
    Limiter limiter;
    CHECK(limiter.Allow(10'000));
    CHECK(!limiter.Allow(10'000 + CoopEmote::kCooldownMs - 1));
    CHECK(limiter.Allow(10'000 + CoopEmote::kCooldownMs));
    CHECK(!limiter.Allow(10'000 + CoopEmote::kCooldownMs + 1));
}

TEST_CASE("emote: a hold plays the emote until it runs out") {
    Holds holds;
    const auto* wave = CoopEmote::ByName("wave");
    REQUIRE(holds.Start(2, wave->id, 1000));
    CHECK(holds.Apply(2, "Wait", 1000) == wave);
    CHECK(holds.Apply(2, "Wait", 1000 + wave->hold_ms - 1) == wave);
    CHECK(holds.Apply(2, "Wait", 1000 + wave->hold_ms) == nullptr);
    CHECK(holds.Apply(2, "Wait", 1000 + wave->hold_ms + 1) == nullptr);  // gone, not restarted
}

TEST_CASE("emote: the hold ends when the sender's animation changes") {
    Holds holds;
    const auto* wave = CoopEmote::ByName("wave");
    REQUIRE(holds.Start(3, wave->id, 0));
    CHECK(holds.Apply(3, "Wait", 10) == wave);
    CHECK(holds.Apply(3, "Wait", 20) == wave);
    CHECK(holds.Apply(3, "Run", 30) == nullptr);
    CHECK(holds.Apply(3, "Wait", 40) == nullptr);  // stays ended
}

TEST_CASE("emote: holds ignore bad ids and numbers, and are per player") {
    Holds holds;
    CHECK(!holds.Start(2, 0, 0));
    CHECK(!holds.Start(2, 250, 0));
    CHECK(!holds.Start(0, 1, 0));
    CHECK(!holds.Start(200, 1, 0));
    CHECK(holds.Apply(2, "Wait", 0) == nullptr);
    CHECK(holds.Apply(200, "Wait", 0) == nullptr);

    REQUIRE(holds.Start(2, 1, 0));
    CHECK(holds.Apply(3, "Wait", 0) == nullptr);  // another player: nothing
    CHECK(holds.Apply(2, "Wait", 0) != nullptr);
}

TEST_CASE("emote: a new emote replaces the old one, and Clear and ClearAll end them (a player left, a new connection)") {
    Holds holds;
    REQUIRE(holds.Start(2, 1, 0));
    CHECK(holds.Apply(2, "Wait", 0) == CoopEmote::ById(1));
    REQUIRE(holds.Start(2, 2, 100));
    CHECK(holds.Apply(2, "Wait", 100) == CoopEmote::ById(2));
    holds.Clear(2);
    CHECK(holds.Apply(2, "Wait", 100) == nullptr);

    REQUIRE(holds.Start(2, 1, 0));
    REQUIRE(holds.Start(5, 1, 0));
    holds.ClearAll();
    CHECK(holds.Apply(2, "Wait", 0) == nullptr);
    CHECK(holds.Apply(5, "Wait", 0) == nullptr);
}

TEST_CASE("emote: a late joiner has nothing: a fresh Holds plays nothing") {
    Holds late;
    for (std::uint8_t n = 1; n <= coop::protocol::kMaxPlayers; ++n) {
        CHECK(late.Apply(n, "Wait", 5000) == nullptr);
    }
}

TEST_CASE("emote: only a call is read in the others' chat, with the sender's name") {
    const auto* here = CoopEmote::ByName("here");
    REQUIRE(here != nullptr);
    CHECK(CoopEmote::HeardLine(*here, "Saria", 2) == "Saria calls: over here");
    CHECK(CoopEmote::HeardLine(*here, "  Saria ", 2) == "Saria calls: over here");
    CHECK(CoopEmote::HeardLine(*here, "", 3) == "Player 3 calls: over here");   // no name known yet
    for (const auto& d : CoopEmote::Table()) {
        if (&d != here) {
            CHECK(CoopEmote::HeardLine(d, "Saria", 2).empty());   // a gesture is seen, not read
        }
    }
}
