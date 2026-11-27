// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native puppets phase 3: lifting another player, the rules (coop_lift_player.h,
// docs/native-puppets.md "Lifting another player").
#include <doctest/doctest.h>

#include "coop_lift_player.h"

namespace proto = coop::protocol;
using namespace CoopLiftPlayer;

namespace {

constexpr std::uint64_t kFrame = 16'667;
const Vec3 kLink{0.0f, 0.0f, 0.0f};

// A young (child) player's copy unless said otherwise: only those are ever lifted.
CopyView Copy(std::uint8_t number, Vec3 drawn, std::optional<Vec3> game, bool young = true) {
    return CopyView{number, drawn, game, young};
}

// A copy the game lifted onto our Link's hands: drawn on the floor beside us, raised 0.9 m by the game.
CopyView Lifted(std::uint8_t number = 2) {
    return Copy(number, {0.5f, 0.0f, 0.0f}, Vec3{0.3f, 0.9f, 0.0f});
}

// A lifter whose own Link plays a lift.
Lifter Lifting() {
    Lifter l;
    l.SetOurLinkLifts(true);
    return l;
}

Local Here(std::uint8_t self = 2) {
    Local l;
    l.self = self;
    l.our_link = Vec3{10.0f, 0.0f, 10.0f};
    l.young = true;
    return l;
}

}  // namespace

TEST_CASE("lift player: a copy the game raises beside our Link is asked for once, after two frames") {
    Lifter lifter = Lifting();
    std::uint64_t now = 1'000'000;
    // Standing beside it, drawn where the samples say: nothing.
    CHECK_FALSE(lifter.Frame(kLink, {Copy(2, {0.5f, 0, 0}, Vec3{0.5f, 0, 0})}, now).ask);
    // Raised one frame: not yet (a bump, a step).
    CHECK_FALSE(lifter.Frame(kLink, {Lifted()}, now += kFrame).ask);
    const auto out = lifter.Frame(kLink, {Lifted()}, now += kFrame);
    REQUIRE(out.ask);
    CHECK(*out.ask == proto::LiftAsk{proto::kPlayerSelf, 2});
    CHECK(lifter.Held() == 2);
    CHECK(lifter.Now() == Lifter::State::asking);
    // Waiting: no second ask, no place before the yes.
    const auto wait = lifter.Frame(kLink, {Lifted()}, now += kFrame);
    CHECK_FALSE(wait.ask);
    CHECK_FALSE(wait.place);
}

TEST_CASE("lift player: a raised copy out of our reach, or standing on it, is not a lift") {
    Lifter lifter = Lifting();
    std::uint64_t now = 0;
    // Raised but 5 m away (another thing moved it): no.
    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(lifter.Frame(kLink, {Copy(2, {5, 0, 0}, Vec3{5, 1, 0})}, now += kFrame).ask);
    }
    // Pushed along the floor (phase 1 body): no.
    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(lifter.Frame(kLink, {Copy(2, {0.5f, 0, 0}, Vec3{0.9f, 0, 0})}, now += kFrame).ask);
    }
    CHECK(lifter.Held() == 0);
}

