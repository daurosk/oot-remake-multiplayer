// SPDX-License-Identifier: GPL-3.0-or-later
// The two debug panels drawn over the picture: the co-op panel (F7, LICHII_COOP_DEBUG_PANEL=1: ping, distances) and
// the item cheats panel (F8, LICHII_COOP_CHEATS=1). Both are drawn boxes, not the game's own UI, so release kits
// do not have them (the owner, 2026-11-21: no handmade drawn panels; ui-audit-01112 B6, B7). A build for tests or
// a developer turns them on with the CMake option COOP_DEBUG_PANELS=ON (hookshot/CMakeLists.txt); the unit tests
// always build with them.
// Without them: F7 and LICHII_COOP_DEBUG_PANEL do nothing, F8 opens nothing and takes no keys. The cheats' queue
// stays (CoopCheats::Give: the play-test bridge's `give` still works with LICHII_COOP_CHEATS=1); only the panel goes.
#pragma once

namespace CoopDebugPanels {

#ifdef COOP_DEBUG_PANELS
inline constexpr bool kOn = true;
#else
inline constexpr bool kOn = false;
#endif

}  // namespace CoopDebugPanels
