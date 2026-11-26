// lichii/hook/coop_char_slots.h: a Character Select pick whose model is not installed is never kept.
#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "../../lichii/hook/coop_char_slots.h"

using CoopCharSlots::Installed;
using CoopCharSlots::Others;
using CoopCharSlots::Own;

namespace {
// Stored number -> file, as the pack maps installed models only (C005 and C010 here).
const std::map<std::uint16_t, std::uint16_t> kFiles{{12, 5}, {30, 10}};
const std::map<std::uint16_t, std::uint16_t> kNone{};
}  // namespace

TEST_CASE("char slots: Link and installed characters are kept") {
    CHECK(Installed(0, kNone));
    CHECK(Installed(-1, kNone));
    CHECK(Installed(12, kFiles));
    CHECK_FALSE(Installed(13, kFiles));
    CHECK_FALSE(Installed(12, kNone));
    CHECK_FALSE(Installed(0x10000 + 12, kFiles));
}

TEST_CASE("char slots: own pick, no mod (no flag): nothing done") {
    Own own;
    const auto s = own.Check(std::nullopt, kFiles);
    CHECK_FALSE(s.set);
    CHECK_FALSE(s.say);
}

TEST_CASE("char slots: own dead pick goes back to the last installed one at once, said once") {
    Own own;
    CHECK_FALSE(own.Check(12, kFiles).set);
    auto s = own.Check(40, kFiles);
    REQUIRE(s.set);
    CHECK(*s.set == 12);
    CHECK(s.say);
    s = own.Check(40, kFiles);   // (the page stores it again before it reads ours)
    REQUIRE(s.set);
    CHECK(*s.set == 12);
    CHECK_FALSE(s.say);
    CHECK_FALSE(own.Check(12, kFiles).set);
    CHECK(own.Check(40, kFiles).say);   // picked again later: said again
}

TEST_CASE("char slots: without the NPC download every character goes back to Link (also a save stored with one)") {
    Own own;
    const auto s = own.Check(30, kNone);   // the first read after a save load
    REQUIRE(s.set);
    CHECK(*s.set == 0);
    CHECK(s.say);
    CHECK_FALSE(own.Check(0, kNone).set);
}

TEST_CASE("char slots: another player's missing pick is said once per number, again only for another one") {
    Others others;
    CHECK_FALSE(others.Tell(2, 0, kNone));
    CHECK(others.Tell(2, 30, kNone));
    CHECK_FALSE(others.Tell(2, 30, kNone));   // every frame after: nothing
    CHECK(others.Tell(3, 30, kNone));         // another player
    CHECK(others.Tell(2, 31, kNone));         // they picked another missing one
    CHECK_FALSE(others.Tell(2, 12, kFiles));  // installed here
    CHECK(others.Tell(2, 31, kNone));
    CHECK_FALSE(others.Tell(17, 31, kNone));  // (no such number)
    CHECK(CoopCharSlots::MissingLine(false, "Saria").rfind("Saria plays", 0) == 0);
    CHECK(CoopCharSlots::MissingLine(true).find("press N") != std::string::npos);
}

