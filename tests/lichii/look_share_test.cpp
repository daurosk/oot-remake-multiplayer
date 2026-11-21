// SPDX-License-Identifier: GPL-3.0-or-later
// Each player's look of several parts over the network (lichii/hook/coop_look_share.h).
#include <doctest/doctest.h>

#include "coop/net/client.hpp"
#include "coop_look_share.h"
#include "coop_puppet.h"

using coop::net::Event;
using coop::net::EventKind;
using coop::protocol::LookPart;
using coop::protocol::PlayerLook;

namespace {
Event Kind(EventKind kind, std::uint8_t player = 0) {
    Event e;
    e.kind = kind;
    e.player = player;
    return e;
}
Event Look(std::uint8_t player, CoopLookShare::Parts parts) {
    Event e;
    e.kind = EventKind::message;
    e.message = PlayerLook{player, std::move(parts)};
    return e;
}
}  // namespace

TEST_CASE("look share: ours goes out once per change, and again for a newcomer") {
    CoopLookShare::Share s;
    CHECK_FALSE(s.TakeSend());                 // nothing chosen: nothing sent
    CHECK_FALSE(s.OnEvent(Kind(EventKind::player_joined, 2)));
    CHECK_FALSE(s.TakeSend());                 // a join with no look chosen sends nothing
    s.Set({{3, 0xE0C030}, {4, 0x20A040}});
    const auto sent = s.TakeSend();
    REQUIRE(sent);
    CHECK(sent->player_number == coop::protocol::kPlayerSelf);
    CHECK(sent->parts == CoopLookShare::Parts{{3, 0xE0C030}, {4, 0x20A040}});
    CHECK_FALSE(s.TakeSend());                 // once
    s.OnEvent(Kind(EventKind::player_joined, 3));
    CHECK(s.TakeSend());                       // the late joiner gets it
    s.OnEvent(Kind(EventKind::reconnected));
    CHECK(s.TakeSend());                       // and a server that lost us
    s.Set({});
    const auto cleared = s.TakeSend();         // everything back to the game's own: an empty look goes out
    REQUIRE(cleared);
    CHECK(cleared->parts.empty());
}

TEST_CASE("look share: the others' looks are kept by player and forgotten when they leave") {
    CoopLookShare::Share s;
    CHECK_FALSE(s.Of(2));
    CHECK(s.OnEvent(Look(2, {{11, 0xFF0000}})));
    CHECK_FALSE(s.OnEvent(Look(2, {{11, 0xFF0000}})));   // the same again (a resend for a newcomer)
    CHECK(s.Of(2) == CoopLookShare::Parts{{11, 0xFF0000}});
    CHECK(s.OnEvent(Look(2, {})));                        // back to the game's own
    CHECK(s.Of(2) == CoopLookShare::Parts{});
    CHECK_FALSE(s.OnEvent(Look(coop::protocol::kPlayerSelf, {{1, 1}})));   // not relayed: no player
    CHECK_FALSE(s.Of(coop::protocol::kPlayerSelf));
    CHECK(s.OnEvent(Kind(EventKind::player_left, 2)));
    CHECK_FALSE(s.Of(2));
}

TEST_CASE("look share: every player gets the lowest free slot, gear colours or not, kept until they leave") {
    CoopLookShare::Share s;
    s.OnEvent(Look(2, {{3, 0xFF0000}}));
    CHECK_FALSE(s.SlotOf(2));                 // slots off (no painter): none
    s.EnableSlots(true);
    CHECK(s.SlotOf(2) == 1u);                 // given once the painter is on
    s.OnEvent(Look(5, {{0, 0x3070FF}}));      // the tunic alone: a slot too (held-sword: the puppet
    CHECK(s.SlotOf(5) == 2u);                 // wears its own copy of the sword, in the game's colours)
    s.OnEvent(Look(5, {{0, 0x3070FF}, {11, 0x202020}}));
    CHECK(s.SlotOf(5) == 2u);                 // a gear colour keeps it
    s.OnEvent(Look(2, {{3, 0x00FF00}}));      // a new colour keeps the slot
    CHECK(s.SlotOf(2) == 1u);
    s.OnEvent(Kind(EventKind::player_left, 2));
    CHECK_FALSE(s.SlotOf(2));
    s.OnEvent(Look(7, {{4, 1}}));
    CHECK(s.SlotOf(7) == 1u);                 // the freed slot again
    s.OnEvent(Look(7, {}));                   // back to the game's own colours: the slot is kept
    CHECK(s.SlotOf(7) == 1u);
    s.OnEvent(Kind(EventKind::player_left, 7));
    for (std::uint8_t n = 8; n < 8 + CoopLookShare::kSlots; ++n) {
        s.OnEvent(Look(n, {{4, 1}}));
    }
    CHECK_FALSE(s.SlotOf(8 + CoopLookShare::kSlots - 1));   // all taken: the game's items
    CHECK_FALSE(s.SlotOf(0));
    s.EnableSlots(false);
    CHECK_FALSE(s.SlotOf(5));
}

