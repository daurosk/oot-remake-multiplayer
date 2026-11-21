// SPDX-License-Identifier: GPL-3.0-or-later
//
// Debug reader for stage 5 (docs/objects.md). With LICHII_COOP_DEBUG_OBJECTS=1
// it walks the game's live actor list every other frame and logs each actor
// that appears or goes (name, position, placement hash), and every change of
// an enemy's life. It reads only; it works with co-op on or off.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
// Rows 136/137: while the host's save load holds MaxLife lower (FlagWatcher::reset_lowered), the walk lowers our
// Link's own Life component to it (and his life to that max), so the game stops writing the replaced save's hearts
// back. Nothing: no hold (the usual case, and always with co-op idle).
// Our Link's hearts filled to his maximum once, on his next read (another player took the boss's heart
// container, whose pickup fills them: CoopBossScenes::Actions::refill). Any thread.
void AskRefill();
void SetMaxLifeLowerTo(std::optional<std::int32_t> quarters);

// Our own Link's hearts as the object walk last read them from his Life
// component, in quarter hearts (4 = one heart): now and the most. Nothing
// when no walk read them in the last 2 s (not connected, no Link loaded,
// LICHII_COOP_OBJECTS off). The one source for the co-op HUD (PLAYER_LIFE,
// coop_hud) and PvP. Any thread.
struct LinkHearts {
    std::int32_t life = 0;
    std::int32_t max = 0;
};
std::optional<LinkHearts> OurLife();
// PvP (coop_pvp_game.cpp; needs LICHII_COOP_OBJECTS=1, on by default): our
// Link's life in quarter hearts as the object walk last read it; nothing
// when no walk saw our Link in the last 1.5 s. Any thread.
std::optional<std::int32_t> OurLinkLife();
// Rows 93/103 (docs/items.md, "The game's own equip"): our Link's actor (Player_Child or Player_Adult) as the
// object walk last saw it; nothing when no walk saw it in the last 1.5 s. Any thread.
std::optional<std::uint64_t> OurLinkActor();
// Any thread: take `quarters` off our Link at the next walk, as the game's
// own hit at `at` from `from` (Link flinches), or by writing the life when
// the game does not take that hit within 0.4 s (LICHII_COOP_PVP_REPLAY=0:
// the write only).
void HurtOurLink(std::int32_t quarters, const std::array<float, 3>& at,
                 const std::array<float, 3>& from);
// The kind words of a PvP strike (coop::protocol::kPvpKindWords; checked in coop_objects.cpp).
inline constexpr std::size_t kPvpKindWords = 11;
// Native puppets phase 4, path B (coop_pvp_native_game.cpp): as HurtOurLink, with the strike's kind words in
// the game's hit (its reaction and effects), and NEVER the life write: a hit the game does not take within 0.4 s
// stays not taken (docs/native-puppets.md 5.5). `quarters` 0: a no-life kind, the game's hit only. Any thread.
void HurtOurLinkNative(std::int32_t quarters, const std::array<float, 3>& at, const std::array<float, 3>& from,
                       const std::array<std::uint32_t, kPvpKindWords>& kind);
// Phase 4, every other screen (PVP_RESULT): the landed kind replayed on our `_Pvp` copy `actor` of the victim
// (CoopPuppet::Director::LivePvpActor), as protocol 14 replays a hit on an enemy copy: its invincible Life shows
// flash, sparks and sound, nothing is taken. Any thread.
void ReplayOnCopy(std::uint64_t actor, std::int32_t amount, const std::array<float, 3>& at,
                  const std::array<std::uint32_t, kPvpKindWords>& kind);
// Native puppets phase 3 (coop_lift_player.h; LICHII_COOP_LIFT_PLAYER=1): another player's Link holds ours.
// Our Link is put at `at` by the next walk, the way copies are moved: its place written, then the game's own
// physics-aware setter (kSetMatrix, vtable slot 0x218) on it. A place older than 200 ms is not used. Any thread.
void PlaceOurLink(const std::array<float, 3>& at);

// Emulator thread, each frame (G1, coop/host/event_owner.hpp): whether an event
// runs in our game (`in_event`: the actors it binds here act for it and their
// shared changes wait, whoever's event it is) and whether our own Link is in it
// (`ours`: a talk, a cutscene of ours; shared changes to actors near our Link
// wait, coop_guard.h, issue 144, and our event claims its actors). Another
// player's event replayed here is in_event without ours.
void SetLocalEvent(bool in_event, bool ours);

