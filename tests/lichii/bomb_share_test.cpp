// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native puppets phase 2: the client's bomb hand-over rules (coop_bomb_share.h,
// docs/native-puppets.md "Lift a bomb, and stash it").
#include <doctest/doctest.h>

#include <limits>

#include "coop_bomb_share.h"

namespace proto = coop::protocol;
using namespace CoopBombShare;

namespace {
const proto::ObjectId kBomb{proto::ObjectKind::made, 1, 77};
}

TEST_CASE("bomb share: only Item_Bomb changes hands") {
    CHECK(IsBomb("Item_Bomb"));
    CHECK_FALSE(IsBomb("Item_Bombchu"));
    CHECK_FALSE(IsBomb("CoopShot_Bomb"));
    CHECK_FALSE(IsBomb(""));
}

TEST_CASE("bomb share: a lift asks once, two lifts at once ask once, an answer or the wait ends it") {
    Takes takes;
    const auto first = takes.Lifted(kBomb, 3, 1'000'000);
    REQUIRE(first);
    CHECK(*first == proto::BombTake{kBomb, 3});
    // The game reports the lift again next walk: no second ask.
    CHECK_FALSE(takes.Lifted(kBomb, 3, 1'033'000));
    CHECK(takes.Asking(kBomb));
    CHECK(takes.Expired(2'400'000).empty());
    // No answer for kAnswerWaitUs: dropped once.
    const auto gone = takes.Expired(1'000'000 + kAnswerWaitUs);
    REQUIRE(gone.size() == 1);
    CHECK(gone[0] == kBomb);
    CHECK_FALSE(takes.Asking(kBomb));
    CHECK(takes.Expired(9'000'000).empty());
    // A new lift may ask again; its answer ends it.
    REQUIRE(takes.Lifted(kBomb, 4, 5'000'000));
    CHECK(takes.Done(kBomb));
    CHECK_FALSE(takes.Done(kBomb));
    CHECK(takes.Size() == 0);
}

TEST_CASE("bomb share: our lift is a Lifted copy its owner does not hold") {
    CHECK(LiftedHere(true, false));
    CHECK_FALSE(LiftedHere(true, true));    // the owner lifted it: the director plays Lifted
    CHECK_FALSE(LiftedHere(false, false));  // Thrown
    CHECK_FALSE(LiftedHere(std::nullopt, false));
}

TEST_CASE("bomb share: row 308: the director yields a lying bomb copy to our lift until the take or a refusal") {
    // share on, Item_Bomb, owner does not hold it, not taking, not refused, not removed
    CHECK(YieldsToLift(true, "Item_Bomb", false, false, false, false));
    CHECK_FALSE(YieldsToLift(false, "Item_Bomb", false, false, false, false));  // switch off: as before
    CHECK_FALSE(YieldsToLift(true, "NormalArrow", false, false, false, false));
    CHECK_FALSE(YieldsToLift(true, "Item_Bomb", true, false, false, false));   // the owner holds it
    CHECK_FALSE(YieldsToLift(true, "Item_Bomb", false, true, false, false));   // taking: place is off already
    CHECK_FALSE(YieldsToLift(true, "Item_Bomb", false, false, true, false));   // refused: back to the owner's place
    CHECK_FALSE(YieldsToLift(true, "Item_Bomb", false, false, false, true));   // removed
}

TEST_CASE("bomb share: one fuse: the new owner goes off when the old owner's fuse would") {
    proto::ObjectState s;
    s.time.server_time_us = 10'000'000;
    s.fuse = 2.5f;
    const auto at = BlowAt(s);
    REQUIRE(at);
    CHECK(*at == 12'500'000);
    CHECK(FuseLeft(*at, 11'000'000) == doctest::Approx(1.5f));
    CHECK(FuseLeft(*at, 13'000'000) == 0.0f);
    // Unlit (no fuse) or nonsense: no moment.
    s.fuse.reset();
    CHECK_FALSE(BlowAt(s));
    s.fuse = -1.0f;
    CHECK_FALSE(BlowAt(s));
    s.fuse = 1e9f;
    CHECK_FALSE(BlowAt(s));
}