TEST_CASE("lift player: taken, carried, thrown, flying until at rest, then one landed place") {
    Lifter lifter = Lifting();
    std::uint64_t now = 0;
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    REQUIRE(lifter.Frame(kLink, {Lifted()}, now += kFrame).ask);
    lifter.Answer(proto::LiftAnswer{2, 1, proto::LiftRefusal::none}, now);
    CHECK(lifter.Now() == Lifter::State::holding);
    // Carried: each frame's place as the game has it, held. Walking and turning keep it in our hands.
    auto out = lifter.Frame(kLink, {Lifted()}, now += kFrame);
    REQUIRE(out.place);
    CHECK(out.place->phase == proto::LiftPhase::held);
    CHECK(out.place->lifted == 2);
    CHECK(out.place->position == Vec3{0.3f, 0.9f, 0.0f});
    const Vec3 walked{3.0f, 0.0f, 0.0f};
    out = lifter.Frame(walked, {Copy(2, {0.5f, 0, 0}, Vec3{2.7f, 0.9f, 0.0f})}, now += kFrame);
    REQUIRE(out.place);
    CHECK(out.place->phase == proto::LiftPhase::held);
    out = lifter.Frame(walked, {Copy(2, {0.5f, 0, 0}, Vec3{3.0f, 0.9f, 0.3f})}, now += kFrame);  // turned
    CHECK(out.place->phase == proto::LiftPhase::held);
    // Thrown: 1.5 m out in one frame: flying, places go on.
    out = lifter.Frame(walked, {Copy(2, {0.5f, 0, 0}, Vec3{3.0f, 1.2f, 1.8f})}, now += kFrame);
    REQUIRE(out.place);
    CHECK(out.place->phase == proto::LiftPhase::flying);
    CHECK(lifter.Held() == 2);
    Vec3 at{3.0f, 0.4f, 4.0f};
    out = lifter.Frame(walked, {Copy(2, {0.5f, 0, 0}, at)}, now += kFrame);
    CHECK(out.place->phase == proto::LiftPhase::flying);
    // At rest for kRestFrames frames: one landed place, and the director places it again.
    int landed = 0;
    for (int i = 0; i < kRestFrames + 2; ++i) {
        out = lifter.Frame(walked, {Copy(2, {0.5f, 0, 0}, at)}, now += kFrame);
        if (out.place && out.place->phase == proto::LiftPhase::landed) {
            ++landed;
            CHECK(out.place->position == at);
        }
    }
    CHECK(landed == 1);
    CHECK(lifter.Held() == 0);
}

TEST_CASE("lift player: set down (lower than carried) is a flight too, and a flight ends by kFlightMaxUs") {
    Lifter lifter = Lifting();
    std::uint64_t now = 0;
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    lifter.Answer(proto::LiftAnswer{2, 1, proto::LiftRefusal::none}, now);
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    auto out = lifter.Frame(kLink, {Copy(2, {0.5f, 0, 0}, Vec3{0.6f, 0.1f, 0.0f})}, now += kFrame);
    REQUIRE(out.place);
    CHECK(out.place->phase == proto::LiftPhase::flying);
    // Never quite still (a slope): ends anyway.
    bool landed = false;
    for (int i = 0; i < 200 && !landed; ++i) {
        const float x = 0.6f + 0.05f * static_cast<float>(i);
        out = lifter.Frame(kLink, {Copy(2, {0.5f, 0, 0}, Vec3{x, 0.1f, 0.0f})}, now += kFrame);
        landed = out.place && out.place->phase == proto::LiftPhase::landed;
    }
    CHECK(landed);
    CHECK(now <= 4 * kFrame + kFlightMaxUs + 2 * kFrame);
}

TEST_CASE("lift player: refused or unanswered, the copy is let go and not asked again until it is down") {
    Lifter lifter = Lifting();
    std::uint64_t now = 0;
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    lifter.Answer(proto::LiftAnswer{2, 1, proto::LiftRefusal::event}, now);
    CHECK(lifter.Held() == 0);
    CHECK(lifter.LastRefusal() == proto::LiftRefusal::event);
    CHECK(lifter.Blocked(2));
    // Still raised (the game's lift holds on): no ask every frame.
    for (int i = 0; i < 10; ++i) CHECK_FALSE(lifter.Frame(kLink, {Lifted()}, now += kFrame).ask);
    // Down again, then a new lift: asked.
    lifter.Frame(kLink, {Copy(2, {0.5f, 0, 0}, Vec3{0.5f, 0, 0})}, now += kFrame);
    CHECK_FALSE(lifter.Blocked(2));
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    CHECK(lifter.Frame(kLink, {Lifted()}, now += kFrame).ask);
    // No answer at all: let go after kAnswerWaitUs.
    lifter.Frame(kLink, {Lifted()}, now += kAnswerWaitUs);
    CHECK(lifter.Held() == 0);
    CHECK(lifter.Blocked(2));
    // A late yes for a lift we dropped changes nothing (the lifted game frees itself: no places).
    lifter.Answer(proto::LiftAnswer{2, 1, proto::LiftRefusal::none}, now);
    CHECK(lifter.Held() == 0);
}

