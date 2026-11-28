// Tests for down but not out (lichii/hook/coop_dbno.cpp).
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop/protocol/scene_place.hpp"
#include "coop_dbno.h"

namespace proto = coop::protocol;
using CoopDbno::Event;
using CoopDbno::Guard;
using CoopDbno::Guarded;
using CoopDbno::Input;
using CoopDbno::Machine;
using CoopDbno::Output;
using CoopDbno::Rules;
using CoopDbno::State;
using CoopDbno::Teammate;
using CoopDbno::Vec3;

namespace {

constexpr std::uint64_t kS = 1'000'000;
constexpr std::uint64_t kFrame = 50'000;  // 20 frames a second

Rules On() {
    Rules r;
    r.on = true;
    return r;
}

Teammate Mate(std::uint8_t number, const Vec3& at = {1, 0, 0}, State state = State::up,
              std::string scene = "spot00") {
    Teammate t;
    t.number = number;
    t.state = state;
    t.in_game = true;
    t.scene = std::move(scene);
    t.position = at;
    return t;
}

// One player's game: the machine, its clock and what the frame is given.
struct Game {
    Machine machine;
    std::uint64_t now = 10 * kS;
    Rules rules = On();
    bool connected = true;
    bool guard_live = true;
    std::optional<std::int32_t> life = 12;
    std::optional<std::int32_t> max_life = 40;
    std::optional<std::string> scene = "spot00";
    Vec3 position{0, 0, 0};
    std::vector<Teammate> team{Mate(1)};
    std::vector<proto::DbnoStatus> sent;
    std::vector<Event> events;
    // The respawn's move: how it goes (answered the frame after it is asked).
    CoopDbno::Moved beside_result = CoopDbno::Moved::done;
    CoopDbno::Moved entrance_result = CoopDbno::Moved::done;
    CoopDbno::Moved pending = CoopDbno::Moved::none;
    int beside_asks = 0;
    int entrance_asks = 0;
    bool enemy_near_us = false;
    bool bottled_fairy = false;
    // Teammate::here as the glue's ShownHere gives it outside a dungeon (in
    // the game, our place, a position); false: the test sets it itself.
    bool auto_here = true;

    Output Step(bool lethal = false) {
        now += kFrame;
        if (auto_here) {
            for (Teammate& t : team) {
                t.here = t.in_game && t.scene && scene && proto::same_place(*t.scene, *scene) && t.position &&
                         std::isfinite((*t.position)[0]) && std::isfinite((*t.position)[1]) &&
                         std::isfinite((*t.position)[2]);
            }
        }
        Input in;
        in.now_us = now;
        in.rules = rules;
        in.connected = connected;
        in.guard_live = guard_live;
        in.lethal = lethal;
        in.life = life;
        in.max_life = max_life;
        in.scene = scene;
        in.position = position;
        in.team = team;
        in.moved = std::exchange(pending, CoopDbno::Moved::none);
        in.enemy_near_us = enemy_near_us;
        in.bottled_fairy = bottled_fairy;
        Output out = machine.Frame(in);
        if (out.move_beside != 0) {
            ++beside_asks;
            pending = beside_result;
        }
        if (out.move_entrance) {
            ++entrance_asks;
            pending = entrance_result;
        }
        if (out.write_life) {
            life = *out.write_life;
        }
        if (out.send) {
            sent.push_back(*out.send);
        }
        events.insert(events.end(), out.events.begin(), out.events.end());
        return out;
    }
    // A lethal hit as the guard leaves it: a quarter heart, and the flag.
    Output Down() {
        life = CoopDbno::kDownedLife;
        return Step(true);
    }
    Output Run(std::uint64_t us) {
        Output out;
        for (const std::uint64_t until = now + us; now < until;) {
            out = Step();
        }
        return out;
    }
    // The same with player `from` holding the revive button all along.
    Output RunHeld(std::uint64_t us, std::uint8_t from = 1) {
        Output out;
        for (const std::uint64_t until = now + us; now < until;) {
            machine.Hold(from, true, now);
            out = Step();
        }
        return out;
    }
    bool Saw(Event e) const { return std::find(events.begin(), events.end(), e) != events.end(); }
};

}  // namespace

TEST_CASE("dbno guard: a hit that would kill leaves a quarter heart, one that would not is the game's") {
    CHECK(Guarded(Guard::last_stand, 12, 4).sum == 4);
    CHECK_FALSE(Guarded(Guard::last_stand, 12, 4).lethal);
    CHECK(Guarded(Guard::last_stand, 12, 11).sum == 11);
    CHECK_FALSE(Guarded(Guard::last_stand, 12, 11).lethal);
    // Exactly the last life, and more than it.
    CHECK(Guarded(Guard::last_stand, 12, 12).sum == 11);
    CHECK(Guarded(Guard::last_stand, 12, 12).lethal);
    CHECK(Guarded(Guard::last_stand, 12, 80).sum == 11);
    CHECK(Guarded(Guard::last_stand, 12, 80).lethal);
    // Already on the last quarter: nothing more to take, and down.
    CHECK(Guarded(Guard::last_stand, 1, 1).sum == 0);
    CHECK(Guarded(Guard::last_stand, 1, 1).lethal);
    // Healing and nothing are never touched.
    CHECK(Guarded(Guard::last_stand, 4, -8).sum == -8);
    CHECK_FALSE(Guarded(Guard::last_stand, 4, -8).lethal);
    CHECK(Guarded(Guard::last_stand, 4, 0).sum == 0);
    // A Link the game already has at no life is the game's.
    CHECK(Guarded(Guard::last_stand, 0, 4).sum == 4);
    CHECK_FALSE(Guarded(Guard::last_stand, 0, 4).lethal);
}

TEST_CASE("dbno guard: off is the game's own, shielded takes nothing") {
    CHECK(Guarded(Guard::off, 4, 80).sum == 80);
    CHECK_FALSE(Guarded(Guard::off, 4, 80).lethal);
    CHECK(Guarded(Guard::shielded, 1, 80).sum == 0);
    CHECK_FALSE(Guarded(Guard::shielded, 1, 80).lethal);
    CHECK(Guarded(Guard::shielded, 1, -8).sum == 0);  // a downed Link does not heal himself up
}

TEST_CASE("dbno guard: the Life step's lethal hit shields at once and is counted once") {
    CoopDbno::LifeGuard guard;
    CHECK_FALSE(guard.Live());
    CHECK(guard.Step(4, 80) == 80);  // off until the frame says otherwise
    CHECK_FALSE(guard.TakeLethal());

    guard.Set(Guard::last_stand);
    CHECK(guard.Step(12, 4) == 4);
    CHECK_FALSE(guard.TakeLethal());
    CHECK(guard.Step(8, 8) == 7);
    CHECK(guard.Get() == Guard::shielded);
    CHECK(guard.Step(1, 8) == 0);  // a second hit in the same frame
    CHECK(guard.TakeLethal());
    CHECK_FALSE(guard.TakeLethal());
}

// Player report 34 (Kerbex's 0.11.0 log): a PvP hit of 2 at life 2 went in as
// the game's own hit, which the guard held at 1 (downed); 400 ms later PvP's
// fallback wrote the life it expected, 0, and the downed Link died.
TEST_CASE("dbno guard: a write past the Life step is guarded as the step is") {
    CoopDbno::LifeGuard guard;
    guard.Set(CoopDbno::Guard::last_stand);
    // The game's own hit: held back, downed.
    CHECK(guard.Step(2, 2) == 1);
    CHECK(guard.Get() == CoopDbno::Guard::shielded);
    // The fallback write of what PvP expected: nothing more is taken.
    CHECK(CoopDbno::GuardedWrite(guard, 1, 0) == 1);
    CHECK(guard.TakeLethal());
    CHECK_FALSE(guard.TakeLethal());  // counted once
    // Without the game's hit (it never came): the write itself downs him.
    guard.Set(CoopDbno::Guard::last_stand);
    CHECK(CoopDbno::GuardedWrite(guard, 2, 0) == CoopDbno::kDownedLife);
    CHECK(guard.TakeLethal());
    // A write that does not kill, and the rules off: as asked.
    guard.Set(CoopDbno::Guard::last_stand);
    CHECK(CoopDbno::GuardedWrite(guard, 8, 6) == 6);
    guard.Set(CoopDbno::Guard::off);
    CHECK(CoopDbno::GuardedWrite(guard, 2, 0) == 0);
    CHECK(CoopDbno::GuardedWrite(guard, 2, 5) == 5);  // a raise is never held
}

