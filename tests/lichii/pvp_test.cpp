// Tests for player against player (lichii/hook/coop_pvp.cpp).
#include <doctest/doctest.h>

#include <cmath>
#include <ostream>
#include <string>
#include <vector>

#include "coop_pvp.h"

namespace proto = coop::protocol;
using CoopPvp::Command;
using CoopPvp::Target;
using CoopPvp::Vec3;

namespace {

constexpr std::uint64_t kS = 1'000'000;
const float kNaN = std::nanf("");

proto::GrassCut Swing(const Vec3& blade, std::string scene = "spot00") {
    proto::GrassCut c;
    c.scene = std::move(scene);
    c.position = blade;
    c.radius = 1.0f;
    c.sweep = {blade, blade, blade};
    return c;
}

Target Other(std::uint8_t number, const Vec3& at, std::string scene = "spot00", std::uint8_t status = 0) {
    return Target{number, std::move(scene), at, status};
}

CoopPvp::Local Me(std::int32_t life = 12) {
    CoopPvp::Local l;
    l.self = 2;
    l.scene = "spot00";
    l.life = life;
    return l;
}

CoopPvp::Hitter Them(std::string scene = "spot00", std::uint8_t status = 0) {
    return CoopPvp::Hitter{std::move(scene), status, true};
}

proto::PvpHit HitOn(std::uint8_t victim, std::int32_t amount = 2, std::uint8_t from = 1) {
    proto::PvpHit h;
    h.player_number = from;
    h.victim = victim;
    h.cause = static_cast<std::uint8_t>(proto::DamageCause::sword);
    h.amount = amount;
    return h;
}

} // namespace

TEST_CASE("pvp: /pvp shows, /pvp on and off ask, other text is chat") {
    CHECK(CoopPvp::Parse("/pvp")->kind == Command::Kind::show);
    CHECK(CoopPvp::Parse("  /PvP  ")->kind == Command::Kind::show);
    CHECK(CoopPvp::Parse("/pvp on")->kind == Command::Kind::on);
    CHECK(CoopPvp::Parse("/pvp YES")->kind == Command::Kind::on);
    CHECK(CoopPvp::Parse("/pvp 0")->kind == Command::Kind::off);
    CHECK(CoopPvp::Parse("/pvp off")->kind == Command::Kind::off);
    CHECK(CoopPvp::Parse("/pvp maybe")->kind == Command::Kind::bad);
    CHECK_FALSE(CoopPvp::Parse("/pvpx"));
    CHECK_FALSE(CoopPvp::Parse("pvp on"));
    CHECK_FALSE(CoopPvp::Parse("hello"));
}