TEST_CASE("look share: a new session forgets the last one's looks and slots") {
    CoopLookShare::Share s;
    s.EnableSlots(true);
    s.OnEvent(Look(2, {{3, 0xFF0000}}));
    REQUIRE(s.SlotOf(2));
    CHECK(s.OnEvent(Kind(EventKind::welcomed)));
    CHECK_FALSE(s.Of(2));
    CHECK_FALSE(s.SlotOf(2));
    s.OnEvent(Look(2, {{3, 0xFF0000}}));
    s.OnEvent(Kind(EventKind::reconnected));   // the same session again: kept
    CHECK(s.SlotOf(2) == 1u);
}

TEST_CASE("look share: a puppet wears its slot's copy from the start (row 198), then the other copy for a new look") {
    using Job = CoopLookShare::Share::Job;
    constexpr std::uint64_t later = CoopLookShare::kCopyDwellMs;
    CoopLookShare::Share s;
    s.EnableSlots(true);
    s.OnEvent(Look(3, {{11, 0xFF0000}}));
    s.OnEvent(Look(4, {{4, 0x00FF00}}));
    // (row 198) Nothing painted yet: the copy the first job paints, marked; never the game's own items, whose
    // textures your own Link's colours are drawn over.
    CHECK(s.CopyOf(3) == 1u);
    CHECK(s.CopyOf(4) == 3u);
    auto jobs = s.Jobs(0);
    REQUIRE(jobs.size() == 2);
    CHECK(jobs[0] == Job{1, 1, 3, {{11, 0xFF0000}}});   // slot 1: copies 1 and 2
    CHECK(jobs[1] == Job{2, 3, 4, {{4, 0x00FF00}}});    // slot 2: copies 3 and 4
    s.Done(jobs[0], 1000);
    CHECK(s.CopyOf(3) == 1u);
    CHECK(s.CopyOf(4) == 3u);                   // (still the copy its job paints)
    CHECK(s.Jobs(1000).size() == 1);            // painted once
    s.OnEvent(Look(3, {{11, 0x0000FF}}));       // a new colour: the other copy is painted...
    CHECK(s.Jobs(1500).size() == 1);            // ...but not while the puppet may still be moving onto copy 1
    jobs = s.Jobs(1000 + later);
    REQUIRE(jobs.size() == 2);
    CHECK(jobs[0] == Job{1, 2, 3, {{11, 0x0000FF}}});
    CHECK(s.CopyOf(3) == 1u);                   // ...while the puppet keeps the one it wears
    s.Done(jobs[0], 1000 + later);
    CHECK(s.CopyOf(3) == 2u);
    s.OnEvent(Look(3, {{11, 0x00FFFF}}));
    CHECK(s.Jobs(1000 + 3 * later)[0].copy == 1u);   // and back

    // Freed and given again: never the copy painted last (a puppet may still wear it).
    s.OnEvent(Kind(EventKind::player_left, 3));   // (no colours keeps the slot since held-sword)
    CHECK_FALSE(s.CopyOf(3));                     // (no slot: none)
    s.OnEvent(Look(3, {{11, 0x101010}}));
    CHECK(s.CopyOf(3) == 1u);                     // (row 198: back at once, on the copy its job paints)
    CHECK(s.Jobs(1000 + 3 * later)[0] == Job{1, 1, 3, {{11, 0x101010}}});   // copy 2 was painted last
    s.OnEvent(Kind(EventKind::welcomed));       // a new session: the same, for every slot
    s.OnEvent(Look(6, {{3, 1}}));
    CHECK(s.Jobs(1000 + 3 * later)[0].copy == 1u);

    // A slot that changed hands while its copy was painted: not the new player's, and not painted again next.
    s.OnEvent(Look(7, {{4, 1}}));
    jobs = s.Jobs(1000 + 3 * later);
    REQUIRE(jobs.size() == 2);
    const Job late = jobs[1];
    CHECK(late.player == 7);
    s.OnEvent(Kind(EventKind::player_left, 7));
    s.OnEvent(Look(5, {{4, 1}}));
    s.Done(late, 100);                          // P7's job, done late
    CHECK(s.CopyOf(5) != late.copy);            // (not P7's colours)
    CHECK_FALSE(s.CopyOf(7));
    const auto next = s.Jobs(1000 + 5 * later);
    REQUIRE(next.size() == 2);
    CHECK(next[1].player == 5);
    CHECK(next[1].copy != late.copy);
    CHECK(s.CopyOf(5) == next[1].copy);         // (the copy P5 wears is the one painted for them)
}

