// SPDX-License-Identifier: GPL-3.0-or-later
//
// Debug reader for stage 5 (docs/objects.md). With LICHII_COOP_DEBUG_OBJECTS=1
// it walks the game's live actor list every other frame and logs each actor
// that appears or goes (name, position, placement hash), and every change of
// an enemy's life. It reads only; it works with co-op on or off.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Core {
class System;
}
namespace coop::host {
class Session;
}
namespace coop::net {
struct Event;
}

namespace CoopObjects {

// Called once per presented frame, before the co-op session's frame.
void OnFrame(Core::System& system);

// LICHII_COOP_OBJECTS=1: shares the enemies. Present thread, once per frame
// after the session's frame; `session` is null when not connected.
void Share(Core::System& system, coop::host::Session* session,
           const std::optional<std::string>& scene, std::uint64_t server_now_us);

// Issue 88 (docs/items.md, "Heart containers"): the game's PlayerStatus.MaxLife
// flag as the session left it this frame (quarter hearts; nothing when not
// readable). The walk logs it beside our Link's Life component and raises the
// component's max to it (on by default; LICHII_COOP_HEARTS_TO_LINK=0 turns
// that off).
void SetMaxLife(std::optional<std::int32_t> quarters);

// Emulator thread, each frame: whether our own Link is in an event (a talk, a
// cutscene). While it is, shared changes to actors near it wait (coop_guard.h,
// issue 144).
void SetLocalEvent(bool in_event);

// Network thread, from the session's on_event: takes the object messages.
void OnEvent(const coop::net::Event& event);

// Emulator thread, after the session's frame: the placement keys the flag
// watcher wrote 0 to 1 for another player (a chest opened, a pickup taken
// there). What of them is loaded now is removed (docs/items.md, "Chests").
void TakenForOthers(const std::vector<std::uint64_t>& keys);

// Issue 109 (coop_boss): whether the boss with body actors `bodies` was
// alive in the last walk of LICHII_COOP_OBJECTS (BossAlive); nothing when
// no walk ran in the last 1.5 s, or its life is not read. Any thread.
std::optional<bool> LastBossAlive(std::span<const std::string_view> bodies);
// The same walk: whether the boss has lost life (BossHurt).
std::optional<bool> LastBossHurt(std::span<const std::string_view> bodies);
// The boss bodies our game is not ready to own (its intro not played here);
// empty when none. Any thread; used by the next share.
void SetUnreadyBosses(std::vector<std::string> bodies);
std::vector<std::string> UnreadyBosses();
// Small keys to give back to our game (the SmallKeyNum flag of the dungeon,
// once each): a door's lock this game opened after another player already
// had, or at the same moment with the higher player number (docs/dungeon.md,
// "Locked doors"). Taken by the hook, which adds 1 to each in the game.
std::vector<std::uint32_t> TakeKeyRefunds();
// Our own game lowered these counts (Session::take_own_count_drops): a key
// spent here. A lock's second key goes back only against one of these.
void OwnCountDrops(const std::vector<std::uint32_t>& key_flags);
// The host loaded a save (host-save's FLAG_RESET): forget the kept lock
// breaks (WorldSync::ForgetKept). For the integrator to call on that event.
void ForgetKeptBreaks();

// Present thread: the actors of the shared things this game owns now (for
// coop_drops: a drop beside one of them is its owner's to decide).
std::vector<std::uint64_t> OwnedActors();

} // namespace CoopObjects