TEST_CASE("dbno guard: the game's thread and the frame's may run at once") {
    CoopDbno::LifeGuard guard;
    guard.Set(Guard::last_stand);
    std::int32_t life = 1000;
    int lethal_seen = 0;
    std::thread game([&] {
        for (int i = 0; i < 20000 && life > 0; ++i) {
            life -= guard.Step(life, 7);
        }
    });
    for (int i = 0; i < 20000; ++i) {
        lethal_seen += guard.TakeLethal() ? 1 : 0;
    }
    game.join();
    lethal_seen += guard.TakeLethal() ? 1 : 0;
    CHECK(life == CoopDbno::kDownedLife);  // never 0, whatever the timing
    CHECK(lethal_seen == 1);
}

TEST_CASE("dbno: up with a teammate up the guard is the last stand, alone, off or unhooked it is off") {
    Game g;
    CHECK(g.Step().guard == Guard::last_stand);
    CHECK(g.Step().state == State::up);
    CHECK(g.sent.empty());  // up is no news

    g.team.clear();
    CHECK(g.Step().guard == Guard::off);
    g.team = {Mate(1, {1, 0, 0}, State::downed)};
    CHECK(g.Step().guard == Guard::off);  // nobody could revive us
    g.team = {Mate(1)};
    g.team[0].in_game = false;  // on the title screen
    CHECK(g.Step().guard == Guard::off);
    g.team = {Mate(1, {900, 0, 0}, State::up, "spot01")};
    CHECK(g.Step().guard == Guard::last_stand);  // far away still counts: they can come

    g.rules.on = false;
    CHECK(g.Step().guard == Guard::off);
    g.rules.on = true;
    g.connected = false;
    CHECK(g.Step().guard == Guard::off);
    g.connected = true;
    g.guard_live = false;
    CHECK(g.Step().guard == Guard::off);
    // And a lethal flag from nowhere downs nobody then.
    CHECK(g.Step(true).state == State::up);
}

TEST_CASE("dbno: a bottled fairy is the game's own revive: no last stand, its death runs, back up, never downed (row 162)") {
    Game g;
    CHECK(g.Step().guard == Guard::last_stand);
    g.bottled_fairy = true;
    CHECK(g.Step().guard == Guard::off);  // the lethal hit is the game's: its death, its fairy
    // The game's death at no life, then its fairy brings him back within the dying watch.
    g.life = 0;
    g.Run(3 * kS);
    CHECK(g.machine.Now() == State::up);
    g.bottled_fairy = false;  // the fairy left the bottle
    g.life = 32;
    const Output back = g.Step();
    CHECK(back.state == State::up);
    CHECK(back.guard == Guard::last_stand);  // the next lethal hit downs him again
    CHECK(g.events.empty());
    CHECK(g.sent.empty());  // nobody was told he was down
    // A fairy that does not save him (the life stays 0): out, as the game's own death.
    g.bottled_fairy = true;
    g.Step();
    g.life = 0;
    g.Run(CoopDbno::kDyingUs + kS);
    CHECK(g.machine.Now() == State::out);
    CHECK(g.Saw(Event::died));
    // Already down, a fairy changes nothing: still shielded, the teammates' revive.
    Game d;
    d.Step();
    d.Down();
    d.bottled_fairy = true;
    CHECK(d.Step().guard == Guard::shielded);
    CHECK(d.machine.Now() == State::downed);
}

TEST_CASE("dbno: a lethal hit downs, tells everyone, shields, and the bleed-out runs") {
    Game g;
    g.Step();
    const Output o = g.Down();
    CHECK(o.state == State::downed);
    CHECK(o.guard == Guard::shielded);
    CHECK(g.Saw(Event::went_down));
    REQUIRE(g.sent.size() == 1);
    CHECK(g.sent[0] == proto::DbnoStatus{0, State::downed, 450, 0, 0});
    CHECK(o.left_us == 45 * kS);
    CHECK(o.reviver == 0);

    const Output later = g.Run(10 * kS);
    CHECK(later.state == State::downed);
    CHECK(later.left_us == doctest::Approx(35.0 * kS).epsilon(0.01));
    // Told again twice a second, not every frame.
    CHECK(g.sent.size() >= 19);
    CHECK(g.sent.size() <= 22);
    CHECK(g.sent.back().left_tenths == doctest::Approx(350).epsilon(0.02));
    CHECK(g.life == CoopDbno::kDownedLife);
}

TEST_CASE("dbno: the downed life is put back when the game changes it") {
    Game g;
    g.Down();
    g.life = 8;  // a heart rolled into him
    CHECK(g.Step().write_life == CoopDbno::kDownedLife);
    CHECK_FALSE(g.Step().write_life);
}

TEST_CASE("dbno: bled out spectates the one still up, shielded, life kept, and says so") {
    Game g;
    g.rules.bleed_s = 10;
    g.team = {Mate(3, {20, 0, 0})};
    g.Down();
    const Output o = g.Run(10 * kS + kFrame);
    CHECK(o.state == State::spectating);
    CHECK(o.guard == Guard::shielded);
    CHECK(o.watching == 3);
    CHECK(g.Saw(Event::bled_out));
    CHECK_FALSE(g.Saw(Event::wiped));
    CHECK(g.life == CoopDbno::kDownedLife);
    CHECK(g.sent.back() == proto::DbnoStatus{0, State::spectating, 0, 0, 0});
    std::vector<std::uint8_t> bytes;
    CHECK(proto::encode(proto::Message{g.sent.back()}, bytes) == proto::Error::none);
    // A hit while spectating takes nothing; a heart rolled in is put back.
    g.life = 8;
    CHECK(g.Step().write_life == CoopDbno::kDownedLife);
}

TEST_CASE("dbno: the game over when nobody is left: out, the life goes, the game's death, then a continue") {
    Game g;
    g.Down();
    g.team = {Mate(1, {1, 0, 0}, State::spectating)};
    const Output o = g.Run(CoopDbno::kWipeGraceUs + kFrame);
    CHECK(o.state == State::out);
    CHECK(o.guard == Guard::off);
    CHECK(g.Saw(Event::wiped));
    CHECK(g.life == 0);
    CHECK(g.sent.back() == proto::DbnoStatus{0, State::out, 0, 0, 0});

    // Dead on the ground, the game over screen (no Link), then a continue.
    g.Run(2 * kS);
    g.life.reset();
    g.Run(3 * kS);
    CHECK(g.machine.Now() == State::out);
    g.life = 12;
    CHECK(g.Step().state == State::up);
    CHECK(g.Saw(Event::back));
    CHECK(g.sent.back() == proto::DbnoStatus{});
    // The teammate was spectating, so it went out with us (its own Game Over). Until it is back up,
    // nobody else is up: the game's own rules (no last stand alone).
    g.team[0].state = State::out;
    CHECK(g.Step().guard == Guard::off);
    // Its continue: up again, and the last stand is back for both.
    g.team[0].state = State::up;
    CHECK(g.Step().guard == Guard::last_stand);
}

TEST_CASE("dbno: out and the write did not hold: written again, a fairy brings him back") {
    Game g;
    g.Down();
    g.team = {Mate(1, {1, 0, 0}, State::out)};
    g.Run(CoopDbno::kWipeGraceUs);  // nobody up: out, life written 0
    REQUIRE(g.machine.Now() == State::out);
    g.life = 1;  // the game put it back before it looked
    int writes = 0;
    for (int i = 0; i < 20; ++i) {  // one second
        const Output o = g.Step();
        if (o.write_life) {
            CHECK(*o.write_life == 0);
            ++writes;
            g.life = 1;
        }
    }
    CHECK(writes == 2);
    CHECK(g.machine.Now() == State::out);
    g.life = 0;
    g.Step();
    g.life = 20;  // the fairy
    CHECK(g.Step().state == State::up);
}

