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

On PC 1 (the server), allow the server in Windows Firewall once, from a Command Prompt run as administrator, in the kit folder (put in your own path):

```
cd /d "C:\Games\oot-coop"
netsh advfirewall firewall add rule name="OoT co-op server UDP 27510" dir=in action=allow program="%CD%\server\coop_server.exe" protocol=UDP localport=27510 profile=private remoteip=localsubnet
```

For Tailscale use `profile=any remoteip=100.64.0.0/10` instead of the last two (Windows calls the Tailscale adapter Public); the rule still lets in only the server, and only from tailnet addresses.

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
| 14 | PC 1 gets a Gold Skulltula token or a heart piece: PC 2's token count or heart pieces go up by one. (That the Skulltula or heart piece is also gone in PC 2's world needs its 64-bit key in the shared flags file; those keys are not mapped yet, [items.md](items.md#one-time-pickups-alex-2026-11-09).) | |

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

On each PC with a problem, close the emulator (the log is complete only after it closes), then double-click `collect-logs.cmd`. It writes `coop-logs-<date>.zip` in the kit folder with the emulator log, `coop.ini`, `coop-options.ini`, the puppet pack's `coop_puppet.json` (file names and hashes only), the server logs (on PC 1), `VERSION.txt`, a note of the PC's Windows, GPU and the kind of each address (crash dumps only with `collect-logs.cmd -WithCrashDumps`). It has no keys, saves or game files, and the copies are masked (`scripts/log-mask.ps1`, tested by `tools/release/tests/log_mask_test.ps1`): each IP address becomes its kind with a number (`<lan-1>`, `<tailscale-1>`, `<hamachi-1>`, `<radmin-1>`, `<loopback-1>`, `<public-1>`; ports stay, the same address keeps its tag), MAC addresses `<mac>`, the Windows user name `<user>`, the PC name `<pc>`, a set `password`/`server_password` `(removed)`. Send it privately (Discord DM), not on a public issue.

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
| `-Recipes <zip or folder>` | Fritchii's release zip (`Fritchii-<version>-Windows-x64.zip`, from the release of the pinned tag) or a folder: its `.nx2recipe` files go to `emulator\recipes\`, so players get Fritchii's "Preparing shaders" step on the first start (less stutter, most of all on AMD). A recipe holds no game code; `SOURCE.txt` names each one with its SHA-256. Release kits use it. |
| `-BuildDir <folder>` | This repo's CMake build folder with `coop_server` (default `build`). |
| `-OutRoot <folder>` | Where the kit goes (default `..\coop-kit`); never inside the repo. |
| `-Port <n>` | The default port written into the kit (27510). |
| `-Python <python.exe>` | A Python that has `zstandard`: its own files (no other packages, no tests or Tk) and `zstandard` with its licence go into the kit's `python\`, about 60 MB, so `make-puppet-pack.cmd` runs on a PC without Python. Without it each PC needs its own. |
| `-IncludePdb` | Adds `lichii.pdb` (about 60 MB) for crash dumps with symbols. |
| `-NoZip` | No zip. |

It refuses a kit in which any file names the build PC's user folder (`tools/release/check_paths.ps1`, tested by `tools/release/tests/paths_test.ps1`): build the emulator with trimmed paths ([lichii/README.md](../lichii/README.md)). The kit's Python is checked without leaving `__pycache__` behind, `VERSION.txt` names commits and versions but no folders or PC name, and the kit's `TESTING.md` is this page up to this section. It also refuses an emulator built without co-op, one that has Fritchii's own updater (build with `-DENABLE_UPDATE_CHECKER=OFF`: that updater would offer a plain Fritchii in place of the co-op one) and a Debug server, and writes which commits went in to `VERSION.txt`. It copies the pack builder (`tools/puppet_pack`, without its tests) but never a pack: no game data goes in a kit. With the zip it writes `<zip>.sha256` and copies `Update.bat` beside it. A release gets all three as assets: `Update.bat` checks a download against the `.sha256` and refuses a release without one, and the standalone `Update.bat` is how a kit from before the updater gets it ([setup.md, Updating](setup.md#updating)). The kit's `MANIFEST.txt` lists its own files, so the next update can remove the ones a newer kit drops. Tests of the updater: `powershell -File tools\release\tests\update_test.ps1` (CI runs them).

To test a kit on the build PC, unzip it to two folders and run `make-puppet-pack.cmd` in each (or copy one filled folder to the other: `play.cmd` repoints the copy).

### Signing the release

The kit is signed in CI, with no key on the build PC and no secret in the repo ([#161](https://github.com/daurosk/oot-remake-multiplayer/issues/161)). When a release is published, `.github/workflows/release-attest.yml` downloads its `oot-coop-<tag>.zip` (and `oot-coop-<tag>-windows.zip`, the same file, from v0.11.0) and `Update.bat`, checks the zip against the release's `.sha256`, and makes a GitHub artifact attestation for each: a statement about that exact file (its SHA-256), signed through Sigstore with this repository's workflow identity and logged publicly. The release step, after the assets are uploaded:

1. Publish the release with the assets in place (`gh release create <tag> <zip> <zip>.sha256 Update.bat ...`: gh uploads them before it publishes). From v0.11.0 the Hookshot kit (`make_kit_hookshot.ps1`) is `oot-coop-<tag>-windows.zip`, and the same file is also `oot-coop-<tag>.zip`: upload BOTH, each with its `.sha256`. The updater of every kit up to v0.10.1 looks for `oot-coop-<tag>.zip` only, so without it those kits never see the release; the release-attest run fails when that name is missing or differs from the `-windows` zip. `tools\release\tests\old_kit_update_test.ps1 -OldZip <the v0.10.1 release zip> -NewDir <OutRoot> -Tag <tag>` updates an old kit to the new zips with the old kit's own updater, before publishing. The "Release attest" run starts by itself; check once that it is green (`gh run list --workflow release-attest.yml --limit 1`; no watch loops). If it did not run (the release was published from a draft in the web page, or it failed), start it: `gh workflow run release-attest.yml -f tag=<tag>`.
2. Check the files from the build PC: `powershell -ExecutionPolicy Bypass -File tools\release\verify_kit.ps1 -Zip <OutRoot>\oot-coop-<tag>.zip` and the same with `-Zip <OutRoot>\Update.bat`. Both must say `OK: signed by our release workflow`.

What it proves: the file is the one the release had when it was published, checked by our workflow, and a file swapped in later has no attestation. What it does not prove: who uploaded it in the first place; anyone with write access to the repo could publish a release and get it attested. `Update.bat` still checks only the `.sha256`; players check the signature by hand ([setup.md, Check the download](setup.md#check-the-download)). The workflow also tests itself on a push that changes it or `verify_kit.ps1`: it attests a made-up zip, `verify_kit.ps1` accepts it, and refuses a changed copy and a wrong `.sha256`.


## Automatic tests

`tools/autotest/run.ps1` runs a two-game test on one PC with no one playing: it plays scripted controller input into both games and keeps screenshots of both windows, so a check can be done and looked at later.

```
tools\autotest\run.ps1 -Scenario walk
```

Who runs it: branches do not run the game or build the emulator on their own. Each branch is proven by its unit tests in CI and a review, and lists what should be checked in the game. The integrator merges a chunk of reviewed branches, builds once (unit tests, emulator, kits) and runs **one big autotest turn** that covers every branch's list, then reports a result per check ([workflow.md](workflow.md#how-a-chunk-of-work-goes)). Only one session runs the games on a PC at a time.

`-Runtime hookshot` (default `emulator`) runs the same scenarios in two Hookshot games instead of the two emulator kits; how to set those up is in `docs/hookshot.md`.

What it does:

1. Starts the server (`build\server\coop_server.exe`, port 27560) and the two kits `coop-kit\auto-A` and `auto-B` (found by going up from the repo; `-KitA`, `-KitB`, `-Game`, `-Exe`, `-Server` override). Each kit is an ordinary kit folder with a filled `emulator\user\` and a save. With `-Exe`, that emulator build is copied into both kits first.
   Every session has a join password (protocol 10: the server does not start without one, and all traffic is encrypted): the server gets `-ServerPassword` (default `autotest-pass`, in an environment variable) and both games get the same in their `coop.ini`, unless a steps file says `password A TEXT` (`password A -` sends none, to test a refusal). With `-Runtime hookshot`, `hookshot.py run --password` (same default) gives it to the server and to both games (`LICHII_COOP_PASSWORD`). There is no switch to turn the encryption off, not even for tests.

   Both games' renderers first: with `-Runtime hookshot`, nothing is pressed until each game's log has `vk: precompile: N programs ... in N ms` (its pipelines made). With a cold driver shader cache that is minutes, and a game still compiling reads no controller, so a scenario used to fail with "did not read the controller in time". `hookshot.py run --renderer-wait SECONDS` (default 600, 0 = no wait); the steps runner (hs_steps.py) waits the same after "welcomed as player", within its `--timeout`.
2. Waits until both logs have `coop: welcomed as player`, puts the windows side by side and plays `scenarios\load.txt` in both (title screen to the first save).
3. Plays `scenarios\<name>.A.txt` in A and `<name>.B.txt` in B (none: B stands still), and captures both windows every `-Every` seconds (default 3) and at each `mark` line.
4. Closes both games and the server, copies the emulator and server logs beside the screenshots, and prints the folder: `coop-kit\shots\<scenario>\<date-time>\` (outside the repo: game screenshots are never committed). A run that went wrong leaves `FAILED.txt` there with the reason.

The co-op switches are `-Switches` (default: the kit's own, puppet, objects and spawn sync, plus object debug lines), as `NAME=value`. Give them as one comma list, `-Switches "LICHII_COOP_PUPPET=1,LICHII_COOP_OBJECTS_CHEAT=1"`: with `powershell -File` that is how several arrive anyway (before 2026-11-09 such a list was set as one wrong variable, so the switches were silently off). The list replaces the default, and the runner prints each switch it sets. A switch written `A:NAME=value` (or `B:`) is set for that game only, for example a key that only A sets (`deku-map`, `deku-web`).

### Start place

Every scenario starts from the same save. The game autosaves on each scene change (six rotating slots per user) and Continue loads the newest, so after a run that walked into Kakariko the next run used to start in Kakariko (chunk 1, 2026-11-09). The runner now copies each kit's **save baseline** (`<kit>\autotest-save\<user>\0400486024DEC000\`, outside the repo) back over the kit's saves before every boot, `start` steps included. Setting it up once:

1. `tools\autotest\kit-paths.ps1` shows which `nand`, `sdmc`, `load` and `dump` folders each kit's emulator really uses (`emulator\user\config\qt-config.ini`). On 2026-11-09 auto-A and auto-B both used `obj-A`'s NAND, so the two games also shared saves and autosaves. `kit-paths.ps1 -Fix` (games closed) points them back at each kit's own `emulator\user\...` and keeps the old file as `qt-config.ini.bak-<time>`. The runner refuses to run while A and B share one NAND.
2. `tools\autotest\run.ps1 -Scenario walk -TakeBaseline` copies each kit's saves as they are into its baseline and stops. Take it while the newest save is the child in the Graveyard at 10:01 (auto-A's own NAND still had that on 2026-11-09: newest autosave 2026-11-08 22:10).
3. Without a baseline the runner stops and says so; `-NoRestore` runs on whatever was saved last.

**Co-op off (`-NoCoop`).** Runs the same kits and scripts without a server: `coop.ini` names no server and `LICHII_COOP_SERVER` is cleared for the games, so each logs `coop: off` (the runner waits for that instead of the join), nothing is shared, and with the default switches (only `LICHII_COOP_DEBUG_OBJECTS=1`) the game's code is not patched for co-op (no `Applying "coop puppet"` line). The test script, the object walk (`+`/`-` lines) and `LICHII_COOP_OBJECTS_CHEAT` still work. Use it to tell whether something the game does wrong comes from our code: run once with and once without. The emulator's own built-in patches (Max Resolution, Dynamic FPS) stay on either way.

**Emulator add-ons off (`-AddOnsOff`, `-Fps30`).** `-AddOnsOff "Dynamic FPS by Dark (built-in)","Water ripples at any frame rate (built-in)"` turns those off for the run: the names go into each kit's `qt-config.ini` `[DisabledAddOns]` before the boot, exactly as the emulator's Add-Ons tab writes them, and the kit's own file is put back after the games close (not with `-KeepOpen`). Any name the Add-Ons list shows works: `Max Resolution (built-in)` (or the switch `EDEN_NX2_NO_MAX_RES=1`), and the puppet pack's mod folder `coop-puppet`. The log then says `Skipping "<name>": disabled in Add-Ons` for a built-in, and has no `coop-puppet` line for the mod. `-Fps30` turns on the emulator's 30 FPS mode (`oot_30fps`) for the run. With `-NoCoop` these tell the emulator's patches and our mod apart from the game (issue 125).

**Items in the baseline save.** `tools/autotest/save_flags.py` changes flag values in a closed save, so a scenario can start from a save that already holds an item, as a player's own save would. `bombs` writes what a real small bomb bag sets (docs/items.md, "Bombs from the cheat"), never lowering a value; `get` and `set` read and write single flags (`int:0xHASH=20`, `bool:`, `uint:`, `enum:`); `list <kit>\autotest-save` shows each user's slots with their bombs, so the autotest user (all slots 0) can be told from another profile. It only touches `<kit>\autotest-save\<user>\<title>\slot_NN\progress.sav`, and never anything under `%APPDATA%` or `%LOCALAPPDATA%` (a player's own saves); it expands `slot_0*` itself, keeps the old file in `<kit>\autotest-save-backup\` (same layout, `progress.sav.bak-<time>`; outside the baseline, which the runner copies into the game), writes through a temp file, and refuses a save that changed while it ran. Only values already in the save change, nothing moves, and there is no checksum. With the games closed, for each kit: `python tools\autotest\save_flags.py bombs <kit>\autotest-save\<user>\0400486024DEC000\slot_0*\progress.sav`. Tested in `tools/autotest/tests/test_save_flags.py`. `bombs` also gives the Goron's Bracelet and sets `PlayerStatus.LiftGrade` from None to Low: without a lift grade the game deletes every pulled bomb (issue 125).

After `load.txt` the runner checks that each game reached the save point: its log must show a Graveyard object (`coop: objects: + ...Graveyard...`, needs `LICHII_COOP_DEBUG_OBJECTS=1`, on by default) within 15 s, else the run stops with `start.txt` saying which game loaded elsewhere.

**Scenario headers.** Lines starting `#!` in the A script (or steps file) are read by the runner; the emulator sees comments:

| Line | Does |
|---|---|
| `#! noload` | the script loads the save itself (as `-NoLoad`) |
| `#! switches A=1,B=2` | added to the switches (a name also given in `-Switches` keeps the command line's value) |
| `#! start REGEX` | what the log must show once loaded; checked at the end for a `noload` script (`#! start coop: test warp done` for the warp scenarios) |
| `#! timeout 300` | seconds per phase, as `-Timeout` (which wins) |

**Screenshots of the game, not the cheats window.** With `LICHII_COOP_CHEATS=1` the cheats window could be the process's main window, and the screenshots showed it. The runner now picks the process's largest visible window that is not "Co-op item cheats".

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
| `lines name B max=3 REGEX` | counts the lines of B's log that match REGEX: PASS when at most 3 (`max=`) or, with `min=3`, at least 3; written to `counts.txt` |
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
| `hold ZR 90 aim` | hold ZR for 90 ticks and say `mark aim` halfway, with ZR still down (an aimed item is in the hand then) |
| `hold LSTICK_UP+B 90` | left stick up and B together (run) |
| `press A` / `press ZR+A 10` | hold, then let go, 6 ticks each (or the number given) |
| `stick 0.5 1.0 60 [KEYS]` | left stick at x, y (each -1 to 1), with buttons if given |
| `rstick -1 0 30` | right stick (camera) the same way |
| `goto -7.5 71.1` / `goto X Z 1.5 600` | walk Link to world x, z (within 1 m, or the radius given; gives up after 10 s, or the ticks given, logging `coop: test goto timed out`) |
| `toward X Z 8 [KEYS]` | push the stick toward that point for 8 ticks, with buttons if given: Link turns to face it |
| `warp KakarikoVillage_Child Kakariko_WellSide` | test only: changes scene the game's own way (the String64 flag `Sequence_ReservedNextBanc`, and `Sequence_ReservedNextStartPosName` for the start position, a `StartPos` name in the scene's Banc). The game takes it only during an event, such as the one right after a save loads, so a warp scenario loads the save itself (run with `-NoLoad`) and asks for the warp first; the hook asks again every 0.5 s until Link is in that scene (up to 90 s) and logs `coop: test warp done`. A scene ending in `_Adult` or `_Child` also sets the bool `Sequence_IsChildhood` (false or true) through the game's queue, to load Link at that age |
| `chat /teleport Saria` | test only: sends the rest of the line as the chat box does (chat, or a command such as `/teleport`); logs `coop: test chat '...': sent`. `#` still starts a comment |
| `place -37.2 109.8 13.5` | test only: puts Link at world x, y (height), z in the scene he is in, through the boss pull's free-play move (`CoopBoss::MoveLink`, needs `LICHII_COOP_BOSS` on, the default); logs `coop: test place x y z: done` (or `failed`: no Link). For rooms no `goto` reaches (other floors of a dungeon). Positions as for `goto`: the objects debug log or a placement's `Translate` |
| `kill Enemy_Skulltula_Gold` | test only: the nearest actor of that name to Link with a life takes a hit of its whole life from Link, through the game's own damage (as a real kill, so it drops what it drops); needs `LICHII_COOP_OBJECTS` on (the kit's default); logs `coop: test kill NAME 0x... at x y z (m, life L): hit`, or `none here with a life`. For kills the aim cannot make (the slingshot never hit the Graveyard Skulltula) |
| `give bombs` | test only: gives `bombs`, `bow`, `hookshot` or any item flag `coop_cheats` knows (needs `LICHII_COOP_CHEATS=1` in `-Switches`) |
| `setflag PlayerStatus.CurrentRupee 99` | test only: sets this game's Int flag, or else its Bool flag, of that name (GameDataList spelling) to the whole number, through the game's queue; logs `coop: test setflag NAME (int\|bool) N: set`. A max-rule item lowered this way stays in this game (not sent). Scenario `shop-buy` empties the kit saves' Deku Shield with it |
| `setenum 0x9472302b MeetPierre` | test only: sets this game's Enum flag (a GameDataList name, or `0x` and its hash: a Threads of Time quest's name is not in the romfs) to the value of that name (`flag_hash`), through the game's queue; logs `coop: test setenum NAME VALUE: set` (or `NOT set`, `unknown`). Scenario `threads-of-time` (#193) |
| `wear head Armor_Child_Mask_Keaton` | test only: sets this game's `PlayerStatus.Equip*` flag for `weapon`, `shield`, `head`, `armor`, `boots` or `hand` (gauntlets) ([items.md](items.md)). Only the flag changes, not this game's Link; the other games' puppets wear it (scenario `wear`). `wear head none` empties the flag: the piece is taken off |
| `assign Bow` | test only: puts an item on ZR, for both ages, through the game's Enum queue (`PlayerStatus.AssignableItem.Child` and `.Adult`); the name is the button enum's (`Bow`, `Hookshot`, `Hammer`, `SlingShot`, `Bomb`, `DekuStick`, ...; GameDataList spelling). The item must be owned (`give`). R is the quick-select carousel and assigns nothing |
| `mark name` | logs `coop: test mark name`; the runner takes a screenshot then |

`goto` and `toward` steer from Link's position and the game camera's ground directions (from its world-to-clip matrix), read each frame; a push that gets nowhere for a second steps aside. Positions are the ones the objects debug log prints (`at x y z`, y is height), or the `Translate` of a placement in the romfs Banc files.

Buttons: `A B X Y L R ZL ZR PLUS MINUS LCLICK RCLICK DUP DDOWN DLEFT DRIGHT`, and full stick directions `LSTICK_UP` (`_DOWN`, `_LEFT`, `_RIGHT`) and `RSTICK_...`. `#` starts a comment. At the end the log has `coop: test script done`; a bad line is logged as `coop: test script <file>: line N: why` and nothing is played.

Gear scenarios (checked 2026-11-09, with `LICHII_COOP_DEBUG_ANIM=1` added to `-Switches` for the `slots` lines): `back` (A stands with its back to B, then draws the sword, guards, puts away; B sees A's sword and shield where A's own Link has them), `backwarp` (the same after both warp into Kakariko; `-NoLoad`) and `late` (B loads the save 30 s after A; `-NoLoad`). See [puppet.md](puppet.md).

Chunk 2 scenarios (written 2026-11-10, first run in the chunk 2 test turn, see the integrate-p5 results; each file's header lists its log lines):

- `held-tools` (#92): A puts the Deku Stick, Deku Nut, a bottle, bombs and the slingshot on ZR through the pause menu's Items page, one at a time, and uses each facing B. R is a quick-select carousel, not "next item", so the chunk 1 version never changed A's item. The menu moves assume the cursor opens on the item last put on ZR; each `menu-*` mark is a screenshot to check that, and `menu-1` shows where the given boomerang sits.
- `skulltula` (#89): the Graveyard's nocturnal Gold Skulltula (on a web at -20.20 104.30 26.40). The save is at 10:01, so the header sets the host's clock to 20:00 75 s after joining and both games walk out to Kakariko and back to load the Graveyard at night; A shoots it with the slingshot while targeting, then jumps for the token; B watches.
- `skulltula-kill` (#89, items-sync): as `skulltula`, but A's Skulltula dies by the test-only `kill` step and `place` puts A's Link on the token where the Skulltula was. The header lists the log lines that pass: A's token goes out once, B's token (if its game made one) is deleted, both token counts end at the same N+1.
- `pot-drops` (#102): both warp to `Kakariko_MountainTrailSide`; A lifts and throws (A, ZR) the pot 16 m west of it and then the pair 28 m south, and walks over where each broke. Lifting breaks a pot for sure, where chunk 1's sword swings at the house pots never reached them.
- `kak-gate` (#171, world-objects-2): both warp to `Kakariko_MountainTrailSide`, on the trail side of the guard's gate; A sets the guard's flag `Npc_Hylia_Soldier_003_GateOpen` (0x19898d14) at 70 s; B tries the gate before (should stop) and after (should pass), shots of the gate in both.
- `deku-map-both` (#146, world-objects-2): both stand at the Deku Tree map chest and open it at about the same time, then open the pause map; both watch `DungeonInfo.InsideTheDekuTree.IsGetMap` (0x89787395).
- `chest-other` (#195, chest-sync-2): B opens the Deku Tree map chest by hand while A stands beside it; A's chest must show open (`OpenWait` played), never vanish; A tries it; then both go to room 3 and back, and the chest is made open in both. The header lists the log lines.
- `stalchild`: Hyrule Field at night (clock set by the header); A walks to where two Stalchildren rose in a chunk 1 run (about 173 29) and fights at once (ZL, Y), then walks over the fight for the drops; B watches from 8 m.
- `rock` needed no change: it failed in chunk 1 only because the game started in Kakariko.

Enemy scenarios (branch `enemy-sync`, not run yet; each file's header has what to look for):

- `deku-wake` (#132, #133): both games in the Deku Tree entrance room; A wakes and fights the Baba at -22.81 -1.82, B the one at -71.33 3.50, about 49 m apart. Both Babas must be up on both screens at `risen`, and each killed Baba gone on both at `after-kill`.
- `skull-sling` (#133): both games in the Deku Tree slingshot room (`place`, as deku-ladder). A stands 6 m from the wall Skulltula 0x282f9be0f907855f and owns it; B shoots it with the slingshot from the door. It must die in both games: A `share got damage`, then gone in A (or `dead by a hit our game did not take`), gone in B; no Skulltula on the wall in either `after-shot` shot.
- `chase` (#101): Hyrule Field at night (clock set by the header); A stands 40 m east of where the Stalchildren rise, B raises them and walks to A with them behind it. At `came` A's screen must show them next to B's puppet; A's `share worst copy` lines (debug, every 5 s when a copy is 1 m or more from where the Mover put it) name any copy the setter does not hold.
- `gohma-door` (#150, #178): both load at the Deku Tree entrance, are put in the corridor before Gohma's door (`place`) and walk in through it, so the game's own boss trigger makes Gohma (a warp into the room skips it, and the old `gohma` scenario had no boss). A then stands at the room's edge; B stays near Gohma and shoots it three times with the slingshot. Needs the kit saves' `Gohma_DefeatBoss` and `HavePlayedEvent.Dm_Deku_BossOut` at 0.
- `gohma-sync` (#150, #178): both warp into the boss rush's Gohma room (`warp GohmaStage GohmaStage`; no kit save is from before Gohma, and the boss rush places its own Gohma). A (owner) shoots seeds into the eye three times; B locks on (ZL), walks up to Gohma and swings the sword (ZL+Y) after each, then shoots a seed itself. A swing outside a stun is inconclusive, not a fail. If no Gohma appears, the fallback is gohma-door with a pre-Gohma save, or Alex's own save as an ALEX TEST. Checks in the script's header: B's `plays ... on slot N` lines, the eye the same on both screens, B's hits taking A's Gohma's life with no hand-over, one hand-over on B's stun, eggs and larvae on both. Inconclusive if neither log has `+ Enemy_DungeonBoss_Gohma`.
- `hs-boss-cutscenes-gohma`, `hs-boss-far-gohma`, `hs-boss-cutscenes-dodongo` (branch boss-cutscenes; group file `tools/autotest/groups/hookshot-boss-cutscenes.txt`): a boss's intro and death for everyone in its area, a far player with chat lines only ([dungeons-boss.md](dungeons-boss.md), "Boss cutscenes"). Proof lines in each header. The Gohma ones need the Deku boss flags cleared in the baselines (`hs-gohma-door`'s header).
- `hs-<boss>-native` (#150, branch bosses; group file `tools/autotest/groups/hookshot-bosses.txt`, one boss per group): for every boss (gohma, dodongo, barinade, phantomganon, volvagia, morpha, bongobongo, twinrova, ganondorf, ganon) both warp into its boss rush room (`<Boss>Stage`), A owns the boss, both lock on and swing now and then. Proof lines in each header: native sync on in both logs, `the owner's verbs came` in B, the change counters growing together, no `started again`, the same move on both screens, B's hit taking A's life for the boss. Needs a Hookshot build with patch 0013; without it INCONCLUSIVE.
- `gohma-movein` (#148, branch boss-movein-2): A loads at the Deku Tree entrance, clears the kit save's boss flags (setflag) and walks into Gohma's room through the door, B waits at the Deku Tree entrance and is moved in only after A's game says A is past the door (shut in). Inconclusive if the warp leaves ShutIn at 0.
- `peahat` (#157): child field by day; A is put 25 m east of the Peahat west of the market and stands still, B is put 25 m from it and walks right up to it, then A walks up too. The Peahat must rise on both screens (shots `b-at`, `risen`).
- `shop-buy` (#130): both load in Kokiri Forest, empty their own Deku Shield (`setflag`, a max drop stays in its game); A gets 99 rupees, is put at the Kokiri Shop's counter, talks to the keeper and buys the shield; B waits in the shop, then equips the Deku Shield from the menu. Pass: B's flags debug `int 0x7ca64f23 0 -> 1` after A's, the shield on B's Link (`equipped`) and on B's puppet in A. Not run yet: the shop's menu presses are a guess; the `item-N` and `on-shield` shots show where the cursor is.
- `threads-of-time` (#193): A shuts the Threads of Time tab for the group (`UI_IsFinishChallengeMenuOpenEvent` 0), both put the scarecrow quest at NotReady; B steps through the pause tabs (shots `tot-b-locked-*`). A opens the tab (flag 1, as the owl's event) and moves the quest to MeetPierre: B's flag and quest follow, B's tab opens (shots `tot-b-open-*`). B moves the quest to MeetBonooru: A takes it (shots `tot-a-a-*`). Checks in `counts.txt` from the logs; look at the shots for the tab itself.

Route fixes after the chunk 2 test (2026-11-10, shots in `coop-kit\shots`): a 6-tick `press A` at a prompt ("Pick Up" at the rock, "Check" at the sign) did nothing, so `rock` and `cutscene` let the stick settle 20 ticks and press 12. With the cheat's bombs every ZR makes a bomb that the game deletes 0.1 s later, held or not (`spawn-bomb` run 20261110-010524, held 60 and 150 ticks): not an input problem; the cheat gives the bag, count and capacity but something else the game checks is missing, so no bomb is thrown yet. `held-tools` waits out the bottle swing before opening the menu again (PLUS was lost and item 4 ended on the Save screen). `skulltula` puts B about 5 m from the Skulltula; A's slingshot never locked on or hit it (no hit line), so the token is still not taken. `pot-drops`: the route is still wrong; A stops at a wall at x -48.9 going west to the pot at -56.92 -16.01, and at z -12.4 going south to the pair. `deku-web` needs a save where the web is not burned yet.

Chunk 2 test turn scenarios (2026-11-10): `boss` (A loads into the Water Temple boss room, B into the entrance and should be moved in, then walks out and should be pulled back; boss-room and object-groups lines) and `deku-map` / `deku-web` (both load into the Deku Tree entrance, only A sets one dungeon key64; dungeon-sync).

Re-routed scenarios for rows the big run of 2026-11-12 left unproven ( each header says why the old run proved nothing and lists the pass lines): `hs-pot-drops` (#101 #102: A is put by the Kakariko hill pots and cuts them, so each drop lies on the pot's spot), `hs-sign-late` (#90: sign-late's steps; the late loader's catch-up logs a hit line just before the words, which is not a live hit), `hs-goron-rock` (#84: the other bomb flower, set down beside the rock; goron-rock's place spot was over the drop), `hs-fishing` (#151: a longer rod talk, the rod checked from B's side), `hs-skulltula-token` (#89: three ways to take the token after the kill), `pvp-lag` (`-Lag 150 -Loss 2`, emulator: both put in Hyrule Field; pvp's walk goes through the Kakariko gate), `hs-npc-anim` (#200: both put near Dampe; grading lines for `groups.py` `expect=`) and `hs-kak-letter` (#171: Zelda's Letter really shown to the guard, no flag switch).

Chunk 7 world-objects scenarios (2026-11-11, `-NoLoad`; they use the test-only `place` step to reach rooms no walk can; each file's header lists the log lines and shots that pass): `deku-ladder` (#129: A opens the slingshot room chest, the trap, and shoots the eye switch; B by the door watches the trap floor and the ladder), `deku-block` (#147: A pushes the basement block, then B pushes it on from where A left it: no snap back), `boulder` (#84: both in the Kokiri Sword crawlspace; the rolling boulder at the same place in both) and `pots-place` (#84: A lifts and throws one Kakariko pot and cuts another; B sees both go). `sign` now has B press ZL so its camera frames the post (#90).

PvP scenario (branch `pvp`, `-NoLoad`; not run yet): `pvp` (both land in Hyrule Field in front of Kakariko; A swings at B with PvP off, types `/pvp on`, swings three more times, then `/pvp off` and swings once more; the A file's header lists the log lines and shots that pass).

World-breakables scenarios (2026-11-11, `-NoLoad`): `sign` (#90) warps both to `Kakariko_GraveYardSide` and places B facing the post (the old walk left B facing a wall), `sign-late` (#90: B walks in from the Graveyard after A's cut) and `goron-rock` (#84: A throws a bomb flower at the Goron City rock `Obj_Goron_BrokenRock_A_01`; B watches it go).

Warp scenarios (run with `-NoLoad`): `warp` (both games load into Kakariko at the well) and `water` (both load into the Water Temple entrance; checked 2026-11-09: both logs `warp done` after about 50 s, screenshots in the temple). Warping a game that is already playing works only while an event runs.

Scenarios (checked 2026-11-09 unless said): `walk` (A walks, dashes, turns; B sees it move), `slingshot` (aims and fires a seed), `menu-gear` (equips the Deku Shield in the menu), `sword` (target and swing; no enemy at the save point, so no hit seen yet), `bomb` (not checked: the auto kits' save has no bombs), `buttons` (presses each button with a screenshot after it). The bindings are the game's own (pause menu > System > Basic Controls): ZL target, ZR use item, L/R quick select, MINUS map, PLUS menu (L/R change page), A actions and dash (hold), B put away, X jump, Y attack. Screenshots are the screen under each game's client area: the runner keeps both windows on top, side by side, at real pixels (DPI-aware).

### Hookshot test loop

For the integrator after each merge. Nothing here is needed to play.

1. **Compile everything:** `python -I tools/hookshot/hs.py compile-all [--emu-build <emulator Ninja build folder>]` runs the unit build (every hook file through the tests, plus `hookshot_coop.dll`), and with `--emu-build` the emulator's `lichii/hook` objects only (found with `ninja -t targets all`), without a game build. It ends with one line, `compile-all: OK` or `compile-all: FAIL`, and the last 25 lines of the failing step. It finds MSVC itself (vswhere) when it is not run from a developer prompt.
2. **Hookshot from the cache:** `python -I tools/hookshot/hs.py build --pair <kit>\hookshot-A1 --pair <kit>\hookshot-B1 ...` The built game depends only on Hookshot's tools release (`tools/release/kit/hookshot/TOOLS.txt`), `hookshot/PIN` and `hookshot/patches/*.patch`: the same lines a kit's Build-Hookshot-Coop writes to `coop-built-from.txt`. Their SHA-256 is the cache key (`hs.py key`). The cache is `%LOCALAPPDATA%\oot-coop\hs-cache\<key>\` (or `OOT_COOP_HS_CACHE`). With the key unchanged, `build` copies the cached game into each pair (changed files only; the pair's `coop.ini`, `user\`, `autotest-save\`, `mods\` and shader caches stay) and rebuilds only `hookshot_coop.dll`. On a miss it builds the game once through a made kit (`--kit <kit> --game <your game file>`, about an hour), or a game folder whose `coop-built-from.txt` matches is stored with `hs.py cache store --from <folder>`. `hs.py cache status` says HIT or MISS; old keys are pruned (`--keep`, default 3; the current one always kept). Prune removes only cache entries (20 hex digits, or a half-stored `.part`/`.build`), never anything else in that folder. A game folder's `romfs` junction is cached as a junction to the same target, not copied: deleting or rebuilding the folder it points to (a kit's work folder) leaves the cached game and the pairs without `romfs` until the game is stored again.
2b. **Shader warm-up, once per new game build, before the groups:** `python -I tools/hookshot/hs.py warmup --pair <kit>\hookshot-A1 --to <kit>\hookshot-B1 --to ... [--timeout 3600]` starts the one pair's game until it logs `vk: start-up: the shaders took N s`, then quits it through its control bridge, so that it writes its caches. It copies that pair's warm files (`hookshot_pipelines.bin` with `.built` and `.predicted`, `hookshot_prepared.bin`, `hookshot_programs.bin`/`.idx`, `hookshot_pipeline_states.bin`, `hookshot_computes.bin`, `mkw_vk_pipelines*.bin`) over the other pairs' files and into the cache entry, so a later `build` brings them too. Why: the game build prepares the shaders in a process of its own, which does not write `hookshot_pipelines.bin.built` (on NVIDIA that process's pipeline cache does not serve the game's). So every new game folder's first start builds all the noted pipelines first: 35269, about 18 minutes for one game alone. In wave 1, six games doing it at once made loads take 9 minutes and more, and they timed out. It is a game run, so it is done in the test turn. Check: a pair's next start logs `vk: precompile: ... in N ms`, not `start-up: pipelines 0 of`. A restore into a pair whose `coop-built-from.txt` names another build now replaces that pair's runtime caches too: they are that build's.
2c. **Incremental game builds (hs-build-speed):** on a cache miss, `build` builds the pin's one game folder `<cache>\pins\<release>-<commit 12>\game` again in place, with that pin's `\tools` and `\work` (`hookshot-build.ps1 -Out -Tools -Work`), whatever the kit, and stores a copy in the cache. The folder must keep its path: Hookshot's lift key hashes it (`OOT_DUMP`, `OOT_ROMFS`), and it keeps the game's code as read (`.hookshot_exefs`) and the prepared shaders. Co-op's own `OOT_COOP_*` variables are cleared for the build, so they do not change the key. A patch change puts back the files the old patches changed and applies the new ones (`apply_patch.py --refresh`: unchanged files keep their time; its list of changed files is written first, and a refresh that fails unpacks the whole source again, once). Hookshot's own lift key then skips the translation unless a lift file changed (`nx2/lift.py`, `coop_hooks.py`, ...; a change in `host/` or `gpu/` alone does not translate again), and its object cache compiles only what changed. `--jobs` defaults to Hookshot's own choice (one compiler per CPU thread, as the free memory allows). A kit made before this has no `-Tools`/`-Work`: a full build, as before. Times: `reports/hs-build-speed.md` in the coordination folder.
3. **Locks:** `build-emu.lock` (emulator) and `build-hs.lock` (unit build, `hookshot_coop`, Hookshot game) in the main checkout's root replace the one `build.lock`, so the two builds can run at the same time. `hs.py lock status|acquire|release --name emu|hs --owner <you>`; `compile-all` and `build` take them themselves. A held `build.lock` still blocks both. A game build (a cache miss) also takes `build-game.lock`, which stops both test lanes ("Two test lanes" below); `lane1.lock` and `lane2.lock` are the lanes' run locks. A lock is stale (and broken by the next taker) after 4 hours, or when its owner's process on this PC is gone: `compile-all` and `build` hold their locks in their own process; a lock taken with `hs.py lock acquire` has no process (that command ends at once) and goes stale by age only, unless `--pid <the process that builds>` is given.
4. **Parallel groups:** a group file (`tools/autotest/groups/hookshot-smoke.txt` is one) lists scenarios under `[g1]`, `[g2]`, ... with optional `switches=`, `expect=` (a regex for `hookshot_log-A.txt` / `-B.txt`, `A:` or `B:` in front), `timeout=` and `label=`. Group N plays on `<kit>\hookshot-A<N>` + `-B<N>`, server port 27560 + 10 x (N-1) and control pipes `coop-auto-g<N>-A/-B` (`run.ps1 -Pipe`), so groups do not see each other; its scenarios play one after another, the groups at the same time.
   - `python -I tools/autotest/groups.py prepare <file>` fills the pair folders (cached game, the base pair `hookshot-A`/`-B`'s `autotest-save`, `mods` and `coop-options.ini`).
   - `python -I tools/autotest/groups.py run <file> --dry-run` prints every `run.ps1` command; without `--dry-run` it plays them and writes `results.md` in `<kit>\shots-hookshot\groups-<time>\`.
   - `python -I tools/autotest/groups.py merge <folders...>` makes the one table (`| Run | Result | Evidence |`) from any saved run folders: `FAILED.txt`, a `FAIL` line in `counts.txt` or a non-zero exit is FAIL; all `expect=` found is PASS, one missing is FAIL; no `expect=` is PLAYED (look at the shots).
   - `run.ps1 -Runtime hookshot -DryRun` prints the `hookshot.py` command of one run and starts nothing.
   - Warps and marks (`hookshot.py`, ls-harness 2026-11-13): a script's `warp` is done when the game is in the map (the bridge's `wait map`, 30 s), not when the teleport is sent: Longshot rc.3 drops a teleport asked while the save's arrival event still plays (`cheats: teleport ... result 0`, Link stays where the save put him), so it is asked again until the map comes (`warp_for`, 90 s). Arrived, the player logs `coop: test warp done (map M)`, the emulator's line. Each `mark` also goes to the module's commands file, so the game's log (`hookshot_log-X.txt`, which `groups.py` grades) has `coop: test mark NAME (Link at x y z)` in order with the game's own lines; `player-X.txt` keeps its copy. Link's position for `goto`/`toward` is still read through the bridge's guest memory (`HOS_CONTROL_MEM=ro`, main + `0x4e7cfa8` -> +0x590): unchanged in Longshot rc.3 (same main build, main at `0x80020000`; a goto reached its point in the 20261113 cutscene run).
5. **Steps files on Hookshot:** `run.ps1 -Runtime hookshot -Scenario stray` (or any `<name>.steps.txt`) plays the steps file through `tools/autotest/hs_steps.py`, with the same lines as on the emulator: `start`, `load`, `play`, `close` (the game quits through its control bridge), `kill`, `server stop`/`start`, `sleep`, `password` (sent as `LICHII_COOP_PASSWORD`), `try`, `expect` (puppets and fairies from the objects debug lines), `lines` and `shot`. Each `start` puts the save baseline back and begins a fresh `hookshot_log.txt` (the old one is kept as `hookshot_log-<x>-<n>.txt`). Hookshot writes `hookshot_log.txt` only when nothing captures its output, so with the runners' capture, the log of start n is `game-out-<x>-<n>.txt`. `hookshot.py` likewise copies `game-out-<x>.txt` as `hookshot_log-<x>.txt` for `groups.py`. Results go to `counts.txt`, as on the emulator, and `groups.py merge` grades the run by them. `-ServerPassword` works here too (join-password: `password=` in a group file). `-DryRun` prints every step's plan; `python -I tools/autotest/hs_steps.py check <file>` parses one. A `lines` pattern that only .NET reads (a variable-width lookbehind) is counted through PowerShell.

### Two test lanes

One PC runs two game pairs at the same time (one Longshot game takes about 10 GB of RAM; this PC has 128 GB and a 32 GB GPU). Each lane owns everything a run touches, so two runs never meet (`python -I tools/autotest/lanes.py info`):

| | Lane 1 | Lane 2 |
|---|---|---|
| Game pair | `coop-kit\ls\lane1-A`, `-B` | `coop-kit\ls\lane2-A`, `-B` |
| Server port (+1, +2 for the bad-connection relays) | 27520 | 27530 |
| Control pipes | `coop-lane1-A`, `-B` | `coop-lane2-A`, `-B` |
| Run folders | `coop-kit\ls\shots-lane1\` | `coop-kit\ls\shots-lane2\` |
| Run lock (main checkout's root) | `lane1.lock` | `lane2.lock` |

Port 27510 is the default game port and the owner's own server: no lane uses it, nor `groups.py`'s 27560 and up.

- **Run in a lane:** `run.ps1 -Runtime hookshot -Scenario walk -Lane 1`, `python -I tools/autotest/hookshot.py run --lane 2 --script-a ...` or `python -I tools/autotest/groups.py run <file> --lane 1` (every group of the file one after another on the lane's pair). The lane fills in the pair, port, pipes and run folder (anything given on the command line wins) and holds the lane's lock for the run, held by the run's own process, so a killed run's lock goes stale by itself. A second run in the same lane waits (up to 30 min) for the first.
- **Fill a lane:** `python -I tools/autotest/lanes.py setup --lane 1 --from-a <kit>\ls\hookshot-A1 --from-b <kit>\ls\hookshot-B1` copies a built, warmed pair. Only files no game writes are hard links: `longshot.exe` and its libraries (not `hookshot_coop.dll`), `image.bin`, `longshot_pipeline_states_shipped.bin`, `romfs\` and `.longshot_exefs\`. Every cache, save, option and log is the lane's own copy. A hard-linked file a game writes made one game prepare every shader at each start (2026-11-13), so nothing written is ever shared. The source's log and a `*.warming` marker are left out. Do it while no game runs from the source pair, since it copies the shader caches too.
- **Cold shaders warm alone:** a pair from a new game build has no prepared pipelines. Its first start builds them all, which takes several GB of RAM per game. Warm it alone first with `hs.py warmup`, on the whole PC, then fill the lanes from it. Side by side, lanes run only warmed pairs: with four games up and one of them building pipelines from cold, Longshot stopped with `LLVM out of memory` (2026-11-14).
- **Check a lane:** `python -I tools/autotest/lanes.py check [--lane N]` lists a missing or unwarmed game, a hard link on a file the game writes, a link into another folder, and a stale `longshot_pipelines.bin.built.warming` left with no game running.
- **Module only:** `python -I tools/autotest/lanes.py install --lane N --dll <build>\...\hookshot_coop.dll` puts a new module into both games of the lane in seconds. It takes no build lock and refuses while that lane's game runs. It deletes the old file first and never writes into it, so a hard link could not carry the change to the other lane.
- **What stops a lane:** its own lock, and a game build: `build-game.lock` (taken by `hs.py build` on a cache miss, which also waits up to 30 min for running lanes), the old `build.lock`, or a `build-hs.lock` made by hand (its `owner.txt` is not `hs.py`'s JSON: the old one-run-at-a-time meaning). A module build (`hs.py compile-all`, or `build` on a cache hit) stops no lane. `python -I tools/autotest/lanes.py lock status` shows what blocks.
- **Turns:** `coord-multiplayer/TEST-TURN` has a `LANE1:` and a `LANE2:` line and one `QUEUE:` (rules in the briefs' common part). The offline queue (`coord-multiplayer/tools/offline-queue.ps1`) runs one job per lane at once: a job's sixth field is `1`, `2` or `any` (the command gets the lane as `{lane}` and `$env:OOT_COOP_LANE`). A job without one takes the whole PC, and so does every job that builds (`hookshot-build`, a kit or emulator build, cmake or ninja, `hs.py build` or `warmup`), whatever its field says.
- **Shader caches:** each lane folder has its own Longshot shader and pipeline caches (copied by `setup`, never replaced by `install`), and a restore only replaces them when the pair held another build. The NVIDIA driver's own cache (`%LOCALAPPDATA%\NVIDIA\GLCache`) is one per PC and stops at about 1 GB at the default size (seen on the test PC, 2026-11-13), less than one Longshot game's shaders. When lanes and kits push each other's shaders out, a start takes 2.5 to 3.5 min again. Raise it in NVIDIA Control Panel: **Manage 3D settings**, **Global Settings**, **Shader Cache Size**, **Unlimited** (or in the NVIDIA app: **Graphics**, **Global Settings**, **Shader Cache Size**).

### Which changes need a game build

The game build (about an hour, the whole PC) depends only on the three inputs of the cache key (`hs.py key`). Everything else is module only: `hs.py build` on a cache hit, or `lanes.py install`, and takes seconds.

| A branch changes | Needs |
|---|---|
| `hookshot/patches/*.patch` (game patches) | a game build (once per key; every lane and pair then takes it from the cache) |
| `hookshot/PIN` (the pinned game source commit) | a game build |
| `tools/release/kit/hookshot/TOOLS.txt` (the tools release) | a game build |
| `hookshot/src/` (`hookshot_coop.dll`), the shared `coop/` code, the server, tools, scenarios, docs | no game build: rebuild `hookshot_coop` and install it into the lanes |

Before a chunk, `hs.py cache status` says HIT (module only) or MISS (one game build first, by the integrator, with `build-game.lock`; then `hs.py warmup` once and `lanes.py setup` from the warmed pair).

## CI

GitHub Actions runs `Build` in two tiers (`.github/workflows/build.yml`), so work branches do not fill the free runners' queue:

- Fast tier, on a push to any branch: `linux (gcc)` (unit tests, `-Werror`) and `python-tools (ubuntu-latest, 3.12)`. About 2 jobs.
- Full tier, on a push to `main` or `integrate-*`, on a tag, on a pull request into `main`, and on a manual run: the fast tier plus Windows (MSVC, unit and kit script tests), linux clang, ASan + UBSan, TSan and python-tools on Windows and Python 3.10. To get the full proof for a branch before integration: Actions, Build, Run workflow, pick the branch (or `gh workflow run build.yml --ref <branch>`).
- A pull request from a branch of this repo into anything but `main` runs nothing (its push already did); a pull request from a fork runs the fast tier.
- A newer push cancels the branch's older run (not on `main` or tags). Docs-only changes (`docs/`, `*.md`, issue templates) skip `Build`; `Content guard` runs on every push.
- Caches: ccache on linux, sccache on Windows, and the fetched libraries (ENet, doctest, Monocypher) at their pinned commits. `linux-emulator` (the hook compile) is full tier too, and also runs on pushes to `linux-build` and `linux-2`.

## Bad connections

Two games on one PC talk over a perfect network: no delay, nothing lost. `tools/netsim/netsim.py` is a small UDP relay that sits between a game and the server and makes the connection bad on purpose, so sync can be watched under lag, loss and stalls without a second PC. Nothing in the game or the server changes; the game's `coop.ini` names the relay's port instead of the server's. It needs Python 3.8 or newer and nothing else.

| Option | What |
|---|---|
| `--lag MS` | added round trip (ping), half each way |
| `--jitter MS` | each packet ± up to this, each way; packets stay in order (one route) |
| `--loss PCT` | packets dropped, each way |
| `--dup PCT` | packets sent twice |
| `--reorder PCT` | packets held back past later ones (by twice the jitter, at least 10 ms; `--up reorder_gap=..` sets it) |
| `--rate KBIT` | link speed each way; a packet that would wait more than `--queue` ms (default 500) is dropped |
| `--spike MS/S` | a stall of MS ms every S seconds, both ways at once: what comes in meanwhile is held and comes out together at the end. This is what makes rubber-banding. |
| `--up`, `--down` | one direction only, as `name=value,...` (lag here is one way): `--down lag=300,loss=5,spike=500/10` |
| `--seed N` | the same drops and delays every run |

Every 10 s (`--report-every`) and at the end it prints, per client and direction, the packets in and out, lost, dropped by the queue, doubled, reordered and held by a stall, and the average and highest delay it gave.

By hand, with any server (here a kit's on 27510) and a game whose `coop.ini` says `port = 27511`:

```
python tools\netsim\netsim.py --listen 27511 --server 127.0.0.1:27510 --lag 150 --jitter 20 --loss 2 --spike 500/10
```

It listens on 127.0.0.1 only (no firewall question); `--listen 0.0.0.0:27511` lets another PC in.

### In the automatic tests

`run.ps1` starts one relay per game when any of these is given, and points that game's `coop.ini` at it (A on `-Port`+1, B on `-Port`+2; the server stays on `-Port`):

```
tools\autotest\run.ps1 -Scenario walk -Lag 150 -Jitter 20 -Loss 2 -Spike 500/10
```

`-NetSimFor A` puts only A behind the relay (B connects straight: A slow, B fine). `-NetSimArgs` passes more relay options, `A:` or `B:` for one game only: `-NetSimArgs "--seed 3","B:--down loss=10"`. The relay's Python is the recomp's `.venv` beside the repo, else `py -3` or `python` (`-Python` sets it). The run folder gets `netsim.txt` (the conditions and each relay's command line) and `netsim-A.txt` / `netsim-B.txt` (each relay's counts).

### What it does to the puppet

`coop_test` ([soak.md](soak.md)) shows the effect without the game: its clients draw each other with the same `coop/presence` code as the emulator, and its `shown error` line is how far, in game units, the drawn player is from where it really was at the time shown. The largest is the biggest correction the puppet makes when the late samples arrive: the rubber-band. One server, client `lagA` through the relay, client `okB` straight, 30 s each (`coop_test --server-only`, then one `--connect` client each; the moving player runs a circle of 100 units every 4 s, about 157 units/s):

| Relay on `lagA` | Round trip of `lagA` (avg / max ms) | `okB` draws `lagA`: error avg / max (units) | `lagA` draws `okB`: error avg / max | Frames extrapolated / held (of about 1780) |
|---|---|---|---|---|
| none (0 ms) | 14 / 17 | 0.000 / 0.08 | 0.000 / 0.00 | 3 / 0 |
| `--lag 100` | 119 / 144 | 0.003 / 0.22 | 0.002 / 0.14 | 190-240 / 0 |
| `--lag 250` | 260 / 296 | 0.26 / 1.26 | 0.26 / 1.27 | all / 0 |
| `--lag 150 --jitter 20 --loss 2` | 170 / 420 | 0.005 / 0.11 | 0.002 / 0.16 | 255-461 / 0 |
| `--lag 100 --spike 500/5` | 106 / 246 | 1.9 / **46** | 2.2 / **50** | 54 / 126-132 (36-42 faded out) |

(2026-11-11, one PC, Python 3.13, coop_test from a main build of that day. The 14 ms round trip with no relay is ENet's own, the same for a client that connects straight.)

Reading it:

- Plain lag hardly moves the puppet: the samples carry velocity, and the moving player is shown 100 ms behind the server clock. Up to about 100 ms one way it is drawn between two samples it already has; past that (250 ms round trip is 125 one way) every frame is extrapolated from the last sample, which on this smooth circle is off by about a unit. A player who turns or stops sharply is off by more, for up to 150 ms.
- 2 % loss with jitter is the same story: a lost sample is bridged by the next one.
- Stalls are what rubber-band. 500 ms with nothing arriving is longer than the 150 ms of extrapolation, so the puppet is held, fades, and when the held samples all arrive at once it jumps up to 50 units (about a third of a second of running) to where the player really is. That jump is the number to watch when changing `coop/presence` (show delay, extrapolation, catching up smoothly instead of jumping).
- The same runs in the game: `run.ps1 -Lag ... -Spike ...`, with the puppet's position read off the screenshots or `LICHII_COOP_DEBUG_CAMERA=1` (`camera debug: P<n> at (x, y, z)`, every 2 s).

deku-dungeon-2 scenarios (issues 176, 177, 129, 196; `-NoLoad`, each file's header lists the log lines and shots that pass): `deku-platforms` (B steps on the compass room's timed plate, A by the lifts: the event starts in A at once, the lifts up and down together), `deku-torch` (B steps on the torch room's plate: the torch lit and the web burning in A too; probe lines for the torch if not), `deku-water-floor` (A on the basement's dive switch, B watches: the moving floor at the same place in both) and `deku-ladder-b` (deku-ladder with B shooting the eye: A's ladder falls while B's cutscene runs).