TEST_CASE("bomb share: the owner answers an ask unless its Link holds the bomb") {
    proto::HandoverAsk ask{kBomb, 2, 3};
    proto::ObjectState s;
    s.time.server_time_us = 7;
    s.fuse = 1.25f;
    CHECK_FALSE(Answer(ask, true, s));
    const auto out = Answer(ask, false, s);
    REQUIRE(out);
    CHECK(out->id == kBomb);
    CHECK(out->epoch == 2);
    REQUIRE(out->state.fuse);
    CHECK(*out->state.fuse == doctest::Approx(1.25f));
}

TEST_CASE("bomb share: stash count: a taken unlit bomb that goes as the pouch rises by one is picked up") {
    PouchWatch pouch;
    pouch.Read(5, 1'000'000);
    pouch.Read(std::nullopt, 1'100'000);  // not readable: kept
    CHECK_FALSE(pouch.RoseNear(1'200'000));
    pouch.Read(6, 2'000'000);  // +1: the stash
    CHECK(pouch.RoseNear(2'000'000));
    CHECK(pouch.RoseNear(2'000'000 + kStashWindowUs));
    CHECK_FALSE(pouch.RoseNear(2'000'000 + kStashWindowUs + 1));
    CHECK(GoneReason(true, false, pouch.RoseNear(2'100'000), proto::RemoveReason::exploded) ==
          proto::RemoveReason::picked_up);
    // A lit one, one we did not take, or no rise: as the sync judged it.
    CHECK(GoneReason(true, true, true, proto::RemoveReason::exploded) == proto::RemoveReason::exploded);
    CHECK(GoneReason(false, false, true, proto::RemoveReason::exploded) == proto::RemoveReason::exploded);
    CHECK(GoneReason(true, false, false, proto::RemoveReason::gone) == proto::RemoveReason::gone);
    // A jump of more than one (a chest, a shop, a load) is no stash.
    PouchWatch jump;
    jump.Read(5, 1'000'000);
    jump.Read(10, 2'000'000);
    CHECK_FALSE(jump.RoseNear(2'000'000));
    jump.Reset();
    jump.Read(10, 3'000'000);
    CHECK_FALSE(jump.RoseNear(3'000'000));
}

TEST_CASE("bomb share: our own bombs are known to the objects sync while ours") {
    OwnBombs own;
    CHECK_FALSE(own.Has(kBomb));
    own.Note(kBomb, true);
    CHECK(own.Has(kBomb));
    own.Note(kBomb, false);
    CHECK_FALSE(own.Has(kBomb));
    own.Note(kBomb, true);
    own.Clear();
    CHECK_FALSE(own.Has(kBomb));
}

TEST_CASE("bomb share: BOMB_SHARE goes out at once and then every few seconds") {
    Announce a;
    CHECK(a.Due(1'000'000));
    CHECK_FALSE(a.Due(1'000'000 + kAnnounceEveryUs - 1));
    CHECK(a.Due(1'000'000 + kAnnounceEveryUs));
    a.Reset();
    CHECK(a.Due(1'000'001 + kAnnounceEveryUs));
}

TEST_CASE("bomb share: a take needs our own Link's place, and the copy beside it") {
    CHECK_FALSE(CoopBombShare::NearOurLink(std::nullopt, proto::Vec3{0, 1, 0}));
    CHECK(CoopBombShare::NearOurLink(proto::Vec3{0, 0, 0}, proto::Vec3{0.3f, 1.3f, 0}));
    CHECK_FALSE(CoopBombShare::NearOurLink(proto::Vec3{0, 0, 0}, proto::Vec3{4, 1, 0}));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(CoopBombShare::NearOurLink(proto::Vec3{0, 0, 0}, proto::Vec3{nan, 0, 0}));
}
