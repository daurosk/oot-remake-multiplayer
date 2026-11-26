// Tests for a throttled part's step in the session's frame (lichii/hook/coop_session_gate.h): the session's end is
// seen before the throttle, so one frame after a menu leave is enough to forget it (the boss glue, the time).
#include <doctest/doctest.h>

#include "coop_session_gate.h"

using CoopSessionGate::Gate;
using CoopSessionGate::Step;

TEST_CASE("session gate: the end of a session is forgotten on the first frame, inside the throttle") {
    Gate g;
    int asked = 0;
    auto due = [&] { ++asked; return true; };
    auto not_due = [&] { ++asked; return false; };
    CHECK(g.Next(false, due) == Step::idle);  // solo from the start: nothing to forget
    CHECK(asked == 0);                        // the throttle is not asked with no session
    CHECK(g.Next(true, not_due) == Step::wait);
    CHECK_FALSE(g.Connected());
    CHECK(g.Next(true, due) == Step::join);   // the first due frame: the join step
    CHECK(g.Next(true, due) == Step::run);
    CHECK(g.Next(true, not_due) == Step::wait);  // a tick just done
    asked = 0;
    CHECK(g.Next(false, not_due) == Step::forget);  // the last frame after a leave: forgotten all the same
    CHECK(asked == 0);
    CHECK(g.Next(false, due) == Step::idle);        // once
    CHECK(g.Next(true, due) == Step::join);         // the next session joins again
}
