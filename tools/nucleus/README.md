# Nucleus Co-op handler for OoT Co-op

`OoTCoop.js` is a handler for [Nucleus Co-op](https://github.com/SplitScreen-Me/splitscreenme-nucleus), a free Windows tool that runs several copies of a PC game split-screen on one PC, each with its own controller. With it, 2 to 4 players on one PC start OoT Co-op on Longshot in one click: player 1's game hosts on this PC, and the others join it.

How to play with it: [docs/play-lan.md, "Split-screen with Nucleus Co-op"](../../docs/play-lan.md#split-screen-with-nucleus-co-op).

The handler is not on the Nucleus handler hub. Install it by hand, as described below.

## Install

1. Get Nucleus Co-op from its [releases](https://github.com/SplitScreen-Me/splitscreenme-nucleus/releases) (`NucleusApp.zip`, version 2.4.2 or later) and unzip it to a short folder, for example `C:\Nucleus`.
2. Copy `OoTCoop.js` into its `handlers` folder (`C:\Nucleus\handlers\OoTCoop.js`).
3. Start `NucleusCoop.exe`, choose **Add game manually** (or the **+** button) and pick the `longshot.exe` of your kit's co-op game. That is `<kit>\hookshot\game\longshot.exe`, or the folder that `hookshot\game-folder.txt` names. Nucleus lists it as **OoT Co-op (Longshot)**.

## What it does at each start

For each player, Nucleus makes an instance folder (`C:\Nucleus\content\OoTCoopLongshot\Instance0..3`) with links to the game's files. To make those links, Nucleus runs its StartGame helper as administrator, so Windows asks for admin rights once. The instance folders are kept afterwards (`Game.KeepSymLinkOnExit`), so later starts make no links and do not ask. Then `Game.Play` writes these files into each instance folder:

| File | What it holds |
|---|---|
| `coop.ini` | `server = 127.0.0.1`, port 27590, the player's Nucleus nickname (1 to 16 characters, else `Player N`), and the session's join password (new each session, made by player 1, kept in `nucleus-players\session.txt` beside the game) |
| `coop-options.ini` | The kit's `coop-options.ini` (or the game's own copy), plus this game's controller: `LICHII_COOP_PAD=<Nucleus's pad number>` and `LICHII_COOP_KEYBOARD=off`. The keyboard player gets `LICHII_COOP_PAD=none` and `LICHII_COOP_KEYBOARD=focus`. Players 2 to 4 have no `color` line, so each gets its slot's colour |
| `user` | A junction (no admin needed). Player 1's points to the game's own `user` folder, so player 1 plays their own saves. Player N's (N from 2 to 4) points to `nucleus-players\playerN\user` beside the game. A guest plays from its co-op save folder, which is filled with the host's save when it joins |
| `player-key.txt` | Player 1's own key, or a key kept for player N in `nucleus-players\playerN` (the Lobby's mutes and bans follow it) |
| `longshot_options.ini` | A copy of the game's own, with `fullscreen=0` (Nucleus places the windows) and `discord_status=0` |

Player 1's start also opens the kit's `server\coop_server.exe --port 27590 --bind 127.0.0.1 --max-players 4 --password <session password>`. The server listens on this PC only, so Windows shows no firewall prompt and nobody else can join. When the session ends, Nucleus closes every `coop_server.exe` on this PC (`Game.KillProcessesOnClose`).

Each game reads its own controller and keeps reading it while another window is in front (patch 0015, `LICHII_COOP_PAD`). So the handler turns off all of Nucleus's input hooks (no x360ce or XInput DLLs in the game folder). The module reads `coop-options.ini` beside `longshot.exe` because the instance folder has no kit of its own (`CoopTitle::ApplyKitOptions`). A kit counts only when it is the game's own: the game is its `hookshot\game`, or `OOT_COOP_KIT` or `coop-kit.txt` names it. So Nucleus unzipped inside a kit folder works too. A co-op game built before that change gives every copy the same controller: build it again (`Build-Hookshot-Coop.bat`).

What stays linked: the game's big files (`longshot.exe`, `romfs`, `mods` and the shader and pipeline caches). The game saves a cache by writing a `.tmp` file and moving it over the old one. That puts a file of the instance's own in its folder and leaves the original as it was, so two games never write the same file.

## Test

Windows only, nothing to install:

```
cscript //nologo //E:jscript tools\nucleus\test_handler.js
```

It runs `Game.Play` for three players (two controllers and one keyboard) against a stand-in of Nucleus's `Context` on a made-up kit in `%TEMP%`, then checks every file above, including real junctions. It prints `ok: N checks` and exits with 0 when all pass. It cannot check Nucleus itself: how it places the windows, its pad numbers, and the admin prompt.
