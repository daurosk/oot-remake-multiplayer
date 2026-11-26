// Each other player's delay (coop_show_delay.h, report 37) reaches the enemy
// copies it owns and the holds that wait for its answer to our hit.

#include <doctest/doctest.h>

#include "coop_objects_sync.h"
#include "coop_show_delay.h"

using CoopObjects::Sync;

TEST_CASE("show delay (hook): a player's delay is the floor until set, and reaches its copies and holds") {
    CHECK(CoopShowDelay::Us(0) == CoopShowDelay::kFloorUs);
    CHECK(CoopShowDelay::Us(200) == CoopShowDelay::kFloorUs);  // no such player
    CoopShowDelay::Set(5, 40'000);                              // under the floor: the floor
    CHECK(CoopShowDelay::Us(5) == CoopShowDelay::kFloorUs);
    CHECK(Sync::BehindUs(5) == Sync::kBehindUs);
    CHECK(Sync::AnswerHoldUs(Sync::kReactHoldUs, 5) == Sync::kReactHoldUs);  // a LAN: as before

    CoopShowDelay::Set(5, 300'000);
    CHECK(CoopShowDelay::Us(5) == 300'000);
    CHECK(Sync::BehindUs(5) == 300'000);
    // 200 ms later each way than on a LAN.
    CHECK(Sync::AnswerHoldUs(Sync::kReactHoldUs, 5) == Sync::kReactHoldUs + 400'000);
    CHECK(Sync::AnswerHoldUs(Sync::kHitHoldUs, 5) == Sync::kHitHoldUs + 400'000);
    CoopShowDelay::Set(5, 0);
    CHECK(Sync::BehindUs(5) == Sync::kBehindUs);
}
