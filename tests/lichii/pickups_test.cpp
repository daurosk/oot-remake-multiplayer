// Tests for the one-time pickups the object walk notes (lichii/hook/coop_pickups.cpp).
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "coop_pickups.h"

TEST_CASE("pickups: the one-time pickups by name") {
    for (const char* name : {"Enemy_Skulltula_Gold", "Obj_Token_Gold", "Obj_Heart_Container",
                             "Obj_Heart_Piece", "Obj_Heart_Piece_Digging_Hole",
                             "Obj_Heart_Piece_MiniGame", "Obj_GerudoValley_Heart_Piece",
                             "Obj_RupeeToken_Silver"}) {
        CAPTURE(name);
        CHECK(CoopPickups::IsPickup(name));
    }
    for (const char* name : {"Enemy_Skulltula", "Obj_Heart", "Obj_Heart_InWater", "Obj_Rupee_Green",
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