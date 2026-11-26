// SPDX-License-Identifier: GPL-3.0-or-later
// CoopSwitchStandMode (ai-switches): the host's shared floor switch setting, on unless the host turns it off.
#include <doctest/doctest.h>

#include "coop_switch_stand_mode.h"

TEST_CASE("shared floor switches: on by default, the host's choice in force, guests follow the host") {
    namespace m = CoopSwitchStandMode;
    m::SetChoice(true);
    m::OnWelcomed(false);
    CHECK(m::InForce());  // a guest, not told yet (or an older host): on
    m::OnHostSettings({{"coop", "race", "off"}});
    CHECK(m::InForce());
    m::OnHostSettings({{"coop", "floor_switches", "off"}});
    CHECK_FALSE(m::InForce());
    CHECK(m::AsSetting().value == "off");
    m::SetChoice(true);
    m::OnHosting();  // the host left: the session keeps what its host told
    CHECK_FALSE(m::InForce());
    m::SetChoice(false);
    m::OnWelcomed(true);  // a new session, we host: our choice
    CHECK_FALSE(m::InForce());
    CHECK(m::AsSetting().key == "floor_switches");
    m::SetChoice(true);
    m::OnWelcomed(true);
    CHECK(m::InForce());
    CHECK(m::AsSetting().value == "on");
}