TEST_CASE("lift player: freed by the lifted game, or the copy gone, ends the carry") {
    Lifter lifter = Lifting();
    std::uint64_t now = 0;
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    lifter.Answer(proto::LiftAnswer{2, 1, proto::LiftRefusal::none}, now);
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    // Another player's answer is not this lift's.
    lifter.Answer(proto::LiftAnswer{3, 1, proto::LiftRefusal::freed}, now);
    CHECK(lifter.Held() == 2);
    lifter.Answer(proto::LiftAnswer{2, 1, proto::LiftRefusal::freed}, now);
    CHECK(lifter.Held() == 0);
    // Gone mid-carry: the last place, landed.
    for (int i = 0; i < 20; ++i) lifter.Frame(kLink, {}, now += kFrame);  // unblocks (no copy)
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    lifter.Answer(proto::LiftAnswer{2, 1, proto::LiftRefusal::none}, now);
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    const auto out = lifter.Frame(kLink, {}, now += kFrame);
    REQUIRE(out.place);
    CHECK(out.place->phase == proto::LiftPhase::landed);
    CHECK(lifter.Held() == 0);
}

TEST_CASE("lift player: the lifted game decides (event, down, busy, far, lifting someone)") {
    const proto::LiftAsk ask{1, 2};
    const Vec3 close_by{10.5f, 0.0f, 10.0f};
    {
        Victim v;
        Local l = Here();
        l.event = true;
        CHECK(v.Ask(ask, l, close_by, 0).refusal == proto::LiftRefusal::event);
        CHECK(v.By() == 0);
    }
    {
        Victim v;
        Local l = Here();
        l.down = true;
        CHECK(v.Ask(ask, l, close_by, 0).refusal == proto::LiftRefusal::down);
    }
    {
        Victim v;
        Local l = Here();
        l.lifting = true;
        CHECK(v.Ask(ask, l, close_by, 0).refusal == proto::LiftRefusal::busy);
    }
    {
        Victim v;
        CHECK(v.Ask(ask, Here(), Vec3{20, 0, 10}, 0).refusal == proto::LiftRefusal::too_far);
    }
    {
        Victim v;
        Local l = Here();
        l.our_link.reset();  // no Link read: a load
        CHECK(v.Ask(ask, l, close_by, 0).refusal == proto::LiftRefusal::event);
    }
    {
        Victim v;
        const auto yes = v.Ask(ask, Here(), close_by, 0);
        CHECK(yes == proto::LiftAnswer{proto::kPlayerSelf, 1, proto::LiftRefusal::none});
        CHECK(v.By() == 1);
        // A second lifter while lifted: busy; the same one again: yes.
        CHECK(v.Ask(proto::LiftAsk{3, 2}, Here(), close_by, 0).refusal == proto::LiftRefusal::busy);
        CHECK(v.Ask(ask, Here(), close_by, 0).refusal == proto::LiftRefusal::none);
    }
}

TEST_CASE("lift player: lifted, our Link follows the lifter's places with input off, then lands its own") {
    Victim v;
    std::uint64_t now = 0;
    REQUIRE(v.Ask(proto::LiftAsk{1, 2}, Here(), Vec3{10.5f, 0.0f, 10.0f}, now).refusal == proto::LiftRefusal::none);
    // Before the first place: input off, nowhere to put it yet.
    auto s = v.Frame(Here(), now += kFrame);
    CHECK(s.input_off);
    CHECK_FALSE(s.put);
    // Another player's place is not taken.
    v.Place(proto::LiftPlace{3, 2, proto::LiftPhase::held, {9, 9, 9}}, now);
    v.Place(proto::LiftPlace{1, 2, proto::LiftPhase::held, {1, 2, 3}}, now);
    s = v.Frame(Here(), now += kFrame);
    CHECK(s.input_off);
    REQUIRE(s.put);
    CHECK(*s.put == Vec3{1, 2, 3});
    v.Place(proto::LiftPlace{1, 2, proto::LiftPhase::flying, {1, 3, 5}}, now);
    s = v.Frame(Here(), now += kFrame);
    CHECK(*s.put == Vec3{1, 3, 5});
    // Landed: put there once, input back, the lift over (no freed answer: the lifter ended it).
    v.Place(proto::LiftPlace{1, 2, proto::LiftPhase::landed, {1, 0, 7}}, now);
    s = v.Frame(Here(), now += kFrame);
    CHECK(*s.put == Vec3{1, 0, 7});
    CHECK_FALSE(s.input_off);
    CHECK_FALSE(s.freed);
    CHECK(v.By() == 0);
    s = v.Frame(Here(), now += kFrame);
    CHECK_FALSE(s.put);
    CHECK_FALSE(s.input_off);
}