TEST_CASE("pvp: a sword swing hits the other Link it reaches, once per swing") {
    CoopPvp::Attacker a;
    const Vec3 self{0, 0, 0};
    const std::vector<Target> targets{Other(2, {1.0f, 0, 0}), Other(3, {6.0f, 0, 0})};
    auto hits = a.Swing(Swing({0.8f, 1.0f, 0.2f}), self, "spot00", 1, targets, 10 * kS);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].victim == 2);
    CHECK(hits[0].cause == static_cast<std::uint8_t>(proto::DamageCause::sword));
    CHECK(hits[0].amount == 2);
    CHECK(hits[0].from == self);
    // The same swing read again (three slots a swing): no second hit.
    CHECK(a.Swing(Swing({0.8f, 1.0f, 0.2f}), self, "spot00", 1, targets, 10 * kS + 100'000).empty());
    // The next swing does.
    CHECK(a.Swing(Swing({0.8f, 1.0f, 0.2f}), self, "spot00", 1, targets, 10 * kS + 500'000).size() == 1);
}

TEST_CASE("pvp: what a swing never hits") {
    CoopPvp::Attacker a;
    const Vec3 self{0, 0, 0};
    const Vec3 blade{0.8f, 1.0f, 0.0f};
    // Not a sword (a roll or a bomb cuts grass with no blade points).
    proto::GrassCut roll = Swing(blade);
    roll.sweep = {Vec3{kNaN, kNaN, kNaN}, Vec3{kNaN, kNaN, kNaN}, Vec3{kNaN, kNaN, kNaN}};
    CHECK(a.Swing(roll, self, "spot00", 1, std::vector<Target>{Other(2, {1, 0, 0})}, kS).empty());
    // Ourselves, another scene, a player in a cutscene, menu, loading or dead.
    CHECK(a.Swing(Swing(blade), self, "spot00", 2, std::vector<Target>{Other(2, {1, 0, 0})}, kS).empty());
    CHECK(a.Swing(Swing(blade), self, "spot00", 1, std::vector<Target>{Other(2, {1, 0, 0}, "spot01")}, kS).empty());
    for (std::uint8_t s : {proto::status::in_cutscene, proto::status::in_menu, proto::status::loading,
                           proto::status::paused, proto::status::dead}) {
        CHECK(a.Swing(Swing(blade), self, "spot00", 1, std::vector<Target>{Other(2, {1, 0, 0}, "spot00", s)}, kS).empty());
    }
    // Above the body, or too far to the side.
    CHECK(a.Swing(Swing({1.0f, 3.0f, 0}), self, "spot00", 1, std::vector<Target>{Other(2, {1, 0, 0})}, kS).empty());
    CHECK(a.Swing(Swing({3.0f, 1.0f, 0}), self, "spot00", 1, std::vector<Target>{Other(2, {1, 0, 0})}, kS).empty());
    // A blade far from our own Link is not our swing.
    CHECK(a.Swing(Swing({9.0f, 1.0f, 0}), self, "spot00", 1, std::vector<Target>{Other(2, {9, 0, 0})}, kS).empty());
    // A cut from another scene's queue.
    CHECK(a.Swing(Swing(blade, "spot01"), self, "spot00", 1, std::vector<Target>{Other(2, {1, 0, 0})}, kS).empty());
}

TEST_CASE("pvp: shots hit within their reach, and a nut never hurts") {
    CoopPvp::Attacker a;
    const Vec3 self{0, 0, 0};
    const std::vector<Target> targets{Other(2, {10, 0, 0}), Other(3, {12.5f, 0, 0})};
    auto bomb = a.Shot(proto::DamageCause::bomb, {10.5f, 0.5f, 0}, self, "spot00", 1, targets, kS);
    REQUIRE(bomb.size() == 2);  // 0.5 m and 2 m from the blast
    CHECK(bomb[0].amount == 8);
    auto arrow = a.Shot(proto::DamageCause::arrow, {12.5f, 1.2f, 0.5f}, self, "spot00", 1, targets, 3 * kS);
    REQUIRE(arrow.size() == 1);
    CHECK(arrow[0].victim == 3);
    CHECK(arrow[0].amount == 4);
    CHECK(a.Shot(proto::DamageCause::nut, {10, 1, 0}, self, "spot00", 1, targets, 5 * kS).empty());
    CHECK(a.Shot(proto::DamageCause::seed, {11.5f, 1, 0}, self, "spot00", 1, targets, 5 * kS).empty());
    // issue 133: a lifeless copy's kill report is never a PvP hit (no amount, no reach)
    CHECK(CoopPvp::AmountOf(proto::DamageCause::killed) == 0);
    CHECK(CoopPvp::AmountOf(proto::DamageCause::reflected) == 0);  // row 177: a notice, never a PvP hit
    CHECK(CoopPvp::ReachOf(proto::DamageCause::killed) == 0.0f);
    CHECK(a.Shot(proto::DamageCause::killed, {10, 1, 0}, self, "spot00", 1, targets, 7 * kS).empty());
}

TEST_CASE("pvp: the victim's game takes the life only when it may") {
    CoopPvp::Victim v;
    // PvP off here, a hit on someone else, or our own: no answer at all.
    CHECK_FALSE(v.Hit(HitOn(2), false, Me(), Them(), kS));
    CHECK_FALSE(v.Hit(HitOn(3), true, Me(), Them(), kS));
    CHECK_FALSE(v.Hit(HitOn(2, 2, 2), true, Me(), Them(), kS));

    auto d = v.Hit(HitOn(2, 2), true, Me(12), Them(), kS);
    REQUIRE(d);
    CHECK(d->take == 2);
    CHECK(d->hurt.attacker == 1);
    CHECK(d->hurt.amount == 2);
    CHECK(d->hurt.life == 10);
    CHECK(d->hurt.flags == 0);
    // Link blinks after a hit: the next one within kMercyUs is kept off.
    d = v.Hit(HitOn(2, 2), true, Me(10), Them(), kS + 300'000);
    REQUIRE(d);
    CHECK(d->take == 0);
    CHECK(d->hurt.flags == proto::pvp_hurt::guarded);
    // After it, a hit that takes the last life defeats.
    d = v.Hit(HitOn(2, 8), true, Me(3), Them(), 3 * kS);
    REQUIRE(d);
    CHECK(d->take == 3);
    CHECK(d->hurt.life == 0);
    CHECK(d->hurt.flags == proto::pvp_hurt::died);
}

TEST_CASE("pvp: no hurt in a cutscene, menu or load, nor from an attacker in one") {
    auto guarded = [](CoopPvp::Local me, CoopPvp::Hitter them) {
        CoopPvp::Victim v;
        const auto d = v.Hit(HitOn(2), true, me, them, kS);
        return d && d->take == 0 && d->hurt.flags == proto::pvp_hurt::guarded;
    };
    CoopPvp::Local me = Me();
    me.event = true;
    CHECK(guarded(me, Them()));
    for (std::uint8_t s : {proto::status::in_cutscene, proto::status::in_menu, proto::status::loading,
                           proto::status::paused, proto::status::dead}) {
        me = Me();
        me.status = s;
        CHECK(guarded(me, Them()));
        CHECK(guarded(Me(), Them("spot00", s)));
    }
    me = Me();
    me.life.reset();  // our life is not read
    CHECK(guarded(me, Them()));
    CHECK(guarded(Me(0), Them()));          // already down
    CHECK(guarded(Me(), Them("spot01")));   // the attacker is elsewhere now
    CHECK(guarded(Me(), CoopPvp::Hitter{}));  // not in the room
}

TEST_CASE("pvp: chat lines") {
    CHECK(CoopPvp::Hearts(1) == "1/4 heart");
    CHECK(CoopPvp::Hearts(2) == "1/2 heart");
    CHECK(CoopPvp::Hearts(4) == "1 heart");
    CHECK(CoopPvp::Hearts(6) == "1 1/2 hearts");
    CHECK(CoopPvp::Hearts(8) == "2 hearts");
    CHECK(CoopPvp::ModeLine({true, 0}, 2, "") == "PvP is on: players can hurt each other");
    CHECK(CoopPvp::ModeLine({false, 1}, 2, "Saria") == "PvP is off (Saria changed it)");
    CHECK(CoopPvp::ModeLine({true, 2}, 2, "") == "PvP is on: players can hurt each other (you changed it)");
    CHECK(CoopPvp::HurtLine(proto::PvpHurt{2, 1, 2, 10, 0}, 1, "Mido", "Saria") == "you hit Mido (-1/2 heart)");
    CHECK(CoopPvp::HurtLine(proto::PvpHurt{2, 1, 2, 10, 0}, 2, "Mido", "Saria") == "Saria hit you (-1/2 heart)");
    CHECK(CoopPvp::HurtLine(proto::PvpHurt{2, 1, 4, 0, proto::pvp_hurt::died}, 3, "Mido", "Saria") ==
          "Saria defeated Mido");
    CHECK_FALSE(CoopPvp::HurtLine(proto::PvpHurt{2, 1, 0, 10, proto::pvp_hurt::guarded}, 1, "Mido", "Saria"));
}

TEST_CASE("pvp: a monster's hit while a replayed PvP hit waits is not booked as the PvP hit") {
    using CoopPvp::PendingGive;
    // Life 12, a PvP hurt of 4 replayed (expect 8); a monster takes 4 before the game takes ours.
    CoopPvp::LinkLifeWatch w;
    std::vector<PendingGive> gives{PendingGive{4, 8, 100, true}};
    CHECK_FALSE(w.Follow(gives, 12, 8, /*own_hit*/ true, /*replay_hit*/ false, /*id_read*/ true));
    CHECK(gives[0].expect == 4);  // still waits: life 8 > 4, so a refused replay ends in the written 4
    // Both in one read: the replay takes its 4 first, the monster the rest.
    w.Reset();
    gives = {PendingGive{4, 8, 100, true}};
    CHECK_FALSE(w.Follow(gives, 12, 2, true, true, true));
    CHECK(gives[0].expect == 2);  // life 2 <= 2: taken
    // Our replayed id with the fall: the landing.
    w.Reset();
    gives = {PendingGive{4, 8, 100, true}};
    CHECK_FALSE(w.Follow(gives, 12, 8, false, true, true));
    CHECK(gives[0].expect == 8);
    // The id not read: a fall is the landing, as before.
    w.Reset();
    CHECK_FALSE(w.Follow(gives, 12, 8, false, false, false));
    CHECK(gives[0].expect == 8);
    // Never below 0.
    w.Reset();
    gives = {PendingGive{4, 2, 100, true}};
    w.Follow(gives, 6, 0, true, false, true);
    CHECK(gives[0].expect == 0);
}

TEST_CASE("pvp: a heart while a PvP hit waits is not taken back by the written life") {
    using CoopPvp::PendingGive;
    CoopPvp::LinkLifeWatch w;
    std::vector<PendingGive> gives{PendingGive{4, 8, 100, true}, PendingGive{2, 6, 120, false}};
    CHECK_FALSE(w.Follow(gives, 12, 16, false, false, true));
    CHECK(gives[0].expect == 12);
    CHECK(gives[1].expect == 10);
    // Not read yet, or unchanged: nothing moves.
    w.Follow(gives, -1, 4, true, false, true);
    w.Follow(gives, 16, 16, true, false, true);
    CHECK(gives[0].expect == 12);
}

TEST_CASE("pvp: a fall, fire or drowning (no hit id moved) is held one read, then not booked as the PvP hit") {
    using CoopPvp::PendingGive;
    CoopPvp::LinkLifeWatch w;
    std::vector<PendingGive> gives{PendingGive{4, 8, 100, true}};
    CHECK(w.Follow(gives, 12, 10, false, false, true));  // fire 2: held (its id may come next read)
    CHECK(gives[0].expect == 8);
    CHECK_FALSE(w.Follow(gives, 10, 10, false, false, true));  // no id came: harm
    CHECK(gives[0].expect == 6);
    CHECK_FALSE(w.Follow(gives, 10, 6, false, true, true));  // then the replay lands with its id
    CHECK(gives[0].expect == 6);  // life 6 <= 6: taken once
    // A refused replay (its id, no fall) and a fall two reads later: that fall is the void's.
    w.Reset();
    gives = {PendingGive{4, 8, 100, true}};
    w.Follow(gives, 12, 12, false, true, true);
    w.Follow(gives, 12, 12, false, false, true);
    CHECK(w.Follow(gives, 12, 9, false, false, true));
    w.Follow(gives, 9, 9, false, false, true);
    CHECK(gives[0].expect == 5);  // the PvP hit still owed: written to 5 after the wait
}

TEST_CASE("pvp: the replay's step landing between the life and hit id reads is taken once (review of 367e23f3)") {
    using CoopPvp::PendingGive;
    // Its fall read a walk after its id: the id read alone carries its take into the next read.
    CoopPvp::LinkLifeWatch w;
    std::vector<PendingGive> gives{PendingGive{4, 8, 100, true}};
    CHECK_FALSE(w.Follow(gives, 12, 12, false, true, true));
    CHECK_FALSE(w.Follow(gives, 12, 8, false, false, true));
    CHECK(gives[0].expect == 8);  // life 8 <= 8: the landing, not written again (was: lowered to 4, dealt twice)
    // Its fall read a walk before its id: held, then paired with the id.
    w.Reset();
    gives = {PendingGive{4, 8, 100, true}};
    CHECK(w.Follow(gives, 12, 8, false, false, true));  // nothing booked or written this read
    CHECK_FALSE(w.Follow(gives, 8, 8, false, true, true));
    CHECK(gives[0].expect == 8);  // the landing
    // A monster's hit read before its id: held, then lowered as harm once the id is not ours.
    w.Reset();
    gives = {PendingGive{4, 8, 100, true}};
    CHECK(w.Follow(gives, 12, 9, false, false, true));
    CHECK_FALSE(w.Follow(gives, 9, 9, true, false, true));
    CHECK(gives[0].expect == 5);
}

TEST_CASE("pvp: a held fall is booked only against the hurts waiting when it was held (review-a38-4)") {
    using CoopPvp::PendingGive;
    // Life 12, a replayed hurt of 4 waits (expect 8). Fire 2 with no id: held; the caller takes no new ask that read.
    CoopPvp::LinkLifeWatch w;
    std::vector<PendingGive> gives{PendingGive{4, 8, 100, true}};
    CHECK(w.Follow(gives, 12, 10, false, false, true));
    // Next read no id came: harm. The ask that waited is taken now, after the waiting hurt and the harm.
    CHECK_FALSE(w.Follow(gives, 10, 10, false, false, true));
    CHECK(gives[0].expect == 6);
    gives.push_back(PendingGive{4, CoopPvp::NextExpect(gives, 10, 4), 200, true});
    CHECK(gives[1].expect == 2);  // (was: 10-4 = 6 at the hold read, lowered by the harm again to 4: 2 extra)
    // Neither is lowered again by that harm: the first replay lands with its id, then the second.
    CHECK_FALSE(w.Follow(gives, 10, 6, false, true, true));
    CHECK(gives[0].expect == 6);
    CHECK(gives[1].expect == 2);
    gives.erase(gives.begin());  // (life 6 <= 6: the caller books the first)
    CHECK_FALSE(w.Follow(gives, 6, 2, false, true, true));
    CHECK(gives[0].expect == 2);  // life 2 <= 2: the second taken once
}

TEST_CASE("pvp: a held fall and our replayed id with a new fall in one read are booked as one change") {
    using CoopPvp::PendingGive;
    // Fire 2 held; next read the replay's id and its fall of 4: harm 2, landing 4. Not held again.
    CoopPvp::LinkLifeWatch w;
    std::vector<PendingGive> gives{PendingGive{4, 8, 100, true}};
    CHECK(w.Follow(gives, 12, 10, false, false, true));
    CHECK_FALSE(w.Follow(gives, 10, 6, false, true, true));
    CHECK(gives[0].expect == 6);  // life 6 <= 6: taken once (was: the replay's fall held again, lowered to 4)
    CHECK_FALSE(w.Follow(gives, 6, 6, false, false, true));
    CHECK(gives[0].expect == 6);
    // The held fall was the landing (its id came next read with no new fall): nothing lowered.
    w.Reset();
    gives = {PendingGive{4, 8, 100, true}};
    CHECK(w.Follow(gives, 12, 8, false, false, true));
    CHECK_FALSE(w.Follow(gives, 8, 8, false, true, true));
    CHECK(gives[0].expect == 8);
    // With a monster's id too: the replay's part first, the rest (held 2 + 3) lowered once.
    w.Reset();
    gives = {PendingGive{4, 8, 100, true}};
    CHECK(w.Follow(gives, 12, 10, false, false, true));
    CHECK_FALSE(w.Follow(gives, 10, 3, true, true, true));
    CHECK(gives[0].expect == 3);
}

TEST_CASE("pvp: hurts waiting together expect lives after each other") {
    using CoopPvp::PendingGive;
    std::vector<PendingGive> gives;
    gives.push_back(PendingGive{4, CoopPvp::NextExpect(gives, 12, 4), 100, true});
    gives.push_back(PendingGive{4, CoopPvp::NextExpect(gives, 12, 4), 100, true});
    CHECK(gives[0].expect == 8);
    CHECK(gives[1].expect == 4);  // (was 8: the first landing booked both, the second refused hit was lost)
    // A waiting hurt already landed this read (expect above life): the new one starts from the life.
    gives = {PendingGive{2, 10, 100, true}};
    CHECK(CoopPvp::NextExpect(gives, 9, 3) == 6);
    CHECK(CoopPvp::NextExpect({}, 2, 3) == 0);  // never below 0
    // The first lands; the second still waits for its own.
    CoopPvp::LinkLifeWatch w;
    gives = {PendingGive{4, 8, 100, true}, PendingGive{4, 4, 100, true}};
    CHECK_FALSE(w.Follow(gives, 12, 8, false, true, true));
    CHECK(gives[0].expect == 8);
    CHECK(gives[1].expect == 4);
}
