// SPDX-License-Identifier: GPL-3.0-or-later
//
// Debug reader for stage 5 (docs/objects.md). With LICHII_COOP_DEBUG_OBJECTS=1
// it walks the game's live actor list every other frame and logs each actor
// that appears or goes (name, position, placement hash), and every change of
// an enemy's life. It reads only; it works with co-op on or off.

#pragma once

namespace Core {
class System;
}

namespace CoopObjects {

// Called once per presented frame, before the co-op session's frame.
void OnFrame(Core::System& system);

} // namespace CoopObjects