TEST_CASE("look items: the pack's item copies are read from its manifest") {
    const auto items = CoopPuppet::PackLookItems(
        R"({"look_files": {}, "look_items": {"slots": 4, "copies": 8, "actors": ["Weapon_Shield_Hylian", "Armor_Adult_Zora"], "files": ["Pack/Actor/X.pack.zs"]}, "x": 1})");
    CHECK(items.copies == 8);
    CHECK(items.actors == std::vector<std::string>{"Weapon_Shield_Hylian", "Armor_Adult_Zora"});
    CHECK(CoopPuppet::PackLookItems(R"({"look_items": null})").copies == 0);
    CHECK(CoopPuppet::PackLookItems(R"({"look_items": {"slots": 4, "copies": 8, "neutral": 9, "actors": ["A"]}})")
              .neutral == 9u);   // (row 198)
    CHECK(CoopPuppet::PackLookItems(R"({"look_items": {"slots": 4, "copies": 8, "actors": ["A"]}})").neutral == 0u);
    CHECK(CoopPuppet::PackLookItems(R"({"actors": []})").copies == 0);
    CHECK(CoopPuppet::PackLookItems(R"({"look_items": {"copies": 2, "actors": []}})").copies == 0);
    CHECK(CoopPuppet::PackLookItems("").actors.empty());
}

TEST_CASE("look items: a puppet wears its painted copy of the items the pack has") {
    CoopPuppet::PlannerSettings settings;
    settings.look_items = {"Weapon_Shield_Hylian", "Weapon_Sword_Kokiri"};
    settings.look_copies = 2;
    CoopPuppet::Planner p(settings);
    std::vector<CoopPuppet::Remote> remotes(3);
    for (std::size_t i = 0; i < remotes.size(); ++i) {
        remotes[i].number = static_cast<std::uint8_t>(i + 2);
        remotes[i].scene = "Kokiri";
        remotes[i].gear[0] = "Weapon_Sword_Kokiri";
        remotes[i].gear[1] = "Weapon_Shield_Deku";   // no copy in the pack: the game's own
    }
    remotes[0].look_copy = 2;
    remotes[2].look_copy = 3;                         // more than the pack has: the game's items
    const auto in = p.Plan(0x8000000, std::string("Kokiri"), CoopPuppet::Vec3{0, 0, 0}, false, remotes, 1000);
    REQUIRE(in.targets.size() == 3);
    CHECK(in.targets[0].gear[0] == std::string("Weapon_Sword_Kokiri_Look2"));
    CHECK(in.targets[0].gear[1] == std::string("Weapon_Shield_Deku"));
    CHECK(in.targets[1].gear[0] == std::string("Weapon_Sword_Kokiri"));   // no slot
    CHECK(in.targets[2].gear[0] == std::string("Weapon_Sword_Kokiri"));
    CHECK(CoopPuppet::LookItemName("Armor_Adult_Zora", 1) == "Armor_Adult_Zora_Look1");
}

TEST_CASE("blade copies: every puppet with no ready Look copy wears its own number's sword copy") {
    // held-piece: the blade is a switch of the sword's model resource, shared by every actor of one
    // name. A 5th or later player (no Look slot left), a player whose copy is not painted yet, a
    // Longshot without patch 0020 and the emulator (no slots at all) wear <sword>_Blade<number>.
    const auto blades = CoopPuppet::PackLookItems(
        R"({"look_items": null, "blade_items": {"copies": 16, "actors": ["Weapon_Sword_Kokiri", "Weapon_Sword_Giant"], "files": []}})",
        "blade_items");
    CHECK(blades.copies == 16);
    CHECK(blades.actors == std::vector<std::string>{"Weapon_Sword_Kokiri", "Weapon_Sword_Giant"});
    CHECK(CoopPuppet::PackLookItems(R"({"look_items": null, "blade_items": null})", "blade_items").copies == 0);
    CHECK(CoopPuppet::PackLookItems(R"({"blade_items": {"copies": 16, "actors": ["A"]}})").copies == 0);  // not look_items
    CHECK(CoopPuppet::BladeItemName("Weapon_Sword_Kokiri", 5) == "Weapon_Sword_Kokiri_Blade5");

    CoopPuppet::PlannerSettings settings;
    settings.look_items = {"Weapon_Shield_Hylian", "Weapon_Sword_Kokiri"};
    settings.look_copies = 8;
    settings.blade_items = blades.actors;
    settings.blade_copies = 16;
    CoopPuppet::Planner p(settings);
    std::vector<CoopPuppet::Remote> remotes(4);
    const std::uint8_t numbers[] = {2, 6, 16, 9};
    for (std::size_t i = 0; i < remotes.size(); ++i) {
        remotes[i].number = numbers[i];
        remotes[i].scene = "Kokiri";
        remotes[i].gear[0] = "Weapon_Sword_Kokiri";
        remotes[i].gear[1] = "Weapon_Shield_Hylian";
    }
    remotes[0].look_copy = 1;                       // painted Look copy: that one, for every piece
    remotes[3].gear[0] = "Weapon_Sword_Giant";      // no Look copy listed here: the blade copy
    const auto in = p.Plan(0x8000000, std::string("Kokiri"), CoopPuppet::Vec3{0, 0, 0}, false, remotes, 1000);
    REQUIRE(in.targets.size() == 4);
    CHECK(in.targets[0].gear[0] == std::string("Weapon_Sword_Kokiri_Look1"));
    CHECK(in.targets[0].gear[1] == std::string("Weapon_Shield_Hylian_Look1"));
    CHECK(in.targets[1].gear[0] == std::string("Weapon_Sword_Kokiri_Blade6"));   // a 5th+ player: no slot
    CHECK(in.targets[1].gear[1] == std::string("Weapon_Shield_Hylian"));         // no neutral copy: the game's
    CHECK(in.targets[2].gear[0] == std::string("Weapon_Sword_Kokiri_Blade16"));
    CHECK(in.targets[3].gear[0] == std::string("Weapon_Sword_Giant_Blade9"));

    // Row 198: with the pack's neutral copy, a puppet with no copy of its own wears it for every coloured piece but
    // its sword's own wearer copy: never the game's item, whose texture your own colour is drawn over.
    settings.look_neutral = 9;
    CoopPuppet::Planner pn(settings);
    const auto withn = pn.Plan(0x8000000, std::string("Kokiri"), CoopPuppet::Vec3{0, 0, 0}, false, remotes, 1000);
    REQUIRE(withn.targets.size() == 4);
    CHECK(withn.targets[0].gear[1] == std::string("Weapon_Shield_Hylian_Look1"));   // (its own copy: as before)
    CHECK(withn.targets[1].gear[0] == std::string("Weapon_Sword_Kokiri_Blade6"));   // (its own wearer copy)
    CHECK(withn.targets[1].gear[1] == std::string("Weapon_Shield_Hylian_Look9"));   // (the neutral copy)
    settings.look_neutral = 0;

    // A pack with fewer numbers than the player's: the game's own sword, as before.
    CoopPuppet::Gear gear;
    gear[0] = "Weapon_Sword_Kokiri";
    CoopPuppet::DressInBlades(gear, 9, settings.blade_items, 8);
    CHECK(gear[0] == std::string("Weapon_Sword_Kokiri"));
    CoopPuppet::DressInBlades(gear, 0, settings.blade_items, 16);   // the debug puppet
    CHECK(gear[0] == std::string("Weapon_Sword_Kokiri"));
}

