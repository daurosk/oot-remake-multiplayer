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

If this PC already plays the game on Fritchii (or Lichii or Eden), copy (do not move) from `%APPDATA%\eden`:

- the `keys` folder to `emulator\user\keys`
- the `nand` folder to `emulator\user\nand` (firmware, game updates and saves; a few GB)

Otherwise put your keys in `emulator\user\keys\`, start the emulator once (`Join.bat`, any address) and use **Tools > Install Firmware** and **File > Install Files to NAND** for your firmware and game update.

## 3. First start

Double-click `Join.bat`. Type `127.0.0.1` for the address for now, and a player name (1 to 16 characters, different from the other players'). The emulator opens. Add the folder with your game dump to its game list (double-click the empty list) and start the game once to see that it runs. Close the emulator.

If Windows SmartScreen says it protected your PC, click **More info**, then **Run anyway**: the kit's programs are not signed. If Windows Firewall asks about `lichii.exe` (the emulator) or `coop_server.exe` (the server), tick **Private networks** and click **Allow access**; it asks once per folder the kit is in. If it says `MSVCP140.dll` or `VCRUNTIME140.dll` is missing, install the Microsoft Visual C++ 2015-2022 Redistributable (x64).

## 4. The puppet pack

The other players appear as Link only with the puppet pack, which is made from your own game files, so it cannot come in the zip. Do this once per PC (and again after an update, when [Update.bat](#updating) says the puppet pack tool changed):

1. Start `Join.bat`, right-click the game in the list and choose **Dump RomFS**, then the full dump. It writes several GB to `emulator\user\dump\0400486024DEC000\romfs\`. Close the emulator when it is done.
2. Double-click `make-puppet-pack.cmd`. It finds the dump and builds the pack in under a minute. If it cannot find the dump, it asks for the folder that holds `Pack`, `Model`, `AS`, `RSDB` and `System`.
3. You can delete the `dump` folder afterwards.

Without the pack, co-op still works, but the other players show only as a name and a dot.

## Check it works

`Host.bat` on one PC opens a server window and the emulator. Start the game: a small panel in the top-left corner says `Co-op: player 1, ping ... ms`. The emulator's log, `emulator\user\log\eden_log.txt`, has lines starting with `coop:`, among them `coop: build ...` (which co-op build this is), `coop: welcomed as player 1` and `coop: game build e0f3ccfaf50065ae`.

Next: [play-lan.md](play-lan.md) or [play-online.md](play-online.md).

## Settings

`coop-options.ini` turns features on and off; by default the puppets, shared enemies and townspeople, and objects (seeds, nuts, bombs, arrows) are on, debug logging off. Everyone should use the same settings. Your server address and name are in `emulator\user\config\coop.ini`; `Join.bat` asks for the address each time, and `play.cmd change` asks for both again.

## Updating

Every player needs the same version: a game of one version cannot join a server of another.

**Kits from v0.1.0-test3 on:** close the emulator and the server, then double-click `Update.bat` in the kit folder. It looks for the newest release on the [Releases](https://github.com/daurosk/oot-remake-multiplayer/releases) page, says which one, and asks before it changes anything. Then it downloads the zip, checks it against the SHA-256 published with the release, and replaces the kit's own files. Host.bat and Join.bat say at start when a newer release is out.

**A v0.1.0-test2 kit** has no `Update.bat` yet: download `Update.bat` from the newest release once, put it in the kit folder beside `Host.bat`, and double-click it (Windows may warn about a file from the internet the first time: **More info**, then **Run anyway**). From then on the kit has its own. (Or download the whole zip once more and copy your `emulator\user` folder into it.)

What it keeps: everything in `emulator\user\` (keys, firmware, saves, settings, your name and server, the puppet pack), `logs\`, any file of yours that is not part of the kit, and `coop-options.ini`: your switches stay as they are, and a switch the new version adds is written at the end behind a `#` (off). Every file it replaces goes first to `backup-<old version>\` in the kit folder; if something fails half way, those are put back. Delete the backup folder once the new version works.

It sends nothing about you: one request for the list of releases, and the two downloads. If it says the puppet pack tool changed, run `make-puppet-pack.cmd` once more.

## Reporting a problem

Close the emulator, double-click `collect-logs.cmd` and send the zip it makes, with what you did and what went wrong, on the repo's issue page.
