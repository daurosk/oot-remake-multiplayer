// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-rejoin's planner (hookshot/src/coop_auto_rejoin.h): when the last friend's game is joined again by itself.
#include <doctest/doctest.h>

#include "coop_auto_rejoin.h"
#include "coop_lobby_game.h"

using namespace CoopAutoRejoin;

namespace {

Facts IdleOn() {
    Facts f;
    f.enabled = f.code = f.idle = true;
    return f;
}

}  // namespace

TEST_CASE("auto-rejoin: once at the game's start, a few seconds in, with the setting on and a last game kept") {
    Planner p;
    const Facts f = IdleOn();
    CHECK_FALSE(p.Tick(f, 1000));
    CHECK_FALSE(p.Tick(f, 1000 + kStartDelayMs - 1));
    CHECK(p.Tick(f, 1000 + kStartDelayMs));
    CHECK(p.Why() == "at the game's start");
    CHECK_FALSE(p.Tick(f, 1000 + kStartDelayMs + 100));   // (once)
    CHECK_FALSE(p.Tick(f, 100000));
}

TEST_CASE("auto-rejoin: never with the setting off, no last game, or co-op from the game's start") {
    for (int which = 0; which < 3; ++which) {
        Planner p;
        Facts f = IdleOn();
        if (which == 0) f.enabled = false;
        if (which == 1) f.code = false;
        if (which == 2) f.launcher = true;
        for (std::uint64_t t = 1000; t < 100000; t += 500) CHECK_FALSE(p.Tick(f, t));
    }
}

TEST_CASE("auto-rejoin: the player's own Host or Join before the start's moment: no join of its own") {
    Planner p;
    Facts f = IdleOn();
    p.Tick(f, 1000);
    f.player_acted = true;
    f.idle = false;
    CHECK_FALSE(p.Tick(f, 2000));
    f.player_acted = false;
    f.idle = true;   // (the player's join failed: still nothing of ours)
    CHECK_FALSE(p.Tick(f, 1000 + kStartDelayMs + 10));
}

TEST_CASE("auto-rejoin: a dropped connection is tried again, five times, further apart, until it is in") {
    Planner p;
    Facts f = IdleOn();
    p.Tick(f, 1000);
    f.idle = false, f.guest_in = true;   // (in the friend's game)
    CHECK_FALSE(p.Tick(f, 2000));
    f.idle = true, f.guest_in = false;   // (it dropped)
    CHECK_FALSE(p.Tick(f, 10000));
    CHECK_FALSE(p.Tick(f, 10000 + kFirstRetryMs - 1));
    CHECK(p.Tick(f, 10000 + kFirstRetryMs));
    CHECK(p.Why() == "the connection dropped (try 1 of 5)");
    CHECK(p.Retrying());
    std::uint64_t t = 10000 + kFirstRetryMs;
    for (int i = 2; i <= kTries; ++i) {   // (each try failed: idle again)
        CHECK_FALSE(p.Tick(f, t + kRetryMs - 1));
        t += kRetryMs;
        CHECK(p.Tick(f, t));
    }
    CHECK_FALSE(p.Tick(f, t + kRetryMs));   // (five tried: it stops)
    CHECK_FALSE(p.Retrying());
    CHECK_FALSE(p.Tick(f, t + 10 * kRetryMs));
}

TEST_CASE("auto-rejoin: a try that gets in stops the tries, and the player's Leave is not a drop") {
    Planner p;
    Facts f = IdleOn();
    p.Tick(f, 1000);
    f.idle = false, f.guest_in = true;
    p.Tick(f, 2000);
    f.idle = true, f.guest_in = false;
    p.Tick(f, 3000);
    CHECK(p.Tick(f, 3000 + kFirstRetryMs));
    f.idle = false, f.guest_in = true;   // (back in)
    CHECK_FALSE(p.Tick(f, 3000 + kFirstRetryMs + 500));
    CHECK_FALSE(p.Retrying());
    f.idle = true, f.guest_in = false, f.player_left = true;   // (the player left)
    CHECK_FALSE(p.Tick(f, 20000));
    f.player_left = false;
    CHECK_FALSE(p.Tick(f, 20000 + kFirstRetryMs + 1));
    CHECK_FALSE(p.Retrying());
}

TEST_CASE("auto-rejoin: turned off while a drop is tried: no more tries") {
    Planner p;
    Facts f = IdleOn();
    p.Tick(f, 1000);
    f.idle = false, f.guest_in = true;
    p.Tick(f, 2000);
    f.idle = true, f.guest_in = false;
    p.Tick(f, 3000);
    f.enabled = false;
    CHECK_FALSE(p.Tick(f, 3000 + kFirstRetryMs));
    f.enabled = true;
    CHECK_FALSE(p.Tick(f, 3000 + kFirstRetryMs + kRetryMs));   // (the plan was dropped with the setting)
}