TEST_CASE("look share: a new room forgets the last room's looks, a reconnect keeps them") {
    CoopLookShare::Share s;
    CHECK_FALSE(s.OnEvent(Kind(EventKind::welcomed)));   // nothing to forget
    CHECK(s.OnEvent(Look(2, {{11, 0xFF0000}})));
    CHECK_FALSE(s.OnEvent(Kind(EventKind::reconnected)));
    CHECK(s.Of(2));                                     // the same room: player 2 is still that player
    CHECK(s.OnEvent(Kind(EventKind::welcomed)));         // another room: its player 2 is someone else
    CHECK_FALSE(s.Of(2));
    s.Set({{3, 0xE0C030}});
    s.TakeSend();
    s.OnEvent(Kind(EventKind::welcomed));
    CHECK(s.TakeSend());                                // ours goes to the new room
}

TEST_CASE("look share: a player with no gear colours wears their own copy, painted in the game's colours") {
    // held-sword: a puppet in the game's own Weapon_Sword_Kokiri shares that model's blade switch
    // with our own Link's sword and showed no blade while ours was put away; its own copy has its own.
    CoopLookShare::Share s;
    s.EnableSlots(true);
    s.OnEvent(Look(4, {}));
    CHECK(s.SlotOf(4) == 1u);
    CHECK(s.CopyOf(4) == 1u);                   // not painted yet: its copy all the same (row 198), the game's colours
    const auto jobs = s.Jobs(0);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].player == 4);
    CHECK(jobs[0].copy == 1u);
    CHECK(jobs[0].parts.empty());               // no colours: the copy keeps the game's own texels
    s.Done(jobs[0], 100);
    CHECK(s.CopyOf(4) == 1u);
}

TEST_CASE("look share: a look with no gear set at start goes out, and the other game gives that player a copy") {
    // held-sword: the module sets its look at start even with no gear colours (coop_look_live.cpp
    // StartModule), so the others dress that player's puppet in its own copy of the sword.
    CoopLookShare::Share ours;
    ours.Set({});
    const auto sent = ours.TakeSend();
    REQUIRE(sent);
    CHECK(sent->parts.empty());
    CoopLookShare::Share theirs;
    theirs.EnableSlots(true);
    theirs.OnEvent(Look(2, sent->parts));      // as relayed, from player 2
    CHECK(theirs.SlotOf(2) == 1u);
    ours.OnEvent(Kind(EventKind::player_joined, 3));
    CHECK(ours.TakeSend());                    // and again for a late joiner
}

