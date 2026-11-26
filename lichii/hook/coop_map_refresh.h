// SPDX-License-Identifier: GPL-3.0-or-later
//
// Player reports #63/#76 (dungeon-sync-b, dungeon-maps-a; on unless LICHII_COOP_MAP_REFRESH=0): a dungeon map, compass,
// boss key or floors written by co-op (another player's, the join snapshot, a reset, a put-back, the solo keeper) shows on
// our Map page and minimap.
//
// The Map page and the minimap do not read the save's flags live. The HUD screen (Longshot patch 0018's
// hud_screen, CoopMapPage::MenuState) keeps its own copy: the floors at +0x1500 and the map, compass and boss
// key at +0x1538..0x153b. The game reads that copy in only at the screen's init (main+0x33e12c0) and refresh
// (main+0x33e4200), which its HUD state machine (main+0x33e4bf0) calls. On each floor change the game writes the
// copy back to DungeonInfo.<dungeon>.EnteredFloorBit (main+0x5851ac), so a shared IsGetMap or floors write
// alone does not show until the game's next refresh: the Map page, a room or scene load, an event's end
// (reports/dungeon-sync-b.md, "What the Map page draws from"; reports/dungeon-maps-a.md).
//
// So after such a write this asks the game's own refresh once, a moment later (after the game's setter has taken the
// flag), through Longshot patch 0018's hos_coop_hud_refresh: the refresh then runs at the HUD screen's own next
// update on the game's thread, so it only ever runs on a screen that is alive (no stored screen pointer is called).
// It never writes the screen's copy. Lichii (the emulator) has no such export: there it does nothing.
// Each call logs the copy before and after. Proven: lane1-a hs-map-deku-walkin 20261116-003904 (guest), hs-map-opener
// 20261116-013911 (without it the receiver's minimap stayed blank until the Map page or a room load).
//
// LICHII_COOP_MAP_REFRESH=0 turns it off. No emulator code (tests/lichii/map_refresh_test.cpp).
#pragma once

#include <cstdint>
#include <mutex>

namespace CoopMapRefresh {

namespace addr {
inline constexpr std::uint64_t kFloors = 0x1500;  // the HUD screen's floors byte (read for the log only)
inline constexpr std::uint64_t kHeld = 0x1538;    // map, compass, boss key, 0x251d4c2a (bytes; log only)
}  // namespace addr

// A shared value lands through the game's own setter a frame or more later: the refresh waits this long.
inline constexpr std::uint64_t kDelayMs = 300;

// When to ask the refresh: once, kDelayMs after the last write, and only while Link is in play.
class Refresher {
public:
    // A co-op write of a HUD dungeon item (any thread): a later one moves the time, one refresh after the last.
    void Want(std::uint64_t now_ms);
    // Row 140 (flag-oneway-c): our own put-back of a one-way dungeon item (the flag watcher's or the solo keeper's).
    // The Hookshot frame runs the flag setters right before it asks Due, and the refresh runs later in that same
    // game frame, so the HUD screen's copy takes the merged value before the game writes the copy back to
    // EnteredFloorBit (main+0x5851ac) and lowers it again: asked on the next Due (no kDelayMs), plus once more
    // kDelayMs later (as Want) in case the setter was late.
    void WantNow(std::uint64_t now_ms);
    // The hook's frame, in a session and alone: Link is in play (a scene loaded, the first Link there). Not at the
    // title, a load or a reload: the refresh waits.
    void SetInPlay(bool in_play);
    // The Hookshot frame: true once when the refresh is to be asked now.
    bool Due(std::uint64_t now_ms);

private:
    std::mutex mutex_;
    std::uint64_t due_ms_ = 0;  // 0: nothing wanted
    bool now_ = false;          // WantNow: on the next Due in play
    bool in_play_ = false;
};

Refresher& TheRefresher();

}  // namespace CoopMapRefresh