TEST_CASE("lift player: the lifted game frees itself (event, death, no places, held too long)") {
    const auto lifted = [](Victim& v, std::uint64_t now) {
        REQUIRE(v.Ask(proto::LiftAsk{1, 2}, Here(), Vec3{10.5f, 0.0f, 10.0f}, now).refusal == proto::LiftRefusal::none);
        v.Place(proto::LiftPlace{1, 2, proto::LiftPhase::held, {1, 2, 3}}, now);
    };
    {
        Victim v;
        lifted(v, 0);
        Local l = Here();
        l.event = true;  // a talk or a cutscene starts
        const auto s = v.Frame(l, kFrame);
        REQUIRE(s.freed);
        CHECK(*s.freed == proto::LiftAnswer{proto::kPlayerSelf, 1, proto::LiftRefusal::freed});
        CHECK_FALSE(s.input_off);
        CHECK(v.By() == 0);
    }
    {
        Victim v;
        lifted(v, 0);
        Local l = Here();
        l.down = true;
        CHECK(v.Frame(l, kFrame).freed);
    }
    {
        Victim v;
        lifted(v, 0);
        CHECK_FALSE(v.Frame(Here(), kPlaceWaitUs - 1).freed);
        CHECK(v.Frame(Here(), kPlaceWaitUs).freed);  // the lifter went quiet
    }
    {
        Victim v;
        lifted(v, 0);
        std::uint64_t now = 0;
        bool freed = false;
        while (!freed && now < kMaxHeldUs + kFrame) {
            now += 100'000;
            v.Place(proto::LiftPlace{1, 2, proto::LiftPhase::held, {1, 2, 3}}, now);
            freed = v.Frame(Here(), now).freed.has_value();
        }
        CHECK(freed);
        CHECK(now >= kMaxHeldUs);  // wriggled free
    }
}

TEST_CASE("lift player: a copy raised while our own Link does not lift (a platform, a step) is never asked for") {
    Lifter lifter;  // our Link's animation not read: no lift
    std::uint64_t now = 0;
    for (int i = 0; i < 5; ++i) {
        const auto out = lifter.Frame(kLink, {Lifted()}, now += kFrame);
        CHECK_FALSE(out.ask);
        // The probe line once per rise, not every frame.
        CHECK(out.rose_alone == (i == 0 ? 2 : 0));
    }
    lifter.SetOurLinkLifts(false);
    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(lifter.Frame(kLink, {Lifted()}, now += kFrame).ask);
    }
    CHECK(lifter.Held() == 0);
    // Down again, then our Link's own lift raises it: asked.
    CHECK(lifter.Frame(kLink, {Copy(2, {0.5f, 0, 0}, Vec3{0.5f, 0, 0})}, now += kFrame).rose_alone == 0);
    lifter.SetOurLinkLifts(true);
    CHECK_FALSE(lifter.Frame(kLink, {Lifted()}, now += kFrame).ask);
    CHECK(lifter.Frame(kLink, {Lifted()}, now += kFrame).ask);
}

TEST_CASE("lift player: our Link's own lift is read from its animation name") {
    CHECK_FALSE(OurLinkLifts(std::nullopt));
    CHECK_FALSE(OurLinkLifts(std::string_view("Wait")));
    CHECK_FALSE(OurLinkLifts(std::string_view("Run")));
    CHECK_FALSE(OurLinkLifts(std::string_view("")));
    CHECK(OurLinkLifts(std::string_view("Lift_Start")));
    CHECK(OurLinkLifts(std::string_view("WaitLIFT")));
    CHECK(OurLinkLifts(std::string_view("CarryRun")));
}

TEST_CASE("lift player: an ask from a lifter whose place we do not know is refused") {
    Victim v;
    CHECK(v.Ask(proto::LiftAsk{1, 2}, Here(), std::nullopt, 0).refusal == proto::LiftRefusal::too_far);
    CHECK(v.By() == 0);
    CHECK_FALSE(v.Frame(Here(), kFrame).input_off);
}

