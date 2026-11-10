# Setting up the co-op test release

This is an early test build (v0.1.0-test2) of online co-op for the Ocarina of Time remake on the Fritchii emulator. Expect bugs; [known-issues.md](known-issues.md) lists what works and what does not yet. After this page, [play-lan.md](play-lan.md) (same network) or [play-online.md](play-online.md) (over the internet) says how to play together.

## What you need

- A Windows 10 or 11 PC (64-bit) that can run the game on Fritchii.
- Your own copy of the game, dumped from your own console, with its keys (`prod.keys`, `title.keys`) and the console's firmware. The release has no game files, keys or firmware and never will. Every player needs their own.
- The same game version as the other players (build id `e0f3ccfaf50065ae`; the emulator's log names it, see [below](#check-it-works)).
- The same release of the co-op kit as the other players.

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

The other players appear as Link only with the puppet pack, which is made from your own game files, so it cannot come in the zip. Do this once per PC (and again after an update, when [Update.bat](#updating) says the puppet pack tool changed):

**If your game is a `.nup` file** (most players): double-click `make-puppet-pack.cmd`. It finds the `.nup` in the emulator's game list folders (step 3), or asks for it (type its path, or drag the file into the window). It reads the game files straight from the `.nup`, so it needs no keys and no dump, and builds the pack in under a minute.

**If your game is an `.nsp` or `.xci`** (or the tool cannot use your `.nup`), dump its RomFS first:

1. Check first: the game starts in the kit's emulator (step 3), and the game **update** is installed in the kit's emulator (step 2), not only in another emulator. Without it you dump the old base game, which co-op cannot use.
2. Start `Join.bat`, right-click the game in the list and choose **Dump RomFS**. If it asks which RomFS, pick **Program [0400486024DEC000]**. Then pick **Full** (not Skeleton). It writes several GB to `emulator\user\dump\0400486024DEC000\romfs\` and needs that much free space plus 1 GB. Keep the progress window open until it says **RomFS Extraction Succeeded!**; then close the emulator.
3. Double-click `make-puppet-pack.cmd`. It checks the dump and builds the pack. If something is wrong it says what and how to fix it (see [Troubleshooting](#troubleshooting)). If it finds nothing, it asks for the `.nup` or the folder that holds `Pack`, `Model`, `AS`, `RSDB` and `System`.
4. You can delete the `dump` folder afterwards.

Without the pack, co-op still works, but the other players show only as a name and a dot.

## Check it works

`Host.bat` on one PC opens a server window and the emulator. Start the game: a small panel in the top-left corner says `Co-op: player 1, ping ... ms`. The emulator's log, `emulator\user\log\eden_log.txt`, has lines starting with `coop:`, among them `coop: build ...` (which co-op build this is), `coop: welcomed as player 1` and `coop: game build e0f3ccfaf50065ae`.

Next: [play-lan.md](play-lan.md) or [play-online.md](play-online.md).

## Settings

`coop-options.ini` turns features on and off; by default the puppets, shared enemies and townspeople, and objects (seeds, nuts, bombs, arrows) are on, debug logging off. Everyone should use the same settings. Your server address and name are in `emulator\user\config\coop.ini`; `Join.bat` asks for the address each time, and `play.cmd change` asks for both again.

## Updating

Every player needs the same version: a game of one version cannot join a server of another.

**On every start** (a kit whose `coop-options.ini` has `auto_update`): when `Host.bat`, `Join.bat` or `play.cmd` start and a newer co-op release is out, they ask "Update now? [Y/n]" (yes after 10 s with no key, so an unattended start updates too). Yes updates the whole kit, the emulator included, then starts the new version. No, no network, or GitHub down: this version starts as it is, after at most 6 s. If the update fails, the old files are put back and this version starts. `auto_update` in `coop-options.ini` sets it: `ask` (the default), `yes` (update without asking) or `no` (only say that one is out).

**By hand:** close the emulator and the server, then double-click `Update.bat` in the kit folder. It looks for the newest release on the [Releases](https://github.com/daurosk/oot-remake-multiplayer/releases) page, says which one, and asks before it changes anything. Then it downloads the zip, checks it against the SHA-256 published with the release, and replaces the kit's own files.

The emulator is updated only this way, with the co-op build from our release. Fritchii's own updater is switched off in our builds and never used: it would put a plain Fritchii without co-op over the kit's. When Frizti publishes a newer Fritchii, the next co-op release moves to it.

**A v0.1.0-test2 kit** has no `Update.bat` yet: download `Update.bat` from the newest release once, put it in the kit folder beside `Host.bat`, and double-click it (Windows may warn about a file from the internet the first time: **More info**, then **Run anyway**). From then on the kit has its own. (Or download the whole zip once more and copy your `emulator\user` folder into it.)

What it keeps: everything in `emulator\user\` (keys, firmware, saves, settings, your name and server, the puppet pack), `logs\`, any file of yours that is not part of the kit, and `coop-options.ini`: your switches stay as they are, and a switch the new version adds is written at the end behind a `#` (off). Every file it replaces goes first to `backup-<old version>\` in the kit folder; if something fails half way, those are put back. If the update is stopped (window closed, power cut), run `Update.bat` again: it finishes it (Host.bat and Join.bat remind you). Delete the backup folder once the new version works.

It sends nothing about you: one request for the list of releases, and the two downloads. If it says the puppet pack tool changed, run `make-puppet-pack.cmd` once more.

## Troubleshooting

- **"RomFS Extraction Failed! There was an error copying the RomFS files or the user cancelled the operation." (step 4).** With a `.nup` game you do not need Dump RomFS: run `make-puppet-pack.cmd` and give it the `.nup`. Otherwise: the emulator shows this one message for nearly every failure and writes nothing about the dump itself in its log. In order:
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
- **Where is the log?** `emulator\user\log\eden_log.txt` in your kit folder. Log lines name the emulator's source files, such as `src\core\...`. In test3 and older kits they show a full folder such as `C:\...\src\...`: that is where the emulator was built, not your folder, and it is not a problem (from test4 on, only the `src\...` part is shown).
- **Still stuck?** Close the emulator, run `collect-logs.cmd` and report it (below).

## Reporting a problem

Close the emulator, double-click `collect-logs.cmd` and send the zip it makes, with what you did and what went wrong, on the repo's issue page.