TEST_CASE("dbno: a held revive fills the ring, stops the bleed-out, and brings him up with the rules' hearts") {
    Game g;
    g.Down();
    g.Run(5 * kS);
    const std::uint64_t left = g.Step().left_us;
    g.sent.clear();

    Output o = g.RunHeld(2500'000);
    CHECK(o.state == State::downed);
    CHECK(o.reviver == 1);
    CHECK(o.progress == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(o.left_us == left);  // stands still
    // The ring is told ten times a second, with the reviver.
    CHECK(g.sent.size() >= 24);
    CHECK(g.sent.size() <= 26);
    CHECK(g.sent.front().reviver == 1);
    CHECK(g.sent.back().progress == doctest::Approx(127).epsilon(0.1));

    o = g.RunHeld(2500'000 + kFrame);
    CHECK(o.state == State::up);
    CHECK(g.Saw(Event::revived));
    CHECK(g.life == 12);
    CHECK(g.sent.back() == proto::DbnoStatus{});
    // Shielded for a moment, then the last stand again.
    CHECK(o.guard == Guard::shielded);
    CHECK(g.Run(kS).guard == Guard::shielded);
    CHECK(g.Run(kS + kFrame).guard == Guard::last_stand);
}

TEST_CASE("dbno: the revive names who did it, and a Link with few hearts gets no more than he has") {
    Game g;
    g.team = {Mate(1, {50, 0, 0}), Mate(3)};
    g.max_life = 8;
    g.rules.revive_life = 40;
    g.Down();
    Output o;
    for (int i = 0; i < 120 && g.machine.Now() == State::downed; ++i) {
        g.machine.Hold(3, true, g.now);
        o = g.Step();
    }
    CHECK(g.machine.Now() == State::up);
    CHECK(o.by == 3);
    CHECK(g.life == 8);
}

TEST_CASE("dbno: letting go empties the ring and the bleed-out goes on") {
    Game g;
    g.Down();
    Output o = g.RunHeld(4 * kS);
    CHECK(o.progress > 0.6f);
    const std::uint64_t left = o.left_us;
    g.machine.Hold(1, false, g.now);
    o = g.Step();
    CHECK(o.reviver == 0);
    CHECK(o.progress == 0.0f);
    CHECK(g.sent.back().reviver == 0);
    CHECK(g.sent.back().progress == 0);
    o = g.Run(kS);
    CHECK(o.left_us < left);
    // Starting over starts from nothing.
    o = g.RunHeld(kS);
    CHECK(o.progress == doctest::Approx(1.0f / 5.0f).epsilon(0.1));
}

TEST_CASE("dbno: a hold whose messages stop counts for a moment only") {
    Game g;
    g.Down();
    g.RunHeld(kS);
    CHECK(g.Run(CoopDbno::kHoldLiveUs - 2 * kFrame).reviver == 1);
    CHECK(g.Run(4 * kFrame).reviver == 0);
}

TEST_CASE("dbno: only a teammate who is up, here and beside him revives") {
    const auto held = [](Game& g, std::uint8_t from = 1) { return g.RunHeld(kS, from).reviver; };
    {
        Game g;
        g.team = {Mate(1, {4.0f, 0, 0})};  // too far (reach 2.5 m, a little slack)
        g.Down();
        CHECK(held(g) == 0);
        g.team = {Mate(1, {3.0f, 0, 0})};  // within the slack
        CHECK(held(g) == 1);
    }
    {
        Game g;
        g.team = {Mate(1, {1, 3.5f, 0})};  // on the ledge above
        g.Down();
        CHECK(held(g) == 0);
    }
    {
        Game g;
        g.Down();  // downed beside them (row 334), then they are in another scene
        g.team = {Mate(1, {1, 0, 0}, State::up, "spot01")};
        CHECK(held(g) == 0);
        CHECK(g.machine.Now() == State::downed);
    }
    {
        Game g;
        g.team = {Mate(1), Mate(2, {1, 0, 0}, State::downed)};
        g.Down();
        CHECK(held(g, 2) == 0);  // the downed cannot revive
        CHECK(held(g, 9) == 0);  // nor someone who is not in the game
        CHECK(held(g, 0) == 0);
        CHECK(held(g, 200) == 0);
    }
    {
        Game g;
        g.team = {Mate(1)};
        g.Down();
        g.team[0].position.reset();  // no puppet to measure from
        CHECK(held(g) == 0);
        CHECK(g.machine.Now() == State::downed);
        g.team = {Mate(1, {std::nanf(""), 0, 0})};
        CHECK(held(g) == 0);
    }
}

TEST_CASE("dbno: a teammate beside him in the other era's scene of his place revives (row 221)") {
    Game g;
    g.scene = "Graveyard_Child";
    g.team = {Mate(1, {1, 0, 0}, State::up, "Graveyard_Adult")};
    g.Down();
    CHECK(g.RunHeld(kS, 1).reviver == 1);
    Game h;
    h.scene = "Graveyard_Child";
    h.team = {Mate(1, {1, 0, 0}, State::up, "Graveyard_Adult")};
    h.Down();
    h.team[0].scene = "Kakariko_Adult";  // then in another place
    CHECK(h.RunHeld(kS, 1).reviver == 0);
    CHECK(h.machine.Now() == State::downed);
}

TEST_CASE("dbno: two revivers: whoever began goes on, then the lower number") {
    Game g;
    g.team = {Mate(1), Mate(3)};
    g.Down();
    g.machine.Hold(3, true, g.now);
    CHECK(g.Step().reviver == 3);
    g.machine.Hold(1, true, g.now);
    g.machine.Hold(3, true, g.now);
    CHECK(g.Step().reviver == 3);
    g.machine.Hold(3, false, g.now);
    const Output o = g.Step();
    CHECK(o.reviver == 1);
    CHECK(o.progress > 0.0f);  // the ring is the downed player's: it goes on
}

TEST_CASE("dbno: nobody left standing for three seconds is out, a moment of it is not") {
    Game g;
    g.Down();
    g.team[0].state = State::downed;
    CHECK(g.Run(2 * kS).state == State::downed);
    g.team[0].state = State::up;
    g.Step();
    g.team[0].state = State::downed;
    CHECK(g.Run(2 * kS).state == State::downed);  // counted from the start again
    CHECK(g.Run(kS + 2 * kFrame).state == State::out);
    CHECK(g.Saw(Event::wiped));
    CHECK(g.life == 0);
}

TEST_CASE("dbno: the connection drops while down: still down, nothing sent, no wipe, the bleed-out runs on") {
    Game g;
    g.Down();
    g.sent.clear();
    g.connected = false;
    CHECK(g.Run(2 * kS).state == State::downed);
    CHECK(g.sent.empty());
    // Back in time: still down, and the new connection is told.
    g.connected = true;
    CHECK(g.Step().state == State::downed);
    REQUIRE(g.sent.size() == 1);
    CHECK(g.sent[0].state == State::downed);
    // A long blip is no wipe; the bleed-out still ends it.
    g.connected = false;
    CHECK(g.Run(10 * kS).state == State::downed);
    CHECK_FALSE(g.Saw(Event::wiped));
    // Offline nobody is judged: spectating, and no respawn until connected.
    CHECK(g.Run(g.rules.BleedUs()).state == State::spectating);
    CHECK(g.Saw(Event::bled_out));
    CHECK(g.Run(10 * kS).state == State::spectating);
    CHECK_FALSE(g.Saw(Event::wiped));
    CHECK_FALSE(g.Saw(Event::respawned));
    CHECK(g.life == CoopDbno::kDownedLife);
}

TEST_CASE("dbno: the host turns it off: the downed get up with the rules' hearts") {
    Game g;
    g.Down();
    g.rules.on = false;
    const Output o = g.Step();
    CHECK(o.state == State::up);
    CHECK(o.guard == Guard::off);
    CHECK(g.Saw(Event::got_up));
    CHECK(g.life == 12);
    CHECK(g.sent.back() == proto::DbnoStatus{});
}

TEST_CASE("dbno: the game takes the life anyway (a script, a pit): out, with no write of ours") {
    Game g;
    g.Down();
    g.life = 0;
    const Output o = g.Step();
    CHECK(o.state == State::out);
    CHECK_FALSE(o.write_life);
    CHECK(g.Saw(Event::died));
    g.life = 12;
    CHECK(g.Step().state == State::up);
}

TEST_CASE("dbno: a load takes no time off, twenty seconds with no Link is the title screen") {
    Game g;
    g.Down();
    const std::uint64_t left = g.Step().left_us;
    g.life.reset();
    Output o = g.Run(15 * kS);
    CHECK(o.state == State::downed);
    CHECK(o.left_us == left);
    g.life = CoopDbno::kDownedLife;
    o = g.Run(kS);
    CHECK(o.state == State::downed);
    CHECK(o.left_us == doctest::Approx(static_cast<double>(left - kS)).epsilon(0.01));

    g.life.reset();
    g.events.clear();
    o = g.Run(20 * kS + kFrame);
    CHECK(o.state == State::up);
    CHECK(g.events.empty());
    CHECK_FALSE(o.write_life);
    CHECK(g.sent.back() == proto::DbnoStatus{});
}

TEST_CASE("dbno: a hitch counts as a quarter second at most") {
    Game g;
    g.rules.bleed_s = 10;
    g.Down();
    g.now += 30 * kS;  // the window was dragged
    const Output o = g.Step();
    CHECK(o.state == State::downed);
    CHECK(o.left_us == 10 * kS - CoopDbno::kMaxStepUs);
}

TEST_CASE("dbno: someone joins: a downed player says so again, one who is up says nothing") {
    Game g;
    g.Step();
    g.machine.TellAgain();
    g.Step();
    CHECK(g.sent.empty());
    g.Down();
    g.sent.clear();
    g.machine.TellAgain();
    g.Step();
    REQUIRE(g.sent.size() == 1);
    CHECK(g.sent[0].state == State::downed);
    g.Step();
    CHECK(g.sent.size() == 1);
}

TEST_CASE("dbno: every status the machine sends is one the codec takes") {
    Game g;
    g.rules.bleed_s = proto::kDbnoMaxBleedS;
    g.Down();
    g.RunHeld(2 * kS);
    g.Run(kS);
    g.RunHeld(6 * kS);
    REQUIRE(g.sent.size() > 20);
    for (const auto& s : g.sent) {
        std::vector<std::uint8_t> bytes;
        CHECK(proto::encode(proto::Message{s}, bytes) == proto::Error::none);
    }
}

TEST_CASE("dbno roster: the others' states, their time running down, and leaving") {
    CoopDbno::Roster roster;
    CHECK(roster.StateOf(2) == State::up);
    CHECK_FALSE(roster.Downed(2));
    roster.Set(proto::DbnoStatus{2, State::downed, 300, 0, 0}, 5 * kS);
    roster.Set(proto::DbnoStatus{3, State::out, 0, 0, 0}, 5 * kS);
    roster.Set(proto::DbnoStatus{0, State::downed, 300, 0, 0}, 5 * kS);    // no player: nothing
    roster.Set(proto::DbnoStatus{200, State::downed, 300, 0, 0}, 5 * kS);
    CHECK(roster.StateOf(2) == State::downed);
    CHECK(roster.StateOf(3) == State::out);
    CHECK(roster.StateOf(0) == State::up);
    CHECK(roster.StateOf(200) == State::up);
    CHECK_FALSE(roster.Downed(3));
    const auto e = roster.Downed(2);
    REQUIRE(e);
    CHECK(CoopDbno::Roster::LeftUs(*e, 5 * kS) == 30 * kS);
    CHECK(CoopDbno::Roster::LeftUs(*e, 15 * kS) == 20 * kS);
    CHECK(CoopDbno::Roster::LeftUs(*e, 90 * kS) == 0);
    CHECK(CoopDbno::Roster::LeftUs(*e, 1 * kS) == 30 * kS);
    // While someone revives them it stands still.
    roster.Set(proto::DbnoStatus{2, State::downed, 300, 1, 40}, 5 * kS);
    CHECK(CoopDbno::Roster::LeftUs(*roster.Downed(2), 15 * kS) == 30 * kS);

    roster.Left(2);
    CHECK(roster.StateOf(2) == State::up);
    roster.Clear();
    CHECK(roster.StateOf(3) == State::up);
}

TEST_CASE("dbno reviver: beside a downed player, holding sends five times a second and letting go says so") {
    CoopDbno::Reviver r;
    const std::optional<std::string> scene = "spot00";
    const std::vector<CoopDbno::Fallen> fallen{{2, "spot00", {1, 0, 0}}};
    std::uint64_t now = kS;
    // In reach, not holding: the prompt, and A is ours.
    auto o = r.Frame(now, true, false, {0, 0, 0}, scene, fallen);
    CHECK(o.target == 2);
    CHECK_FALSE(o.holding);
    CHECK_FALSE(o.send);
    CHECK(r.TakesButton());

    int sends = 0;
    for (int i = 0; i < 20; ++i) {  // one second
        now += kFrame;
        o = r.Frame(now, true, true, {0, 0, 0}, scene, fallen);
        CHECK(o.holding);
        if (o.send) {
            CHECK(*o.send == proto::DbnoRevive{0, 2, true});
            ++sends;
        }
    }
    CHECK(sends == 5);

    now += kFrame;
    o = r.Frame(now, true, false, {0, 0, 0}, scene, fallen);
    REQUIRE(o.send);
    CHECK(*o.send == proto::DbnoRevive{0, 2, false});
    CHECK_FALSE(o.holding);
    CHECK(o.target == 2);
    now += kFrame;
    CHECK_FALSE(r.Frame(now, true, false, {0, 0, 0}, scene, fallen).send);
}

TEST_CASE("dbno reviver: nobody in reach, another scene, or not able: no target and A is the game's") {
    CoopDbno::Reviver r;
    const std::optional<std::string> scene = "spot00";
    CHECK(r.Frame(kS, true, true, {0, 0, 0}, scene, {}).target == 0);
    CHECK_FALSE(r.TakesButton());
    const std::vector<CoopDbno::Fallen> far{{2, "spot00", {2.6f, 0, 0}}};
    CHECK(r.Frame(kS, true, true, {0, 0, 0}, scene, far).target == 0);
    const std::vector<CoopDbno::Fallen> above{{2, "spot00", {1, 2.1f, 0}}};
    CHECK(r.Frame(kS, true, true, {0, 0, 0}, scene, above).target == 0);
    const std::vector<CoopDbno::Fallen> elsewhere{{2, "spot01", {1, 0, 0}}};
    CHECK(r.Frame(kS, true, true, {0, 0, 0}, scene, elsewhere).target == 0);
    const std::vector<CoopDbno::Fallen> near{{2, "spot00", {1, 0, 0}}};
    CHECK(r.Frame(kS, false, true, {0, 0, 0}, scene, near).target == 0);  // we are down, or in a cutscene
    CHECK(r.Frame(kS, true, true, {0, 0, 0}, std::nullopt, near).target == 0);
    CHECK_FALSE(r.Frame(kS, true, true, {0, 0, 0}, scene, {}).send);
}

TEST_CASE("dbno reviver: a downed player in the other era's scene of our place is a target (row 221)") {
    CoopDbno::Reviver r;
    const std::optional<std::string> scene = "Graveyard_Adult";
    const std::vector<CoopDbno::Fallen> child{{2, "Graveyard_Child", {1, 0, 0}}};
    CHECK(r.Frame(kS, true, true, {0, 0, 0}, scene, child).target == 2);
    CoopDbno::Reviver q;
    const std::vector<CoopDbno::Fallen> elsewhere{{2, "Kakariko_Child", {1, 0, 0}}};
    CHECK(q.Frame(kS, true, true, {0, 0, 0}, scene, elsewhere).target == 0);
}

TEST_CASE("dbno reviver: the nearest is the target, it stays ours a little past the reach, a change lets go first") {
    CoopDbno::Reviver r;
    const std::optional<std::string> scene = "spot00";
    std::vector<CoopDbno::Fallen> fallen{{2, "spot00", {2, 0, 0}}, {3, "spot00", {1, 0, 0}}};
    std::uint64_t now = kS;
    auto o = r.Frame(now, true, true, {0, 0, 0}, scene, fallen);
    CHECK(o.target == 3);
    REQUIRE(o.send);
    CHECK(o.send->target == 3);

    // 3 crawls to 2.8 m: past the reach, within the keep.
    fallen[1].position = {2.8f, 0, 0};
    now += kFrame;
    o = r.Frame(now, true, true, {0, 0, 0}, scene, fallen);
    CHECK(o.target == 3);
    CHECK(o.holding);

    // And on to 3.2 m: let go of 3, then hold 2.
    fallen[1].position = {3.2f, 0, 0};
    now += kFrame;
    o = r.Frame(now, true, true, {0, 0, 0}, scene, fallen);
    REQUIRE(o.send);
    CHECK(*o.send == proto::DbnoRevive{0, 3, false});
    CHECK(o.target == 2);
    CHECK_FALSE(o.holding);
    now += kFrame;
    o = r.Frame(now, true, true, {0, 0, 0}, scene, fallen);
    REQUIRE(o.send);
    CHECK(*o.send == proto::DbnoRevive{0, 2, true});

    // We go down ourselves while holding: let go.
    now += kFrame;
    o = r.Frame(now, false, true, {0, 0, 0}, scene, fallen);
    REQUIRE(o.send);
    CHECK(*o.send == proto::DbnoRevive{0, 2, false});
    CHECK(o.target == 0);
    CHECK_FALSE(r.TakesButton());

    // A tie goes to the lower number.
    r.Reset();
    const std::vector<CoopDbno::Fallen> tie{{5, "spot00", {1, 0, 0}}, {4, "spot00", {-1, 0, 0}}};
    CHECK(r.Frame(now, true, false, {0, 0, 0}, scene, tie).target == 4);
}

TEST_CASE("dbno pad: a downed player keeps pause, the camera and a slow stick, a reviver loses A only") {
    using CoopDbno::Pad;
    namespace pad = CoopDbno::pad;
    const std::uint64_t everything = 0xFFFFFFull;
    const Pad in{everything, {32767, -32767, 1000, -2000}};
    const Pad down = CoopDbno::DownedPad(in);
    CHECK(down.buttons == (pad::plus | pad::minus | pad::stick_l_dirs | pad::stick_r_dirs));
    CHECK(down.stick[0] == 32767 * 30 / 100);
    CHECK(down.stick[1] == -32767 * 30 / 100);
    CHECK(down.stick[2] == 1000);
    CHECK(down.stick[3] == -2000);
    CHECK((down.buttons & pad::a) == 0);

    const Pad rev = CoopDbno::ReviverPad(in);
    CHECK(rev.buttons == (everything & ~pad::a));
    CHECK(rev.stick == in.stick);
    CHECK(CoopDbno::DownedPad(Pad{}) == Pad{});

    // Issue 337: in the game's own crawl the stick is whole (its own crawl speed); buttons as before.
    const Pad crawl = CoopDbno::DownedPad(in, true);
    CHECK(crawl.buttons == down.buttons);
    CHECK(crawl.stick == in.stick);
}

TEST_CASE("dbno rules: the server's message, and the host's ask kept within the limits") {
    const Rules r = CoopDbno::RulesOf(proto::DbnoMode{2, true, 60, 25, 8});
    CHECK(r.on);
    CHECK(r.set_by == 2);
    CHECK(r.BleedUs() == 60 * kS);
    CHECK(r.ReviveUs() == 2'500'000);
    CHECK(r.revive_life == 8);
    CHECK_FALSE(Rules{}.on);  // off until the server says

    CHECK(CoopDbno::Ask(true, 60, 25, 8) == proto::DbnoMode{0, true, 60, 25, 8});
    CHECK(CoopDbno::Ask(false, 0, 0, 0) ==
          proto::DbnoMode{0, false, proto::kDbnoMinBleedS, proto::kDbnoMinReviveTenths,
                          proto::kDbnoMinReviveLife});
    CHECK(CoopDbno::Ask(true, 100000, 100000, 100000) ==
          proto::DbnoMode{0, true, proto::kDbnoMaxBleedS, proto::kDbnoMaxReviveTenths,
                          proto::kDbnoMaxReviveLife});
    std::vector<std::uint8_t> bytes;
    CHECK(proto::encode(proto::Message{CoopDbno::Ask(true, -5, -5, -5)}, bytes) == proto::Error::none);
}

TEST_CASE("dbno chat command: /dbno shows, on and off and the three numbers change, the rest is chat or bad") {
    using CoopDbno::Command;
    using CoopDbno::Parse;
    CHECK_FALSE(Parse("hello"));
    CHECK_FALSE(Parse("/dbnox on"));
    CHECK_FALSE(Parse("/pvp on"));
    REQUIRE(Parse("/dbno"));
    CHECK(Parse("  /DBNO  ")->kind == Command::Kind::show);

    auto c = Parse("/dbno on");
    REQUIRE(c);
    CHECK(c->kind == Command::Kind::change);
    CHECK(c->on == true);
    CHECK(Parse("/dbno OFF")->on == false);
    CHECK(Parse("/dbno no")->on == false);

    CHECK(Parse("/dbno time 60")->bleed_s == 60);
    CHECK(Parse("/dbno hold 2.5")->revive_tenths == 25);
    CHECK(Parse("/dbno hold 3")->revive_tenths == 30);
    CHECK(Parse("/dbno hearts 3")->revive_life == 12);
    CHECK(Parse("/dbno hearts 0.5")->revive_life == 2);
    CHECK(Parse("/dbno hearts 1.25")->revive_life == 5);
    CHECK(Parse("/dbno   Hearts    2 ")->revive_life == 8);

    for (const char* bad : {"/dbno maybe", "/dbno time", "/dbno time x", "/dbno time 2.5", "/dbno time -3",
                            "/dbno hold 2.55", "/dbno hearts 1.1", "/dbno hearts 1.", "/dbno hearts .5",
                            "/dbno time 99999", "/dbno speed 3", "/dbno time 3 4"}) {
        CAPTURE(bad);
        REQUIRE(Parse(bad));
        CHECK(Parse(bad)->kind == Command::Kind::bad);
    }

    // A change keeps the rest as the session has it, within the limits.
    Rules now = On();
    now.bleed_s = 90;
    CHECK(CoopDbno::Applied(now, *Parse("/dbno hearts 5")) == proto::DbnoMode{0, true, 90, 50, 20});
    CHECK(CoopDbno::Applied(now, *Parse("/dbno off")) == proto::DbnoMode{0, false, 90, 50, 12});
    CHECK(CoopDbno::Applied(now, *Parse("/dbno time 5")).bleed_s == proto::kDbnoMinBleedS);
}

TEST_CASE("dbno texts: the clock, the rules, our own notices and the others'") {
    CHECK(CoopDbno::Clock(45 * kS) == "0:45");
    CHECK(CoopDbno::Clock(44 * kS + 1) == "0:45");  // rounds up: 0:00 only at the end
    CHECK(CoopDbno::Clock(65 * kS) == "1:05");
    CHECK(CoopDbno::Clock(0) == "0:00");

    CHECK(CoopDbno::RulesLine(Rules{}, 1, "") == "Down but not out is off: at no hearts you die");
    CHECK(CoopDbno::RulesLine(On(), 1, "") ==
          "Down but not out is on: 0:45 to be revived, hold A 5 s, back with 3 hearts");
    Rules r = On();
    r.revive_tenths = 25;
    r.revive_life = 2;
    r.set_by = 2;
    CHECK(CoopDbno::RulesLine(r, 1, "Saria") ==
          "Down but not out is on: 0:45 to be revived, hold A 2.5 s, back with 1/2 heart (Saria changed it)");
    CHECK(CoopDbno::RulesLine(r, 2, "Saria").ends_with("(you changed it)"));

    CHECK(CoopDbno::OwnLine(Event::went_down, "") == "You are down. A teammate can revive you");
    CHECK(CoopDbno::OwnLine(Event::revived, "Mido") == "Mido revived you");
    CHECK(CoopDbno::OwnLine(Event::revived, "") == "A teammate revived you");
    CHECK(CoopDbno::OwnLine(Event::bled_out, ""));
    CHECK(CoopDbno::OwnLine(Event::wiped, ""));
    CHECK(CoopDbno::OwnLine(Event::got_up, ""));
    CHECK_FALSE(CoopDbno::OwnLine(Event::died, ""));
    CHECK_FALSE(CoopDbno::OwnLine(Event::back, ""));

    const proto::DbnoStatus down{2, State::downed, 450, 0, 0};
    const proto::DbnoStatus held{2, State::downed, 450, 3, 200};
    const proto::DbnoStatus up{2, State::up, 0, 0, 0};
    const proto::DbnoStatus out{2, State::out, 0, 0, 0};
    CHECK(CoopDbno::TheirLine(std::nullopt, down, "Saria", "") == "Saria is down: hold A beside them to revive");
    CHECK(CoopDbno::TheirLine(up, down, "", "") == "P2 is down: hold A beside them to revive");
    CHECK_FALSE(CoopDbno::TheirLine(down, held, "Saria", "Mido"));  // still down: no news
    CHECK(CoopDbno::TheirLine(held, up, "Saria", "Mido") == "Mido revived Saria");
    CHECK(CoopDbno::TheirLine(held, up, "Saria", "") == "P3 revived Saria");
    CHECK(CoopDbno::TheirLine(down, up, "Saria", "") == "Saria is up again");
    CHECK(CoopDbno::TheirLine(down, out, "Saria", "") == "Saria is out");
    CHECK_FALSE(CoopDbno::TheirLine(out, up, "Saria", ""));
    CHECK_FALSE(CoopDbno::TheirLine(std::nullopt, up, "Saria", ""));
}

TEST_CASE("dbno: the rules lower the life through the game's own step, never less than it takes") {
    using CoopDbno::Lowered;
    CHECK(Lowered(5, 0, -1) == 0);   // nothing wanted
    CHECK(Lowered(5, 2, -1) == 2);
    CHECK(Lowered(5, 0, 1) == 4);    // a downed Link healed to 5: back to a quarter
    CHECK(Lowered(5, 0, 0) == 5);    // out: all of it, the game's death
    CHECK(Lowered(5, 5, 0) == 5);
    CHECK(Lowered(5, 8, 1) == 8);    // the game takes more already
    CHECK(Lowered(1, 0, 1) == 0);    // already there
    CHECK(Lowered(1, 0, 3) == 0);    // a raise is not this step's (it is written)
}

TEST_CASE("dbno: a lowering the Life step does not take in a quarter second is written instead") {
    CoopDbno::Lowering l;
    const std::uint64_t t0 = 1'000'000;
    CHECK_FALSE(l.Ask(0, t0));
    CHECK(l.Take() == 0);  // the step took it
    CHECK(l.Take() == -1);
    CHECK_FALSE(l.Ask(0, t0 + 100'000));  // asked again: a new wait
    CHECK_FALSE(l.Ask(0, t0 + 200'000));
    CHECK(l.Ask(0, t0 + 100'000 + CoopDbno::kTakeWaitUs) == std::optional<std::int32_t>{0});
    CHECK(l.Take() == -1);  // written: the step has nothing left to do
    CHECK_FALSE(l.Ask(1, t0 + 400'000));
    l.Drop();
    CHECK(l.Take() == -1);
    CHECK_FALSE(l.Ask(1, t0 + 900'000));  // after a drop the wait starts again
}

// --- Spectating (row 113 parts 3 to 5) ---

namespace {

// Down, then bled out: spectating.
void Spectate(Game& g) {
    g.rules.bleed_s = 10;
    g.Down();
    g.Run(10 * kS);
    REQUIRE(g.machine.Now() == State::spectating);
}

Teammate Threatened(Teammate t, CoopDbno::Threat threat) {
    t.threat = threat;
    return t;
}

std::vector<std::uint8_t> Encoded(const proto::Message& m, proto::Error want = proto::Error::none) {
    std::vector<std::uint8_t> bytes;
    CHECK(proto::encode(m, bytes) == want);
    return bytes;
}

}  // namespace

TEST_CASE("dbno spectate: back up beside the one watched once no enemy is near, with the continue's hearts") {
    Game g;
    g.team = {Threatened(Mate(2, {30, 0, 0}), CoopDbno::Threat::enemy)};
    Spectate(g);
    // A fight goes on near them: no respawn however long.
    CHECK(g.Run(20 * kS).state == State::spectating);
    CHECK_FALSE(g.Saw(Event::respawned));
    // Cleared: kQuietUs later, up beside them.
    g.team[0].threat = CoopDbno::Threat::clear;
    CHECK(g.Run(CoopDbno::kQuietUs - kFrame).state == State::spectating);
    g.events.clear();
    // First our Link is moved beside them (still spectating), then up.
    const Output asked = g.Step();
    CHECK(asked.state == State::spectating);
    CHECK(asked.move_beside == 2);
    CHECK_FALSE(g.Saw(Event::respawned));
    const Output o = g.Step();
    CHECK(o.state == State::up);
    CHECK(o.by == 2);
    CHECK(g.beside_asks == 1);
    CHECK(g.entrance_asks == 0);
    CHECK(g.Saw(Event::respawned));
    CHECK(g.life == CoopDbno::kRespawnLife);
    CHECK(o.guard == Guard::shielded);  // the mercy after coming back
    CHECK(g.sent.back() == proto::DbnoStatus{});
    CHECK(CoopDbno::OwnLine(Event::respawned, "Saria") == "No enemy is near: you are back beside Saria");
}

TEST_CASE("dbno spectate: an enemy near anyone up in the watched one's scene holds the respawn") {
    Game g;
    g.team = {Threatened(Mate(2, {30, 0, 0}), CoopDbno::Threat::clear),
              Threatened(Mate(3, {35, 0, 0}), CoopDbno::Threat::enemy),
              Threatened(Mate(4, {0, 0, 0}, State::up, "elsewhere"), CoopDbno::Threat::enemy)};
    Spectate(g);
    CHECK(g.Run(10 * kS).state == State::spectating);
    // Player 4's fight is in another scene: it does not count.
    g.team[1].threat = CoopDbno::Threat::clear;
    CHECK(g.Run(CoopDbno::kQuietUs + kFrame).state == State::up);
}

TEST_CASE("dbno spectate: an unknown threat (none heard) never traps anyone") {
    Game g;
    g.team = {Mate(2, {30, 0, 0})};
    Spectate(g);
    g.Run(CoopDbno::kSpectateMinUs + 2 * kFrame);
    CHECK(g.machine.Now() == State::up);
    CHECK(g.Saw(Event::respawned));
}

TEST_CASE("dbno spectate: respawn hearts are capped at the player's own") {
    Game g;
    g.max_life = 8;
    g.team = {Mate(2)};
    Spectate(g);
    g.Run(CoopDbno::kSpectateMinUs + 2 * kFrame);
    CHECK(g.machine.Now() == State::up);
    CHECK(g.life == 8);
}

TEST_CASE("dbno spectate: the first pick takes the other era's scene of our place as ours (row 221)") {
    Game g;
    g.scene = "Graveyard_Child";
    g.position = {0, 0, 0};
    g.team = {Threatened(Mate(2, {40, 0, 0}, State::up, "Graveyard_Child"), CoopDbno::Threat::enemy),
              Threatened(Mate(4, {10, 0, 0}, State::up, "Graveyard_Adult"), CoopDbno::Threat::enemy),
              Threatened(Mate(5, {3, 0, 0}, State::up, "Kakariko_Child"), CoopDbno::Threat::enemy)};
    Spectate(g);
    CHECK(g.Step().watching == 4);  // nearest up in our place (5 is in another place)
}

TEST_CASE("dbno spectate: the first pick is the nearest in our scene, and A goes round the players still up") {
    Game g;
    g.position = {0, 0, 0};
    g.team = {Threatened(Mate(5, {3, 0, 0}, State::up, "other"), CoopDbno::Threat::enemy),
              Threatened(Mate(2, {40, 0, 0}), CoopDbno::Threat::enemy),
              Threatened(Mate(4, {10, 0, 0}), CoopDbno::Threat::enemy),
              Threatened(Mate(3, {1, 0, 0}, State::downed), CoopDbno::Threat::enemy)};
    Spectate(g);
    CHECK(g.Step().watching == 4);  // nearest up in our scene (3 is down, 5 elsewhere)
    // Row 318: A goes round the nearby ones only (5 is elsewhere: never watched).
    auto next = [&g] {
        g.now += kFrame;
        Input in;
        in.now_us = g.now;
        in.rules = g.rules;
        in.connected = true;
        in.guard_live = true;
        in.life = g.life;
        in.max_life = g.max_life;
        in.scene = g.scene;
        in.position = g.position;
        in.team = g.team;
        in.next = true;
        return g.machine.Frame(in).watching;
    };
    CHECK(next() == 2);  // round again, by number: 2, 4
    CHECK(next() == 4);
    CHECK(next() == 2);
    CHECK(next() == 4);
    CHECK(g.Step().watching == 4);  // no press: stays
    // The one watched goes down: the next pick.
    g.team[2].state = State::downed;
    CHECK(g.Step().watching == 2);
}

TEST_CASE("dbno spectate: nobody left up: out in every game (each runs its own Game Over)") {
    Game g;
    g.team = {Threatened(Mate(2, {30, 0, 0}), CoopDbno::Threat::enemy), Mate(3, {31, 0, 0}, State::downed)};
    Spectate(g);
    g.team[0].state = State::out;  // the last one standing died
    g.events.clear();
    CHECK(g.Run(CoopDbno::kWipeGraceUs - kFrame).state == State::spectating);
    const Output o = g.Step();
    CHECK(o.state == State::out);
    CHECK(g.Saw(Event::wiped));
    CHECK(g.life == 0);  // through the game's own Life step: its death and Game Over
    CHECK(g.sent.back().state == State::out);
}

TEST_CASE("dbno: the last one standing dies the game's death and says out, so the others wipe") {
    Game g;
    g.team = {Mate(2, {5, 0, 0}, State::spectating)};
    CHECK(g.Step().guard == Guard::off);  // nobody else up: the game's own rules
    g.life = 0;
    CHECK(g.Run(kS).state == State::up);  // its death runs; a fairy could still save him
    CHECK(g.sent.empty());
    g.life.reset();  // the Game Over screen: no Link
    const Output o = g.Step();
    CHECK(o.state == State::out);
    CHECK(g.Saw(Event::died));
    CHECK(g.sent.back().state == State::out);
    // Try Again (the host's save, patch 0024): alive, up again.
    g.life.reset();
    g.Run(kS);
    g.life = 12;
    CHECK(g.Step().state == State::up);
    CHECK(g.Saw(Event::back));
}

TEST_CASE("dbno: solo, or the mode off, a death is the game's alone and nothing is sent") {
    Game g;
    g.connected = false;
    g.team.clear();
    g.life = 0;
    CHECK(g.Step().state == State::up);
    g.connected = true;
    g.rules.on = false;
    CHECK(g.Step().state == State::up);
    CHECK(g.sent.empty());
    CHECK(g.events.empty());
}

TEST_CASE("dbno spectate: the host turns the mode off: up again where he lies") {
    Game g;
    g.team = {Threatened(Mate(2), CoopDbno::Threat::enemy)};
    Spectate(g);
    g.rules.on = false;
    CHECK(g.Step().state == State::up);
    CHECK(g.Saw(Event::got_up));
    CHECK(g.life == g.rules.revive_life);
}

TEST_CASE("dbno spectate: no revive holds count while spectating") {
    Game g;
    g.team = {Threatened(Mate(1, {1, 0, 0}), CoopDbno::Threat::enemy)};
    Spectate(g);
    CHECK(g.RunHeld(10 * kS).state == State::spectating);
    CHECK_FALSE(g.Saw(Event::revived));
}

TEST_CASE("dbno spectate: the pad keeps the camera stick and the pause buttons only") {
    using CoopDbno::Pad;
    namespace pad = CoopDbno::pad;
    Pad in;
    in.buttons = pad::a | pad::plus | pad::minus | pad::stick_l_dirs | pad::stick_r_dirs | (1ull << 4);
    in.stick = {30000, -20000, 1000, -2000};
    const Pad out = CoopDbno::SpectatorPad(in);
    CHECK(out.buttons == (pad::plus | pad::minus | pad::stick_r_dirs));
    CHECK(out.stick == std::array<std::int32_t, 4>{0, 0, 1000, -2000});
}

TEST_CASE("dbno spectate: the others' lines") {
    const proto::DbnoStatus down{2, State::downed, 100, 0, 0};
    const proto::DbnoStatus watching{2, State::spectating, 0, 0, 0};
    const proto::DbnoStatus up{2, State::up, 0, 0, 0};
    CHECK(CoopDbno::TheirLine(down, watching, "Saria", "") == "Saria is watching: back once no enemy is near");
    CHECK(CoopDbno::TheirLine(watching, up, "Saria", "") == "Saria is up again");
    CHECK(CoopDbno::OwnLine(Event::bled_out, "Mido")->find("you watch Mido") != std::string::npos);
}

TEST_CASE("dbno threat: enemies near, level and height") {
    const Vec3 self{0, 0, 0};
    CHECK_FALSE(CoopDbno::AnyNear(self, std::vector<Vec3>{}));
    CHECK(CoopDbno::AnyNear(self, std::vector<Vec3>{{10, 1, 10}}));
    CHECK_FALSE(CoopDbno::AnyNear(self, std::vector<Vec3>{{20, 0, 0}}));  // fled out of range
    CHECK_FALSE(CoopDbno::AnyNear(self, std::vector<Vec3>{{1, 20, 0}}));  // another floor
    CHECK_FALSE(CoopDbno::AnyNear(self, std::vector<Vec3>{{NAN, 0, 0}}));
}

TEST_CASE("dbno threat: told on a change and every few seconds, only while able") {
    CoopDbno::ThreatTeller t;
    const std::uint64_t t0 = 5 * kS;
    CHECK_FALSE(t.Frame(t0, false, true));
    auto a = t.Frame(t0, true, true);
    REQUIRE(a);
    CHECK(a->enemy_near);
    CHECK_FALSE(t.Frame(t0 + kS, true, true));
    a = t.Frame(t0 + kS + kFrame, true, false);  // a change: at once
    REQUIRE(a);
    CHECK_FALSE(a->enemy_near);
    CHECK_FALSE(t.Frame(t0 + 2 * kS, true, false));
    CHECK(t.Frame(t0 + kS + kFrame + CoopDbno::kThreatSendUs, true, false));  // again
    CHECK_FALSE(t.Frame(t0 + 10 * kS, false, false));
    CHECK(t.Frame(t0 + 10 * kS + kFrame, true, false));  // able again: at once
}

TEST_CASE("dbno threat: DBNO_THREAT and the spectating state through the codec") {
    for (const bool value : {false, true}) {
        const auto bytes = Encoded(proto::Message{proto::DbnoThreat{3, value}});
        proto::Message back;
        REQUIRE(proto::decode(bytes, back) == proto::Error::none);
        CHECK(std::get<proto::DbnoThreat>(back) == proto::DbnoThreat{3, value});
        CHECK(proto::type_of(back) == proto::MessageType::dbno_threat);
        CHECK(proto::channel_of(back) == proto::kChannelSession);
    }
    auto bad = Encoded(proto::Message{proto::DbnoThreat{3, true}});
    bad.back() = 2;  // neither 0 nor 1
    proto::Message back;
    CHECK(proto::decode(bad, back) == proto::Error::bad_value);
    // Spectating carries no time, reviver or progress.
    Encoded(proto::Message{proto::DbnoStatus{2, State::spectating, 10, 0, 0}}, proto::Error::bad_value);
    auto st = Encoded(proto::Message{proto::DbnoStatus{2, State::spectating, 0, 0, 0}});
    REQUIRE(st.size() >= 5);
    st[st.size() - 5] = 4;  // a state past spectating
    CHECK(proto::decode(st, back) == proto::Error::bad_value);
}

TEST_CASE("dbno spectate: the move beside them refused: the entrance, then up there") {
    Game g;
    g.team = {Mate(2, {30, 0, 0})};
    g.beside_result = CoopDbno::Moved::failed;
    Spectate(g);
    g.Run(CoopDbno::kSpectateMinUs + 3 * kFrame);
    CHECK(g.beside_asks == 1);
    CHECK(g.entrance_asks == 1);
    CHECK(g.machine.Now() == State::up);
    CHECK(g.Saw(Event::respawned));
    CHECK(g.life == CoopDbno::kRespawnLife);
}

TEST_CASE("dbno spectate: no move at all: up where he lay only with no enemy near him, else wait and try again") {
    Game g;
    g.team = {Mate(2, {30, 0, 0})};
    g.beside_result = CoopDbno::Moved::failed;
    g.entrance_result = CoopDbno::Moved::failed;
    g.enemy_near_us = true;
    Spectate(g);
    g.Run(CoopDbno::kSpectateMinUs + 3 * kFrame);
    CHECK(g.machine.Now() == State::spectating);
    CHECK(g.life == CoopDbno::kDownedLife);
    // Quiet again (kQuietUs): asked again.
    g.Run(CoopDbno::kQuietUs + kFrame);
    CHECK(g.beside_asks == 2);
    g.enemy_near_us = false;
    g.Run(CoopDbno::kQuietUs + 3 * kFrame);
    CHECK(g.machine.Now() == State::up);
    CHECK(g.Saw(Event::respawned));
}

TEST_CASE("dbno spectate: a move that never answers counts as failed after kRespawnWaitUs") {
    Game g;
    g.team = {Mate(2, {30, 0, 0})};
    g.beside_result = CoopDbno::Moved::none;  // no answer ever
    Spectate(g);
    g.Run(CoopDbno::kSpectateMinUs + kFrame);
    CHECK(g.beside_asks == 1);
    CHECK(g.Run(CoopDbno::kRespawnWaitUs - 2 * kFrame).state == State::spectating);
    CHECK(g.entrance_asks == 0);
    g.Run(2 * kFrame);
    CHECK(g.entrance_asks == 1);
    g.Step();
    CHECK(g.machine.Now() == State::up);
}

TEST_CASE("dbno: the last one up loading (a door, a teleport) is still up for the wipe while connected") {
    Game g;
    g.team = {Mate(2, {30, 0, 0}), Threatened(Mate(3, {31, 0, 0}), CoopDbno::Threat::enemy)};
    g.rules.bleed_s = 120;  // the wipe, not the bleed-out
    g.Down();
    g.team[0].state = State::downed;
    g.team[1].in_game = false;  // 3 takes a door
    g.team[1].loading = true;
    CHECK(g.Run(40 * kS).state == State::downed);  // a slow first load: as long as it takes
    CHECK_FALSE(g.Saw(Event::wiped));
    // They leave the session (a crash, a quit): nobody up, the wipe.
    g.team.pop_back();
    CHECK(g.Run(CoopDbno::kWipeGraceUs + kFrame).state == State::out);
    CHECK(g.Saw(Event::wiped));
}

TEST_CASE("dbno: one up who never had a Link (still on the title) is nobody up") {
    Game g;
    Teammate title = Mate(2);
    title.in_game = false;
    g.team = {Mate(3)};
    g.Down();
    g.team = {title};
    CHECK(g.Run(CoopDbno::kWipeGraceUs + kFrame).state == State::out);
    CHECK(g.Saw(Event::wiped));
}

TEST_CASE("dbno: the last one standing saved by a fairy is never out") {
    Game g;
    g.team = {Mate(2, {5, 0, 0}, State::spectating)};
    g.life = 0;
    CHECK(g.Run(2 * kS).state == State::up);  // the fairy's revive reads 0 for a while
    g.life = 20;
    CHECK(g.Run(CoopDbno::kDyingUs).state == State::up);
    CHECK(g.sent.empty());
    CHECK_FALSE(g.Saw(Event::died));
    // At no life past kDyingUs with a Link (no Game Over screen seen): out after all.
    g.life = 0;
    g.Run(CoopDbno::kDyingUs + kFrame);
    CHECK(g.machine.Now() == State::out);
}

// --- Rows 318 + 334: downed and spectating only with a teammate nearby ---

TEST_CASE("dbno row 334: a lethal hit with nobody nearby: no downed wait, the entrance at once, then up") {
    Game g;
    g.team = {Mate(1, {1, 0, 0}, State::up, "spot01")};  // up, but in another scene
    CHECK(g.Step().guard == Guard::last_stand);          // still held back: the respawn, not a Game Over
    const Output hit = g.Down();
    CHECK(hit.state == State::spectating);
    CHECK(hit.guard == Guard::shielded);
    CHECK(hit.move_entrance);
    CHECK(g.Saw(Event::alone));
    CHECK_FALSE(g.Saw(Event::went_down));
    const Output o = g.Step();
    CHECK(o.state == State::up);
    CHECK(o.by == 0);
    CHECK(g.Saw(Event::respawned));
    CHECK_FALSE(g.Saw(Event::bled_out));
    CHECK(g.entrance_asks == 1);
    CHECK(g.beside_asks == 0);
    CHECK(g.life == CoopDbno::kRespawnLife);
    CHECK(o.guard == Guard::shielded);  // the mercy after coming back
    REQUIRE(g.sent.size() == 2);
    CHECK(g.sent[0].state == State::spectating);
    CHECK(g.sent[1] == proto::DbnoStatus{});
    CHECK(CoopDbno::OwnLine(Event::respawned, "") == "No teammate is near: you are back at the entrance");
    CHECK_FALSE(CoopDbno::OwnLine(Event::alone, ""));
}

TEST_CASE("dbno row 334: nearby is the game showing them (here): not here, no position or a bad one: alone") {
    for (int i = 0; i < 3; ++i) {
        Game g;
        g.team = {Mate(1)};
        if (i == 0) {
            g.auto_here = false;  // our scene, but another dungeon room (CheckMove not ok)
            g.team[0].here = false;
        } else if (i == 1) {
            g.team[0].position.reset();
        } else {
            g.team[0].position = Vec3{std::nanf(""), 0, 0};
        }
        CHECK(g.Down().state == State::spectating);
        CHECK(g.Saw(Event::alone));
    }
}

TEST_CASE("dbno row 334: a teammate taking a door at that frame still counts (kNearbyGraceUs, the load)") {
    {
        Game g;
        g.team = {Mate(1)};
        g.Step();
        g.team[0].in_game = false;  // a door: their scene loads
        g.team[0].loading = true;
        g.Run(5 * kS);  // however long the load takes
        CHECK(g.Down().state == State::downed);
        CHECK(g.Saw(Event::went_down));
        CHECK_FALSE(g.Saw(Event::alone));
    }
    {
        Game g;
        g.team = {Mate(1)};
        g.Step();
        g.team[0].scene = "spot01";  // gone through, now elsewhere
        g.Run(CoopDbno::kNearbyGraceUs - 2 * kFrame);
        CHECK(g.Down().state == State::downed);  // a moment ago: still nearby
    }
    {
        Game g;
        g.team = {Mate(1)};
        g.Step();
        g.team[0].scene = "spot01";
        g.Run(CoopDbno::kNearbyGraceUs);
        CHECK(g.Down().state == State::spectating);  // past the grace: alone
        CHECK(g.Saw(Event::alone));
    }
    {
        Game g;
        g.team = {Mate(1, {1, 0, 0}, State::up, "spot01")};
        g.Step();
        g.team[0].in_game = false;  // a load that did not start beside us
        g.team[0].loading = true;
        CHECK(g.Down().state == State::spectating);
    }
}

TEST_CASE("dbno row 334: three or more: any one nearby who is up is enough") {
    {
        Game g;
        g.team = {Mate(2, {1, 0, 0}, State::up, "spot01"), Mate(3, {30, 0, 0})};
        CHECK(g.Down().state == State::downed);
    }
    {
        Game g;
        g.team = {Mate(2, {1, 0, 0}, State::up, "spot01"), Mate(3, {1, 0, 0}, State::downed)};
        CHECK(g.Down().state == State::spectating);  // the one nearby is down: cannot revive
        CHECK(g.Saw(Event::alone));
    }
    {
        Game g;
        g.team = {Mate(2, {1, 0, 0}, State::up, "spot01"), Mate(3, {1, 0, 0}, State::spectating)};
        CHECK(g.Down().state == State::spectating);
        CHECK(g.Saw(Event::alone));
    }
}

TEST_CASE("dbno row 334: everyone else down: the downed path and its wipe, unchanged") {
    Game g;
    g.team = {Mate(2, {1, 0, 0}, State::downed)};
    CHECK(g.Down().state == State::downed);
    CHECK_FALSE(g.Saw(Event::alone));
    CHECK(g.Run(CoopDbno::kWipeGraceUs + kFrame).state == State::out);
    CHECK(g.Saw(Event::wiped));
    CHECK(g.entrance_asks == 0);
}

TEST_CASE("dbno row 334: solo or the mode off: nothing new") {
    Game g;
    g.connected = false;  // solo
    g.team.clear();
    CHECK(g.Step().guard == Guard::off);
    CHECK(g.Step(true).state == State::up);
    g.connected = true;
    g.team = {Mate(1, {1, 0, 0}, State::up, "spot01")};
    g.rules.on = false;
    CHECK(g.Step(true).state == State::up);
    CHECK(g.events.empty());
    CHECK(g.entrance_asks == 0);
}

TEST_CASE("dbno row 318: bled out with nobody nearby: no spectating, the entrance at once, then up") {
    Game g;
    g.rules.bleed_s = 10;
    g.team = {Threatened(Mate(2), CoopDbno::Threat::enemy)};
    CHECK(g.Down().state == State::downed);
    g.team[0].scene = "spot01";  // they leave while we bleed out
    g.Run(10 * kS + 2 * kFrame);
    CHECK(g.machine.Now() == State::up);
    CHECK(g.Saw(Event::alone));
    CHECK(g.Saw(Event::respawned));
    CHECK_FALSE(g.Saw(Event::bled_out));
    CHECK(g.entrance_asks == 1);
    CHECK(g.beside_asks == 0);
    CHECK(g.life == CoopDbno::kRespawnLife);
}

TEST_CASE("dbno row 318: the one watched leaves (a door, a warp): watched through the load, then the entrance") {
    Game g;
    g.team = {Threatened(Mate(2, {30, 0, 0}), CoopDbno::Threat::enemy)};
    Spectate(g);
    CHECK(g.Step().watching == 2);
    g.team[0].in_game = false;  // a door
    g.team[0].loading = true;
    CHECK(g.Run(5 * kS).watching == 2);
    CHECK(g.entrance_asks == 0);
    g.team[0].in_game = true;  // through, in another scene
    g.team[0].loading = false;
    g.team[0].scene = "spot01";
    const Output still = g.Run(CoopDbno::kNearbyGraceUs);
    CHECK(still.state == State::spectating);
    CHECK(g.entrance_asks == 0);
    g.events.clear();
    const Output asked = g.Step();
    CHECK(asked.move_entrance);
    CHECK(asked.watching == 0);
    CHECK(g.Saw(Event::alone));
    const Output o = g.Step();
    CHECK(o.state == State::up);
    CHECK(o.by == 0);
    CHECK(g.Saw(Event::respawned));
    CHECK(g.beside_asks == 0);
}

TEST_CASE("dbno row 318: three or more: only a nearby one is watched, the far one never") {
    Game g;
    g.position = {0, 0, 0};
    g.team = {Threatened(Mate(2, {1, 0, 0}, State::up, "spot01"), CoopDbno::Threat::enemy),
              Threatened(Mate(4, {20, 0, 0}), CoopDbno::Threat::enemy)};
    Spectate(g);
    CHECK(g.Step().watching == 4);
    CHECK_FALSE(g.Saw(Event::alone));
    // The nearby one goes down; the far one is still up: the entrance.
    g.team[1].state = State::downed;
    g.events.clear();
    const Output asked = g.Step();
    CHECK(asked.move_entrance);
    CHECK(g.Saw(Event::alone));
}

TEST_CASE("dbno row 318: the entrance refused with an enemy near us: tried again after kQuietUs, never every frame") {
    Game g;
    g.team = {Mate(2, {1, 0, 0}, State::up, "spot01")};
    g.entrance_result = CoopDbno::Moved::failed;
    g.enemy_near_us = true;
    CHECK(g.Down().state == State::spectating);
    CHECK(g.entrance_asks == 1);
    g.Run(CoopDbno::kQuietUs - 2 * kFrame);
    CHECK(g.entrance_asks == 1);
    CHECK(g.machine.Now() == State::spectating);
    CHECK(g.life == CoopDbno::kDownedLife);
    g.Run(3 * kFrame);
    CHECK(g.entrance_asks == 2);
    g.enemy_near_us = false;
    g.Run(3 * kFrame);
    CHECK(g.machine.Now() == State::up);  // refused again, nothing near: up where he lay
    CHECK(g.Saw(Event::respawned));
}
