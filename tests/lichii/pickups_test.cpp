// Tests for the one-time pickups the object walk notes (lichii/hook/coop_pickups.cpp).
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "coop_pickups.h"
#include "../test_temp.h"

namespace {

void WriteFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string Utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

} // namespace

TEST_CASE("pickups: FNV-1a 64 as the kit's start writes it") {
    CHECK(CoopPickups::Fnv64Hex("") == "cbf29ce484222325");
    CHECK(CoopPickups::Fnv64Hex("a") == "af63dc4c8601ec8c");
}

TEST_CASE("pickups: the one-time list of the kit's start (row 219)") {
    const auto list = CoopPickups::ParseOneTime(
        "coop-one-time-keys 1\r\ngame 0123456789abcdef\r\nserved OoT Randomizer x/romfs/GameData/GameDataList.byml.zs "
        "00000000000000aa\r\npickup 0000000000000010\r\npickup 16\r\nholder 0000000000000013\r\n");
    REQUIRE(list);
    CHECK(list->pickups == std::vector<std::uint64_t>{0x10, 0x16});
    CHECK(list->holders == std::vector<std::uint64_t>{0x13});
    CHECK(list->served == "OoT Randomizer x/romfs/GameData/GameDataList.byml.zs");  // (a path with spaces)
    CHECK(list->served_fnv == "00000000000000aa");
    const auto own = CoopPickups::ParseOneTime("coop-one-time-keys 1\ngame 1\nserved - -\npickup 10\n");
    REQUIRE(own);
    CHECK(own->served.empty());
    std::string why;
    for (const char* bad : {"", "coop-one-time-keys 2\nserved - -\n", "coop-one-time-keys 1\npickup 10\n",
                            "coop-one-time-keys 1\nserved - -\npickup zz\n", "coop-one-time-keys 1\nserved - -\npickup 0\n",
                            "coop-one-time-keys 1\nserved - -\nkey 10\n", "coop-one-time-keys 1\nserved x 12\n",
                            "coop-one-time-keys 1\nserved - -\nholder 11112222333344445\n"}) {
        CAPTURE(bad);
        why.clear();
        CHECK_FALSE(CoopPickups::ParseOneTime(bad, &why));
        CHECK_FALSE(why.empty());
    }
}

TEST_CASE("pickups: a one-time list's holders only while its served list is the same (row 219)") {
    const auto dir = ModTestDir("coop_one_time_test");
    std::filesystem::remove_all(dir);
    const std::string served = "seed mod/romfs/GameData/GameDataList.Product.110.byml.zs";
    WriteFile(dir / served, "the seed's list");
    const std::string head = "coop-one-time-keys 1\ngame 1\nserved " + served + " ";
    WriteFile(dir / CoopPickups::kOneTimeFile,
              head + CoopPickups::Fnv64Hex("the seed's list") + "\npickup 10\nholder 13\n");
    std::string note;
    auto list = CoopPickups::LoadOneTime(Utf8(dir / CoopPickups::kOneTimeFile), note);
    REQUIRE(list);
    CHECK(note.empty());
    CHECK(list->holders == std::vector<std::uint64_t>{0x13});
    // A seed built or removed since the kit's start: its pots may reset again, so they are not shared.
    WriteFile(dir / served, "another seed's list");
    list = CoopPickups::LoadOneTime(Utf8(dir / CoopPickups::kOneTimeFile), note);
    REQUIRE(list);
    CHECK(list->pickups == std::vector<std::uint64_t>{0x10});
    CHECK(list->holders.empty());
    CHECK(note.find("1 one-time holders left out") != std::string::npos);
    std::filesystem::remove_all(dir);
    CHECK_FALSE(CoopPickups::LoadOneTime(Utf8(dir / CoopPickups::kOneTimeFile), note));
    CHECK(note.find("no ") == 0);
}

TEST_CASE("pickups: the listed placements, holders apart, until a reset") {
    CoopPickups::OneTime list;
    list.pickups = {0x10};
    list.holders = {0x13};
    CoopPickups::SetOneTime(list);
    CHECK(CoopPickups::IsOneTime(0x10));
    CHECK(CoopPickups::IsOneTime(0x13));
    CHECK_FALSE(CoopPickups::IsOneTimeHolder(0x10));
    CHECK(CoopPickups::IsOneTimeHolder(0x13));
    CHECK_FALSE(CoopPickups::IsOneTime(0x11));
    CoopPickups::Reset();
    CHECK_FALSE(CoopPickups::IsOneTime(0x10));
    CHECK_FALSE(CoopPickups::IsOneTimeHolder(0x13));
}

TEST_CASE("pickups: the one-time pickups by name") {
    for (const char* name : {"Enemy_Skulltula_Gold", "Obj_Token_Gold", "Obj_Heart_Container",
                             "Obj_Heart_Piece", "Obj_Heart_Piece_Digging_Hole",
                             "Obj_Heart_Piece_MiniGame", "Obj_GerudoValley_Heart_Piece",
                             "Obj_RupeeToken_Silver", "Obj_Rupee_Green", "Obj_Rupee_Red",
                             "Obj_Heart", "Obj_Heart_InWater"}) {
        CAPTURE(name);
        CHECK(CoopPickups::IsPickup(name));
    }
    for (const char* name : {"Enemy_Skulltula", "Obj_Heart_Shootable", "Obj_MiniGame_SmallKey",
                             "Obj_Dungeon_SmallKey", "Obj_Dungeon_SmallKey_GerudoFortress_01",
                             "Obj_OrnamentalRupee_Green", "Obj_silverRupeeCounter",
                             "Pouch_Heart_Piece", "HeartPieceAnchor", "Obj_Pot_A_01", "Item_Token_Gold"}) {
        CAPTURE(name);
        CHECK_FALSE(CoopPickups::IsPickup(name));
    }
}

TEST_CASE("pickups: each placed pickup is noted once, in order") {
    CoopPickups::Reset();
    CoopPickups::Note("Enemy_Skulltula_Gold", 0x11);
    CoopPickups::Note("Obj_Pot_A_01", 0x22);           // not a pickup
    CoopPickups::Note("Obj_Heart_Container", 0);       // made by the game: no placement
    CoopPickups::Note("Obj_Heart_Container", ~std::uint64_t{0});
    CoopPickups::Note("Obj_Heart_Container", 0x33);
    CoopPickups::Note("Enemy_Skulltula_Gold", 0x11);   // seen again
    CHECK(CoopPickups::Since(0) == std::vector<std::uint64_t>{0x11, 0x33});
    CHECK(CoopPickups::Since(1) == std::vector<std::uint64_t>{0x33});
    CHECK(CoopPickups::Since(2).empty());
    CHECK(CoopPickups::Since(9).empty());
    CoopPickups::Note("Obj_Token_Gold", 0x44);
    CHECK(CoopPickups::Since(2) == std::vector<std::uint64_t>{0x44});
    CoopPickups::Reset();
    CHECK(CoopPickups::Since(0).empty());
}