// Rows 239/240/241: a saved game (tools/autotest/save_flags.py's layout) with an Int `hash` = `value` in type 2, or none.
namespace {
std::vector<std::uint8_t> FakeSave(std::optional<std::pair<std::uint32_t, std::int32_t>> int_value) {
    std::vector<std::uint8_t> d(0x20, 0);
    const auto put = [&](std::size_t at, std::uint32_t v) {
        if (d.size() < at + 4) d.resize(at + 4, 0);
        for (int i = 0; i < 4; ++i) d[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    put(0, 0x01020304);
    std::size_t at = 0x20;
    const auto rec = [&](std::uint32_t a, std::uint32_t b) { put(at, a), put(at + 4, b), at += 8; };
    for (std::uint32_t type = 0; type < 0x20; ++type) {
        rec(0, type);
        if (type == 0) rec(0x11111111, 1);   // a Bool with the same word layout: never taken for the Int
        if (type == 2) {
            rec(0x22222222, 7);
            if (int_value) rec(int_value->first, static_cast<std::uint32_t>(int_value->second));
        }
    }
    rec(0, 0x20);
    rec(0x12345678, 0);   // the end record's pair
    put(8, static_cast<std::uint32_t>(at));   // the pool starts right after the table
    return d;
}
constexpr std::uint32_t kPick = 0xd6b580ea;   // flag_hash("CharacterMenu_SelectedCharacter")
std::int32_t IntIn(const std::vector<std::uint8_t>& d, std::uint32_t hash) {
    const auto at = CoopCharSlots::SaveIntAt(d.data(), d.size(), hash);
    REQUIRE(at);
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = v << 8 | d[*at + static_cast<std::size_t>(i)];
    return static_cast<std::int32_t>(v);
}
}  // namespace

TEST_CASE("char slots: a saved pick with no model here becomes Link in the save file (rows 239/240/241)") {
    auto d = FakeSave(std::make_pair(kPick, 93));
    const auto before = d;
    const auto fix = CoopCharSlots::FixSavePick(d.data(), d.size(), kPick, kFiles);
    CHECK(fix.changed);
    REQUIRE(fix.was);
    CHECK(*fix.was == 93);
    CHECK(IntIn(d, kPick) == 0);
    CHECK(IntIn(d, 0x22222222) == 7);   // nothing else moved or changed
    CHECK(d.size() == before.size());
    std::size_t differ = 0;
    for (std::size_t i = 0; i < d.size(); ++i) differ += d[i] != before[i];
    CHECK(differ == 1);   // 93 = 0x5d: one byte
    // Again: nothing more to do.
    CHECK_FALSE(CoopCharSlots::FixSavePick(d.data(), d.size(), kPick, kFiles).changed);
}

TEST_CASE("char slots: an installed pick, Link, or a save without the mod's value stays as it is") {
    auto kept = FakeSave(std::make_pair(kPick, 12));
    CHECK_FALSE(CoopCharSlots::FixSavePick(kept.data(), kept.size(), kPick, kFiles).changed);
    CHECK(IntIn(kept, kPick) == 12);
    auto link = FakeSave(std::make_pair(kPick, 0));
    CHECK_FALSE(CoopCharSlots::FixSavePick(link.data(), link.size(), kPick, kNone).changed);
    auto none = FakeSave(std::nullopt);
    const auto fix = CoopCharSlots::FixSavePick(none.data(), none.size(), kPick, kNone);
    CHECK_FALSE(fix.changed);
    CHECK_FALSE(fix.was);
}

TEST_CASE("char slots: not a save, or a table out of order: never written") {
    auto d = FakeSave(std::make_pair(kPick, 93));
    auto bad_magic = d;
    bad_magic[0] ^= 0xff;
    CHECK_FALSE(CoopCharSlots::FixSavePick(bad_magic.data(), bad_magic.size(), kPick, kNone).changed);
    CHECK(bad_magic[0] == static_cast<std::uint8_t>(d[0] ^ 0xff));
    auto short_file = d;
    short_file.resize(0x10);
    CHECK_FALSE(CoopCharSlots::SaveIntAt(short_file.data(), short_file.size(), kPick));
    auto out_of_order = d;
    out_of_order[0x20 + 4] = 1;   // the first type opener says type 1
    CHECK_FALSE(CoopCharSlots::SaveIntAt(out_of_order.data(), out_of_order.size(), kPick));
    auto pool_past_end = d;
    pool_past_end[8] = 0xff, pool_past_end[9] = 0xff;
    CHECK_FALSE(CoopCharSlots::SaveIntAt(pool_past_end.data(), pool_past_end.size(), kPick));
    CHECK_FALSE(CoopCharSlots::SaveIntAt(nullptr, 0, kPick));
}

TEST_CASE("char slots: an NPC model file is told from the UI part's own files (no pack: UI-only means no NPC)") {
    CHECK(CoopCharSlots::IsNpcModelName("Player_Child_C091.Player_Child.bfres.zs"));
    CHECK(CoopCharSlots::IsNpcModelName("Player_Adult_C000.Player_Adult.bfres.zs"));
    CHECK_FALSE(CoopCharSlots::IsNpcModelName("Player_Child_Cba_Animation.Skeleton.bfres.zs"));
    CHECK_FALSE(CoopCharSlots::IsNpcModelName("Player_Child.Player_Child.bfres.zs"));
    CHECK_FALSE(CoopCharSlots::IsNpcModelName("Player_Child_C"));
    CHECK_FALSE(CoopCharSlots::IsNpcModelName("Player_Dark_Animation.Skeleton.bfres.zs"));
}

TEST_CASE("char slots: every load kind starts from a checked pick (the guard runs every frame, title included)") {
    // The same Own across the title, a load and play: a missing pick set on the title (Character Select's page, a
    // host's save, a Game Over or System Load back to the title) is put back on the first frame it is read, before any
    // Continue; the mod re-storing it at a load's start is put back again on that frame.
    Own own;
    CHECK_FALSE(own.Check(0, kFiles).set);           // title, Link
    const auto title = own.Check(93, kFiles);        // a pick made on the title with no model here
    REQUIRE(title.set);
    CHECK(*title.set == 0);
    CHECK(title.say);
    CHECK_FALSE(own.Check(0, kFiles).set);           // put back: the load starts from Link
    const auto again = own.Check(93, kFiles);        // the mod re-stores it at the load's start
    REQUIRE(again.set);
    CHECK(*again.set == 0);
    CHECK_FALSE(own.Check(12, kFiles).set);          // an installed pick is kept (Try Again, System Load, a join)
    const auto later = own.Check(93, kFiles);        // a missing one after it: back to the last installed pick
    REQUIRE(later.set);
    CHECK(*later.set == 12);
    // No pack and no NPC model in the mod (the UI-only part): every NPC pick goes back to Link.
    Own ui_only;
    const auto ui = ui_only.Check(93, kNone);
    REQUIRE(ui.set);
    CHECK(*ui.set == 0);
}
