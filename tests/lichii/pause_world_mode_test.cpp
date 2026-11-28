// SPDX-License-Identifier: GPL-3.0-or-later
// CoopPauseWorldMode (row 299): the host's "World runs in menus" setting, off unless the host turns it on.
#include <doctest/doctest.h>

#include "coop_pause_world_mode.h"

TEST_CASE("world runs in menus: off by default, the host's choice in force, guests follow the host") {
    namespace m = CoopPauseWorldMode;
    m::OnLeft();
    CHECK_FALSE(m::InForce());  // no session: never in force
    m::SetChoice(true);
    CHECK_FALSE(m::InForce());  // our choice alone changes nothing until we host
    m::OnWelcomed(false);
    CHECK_FALSE(m::InForce());  // a guest, not told yet (or an older host): off
    m::OnHostSettings({{"coop", "floor_switches", "on"}});
    CHECK_FALSE(m::InForce());
    m::OnHostSettings({{"coop", "pause_world", "on"}});
    CHECK(m::InForce());
    CHECK(m::AsSetting().value == "on");
    m::OnHostSettings({{"coop", "pause_world", "maybe"}});
    CHECK_FALSE(m::InForce());  // only "on" turns it on
    m::OnHostSettings({{"coop", "pause_world", "on"}});
    m::SetChoice(false);
    m::OnHosting();  // the host left: the session keeps what its host told
    CHECK(m::InForce());
    m::OnLeft();  // the session ended: a game alone pauses as in solo
    CHECK_FALSE(m::InForce());
    m::SetChoice(true);
    m::OnWelcomed(true);  // a new session, we host: our choice
    CHECK(m::InForce());
    CHECK(m::AsSetting().key == "pause_world");
    m::SetChoice(false);
    m::OnWelcomed(true);
    CHECK_FALSE(m::InForce());
    CHECK(m::AsSetting().value == "off");
    CHECK_FALSE(m::Of({{"other", "pause_world", "on"}}).has_value());
}
