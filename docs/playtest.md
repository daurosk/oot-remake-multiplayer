# Live play-testing (two games, step by step)

The unit tests prove the sync logic; only a game run shows what two players actually see. The two-game runner
(`tools/autotest/hookshot.py run`, [testing.md](testing.md)) plays a whole script blind. The live kit keeps both games
open and takes one step at a time, so a tester (a person or Claude Code) acts, looks at both games, and decides the
next step: the way agent-driven play of other games works, with the game handing over its state as data.

Maintainer only, like the rest of `tools/autotest`: it needs two Hookshot test copies with the game.

## The pieces

| Piece | What it does |
| --- | --- |
| `tools/autotest/live.py start` | Starts the server and both games (the runner with `--keep-open`), plays each game's boot script (e.g. warp to a dungeon, past the title), and leaves them running. Each game's save is restored to its baseline first (`--no-restore` keeps the copies' saves). `--lane N` uses a test lane's pair, port and pipes (`tools/autotest/lanes.py`). One game alone: only `--game-a` or only `--game-b`, or `--lane N --only A` (or `B`); it keeps its own bridge pipe and play-test port, and with `--lane` takes its own lock (`laneN-A.lock`), so two sessions can drive A and B of one lane at once, while a two-game session on that lane waits for both. `--server` is optional (without it the games use their `coop.ini`). Starts the session's watchdog. Writes `session.json` in the session folder. |
| `live.py do --session DIR A "lines"` | Plays test script lines in one game (`goto`, `toward`, `press`, `hold`, `warp`, `place`, `setflag`, `kill`, `hit`, ...; `;` between lines), then returns that game's state. |
| `live.py state --session DIR [A\|B] [--since ID]` | Each game's control-bridge snapshot (map, Link's place, facing, camera, life, rupees, age, event, loading, A prompt) and its new `coop:` log lines since the last look. Each look has a `state_id`; with `--since` only what changed comes back ("Kit helpers"). A session started with the play-test bridge adds `menu` (what co-op's menus show, see below; `menu_error` when the bridge did not answer). |
| `live.py shot --session DIR [A\|B]` | A screenshot of each game (a PNG path). |
| `live.py checkpoint --session DIR TAG` | Both games write the value of every flag in the shared dungeon and story lists (the module's `dump TAG` test command, next to its commands file; `CoopDungeon::Flags` and `CoopStory::Flags`, not the live-only or autogen lists), and a screenshot of each game is taken (`checkpoints/TAG-A.png`, `TAG-B.png`); all are kept as `checkpoints/TAG.json` with each game's status. The games dump one after the other, so a flag still on its way between them can differ in one checkpoint: a real desync still differs in the next one. |
| `live.py stop --session DIR` | Ends the session: the watchdog quits both games, kills what did not quit and the server, and lets the lane lock go. |
| `tools/autotest/desync_check.py DIR` | Compares the checkpoints in order: the first tag where the two games' shared flags differ, and which flags, with the checkpoint's shots. Exit 0 every checkpoint compared and agrees, 1 desync, 2 nothing compared (no checkpoint, or none with both games' flags), 3 incomplete (one could not be compared). A plain script, no judgment. |
| `tools/autotest/mcp_playtest.py --session DIR` | An MCP server (stdio, standard library only) with the tools `state`, `act`, `shot`, `checkpoint` and `desync_check`, so Claude Code calls them by name. |

Register the MCP server once from the repository folder, for a session started with `live.py start`:

```sh
claude mcp add oot-playtest -- python -I tools/autotest/mcp_playtest.py --session <session folder>
```

## Where the game windows go

`live.py start` puts both game windows side by side on one monitor and does not take the focus. Choose the monitor
with `--display \\.\DISPLAY2` (a device name), or the environment variable `OOT_TEST_DISPLAY`; the default is
`\\.\DISPLAY2`. A name that no monitor has (DISPLAY2 off) puts the windows on the primary monitor, and the start says so. Details and what to check:
[testing.md](testing.md#where-the-test-games-windows-go). `live.py shot` and the MCP `shot` tool go through the
game's own control bridge, so they work wherever the windows are. A bridge shot leaves out Longshot's overlays (the
co-op pages); read `menu` in the state instead.

## Bridge protocol

The play-test bridge is a small part of the co-op module inside each game (`hookshot/src/coop_playtest.cpp`). An agent
or MCP client drives one game per connection. It adds what Longshot's own control bridge lacks: **frame control** (the
game holds still between calls). It also gives the co-op module's test commands and Longshot's input, shots and Link
snapshot through one socket with one frame count.

### Turning it on (off by default)

Nothing listens unless the game is started with:

| Switch | Value |
|---|---|
| `LICHII_COOP_PLAYTEST_PORT` | TCP port, 1024 to 65535. Unset, empty or `0`: the bridge is off and nothing listens |
| `HOS_CONTROL` | Longshot's control socket (Linux) or pipe (Windows). The bridge forwards `input`, `shot` and the Link snapshot in `state` to it. Without it those answer `no_control` |
| `LICHII_COOP_PLAYTEST_IDLE` | Seconds a hold may last with no command before the game resumes by itself (default 600, `0` never), in case the tester crashed |

The module has the bridge built in but off. A tester turns it on for one game with
these variables; `live.py start` sets them for each game it starts. The log says
`coop: playtest: listening on 127.0.0.1:<port>` or `coop: playtest: off (<why>)`.

**Ports, one per game:** player N on `5000 + N` (p1 5001, p2 5002, ... p4 5004). On test lane L: `5000 + 10*L + N`
(lane 1: 5011, 5012). The MCP server takes the port as its argument.

**Security:** bound to `127.0.0.1` only, never another address. One client at a time: a second connection gets one
`busy` reply and is closed. **Keep one connection open for the whole session:** when the client's connection closes
(it quit or crashed), a held game runs again at once, so a lost tester never leaves a game frozen. Dumps and shots go
only into the game's own `playtest/dumps/` and `playtest/shots/` folders, named by a tag; no path is taken from the
client. It can do what Longshot's control bridge and the cheats panel can do (move Link, warp, set
flags, give items), so it is for test copies with a throwaway save.

### Messages

One JSON object per line (UTF-8, `\n`), at most 64 KB a line. A request has `cmd` and an optional `id` (any JSON value,
given back). Requests are answered in order, one reply each. Every reply carries:

| Field | Meaning |
|---|---|
| `id` | The request's `id`, or `null` |
| `ok` | `true`, or `false` with `error` (a code below) and `message` (for a person) |
| `frame` | The game's frame count: calc frames since the game started (the module's per-frame hook). It is what `step` counts and what lines up two games' dumps |
| `paused` | Whether the game is held now |

`input`, `step` and `warp` replies hold the end state nested as `"state": {...}`; `state` itself is flat.

Error codes: `bad_request` (unknown `cmd`, a missing or bad field), `busy` (another client), `no_control`
(`HOS_CONTROL` unset or unreachable), `not_in_play` (no Link: title, file select, a load), `not_in_event` (a `warp`
in free play that only an event can take: an entrance, or a map Longshot's teleport does not list; the message names
the way out), `wrong_age` (no verb answers it now: a `warp` scene ending in the other age sets that age first), `not_ready` (a `give` with a load running or no fresh Link sample: step until `state.not_ready` is null), `refused` (a verb the gate shuts, a strike
id with no world list, or a full strike queue), `timeout`, `failed` (the game refused; `message`
says why), `did_not_stick` (a `give` or number `flag` write the game's next frames put back: `message` names the flag, the value written and the one back).

```
> {"id":1,"cmd":"hello"}
< {"id":1,"ok":true,"frame":5321,"paused":false,"protocol":1,"control":true}
```

### Frame control

The game holds inside its own per-frame hook: the calc thread waits there before the frame's update, so a held game
does not advance at all (no Link, no enemies, no timers). The window, the sound thread, Longshot's control bridge and
the co-op connection keep running (the network thread answers the server's pings, so a held game is never dropped
as timed out). The other game's puppet of this player stands still while it is held: the network thread sends this
player's last sample again every 200 ms (the keep-alive), standing and with the last animation, because a player
whose samples stop fades out in 0.4 s and loses its puppet ([puppet.md](puppet.md) "Cases"). After a reconnect while
held it also sends the status and equipment again. Nothing is sent while held in a load or on the title (no sample
then). The server does not know a game is held: an enemy or boss this game owns stays with it while the other game
runs. So **always hold both games** and step them in turn, the same number of frames.

| Command | Fields | Does |
|---|---|---|
| `pause` | | Holds the game at its next frame. Replies once it is held |
| `resume` | | Lets it run in real time again |
| `step` | `frames` (1 to 3600, default 1) | Lets exactly that many frames run, then holds. Replies when held again, with `from` (`frame - from == frames`). A running game is held first |

### Commands

| Command | Fields | Does / replies |
|---|---|---|
| `state` | | The state below. Does not advance the game, with one exception: when Longshot's Link snapshot is older than the frame, or none was ever taken (the first `state` on a game held since its start), and the game is held, `state` lets ONE frame run (a status pending) to take a fresh one. If none comes (a title, a pause or a load) the Link members are `null`, `snapshot_stale` is `true` for an old one, and `not_ready` says why |
| `input` | `buttons` (list of names, default none), `stick` `[angle, strength]` (left stick: angle in degrees, 0 = up, 90 = right, clockwise; strength 0 to 1), `rstick` (the same for the right stick), `frames` (1 to 3600, default 1), `exclusive` (default `true`: the real pad and keys are shut out meanwhile) | Holds that pad for `frames` frames from the next frame, lets them run, then holds. Replies with `from` and the `state` at the end. Buttons (Switch names): `A B X Y L R ZL ZR PLUS MINUS LS RS UP DOWN LEFT RIGHT` |
| `move` | `yaw` (the world direction in degrees: 0 toward +z, 90 toward +x, the state's `facing` and `camera` convention), `strength` (0 to 1, default 1), `frames` (1 to 3600, default 1), `exclusive` (as `input`) | Walks toward that world direction for `frames` frames, then holds. The module turns the yaw into the left stick at each pad read with the camera of that same frame (the renderer's `clip_from_world`, the matrix the name tags use; the bridge keeps it on while it runs, so `move` works solo and before a session too: the ground part of its view direction is stick up, its screen x made square to that is stick right), so a camera that turns during the walk is followed and no handedness or offset is assumed. Buttons and the right stick are neutral. Replies with `from`, `yaw`, `reads` (pad reads it steered), `no_camera` (reads with no camera: the stick was centred), `stick` (the last `[angle, strength]` it made, as `input`'s, or null), `view` (that frame's camera yaw, or null) and the `state`. Refused with `no_camera` (nothing run) when the game has no camera yet (a title, a load) or the view looks straight down |
| `warp` | `scene` (a map name, e.g. `KokiriForest_Child`), `entrance` (a start position name in that scene; omitted: the scene's own), `timeout` (seconds, default 60) | The co-op test warp (the game's own `Sequence_ReservedNextBanc`, as the test script's `warp`). Two ways, picked from a fresh snapshot (`PlanWarp`). **While an event runs** (a talk, a sign, the event a save load starts) the module's own request goes in; the game takes that only then ([scenes.md](scenes.md) "Free-play scene change": the game reads `Sequence_ReservedNextBanc` only while an event runs; [testing.md](testing.md)). **In free play** the verb asks Longshot's control `teleport` (the Cheats page's list: fields, villages, dungeon entrances; the game takes it at Link's next update, so frames run), the way `hookshot.py` boot-script warps go; the reply has `"via":"teleport"` (or `"event"`) and `"map"`, the map it waited for. Longshot picks the map for Link's current age, in its own spelling: the verb waits for the map in Longshot's reply, not for the name asked, so any case of the name works; a name ending in the other age (`KokiriForest_Adult` on a child Link) first sets `Sequence_IsChildhood` (the lane recipe: setflag, 30 frames, then the warp) and goes by the module's map change event, since Longshot's teleport lands at Link's current age (a name with no suffix is as it was), and a teleport Longshot sent but the game's event manager did not accept (`accepted` false) answers `refused` at once instead of waiting out the timeout. The teleport has no start position and no map outside its list, so in free play an `entrance`, or a map not in the list (a boss stage, when Longshot answers `not_found`), goes by the game's map change event instead (`CoopTeleport::MapChange`, the start from `lichii/hook/coop_start_points.inc`; the reply's `"via":"map_event"`); only a scene or start the module does not have, or the map change event switched off, answers `not_in_event` (or `failed`) at once and changes nothing. A boss stage starts its intro event on arrival, so its warp reply is usually `timeout` with Link already in the scene (`Pair.warp_both(..., arrive_in_event=True)` takes that as the arrival). The free-play path is not proven through this verb in a game yet (Longshot refuses a teleport while an event plays, so one asked during the save's arrival event answers `failed: Longshot's teleport: refused: an event is playing ...`; Longshot also allows one teleport every 8 s per game, so two warps in a row on one game answer `failed` with `another teleport is still in progress`). The game runs (10 frames at a time) until Link is in that scene with no event or load (a fresh snapshot says so), then holds again. Replies with `"arrived": true` and the `state`; after `timeout` seconds, still held, `timeout` with the state in `message` |
| `flag` | `name` (a GameData flag name or a `0x` hash), `value` (number, `true`/`false`, or an enum value's name) | Sets the flag through the game's own setter (the test script's `setflag` / `setenum`). Done inside the held frame; a number write is then read back after 2 frames (`did_not_stick` when the game put the old value back). `PlayerStatus.Life` and `PlayerStatus.MaxLife` are written into our Link's Life component first (the game keeps them there; the flags are only its mirror). Replies with `was` and `value`. A `0x` value goes to an Enum and is taken only when `flag_read` read that hash from that same flag in this game run (no raw number is written to an Enum) Row 294: a `NAME[i]` name (`OwnedHorseList.ActorName[0]`) sets element i of a String64Array to the text `value` (`""` empties it) through the game's String64Array queue; the log says `coop: test settext NAME[i] "text": set` |
| `flag_read` | `name` (a GameData flag name, a `0x` hash, `key64:0xKEY`, or `NAME[i]` for element i of a String64Array such as `OwnedHorseList.ActorName[0]`, row 294) | Reads one flag inside the held frame; writes nothing, so it is not gated (it exists only where the bridge does). Replies with `kind` (`int`, `bool`, `enum`, `uint`, `key64`, `text`, `unknown`) and `value` (an enum as its value's hash `0x` and 8 hex digits; `null` when unknown); an `int` also has `pending` (true while a write of ours still waits in the game's queue) |
| `give` | `item` (`bombs`, `bow`, `hookshot`, a bare item name such as `Item_Boomerang` or `Weapon_Sword_Kokiri` (its `PouchData.<name>.StockNum` count if the game has one, else its `IsGet.<name>` flag; neither: the reply says which two it looked for), or a full item flag name such as `IsGet.Item_Boomerang` or `PouchData.Item_Bomb.StockNum`) | The test script's `give`, needs `LICHII_COOP_CHEATS=1`. The flags are written at once inside the held call (the item, its `IsGet`, and the ammo and capacity the game gives with it, never lowered), not queued for a frame: the reply's `result` says what each flag was and is (`no change: ... already 1` when Link had the item; `failed` with the reason when the item was not written; `partial` true when the item is in but its `IsGet` or a grant was not, `result` naming the flag); `get_state` with `inventory` true shows it with no step. Each item flag is then read back after 2 frames: `did_not_stick` when the game put the old value back (a value it keeps elsewhere). `PlayerStatus.MaxLife` goes into our Link's Life component first, the flag being its mirror (no Link found: `failed`). Refused with `not_ready` while a load runs or when there is no fresh Link sample (a load sends no Link update, so the old sample cannot tell; a pause and a title the same): the give runs one frame first to take a sample, as `state` does, and if none comes the answer is `not_ready`, nothing written. Step until `state.not_ready` is null, then give again. `bombs` is the small bag and 20 bombs, `bow` the bow with the small quiver and 30 arrows. A bomb that can be lifted also needs `PlayerStatus.LiftGrade` (docs/items.md, "Bombs from the cheat") |
| `act` | `lines`: ONE test line, at most 256 characters, no control characters, no `;` or `#`. Allowed kinds: `kill`, `hit`, `hurt`, `place`, `face`, `aim`, `wear`, `dress`, `assign`, `chat`, `menu`, `setflag`, `setenum`, `verb` (`verb ACTOR N`, test only, row 287: the boss of that name that this game owns runs action verb N) | The test script's line, run on the game's own thread inside the held frame (the same parser as `live.py do`). Any other kind (`goto`, `warp`, `press`, ...) is refused with `line N: WORD is not allowed here (allowed: ...)`; use the commands above for those. Replies with `lines` and a `note`. Written at once inside the held call: `place`, `face`, `wear`, `assign`, `setflag`, `setenum`. Queued for the next frames (`step` a few, then read the `state`): `dress`, `kill`, `hit`, `aim`, `hurt`, `chat`, `menu`, `verb`. `kill` and `hit` take an optional actor id before the name (`kill 2147483648 Enemy_Example`, `hit 2147483648 Enemy_Example 2`); their reply also carries `strike_seq`, the number the result in `state.strike` will have. Without the sharing walk's list (solo, where the list is the solo walk's, or the walk not running) an id is refused with `refused: no world list`. Behind the verb gate below |
| `dump` | `tag` (1 to 64 letters, digits, `-` and `_`) | Writes `playtest/dumps/<tag>.json` in the game's folder: `{"frame", "tag", "state", "flags": {name: value}}`, the shared flags (the checkpoint's lists) read inside the held frame. Replies with `path` (absolute) and `flags` (the count) |
| `text` | `since` (a whole number, default 0: every text kept), `layout` (optional: only layouts whose name has it, e.g. `MessageDialog_00`) | The texts the game set in its text panes after `since`, oldest first; read only, no frame runs, not gated. Replies with `hooked` (this exe has patch 0041's wrap: false means `texts` stays empty), `last` (the newest `seq`: pass it as the next `since`), `lost` (texts after `since` that are no longer kept) and `texts`: `[{seq, frame, layout, pane, text, length, cut, markup}]`. See [Text boxes](#text-boxes) |
| `shot` | `name` (optional; letters, digits, `-` and `_`; default `shot-f<frame>`) | A screenshot through Longshot's bridge into `playtest/shots/<name>.png`. A held game presents nothing, so it runs 1 frame at a time (at most 5) until the picture comes: the reply's `advanced` says how many. Replies with `path`, `width`, `height` |
| `key` | `key`: a host key's name, `F1` to `F12`, `A` to `Z` or `0` to `9` (a key, not a character: text is `act chat`, the game's pad is `input`) | The key as the game window gives it to the module: `hos_coop_key`'s steps for `(vk, 0)`, as Longshot's `WM_KEYDOWN` calls it (not its Linux SDL-thread mark: the bridge's thread is not the window's) (ls32-src/host/display.c; `vk` the Windows virtual key, `KeyVk`), so what it does and logs is a real press's: F1's route (`coop: F1: ...`: free play opens the game's - menu on co-op's Multiplayer tab, an event, a load or the title Longshot's menu on co-op's Multiplayer tab, hookshot/src/coop_menu_tabs_rule.h), F6's Players, the chat key. No OS key, no window focus, no frame runs: what it opens moves at the next frames. Not gated (a key reaches no wire). Replies with `key`, `vk` and `taken` (co-op took it). A key co-op leaves is Longshot's, which the bridge never plays (`longshot` `none`), with one exception: F1 left while co-op's tab shows is Longshot's F1 closing its menu (gpu/vk/vk_menu.cpp: F1 toggles it), done as `HOS_COOP_MENU_WANT_CLOSE` (`longshot` `close`), so a test shuts what its F1 opened. Python: `Pair.key(x, name)`; tests: `tools/autotest/ingame/test_f1.py` |

**The verb gate.** `act`, `warp`, `flag` and `give` reach the wire and the other players' games, so they run only when
the test runner asked for them and cheats are on: `LICHII_COOP_PLAYTEST_VERBS=1` and `LICHII_COOP_CHEATS=1`. The runner
(`tools/autotest/hookshot.py`, so `live.py start --playtest` too) sets both next to `LICHII_COOP_PLAYTEST_PORT`. No file
a player starts from names `LICHII_COOP_PLAYTEST_VERBS`: not `play.ps1`, not `start.sh`, not `coop-options.ini`
(`test_playtest_bridge.py` checks it). Otherwise the reply is `refused` with one of:

- `refused: this is not a test run (LICHII_COOP_PLAYTEST_VERBS is not set by the test runner), and these commands reach the wire and other players`
- `refused: cheats are off (LICHII_COOP_CHEATS is not set)`

### Text boxes

The `text` command reads what the game writes into its text panes, so a tester can read a talk without a picture.
Patch 0041 (`hookshot/patches/0041-coop-pane-text-wrap.patch`) adds wrap 25 to patch 0005's table: the game's pane
text setter (main+0x439520, `kPaneSetMsg` in [addresses.md](addresses.md); x0 the pane, x1 a reference {+0 the text,
+8 its length in bytes, +0x10 its attributes}). The patch only names the function; what is kept, and when, is the
module's (`hookshot/src/coop_talk_text.{h,cpp}`), and only while the bridge is on (`LICHII_COOP_PLAYTEST_PORT`): a
player's game copies nothing.

- The text is copied at the call (the box keeps the pointer, not a copy). At most 2048 bytes a text (`cut` true when
  longer) and the last 256 texts are kept.
- Each text names its layout and pane, read from the pane as Longshot's `host/hos_wide_ui.c` reads them (pane +0x18
  its layout, layout +0xf8 the file header: +0x18 its name, +0x20 the pane names; layout +0x100 the pane array of
  0xb0-byte panes).
- Read in a lane run (Kokiri shop, Mido): a talk is `MessageDialog_00`, `T_Name_00` (and `T_NameNavi_00`) the
  speaker; its reply buttons are `PaMessageBtn_00`, one pane `T_Btn_00` set once per button, in order. The box's own
  text did **not** come through this setter there. Others seen: `PaNumDisplay_00` (the rupee counter, set every frame
  while it counts), `PaShopBuyInfo_00` (a shop item's info, with markup), `DoCommand_00` `T_GuideA_00` (the A
  prompt), `PaGuideKey_00`, `ActionGuideTips_00`, `PaMenuMessage_00` ([game-ui.md](game-ui.md)).
- Patch 0042 (`hookshot/patches/0042-coop-pane-body-wrap.patch`) wraps the two setters found in main that skip
  0041's call and call the text box's set-string (main+0x439680) themselves ([game-ui.md](game-ui.md), "Text"): wrap
  26, a letter-animated pane's set-text (main+0x76a460, `kPaneSetBody`; x0 the pane, x1 the text, w2 its length; its
  only caller is a LetterAnimControl's set-text), and wrap 27, the set-text of each pane with a matching alias
  (main+0x377a990, `kPaneSetAlias`; the same reference as 0041's). Seen in a lane run (2026-11-24, Kokiri shop and
  Mido): a talk's box text comes through wrap 26, layout `MessageDialog_00`, pane `T_Message_00`, `markup` true; the
  speaker panes (`T_Name_00`, `T_NameNavi_00`), the reply buttons and the HUD panes stay wrap 25; wrap 27 did not
  appear in a talk. The reply's `body` is true when the exe has 0042, and each text's `wrap` (25, 26 or 27)
  says which setter gave it. `talk` takes a wrap-26 text of a message layout as a box whatever its pane. Without
  0042 a talk's body is not in `text` (take a screenshot to read it).
- The window sets its text again every frame, so a text the pane already shows is not kept again (unless the pane went 30 frames without a set: a later talk with the same speaker or button is kept), and a pane's first
  set to "" is not kept. A text that only grew replaces its shorter self when nothing came between (a box typed out
  letter by letter, if the game does that).
- Valid UTF-8 comes as it is; any other byte and every control character as a JSON `\u00XX`, and `markup` is true:
  those are the game's markup. A tag is the byte 0x0e, then group, type and size (16-bit little-endian each), then
  `size` bytes of parameters; seen: a pause or colour (no or few bytes), a name (a length, the name, a 0 byte: an
  animation such as `Greeting`) and a variable (`Shop_SelectItemName`, `UI_MessageTargetActorName`), between the
  words. `talk` cuts the tags out of a box's `text` and keeps the whole in `raw`. The page-break tag (group 0, type 4,
  no bytes, seen in the Kokiri shop and Mido's text, 2026-11-24) becomes one space, so two pages are not glued into one
  word. A variable (an item name, a price: tag group 2) stays in `text` as `{Name}`, e.g. `The {Shop_SelectItemName}?`:
  the bridge has only the variable's name, the game fills in the value later (read it from the shop's info panes). A
  tag's size is in bytes; a character from U+0100 up counts as its UTF-8 bytes, a lower one as one byte.
- An exe without patch 0041 answers `hooked` false, and the module logs `coop: playtest: this Hookshot has no pane
  text wrap (patch 0041)` once at the HELLO.

The MCP `talk` tool is built on it (see [Helpers](#helpers-several-calls-in-one)); `get_text` is the bare command.

`OOT_COOP_KIT_START` is no sign of a test: the kit's start files set it for every player's game. Nor is a loopback
server: a real host's game connects to its own server at 127.0.0.1. At start the module logs `coop: playtest: verbs
on|off (test runner: yes|no, cheats: on|off)`. `state`, `dump`, `input`, `step`, `pause`, `resume`, `shot` and `text` are not
gated.

### State

```
{"frame":5400,"paused":true,"scene":"KokiriForest_Child","room":null,"pos":[-7.5,0.0,71.1],"facing":90.0,
 "camera":180.0,"life":12,"max_life":12,"rupees":5,"age":"child","event":false,"loading":false,"prompt":"Talk","body":"child",
 "equips":{"weapon":"Weapon_Sword_Kokiri","shield":null,"head":null,"armor":null,"boots":null},
 "inventory":{"PouchData.Item_Bomb.StockNum":0,"PouchData.Item_Hookshot.StockNum":1},"coop":{"players":[{"number":1,"name":"A","self":true,"state":"connected","scene":"KokiriForest_Child"}]},
 "snapshot_present":5398,"control":true}
```

`scene`, `pos`, `facing`, `camera`, `life`, `max_life`, `rupees`, `age`, `event`, `loading` and `prompt` are
Longshot's Link snapshot (the control bridge's `status`, `game`): metres, yaw in degrees (0 toward +z, 90 toward +x),
quarter hearts. Longshot takes the snapshot only in a running Link update, so on a held game it is as old as the last
frame that ran. The bridge therefore never gives an old one: if `snapshot_present` is more than 4 frames behind the
reply's `frame` (the constant `kSnapshotSlack`, 4; a margin, not measured: while a game runs, Longshot answers a `status` at Link's next update, so the gap is about 0 to 1 presents, and it keeps the old sample only after its 250 ms wait for an update, Longshot `control.c` `cmd_status`: `oot_sample_ask`, 250 ms deadline), a held game runs one frame and reads again; if the snapshot is still old (a
title, a pause or a load: no Link update runs), every Link member except `scene` is `null` and `snapshot_stale` is
`true` (`snapshot_present` stays so you can see how old). A game held since its start has no sample at all (no
`snapshot_present`): a `state` with no sample runs one frame to take it the same way (the first on a fresh hold, and every one at a title or a file select until Link is in play), and if none comes the members are
`null` with `snapshot_stale` `false`. `not_ready` is `null` when the sample is fresh, else the reason the Link members
are `null` (no answer from the control bridge, no sample yet, or an old one): a `null` is never silent. Treat `null` as "do not know", never as "arrived" or
"did not move". The `input`, `warp` and `checkpoint` states follow the same rule. `snapshot_present` is Longshot's present count when it was taken. `control` is false when Longshot's bridge did not answer (those fields are then `null`). `equips`, `inventory` and `coop` come from the module. A field it cannot read is `null`. An empty `equips` slot (no mask, no shield) is `null` too, never `""`.
`room` is the dungeon room Link stands in, guessed from the puppet pack's rooms table (`coop_rooms.txt`, the same guess
the Fairy and the joiner use: the room of the nearest placement, `lichii/hook/coop_rooms.h`) at the Link sample's
`pos`; it is a guess, not the game's own room number, and `null` outside a dungeon, without a pack, with no placement
near, or when the sample is old. `body` is `"child"` or `"adult"` from the name of our own Link actor in the world list
(`Player_Child`, `Player_Adult`): it follows the actor the game made, so it is right at once after an area change,
where Longshot's `age` can lag. It is `null` without a live world list (a load). `view` is the game's camera of the
last drawn frame (the renderer's `clip_from_world`, the matrix `move` steers by): `{"yaw", "pitch", "clip", "y_down"}`,
the yaw in the `facing` convention, the pitch in degrees up (negative: looking down), `clip` the 16 numbers row-major
(clip = m * [x y z 1]; a point is on the screen at x/w, y/w, -1 to 1, with y flipped when `y_down`), so a tester can
aim the way a player does: turn the view until the target is under the reticle. `null` with no camera (a title, a
load). `coop_menu` is where co-op's own menu is (`"closed"`, or the page's name such as
`"customize"`). It does not see the game's own pause screen: `"closed"` says only that co-op's menu is shut, so a
tester who pressed Start reads the screen (`shot`), not this.

**`map_icons`** (in `state`; `hookshot/src/coop_map_drawn.h`): what co-op drew on the maps in the last present,
because the others' map arrows and names are drawn through Longshot's overlay (`coop_draw.cpp` DrawMap) and a bridge
shot leaves them out. `view` is `"minimap"` (the HUD minimap), `"page"` (the Map page's picture) or `"none"` (no map
drawn); `presents` counts the presents that wrote it (it grows while the game draws); `width` and `height` are the
present's size. `icons`: one per other player drawn, each `{"number", "x", "y", "turn", "arrow", "clamped", "alpha",
"floor_delta", "label"}`: the centre in present pixels (y down), the arrow's turn in radians clockwise (`null` with no
facing known yet), `arrow` true when the game's map arrow was drawn (false: the name alone), `clamped` at the map's
edge, the alpha drawn (0..1), their floor minus the one shown, and the words under it. Our own Link is the game's
own arrow and is not listed. A test of something a player sees still needs a window shot with the overlay.

**`menu`** (in `state`; `hookshot/src/coop_menu_state.h`): what co-op's menus show, as text, because a bridge shot is
taken before Longshot draws its overlays and so never has co-op's pages on it. Three members:

- `coop`: co-op's tab of Longshot's Insert menu. `{"open":false}` while it is shut, else `"tab":"multiplayer"`, `page`
  (the same word as `coop_menu`), `rows` (each `{"name", "kind"}`; `kind` is `action`, `choice`, `toggle` (with `on`),
  `heading` or `note`; `"fixed":true` on a row that cannot be used now), `focused` (the row's index, or `null`),
  `focused_name` and `typing`. Only the rows' names are given, never their values: a value can be a join code, an
  address or a password.
- `customize_screen`: Character Select's Customize screen while co-op drives it. `{"open":false}` when it is not open;
  `"driven":false` while co-op waits for its panes or has let go of it (no rows then). Driven: `title` (as the screen
  shows it, e.g. `"Eyes (page 1 of 3)  Y: page  A: exact hue"`), `options` (each `{"option", "label", "value"}`, his
  six places 0..5, left column top to bottom then the right; empty places are left out), `focused`, `focused_label`,
  `mixer` (the any-colour mixer is open) and `page_no` and `pages` (`0` and `0` for a character with no colour option).
  A character's Eye Colour row is left out when its model is known to have no iris.
- `minus`: co-op's tabs of the game's - menu (Multiplayer, Lobby, Mods) as co-op last drew them. `{"open":false}`
  while none of them shows, else `tab` (`multiplayer`, `lobby`, `mods`), `view` (the rows' view: `main`, `session`,
  `players`, `join`, `lan`, `menu`, ...), `details_page` and `details_pages` of the list on the right of the main and
  Session views (row 223: page `0` the session's lines, page `1` the key items, turned with ZL and ZR; `details_pages`
  is `2` while the game's key items can be read, `1` when not, `0` on a view without that list) and `key_items`: on
  page `1` its lines (each `{"name", "value"}`, e.g. `{"name":"Songs","value":"3 of 12"}`), else `null`. The
  session's page is never given: it can hold a join code or an address.

The labels and values are co-op's own words in the game's language, never text read from the game. Example, a
Character Select character with irises:
`"menu":{"coop":{"open":false},"customize_screen":{"open":true,"driven":true,"title":"Eyes (page 1 of 1)  A: exact hue","options":[{"option":0,"label":"Eye Colour","value":"Game's own"}],"focused":0,"focused_label":"Eye Colour","mixer":false,"page_no":1,"pages":1}}`.

**`map_native`** (the bridge adds it to `state`; `null` until the module first published it): the
other players on the game's own map icons (hookshot/src/coop_map_icons.h `State`, #255 item 8). `up` (the pack's
copies of `Pa_MapIcon_00` found), `why` (what is missing when not), `table` (the map layout's pane table found),
`turn_at` (the pane word the arrow's turn is written to, kPaneTurn), `game_arrow` and `game_arrow_words` (the game's own
icon part's arrow `N_PlayerRotate_00`, words +0x60..+0x7c), and `shown`: per copy shown its `copy` number, `x` / `y`
(the pane's place in the map clip's frame), `turn` (read back from the pane) and `wrote_turn` (what was written),
`name` and `name_set` (the game's path setter took the label's name). While the copies are not stepped (no turn
place known, solo, nobody else on the map) it is the turn probe's instead (`Icons::ProbeArrow`, every 30 frames,
solo too): `probe` true, nothing up or shown, `game_arrow_words` and `run_arrow_words` (the same words of up to 4 of the
game's run-time icon copies under the map screen), every pane looked up fresh (tools/autotest/ingame/test_map_turn_probe.py).
Test-only: nothing here changes what is shown.

**`slow_steps`** (the bridge adds it to `state`): `party_hud` and `name_tags`, each the last step of that part that ran
a second or more after the one before (a frame stepped after the game was held that long, a slow PC, a hitch):
`count` such steps so far (0 none), `gap_ms` its gap, `shown` the other players' blocks on the game's HUD or the name
tags on the game's balloons at that step (hookshot/src/coop_hookshot.h `SlowStep`). Before hud-fresh-0121 such a step
showed 0 of both (its readings counted stale by the wall clock alone). Test-only: nothing here changes what is shown
(tools/autotest/ingame/test_stepped_hud.py).

**`world`** (the bridge adds it to `state` and to a `dump`): the actors the object walk sees, nearest to our Link first,
the kinds that matter (player, enemy, npc, chest, door, switch, item) before `other` (grass, pots, ...). The walk
reads the place of every actor it sees and keeps up to 200, `other` last, because the sync checks need grass, bushes and
beehives too. `state` lists only the nearest 40 of the known kinds and counts the rest (`total`, `other_count`); `dump`
lists all of them. In a session the sharing walk makes it. In solo play a light walk of its own makes it (names and
places only, every 100 ms, only while the bridge is on): `solo` is `true` then, `owner` is `null`, and a `kill` or
`hit` with an id is refused (strikes run in the sharing walk only).

**`strike`** (in `state`, after a `kill` or `hit`): `{"seq":4,"result":"done","name":"Enemy_Example","id":2147483648}`, the objects walk's answer to the strike with that `seq` (the `strike_seq` of the `act` reply). `result` is `done` (the strike was made), `gone` (an id was given and no actor with that id and that name is in the walk's list now: nothing else is struck, there is no fallback to another actor of the name), `not shared` (the actor is in `get_world`'s list, but co-op does not share it, as with enemy shots and the Scrub Brothers: the walk does not see it as a target, so nothing is struck; this is not `gone`. One exception: a `kill` of a Gold Skulltula in Not shared mode (race, docs/world.md) is `done` on this game's own Skulltula, in this game only, since there it is each game's own enemy; the log line ends `this game's own (not shared)`), or `none with a life` (no actor to strike). The queue holds 16 strikes: a 17th is refused with "the strike queue is full" and has no `strike_seq`. It is absent until a strike was sent. Only the last strike is kept: a newer one replaces it.

**An empty `actors` list does not mean "nothing here".** Each empty list carries a `why`:

- `"no co-op session: the world list is read only while connected and sharing"`: no walk ran yet (a title, or a
  module with object sharing off).
- `"stale: no walk for N ms (a load, or the session ended)"`: the last list is older than 1.5 s of wall clock and the game is 30 or more frames past it.
- `"empty: the walk ran and saw no actors"`: connected, and the room really has none.
- `"only other kinds near: dump lists them"`: `state` lists known kinds only, and the room has only grass, pots and the like.

```
"world":{"frame":5400,"age_ms":40,"stale":false,"behind":false,"cost_us":310,"solo":false,"total":31,"other_count":30,"count":1,"actors":[
  {"id":2147483648,"addr":"0x80000000","name":"Enemy_Example","kind":"enemy","pos":[1.5,0.0,-3.2],"dist":4.0,"life":2,"max_life":4,
   "owner":"server","alive":true}]}
```

- `id` is the actor's address in decimal and `addr` the same in hex, as the `LICHII_COOP_DEBUG_OBJECTS` log prints it.
- `owner` is `"server"`, a player number, or `null` (the sharing does not know the actor). `life`, `max_life` and
  `alive` are read for enemies only; for any other kind they are `null` (unknown, not dead).
- `frame` is the bridge's frame count when the walk began; the reply's own `frame` is the game's now. While the game
  is held, `state` waits up to 300 ms for a snapshot of that frame, but only when a fresh one is behind the frame (solo
  or stale: no wait); if none comes, `behind` is true and the older list is returned with its frame. A running game returns the newest list, which is a few frames old.
- A snapshot older than 1.5 s of wall clock **and** 30 or more frames behind the game (a load, a closed room, the
  walk stopped) is not shown: `stale` is true, `count` 0, `actors` empty, `why` says "stale", `age_ms` says how old.
  The walk runs on presents, and a held game makes none, so a held game keeps its last list however long the tester
  waits: only the frames count then.
- The list costs the object walk `cost_us` microseconds about every 100 ms. Without the bridge (the switch is never
  turned on) the walk does two relaxed atomic loads (before and after the actor loop) and the `state` text has no `world` member.

Walking: use `move` with the world yaw (`atan2(dx, dz)` in degrees). The stick is camera-relative, and the module
turns the yaw into it with each frame's own camera, so the client never guesses the stick's sign or offset. `input`'s
`stick` is the raw stick for anything else (aiming, menus).

## On a test lane: the lock and the watchdog

`live.py start --lane N` takes the lane's game pair (`<kit root>\ls\laneN-A` / `-B`), its server port and its control
pipes (`coop-laneN-A` / `-B`) from `tools/autotest/lanes.py`, and holds the lane's lock (`laneN.lock`) for the whole
session: the games stay open after the boot, so the lock must too. A busy lane refuses the session at once
(`--lock-wait S` waits for it). The lock is held by the session's watchdog (`live.py watchdog`, a process of its own
that `start` starts), under the watchdog's own pid. The watchdog ends the session when:

- `live.py stop` asks;
- the maximum duration is reached (`--max-minutes`, 30 by default: a lane slot, not an evening; 0 means no limit);
- no tester command came for `--idle-minutes` (10 by default, 0 means never; counted from the end of the boot): the tester handed
  off, crashed or paused. Only tool calls count, not time spent thinking or writing notes: a tester that writes long
- a game ended by itself (asked of the process itself: on Windows its handle, not `tasklist`; a check that cannot tell, on a busy PC, counts as running, so a slow answer never ends a live session or frees a live lock);
- a game ended by itself;
- the watchdog is stopped by a signal, or fails.

Then it asks both games to quit, kills what is still running 15 s later and the server, and lets the lock go;
`watchdog.json` in the session folder says why and what happened. Commands on an ended session are refused. A
watchdog killed outright cannot clean up, but the lock names its pid, so the lock is stale as soon as it is gone.
Without `--lane` the same watchdog runs (the end rules and the clean-up), with no lock.

The lane rules still hold: one game run at a time on the lane, only in a slot the coordinator gave, no local builds
or tests while it runs.

## Rules for a tester

- With the play-test bridge on ([Bridge protocol](#bridge-protocol)) the games are held between calls: nothing
  happens unless you act. Without it (the older `act` path) they run in real time between steps: put the moves that
  must happen together in one `act`.
- A `warp` asked while a talk is open waits for that event (a sign is likely the same, not tried): the game takes the
  scene request only after the event ends, so the verb answers `timeout` (held, `event` still true) meanwhile, and no
  press reaches the game during that call. End the talk first (press A or B until `state.event` is false), then warp.
  Seen in a two-game run: a Market soldier's talk, a `warp` with `timeout` 40 and another with 60 both gave `timeout`;
  after about ten rounds of A and B, sent between calls, the game took the request and Link arrived with an arrival
  event (about 200 frames, a fresh Link sample every frame).
- Set a test up with `place` and `setflag`; do not walk across Hyrule to reach it. `warp` in free play goes by Longshot's teleport (maps of its list, no entrance); with an entrance or a map outside the list, and no event running, it answers `not_in_event`: restart the session with a boot script that warps there. `warp` takes a real scene name (`Market_Child`, not `Market`); an unknown one is refused with an `unknown scene` line and nothing is written ([testing.md](testing.md)).
- The pause menu's Key Items and Threads of Time tabs read `???` until the save has a key item and the owl's flag
  (`UI_IsFinishChallengeMenuOpenEvent`): that is the game's own masked tab ([game-ui.md](game-ui.md), "The tab strip"),
  not missing text. Set the flag, and `give` `PouchData.Item_PocketEgg.StockNum`, `Item_OddMushroom`, `Item_Stone_Forest`,
  `Item_Ocarina_Fairy` and `Item_Medal_Forest` (all five, the same `PouchData.<item>.StockNum` form) first when a test needs the
  tab names. Seen in a two-game run: with all five the Key Items tab is named, and the Zelda's Letter alone leaves it `???`
  (so the Letter is not a Key Items item for the tab). Which of the five is enough was not tried one by one. In a session the flag is shared, so the other game
  follows.
- Take a `checkpoint` on both games after every action that should sync, then run `desync_check`.
- Use `state` to look; `shot` only when the picture matters (a puppet in the wrong place, a door drawn open). A
  checkpoint takes its own shots: anything visible is proven by them, never by flags or log lines alone.
- A report says: the scenario, the steps, what was expected, what happened, and the first checkpoint that differs.
- A test run reports; it does not change the mod. Fixing is a separate session, so a run cannot quietly hide what it
  was meant to catch.

## Proven (2026-11-22)

Both test copies booted into the Forest Temple and left open (`live.py start`); `state` read both games (map, place,
life, rupees, age); `do A "goto ..."` walked A to within 0.4 m of the point; `checkpoint` wrote 1204 shared flags
from each game; `setflag ForestTemple_Room18_Picture_Shot 1` in A alone was 1 in both by the next checkpoint, and
`desync_check` said every checkpoint agreed. The MCP server answered `initialize`, `tools/list` and real `state` and
`desync_check` calls against the running games.

## Not there yet

- **Frame step in a game.** The play-test bridge holds the game in the module ([Bridge protocol](#bridge-protocol));
  that is not proven in a game yet. `tools/autotest/bridge_proof.py` on a test lane checks it first.
- **World list and the actor compare in a game.** `get_world`, `get_actor` and the actor compare of `desync_check` are
  built and unit-tested. None has run in a game: the list needs a lane run (connected games, an enemy in the room), and
  the compare's numbers (0.2 m, 40 m, 60 frames) and the helpers' limits are guesses until then.
- **More than two games** (the runner starts A and B).
- **Lag on purpose.** The runner's `--test-delay-ms` and `--test-jitter-ms` give the server a delay; a live session
  can pass them through `start` once it takes them.

## Scenarios worth running first

- Both players open the same chest in the same second.
- One player picks up an item while the other changes scene.
- Both players hit the same switch, or kill the last enemy of a room.
- One player disconnects in a dungeon and comes back.
- One player saves and reloads while the other plays on.
- The same flag set by both players with the server's delay on.
- Child and adult Link in different eras at once.
- Tonight's survey fixes ([boss-multiplayer.md](boss-multiplayer.md), "Whole-game survey"): the Forest Temple
  basement floor turned by one player, a Lizalfos pair's owner, a Like Like grab while another player is near.

## Playing through the bridge: one MCP server per game

With the play-test bridge on (see "Bridge protocol"), each game gets its own MCP server,
`tools/autotest/mcp_playtest.py --player pN`. The server keeps one bridge connection open for as long as it runs. It
holds the game at its first call, so **the game is paused between calls**: it runs only the frames a call asks for.
When the server exits, the connection closes and the game runs again (so does a crashed tester's). Standard library
only, no `pip install`.

| Tool | What it does |
| --- | --- |
| `get_state` | Scene, room, Link's position `[x, y, z]` in metres, facing and camera yaw (0 = +z, 90 = +x), life, rupees, age, equips, co-op status, event, loading, A prompt. No frame passes. The bridge's `state` carries the whole inventory (about 290 entries on a full save, 10 to 12 thousand tokens) and the nearest 40 actors; **the MCP layer leaves both out of every reply** (`get_state`, `warp`, `step`, `press`, `place`, `walk_to`, `use_item`, ...): the state has `inventory_count` (how many entries are set), and `get_state` with `inventory` true adds `inventory` (the non-zero entries). Ask once, not at every step. The actors are `get_world`. The MCP tool adds words beside the numbers: `life_word` (full, ok above half, low at half or less, critical at a quarter or less), `enemy_near` (living enemies within 10 m) and `danger` (one within 4 m). The last two are null, never 0 or false, when there is no world list; `enemy_near_at_least` is true when the 40 listed actors were cut inside 10 m (the count is then a lower bound) |
| `get_world [kind] [within_m] [limit]` | The actors near Link from the bridge's world list, nearest first: `id`, `addr`, `name`, `kind`, `pos`, `dist`, `life`, `max_life`, `owner` (`"server"`, a player number, or null = unknown), `alive`. It reads the whole list (up to 200) through a `dump` under the tag `world_query`, not the 40 of `state`. `kind` is one of player, enemy, npc, chest, door, switch, item, other; `within_m` is a distance in metres; `limit` is 1 to 200 (50 by default). The reply has `matched`, `of` and `truncated`, and keeps the list's `why`: **an empty list with a `why` is not "nothing here"** (solo, stale, or no walk yet). No frame passes |
| `look` | ONE call for the whole situation, as short text (no picture, about 20 to 40 lines): scene, Link (age, life, place, facing, sword, shield, ammo), doors, enemies (life, metres, world yaw, ahead/left/right/behind), chests, switches, items, NPCs within 25 m, a running event, what A and B would do, and what CHANGED since the last `look` of this game. It reads one state, so it costs one bridge call. The text of a text box and the C buttons are not readable yet (it says so) |
| `wait_until what` | Runs the game in chunks of 8 frames until `what` happens: `event_over`, `scene_changed`, `enemy_dead` (with `id`), `enemies_clear` (none alive within 10 m), `on_ground` (height steady for two looks), `prompt`, or `condition` (as `step_until`). Ends early, with `stopped` saying why, when Link is hurt, dies, a scene loads or an event starts (unless that is the wait). An enemy id that was never listed is "not met", never "dead". Reply: `met`, `frames_run`, `stopped`, `why`, and a `look` |
| `go_to` | Walk and end with a `look`. ONE of `place` (a position name from the scene's file in `tools/playtest/data`; the recorded routes are walked from the named position nearest to Link within 6 m, one way, start to end; no chain: a straight line, said in `note`), `id` + `name` (an actor, as `go_to_actor`) or `x` + `z`. Stops early and says why: scene change (through a door is the way out), load, event, hurt, stuck (with the recorded stuck-spot note and fix). It does not climb, jump or read the game's collision: `climb`, `jump` and `push` do those |
| `shoot item` | Aims the way a player does. Takes out `item` (slingshot, bow, boomerang, hookshot, longshot, bomb, nut) with `assign` (ZR), turns Link to the target (`id` + `name`, or a point `at` [x, y, z]) (with the stick) and taps ZL (the camera goes behind him; drawing turns Link to the camera), holds ZR 60 frames to draw, then turns the view with a stick until the target sits under the reticle (screen x 0, y +0.028: about pixel 640,350 of 720p), read from the state's `view`. It first pushes each axis of the right stick, then the left, a little to learn which one turns the view and which way; each turn after that pushes the stick by how far the target is from the reticle (40 turns of 2 frames at most). The game's aim view eases in and coasts on after the stick, so each look (the probes too) first waits with ZR held and no stick until the view stops, drifts steadily, or 40 frames passed; a coast still slowing is counted to where it will rest. 40 turns can take about 1700 frames. It holds 30 frames, checks the target is still under the reticle (not with `tilt_frames`: a hand tilt moves it off on purpose, so that shot is not checked), lets go and watches the target for `watch_frames` (40). `tilt_frames` (+ up, - down) tilts by hand after the aim, for an arc. `result`: `hit` (its life fell, it died or left the list), `changed` (another field it shows), `miss` (the ammo went down, the target did not change), `fired` (a point, or no ammo count), `not fired` (no ammo, a help box opened while drawing, the ammo did not move), `not aimed` (no view, neither stick turned it, the target left the screen, not reached in 40 turns, or it drifted off before the let-go: B puts the item away, no shot) or a stop. `aim` has the world yaw and pitch from Link's feet to the target, the `stick` that turned the view, its `gains`, `error_start`, the `steps` and the last `error` (screen x, y from the reticle). `assist` adds the test `aim X Y Z` line: it only nudges the last bit. When the arrow or seed in Link's hands leaves him (also one made before the line; one more than 2 m from Link, a shot from before, is not taken) and its flight points within 4 degrees of the target and passes within 0.5 m of it, it is moved sideways onto the line through the target along its own flight; its way and speed stay the game's. Farther off it is left alone and the log says how far off (`not put (... flew N degrees, M m off it ...)`) |
| `climb [up] [to_y] [face_x face_z]` | The stick up (or down) a look every 8 frames until the height reaches `to_y`, or with no `to_y` until it stops (the top or the bottom). `face_x`/`face_z` turn Link to the wall first with the stick; going down with an A prompt presses A first. `result`: `arrived`, `stopped`, `stuck` (the height did not move for 4 looks), `not climbing` (Link moved across instead: not on the wall), or a stop (hurt, event, scene change, timed out). `trace` is [frames, height] |
| `push id name distance_m [yaw]` | Walks to the block's far side from the push (1.2 m from its centre), turns to it with the stick, leans on it until A offers something (the game's "Grab", 24 frames at most), holds A 30 frames to grab it (a short tap climbs onto the block instead), then pushes at full stick with A still held (a frame without A lets go of the block; the stick is worked out from the state's `view` by the bridge's own `move` rule) until the block moved `distance_m` along `yaw` (none: from Link toward it). `grab` says what A offered, whether it was held and the prompt after. `result`: `pushed`, `stuck` (it did not move for 48 frames: blocked, at its end, not a block that moves; or Link went up onto it), `slipped` (Link slid off its side), `stopped` (a move was refused, such as no camera, or after the grab there is no camera view in the state for the stick; no frame ran), `not reached`, `gone`, or a stop. Needs the world list |
| `jump x z [land_y] [tolerance]` | Runs straight at the point at full stick, no side steps, so Link jumps off the edge by himself (a run off a ledge jumps, a walk drops). `result`: `landed`, `fell` (on the ground more than 1 m below `land_y`), `respawned` (put back more than 3 m: a pit, a void), `stuck` (no nearer for 4 looks on the ground; a long fall that carries Link past the point is not counted), `stopped` (a move was refused, such as no camera; no frame ran), or a stop. On the ground means two looks in a row with a steady height; within `tolerance` of the point and down (one steady look, or within 0.15 m of `land_y`) the stick lets go, so Link stops there and does not run on past it; over a gap it stays full. `airborne` says if the height moved on the way |
| `record_route action` | `start` samples Link's place from every reply of this game (a point every 2 m, at most 40, same scene only); `save` with `name` (lowercase words with hyphens) and `note` writes the points as route NAME in `tools/playtest/data/<scene>.json` (points NAME-1, NAME-2 ...; the file must pass `kit_library.py check`); `cancel` drops it. Every `move`, `step` and `press` reply of this game while recording adds Link's place, whichever tool walked, so the corners of a walk are kept. Only places Link walked through are saved. A replay and `save_script` leave it out |
| `fight id name [max_frames] [lock] [sword] [shield] [min_life]` | Fight ONE enemy and end with a `look`: walk to sword reach, then slash with the lock-on held (`ZL`+`Y` for 4 frames, then `Y` let go for 8, `ZL` still held), guard for one round after Link is hit (`ZL` held alone: the shield), repeat. `ZL` stays held from one round to the next (each round is one `input`, with no `step` between that would let it go); only a walk up lets it go. Ends as `dead` (its life seen at 0), `gone` (left the list: perhaps dead, the look says), `low` (Link at `min_life` quarter hearts or less, default 4: he backs off and the fight ends), `stopped` (scene change, load, event, stale list, a walk-up the game refused, or `stuck`: four walk-ups in a row that got no 0.3 m nearer; it never says `gone` then, and it presses nothing during an event: wait a cutscene out first, for example `step_until "event == false"`) or `alive` (`max_frames` used: 600 by default, 1200 at most; no round starts that would pass it, a back-off when low adds 14; `hint` when its life never moved). Also `hits`, `hurt`, `slashes`, `guards`, `rounds`. The buttons are Longshot's own map (its `host/nn_hid.c`: the mouse's left button is `Y`, attack; its right button is `ZL`, shield and lock-on; a logged Longshot run held the shield with `ZL`): lock-on `ZL`, sword `Y`, shield `ZL`. Longshot's Toggle ZL/ZR setting must be off (it latches `ZL` on one press). The enemy's life is the world list's `life` (the objects walk's Life read, `kComponentLife` in [addresses.md](addresses.md)). Gohma takes the sword only in her eye while stunned: a Deku Seed first (`use_item`), then `fight` |
| `get_actor id` | One actor by `id` (the number `get_world` gives) or by `0x...` address. `found` is false with a `why` when it is gone, outside the 200 nearest, or there is no list |
| `walk_to x z [tolerance] [max_frames]` | Walks to the point 8 frames at a time with the bridge's `move` (the world yaw toward the target; the module makes the stick from each frame's camera), stopping on arrival (0.5 m by default), a scene change, a load, an event, a refused move (`move refused: no_camera: ...`) or `max_frames` (600). It says why it stopped, the frames used and the distance left, and keeps `trace` (the last 24 pushes: `[frames, x, z, distance, world yaw, strength, stick]`, the stick being the `[angle, strength]` the module made). A walk that gets nowhere for three pushes, or farther from the target for three pushes in a row, steps aside 90 degrees for three pushes, the other side each time; after three side steps for getting farther it stops with `moving away` instead of walking on |
| `press [buttons] [stick_angle stick_strength] [frames]` | Holds the buttons (and the stick: 0 = up, away from the camera, 90 = right) for N frames (2 by default), then holds the game |
| `step frames` | Lets N frames run with no input |
| `step_until condition [max_frames] [chunk]` | Steps `chunk` frames at a time (8 by default, 1 to 60) until a state field matches, then holds. `condition` is `field`, `!field` or `field OP value` (OP: `==` `!=` `<` `<=` `>` `>=`; the value is JSON or a bare word; a dotted path reaches into objects and lists: `equips.weapon`, `pos.1`): `event == false`, `loading == false`, `scene == KokiriForest_Child`, `prompt`, `life == 0`, `rupees >= 50`. It looks first (a condition that holds costs no frame) and stops after `max_frames` (300 by default, 3600 at most). Reply: `met`, `value` (the field now), `frames_run`, the last `state`, and `why` when not met. A field the state does not have is an error (a typo is not a time-out). A null (a stale Link snapshot) is "do not know": it never matches an ordering, and `== null` matches it. It replaces a `step`, `get_state`, `step` loop ("Kit helpers") |
| `warp scene [entrance] [timeout]` | The game's own scene change: by the module's request during an event, in free play by Longshot's teleport (a map of its list, no entrance) or, with an entrance or for a boss stage such as `GohmaStage`, by the module's map change event (`via` says which; `not_in_event` with the reason if that event is off). It returns once Link is there with no event or load; `timeout` is 1 to 600 seconds (default 60) |
| `setup` (hearts, max_hearts, magic, rupees, small_keys, boss_keys, maps, compasses, items, flags, age, warp, place) | A whole test start in one call: the spec's parts run in order (items, magic, rupees, keys, flags, max life, life, then the warp, then the place), one answer per step; it stops at the first refused call and says which. Hearts are written as quarter hearts (`PlayerStatus.Life`, `MaxLife`: the game may write `MaxLife` back, [items.md](items.md)); magic is `give Item_FairyBless_MP` (and `_MPDouble`); keys are `DungeonInfo.<Dungeon>.SmallKeyNum` / `IsGetBossKey`. Code: `tools/autotest/kit_setup.py` (the same spec is a preset file's body). Needs the verb gate. Names only, nothing from the game. Unproven in a game |
| `preset name` | `setup` from a named, versioned data file in `tools/playtest/data/presets/` (`deku-start`, `deku-before-gohma`, `before-dodongo`, `before-barinade`, `before-phantom-ganon`, `before-volvagia`, `before-morpha`, `before-bongo-bongo`, `before-twinrova`, `before-ganondorf`, `before-ganon`). A file is `{preset, version: 1, title, spec, notes}`; `spec` is the `setup` spec. `deku-before-gohma` warps into the Deku Tree and places Link in the corridor before Gohma's door (`place`); walk through the door so the game's own trigger and intro make Gohma (a free-play warp into the rush room `GohmaStage` left both games black with no control). The seven other boss presets do the same in their dungeon (the entrance, the boss key, the intro's played flag and the boss's quest step and Bools put back), then `place` Link at the boss room's arrival point from [dungeons-boss.md](dungeons-boss.md); the place before each boss door is not in the repository and is set once a lane run has measured it. `before-ganon` loads the final battle field. A preset's `age` (`child` or `adult`) is set with `Sequence_IsChildhood` and 30 frames before the warp, so a save of either age works. A `before-<boss>` preset makes the boss alive the way the host-reset put-back does ([dungeons-boss.md](dungeons-boss.md), the boss set table): the shared quest goes back to the step of the fight, and the boss's Bools to their before values. Unproven in a game |
| `save_point tag` / `load_point tag` | A test point to retry a fight: `save_point` is a `checkpoint` (shared flags plus Link's scene, place and life); `load_point` writes every flag that differs first, then loads the scene again (also when Link is already in it: a room reads its boss state when it loads, so a beaten room comes back only by a load), places Link, sets his life and dumps again; `reloaded` is false when the scene did not change and Link did not move after the warp (no load could be told); `exact` is true only when reloaded and the second dump equals the saved one, `left` names what still differs. The flags `dump` lists are saved from the dump. The names the LAST `setup` or `preset` of this server wrote that the dump does not list (a boss quest, a boss key) are read by `save_point` with `flag_read` and kept under the tag at their values at save time (`setup_flags`; a name the game has no flag for is in `setup_flags_unread`). `load_point` writes them first and reads them again at the end: `setup_flags_checked` is true only when every one matches, a mismatch is in `left` and `setup_flags_left` (want and now), and `exact` needs them all. An enum read in an earlier game run is refused by `flag` (`setup_flags_refused`) and is not checked. A new server starts with none, and a name the dump lists takes the saved dump value. A scene outside Longshot's teleport list uses the map change event (see `warp`). Proved on the test lane (2026-11-23), with the old `deku-before-gohma` that warped into the rush room GohmaStage (now replaced: a later lane run found that warp black, with no control): Gohma was alive, `load_point` put Link's place and the boss key back. `reloaded` stays false while Link has not left the room's start (the 0.5 m check cannot see a load there): move first. Not proven: a retry after a real fight |
| `flag name [value]` | Sets a flag (name or `0x` hash; a number, true/false or an enum name) inside the held frame; replies with the old value |
| `give item` | Gives the item at once (the item, its `IsGet` and its ammo); the reply's `result` says what changed, and `get_state` with `inventory` true shows it with no step |
| `kill actor [id]`, `hit actor amount [id]` (`actor` is the **name**. With no `id` the game takes the nearest actor of that name with a life. With the `id` and `name` `get_world` gave, it takes exactly that actor, and answers `refused: actor 7 (NAME) is gone` when the walk's list has no actor with that id and that name; an id is refused in solo. The tool steps up to 12 frames, looking every 2, for the answer in `state.strike`: `ok`, `refused: ...`, or `unknown: no result after 12 frames (the strike may still land: look with get_world)`. For a `kill` that struck, with an id, it then waits up to `wait_frames` more frames (default 36, a guess: S8 saw one die only after Link came near it; 0 to 600) for the actor to be gone, not alive or at life 0: `ok` with `died` true. If it is still alive: `struck: the hit landed, but NAME still lives after N frames` with `died` false (a far actor may not die until Link is near it, S8). With no id, or with no world list to look in, the answer is `struck: ...` with `died` null (not checked). A `hit` is `ok` when it struck. It never retries, because a second strike could land twice) | One `act` line each, then the wait above. |
| `hurt quarter_hearts`, `place x y z [facing or face_x face_z]` (with a facing in degrees, 0 = +z, 90 = +x, or a point to look at, Link is placed and then turned by the test `face` line, 2 frames each: a chest or a talk needs no walk; both ways together are refused), `wear part name`, `dress [part]`, `assign item`, `chat text`, `menu page` | One `act` line each (the arguments are checked here first: no spaces in a name, `amount` and `quarter_hearts` 1 to 80, `part` one of weapon, shield, head, armor, boots, hand, `page` customize, close, host, paste, leave or main: main opens co-op's Multiplayer page and presses nothing). Same gate and refusal text as `act`. They take effect on the next frames: `step`, then `get_state` |
| `go_to_actor id name [near_m] [max_frames]`, `face id name`, `attack id name [button] [max_frames]`, `use_item item [button]`, `talk`, `open`, `interact id name [near_m] [max_frames]` | The helpers below: a few calls in one, each bounded to 120 frames, each saying why it stopped |
| `checkpoint tag` | The game's `dump` (every shared flag) with its frame and state, appended to `<log dir>/<player>.checkpoints.jsonl` |
| `desync_check [all] [flags_only]` | `desync_check.py --clients` on the log folder: tags matched across every game, each game's frame printed, every game's flags compared, and the games' actors compared by what [sync-coverage.md](sync-coverage.md) says about each name (below). `flags_only` leaves the actors out |
| `log [match]` | **Raw, unmasked** (do not paste it into an issue; use `check_logs` for that). The game's new `coop:` log lines since the last call (`match`: only lines with that text, the oldest 80 of them; a `match` has a cursor of its own, so the plain cursor stays and the lines it left out come with the next plain `log`, and a plain `log` read in between can show a line again that a later `match` returns). Needs `--session`: games from `live.py start`, p1 = A, p2 = B |
| `check_logs [since] [names]` | The oracle for "did anything go wrong". This game's new `coop:` lines that are flagged, **masked**. A line is flagged when it starts with the game's level prefix `warning: `, `error: ` or `critical: ` (most real warnings contain none of the four words, for example `connection lost; reconnecting`), or when it says `failed`, `error`, `refused` or `resync` (any case, also `fails`, `failure`, `refusal`, `resyncing`), except a count of zero: `(0 copies' shots refused)` in the periodic `actors:` line and `0 failed` are not refusals. Reply: `flagged` (how many), `lines` (the newest 40, each `{why, line}` with `why` = `level:warning`, `level:error`, `level:critical` or `word:<word>`; `left_out` counts the rest), `read` (all new `coop:` lines), `names_hidden` and `verdict`: `clean`, `flagged`, or `no log`. `since` is `last_check` (the default: since the last `check_logs` of this server, or the boot) or `boot`. It keeps its own place in the log (and reads only whole lines: a half-written last line waits for the next call), so it and `log` never take lines from each other. `names` (up to 20 words) are extra words to hide; the names the games print themselves (`name '...'`, `player N '...'`) are hidden without being passed. Needs `--session`; **it answers after the session ended** (that is when the last lines matter) and does not restart the idle clock. **A clean verdict is not proof**: it says only that the game printed none of these. No frame passes |
| `session_status` | Is the run still sound: `bridge` (`connected`, `held`, `last_frame`, `connects`) and, with `--session`, `session` (`running`, or `ended` with the watchdog's reason; each game's `pid` and `alive`; `max_minutes_left`, `idle_minutes_left`). It asks the game nothing, works after the session ended, and does not restart the idle clock. No frame passes |
| `pause`, `resume` | Hold the game, or let it run in real time (a person takes over; the next call holds it again) |
| `screenshot [name]` | A PNG of this game: the game saves it as `<game>/playtest/shots/<player>-<name>-<time>.png` (it runs one frame to show it); a copy goes to `<log dir>/shots/` |

**What `check_logs` masks.** IPv4 addresses (also before a full stop or a comma) and IPv6 addresses, with a port; a
host name with a port (`myhouse.duckdns.org:7777`); the user name in `C:/Users/<name>`, `/home/<name>` and
`/Users/<name>` paths; memory addresses (`0x` and six or more hex digits); hex strings of 32 or more characters; the
value after `password`, `token`, `secret` and `nickname` with `=` or `:`; the value after `name`, `host`, `server`,
`room` and `key` with `=` only (`coop: host: refused ...` is a real prefix and keeps its words); the join password;
`name '...'` and `player N '...'`; every double-quoted text; and the players' names, which are read from the games' own
logs plus the words in `names`. A name of two characters or fewer is matched with its case. It is the filter of the
tool, not a guarantee: a name that no log line printed in those forms and that the tester did not pass stays, and so
does a host name written without a port, or a dotted host name (`myhouse.duckdns.org`) with no port after it. The mask
also takes too much: `texture:2048` and `slot:1234` are masked as hosts, and `MyPC:443` (a three-digit port) is not. A host
name with no dot and a port of four or five digits (`DESKTOP-K3J9:7777`) is masked; the words `error`, `port`, `frame`
and a few like them before a colon and a number are not. A name may hold an apostrophe with a letter after it
(`O'Neil`). `check_logs` reads the `coop:` lines and the `coop voice:` lines, and a level word may come after one
more `[tag] `. Nothing from a log goes into an issue or a pull request unmasked (AGENTS.md); quote only the lines you
need.

### Helpers: several calls in one

`go_to_actor`, `face`, `attack`, `use_item`, `talk`, `open` and `interact` are in `tools/autotest/playtest_helpers.py` (the MCP
server calls them; a script can too). They only use the bridge's commands, so they add no game code. Rules:

- Only inputs play. `face` (and the turns in `attack`, `interact`, `talk`, `shoot`, `push` and `climb`) turns Link with
  light pushes of the left stick toward the point (`move` at 0.25 strength, 2 frames each, 12 pushes at most) until the
  state's `facing` is within 10 degrees; it says `turned`, the degrees left (`off`), the pushes and how far they
  stepped him (`moved_m`). None of them writes his yaw. The test `face X Z` line is kept for set-up only: `place` with a
  facing (Link is put somewhere, so it is not play either).

- Each takes the `id` and the `name` `get_world` gave, and looks the actor up in the world list again on every leg of
  the walk and before every press. When the id is not in the list any more, or has another name, the answer is `gone`:
  they never go for another actor of the name. A reused address (the game puts a new actor of the same name at the
  address of one that went) is accepted: nothing here can tell it from the same actor, so read `life` and `pos` if it
  matters.
- At most 120 frames pass in one call (`max_frames` lowers it). Every answer says why it stopped: `go_to_actor`:
  `near` (arrived), `gone`, `stuck` (Link got no 0.3 m nearer over 60 frames; `moved_m` is how far he went), `timed out`,
  `scene changed`, `loading`, `event`, `stale position: ...` (no fresh Link snapshot: the `state` Link members are null), `stale world list: ...` or `no world list: ...`. `attack`: `dead`, `gone`,
  `alive` (the frames ran out with life left; `hint` says so when its life never moved), or `stopped` with the reason
  of the walk or the fight. The scene, a load and the list are checked first, then `gone`: after a door or a load the
  list lacks the actor, and that says nothing about its death. A refusal of the bridge comes back as the reason, not as
  a crash.
- `attack` walks up, faces the actor and presses the sword button in rounds of 6 frames. The default button `Y` is
  Longshot's attack button (see `fight`); the walk's leg size and stuck limit are guesses made without a game: the first
  lane run sets them.
- `use_item` puts the item on ZR (`assign`), presses ZR (`button` overrides it; Y is the sword), and lists what changed in life, rupees, event, prompt,
  equips and each inventory entry (`inventory_changed`, one entry per item). `open` presses `A` only when the state's `prompt` says something: no prompt, or the game's `Empty`, presses nothing. `talk` (below) keeps that rule for its first press. `interact` is go_to_actor to within 1 m, `face`, then the same A rule; a walk or turn that did not get there presses nothing and says why (`not reached: ...`, `not faced: ...`).
- `talk` (with `id` and `name`, and no prompt up yet: go_to_actor and `face` first; with the prompt already up it
  does not walk) presses `A`, then reads the new texts of the message layouts through `text`: `speakers` (in order),
  `boxes` [{speaker, text, markup, raw}] (a wrap-26 text of a message layout, see [Text boxes](#text-boxes); empty
  without patch 0042), and the reply buttons (`PaMessageBtn_00`) as `choices`. A box's `text` is its words with the
  markup cut out; `raw` (only when something was cut) is the text as the game set it. A box set again as it was, before
  the next A, is one box; the same line after an A is a new box. A text of only markup or variables is no box. `speaker` is the name pane set with the box, also
  when the game sets it after the body; it is null only when the game shows no name. It presses `A` when a speaker,
  a box or buttons came, or when the event went quiet for 3 reads (10 frames each). When buttons show it takes the
  next of `reply` (a text, or a list: one per set of buttons, in order), presses Down to the button of that text
  (whole text first, then a part, case and spaces ignored) and `A`. With no reply left it picks nothing: it stops with
  `choices showed and no reply is left: ...` (the buttons listed, the talk left open) and a `since`; call `talk` again
  with `reply` and that `since` to go on (no new `A`: the same buttons are read again). A shop's item list is not
  buttons: move to the item with `press`, then `talk` with `since`.
  It ends `done` when the event is over and 3 reads bring no text, or says why it stopped: `reply not offered: ...`,
  `done, but no choices showed for ...`, `too many boxes`, `timed out` (1200 frames with nothing new, or 7200 in all: it keeps waiting while boxes
  come), `scene changed`, `loading`, or
  `no text: ...` (no `text` command, or an exe without patch 0041: one `A` pressed).
- `go_to_actor` calls an actor `gone` only after 3 reads in a row without it (one read of the list can miss an actor
  that is there).
- They act on this one game. A script can wrap two clients in `InStep`, which steps the other game by the same frames
  on every `input`, `move` and `step`. Over MCP, the `--pair` server does this for every tool (below); with one server per
  game the tester steps the other game itself.

Every reply carries the game's `frame`.

**walk_to's direction.** walk_to sends world yaws (`move`), never a stick: the module turns each into the stick with
the camera of the frame the pad is read in, so nothing is learnt and a turning camera cannot send Link the wrong way
(the earlier client-side model learnt the stick's sign and offset from how Link moved, and once walked away from its
target). The yaw-to-stick maths is unit tested for both handednesses, every quadrant, camera turns and pitch
(`tests/hookshot/playtest_test.cpp`). A walk that gets nowhere for three pushes steps aside for three. Into a wall it times out, and near a wall a detour can also trip the moving-away stop, so `moving away` can mean blocked (read the `trace`): walk_to does no
pathfinding, so give it points with a clear line between them and a boot-script warp or `place` for anything far. On a null position (stale snapshot) it stops with `stale position: ...`; it never says `arrived` from an old place. The walk that once went away from its target (Link from z 77.7 toward z 66) is what `move` replaces; the lane check is three targets in different directions on both games with the camera turned.

### Kit helpers (`tools/autotest/kit_tools.py`)

Standard library only; tested without a game by `tools/autotest/tests/test_kit_tools.py`.

- **`step_until`** (a player tool, above): `step_until condition 120` instead of a `step`, `get_state`, `step` loop.
- **Delta state.** Every `get_state` (player server) and `state` (session server, `live.py state`) reply carries a `state_id`. Pass it back as `since` and only what changed comes back: `changed` (dotted path to the new value: `pos`, `equips.shield`, `coop.players`; a list is one value), `removed` (paths that are gone), the `frame`, the new `state_id` and `since`. The counters that move on every look (`frame`, `snapshot_present`, `world_frame`, `id`, `ok`, `paused`) and the `inventory` are left out of the comparison (`inventory_count` says it changed). New `coop:` log lines (`live.py state`) are always whole: they are new since the last look already. The last 16 looks are kept (in memory on the player server; `state-history.json` in the session folder for `live.py`). An id that is not kept gives the whole state and a `why`; so does a player the old look did not have. `since` must be a whole number from 1. A player server numbers its looks from its own base (p1 from 100001, p2 from 200001), so an id from one player's server is "not kept" on another's.

- **Call log and replay.** Every kit call is written as one JSON line to `calls.jsonl`: `{"t", "server", "tool", "args", "ok", "error"}` (`t` is when the call began, `server` is `oot-p1`..`oot-p4`, `oot-pair` (`--pair`, each call with its `player`), `oot-playtest`, or `live` for a `live.py` command, which is logged under its MCP tool name: `do` is `act`). Where: the session folder when the server has `--session`, else its `--log-dir`; `--call-log FILE` picks another file, `--no-call-log` turns it off. All servers of a run append to the one file, a line in one write, so the lines do not mix. A log that cannot be written never stops a test. Run it again:

  `python -I tools/autotest/live.py replay [FILE ...] --session DIR [--lane N] [--log-dir DIR] [--dry-run] [--keep-going] [--delay S] [--include-reads] [--include-failed]`

  With no FILE it reads `calls.jsonl` of `--session`; several files are merged by time. It runs the calls in order, each as the server that made it (a player server: that game's bridge, on `--lane`'s ports; `oot-playtest` and `live`: the session's tools, so those need `--session`). Calls that only looked (`get_state`, `get_world`, `log`, `screenshot`, ...) and calls that failed the first time are left out unless you ask; it stops at the first call that fails now unless `--keep-going`; `--dry-run` lists what it would run. The calls are the same, the game is not: a replay starts from the state the first run started from (a save, the same warp), and a `step_until` or a walk may land on other frames. It is not logged again. Exit code 1 when a call failed; the progress goes to stderr, the result (`ran`, `skipped`, `failed`) to stdout.

### Kit doctor (`tools/autotest/kit_doctor.py`)

Run it before a run (and when a run does not start): one line per check, `OK`, `WARN` or `FAIL`, the name and what is wrong. Exit code 1 when a line says `FAIL`. It starts nothing and changes nothing.

`python -I tools/autotest/kit_doctor.py --lane 1 [--session DIR] [--mcp .mcp.json] [--running] [--players N] [--json]`

(`--game-a FOLDER --game-b FOLDER` instead of `--lane` for two folders of your own.) It checks:

- **python**: 3.10 or newer. **kit files**: `tools/autotest` has the play-test scripts.
- **game A / game B**: the folder exists, has `longshot.exe` (in `bin/` or the folder), and `hookshot_coop.dll` next to it (the fix is `lanes.py install --lane N --dll ...`); a `WARN` when the shaders are not prepared yet. **game copies**: A and B are two folders.
- **port p1, p2 ...**: the bridge port (5000 + N, or 5000 + 10*L + N on lane L). Before a run it must be free (a `WARN` names what is taken); with `--running` it must answer (a `FAIL` says no game listens: it did not start, was not started with `--playtest`, or its bridge is off). **port server**: the lane's server ports are free.
- **session** (`--session DIR`): the folder is a `live.py start` session, has not ended, and each game process in `pids.json` is alive.
- **mcp** (`--mcp FILE`, or `./.mcp.json` when there is one): each `oot-pN` server names a command that runs, an `mcp_playtest.py` that exists (a relative path is read from the folder of the `.mcp.json`), its own `--player`, the `--lane` you gave, a `--session` folder that exists; all servers share one `--log-dir`.

`python -I tools/autotest/kit_doctor.py --write-mcp .mcp.json --lane 1 --session DIR --log-dir DIR` writes a `.mcp.json` from `tools/autotest/mcp.json.template` (`{LANE}`, `{SESSION}` and `{LOG_DIR}` filled in; an option you do not give is left out, and so is the `oot-playtest` session server when there is no `--session`). It never replaces a file without `--force`. The template has no paths of this PC: the script paths are relative to the repository folder, where the client runs. Register the file as in "Setting up a run".

### Check in game (first lane run of the strike by id, the helpers and the held list)

Nothing here has run in a game. The first run checks:

- Hold the game (any `state`), wait 3 s, run `get_world`: the list is still there and `stale` is false. Then `kill`
  with an `id` and `name` from it: `ok` (the strike landed and the actor was then seen dead, `died` true) or `struck: ...
  still lives` (it was hit and did not die: far actors may not die until Link is near; note which). (A list that went
  stale in a held game, or a strike that says `gone`, fails this.)
- `get_state` on a held game right after `step`: `snapshot_present` is within 4 frames of `frame`, pos matches the world list's
  Link, and `walk_to` to a point 5 m away stops near it (not `arrived` at 0 frames). In a title or pause the Link members are
  null and `snapshot_stale` is true.
- `warp` in free play with an entrance (or to a boss stage) uses the module's map change event (`via` "map_event"; written 2026-11-19, unproven in a game until a lane run). `warp` to a map of Longshot's list moves Link (unproven: the "via teleport" path), and with `timeout` 5 it never waits longer than about 5 s.
- `warp` during the save's arrival event (title just left): the module's request is taken, or `failed: ... refused: an event is playing`; an event in which Link's update does not run gives `not_in_play` even though the event request would work (not seen in a game yet). A `warp` treats a missing Link sample like an old one (the typed `event`, `loading` and `age` alone do not count), after the one frame `state` also runs.
- `kill` with an id of an enemy shot or a Scrub Brother in the list: `refused: ... does not share it`, not `gone`.
- `kill` with the id of a normal enemy: only that enemy takes the hit, not another of the same name nearer to Link.
- Walk through a door during `go_to_actor`: the answer is `scene changed`, not `gone`.
- The guesses made without a game, set by this run: the 4 m leg of `go_to_actor` and its 60-frame stuck limit. (The
  sword button `Y` and the lock-on and shield button `ZL` are Longshot's input map: see `fight`.)

### Check in game (talk and the text wrap, patch 0041)

Needs a game built with patch 0041. One lane run so far (Kokiri shop, Mido; with the helper as it was before the
button layout was known):

- Seen: `hooked` true, texts with their layout and pane names, no crash; the speaker and the reply buttons came
  (layouts above). The Deku Shield was bought by talk (stock 0 to 1, rupees down by 40), but by the cursor's default
  picks; the helper now stops at buttons instead.
- Not seen (0041 alone): the box body. Patch 0042, lane run 2026-11-24: `body` true, each box came through wrap 26
  (see "Text boxes"), the shop was bought by `reply`, Mido's long talk was still giving boxes when the old
  1200-frame cap ended it; the boxes had raw markup, one was set twice, and the first box's
  `speaker` was null (the shop sets the name after the body; Mido's talk set none).
- Next run (helper fixed for these): `talk` to Mido with `reply` "Yes, master!" where buttons show: `boxes` with
  clean `text` and the `raw` kept, no box twice, `speaker` the name pane when the game shows one, `done` true, Link
  free to walk after. Kokiri shop: `talk` stops at the first buttons and lists them; `talk` with the `reply` texts
  as listed and `since` buys the shield.
- Also look at (helper nits, 2026-11-24): a box with an item or price shows `{Shop_SelectItemName}` and
  `{Shop_SelectItemPrice}` in `text`; two equal lines in a row are two boxes. A box's `speaker` can be the name of the
  box before when the name pane is set late: check the first box after a speaker change.

### Set a test up (start a boss or a dungeon in one call)

Do not walk to a boss or give things one by one. Four tools do it (all need the verb gate, `LICHII_COOP_PLAYTEST_VERBS=1` and `LICHII_COOP_CHEATS=1`):

| I want | Call |
|---|---|
| A named start (Deku Tree entrance, the room of a boss) | `preset name`: `deku-start`, `deku-before-gohma`, `before-dodongo`, `before-barinade`, `before-phantom-ganon`, `before-volvagia`, `before-morpha`, `before-bongo-bongo`, `before-twinrova`, `before-ganondorf`, `before-ganon` |
| My own start | `setup` with `hearts`, `max_hearts`, `magic`, `rupees`, `small_keys`, `boss_keys`, `maps`, `compasses`, `items`, `flags`, then `warp {scene, entrance}` (in free play it goes by the map change event) and `place {x, y, z}` after it (Link put at a point of the new scene) |
| To retry the fight | `save_point tag` before it, `load_point tag` after: it replies `exact` and `left` |
| Both games the same | in a pair, call `setup`, `preset` or `load_point` with no `player` (both games, one after the other; small keys, which the share carries as count deltas, are written on the first game only, so they are not added twice) |

Recipe for a boss: (1) start from a save of the age the boss needs (a preset does not set Link's age); (2) `preset before-<boss>`; (3) `save_point <boss>-start`; (4) fight; (5) `load_point <boss>-start` and check `reloaded` and `exact` are true; (6) `checkpoint` on both games to see what spread.

What reaches the other game by itself (items.md, flags.md): items, small keys (count deltas), maps, compasses, boss keys, story and dungeon flags, `MaxLife` and `MPMax`. Per player and not shared: `Life`, `MP`, rupees, ammo. A late set-up on one game is a normal flag write: the co-op watcher sends it like any other change, so a preset applied on one game moves the shared parts to the other.

Not proven in a game yet: the map change event in free play, `MaxLife` (the game may write it back, items.md), the exact restore, and that a boss preset really brings the boss back in each room (the put-back is proven for the host reset only).

### Setting up a run

1. Start the games with the bridge on, one port each: player N on `5000 + N`, or `5000 + 10*L + N` on test lane L.
   `live.py start --playtest [--lane L]` does it for its two games (A = p1, B = p2: the runner's
   `--playtest-ports BASE` sets each game's `LICHII_COOP_PLAYTEST_PORT`, `LICHII_COOP_PLAYTEST_VERBS=1` and
   `LICHII_COOP_CHEATS=1` (the gate of `act`, `warp`, `flag` and `give`);
   `HOS_CONTROL` was set already), and its answer lists the ports. Give it `--max-minutes` for a long run (30 by
   default). By hand: set `LICHII_COOP_PLAYTEST_PORT`, `HOS_CONTROL`, `LICHII_COOP_PLAYTEST_VERBS=1` and `LICHII_COOP_CHEATS=1` in each game's
   environment (or `live.py start --env A:LICHII_COOP_PLAYTEST_PORT=5011 --env B:...`). Each game's log says
   `coop: playtest: listening on 127.0.0.1:<port>`.
2. (`kit_doctor.py --write-mcp` writes the `.mcp.json` below for you; `kit_doctor.py --lane N` checks the run: "Kit doctor".)
   Register one MCP server per game, all with the **same** `--log-dir` (desync_check reads every game's log there),
   and, for games from `live.py start`, the same `--session`: each call then counts as a tester command for the
   session's watchdog (else it ends the session after `--idle-minutes`), and an ended session is refused. In a
   project `.mcp.json` (the shape most MCP clients read):

   ```json
   {"mcpServers": {
     "oot-p1": {"command": "python", "args": ["-I", "tools/autotest/mcp_playtest.py", "--player", "p1", "--lane", "1", "--session", "sessions/run1", "--log-dir", "playtest-logs/run1"]},
     "oot-p2": {"command": "python", "args": ["-I", "tools/autotest/mcp_playtest.py", "--player", "p2", "--lane", "1", "--session", "sessions/run1", "--log-dir", "playtest-logs/run1"]}
   }}
   ```

   The same with the `claude` command, one line per game (run from the repository folder; `--scope project` writes
   `.mcp.json`, `--scope local` keeps it to you; a Claude session sees the servers only if they were registered
   before it started):

   ```sh
   claude mcp add oot-p1 -- python -I tools/autotest/mcp_playtest.py --player p1 --lane 1 --session <session folder> --log-dir <log dir>
   claude mcp add oot-p2 -- python -I tools/autotest/mcp_playtest.py --player p2 --lane 1 --session <session folder> --log-dir <log dir>
   claude mcp list    # both must show Connected
   ```

   `--port N` names a port outright. Three or four players: `oot-p3` and `oot-p4` the same way (games started by
   hand: `live.py` starts two).

   **Both games in step (`--pair`).** One server instead of the two: `claude mcp add oot-pair -- python -I
   tools/autotest/mcp_playtest.py --pair --lane 1 --session <session folder> --log-dir <log dir>`. It holds both
   bridges (p1 and p2, ports from `--lane`). Every tool is the per-game tool with a `player`. Every frame that
   player's game runs, the other game runs too: walks, presses and steps frame for frame (`InStep`), and what ran
   outside them (a warp, a test line, a shot) right after the call, from the game's frame counts. So neither game is
   held while the other plays: its puppet keeps moving, and an enemy it owns is not frozen. Each such reply has
   `other`: the other player, the `frames` it ran, how many of them were `caught_up` after the call, and `in_step`
   (false with an `error` when the other game could not be stepped). When the player's game ran fewer frames than
   asked (a pause, a load), the other game runs only what ran, and `short` says how many did not. `get_state` and
   `step` with no `player` act on both games (`games`, by player); `pause` and `resume` always act on both, so one
   game is never left running free. A tool that runs no frame (get_world, checkpoint, log, ...) touches only its
   game. Calls are logged under the server `oot-pair` (with their `player`); `replay`, `save_script` and the script
   check read them. A bridge takes one client at a time, so `oot-pair` replaces `oot-p1` and `oot-p2`: never
   register both (`kit_doctor` fails an `.mcp.json` that has both, or an `oot-pair` without `--pair` or with a
   `--player`).
3. The tester follows [AGENTS.md](../AGENTS.md), "Play-testing the game", and runs [tests/scenarios.md](../tests/scenarios.md)
   in order.

After a scenario, `python -I tools/autotest/desync_check.py <log dir> --clients` gives the same answer as the
`desync_check` tool (exit 0 agree, 1 desync, 2 nothing compared, 3 incomplete). `--flags-only` leaves the actors out.

#### What `desync_check` compares in the actors

Each checkpoint keeps the `get_world` list of its game (`state.world`; the dump lists every kind, but the snapshot
behind it keeps at most 200 actors: the known kinds first, then the nearest to that game's own Link). For each tag,
the first game's list is
compared with every other game's, under these rules (code: `compare_world` in `desync_check.py`; the page is read by
`sync_rows.py`; the numbers are in `TOL`):

- **Only in the same area.** Both games in the same scene and their Links within 40 m, and neither list stale or
  without a session. Otherwise the line says `not compared (...)` and why. (A rolled spawn is shared only near its
  place, so apart the games roll their own.)
- **A cut list is compared only near both Links.** A list with 200 actors was cut, and what it lacks is mostly actors
  of kind `other` (crates, rocks, cracked walls, webs, falling floors) beyond the farthest one it kept. Each cut list
  has its own kept distance, measured from its own game's Link. An actor is compared only when its place is within
  A's kept distance of A's Link AND within B's kept distance of B's Link (a list that was not cut has no limit; an
  actor with no place is left out when a list is cut). So with the Links 30 m apart and both lists keeping 100 m, a
  crate 95 m from A and 65 m from B is compared, one 105 m from A is not. A count or a missing actor outside is
  taken to be the cut. The line says `a list was cut at 200 actors: only actors inside the kept distance of each
  game's own Link compared (A N m, B N m; M left out)`. A scene with fewer than 200 actors is compared whole. A bare
  200 is taken as a cut: the dump has no flag for it.
- **One to one.** Actors of one name are paired by nearest place (the games' addresses differ). An actor in one game
  with none to pair with in the other is a count difference, named.
- **What is compared.** `alive`, `owner` and `life` exactly. The place only for placed static actors (chests, doors,
  switches) and bosses, to 0.2 m. A moving enemy's place is never compared: each game stands at its own frame.
- **The page decides what a difference means.** The row for the name (best glob wins: no wildcard first, then the most
  literal characters; rows tied on it with different labels are both printed and the weaker label is used):
  `live` is a desync; `?` is `live not proven: check it twice, then report what you saw`; `reload` is `shared, shown on
  reload: not a desync`; `-`, or a Path starting with `none`, is not shared and left out (counted); a name with no row is
  `no row in sync-coverage: not compared` (named). Only `live` rows give exit 1. A placed actor or boss with no place
  in one game is not compared for its place and is named (`no place in one game: place not compared`). A row for the
  mini-boss and boss rooms has no actor names, so a boss lands on the Enemies row unless a `live` row names it
  (`Enemy_*Boss*` is checked for its place only when one does). The page's Enemies and Rolled spawns
  rows are `?` until a game run proves them.
- **Two looks.** A `live` difference counts only when the same one is in two checkpoints in a row, with both games 60 or
  more frames further on in the second. The first look is `recheck after both games step 60 frames` and the exit status
  is 3 (incomplete), never 0. Take a second checkpoint after stepping 60 frames or more. A difference that is gone in the
  second checkpoint was a game still on its way. The second look must be of the same actors: it matches the first
  by the two games' actor ids (a different Deku Baba does not confirm another one). A DESYNC line gives the actor (id
  and place in both games), `first seen at TAG (frames)`, `confirmed at TAG (frames)`, and the page's row (line, Path,
  Live), so a report can quote it.
- **Older logs.** A checkpoint with no `world` (written before this) is compared on its flags only, with the same
  output as before.

Not proven in a game: the 0.2 m, 40 m and 60 frame numbers are guesses from the design, and no run has compared a real
world yet.

### Rules for a tester on the bridge

- The games are held between calls. Nothing happens unless you act. Hold **every** game: a game whose server
  you have not called yet still runs (call `get_state` on each first).
- Set a test up with `place`, `flag` and `give`; `warp` in free play goes by Longshot's teleport (a map of its list, no entrance, unproven); for an entrance or another map start the session with a boot script that warps. Do not walk across Hyrule.
- Take a `checkpoint` with the same tag on **every** game after each action that should sync. Let the other game
  run a few frames first (`step 30`): a flag reaches it over the network, and a held game receives nothing. Then
  run `desync_check`.
- Use `get_state` to look and `screenshot` only when the picture matters. Anything visible is proven by a picture,
  never by flags alone.
- A bug report has: the scenario, the steps (tool calls), what was expected, what happened, the first tag that
  differs with each game's frame, and the shots.
- Never edit the mod's source during a test run. Report; fixing is a separate session.

## Lag on purpose

Two games on one PC really go through the network stack. Each game's co-op module connects to the server with ENet
(UDP) at `127.0.0.1:<server port>` (`lichii/hook/coop_hook.cpp`, `coop/net/src/client.cpp`), so its packets pass
through loopback sockets like any other program's. The play-test bridge is TCP on loopback too: shape only the
server's UDP port, or the tester's own calls are delayed as well.

- **Longshot (the kits): the server's test delay.** `coop_server --test-delay-ms N --test-jitter-ms J` with
  `OOT_COOP_TEST=1` holds every packet (N is the round trip, half each way). `run.ps1 -Runtime hookshot -Lag N -Jitter J`
  and `groups.py --lag N` pass it on ([testing.md](testing.md), "On Longshot: the server's test delay"). This is the
  repeatable choice: same rules every run, nothing to install.
- **Windows, any build: Clumsy.** Filter on the server's port only, for example
  `udp and (udp.DstPort == 27511 or udp.SrcPort == 27511)` (Clumsy sees loopback packets: both directions go
  through the loopback interface). Then lag, drop, out-of-order. It needs administrator rights (WinDivert), so it
  is for a person's PC, not an unattended lane run.
- **Linux: tc netem on lo, on the server's port only.** Plain `tc qdisc add dev lo root netem delay 150ms loss 5%`
  delays every loopback packet, the bridge included, and each packet crosses `lo` twice (out and in), so it doubles.
  On the port only:

  ```sh
  sudo tc qdisc add dev lo root handle 1: prio
  sudo tc qdisc add dev lo parent 1:3 handle 30: netem delay 75ms loss 2.5%
  sudo tc filter add dev lo parent 1:0 protocol ip u32 match ip protocol 17 0xff match ip dport 27511 0xffff flowid 1:3
  sudo tc filter add dev lo parent 1:0 protocol ip u32 match ip protocol 17 0xff match ip sport 27511 0xffff flowid 1:3
  # ... run the scenarios ...
  sudo tc qdisc del dev lo root
  ```

  Each direction gets 75 ms here, so the round trip is about 150 ms.
- **The emulator build: `tools/netsim/netsim.py`**, the UDP relay ([testing.md](testing.md), "Bad connections").

Run the same scenario without lag first, then with it, and compare the two `desync_check` reports.
