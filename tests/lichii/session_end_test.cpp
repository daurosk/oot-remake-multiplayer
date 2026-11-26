// Tests for the frame's path and the ended session's last frame (lichii/hook/coop_session_end.h).
#include <doctest/doctest.h>

#include "coop_session_end.h"

using CoopSessionEnd::LastFrame;
using CoopSessionEnd::Path;
using CoopSessionEnd::PathOf;

namespace {
struct Session {};
}  // namespace

TEST_CASE("session end: the frame's path, a code mod reads only (issue 136)") {
    CHECK(PathOf(true, false) == Path::session);
    CHECK(PathOf(false, false) == Path::solo);
    CHECK(PathOf(false, true) == Path::read_only);  // refused at the start: the Customize picture still reads
    CHECK(PathOf(true, true) == Path::read_only);   // refused mid-session: its Coop stays, never its frame
}

TEST_CASE("session end: a menu leave gives the ended session one last frame, once End has kept it") {
    Session a;
    LastFrame<Session> last;
    CHECK(last.Note(nullptr).started == false);  // solo from the start
    CHECK(last.Take(nullptr, false) == nullptr);
    CHECK(last.Note(&a).started);
    CHECK(last.Take(nullptr, false) == nullptr);  // in a session: nothing due
    CHECK(last.Note(nullptr).ended);              // End cleared it ...
    CHECK(last.Take(nullptr, false) == nullptr);  // ... but has not kept it yet (its client still stopping)
    CHECK(last.Note(nullptr).ended == false);
    CHECK(last.Take(&a, false) == &a);            // kept: its last frame
    CHECK(last.Take(&a, false) == nullptr);       // once
}

TEST_CASE("session end: a new session before the last frame, and a code mod, give none") {
    Session a, b;
    LastFrame<Session> last;
    last.Note(&a);
    last.Note(nullptr);
    last.Note(&b);                                // a new session began first
    CHECK(last.Take(&a, false) == nullptr);
    last.Note(nullptr);
    CHECK(last.Take(&b, true) == nullptr);        // a code mod: that frame would write to the game
}
