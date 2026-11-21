# Nucleus Co-op handler for Squadala!

`OoTCoop.js` is a handler for [Nucleus Co-op](https://github.com/SplitScreen-Me/splitscreenme-nucleus), a free Windows tool that runs several copies of a PC game split-screen on one PC, each with its own controller. With it, 2 to 4 players on one PC start Squadala! on Longshot in one click: player 1's game hosts on this PC, and the others join it. Friends on other PCs can join the same session too, with the invite code that the server window shows. When another PC hosts, the option **This PC: Join a game on this network** makes every game on this PC join that host instead.

How to play with it: [docs/play-lan.md, "Split screen (experimental)"](../../docs/play-lan.md#split-screen-experimental).

The handler is not on the Nucleus handler hub. Install it by hand, as described below. Each kit has a copy in its `nucleus` folder (with a short `README.txt`).

## Install

1. Get Nucleus Co-op from its [releases](https://github.com/SplitScreen-Me/splitscreenme-nucleus/releases) (`NucleusApp.zip`, version 2.4.2 or later) and unzip it to a short folder, for example `C:\Nucleus`.
2. Copy `OoTCoop.js` (the kit's `nucleus\OoTCoop.js`, or this folder's) into its `handlers` folder (`C:\Nucleus\handlers\OoTCoop.js`).
3. Start `NucleusCoop.exe`, click the **+** button (**Add game**; not the handler hub, which does not have this handler) and pick the `longshot.exe` of your kit's co-op game. That is `<kit>\hookshot\game\longshot.exe`, or the folder that `hookshot\game-folder.txt` names. Nucleus lists it as **OoT Co-op (Longshot)**.

## What it does at each start

For each player, Nucleus makes an instance folder (`C:\Nucleus\content\OoTCoopLongshot\Instance0..3`) with links to the game's files. To make those links, Nucleus runs its StartGame helper as administrator, so Windows asks for admin rights once. The instance folders are kept afterwards (`Game.KeepSymLinkOnExit`), so later starts make no links and do not ask. Then `Game.Play` writes these files into each instance folder:

| File | What it holds |
|---|---|
| `coop.ini` | The session's server and port (below), the player's name (the one typed in the game last time, else the Nucleus nickname, 1 to 16 characters, else `Player N`) and look, and the session's join password. Player 1's start makes the session and keeps it in `nucleus-players\session.txt` beside the game (a line each: password, server, port). With **Join a game on this network**: `join_lan` and the name only (below) |
| `coop-options.ini` | This game's own from its last start (the settings and look saved in the game stay), else the kit's `coop-options.ini` (or the game's own copy); remove it to start from the kit's again. Plus this game's controller: `LICHII_COOP_PAD=<Nucleus's pad number>` and `LICHII_COOP_KEYBOARD=off`. The keyboard player gets `LICHII_COOP_PAD=none` and `LICHII_COOP_KEYBOARD=focus`. Players 2 to 4 have no `color` line, so each gets its slot's colour |
| `user` | A junction (no admin needed). Player 1's points to the game's own `user` folder, so player 1 plays their own saves. Player N's (N from 2 to 4) points to `nucleus-players\playerN\user` beside the game. A guest plays from its co-op save folder, which is filled with the host's save when it joins |
| `player-key.txt` | Player 1's own key, or a key kept for player N in `nucleus-players\playerN` (the Lobby's mutes and bans follow it) |
| `longshot_options.ini` | A copy of the game's own, with `fullscreen=0` (Nucleus places the windows) and `discord_status=0`. A controller player's also gets `mouse_camera=0` (Mouse camera **As the stick**, not **Native**): the native mouse camera puts its last angles back on the camera each frame, over the right stick's. The keyboard player keeps their own choice |
| `logs` | Not linked (`Game.DirExclusions`): each game makes its own at its start, so two games never write the same `longshot_log.txt` and co-op logs. An instance folder made by an older handler kept a link to the game's `logs` folder: the handler removes that link (the logs it pointed to stay) |

Player 1's start also runs the kit's `start-server.cmd <port>` in a window of its own (`cmd /c start "Squadala server"`), the same server start as `Host.bat` (`scripts\start-server.ps1`): `coop_server` on every address of this PC with `--upnp` and `--invite` (unless `coop-options.ini` says `upnp = no` / `invite = no`), the invite code shown in that window and copied to the clipboard, and a log in the kit's `logs` folder. The port is the kit's (`port` in its `coop.ini`, else 27510). The join password is `server_password` from the kit's `coop-options.ini`, else a new one, handed to `start-server.ps1` in `OOT_COOP_SESSION_PASSWORD` as `Host.bat` does (never on a command line; the handler removes it from Nucleus's environment right after). `server_bind` goes along as `-Bind`, and then the players on this PC use that address; `server_allow` goes along as `-Allow` with `127.0.0.1` added. The players on this PC join at `127.0.0.1` (or the bound address); friends on other PCs paste the invite code under **Join** in their own game. The first time, Windows may ask to let `coop_server.exe` through its firewall.

If a `coop_server.exe` already runs on this PC when player 1 starts (`Host.bat`, `start-server.cmd`, a session not ended), the handler starts no server and says so (a message box, else Nucleus's log): that server has another password or holds the port, and it is left alone (friends may be on it). With no kit above the game (no `server\coop_server.exe`), or a kit without `start-server.cmd`, it starts no server and says so in Nucleus's log; the games then look for one at `127.0.0.1`, port 27510. The server window runs `start-server.cmd` through `cmd /c`, so it closes with the server.

When the session ends, Nucleus closes every `coop_server.exe` on this PC (`Game.KillProcessesOnClose`).

### This PC: Host on this PC, or Join a game on this network

The handler's one option on Nucleus's setup screen (`Context.Options["ThisPC"]`):

- **Host on this PC** (the default): all of the above. Any other value counts as this one: no choice yet, an older Nucleus, or the `Online` key of an older handler.
- **Join a game on this network**, when another PC on your network hosts: no server is started, and `session.txt` is removed. Each game's `coop.ini` has `join_lan = first` and its `name`, with no `server`, `port` or `password`. With `join_lan_host = <name>` in the kit's `coop-options.ini` (or the game's, as above), it is `join_lan = <name>`. At its start, the game listens for games hosted on this network and joins the first one it can join (or the one whose host has that name), with no password: a host lets its own network in. After 60 seconds with none, it says so, and the player can pick one under **Browse games**. Saves, controllers, keys, logs and the 15 seconds between starts stay as above. Ending the session still closes every `coop_server.exe` on this PC.

Nucleus keeps the choice in its game profile with the rest of the setup (`game profiles\<game>\Profile[n].json`, key `Options`), so a saved profile starts the same way next time.

Each game reads its own controller and keeps reading it while another window is in front (patch 0015, `LICHII_COOP_PAD`). So the handler uses none of Nucleus's input hooks (no x360ce or XInput DLLs in the game folder). It does say it uses XInput (`Game.Hook.XInputEnabled = true`): that only makes Nucleus list the controllers on its setup screen and give each player its pad number; the DLLs come only with `Game.Hook.CustomDllEnabled`, `Game.XInputPlusDll`, `Game.UseX360ce` and `Game.ProtoInput`, all off. The module reads `coop-options.ini` beside `longshot.exe` because the instance folder has no kit of its own (`CoopTitle::ApplyKitOptions`). A kit counts only when it is the game's own: the game is its `hookshot\game`, or `OOT_COOP_KIT` or `coop-kit.txt` names it. So Nucleus unzipped inside a kit folder works too. A co-op game built before that change gives every copy the same controller: build it again (`Build-Hookshot-Coop.bat`).

What stays linked: the game's big files (`longshot.exe`, `romfs`, `mods` and the shader and pipeline caches). The game saves a cache by writing a `.tmp` file and moving it over the old one. That puts a file of the instance's own in its folder and leaves the original as it was, so two games never write the same file.

## Troubleshooting

- **A controller player's right stick barely turns the camera, and the camera does not follow Link** (the handler of 0.11.7 or older, with **Mouse camera: Native** in Longshot's options): after Nucleus's Ctrl+H, every game is behind another window. A game behind skips the input read that tells the native mouse camera a pad is turning it, so the game that last had the focus keeps putting its last mouse angles back. Take the newer `OoTCoop.js`, or set **Mouse camera** to **As the stick** in each controller player's game (Insert menu). A co-op game built with the newer patch 0015 leaves the camera to the pad in a game with `LICHII_COOP_KEYBOARD=off`, in front or not.

- **No controllers on Nucleus's setup screen** (the handler of 0.11.1 or older): that handler had XInput off, and Nucleus then lists no pads at all. In your copy (`C:\Nucleus\handlers\OoTCoop.js`) change `Game.Hook.XInputEnabled = false;` to `Game.Hook.XInputEnabled = true;` (or take the newer `OoTCoop.js` from the next release), then restart Nucleus. Nucleus lists Xbox-style (XInput) controllers; a PlayStation or Switch controller needs a tool such as DS4Windows that makes it look like an Xbox controller.

## Test

Windows only, nothing to install:

```
cscript //nologo //E:jscript tools\nucleus\test_handler.js
```

It runs `Game.Play` for three players (two controllers and one keyboard) against a stand-in of Nucleus's `Context` on a made-up kit in `%TEMP%`, then checks every file above, including real junctions, the server start (the server window's command line, the kit's port, password, bind and allow lines, `session.txt`, no kit), and both values of **This PC** (no server and `join_lan` in join mode, `join_lan_host`, an old `Online` value hosting). It prints `ok: N checks` and exits with 0 when all pass. It cannot check Nucleus itself: how it places the windows, its pad numbers, its option screen, and the admin prompt.
