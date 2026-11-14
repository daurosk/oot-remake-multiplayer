// SPDX-License-Identifier: GPL-3.0-or-later
// The map pictures' table the puppet pack made from this player's game
// (coop_maps.txt beside coop_rooms.txt; tools/puppet_pack/map_captures.py),
// for the other players on the minimap and the pause map
// (coop/overlay/map_icons.hpp, docs/overlay.md "Players on the map").
#pragma once

#include "coop/overlay/map_icons.hpp"

namespace CoopMaps {

// Read once at the first call (any thread); empty without a pack that has
// it (then no player shows on a map, and the log says to make the pack again).
const coop::overlay::map::Table& Maps();

}  // namespace CoopMaps