TEST_CASE("auto-rejoin: turned on later in the game, it does not join at once: it waits for a drop") {
    Planner p;
    Facts f = IdleOn();
    f.enabled = false;
    for (std::uint64_t t = 1000; t < 60000; t += 500) CHECK_FALSE(p.Tick(f, t));
    f.enabled = true;   // (the page's switch, a minute in)
    for (std::uint64_t t = 60000; t < 120000; t += 500) CHECK_FALSE(p.Tick(f, t));
    f.idle = false, f.guest_in = true;   // (joined by hand, then dropped: that is tried again)
    CHECK_FALSE(p.Tick(f, 120000));
    f.idle = true, f.guest_in = false;
    CHECK_FALSE(p.Tick(f, 121000));
    CHECK(p.Tick(f, 121000 + kFirstRetryMs));
}

TEST_CASE("auto-rejoin: turned on before the start's moment, the start's join is still made") {
    Planner p;
    Facts f = IdleOn();
    f.enabled = false;
    CHECK_FALSE(p.Tick(f, 1000));
    f.enabled = true;
    CHECK_FALSE(p.Tick(f, 2000));
    CHECK(p.Tick(f, 1000 + kStartDelayMs));
}

TEST_CASE("auto-rejoin: the host's Kick or ban is not a drop: no join by itself until the player joins") {
    Planner p;
    Facts f = IdleOn();
    std::uint64_t t = 1000;
    CHECK_FALSE(p.Tick(f, t));
    CHECK(p.Tick(f, t += kStartDelayMs));   // (the start's join)
    Facts in = f;
    in.idle = false, in.guest_in = true;
    CHECK_FALSE(p.Tick(in, t += 1000));     // (in the friend's game)
    Facts away = in;
    away.turned_away = true;                // (the host's Kick: refused, still "connected" a moment)
    CHECK_FALSE(p.Tick(away, t += 100));
    away.guest_in = false, away.turned_away = false;
    CHECK_FALSE(p.Tick(away, t += 100));
    for (std::uint64_t end = t + 120000; t < end; t += 500) CHECK_FALSE(p.Tick(f, t));   // (idle: no try at all)
    CHECK_FALSE(p.Retrying());
}

TEST_CASE("auto-rejoin: a refusal while idle after the drop also stops the tries planned") {
    Planner p;
    Facts f = IdleOn();
    std::uint64_t t = 1000;
    CHECK_FALSE(p.Tick(f, t));
    CHECK(p.Tick(f, t += kStartDelayMs));
    Facts in = f;
    in.idle = false, in.guest_in = true;
    CHECK_FALSE(p.Tick(in, t += 1000));
    CHECK_FALSE(p.Tick(f, t += 100));       // (the drop: a try is planned)
    CHECK(p.Retrying());
    Facts away = f;
    away.turned_away = true;                // (the refusal's event came after the disconnect)
    CHECK_FALSE(p.Tick(away, t += 100));
    CHECK_FALSE(p.Retrying());
    for (std::uint64_t end = t + 120000; t < end; t += 500) CHECK_FALSE(p.Tick(f, t));
}

TEST_CASE("auto-rejoin: after a refusal, the player's own Join brings the drop's tries back") {
    Planner p;
    Facts f = IdleOn();
    std::uint64_t t = 1000;
    CHECK_FALSE(p.Tick(f, t));
    CHECK(p.Tick(f, t += kStartDelayMs));
    Facts in = f;
    in.idle = false, in.guest_in = true;
    in.turned_away = true;
    CHECK_FALSE(p.Tick(in, t += 100));     // (kicked)
    Facts acted = f;
    acted.player_acted = true;             // (the player's own Join)
    CHECK_FALSE(p.Tick(acted, t += 1000));
    in.turned_away = false;
    CHECK_FALSE(p.Tick(in, t += 1000));    // (in again)
    CHECK_FALSE(p.Tick(f, t += 100));      // (a real drop now)
    CHECK(p.Retrying());
}

TEST_CASE("auto-rejoin: only the host's choice and hopeless refusals stop it, a full server or a name taken retries") {
    using R = coop::protocol::RefuseReason;
    for (const R r : {R::kicked, R::banned, R::not_let_in, R::bad_password, R::wrong_protocol_version,
                      R::wrong_game_build}) {
        CHECK(CoopLobby::StopsRejoin(r));
    }
    for (const R r : {R::server_full, R::name_taken}) {
        CHECK_FALSE(CoopLobby::StopsRejoin(r));
    }
}
