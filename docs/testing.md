# Testing co-op on two PCs

How to test the co-op build with the test kit: the server on one PC, the patched emulator on both, each with its own copy of the game. The kit is made by `tools/release/make_kit.ps1` ([Making the kit](#making-the-kit)); in the kit this file is `TESTING.md`.

## What a kit can show

Which features are in `main`, and so in a kit built from it, as of 2026-11-09 (for players: [known-issues.md](known-issues.md)). A feature that is not merged yet has no check to do: skip its rows in the [checklist](#checklist) and write "not in this kit". `VERSION.txt` in the kit says which commit it was built from.

| Feature | In `main` | Proven so far | Rows |
|---|---|---|---|
| Server, join, leave, rejoin, reconnect | Yes | One PC, two instances (2026-11-08) | 1-3, 20-24 |
| Presence: the other player's name, scene and distance in the panel, a marker where they stand | Yes | One PC, two instances | 4-7 |
| Shared world flags (a chest opened by one is open for both) | Yes, with a shared flags file ([below](#shared-flags-file)) | One PC: a Bool chest flag crossed from A to B | 8-9 |
| Chat box and player list (Enter, Tab) | Yes | One PC, two instances: chat both ways, Tab list with ping, join and leave lines, Esc closes the box | 10-11 |
| Items: key items, bottles, songs, one-time pickups shared; rupees and the rest per player | Yes. `LICHII_COOP_CHEATS=1` opens a debug window that gives any shared item, for checking without playing to it | One PC, two instances: Hookshot, Song of Storms and a boss key crossed and showed | 12-14 |
| Counts that add up (small keys, Skulltula tokens, heart pieces) | Yes (with items) | One PC: tokens, heart pieces and Forest Temple keys ended on the same totals in both games. Heart pieces add to the count but make no new heart | 13-14 |
| Puppet Link where the other player is | Yes (on by default; needs the puppet pack): follows live, copies the whole skeleton pose each frame, child or adult by the save | One PC, two games (autotest): walk, run, turn and sword stance match in screenshots of both windows | 15-17 |
| Equipment shown on the puppet | Sword and shield; tunic, boots and masks not yet | One PC (autotest): Kokiri Sword and Hylian Shield worn; a menu swap to the Deku Shield followed | 18 |
| Navi on each puppet | Yes (with the puppet; `LICHII_COOP_FAIRY=0` turns it off) | One PC, two games: a Navi at each puppet's shoulder | 15 |
| Scenes: puppets only in your scene, hidden in cutscenes | Yes | Unit tests only | 19 |
| Shared enemies, townspeople and animals (position, animation, life, kills) | Yes (`LICHII_COOP_OBJECTS=1`, on in the kit) | One PC, two games: copies hold the owner's place exactly; a Stalchild hit in A dies in both; Kakariko townspeople stand the same. Hit reactions (flinch, flash) not yet | 25 |
| Objects a player makes (seeds, nuts; bombs, arrows built) | Yes (`LICHII_COOP_SPAWN=1`, on in the kit) | One PC, two games, logs: seeds and a Deku Nut copied. No bomb thrown or arrow shot in a test yet | (none yet) |
| Dungeon switches, doors, cleared rooms, water levels | Yes (on unless `LICHII_COOP_DUNGEON=0`) | Unit tests and the shared flag path; not played through a dungeon | (none yet) |

## One PC first

Two copies of the kit on one PC show most of it before the two-PC test (done on 2026-11-08; see [lichii.md](lichii.md#the-first-run-2026-11-08-one-pc-two-instances)):

1. Unzip the kit twice, for example `local-A` and `local-B`, and fill each `emulator\user\` as below (keys, firmware, a save). Copying a kit folder that has already run is fine too: `play.cmd` points the copy's saved folders at itself.
2. Start the server once (`start-server.cmd` in either copy).
3. In each copy, `play.cmd` with server `127.0.0.1` and a different name (`A`, `B`). To see the markers on yourself too, turn on `LICHII_COOP_DEBUG_CAMERA=1` in that copy's `coop-options.ini`.
4. Load the same save in both and put the two windows side by side. The keyboard goes only to the window that has focus, so each Link can be moved alone.

Each panel lists the other (`P1 A: KakarikoVillage_Child, 1 units away`), the other's dot is where that Link stands in the other window, and the distance follows as one walks. Close each emulator with its own close button (it asks first). A killed emulator keeps its name taken on the server for a while: the next start with that name is refused (`name taken`) until the server notices, or restart the server.

## The kit

```
oot-coop-<date>\
  start-server.cmd       starts the server (one PC only)
  play.cmd               asks for the server address and your name once, then starts the emulator
  make-puppet-pack.cmd   builds the puppet Link from this PC's own game files (once per PC)
  coop-options.ini       extra switches (shared flags file, puppet, debug logging), all off
  collect-logs.cmd       zips the logs to send back
  emulator\              the patched emulator; user\ is its portable data folder
  server\                coop_server.exe
  tools\puppet_pack\     the pack builder make-puppet-pack.cmd runs
  python\                a Python for it (only if the kit was made with -Python)
  logs\                  the server's logs
  VERSION.txt            which builds these are
```

Copy the whole folder (or the zip, then unzip it) to each PC, anywhere you can write to, for example the desktop. Both PCs must use the same kit.

## What each PC needs

The kit has no game files, keys or firmware. Each PC uses its own, from its own console, put into the kit's portable folder `emulator\user\`:

| What | Where in `emulator\user\` |
|---|---|
| Keys (`prod.keys`, `title.keys`) | `keys\` |
| Firmware | `nand\system\Contents\registered\` (or start `play.cmd` once and use the emulator's Tools > Install Firmware) |
| Game updates you installed | `nand\user\Contents\registered\` (or File > Install Files to NAND) |
| Saves (optional; a new save works for most checks) | `nand\user\save\` |
| The game dump itself | Anywhere. Load it with File > Load File, or add its folder to the game list. |

If a PC already runs Lichii or Fritchii normally, all of that is in `%APPDATA%\eden`. Copy (do not move) from there into `emulator\user\`:

- `%APPDATA%\eden\keys` to `emulator\user\keys`
- `%APPDATA%\eden\nand` to `emulator\user\nand` (firmware, updates and saves, about 3 GB)
- optional, Ocarina's own settings (GPU mode and the rest): `%APPDATA%\eden\config\custom\0400486024DEC000.ini` to `emulator\user\config\custom\`

The kit's emulator never reads or writes `%APPDATA%\eden`: because `emulator\user\` exists, it keeps everything there. Your normal install and saves are not touched.

Both PCs need the same game version. Co-op only reads and sends for build id `e0f3ccfaf50065ae`; the emulator log says which build is running (below).

The Visual C++ runtime is in the kit, beside each program. If Windows still says `MSVCP140.dll` or `VCRUNTIME140.dll` is missing, install the Microsoft Visual C++ 2015-2022 Redistributable (x64).

### The puppet pack

The puppet Link is a small actor pack made from the game's own Link model and animations, so it cannot be in the kit: each PC builds it from its own game, once.

1. Dump the game's romfs: start `play.cmd`, add the game's folder to the game list if it is not there, right-click the game and choose **Dump RomFS** (full). It writes `emulator\user\dump\0400486024DEC000\romfs\` (several GB). Any other romfs of the same game version works too: the folder that holds `Pack`, `Model`, `AS`, `RSDB` and `System`.
2. Close the emulator and double-click `make-puppet-pack.cmd`. It finds the dumped romfs (or asks for the folder) and writes the mod `emulator\user\load\0400486024DEC000\coop-puppet\`. It takes under a minute.
3. It needs Python 3 with the `zstandard` package. A kit made with `-Python` brings its own; otherwise install Python 3 from python.org and run `py -m pip install zstandard` once. The script says which is missing.

Keep the kit in a short folder (the desktop or `C:\oot-coop`): the pack's longest file path must stay under 260 characters, and the script stops with a message if the kit is too deep. Running it again is safe (it rebuilds the same files). The romfs dump can be deleted afterwards. When the emulator loads the game with the pack, its log has `RomFS: LayeredFS patches applied successfully`. `play.cmd` says `Puppet pack: found.` or `not made yet` each time it starts.

The step this needs in the emulator (Dump RomFS on a `.nup`) has not been checked in a kit yet; `make-puppet-pack.cmd` itself has (2026-11-09, in a kit with its own Python, from a romfs dumped on the build PC): its packs are byte for byte the ones that loaded in game.

### Shared flags file

The emulator shares only the world flags it is told to. Until a full list exists, the two-PC test uses one known chest: the flower-grave chest in the Kakariko graveyard (child), Bool flag `0x39e4b7ec` ([flags.md](flags.md#live-check)).

1. In the kit folder, make a text file `shared_flags.txt` with the one line `0x39e4b7ec`. More flags go one a line, in the format of [host.md](host.md) (a name, a `0x` hash, or `key64:` and a 64-bit key).
2. In `coop-options.ini`, remove the `#` from `# LICHII_COOP_FLAGS=shared_flags.txt`.

Do both on both PCs. `play.cmd` prints `Option: LICHII_COOP_FLAGS=...` when it is on.

### coop-options.ini

Extra switches, one `NAME=value` a line, all off (commented out) in a new kit. Turn one on by removing its `#`, then close the emulator and start `play.cmd` again; it prints each switch it sets. A file path may be relative to the kit folder. The debug switches only add lines to the emulator log; turn them on when a check fails and send the logs. What each does is in [lichii.md](lichii.md) and the file itself.

## The network

The two PCs must reach each other over UDP:

- **Same LAN:** use PC 1's LAN address, `192.168.x.y` or `10.x.y.z` (`ipconfig`, "IPv4 Address").
- **Tailscale:** both PCs signed in to the same tailnet. Use PC 1's Tailscale address, `100.x.y.z` (`tailscale ip -4` on PC 1, or the Tailscale window).

`start-server.cmd` prints PC 1's addresses when it starts.

On PC 1 (the server), allow the server's UDP port in Windows Firewall once, from a PowerShell or Command Prompt run as administrator:

```
netsh advfirewall firewall add rule name="OoT co-op server UDP 27510" dir=in action=allow protocol=UDP localport=27510
```

(Or click **Allow** for private networks when Windows asks about `coop_server.exe` the first time.) Use the same port in the rule if you start the server on another port. To remove the rule after testing:

```
netsh advfirewall firewall delete rule name="OoT co-op server UDP 27510"
```

PC 2 needs no rule: the emulator only connects out. Only UDP is used; no TCP port needs to be open.

## Steps

1. **Both PCs:** unzip the same kit, fill `emulator\user\` (above), run `make-puppet-pack.cmd` if the kit has the puppet, and set up the [shared flags file](#shared-flags-file).
2. **PC 1:** double-click `start-server.cmd` (or `start-server.cmd 27600` for another port). It shows `coop_server: listening on port 27510` and the addresses to use. Leave the window open.
3. **PC 1:** double-click `play.cmd`. Server address: `127.0.0.1` (the server is on this PC). Name: anything, 1 to 16 characters, for example `Saria`.
4. **PC 2:** double-click `play.cmd`. Server address: PC 1's LAN or Tailscale address from step 2. Name: a different one, for example `Mido`.
5. Load the game in both emulators and get into the world on both. For the flag and puppet checks, use a child save that can reach Kakariko Village and its graveyard; the flower-grave chest must still be closed on both saves.

`play.cmd` asks only the first time; after that it reuses the answers. `play.cmd change` asks again. The answers are in `emulator\user\config\coop.ini`.

## What should happen

- In the server window: `player 1 joined: Saria`, then `player 2 joined: Mido` (the order is who started first).
- In each emulator, a small see-through panel in the top-left corner of the game:

  ```
  Co-op: player 1, ping 3 ms
  you: KokiriForest_Child
  P2 Mido: KokiriForest_Child, 412 units away
  ```

  The other player's line is grey while they are in another scene or have not sent a position yet. A coloured dot with their name marks where they stand when they are in view.
- When either player walks, the distance changes. When one goes through a door or a loading zone, the scene name changes on both panels.
- The emulator log, `emulator\user\log\eden_log.txt`, has (in this order):
  - `coop: loaded (ENet 1.3.18)` and `coop: on, server ..., name '...'` at start;
  - `coop: welcomed as player N on ...`;
  - `coop: player M '...' joined` for the other player;
  - after the game loads, `coop: game build e0f3ccfaf50065ae, main at 0x...`.

## Checklist

Write down pass, fail or "not in this kit" for each, and the time, so the logs can be matched. Rows for a feature that is not in the kit ([table above](#what-a-kit-can-show)) are skipped.

**Connection**

| # | Check | Pass |
|---|---|---|
| 1 | Server starts and shows `listening on port ...` | |
| 2 | PC 1's emulator starts; log has `coop: welcomed as player 1` | |
| 3 | PC 2's emulator starts; log has `coop: welcomed as player 2`; the server shows both joined | |

**Presence**

| # | Check | Pass |
|---|---|---|
| 4 | With the game loaded on both, each panel lists the other player by name | |
| 5 | Both in the same scene: the distance goes down as you walk towards each other and up as you walk away; the marker sits where the other Link stands | |
| 6 | One player changes scene: both panels show the new scene, and the line goes grey | |
| 7 | Ping on the panel stays under about 100 ms (LAN: a few ms) | |

**Shared world flags** (both PCs with the shared flags file)

| # | Check | Pass |
|---|---|---|
| 8 | Both in the Kakariko graveyard. PC 1 opens the flower-grave chest. PC 2's player walks there: that chest is already open. PC 2's log (with `LICHII_COOP_DEBUG_FLAGS=1`) has `bool 0x39e4b7ec ... 0 -> 1` | |
| 9 | PC 2 closes its emulator and starts again (or joins late): after loading, the chest is still open on PC 2 | |

**Chat and player list**

| # | Check | Pass |
|---|---|---|
| 10 | Enter on PC 1, type `hello`, Enter: PC 2 shows `Saria: hello` at the bottom left; PC 1 shows `you: hello`. The other way round too | |
| 11 | Hold Tab: the player list shows both players | |

**Items and counts**

| # | Check | Pass |
|---|---|---|
| 12 | PC 1 gets a key item (for example the slingshot, a bottle, a song): PC 2 has it too. PC 1 picks up rupees: PC 2's rupees do not change | |
| 13 | PC 1 picks up a small key in a dungeon: PC 2's key count for that dungeon goes up by one. Each uses one: the count goes down for both | |
| 14 | PC 1 gets a Gold Skulltula token or a heart piece: PC 2's token count or heart pieces go up by one. (That the Skulltula or heart piece is also gone in PC 2's world needs its 64-bit key in the shared flags file; those keys are not mapped yet, [items.md](items.md#one-time-pickups-caleb-2026-11-09).) | |

**Puppet Link** (both PCs ran `make-puppet-pack.cmd`; `LICHII_COOP_PUPPET=1` and `LICHII_COOP_PUPPET_ACTOR=CoopPuppet_Child` turned on in `coop-options.ini`, or `CoopPuppet_Adult` for an adult save)

| # | Check | Pass |
|---|---|---|
| 15 | Both in the same scene: each sees a second Link where the other player stands | |
| 16 | The other walks, runs, rolls and jumps: the puppet follows smoothly and plays the same moves, not a T-pose | |
| 17 | Child on one PC and adult on the other (if a save allows): each puppet has the right age | |
| 18 | The other changes sword, shield, tunic or boots (or draws the sword): the puppet changes too | |

**Scenes**

| # | Check | Pass |
|---|---|---|
| 19 | The puppet shows only when both are in the same scene; it is hidden in a cutscene | |

**Shared enemies** (`LICHII_COOP_OBJECTS=1` on both PCs; add `LICHII_COOP_DEBUG_OBJECTS=1` for the log lines)

| # | Check | Pass |
|---|---|---|
| 25 | Both in Hyrule Field at night, near each other. PC 1 hits a Stalchild with the sword: on PC 2 the same Stalchild dies when it dies on PC 1 (PC 2's log has `coop: objects: share life`) | |

**Leaving, rejoining, reconnecting**

| # | Check | Pass |
|---|---|---|
| 20 | PC 2 closes its emulator: the server shows `player 2 left`, PC 1's log has `coop: player 2 '...' left`, the line (and puppet) goes from PC 1 | |
| 21 | PC 2 starts `play.cmd` again and loads the game: welcomed again, and both panels list each other again | |
| 22 | Stop the server (Ctrl+C) and start it again: both logs show `connection lost; reconnecting`, then welcomed again, without restarting the emulators | |
| 23 | Both emulators close without an error or a crash dump | |
| 24 | Ten minutes of play together with no drop (no `connection lost` in either log) | |

After the test, run `collect-logs.cmd` on both PCs anyway and send both zips, even if everything passed.

## If something is wrong

| You see | Check |
|---|---|
| No panel, and the log says `coop: off (...)` | The line above it says what is wrong in `coop.ini`. Run `play.cmd change`. |
| The panel stays on `Co-op: connecting` (PC 2) | The address, the firewall rule on PC 1, and that the server window is still open. On Tailscale, that both PCs show as connected. |
| `name taken` or refused in the log | Both players used the same name: `play.cmd change` on one. |
| `coop: game build ... is not e0f3ccfaf50065ae` | That PC has another game version; co-op reads nothing there. |
| The other player's line never gets a scene or distance | Both must be in the game world, not the title screen. Then send the logs. |
| A chest is not shared | Both PCs have `shared_flags.txt` and the `LICHII_COOP_FLAGS` line on; `play.cmd` printed it. Turn on `LICHII_COOP_DEBUG_FLAGS=1`, try again and send the logs. |
| No puppet | `play.cmd` said `Puppet pack: found.`; the log has `LayeredFS patches applied`. Turn on `LICHII_COOP_DEBUG_PUPPET=1`, try again and send the logs. |
| Saves or mods of another kit folder show up | The kit was copied after a run; `play.cmd` fixes that by itself at start (it prints `This kit was copied`). Starting `lichii.exe` directly skips that fix. |
| The emulator crashes or freezes | Send the logs. |

On each PC with a problem, close the emulator (the log is complete only after it closes), then double-click `collect-logs.cmd`. It writes `coop-logs-<PC name>-<date>.zip` in the kit folder with the emulator log, `coop.ini`, `coop-options.ini`, the puppet pack's `coop_puppet.json` (file names and hashes only), the server logs (on PC 1), `VERSION.txt`, the newest crash dumps and a note of the PC's Windows, GPU and addresses. Send that zip back. It has no keys, saves or game files.

<!-- kit: TESTING.md ends here -->

## Making the kit

On the PC that builds (needs the patched emulator built from this repo's `main` as [lichii/README.md](../lichii/README.md) says, and this repo's server built Release):

```powershell
cmake -S . -B build
cmake --build build --config Release --target coop_server
powershell -ExecutionPolicy Bypass -File tools\release\make_kit.ps1 -Python <python.exe with zstandard>
```

It writes `..\coop-kit\oot-coop-<yyyymmdd-hhmm>\` and a zip of it beside the main checkout. Options:

| Option | What |
|---|---|
| `-EmulatorBin <folder>` | The patched emulator's `bin` folder (default `..\fritchii-src\build\coop\bin`). Its hook folder (`LICHII_COOP_DIR`) should be a checkout of `main`; `VERSION.txt` records which. |
| `-BuildDir <folder>` | This repo's CMake build folder with `coop_server` (default `build`). |
| `-OutRoot <folder>` | Where the kit goes (default `..\coop-kit`); never inside the repo. |
| `-Port <n>` | The default port written into the kit (27510). |
| `-Python <python.exe>` | A Python that has `zstandard`: its own files (no other packages, no tests or Tk) and `zstandard` with its licence go into the kit's `python\`, about 60 MB, so `make-puppet-pack.cmd` runs on a PC without Python. Without it each PC needs its own. |
| `-IncludePdb` | Adds `lichii.pdb` (about 60 MB) for crash dumps with symbols. |
| `-NoZip` | No zip. |

It refuses a kit in which any file names the build PC's user folder (`tools/release/check_paths.ps1`, tested by `tools/release/tests/paths_test.ps1`): build the emulator with trimmed paths ([lichii/README.md](../lichii/README.md)). The kit's Python is checked without leaving `__pycache__` behind, `VERSION.txt` names commits and versions but no folders or PC name, and the kit's `TESTING.md` is this page up to this section. It also refuses an emulator built without co-op, one that has Fritchii's own updater (build with `-DENABLE_UPDATE_CHECKER=OFF`: that updater would offer a plain Fritchii in place of the co-op one) and a Debug server, and writes which commits went in to `VERSION.txt`. It copies the pack builder (`tools/puppet_pack`, without its tests) but never a pack: no game data goes in a kit. With the zip it writes `<zip>.sha256` and copies `Update.bat` beside it. A release gets all three as assets: `Update.bat` checks a download against the `.sha256` and refuses a release without one, and the standalone `Update.bat` is how a kit from before the updater (v0.1.0-test2) gets it ([setup.md, Updating](setup.md#updating)). The kit's `MANIFEST.txt` lists its own files, so the next update can remove the ones a newer kit drops. Tests of the updater: `powershell -File tools\release\tests\update_test.ps1` (CI runs them).

To test a kit on the build PC, unzip it to two folders and run `make-puppet-pack.cmd` in each (or copy one filled folder to the other: `play.cmd` repoints the copy).

## Automatic tests

`tools/autotest/run.ps1` runs a two-game test on one PC with no one playing: it plays scripted controller input into both games and keeps screenshots of both windows, so a check can be done and looked at later.

```
tools\autotest\run.ps1 -Scenario walk
```

What it does:

1. Starts the server (`build\server\coop_server.exe`, port 27560) and the two kits `coop-kit\auto-A` and `auto-B` (found by going up from the repo; `-KitA`, `-KitB`, `-Game`, `-Exe`, `-Server` override). Each kit is an ordinary kit folder with a filled `emulator\user\` and a save. With `-Exe`, that emulator build is copied into both kits first.
2. Waits until both logs have `coop: welcomed as player`, puts the windows side by side and plays `scenarios\load.txt` in both (title screen to the first save).
3. Plays `scenarios\<name>.A.txt` in A and `<name>.B.txt` in B (none: B stands still), and captures both windows every `-Every` seconds (default 3) and at each `mark` line.
4. Closes both games and the server, copies the emulator and server logs beside the screenshots, and prints the folder: `coop-kit\shots\<scenario>\<date-time>\` (outside the repo: game screenshots are never committed). A run that went wrong leaves `FAILED.txt` there with the reason.

The co-op switches are `-Switches` (default puppet, objects and object debug lines on), as `NAME=value`. Give them as one comma list, `-Switches "LICHII_COOP_PUPPET=1,LICHII_COOP_OBJECTS_CHEAT=1"`: with `powershell -File` that is how several arrive anyway (before 2026-11-09 such a list was set as one wrong variable, so the switches were silently off). The list replaces the default, and the runner prints each switch it sets.

### Steps files

A scenario can instead be `scenarios\<name>.steps.txt`, which drives both games and the server step by step, for tests where a game quits, crashes or joins late. One command a line (`#` starts a comment):

| Line | Does |
|---|---|
| `start A B` | boots the games named (again), waits until each has joined; an earlier log of that game is kept as `eden_log-<x>-<n>.txt` |
| `load A B` | plays `load.txt` in the games named (title screen to the save) |
| `play A file.txt B file.txt` | plays those input scripts and waits for both |
| `close B` / `kill B` | closes B's window (it leaves the server) / ends it at once (a crash) |
| `server stop` / `server start` | the server goes away / comes back on the same port |
| `sleep 10` | seconds |
| `expect name A=1 B=0` | waits up to `-ExpectWait` seconds (default 30) until each game named has that many puppets (`CoopPuppet_*`) and as many puppet fairies (`Dm_Npc_Navi`) alive, counted from the objects debug lines (`LICHII_COOP_DEBUG_OBJECTS=1`, on by default); writes PASS or FAIL to `counts.txt` and takes a screenshot. A FAIL does not stop the test; at the end `FAILED.txt` says how many failed |
| `shot name` | a screenshot of both |

A steps run also writes `steps.txt`: each step with the local time to the millisecond, and after each step every game's process id and visible windows. A game that ends while the steps did not close or kill it is a FAIL in `counts.txt` ("ended by itself during step N"). Before each start the driver sets `confirmStop=2` (never ask) in the kit's `qt-config.ini`: with the default the window asks "Are you sure you want to close Lichii?", nobody answers, and `close B` ended B like a crash after 20 s (issue 112).

`stray` (checked 2026-11-09 on main c8ea805: 9 of 9 counts right) loads both games into the Graveyard, sends A to Kakariko and back, then has B quit, join late, crash and rejoin, stops and restarts the server, and brings B back as adult Link; at each step it checks that no puppet or fairy is left where it should not be.

### Input scripts

The emulator reads scripted input from the file in `LICHII_COOP_TEST_SCRIPT` ([coop_testinput.h](../lichii/hook/coop_testinput.h)). It looks at the file about twice a second; each time the file changes, it plays it from the start into player 1's pad, on top of the real keyboard. The window needs no focus. Time is counted in ticks of 1/60 s of real time.

| Line | Does |
|---|---|
| `wait 120` | nothing for 120 ticks (2 s) |
| `hold A 30` | hold A for 30 ticks |
| `hold LSTICK_UP+B 90` | left stick up and B together (run) |
| `press A` / `press ZR+A 10` | hold, then let go, 6 ticks each (or the number given) |
| `stick 0.5 1.0 60 [KEYS]` | left stick at x, y (each -1 to 1), with buttons if given |
| `rstick -1 0 30` | right stick (camera) the same way |
| `goto -7.5 71.1` / `goto X Z 1.5 600` | walk Link to world x, z (within 1 m, or the radius given; gives up after 10 s, or the ticks given, logging `coop: test goto timed out`) |
| `toward X Z 8 [KEYS]` | push the stick toward that point for 8 ticks, with buttons if given: Link turns to face it |
| `warp KakarikoVillage_Child Kakariko_WellSide` | test only: changes scene the game's own way (the String64 flag `Sequence_ReservedNextBanc`, and `Sequence_ReservedNextStartPosName` for the start position, a `StartPos` name in the scene's Banc). The game takes it only during an event, such as the one right after a save loads, so a warp scenario loads the save itself (run with `-NoLoad`) and asks for the warp first; the hook asks again every 0.5 s until Link is in that scene (up to 90 s) and logs `coop: test warp done`. A scene ending in `_Adult` or `_Child` also sets the bool `Sequence_IsChildhood` (false or true) through the game's queue, to load Link at that age |
| `give bombs` | test only: gives `bombs`, `bow`, `hookshot` or any item flag `coop_cheats` knows (needs `LICHII_COOP_CHEATS=1` in `-Switches`) |
| `wear head Armor_Child_Mask_Keaton` | test only: sets this game's `PlayerStatus.Equip*` flag for `weapon`, `shield`, `head`, `armor`, `boots` or `hand` (gauntlets) ([items.md](items.md)). Only the flag changes, not this game's Link; the other games' puppets wear it (scenario `wear`) |
| `mark name` | logs `coop: test mark name`; the runner takes a screenshot then |

`goto` and `toward` steer from Link's position and the game camera's ground directions (from its world-to-clip matrix), read each frame; a push that gets nowhere for a second steps aside. Positions are the ones the objects debug log prints (`at x y z`, y is height), or the `Translate` of a placement in the romfs Banc files.

Buttons: `A B X Y L R ZL ZR PLUS MINUS LCLICK RCLICK DUP DDOWN DLEFT DRIGHT`, and full stick directions `LSTICK_UP` (`_DOWN`, `_LEFT`, `_RIGHT`) and `RSTICK_...`. `#` starts a comment. At the end the log has `coop: test script done`; a bad line is logged as `coop: test script <file>: line N: why` and nothing is played.

Gear scenarios (checked 2026-11-09, with `LICHII_COOP_DEBUG_ANIM=1` added to `-Switches` for the `slots` lines): `back` (A stands with its back to B, then draws the sword, guards, puts away; B sees A's sword and shield where A's own Link has them), `backwarp` (the same after both warp into Kakariko; `-NoLoad`) and `late` (B loads the save 30 s after A; `-NoLoad`). See [puppet.md](puppet.md).

Warp scenarios (run with `-NoLoad`): `warp` (both games load into Kakariko at the well) and `water` (both load into the Water Temple entrance; checked 2026-11-09: both logs `warp done` after about 50 s, screenshots in the temple). Warping a game that is already playing works only while an event runs.

Scenarios (checked 2026-11-09 unless said): `walk` (A walks, dashes, turns; B sees it move), `slingshot` (aims and fires a seed), `menu-gear` (equips the Deku Shield in the menu), `sword` (target and swing; no enemy at the save point, so no hit seen yet), `bomb` (not checked: the auto kits' save has no bombs), `buttons` (presses each button with a screenshot after it). The bindings are the game's own (pause menu > System > Basic Controls): ZL target, ZR use item, L/R quick select, MINUS map, PLUS menu (L/R change page), A actions and dash (hold), B put away, X jump, Y attack. Screenshots are the screen under each game's client area: the runner keeps both windows on top, side by side, at real pixels (DPI-aware).