// Network thread, from the session's on_event: takes the object messages.
void OnEvent(const coop::net::Event& event);

// Emulator thread, after the session's frame: the placement keys the flag
// watcher wrote 0 to 1 for another player (a chest opened, a pickup taken
// there). What of them is loaded now is removed (docs/items.md, "Chests").
void TakenForOthers(const std::vector<std::uint64_t>& keys);

// Emulator thread: how many chests shown open here (another player opened
// them) our Link opened again since the last call (issue 195). Each one's
// content is taken back from the shared counts (Session::expect_doubled_gain).
unsigned TakeChestsOpenedTwice();

// Issue 109 (coop_boss): whether the boss with body actors `bodies` was
// alive in the last walk of LICHII_COOP_OBJECTS (BossAlive); nothing when
// no walk ran in the last 1.5 s, or its life is not read. Any thread.
std::optional<bool> LastBossAlive(std::span<const std::string_view> bodies);
// The same walk: whether LastBossAlive's false is a body read at life 0 (BossReadDead); false when unread.
bool LastBossReadDead(std::span<const std::string_view> bodies);
// The same walk: whether the boss has lost life (BossHurt).
std::optional<bool> LastBossHurt(std::span<const std::string_view> bodies);
// The same walk: whether the boss is still in its pre-battle phase (BossPreBattle).
std::optional<bool> LastBossPreBattle(std::span<const std::string_view> bodies);
// Whether our game owned the dungeon's boss in the last walk (nothing when it is not seen, or the walk is stale).
std::optional<bool> LastBossOurs(std::span<const std::string_view> bodies);
// The boss bodies our game is not ready to own (its intro not played here);
// empty when none. Any thread; used by the next share.
void SetUnreadyBosses(std::vector<std::string> bodies);
// The boss bodies whose death has begun in some game (player report #105): a refused hit is given to their Life
// step after the wait (Sync::SetDying). Any thread; used by the next share.
void SetDyingBosses(std::vector<std::string> bodies);
// Small keys to give back to our game (the SmallKeyNum flag of the dungeon,
// once each): a door's lock this game opened after another player already
// had, or at the same moment with the higher player number (docs/dungeon.md,
// "Locked doors"). Taken by the hook, which adds 1 to each in the game.
std::vector<std::uint32_t> TakeKeyRefunds();
// dungeon-sync-b (#75): small keys to lend to our game only (false) or to
// take back unused (true), by the SmallKeyNum flag (WorldSync::Out::key_lends,
// lend_returns). The hook keeps them from the group (coop::host::LentKeys).
std::vector<std::pair<std::uint32_t, bool>> TakeKeyLends();
// Our own game lowered these counts (Session::take_own_count_drops): a key
// spent here. A lock's second key goes back only against one of these.
void OwnCountDrops(const std::vector<std::uint32_t>& key_flags);
// The host loaded a save (host-save's FLAG_RESET): forget the kept lock
// breaks (WorldSync::ForgetKept). For the integrator to call on that event.
void ForgetKeptBreaks();

// Test only (coop_testinput's "kill NAME" step): the nearest actor of that
// name to our Link takes a hit of its whole life from our Link in the next
// walk of LICHII_COOP_OBJECTS (logs "coop: test kill ..."). Any thread.
void QueueTestKill(std::string name);
// Test only ("place PLATFORM UP", #196): the place of the moving platform (a PlatformSync driver) of that name
// nearest our Link at the last walk; nothing when none is here.
std::optional<std::array<float, 3>> TestPlatformPlace(const std::string& name);
// Test only ("hit NAME N"): as QueueTestKill, a hit of `amount` through the game's own Life step.
void QueueTestHit(std::string name, std::int32_t amount);
// Test only ("stun NAME"): our copy of the nearest boss of that name says its owner a stun (Sync::TestStun).
void QueueTestStun(std::string name);

// Present thread: the actors of the shared things this game owns now (for
// coop_drops: a drop beside one of them is its owner's to decide).
std::vector<std::uint64_t> OwnedActors();

} // namespace CoopObjects