// Character Select (github.com/snaildev99/OoT-Character-Select): the picked character with the look.
TEST_CASE("look share: our picked character goes out as a part of its own, with or without colours") {
    CoopLookShare::Share s;
    s.SetCharacter(0);
    CHECK_FALSE(s.TakeSend());                 // Link, and no look chosen: nothing sent, as before
    s.SetCharacter(9);
    auto sent = s.TakeSend();
    REQUIRE(sent);                             // a character alone sends a look
    CHECK(sent->parts == CoopLookShare::Parts{{CoopLookShare::kCharacterPart, CoopLookShare::kCharacterMark | 9u}});
    CHECK_FALSE(s.TakeSend());                 // once
    s.SetCharacter(9);
    CHECK_FALSE(s.TakeSend());                 // the same number again is no change
    s.Set({{3, 0xE0C030}});
    sent = s.TakeSend();
    REQUIRE(sent);                             // the colours first, the character last
    CHECK(sent->parts == CoopLookShare::Parts{{3, 0xE0C030}, {CoopLookShare::kCharacterPart, CoopLookShare::kCharacterMark | 9u}});
    s.OnEvent(Kind(EventKind::player_joined, 3));
    CHECK(s.TakeSend());                       // a newcomer gets it
    s.SetCharacter(0);
    sent = s.TakeSend();
    REQUIRE(sent);                             // back to Link: the look without the part
    CHECK(sent->parts == CoopLookShare::Parts{{3, 0xE0C030}});
    CoopLookShare::Share alone;
    alone.SetCharacter(4);
    CHECK(alone.TakeSend());
    alone.SetCharacter(0);
    sent = alone.TakeSend();
    REQUIRE(sent);                             // with no colours ever chosen too: an empty look says Link again
    CHECK(sent->parts.empty());
    alone.OnEvent(Kind(EventKind::welcomed));
    CHECK(alone.TakeSend());                   // and a new session hears it
    alone.SetCharacter(60000);
    CHECK_FALSE(alone.TakeSend());             // a number that is no character counts as Link: no change
}

TEST_CASE("look share: a player's character is kept apart from the colours of their look") {
    CoopLookShare::Share s;
    s.EnableSlots(true);
    CHECK(s.CharacterOf(2) == 0);
    const LookPart nine{CoopLookShare::kCharacterPart, CoopLookShare::kCharacterMark | 9u};
    CHECK(s.OnEvent(Look(2, {{11, 0xFF0000}, nine})));
    CHECK(s.CharacterOf(2) == 9);
    CHECK(s.Of(2) == CoopLookShare::Parts{{11, 0xFF0000}});   // what paints a look never sees the part
    CHECK(s.SlotOf(2) == 1);
    CHECK_FALSE(s.OnEvent(Look(2, {{11, 0xFF0000}, nine})));  // the same again
    CHECK(s.OnEvent(Look(2, {{11, 0xFF0000}, {CoopLookShare::kCharacterPart, CoopLookShare::kCharacterMark | 15u}})));
    CHECK(s.CharacterOf(2) == 15);                            // another pick is a change
    CHECK(s.OnEvent(Look(2, {{11, 0xFF0000}})));              // no part: Link again
    CHECK(s.CharacterOf(2) == 0);
    // A part 63 that is not ours (no mark, or a number out of range) is no character, and no colour either.
    CHECK_FALSE(s.OnEvent(Look(2, {{11, 0xFF0000}, {CoopLookShare::kCharacterPart, 0x000009}})));
    CHECK(s.CharacterOf(2) == 0);
    CHECK(s.Of(2) == CoopLookShare::Parts{{11, 0xFF0000}});
    CHECK_FALSE(s.OnEvent(Look(2, {{11, 0xFF0000}, {CoopLookShare::kCharacterPart, CoopLookShare::kCharacterMark | 0xFFFFu}})));
    CHECK(s.CharacterOf(2) == 0);
    CHECK(s.OnEvent(Look(3, {nine})));
    CHECK(s.CharacterOf(3) == 9);
    s.OnEvent(Kind(EventKind::player_left, 3));
    CHECK(s.CharacterOf(3) == 0);                             // gone with the player
    CHECK(s.OnEvent(Look(2, {nine})));
    s.OnEvent(Kind(EventKind::welcomed));
    CHECK(s.CharacterOf(2) == 0);                             // and with the session
}

TEST_CASE("look share: a new pick of character alone paints no copy again") {
    // Review of #299 (7): only the character changed, the colours did not: the slot's copy stays painted.
    CoopLookShare::Share s;
    s.EnableSlots(true);
    const LookPart nine{CoopLookShare::kCharacterPart, CoopLookShare::kCharacterMark | 9u};
    const LookPart ten{CoopLookShare::kCharacterPart, CoopLookShare::kCharacterMark | 10u};
    s.OnEvent(Look(2, {{11, 0xFF0000}, nine}));
    auto jobs = s.Jobs(0);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].parts == CoopLookShare::Parts{{11, 0xFF0000}});
    s.Done(jobs[0], 0);
    CHECK(s.OnEvent(Look(2, {{11, 0xFF0000}, ten})));   // a change (the puppet's actor)...
    CHECK(s.Jobs(CoopLookShare::kCopyDwellMs * 2).empty());   // ...but no repaint
    s.OnEvent(Look(3, {nine}));                          // a character and no colours: its copy once, as any look
    jobs = s.Jobs(CoopLookShare::kCopyDwellMs * 2);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].parts.empty());
    s.Done(jobs[0], 0);
    s.OnEvent(Look(3, {ten}));
    CHECK(s.Jobs(CoopLookShare::kCopyDwellMs * 4).empty());
}

