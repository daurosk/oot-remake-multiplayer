# Setting up the co-op test release

This is an early test build of online co-op (the newest is on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases)) for the Ocarina of Time remake on the Fritchii emulator. Expect bugs; [known-issues.md](known-issues.md) lists what works and what does not yet. After this page, [play-lan.md](play-lan.md) (same network) or [play-online.md](play-online.md) (over the internet) says how to play together.

## What you need

- A Windows 10 or 11 PC (64-bit) that can run the game on Fritchii.
- Your own copy of the game, dumped from your own console, with its keys (`prod.keys`, `title.keys`) and the console's firmware. The release has no game files, keys or firmware and never will. Every player needs their own.
- The same game version as the other players (build id `e0f3ccfaf50065ae`; the emulator's log names it, see [below](#check-it-works)).
- The same release of the co-op kit as the other players.
- **No code or cheat mods.** Mods that change the game's code (an `exefs` folder: cheat menus, gameplay patches) and cheats (`cheats` folders, `cheat_*` files) are not supported in co-op. Remove them, or turn them off for the game (in the emulator, right-click the game, Properties, Add-Ons, untick them). **Texture and model mods are fine** (only a `romfs` folder, for example fixes for missing Water Temple or Fairy Fountain textures); if one changes models, every player should use the same ones. Fritchii's built-in Dynamic FPS is not a mod, and neither is co-op's own puppet pack (`coop-puppet`, step 4).

## 1. Unzip

Unzip `oot-coop-<version>.zip` into a short folder you can write to, for example `C:\oot-coop` or your desktop. Keep the path short: the puppet pack (step 4) stops if the folder is too deep.

You get:

| File | What it is for |
|---|---|
| `Host.bat` | Start a game others join: starts the server and your emulator |
| `Join.bat` | Join someone's game: asks for the host's address |
| `make-puppet-pack.cmd` | Builds the puppet Link from your own game files (once) |
| `coop-options.ini` | Which co-op features are on |
| `collect-logs.cmd` | Zips your logs to send with a bug report |
| `start-server.cmd`, `play.cmd` | The server alone, and the emulator alone (for advanced setups) |
| `emulator\` | The patched emulator; `emulator\user\` holds all its data |
| `SOURCE.txt`, `licenses\`, `THIRD_PARTY_NOTICES.md` | Where the source is, and the licences |

## 2. Your keys, firmware and saves

The kit's emulator is portable: it keeps everything in `emulator\user\` and never touches a normal Fritchii, Lichii or Eden install.

If this PC already plays the game on Fritchii (or Lichii or Eden), copy (do not move) your `keys` and `nand` folders into the kit:

- the `keys` folder to `emulator\user\keys`
- the `nand` folder to `emulator\user\nand` (firmware, game updates and saves; a few GB)

Where to find them:

- **A normal install** keeps them in `%APPDATA%\eden` (paste that into the Explorer address bar).
- **A portable install** (a Fritchii, Lichii or Eden you unzipped, with a `user` folder next to its exe) keeps them in that folder: `user\keys` and `user\nand` next to the exe. `%APPDATA%\eden` is then empty or missing.
- **Not sure which?** Search your drives for `prod.keys` in Explorer. The folder it is in is the `keys` folder; the `nand` folder is beside it.

Otherwise put your keys in `emulator\user\keys\`, start the emulator once (`Join.bat`, any address) and use **Tools > Install Firmware** and **File > Install Files to NAND** for your firmware and game update.

## 3. First start

Double-click `Join.bat`. Type `127.0.0.1` for the address for now, and a player name (1 to 16 characters, different from the other players'). The emulator opens. Add the folder with your game dump to its game list (double-click the empty list) and start the game once to see that it runs. Close the emulator.

If Windows SmartScreen says it protected your PC, click **More info**, then **Run anyway**: the kit's programs are not signed. If Windows Firewall asks about `lichii.exe` (the emulator) or `coop_server.exe` (the server), tick **Private networks** and click **Allow access**; it asks once per folder the kit is in. If it says `MSVCP140.dll` or `VCRUNTIME140.dll` is missing, install the Microsoft Visual C++ 2015-2022 Redistributable (x64).

## 4. The puppet pack

**Most players: your game is a `.nup`. Just double-click `make-puppet-pack.cmd`. No keys, no Dump RomFS.**

1. Double-click `make-puppet-pack.cmd` in the kit folder.
2. It finds the `.nup` in the emulator's game list folders (step 3). If it asks for it, type its path or drag the file into the window.
3. Wait for it to finish: it reads the game files straight from the `.nup` and builds the pack in under a minute.

Do not use the emulator's **Dump RomFS** for a `.nup`: you do not need it, and it often fails with "RomFS Extraction Failed" (for example when your keys are older than the game's update).

**Which file do you have?** Look at the file you added to the emulator's game list in step 3: its name ends in `.nup`, `.nsp` or `.xci` (the game list also shows it in its **File type** column: NUP, NSP or XCI). `.nup`: the steps above are all you need. `.nsp` or `.xci`: see below.

The other players appear as Link only with the puppet pack, which is made from your own game files, so it cannot come in the zip. Do this once per PC (and again after an update, when [Update.bat](#updating) says the puppet pack tool changed). Without the pack, co-op still works, but the other players show only as a name and a dot.

### Only if your game is an `.nsp` or `.xci`: dump the RomFS first

<details>
<summary>Dump steps for an <code>.nsp</code> or <code>.xci</code> (or when the tool cannot use your <code>.nup</code>)</summary>

1. Check first: the game starts in the kit's emulator (step 3), and the game **update** is installed in the kit's emulator (step 2), not only in another emulator. Without it you dump the old base game, which co-op cannot use.
2. Start `Join.bat`, right-click the game in the list and choose **Dump RomFS**. If it asks which RomFS, pick **Program [0400486024DEC000]**. Then pick **Full** (not Skeleton). It writes several GB to `emulator\user\dump\0400486024DEC000\romfs\` and needs that much free space plus 1 GB. Keep the progress window open until it says **RomFS Extraction Succeeded!**; then close the emulator.
3. Double-click `make-puppet-pack.cmd`. It checks the dump and builds the pack. If something is wrong it says what and how to fix it (see [Troubleshooting](#troubleshooting)). If it finds nothing, it asks for the `.nup` or the folder that holds `Pack`, `Model`, `AS`, `RSDB` and `System`.
4. You can delete the `dump` folder afterwards.

</details>

## Check it works

`Host.bat` on one PC opens a server window and the emulator. Start the game: a small panel in the top-left corner says `Co-op: player 1, ping ... ms`. The emulator's log, `emulator\user\log\eden_log.txt`, has lines starting with `coop:`, among them `coop: build ...` (which co-op build this is), `coop: welcomed as player 1` and `coop: game build e0f3ccfaf50065ae`.

Next: [play-lan.md](play-lan.md) or [play-online.md](play-online.md).

## Settings

`coop-options.ini` turns features on and off; by default the puppets, shared enemies and townspeople, and objects (seeds, nuts, bombs, arrows) are on, debug logging off. Everyone should use the same settings. Your server address and name are in `emulator\user\config\coop.ini`; `Join.bat` asks for the address each time, and `play.cmd change` asks for both again.

## Chat commands

Press Enter in the game to open the chat box. A line that starts with `/teleport` (or `/tp`) is a command, not chat:

- `/teleport` alone lists the other players.
- `/teleport <player>` takes you to that player. The whole name works in any case, and so does the start of a name if only one player's name starts that way, or the player's number (`/teleport 2`).
  - The other player's game answers with where they last stood on their feet, so you never land in a wall or in mid-air.
  - In the same scene you are moved there at once.
  - In another scene the game changes scene first, as a loading zone does: a fade and a load, landing at that scene's usual entrance. After the load you are moved next to them. A scene change not taken in 60 seconds is given up.
  - Refused, with a line in the chat box: while your game loads or plays a cutscene or a talk, during your boss fight, while the other player is busy (loading, a cutscene, a talk, in the air) or in a boss fight, and when the other player is in a place only for the other age (`_Child` or `_Adult`) than your Link. A dungeon gives a warning: you may need its items to get on from there.
  - A player with a build from before test8 does not answer: "no answer from" after 5 seconds.

`LICHII_COOP_TELEPORT=0` turns it off in your game: the command says so, and the others' requests to come to you are refused.

## Updating

Every player needs the same version: a game of one version cannot join a server of another.

**On every start** (a kit whose `coop-options.ini` has `auto_update`): when `Host.bat`, `Join.bat` or `play.cmd` start and a newer co-op release is out, they ask "Update now? [Y/n]" (yes after 10 s with no key, so an unattended start updates too). Yes updates the whole kit, the emulator included, then starts the new version. No, no network, or GitHub down: this version starts as it is, after at most 6 s. If the update fails, the old files are put back and this version starts. `auto_update` in `coop-options.ini` sets it: `ask` (the default), `yes` (update without asking) or `no` (only say that one is out).

**By hand:** close the emulator and the server, then double-click `Update.bat` in the kit folder. It looks for the newest release on the [Releases](https://github.com/daurosk/oot-remake-multiplayer/releases) page, says which one, and asks before it changes anything. Then it downloads the zip, checks it against the SHA-256 published with the release, and replaces the kit's own files.

The emulator is updated only this way, with the co-op build from our release. Fritchii's own updater is switched off in our builds and never used: it would put a plain Fritchii without co-op over the kit's. When Frizti publishes a newer Fritchii, the next co-op release moves to it.

**A v0.1.0-test2 kit** has no `Update.bat` yet: download `Update.bat` from the newest release once, put it in the kit folder beside `Host.bat`, and double-click it (Windows may warn about a file from the internet the first time: **More info**, then **Run anyway**). From then on the kit has its own. (Or download the whole zip once more and copy your `emulator\user` folder into it.)

What it keeps: everything in `emulator\user\` (keys, firmware, saves, settings, your name and server, the puppet pack), `logs\`, any file of yours that is not part of the kit, and `coop-options.ini`: your switches stay as they are, and a switch the new version adds is written at the end behind a `#` (off). When the new release brings a different emulator, the old shader cache (`emulator\user\cache\shader\`) is moved to the backup too, because a cache made by another emulator build can crash it; the first minutes of play may stutter while a new one is built. With the same emulator it is kept. Every file it replaces goes first to `backup-<old version>\` in the kit folder; if something fails half way, those are put back. If the update is stopped (window closed, power cut), run `Update.bat` again: it finishes it (Host.bat and Join.bat remind you). Delete the backup folder once the new version works.

It sends nothing about you: one request for the list of releases, and the two downloads. If it says the puppet pack tool changed, run `make-puppet-pack.cmd` once more.

## Chat notices

When another player's progress reaches your game, a line in the chat says so (only in your game, nothing is sent): "Tobi got the Fairy Slingshot - you got one too!", "Tobi found a Small Key (Deku Tree) - the group has 2", "Tobi got a Heart Container - your max health went up", "Tobi got the map (Deku Tree)", "Tobi learned Zelda's Lullaby", "Tobi got a Gold Skulltula token (12)", "Tobi opened a chest". Joining a session says one line ("Synced with the group: 14 items, 3 songs"), and so does the host loading a save. At most 4 lines in 3 seconds; the rest is one "...and N more" line. To turn them off, set `LICHII_COOP_NOTIFY=0`.

## Saves

The host's save decides. Everyone's progress is shared while you play, but when the host loads a save, every game in the session goes back to that save's progress: items, keys, chests, doors and switches. A guest who loads a save gets the host's progress, and nothing changes for the others. To undo something (a door that locked you out, a key used in the wrong place), the host loads an older save. Rupees, ammo, hearts and where you stand stay your own. Be careful: if the host restarts the game and loads its save, **everything the guests did since the host last saved is undone** for everyone: chests, keys, doors, items. The host should save often, especially after the guests make progress.

## Troubleshooting

- **"These mods for Ocarina of Time change the game's code or are cheats"** when you start `Host.bat`, `Join.bat` or `play.cmd`: the folders or files it names are code or cheat mods (in `emulator\user\load\0400486024DEC000\`, or the SD card's `atmosphere\contents\0400486024DEC000\`). Code and cheat mods are not supported in co-op; texture and model mods are fine. Remove the named ones, or turn them off in the game's Add-Ons (right-click the game, Properties, Add-Ons), then start again.
- **The co-op panel says "Co-op is off: a code mod is installed"**: a mod changed the game's code where co-op hooks in, so co-op left its code out and did not connect. The log says where (`coop: off: a code mod is installed (main+0x...)`). Turn the mod off as above and restart the emulator.
- **"RomFS Extraction Failed! There was an error copying the RomFS files or the user cancelled the operation." (step 4).** First check which game file you have ([step 4](#4-the-puppet-pack)): if it ends in `.nup`, you do not need Dump RomFS at all. Double-click `make-puppet-pack.cmd` and give it the `.nup`. Only with an `.nsp` or `.xci`: the emulator shows this one message for nearly every failure and writes nothing about the dump itself in its log. In order:
  - Your keys are older than the game's update. The log (`emulator\user\log\eden_log.txt`) then has `Cannot decrypt ...: keys for gen 0x17+ required.`, and `make-puppet-pack.cmd` says **"Your prod.keys are too old for this game's update"**. Gen 0x17 needs `master_key_16` in `prod.keys`; keys that stop at `master_key_15` cannot decrypt the update. The game can still run from a `.nup`, which is not encrypted. Fix: build the pack from the `.nup` (step 4), or dump newer keys from your console.
  - It came at once, before any progress bar: the emulator cannot read the game. Check that `emulator\user\keys\prod.keys` exists (the "No keys" item below), the firmware is installed, and the game starts in this emulator.
  - You pressed Cancel or closed a window: the dump-mode question, the "which RomFS" list, or the progress window. Dump again and keep the progress window open.
  - It came part way through: writing failed (disk full, the kit on a USB stick that was pulled out, or a too-deep folder). Free up space or move the kit to a short folder such as `C:\oot-coop`, then dump again.
- **"There is not enough free space at ... to extract the RomFS" (step 4).** The full dump needs the game's size plus 1 GB on the kit's drive. Free up space, or move the kit to another drive, then dump again.
- **make-puppet-pack.cmd says ".nup ... is damaged", "is cut short" or "has no ...".** The `.nup` file is incomplete (a copy or download that did not finish) or does not hold the whole updated game. Copy it again from where you made it.
- **make-puppet-pack.cmd says "Found no .nup game file ... and no RomFS dump".** Give it your `.nup` (type the path or drag it in). With an `.nsp`/`.xci`: the dump did not finish (it deletes its folder when it fails), or it went to another folder (**Emulation > Configure > System > Filesystem > Dump Root**; the tool looks there too). It lists what usually stops the dump: no keys, nothing in the NAND, little free space, a deep folder. Fix those and dump again, or type the folder of a dump you have elsewhere.
- **"... has folders but no files: that is a Skeleton dump."** You picked Skeleton. Delete the folder it names and dump again with **Full**.
- **"The dump is not complete: no ..." or "The dump has empty or damaged files".** The dump stopped part way (emulator closed, crashed, PC slept, disk filled). Delete the folder it names and dump again; keep the progress window open until it says it succeeded.
- **"... is not the game's RomFS ... the manual or legal text was dumped instead."** In the "which RomFS" list you picked HTML document or Legal information. Delete the folder and dump again, picking **Program [0400486024DEC000]**.
- **"This dump is game data version 100; co-op needs 111".** The kit's emulator does not have the game update, so it dumped the base game. Install the update (**File > Install Files to NAND**, or copy your `nand` folder, step 2), check the game starts as the updated version, delete the dump and dump again.
- **"Python is missing the zstandard package" or "No Python found".** Do what it says (a Command Prompt line), then run `make-puppet-pack.cmd` again.
- **"The kit folder is too deep for Windows".** Move the whole kit folder to a short path such as `C:\oot-coop` and run it again.
- **Still failing?** Comment on [issue 115](https://github.com/daurosk/oot-remake-multiplayer/issues/115) with the exact message (a screenshot of the box), everything `make-puppet-pack.cmd` printed, your `VERSION.txt`, and the zip from `collect-logs.cmd`.
- **"No keys", or the game does not show in the list or does not start.** The kit's emulator does not see your keys. Check that `emulator\user\keys\prod.keys` exists (not one folder deeper) and that you copied from the right place (step 2: a portable install keeps them next to its exe, not in `%APPDATA%\eden`). Restart the emulator after copying.
- **The emulator crashes after an update, or after copying files from another emulator: delete the shader cache.** Close the emulator and delete the folder `emulator\user\cache\shader` in your kit folder (only that folder: your saves, keys and settings are elsewhere in `emulator\user`). The emulator builds a new cache; the first minutes of play may stutter. From another emulator copy only the keys, firmware and `nand` (step 2), never its `cache` folder.
- **The emulator closes by itself on a PC with an AMD graphics card** (while building shaders, or a few seconds after the game starts), and Windows' Event Viewer (Windows Logs > Application, Application Error) names `amdvlk64.dll`. AMD's driver crashes with the emulator's shader libraries ([issue 118](https://github.com/daurosk/oot-remake-multiplayer/issues/118)). From test4.2 on, `Host.bat`, `Join.bat` and `play.cmd` turn them off when the PC has an AMD card and no NVIDIA card, and say `AMD GPU found: shader libraries off (EDEN_NX2_GPL=0)`. With both an AMD and an NVIDIA card the game runs on NVIDIA and they stay on; if it still crashes in `amdvlk64.dll`, add the line `EDEN_NX2_GPL=0` to `coop-options.ini`. `EDEN_NX2_GPL=1` there turns them back on. Older kits: make a `Host-AMD.bat` next to `Host.bat` with the three lines `@echo off`, `set EDEN_NX2_GPL=0`, `call "%~dp0Host.bat" %*` and start that instead.
- **Over Hamachi, Radmin VPN, ZeroTier or Tailscale the game connects, then says "connection lost, reconnecting" every 15-20 seconds** ([issue 143](https://github.com/daurosk/oot-remake-multiplayer/issues/143)). Ten seconds with nothing from the other side ends a connection. Check in this order:
  1. **Windows Firewall on Public networks.** Windows puts a VPN's network adapter in the **Public** network type, and the first-start question above allows only **Private**. On **every** PC (the host and each player): Start, type `Allow an app through Windows Firewall`, click **Change settings**, find **every** `coop_server.exe` and `lichii.exe` entry (there is one per kit folder; a name can appear more than once) and tick **both Private and Public**. No entry: **Allow another app...**, browse to the kit's `server\coop_server.exe`, then again to `emulator\lichii.exe`.
  2. **The same version everywhere, test7 or later.** From test7 on, the server and the game send packets small enough for a VPN or a tunnelled home line (1100 bytes; the server window says `datagrams of 1100 bytes`). Older kits sent 1392-byte packets, more than Hamachi carries in one piece. Run `Update.bat` on every PC.
     - **Without a VPN, the same drops:** some internet lines carry less than the usual 1500 bytes per packet. **DS-Lite** (common with cable and fibre providers that give an IPv6 line with shared IPv4, often behind an OpenWrt or Fritz!Box router) and **PPPoE** (DSL) lines put each packet in a tunnel, and a small tunnel (one player's was 1200 bytes) drops bigger packets. The test7 packets (1100 bytes) fit; older kits do not.
  3. **Join with the host's VPN address** (Hamachi's `25.x.x.x`, Radmin's `26.x.x.x`), not its home address.
  4. Still dropping: run `collect-logs.cmd` on **both** PCs and report it with each `VERSION.txt`. The game's log says `coop: no packets from the server for ... ms` when the server's packets stop arriving, and each `coop: connection lost; reconnecting (...)` line says how long it heard nothing, the round trip and the packet size.
- **Where is the log?** `emulator\user\log\eden_log.txt` in your kit folder. Log lines name the emulator's source files, such as `src\core\...`. In test3 and older kits they show a full folder such as `C:\...\src\...`: that is where the emulator was built, not your folder, and it is not a problem (from test4 on, only the `src\...` part is shown).
- **Still stuck?** Close the emulator, run `collect-logs.cmd` and report it (below).

## Reporting a problem

Close the emulator, double-click `collect-logs.cmd` and send the zip it makes, with what you did and what went wrong, on the repo's issue page.
