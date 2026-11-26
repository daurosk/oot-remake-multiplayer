// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "coop_char_ages.h"

using CoopCharAges::Input;
using CoopCharAges::Picks;
using CoopCharAges::Stored;

namespace {

// Installed NPC models: Saria 2, Zelda (adult era) 8, Sheik 9 (number -> file, as the puppet pack lists them).
const std::map<std::uint16_t, std::uint16_t> kFiles{{2, 2}, {8, 8}, {9, 9}};

Input At(bool child, std::int32_t pick) {
    Input in;
    in.childhood = child;
    in.pick = pick;
    return in;
}

} // namespace

TEST_CASE("char ages: off, no mod, no age, the title: nothing") {
    Picks p;
    CHECK_FALSE(p.Frame(At(true, 2), kFiles).write);
    Stored s;
    s.on = true;
    Picks on(s);
    Input no_mod;
    no_mod.childhood = true;
    CHECK_FALSE(on.Frame(no_mod, kFiles).write);
    Input no_age;
    no_age.pick = 2;
    CHECK_FALSE(on.Frame(no_age, kFiles).write);
    Input title = At(false, 2);
    title.title = true;
    CHECK_FALSE(on.Frame(title, kFiles).write);
    CHECK_FALSE(on.stored().adult);  // nothing learnt on the title
}

TEST_CASE("char ages: turning it on keeps what is shown, then each age has its own character") {
    Picks p;
    p.SetOn(true);
    auto st = p.Frame(At(true, 2), kFiles);  // a child playing Saria
    CHECK_FALSE(st.write);
    CHECK(st.stored_changed);
    CHECK(p.stored().child == 2);
    // He grows up: the mod's value still says Saria; the adult has no pick yet, so Saria it is for him too.
    st = p.Frame(At(false, 2), kFiles);
    CHECK_FALSE(st.write);
    CHECK(p.stored().adult == 2);
    // On his page as an adult he picks Zelda: stored for the adult only, not written (his page writes it).
    Input page = At(false, 2);
    page.page_open = true;
    CHECK_FALSE(p.Frame(page, kFiles).write);
    page.pick = 8;
    st = p.Frame(page, kFiles);
    CHECK_FALSE(st.write);
    CHECK(st.stored_changed);
    CHECK(p.stored().adult == 8);
    CHECK(p.stored().child == 2);
    CHECK_FALSE(p.Frame(At(false, 8), kFiles).write);  // the page closed on Zelda: nothing to do
    // Back in time: the child's Saria is written into the mod's value.
    st = p.Frame(At(true, 8), kFiles);
    REQUIRE(st.write);
    CHECK(*st.write == 2);
    CHECK_FALSE(st.stored_changed);
    // Taken: nothing more. Grown up again: Zelda.
    CHECK_FALSE(p.Frame(At(true, 2), kFiles).write);
    st = p.Frame(At(false, 2), kFiles);
    REQUIRE(st.write);
    CHECK(*st.write == 8);
}

TEST_CASE("char ages: a value changed by a save load is written over, never stored, re-written at most every 30") {
    Stored s;
    s.on = true;
    s.child = 2;
    s.adult = 9;
    Picks p(s);
    auto st = p.Frame(At(false, 0), kFiles);  // a save of Link loaded as an adult
    REQUIRE(st.write);
    CHECK(*st.write == 9);
    CHECK(p.stored().adult == 9);
    // Not taken yet: no second write for 30 frames, then again.
    for (int i = 0; i < Picks::kRewriteFrames; ++i) {
        CHECK_FALSE(p.Frame(At(false, 0), kFiles).write);
    }
    CHECK(p.Frame(At(false, 0), kFiles).write == 9);
}

TEST_CASE("char ages: a stored pick not installed shows Link and is kept, the settings rows set a pick") {
    Stored s;
    s.on = true;
    s.child = 77;  // a character whose model is not downloaded
    s.adult = 9;
    Picks p(s);
    const auto st = p.Frame(At(true, 9), kFiles);
    REQUIRE(st.write);
    CHECK(*st.write == 0);
    CHECK(p.stored().child == 77);
    p.SetPick(true, 2);
    CHECK(p.Frame(At(true, 0), kFiles).write == 2);
    // A swap still running: nothing written until it ends.
    Picks q(s);
    Input busy = At(true, 9);
    busy.swap_busy = true;
    CHECK_FALSE(q.Frame(busy, kFiles).write);
}