TEST_CASE("look share: a guest on the host's save sends its own pick, not the host's") {
    // Review of #299 (5): the host's save carries the host's pick.
    CoopLookShare::OwnPick p;
    CHECK(p.Update(0, false, 0) == 0);
    CHECK(p.Update(12, false, 0) == 12);       // our own save: what we picked
    CHECK(p.TakeOwnChange() == std::optional<std::uint64_t>(12));
    CHECK_FALSE(p.TakeOwnChange());            // once
    CHECK(p.Update(30, true, 1) == 12);        // the host's save loads with the host's 30: ours goes out
    CHECK(p.Update(30, true, 1) == 12);
    CHECK_FALSE(p.TakeOwnChange());            // the host's pick is never kept as ours
    CHECK(p.Update(44, true, 1) == 44);        // the guest picks on the host's save: that one
    CHECK(p.Update(30, true, 1) == 30);        // even back to the host's number: it is the guest's pick now
    CHECK(p.Update(30, true, 2) == 12);        // a new load of the host's save: its number is the host's again
    CHECK(p.Update(0, true, 3) == 12);         // a host as Link: still ours
    CHECK(p.Update(0, false, 3) == 0);         // back on our own save (as Link): that
    CoopLookShare::OwnPick seeded;
    seeded.Seed(7);                            // kept from the last start (coop-character.txt)
    CHECK_FALSE(seeded.TakeOwnChange());
    CHECK(seeded.Update(30, true, 0) == 7);    // a guest that joined before playing its own save
    CoopLookShare::OwnPick fresh;
    CHECK(fresh.Update(30, true, 0) == 0);     // nothing known: Link, never the host's
    // The Customize options are in the save too: they go with the pick (OwnPick::Of), the guest's own until it
    // changes either on the host's save (review of 07de2fe2, MUST 1).
    using CoopLookShare::OwnPick;
    CHECK(OwnPick::CharacterIn(OwnPick::Of(9, 0x123456)) == 9);
    CHECK(OwnPick::CustomIn(OwnPick::Of(9, 0x123456)) == 0x123456u);
    CHECK(OwnPick::CustomIn(OwnPick::Of(9, 0xFF123456u)) == 0x123456u);
    OwnPick g;
    CHECK(g.Update(OwnPick::Of(9, 0x000011), false, 0) == OwnPick::Of(9, 0x000011));
    CHECK(g.Update(OwnPick::Of(30, 0x222222), true, 1) == OwnPick::Of(9, 0x000011));   // the host's pick and options
    // The options are unsaved (0 after a load): one changed on the host's save is no pick of the guest's own.
    CHECK(g.Update(OwnPick::Of(30, 0x222223), true, 1) == OwnPick::Of(9, 0x000011));
    CHECK(g.Update(OwnPick::Of(44, 0x222223), true, 1) == OwnPick::Of(44, 0x222223));  // a pick: it and its options
    CHECK(g.Update(OwnPick::Of(44, 0x000005), true, 1) == OwnPick::Of(44, 0x000005));  // then each option change
    CHECK(g.Update(OwnPick::Of(30, 0x222222), true, 2) == OwnPick::Of(9, 0x000011));   // a new load: ours again
    CHECK(g.TakeOwnChange() == std::optional<std::uint64_t>(OwnPick::Of(9, 0x000011)));
}

TEST_CASE("look share: a character's Customize options ride along as a part of their own") {
    CoopLookShare::Share s;
    s.SetCustom(0x123456);
    CHECK_FALSE(s.TakeSend());                 // Link: nothing of it is sent
    s.SetCharacter(9);
    auto sent = s.TakeSend();
    REQUIRE(sent);
    const LookPart nine{CoopLookShare::kCharacterPart, CoopLookShare::kCharacterMark | 9u};
    CHECK(sent->parts == CoopLookShare::Parts{nine, {CoopLookShare::kCustomPart, 0x123456}});
    s.SetCustom(0x123456);
    CHECK_FALSE(s.TakeSend());                 // the same again is no change
    s.SetCustom(0x000001);
    sent = s.TakeSend();
    REQUIRE(sent);                             // a changed option sends the look again
    CHECK(sent->parts == CoopLookShare::Parts{nine, {CoopLookShare::kCustomPart, 0x000001}});
    s.SetCustom(0);
    sent = s.TakeSend();
    REQUIRE(sent);                             // none set: the part is left out
    CHECK(sent->parts == CoopLookShare::Parts{nine});

    CoopLookShare::Share r;
    r.EnableSlots(true);
    CHECK(r.CustomOf(2) == 0);
    // (0.11.9, one customizer: the options a 0.11.8 game sends are taken out and never kept, so no puppet is
    // dressed by the mod's routine with them; the character itself still shows)
    CHECK(r.OnEvent(Look(2, {{11, 0xFF0000}, nine, {CoopLookShare::kCustomPart, 0x654321}})));
    CHECK(r.CustomOf(2) == 0);
    CHECK(r.CharacterOf(2) == 9);
    CHECK(r.Of(2) == CoopLookShare::Parts{{11, 0xFF0000}});   // no painter sees either part
    CHECK_FALSE(r.OnEvent(Look(2, {{11, 0xFF0000}, nine, {CoopLookShare::kCustomPart, 0x654321}})));
    CHECK_FALSE(r.OnEvent(Look(2, {{11, 0xFF0000}, nine, {CoopLookShare::kCustomPart, 0x654320}})));   // (ignored)
    CHECK(r.CustomOf(2) == 0);
    CHECK(r.OnEvent(Look(2, {{11, 0xFF0000}, {CoopLookShare::kCustomPart, 0x654320}})));         // Link
    CHECK(r.CustomOf(2) == 0);
    CHECK(r.OnEvent(Look(2, {nine, {CoopLookShare::kCustomPart, 0x000002}})));
    r.OnEvent(Kind(EventKind::player_left, 2));
    CHECK(r.CustomOf(2) == 0);
}

