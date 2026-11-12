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