TEST_CASE("char ages: the settings line round-trips, garbage is refused") {
    Stored s;
    s.on = true;
    s.child = 2;
    CHECK(CoopCharAges::Format(s) == "1 2 -1\n");
    CHECK(CoopCharAges::Parse("1 2 -1\n") == s);
    CHECK(CoopCharAges::Parse("0 -1 9") == Stored{false, std::nullopt, 9});
    CHECK_FALSE(CoopCharAges::Parse(""));
    CHECK_FALSE(CoopCharAges::Parse("2 1 1"));
    CHECK_FALSE(CoopCharAges::Parse("1 x 3"));
    CHECK_FALSE(CoopCharAges::Parse("1 -5 3"));
}

TEST_CASE("char ages: the pickable characters of a characters.json") {
    // Made up: a row has a number, a name, a picker flag and sometimes a nested object.
    const char* text = R"({"format": 1, "mod": "Made Up Mod", "characters": [
        {"number": 0, "name": "Link", "picker": true},
        {"number": 1, "name": "Retro \"Link\"", "picker": false},
        {"number": 2, "name": "Saria", "picker": true, "file": 0, "bones": {"Child": 543, "Adult": 565}},
        {"number": 8, "name": "Zelda", "picker": true, "file": 6},
        {"name": "No number", "picker": true},
        {"number": 9, "picker": true},
        {"number": 10, "name": "Sheik", "picker": true, "file": 7}], "other": []})";
    const auto list = CoopCharAges::ParseCharacters(text);
    REQUIRE(list.size() == 4);
    CHECK(list[0].number == 0);
    CHECK(list[1].name == "Saria");
    CHECK(list[2].number == 8);
    CHECK(list[3].name == "Sheik");
    CHECK(CoopCharAges::ParseCharacters("").empty());
    CHECK(CoopCharAges::ParseCharacters(R"({"characters": [{"number": 2, "name": "Cut)").empty());
    CHECK(CoopCharAges::ParseCharacters(R"({"characters": 5})").empty());
}

TEST_CASE("char ages: the rows offered are Link and the installed ones") {
    const auto list = CoopCharAges::ParseCharacters(
        R"({"characters": [{"number": 0, "name": "Link", "picker": true},
            {"number": 2, "name": "Saria", "picker": true}, {"number": 3, "name": "Mido", "picker": true}]})");
    const auto some = CoopCharAges::Pickable(list, kFiles);   // only Saria (2) has a model
    REQUIRE(some.size() == 2);
    CHECK(some[0].number == 0);
    CHECK(some[1].number == 2);
    CHECK(CoopCharAges::Pickable(list, std::nullopt).size() == 3);   // unknown: all
    CHECK(CoopCharAges::Pickable(list, std::map<std::uint16_t, std::uint16_t>{}).size() == 1);   // none: Link
    CHECK(CoopCharAges::Pickable({}, std::nullopt).size() == 1);   // no list: Link
}

TEST_CASE("char ages: the models unknown (no pack): every pick counts as installed, a page pick is stored, an age change is written") {
    const std::optional<std::map<std::uint16_t, std::uint16_t>> unknown;
    Stored s;
    s.on = true;
    s.child = 77;
    s.adult = 9;
    Picks p(s);
    auto st = p.Frame(At(true, 0), unknown);
    REQUIRE(st.write);
    CHECK(*st.write == 77);
    Input page = At(true, 0);
    page.page_open = true;
    CHECK_FALSE(p.Frame(page, unknown).write);
    page.pick = 5;  // picked on his page
    st = p.Frame(page, unknown);
    CHECK(st.stored_changed);
    CHECK(p.stored().child == 5);
    CHECK(p.Frame(At(false, 5), unknown).write == 9);  // grown up: the adult's pick
    CHECK(CoopCharAges::IsInstalled(123, unknown));
}

TEST_CASE("char ages: a fight over the value is said once, not at every re-write") {
    Stored s;
    s.on = true;
    s.child = 2;
    s.adult = 9;
    Picks p(s);
    const auto first = p.Frame(At(true, 0), kFiles);
    CHECK(first.say);
    p.Wrote(*first.write);  // the game took it
    for (int i = 0; i < Picks::kRewriteFrames; ++i) p.Frame(At(true, 0), kFiles);
    const auto again = p.Frame(At(true, 0), kFiles);
    CHECK(again.write == 2);
    CHECK_FALSE(again.say);
    p.Wrote(*again.write);
    p.Frame(At(true, 2), kFiles);  // taken: settled
    CHECK(p.Frame(At(true, 0), kFiles).say);
}

