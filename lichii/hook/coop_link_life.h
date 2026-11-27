// SPDX-License-Identifier: GPL-3.0-or-later
//
// Our Link's Life component as a CoopCheatGive::LinkLife (coop_cheat_give.h): the play-test bridge's give and
// setflag of PlayerStatus.Life/MaxLife and the cheats window's gift write it, since the game keeps those values there
// and only mirrors them into the flags (issue 88; give-life-fix 2026-11-26).
#pragma once

#include <cstdint>
#include <optional>

#include "coop_cheat_give.h"
#include "coop_objects.h"

namespace Core {
class System;
}

namespace CoopLinkLife {

// Found at the first write by a fresh walk of the actor list from `main` (the main module; no session or earlier
// walk needed: solo play, a held game), written as the object walk writes it (the "from MaxLife" raise), and the HUD's
// gauges asked to show it (row 238). One object per give or setflag: the actor is not kept past it.
class GameLinkLife final : public CoopCheatGive::LinkLife {
public:
    GameLinkLife(Core::System& system, std::uint64_t main) : system_(system), main_(main) {}
    bool set_life(std::int32_t quarters) override { return Put(false, quarters); }
    bool set_max(std::int32_t quarters) override { return Put(true, quarters); }

private:
    bool Put(bool max, std::int32_t quarters);
    Core::System& system_;
    std::uint64_t main_;
    std::optional<CoopObjects::LinkLifeWords> words_;
};

}  // namespace CoopLinkLife