TEST_CASE("lift player: input off holds only while the lift's frame renews it") {
    CHECK_FALSE(InputOffFresh(0, 1'000'000));
    CHECK(InputOffFresh(1'000'000, 1'000'000));
    CHECK(InputOffFresh(1'000'000, 1'000'000 + kInputOffStaleUs));
    CHECK_FALSE(InputOffFresh(1'000'000, 1'000'001 + kInputOffStaleUs));
    CHECK(InputOffFresh(2'000'000, 1'000'000));  // a stamp from a later read: still fresh
}

// Owner rule (2026-11-19): only a YOUNG Link is lifted, carried as Ruto; any lifter age.

TEST_CASE("lift player: an adult's copy (or one whose age is not known) is never asked for, and logs nothing") {
    Lifter lifter = Lifting();
    std::uint64_t now = 0;
    for (int i = 0; i < 10; ++i) {
        const CopyView adult = Copy(2, {0.5f, 0.0f, 0.0f}, Vec3{0.3f, 0.9f, 0.0f}, false);
        const auto out = lifter.Frame(kLink, {adult}, now += kFrame);
        CHECK_FALSE(out.ask);
        CHECK_FALSE(out.place);
        CHECK(out.rose_alone == 0);  // no probe line either: nothing happens at all
    }
    CHECK(lifter.Held() == 0);
    // Not our own lift either: still no line for an adult.
    Lifter idle;
    CHECK(idle.Frame(kLink, {Copy(2, {0.5f, 0, 0}, Vec3{0.3f, 0.9f, 0.0f}, false)}, kFrame).rose_alone == 0);
    // The same player young again (the age is re-read every frame): asked as usual.
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    CHECK(lifter.Frame(kLink, {Lifted()}, now += kFrame).ask);
}

TEST_CASE("lift player: an adult lifter lifts a young Link (the lifter's age is never a reason)") {
    // The rules have no lifter age at all: our Link (adult or young) asks for a young copy and the young
    // lifted game says yes, whatever the lifter's age.
    Lifter adult_lifter = Lifting();
    std::uint64_t now = 0;
    adult_lifter.Frame(kLink, {Lifted()}, now += kFrame);
    const auto out = adult_lifter.Frame(kLink, {Lifted()}, now += kFrame);
    REQUIRE(out.ask);
    proto::LiftAsk relayed = *out.ask;
    relayed.player_number = 1;  // the server writes the lifter in
    Victim young;
    const auto answer = young.Ask(relayed, Here(2), Vec3{10.5f, 0.0f, 10.0f}, now);
    CHECK(answer.refusal == proto::LiftRefusal::none);
    CHECK(young.Carried());
}

TEST_CASE("lift player: the copy grows up while asked or carried: the lift ends with a landed place") {
    Lifter lifter = Lifting();
    std::uint64_t now = 0;
    // Asking, then the copy is adult: dropped without waiting for the answer.
    lifter.Frame(kLink, {Lifted()}, now += kFrame);
    REQUIRE(lifter.Frame(kLink, {Lifted()}, now += kFrame).ask);
    lifter.Frame(kLink, {Copy(2, {0.5f, 0, 0}, Vec3{0.3f, 0.9f, 0.0f}, false)}, now += kFrame);
    CHECK(lifter.Held() == 0);
    // Carried, then adult: one landed place where we last had it, then let go.
    Lifter carry = Lifting();
    carry.Frame(kLink, {Lifted()}, now += kFrame);
    REQUIRE(carry.Frame(kLink, {Lifted()}, now += kFrame).ask);
    carry.Answer(proto::LiftAnswer{2, 1, proto::LiftRefusal::none}, now);
    REQUIRE(carry.Frame(kLink, {Lifted()}, now += kFrame).place);
    const auto out = carry.Frame(kLink, {Copy(2, {0.5f, 0, 0}, Vec3{0.3f, 0.9f, 0.0f}, false)}, now += kFrame);
    REQUIRE(out.place);
    CHECK(out.place->phase == proto::LiftPhase::landed);
    CHECK(out.place->position == Vec3{0.3f, 0.9f, 0.0f});
    CHECK(carry.Held() == 0);
    // Flying when it grows up: the same.
    Lifter fly = Lifting();
    fly.Frame(kLink, {Lifted()}, now += kFrame);
    fly.Frame(kLink, {Lifted()}, now += kFrame);
    fly.Answer(proto::LiftAnswer{2, 1, proto::LiftRefusal::none}, now);
    fly.Frame(kLink, {Lifted()}, now += kFrame);
    REQUIRE(fly.Frame(kLink, {Copy(2, {0.5f, 0, 0}, Vec3{0.3f, 1.2f, 1.8f})}, now += kFrame).place->phase ==
            proto::LiftPhase::flying);
    const auto f = fly.Frame(kLink, {Copy(2, {0.5f, 0, 0}, Vec3{0.3f, 1.0f, 2.5f}, false)}, now += kFrame);
    REQUIRE(f.place);
    CHECK(f.place->phase == proto::LiftPhase::landed);
    CHECK(fly.Held() == 0);
}