TEST_CASE("char ages: a write the game did not take is said with the next one that it takes (the adult line)") {
    // Lane run 2026-11-23: the age change's first write came during the load and failed; the retry worked but was
    // not said, so the log had no adult line at all.
    Stored s;
    s.on = true;
    s.child = 2;
    s.adult = 9;
    Picks p(s);
    p.Frame(At(true, 2), kFiles);              // a child showing Saria: nothing to do
    const auto failed = p.Frame(At(false, 2), kFiles);   // grown up
    REQUIRE(failed.write == 9);
    CHECK(failed.say);
    // (no Wrote: the setter refused it)
    for (int i = 0; i < Picks::kRewriteFrames; ++i) CHECK_FALSE(p.Frame(At(false, 2), kFiles).write);
    const auto retry = p.Frame(At(false, 2), kFiles);
    CHECK(retry.write == 9);
    CHECK(retry.say);   // said now, with the write that works
}

TEST_CASE("char ages: the pick wanted for an age") {
    Stored s;
    s.on = true;
    s.child = 2;
    s.adult = 77;   // no model here
    CHECK(CoopCharAges::Wanted(s, true, kFiles) == 2);
    CHECK(CoopCharAges::Wanted(s, false, kFiles) == 0);   // shown as Link, the pick kept
    CHECK(CoopCharAges::Wanted(s, false, std::nullopt) == 77);   // models unknown: counted as installed
    CHECK_FALSE(CoopCharAges::Wanted(s, std::nullopt, kFiles));   // age unknown
    s.adult.reset();
    CHECK_FALSE(CoopCharAges::Wanted(s, false, kFiles));   // no pick for the adult yet
    s.on = false;
    CHECK_FALSE(CoopCharAges::Wanted(s, true, kFiles));
}

TEST_CASE("char ages: a save load writes the save's age pick at once and holds it through the load") {
    using CoopCharAges::LoadHold;
    LoadHold h;
    // The title load of the lane run: a child save whose own pick is 0, the child's pick 2.
    CHECK(h.Loaded(2, 0, 1000) == 2);   // written in place before the player is made
    CHECK(h.held());
    CHECK_FALSE(h.Applied(2, 1010));    // the game's applier left it
    CHECK(h.Applied(0, 1020) == 2);     // it put the save's 0 back: written again
    CHECK_FALSE(h.Applied(2, 1030));
    CHECK_FALSE(h.TakeSummary());       // still held: nothing said yet
    h.Expire(1000 + LoadHold::kHoldMs);
    CHECK_FALSE(h.held());
    const auto said = h.TakeSummary();
    REQUIRE(said);
    CHECK(said->want == 2);
    CHECK(said->passes == 3);
    CHECK(said->put_back == 1);
    CHECK(said->game_value == 0);
    CHECK_FALSE(h.TakeSummary());       // said once
    CHECK_FALSE(h.Applied(0, 999999));  // not held: the game's own changes are left alone
}

TEST_CASE("char ages: a save load with the pick already there, not held, stopped or timed out") {
    using CoopCharAges::LoadHold;
    LoadHold h;
    CHECK_FALSE(h.Loaded(9, 9, 0));   // an adult save that has Sheik already: nothing to write
    CHECK(h.held());                  // but held: the load may still bring another value
    CHECK(h.Applied(0, 5) == 9);
    h.Stop();                         // the frame: his page opened (the player picks)
    CHECK_FALSE(h.Applied(0, 6));
    CHECK(h.TakeSummary());
    CHECK_FALSE(h.Loaded(std::nullopt, 0, 10));   // off, no pick for the age, or the age unknown
    CHECK_FALSE(h.held());
    CHECK_FALSE(h.Loaded(2, std::nullopt, 10));   // the mod's value not there (no mod)
    CHECK_FALSE(h.held());
    CHECK(h.Loaded(2, 0, 100) == 2);
    CHECK_FALSE(h.Applied(0, 100 + LoadHold::kHoldMs));   // past the hold: left to the frame
    CHECK_FALSE(h.held());
    CHECK(h.Loaded(2, 0, 200) == 2);
    CHECK(h.Loaded(9, 2, 300) == 9);   // a second load (Try Again soon after): its own hold, the first one said
    REQUIRE(h.TakeSummary());
}

