// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scene-change probe, test only (docs/scenes.md "Free-play scene change").
// LICHII_COOP_SCENE_PROBE=1 logs, on every change, the game's scene-change
// flags: the Bool 0x3fc35576 that makes Main pick BancChange, the
// Sequence_Reserved* strings, Sequence_IsLoadingBanc, Sequence_ControlInEvent,
// whether an event runs, and the scene. Walking through one door or exit with
// it on shows what a loading zone sets, frame by frame. Off: nothing is read.
#pragma once

#include <cstdint>
#include <optional>

#include "coop/host/player_reader.hpp"

namespace Core {
class System;
}

namespace CoopSceneProbe {

void Frame(Core::System& system, std::uint64_t main_base, const std::optional<coop::host::PlayerReading>& reading);

} // namespace CoopSceneProbe