TEST_CASE("lift player: the lifted game refuses when its own Link is adult or its age not read (not_young)") {
    Victim v;
    Local adult = Here(2);
    adult.young = false;
    const auto a = v.Ask(proto::LiftAsk{1, 2}, adult, Vec3{10.5f, 0.0f, 10.0f}, 0);
    CHECK(a.refusal == proto::LiftRefusal::not_young);
    CHECK(a.lifter == 1);
    CHECK_FALSE(v.Carried());
    CHECK_FALSE(v.Frame(adult, kFrame).input_off);  // nothing taken: input stays ours
    // Age changed while the ask was on its way (asked as child, adult when it lands): the answer is the game's now.
    Victim w;
    Local grown = Here(2);
    grown.young = false;
    CHECK(w.Ask(proto::LiftAsk{1, 2}, grown, Vec3{10.5f, 0.0f, 10.0f}, 0).refusal == proto::LiftRefusal::not_young);
    // Young: yes.
    CHECK(w.Ask(proto::LiftAsk{1, 2}, Here(2), Vec3{10.5f, 0.0f, 10.0f}, 0).refusal == proto::LiftRefusal::none);
}

TEST_CASE("lift player: grown up while lifted, the lifted game frees itself (not_young) and its Link is its own") {
    Victim v;
    std::uint64_t now = 0;
    REQUIRE(v.Ask(proto::LiftAsk{1, 2}, Here(2), Vec3{10.5f, 0.0f, 10.0f}, now).refusal == proto::LiftRefusal::none);
    v.Place(proto::LiftPlace{1, 2, proto::LiftPhase::held, Vec3{10.3f, 0.9f, 10.0f}}, now += kFrame);
    CHECK(v.Frame(Here(2), now).input_off);
    CHECK(v.Carried());
    Local grown = Here(2);
    grown.young = false;
    const auto step = v.Frame(grown, now += kFrame);
    REQUIRE(step.freed);
    CHECK(step.freed->refusal == proto::LiftRefusal::not_young);
    CHECK(step.freed->lifter == 1);
    CHECK_FALSE(step.input_off);  // input back: our Link falls and lands by its own controller
    CHECK_FALSE(step.put);        // and is not put anywhere by us
    CHECK_FALSE(v.Carried());
    // Places that still come are ignored.
    v.Place(proto::LiftPlace{1, 2, proto::LiftPhase::held, Vec3{10.3f, 0.9f, 10.0f}}, now += kFrame);
    CHECK_FALSE(v.Frame(grown, now).put);
}

TEST_CASE("lift player: carried as Ruto: Link's own Ruto carry counts as a lift, and the carried pose") {
    CHECK(OurLinkLifts(std::string_view("Lift_Up_Ruto")));
    CHECK(OurLinkLifts(std::string_view("Lift_Ready_Ruto")));
    CHECK(OurLinkLifts(std::string_view("Pose_Ruto")));
    CHECK(OurLinkLifts(std::string_view("Pose_Ruto_Run")));
    CHECK_FALSE(OurLinkLifts(std::string_view("Pose_Bow")));
    const auto pose = CarriedAnimation();
    CHECK(pose.name == "Eaten");
    CHECK(pose.frame == 0.0f);
    CHECK(pose.rate == 1.0f);  // the copy loops Ruto's Act_LiftRest_Wait, not frame 0 held
}