TEST_CASE("char ages: an age change soon after a save load ends the hold, the other age's pick is not put back") {
    using CoopCharAges::LoadHold;
    Stored s;
    s.on = true;
    s.child = 2;   // Saria
    s.adult = 9;   // Sheik
    LoadHold h;
    // An adult save (it loads at the Temple of Time): Sheik held.
    CHECK(h.Loaded(CoopCharAges::Wanted(s, false, kFiles), 0, 1000) == 9);
    h.Follow(s, false, kFiles, false);          // still the adult: held
    CHECK(h.Applied(0, 1010) == 9);
    // The Master Sword put back within the hold: Link is a child now, the frame writes Saria (2).
    h.Follow(s, true, kFiles, false);
    CHECK_FALSE(h.held());
    CHECK_FALSE(h.Applied(2, 1020));     // Saria stays: Sheik is not written over the child's pick
    CHECK_FALSE(h.Applied(0, 1030));     // and the game's own changes are the frame's again
    const auto said = h.TakeSummary();
    REQUIRE(said);
    CHECK(said->want == 9);
    CHECK(said->end == LoadHold::End::kWantChanged);
}

TEST_CASE("char ages: the hold follows the picks: age unknown keeps it, a pick set or turned off ends it") {
    using CoopCharAges::LoadHold;
    Stored s;
    s.on = true;
    s.child = 2;
    s.adult = 9;
    LoadHold h;
    CHECK(h.Loaded(2, 0, 0) == 2);
    h.Follow(s, std::nullopt, kFiles, false);   // the age not read this pass (mid-load): kept
    CHECK(h.held());
    h.Follow(s, true, kFiles, false);
    CHECK(h.held());
    s.child = 8;                         // the settings rows: another pick for the child
    h.Follow(s, true, kFiles, false);
    CHECK_FALSE(h.held());
    REQUIRE(h.TakeSummary());
    CHECK(h.Loaded(8, 0, 10) == 8);
    s.on = false;                        // the feature turned off
    h.Follow(s, true, kFiles, false);
    CHECK_FALSE(h.held());
    s.on = true;
    s.child = 77;                        // a pick with no model here: Link (0) wanted
    CHECK(h.Loaded(CoopCharAges::Wanted(s, true, kFiles), 2, 20) == 0);
    h.Follow(s, true, kFiles, false);
    CHECK(h.held());
    s.child.reset();                     // no pick for the age any more: nothing wanted, not held
    h.Follow(s, true, kFiles, false);
    CHECK_FALSE(h.held());
}

TEST_CASE("char ages: each hold counts its own put-backs and says how it ended") {
    using CoopCharAges::LoadHold;
    LoadHold h;
    CHECK(h.Loaded(2, 0, 0) == 2);
    CHECK(h.Applied(0, 1) == 2);
    CHECK(h.put_backs() == 1);           // the first of this hold: said
    CHECK(h.Applied(0, 2) == 2);
    CHECK(h.put_backs() == 2);
    CHECK(h.Loaded(2, 0, 100) == 2);     // System Load: a new hold
    auto said = h.TakeSummary();
    REQUIRE(said);
    CHECK(said->end == LoadHold::End::kNextLoad);
    CHECK(said->put_back == 2);
    CHECK(h.put_backs() == 0);
    CHECK(h.Applied(0, 101) == 2);
    CHECK(h.put_backs() == 1);           // its first is said too
    h.Stop();
    said = h.TakeSummary();
    REQUIRE(said);
    CHECK(said->end == LoadHold::End::kStopped);
    CHECK(h.Loaded(2, 0, 200) == 2);
    h.Expire(200 + LoadHold::kHoldMs);
    said = h.TakeSummary();
    REQUIRE(said);
    CHECK(said->end == LoadHold::End::kTime);
    CHECK(std::string(CoopCharAges::EndName(LoadHold::End::kWantChanged)) == "the age or the picks changed");
}

TEST_CASE("char ages: his page or swap ends the hold on the game's thread, the player's pick is not written over") {
    using CoopCharAges::LoadHold;
    Stored s;
    s.on = true;
    s.child = 2;
    s.adult = 9;
    LoadHold h;
    CHECK(h.Loaded(2, 0, 0) == 2);
    // His page opened and the player picked Zelda (8): the applier applied the page's write. The pass sees the page
    // before the frame does: the hold ends, 8 is left there.
    h.Follow(s, true, kFiles, true);
    CHECK_FALSE(h.held());
    CHECK_FALSE(h.Applied(8, 10));
    const auto said = h.TakeSummary();
    REQUIRE(said);
    CHECK(said->end == LoadHold::End::kStopped);
}