TEST_CASE("character puppets: the mod's colour routine is read from the pack's manifest") {
    using CoopPuppet::PackCharacterColours;
    const auto c = PackCharacterColours(
        R"({"characters": {"mod": "x", "colours": {"offset": 48340992, "head": "fd7bbfa9e0030034aB2c40b96b1d0051"}}, "x": 1})");
    REQUIRE(c);
    CHECK(c->first == 48340992u);
    CHECK(c->second == std::array<std::uint8_t, 16>{0xfd, 0x7b, 0xbf, 0xa9, 0xe0, 0x03, 0x00, 0x34,
                                                    0xab, 0x2c, 0x40, 0xb9, 0x6b, 0x1d, 0x00, 0x51});
    CHECK_FALSE(PackCharacterColours(R"({"characters": {"mod": "x", "colours": null}})"));
    CHECK_FALSE(PackCharacterColours(R"({"characters": null})"));
    CHECK_FALSE(PackCharacterColours(R"({"colours": {"offset": 0, "head": "fd7bbfa9e0030034ab2c40b96b1d0051"}})"));
    CHECK_FALSE(PackCharacterColours(R"({"colours": {"offset": 5, "head": "fd7bbfa9e0030034ab2c40b96b1d00"}})"));
    CHECK_FALSE(PackCharacterColours(R"({"colours": {"offset": 5, "head": "zz7bbfa9e0030034ab2c40b96b1d0051"}})"));
    CHECK_FALSE(PackCharacterColours(R"({"colours": {"offset": 99999999999, "head": "fd7bbfa9e0030034ab2c40b96b1d0051"}})"));
    CHECK_FALSE(PackCharacterColours(R"({"colours": {"head": "fd7bbfa9e0030034ab2c40b96b1d0051"}, "offset": 5})"));
}

TEST_CASE("character puppets: the pack's are read from its manifest, and a player who picked one is shown as it") {
    using namespace CoopPuppet;
    CHECK(CharacterActor(true, 7) == "CoopPuppet_Adult_C007");
    CHECK(CharacterActor(false, 0) == "CoopPuppet_Child_C000");
    CHECK(CharacterActor(false, 113) == "CoopPuppet_Child_C113");
    CHECK(CharacterActor(true, 1000).empty());
    CHECK(IsCharacterActor("CoopPuppet_Adult_C007"));
    CHECK(IsCharacterActor("CoopPuppet_ChildQuiet_C113"));
    CHECK(IsCharacterActor("CoopPuppet_Adult_C007_Pvp"));
    CHECK_FALSE(IsCharacterActor("CoopPuppet_Adult_Cyan"));
    CHECK_FALSE(IsCharacterActor("CoopPuppet_Adult"));
    CHECK_FALSE(IsCharacterActor("CoopPuppet_Adult_C07"));
    CHECK_FALSE(IsCharacterActor("CoopPuppet_Adult_C0077"));
    CHECK_FALSE(IsCharacterActor("CoopTunic_Adult_C007"));
    // As build.py writes it: the mod's stored number 9 is file 5 (a gap: not 9 - 2), 4 is file 2.
    const std::string manifest =
        R"({"actors": [{"actor": "CoopPuppet_Child", "character": null}, {"actor": "CoopPuppet_Adult_Cyan", "tunic": "cyan"},)"
        R"( {"actor": "CoopPuppet_Adult_C005", "character": 5, "model": "Player_Adult_C005"},)"
        R"( {"actor": "CoopPuppet_Child_C005", "character": 5}, {"actor": "CoopPuppet_Adult_C002", "character": 2},)"
        R"( {"actor": "CoopPuppet_Adult_C005"}], "characters": {"mod": "OoT Character Select",)"
        R"( "numbers": {"2": 4, "5": 9}, "variants": {"2": ["Adult"], "5": ["Child", "Adult"]}}, "test_tunic": null})";
    const auto actors = PackCharacterActors(manifest);
    CHECK(actors == std::vector<std::string>{"CoopPuppet_Adult_C002", "CoopPuppet_Adult_C005", "CoopPuppet_Child_C005"});
    CHECK(PackCharacterActors(R"({"actors": [{"actor": "CoopPuppet_Adult_Cyan"}]})").empty());
    const auto files = PackCharacterNumbers(manifest);
    CHECK(files == std::map<std::uint16_t, std::uint16_t>{{4, 2}, {9, 5}});
    CHECK(PackCharacterNumbers(R"({"actors": [], "characters": null, "numbers": {"1": 3}})").empty());
    CHECK(PackCharacterNumbers(R"({"actors": []})").empty());
    CHECK(PackCharacterNumbers(R"({"characters": {"mod": "x", "numbers": {}}})").empty());

    PlannerSettings settings;
    settings.character_actors = actors;
    settings.character_files = files;
    Planner p(settings);
    CHECK(p.ActorFor(true, 0xFF3030u, 9) == "CoopPuppet_Adult_C005");    // the pack's file, before the colour
    CHECK(p.ActorFor(false, std::nullopt, 9) == "CoopPuppet_Child_C005");
    CHECK(p.ActorFor(true, 0xFF3030u, 4) == "CoopPuppet_Adult_C002");
    CHECK(p.ActorFor(false, 0xFF3030u, 4) == "CoopPuppet_Child_Red");    // no child model of it
    CHECK(p.ActorFor(true, 0xFF3030u, 0) == "CoopPuppet_Adult_Red");     // Link: the colour, as before
    CHECK(p.ActorFor(true, 0xFF3030u, 7) == "CoopPuppet_Adult_Red");     // 7 - 2 = 5 is no rule: 7 is not listed
    CHECK(p.ActorFor(true, 0xFF3030u, 12) == "CoopPuppet_Adult_Red");    // a number this viewer's mod has not
    CHECK(p.ActorFor(true, 0xFF3030u) == "CoopPuppet_Adult_Red");
    p.SetFailedTunics({"CoopPuppet_Adult_C005"});
    CHECK(p.ActorFor(true, 0xFF3030u, 9) == "CoopPuppet_Adult_Red");     // one the game refused: the colour
    PlannerSettings none;
    CHECK(Planner(none).ActorFor(true, 0xFF3030u, 9) == "CoopPuppet_Adult_Red");   // a pack without them
    PlannerSettings no_map;
    no_map.character_actors = actors;
    CHECK(Planner(no_map).ActorFor(true, 0xFF3030u, 9) == "CoopPuppet_Adult_Red");   // puppets but no map: none
}