TEST_CASE("lift player: row 312, a lifted player mashing breaks free, a held button or slow presses do not") {
    const auto lifted = [](Victim& v, std::uint64_t now) {
        REQUIRE(v.Ask(proto::LiftAsk{1, 2}, Here(), Vec3{10.5f, 0.0f, 10.0f}, now).refusal == proto::LiftRefusal::none);
        v.Place(proto::LiftPlace{1, 2, proto::LiftPhase::held, {1, 2, 3}}, now);
    };
    CHECK(NewPresses(0, 0b11) == 2);
    CHECK(NewPresses(0b01, 0b11) == 1);
    CHECK(NewPresses(0b11, 0b11) == 0);  // held: no new press
    CHECK(NewPresses(0b11, 0) == 0);     // let go: no press
    {
        // Mashing: one press every 100 ms (10 a second against the drain of 2) frees in about 1.3 s, not before
        // kStruggleFull presses.
        Victim v;
        lifted(v, 0);
        std::uint64_t now = 0;
        int presses = 0;
        Victim::Step s;
        while (!s.freed && now < 10'000'000) {
            now += 100'000;
            v.Place(proto::LiftPlace{1, 2, proto::LiftPhase::held, {1, 2, 3}}, now);
            v.Struggle(1, now);
            ++presses;
            s = v.Frame(Here(), now);
        }
        REQUIRE(s.freed);
        CHECK(s.struggled);
        CHECK(*s.freed == proto::LiftAnswer{proto::kPlayerSelf, 1, proto::LiftRefusal::freed});
        CHECK_FALSE(s.input_off);
        CHECK(presses >= static_cast<int>(kStruggleFull));
        CHECK(now < kMaxHeldUs);
        CHECK(v.By() == 0);
        // Freed by a struggle: no new lift (from anyone) for kFreeGraceUs, then yes again.
        CHECK(v.Ask(proto::LiftAsk{1, 2}, Here(), Vec3{10.5f, 0.0f, 10.0f}, now + 1).refusal ==
              proto::LiftRefusal::busy);
        CHECK(v.Ask(proto::LiftAsk{3, 2}, Here(), Vec3{10.5f, 0.0f, 10.0f}, now + kFreeGraceUs - 1).refusal ==
              proto::LiftRefusal::busy);
        CHECK(v.Ask(proto::LiftAsk{1, 2}, Here(), Vec3{10.5f, 0.0f, 10.0f}, now + kFreeGraceUs).refusal ==
              proto::LiftRefusal::none);
        CHECK(v.Meter() == 0.0f);  // a new lift starts with an empty meter
    }
    {
        // One press every 600 ms: the drain (2 a second) empties it each time; never free by struggling.
        Victim v;
        lifted(v, 0);
        bool struggled = false;
        for (std::uint64_t now = 600'000; now < 10'000'000; now += 600'000) {
            v.Place(proto::LiftPlace{1, 2, proto::LiftPhase::held, {1, 2, 3}}, now);
            v.Struggle(1, now);
            const auto s = v.Frame(Here(), now);
            struggled = struggled || s.struggled;
        }
        CHECK_FALSE(struggled);
        CHECK(v.By() == 1);  // still held by P1
    }
    {
        // Presses before the lift, or after it ended, count for nothing.
        Victim v;
        v.Struggle(50, 0);
        lifted(v, 0);
        CHECK(v.Meter() == 0.0f);
        const auto s = v.Frame(Here(), kFrame);
        CHECK_FALSE(s.freed);
        CHECK(s.input_off);
    }
    {
        // Other frees (an event) are not "struggled" and leave no grace.
        Victim v;
        lifted(v, 0);
        Local l = Here();
        l.event = true;
        const auto s = v.Frame(l, kFrame);
        REQUIRE(s.freed);
        CHECK_FALSE(s.struggled);
        CHECK(v.Ask(proto::LiftAsk{1, 2}, Here(), Vec3{10.5f, 0.0f, 10.0f}, 2 * kFrame).refusal ==
              proto::LiftRefusal::none);
    }
}
