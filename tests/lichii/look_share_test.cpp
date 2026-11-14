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

TEST_CASE("look share: a player who colours gear gets the lowest free slot, kept until they leave") {
    CoopLookShare::Share s;
    s.OnEvent(Look(2, {{3, 0xFF0000}}));
    CHECK_FALSE(s.SlotOf(2));                 // slots off (no painter): none
    s.EnableSlots(true);
    CHECK(s.SlotOf(2) == 1u);                 // given once the painter is on
    s.OnEvent(Look(5, {{0, 0x3070FF}}));      // the tunic alone is no gear: no slot
    CHECK_FALSE(s.SlotOf(5));
    s.OnEvent(Look(5, {{0, 0x3070FF}, {11, 0x202020}}));
    CHECK(s.SlotOf(5) == 2u);
    s.OnEvent(Look(2, {{3, 0x00FF00}}));      // a new colour keeps the slot
    CHECK(s.SlotOf(2) == 1u);
    s.OnEvent(Kind(EventKind::player_left, 2));
    CHECK_FALSE(s.SlotOf(2));
    s.OnEvent(Look(7, {{4, 1}}));
    CHECK(s.SlotOf(7) == 1u);                 // the freed slot again
    s.OnEvent(Look(7, {}));                   // back to the game's own: the slot is freed
    CHECK_FALSE(s.SlotOf(7));
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

TEST_CASE("look share: a puppet wears a copy only once it is painted, then the slot's other copy for a new look") {
    using Job = CoopLookShare::Share::Job;
    constexpr std::uint64_t later = CoopLookShare::kCopyDwellMs;
    CoopLookShare::Share s;
    s.EnableSlots(true);
    s.OnEvent(Look(3, {{11, 0xFF0000}}));
    s.OnEvent(Look(4, {{4, 0x00FF00}}));
    CHECK_FALSE(s.CopyOf(3));                   // nothing painted yet: the game's items
    auto jobs = s.Jobs(0);
    REQUIRE(jobs.size() == 2);
    CHECK(jobs[0] == Job{1, 1, 3, {{11, 0xFF0000}}});   // slot 1: copies 1 and 2
    CHECK(jobs[1] == Job{2, 3, 4, {{4, 0x00FF00}}});    // slot 2: copies 3 and 4
    s.Done(jobs[0], 1000);
    CHECK(s.CopyOf(3) == 1u);
    CHECK_FALSE(s.CopyOf(4));
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
    s.OnEvent(Look(3, {}));
    CHECK_FALSE(s.CopyOf(3));
    s.OnEvent(Look(3, {{11, 0x101010}}));
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
    CHECK_FALSE(s.CopyOf(5));
    CHECK_FALSE(s.CopyOf(7));
    const auto next = s.Jobs(1000 + 5 * later);
    REQUIRE(next.size() == 2);
    CHECK(next[1].player == 5);
    CHECK(next[1].copy != late.copy);
}

TEST_CASE("look items: the pack's item copies are read from its manifest") {
    const auto items = CoopPuppet::PackLookItems(
        R"({"look_files": {}, "look_items": {"slots": 4, "copies": 8, "actors": ["Weapon_Shield_Hylian", "Armor_Adult_Zora"], "files": ["Pack/Actor/X.pack.zs"]}, "x": 1})");
    CHECK(items.copies == 8);
    CHECK(items.actors == std::vector<std::string>{"Weapon_Shield_Hylian", "Armor_Adult_Zora"});
    CHECK(CoopPuppet::PackLookItems(R"({"look_items": null})").copies == 0);
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