TEST_CASE("look share (row 198): a puppet never wears the game's own coloured items while it has a slot") {
    // A player who leaves and joins again (the owner's log: P2's puppet made again wore 'Weapon_Shield_Hylian',
    // the game's own, and showed the owner's shield colour): from its first look on, a copy.
    CoopLookShare::Share s;
    s.EnableSlots(true);
    for (std::uint8_t p = 2; p <= 5; ++p) s.OnEvent(Look(p, {{4, 0x0000FF}}));
    for (std::uint8_t p = 2; p <= 5; ++p) CHECK(s.CopyOf(p));
    s.OnEvent(Kind(EventKind::player_left, 2));
    s.OnEvent(Look(2, {{4, 0x0000FF}}));
    CHECK(s.CopyOf(2));
    // More players than slots (kSlots): the fifth has none (the game's items; the module logs it).
    s.OnEvent(Look(6, {{4, 0x0000FF}}));
    CHECK_FALSE(s.CopyOf(6));
    // Slots off (no texture overrides in this Longshot: your own Link is not painted either): none.
    s.EnableSlots(false);
    CHECK_FALSE(s.CopyOf(3));
}

TEST_CASE("look share (0.11.9, one customizer): the mod's own options read and not 0 are held back at 0") {
    using V = std::array<std::optional<std::int32_t>, 6>;
    CHECK(CoopLookShare::OptionsToHold(V{}).empty());                                    // (no mod: none read)
    CHECK(CoopLookShare::OptionsToHold(V{0, 0, 0, 0, 0, 0}).empty());                    // (already the model's own)
    CHECK(CoopLookShare::OptionsToHold(V{3, 0, std::nullopt, 0, 7, -1}) == std::vector<std::size_t>{0, 4, 5});
}

TEST_CASE("look share (row 198, 0.11.9): a coloured copy the game did not make is told from one it made") {
    // The 0.11.9 pack sized the copies' AnimationParam too small: the game kept the puppet's own shield
    // ('Weapon_Shield_Hylian', the viewer's colours) though the equip of its copy returned 1.
    using CoopPuppet::CopyRefused;
    CHECK(CopyRefused("Weapon_Shield_Hylian_Look1", "Weapon_Shield_Hylian"));
    CHECK(CopyRefused("Weapon_Shield_Mirror_Look9", "Weapon_Shield_Mirror"));             // (the neutral copy too)
    CHECK(CopyRefused("Weapon_Sword_Kokiri_Blade3", "Weapon_Sword_Kokiri"));               // a wearer copy
    CHECK_FALSE(CopyRefused("Weapon_Shield_Hylian_Look1", "Weapon_Shield_Hylian_Look1"));  // made
    CHECK_FALSE(CopyRefused("Weapon_Shield_Hylian", "Weapon_Shield_Hylian"));              // no copy worn
    CHECK_FALSE(CopyRefused("Weapon_Shield_Hylian", "Weapon_Shield_Deku"));                // (not a copy: not ours)
    CHECK_FALSE(CopyRefused("Weapon_Shield_Hylian_Look1", ""));                            // name not read
}
