// SPDX-License-Identifier: GPL-3.0-or-later
// The play-test bridge in the game (docs/playtest.md, "Bridge protocol"): a TCP socket on 127.0.0.1 that an agent's
// MCP client drives one game through, with the frame hold (coop_playtest_core.h). Off unless
// LICHII_COOP_PLAYTEST_PORT is set: then nothing listens and AtFrame costs one atomic load.
#pragma once

#include <cstdint>
#include <filesystem>

namespace Core {
class System;
}

namespace CoopPlaytest {

// At the module's start: reads the switches and, when on, starts the bridge's thread. `game_dir`: dumps and shots
// go in <game_dir>/playtest/.
void Start(Core::System& system, const std::filesystem::path& game_dir);
// The game's calc thread, first thing at every frame hook (HOS_COOP_SITE_FRAME), before any co-op work and with
// no lock held: holds there while the bridge asks.
void AtFrame();
// The game's input thread, at each pad read (hos_coop_pad, before the pause page and the mods see the pad): while a
// `move` runs, puts its stick in stick[0] and stick[1] and says true; otherwise leaves the stick and says false.
bool PadMove(std::int32_t stick[4]);
// Whether the bridge is on (LICHII_COOP_PLAYTEST_PORT set and listening): one atomic load.
bool On();
// The bridge's frame count (0 while it is off).
std::uint64_t Frame();
// At exit: lets a held game go and stops the thread.
void Stop();

}  // namespace CoopPlaytest
