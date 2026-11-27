# Wait for the party (tracker row 298)

Player suggestion (row 298, 0.12.0), wanted by the owner on 2026-11-27: an optional host setting that, before the
moments where players drift apart most, tells a player on the game's banner to wait for the others. It never stops
anyone: it is a notice, nothing more.

Status: built: the setting, the message, the rules and the boss door moment. The locked-room and big-chest moments are
designed below but not built: they need the probe in "Not proven" (2) first.

## The setting

- **Wait for the party** on the Host settings page (the pause menu's co-op Host settings, a Toggle row beside PvP,
  `coop_pause_menu.cpp`), **off by default**. Saved with the other host options in `coop-options.ini`
  (`party_wait = on|off`, as `pvp`), sent at Host in `HosCoopHostOptions` (the byte that was `reserved2`; every older
  caller wrote 0 there: off).
- The session's value is the server's: `PARTY_WAIT_MODE` (message type 560, protocol 19), the same shape and rules
  as `PVP_MODE` (protocol.md, "PvP"): from the server to each player when it joins and to everyone on a change; the
  host asks for a change (from anyone else it is ignored). Chat: `/wait` says it, `/wait on` and `/wait off` (host only)
  change it. A guest's row shows the host's value, fixed (as PvP's).
- Off, solo, co-op idle (no session), or alone in the session: nothing runs, nothing is read, nothing is shown.

## The moments

A moment is a place where our Link is about to start something whose result depends on who is there. Every value
below is from a source named beside it; none is new.

| Moment | When our game is in it | Source |
|---|---|---|
| Boss door | Our Link in the area of one of the eight dungeon bosses (`CoopBossScenes::InArea`, the boss room's circle grown by `kAnteGrow` 14 and its height band by `kAnteHigh` 6) but not in the room itself (`CoopBossScenes::InRoom`), and no fight of that boss on (`CoopBoss::Fighting()` false) | `lichii/hook/coop_boss.h` `Rooms()` (scene, room centre, radius, heights: docs/dungeons-boss.md "The eight rooms", read from `Banc/<Dungeon>` placements); `coop_boss_scenes.h` `kAnteGrow`/`kAnteHigh` |
| Locked combat room | Our Link in the room of a loaded room enemy counter `Obj_EnemyCounter_*` whose 64-bit key is still 0 (the room not cleared) | the 51 counter keys in `CoopDungeon::Keys()` (`coop_dungeon.cpp`, ResetTypeValue 24: "the rooms' enemy counters (a room cleared)"); docs/play-online.md "Beat-them-all rooms" |
| Big chest | Our Link within 18 m (`CoopLiveEvents::kNearM`, "its room is loaded and the player sees it") and in the same room of a loaded, unopened big chest: `Obj_TBox_A_02*` or `Obj_TBox_A_03*` | docs/items.md "The lid opens": A_02 and A_03 are the chests whose packs bring the big opening `FrontOpen` (`AS/Obj_TBox_A_02.root.asb`), every other `Obj_TBox_*` opens with `FrontOpenSmall`; the Deku Tree slingshot chest is `Obj_TBox_A_02` (docs/dungeon.md "The Deku Tree slingshot room"); keys and opened flags `CoopDungeon::Keys()`, `CoopChests::OpenedFlags()` |

Why the boss door and not the boss intro: the intro already brings everyone in the boss's area along
(docs/dungeons-boss.md "Boss cutscenes"); a player outside the area only reads a chat line and fights late. The wait
is for the moment before that, where the area rules would leave someone out.

Not moments: Ganondorf and Ganon (no room in the table: their area is "near the starter"), mid-bosses (no event flow
of the romfs is a mid-boss's intro, docs/dungeons-boss.md), small chests, overworld scenes.

## Who is "here"

Each other player in the session, from presence (`CoopPuppet::Collect`: scene, position, status), is:

- **here**: has a Link (a scene, not loading), the same scene (`coop::protocol::same_place`), and in our room:
  `CoopTeleport::CheckMove(rooms, scene, ours, theirs) == ok` (both in one room of the puppet pack's room table,
  `coop_rooms.txt`; with no table, within `kNearMove` 10 units). This is the rule the down-but-not-out spectate camera
  already uses for "the game draws them".
- **not here**: anything else, including on the title or loading.

Everyone counts, the host as much as a guest. A player who leaves the session stops counting at once.

## What a player sees

All on the game's banner (SimpleInfo_00) as notices, the 0.12.0 notice path (`chat_.Add(line, true)`): one page, the
game's own font and colour; chat goes on between them. Nothing is held on the banner (the held line is down but not
out's), and nothing is drawn.

1. Entering a moment with someone not here: `Waiting for the other players to sync... (1/3 here)`.
2. Each time the count changes while waiting: the same line with the new count (at most one every 3 s).
3. Everyone here: `Everyone is here.` The wait ends.
4. **Never forever:** 60 s after the first line (`kGiveUpUs`), or when our Link leaves the moment: the wait ends
   (`Not everyone is here: going on.` on the time-out, nothing on leaving).
5. The others: one chat line from the waiting player when a wait starts, `<name> is waiting for the party: <moment>`
   (e.g. `Waiting for the party at the boss door (Phantom Ganon)`), sent as the Forest Temple hallway's lines are (`SendChat`), so they know to
   come.

Each moment waits once a visit: the same moment (kind + dungeon + boss room or placement key) starts no new wait until
our scene changes. So walking in and out of the antechamber does not repeat it, and a beaten room or opened chest
never starts one (not a moment any more).

## Code

- `lichii/hook/coop_party_wait.{h,cpp}`: the rules, pure (no game, no session): moments in, notices and the chat line
  out. Tests: `tests/lichii/party_wait_test.cpp`.
- `lichii/hook/coop_party_wait_game.cpp`: the frame: reads the moment (boss door from `CoopBossScenes`; chests and
  counters from the objects walk) and presence, runs the rules, hands over the lines. Called after `CoopBoss::Frame`
  in `coop_hook.cpp`.
- Protocol: `PartyWaitMode` in `messages.hpp`, codec, server `Session` (as PvP).

## Not proven (the in-game checks)

1. The boss door: a player standing before a boss door is in `InArea` and not `InRoom`. The antechamber margin is
   the boss-cutscene rules' own, which docs/dungeons-boss.md calls not checked in game; the same run proves both.
   Bridge test: A warped before the Deku Tree boss door, B in another room: A's banner shot has the wait line
   (1/2 here), B's log has the chat line; B warped next to A: `Everyone is here.` on A's banner shot.
2. Chests and counters are read from the objects walk by placement hash (`[actor+0x3d8]+0x88`, coop_objects.cpp
   header): not yet known whether `Obj_EnemyCounter_*` is in the actor list at all (it may be a logic-only
   placement). Probe first (a one-line log of the loaded counters and big chests with key and position in the Deku
   Tree), then build those two moments.
3. The time-out and the never-block: A alone before the door, nobody comes: the line, then after 60 s
   `Not everyone is here: going on.`; A walks through meanwhile and nothing stops it.
4. Solo and co-op idle: no line, no read (the frame returns before reading anything while the setting is off or
   there is no session).
