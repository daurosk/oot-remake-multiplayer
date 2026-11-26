# Questions testers ask

Short answers about Squadala!, the co-op mod. Setting up is in [setup.md](setup.md), playing over the internet in [play-online.md](play-online.md), what works in [known-issues.md](known-issues.md).

## Who is the host?

The **first player to join** the server is the host. The host's game is the one the others follow: time of day, and the shared progress when the host loads a save ([Playing from the host's save](play-online.md#playing-from-the-hosts-save)).

When the host leaves, the host role **moves to another player** who is still in (the one who has been in the session longest). Nobody needs to do anything. A player who joins later never takes the role by joining. While others stay, the new host keeps playing the group's save (the old host's) for the rest of the session, so everyone carries on with the same progress. A player left alone goes back to their own saves: on the title right away, or on their next return to the title when a save is being played (never under a game being played). When the server restarts, the first game back hosts (it keeps its player number); alone, it plays from its own saves, and a game that comes back after it is a guest and gets that host's save. After **Leave**, a game on the host's save goes back to its own saves on the title too.

## Host in the game or start-server.cmd?

- **Host** in the game's Multiplayer tab starts the co-op server on your PC and joins your own game to it. Use it when you play and run the server on the same PC. It copies an invite code; the others paste it under **Join** ([play-online.md](play-online.md)). On the same network or a VPN, **Same network or VPN** on the Host screen gives one line to paste instead ([play-lan.md](play-lan.md)).
- **`start-server.cmd`** in the kit folder starts only the server, with no game. Everyone, you included, then joins it from the game.

The PC that runs the server and the host are two separate things. The server only passes messages on; the host is still whoever joins first.

## How do I keep a permanent server?

Run `start-server.cmd` on a PC that stays on (it can be a PC nobody plays on) and leave its window open. Everyone joins it from the game with that PC's address and the join password (set a fixed one with `server_password` in that kit's `coop-options.ini`). Opening it to the internet: [play-online.md](play-online.md); list who may join with `server_allow` ([Who can join](play-online.md#who-can-join)).

The server keeps the shared progress only while someone is in. When the last player leaves it forgets it, and the next session starts from the host's save. Progress lives in the players' saves, so save often, especially the host.

## The game stutters. What can I do?

That is Longshot building its shaders: the first time it sees an effect or an area, it has to prepare it for your graphics card. It is worst in the first minutes and in new areas, and is smooth the next time. Close other programs that use the graphics card and keep the driver up to date. Do not copy shader caches from other people: we never share them, and one from another build can crash the game.

## Where are my saves?

In the kit: `hookshot\game\user\save\`. They are your own and stay on your PC; updates keep them. Back up that folder if you want a copy. A guest who joined from the title plays from a copy of the host's save in a co-op save folder; their own saves are untouched ([setup.md](setup.md)).

We never ship save files, keys, firmware or the game in a release or in this repository: each player uses their own.

## I started longshot.exe and cannot see my friend

Always start with **Start Squadala.bat** (`bash start-squadala.sh` on Linux), never `longshot.exe`. You do not need your own Longshot build: the kit builds its own game, in its `hookshotgame` folder, with co-op inside. A Longshot you built or downloaded yourself has no co-op at all, so it cannot see anyone. The kit's own `longshot.exe` started by hand has co-op, but not what the start file adds (the puppet pack that gives the others their Link and colours, your settings, the update check), so the game's chat says "This game was not started with Start Squadala.bat" and the others may show as names only. Close the game and start the kit with Start Squadala.bat (the **Play OoT Co-op** shortcut on your desktop does the same). Your saves are not touched either way.

## Start Squadala.bat or Start OoT Co-op.bat?

`Start Squadala.bat` (`bash start-squadala.sh` on Linux). It is the only start file from 0.11.4 on; `Start OoT Co-op.bat` (`start-oot-coop.sh`) was its old name. When your kit updates to 0.11.4, your desktop and Start menu shortcuts, the Linux menu entry and your Steam entry are moved over to the new name and the old file goes. If Steam was open at the time, restart it once.

## The update did not happen

`Start Squadala.bat` and `Update.bat` say why and how to update by hand. Most often: the game is still open (close it), there is no network, or GitHub turned the check away for a while (it allows 60 checks an hour from one internet address). If it says the release signature does not check, the release was published moments ago and is not signed yet: try again later (only set `verify_signature = no` in `coop-options.ini` if it keeps failing and you downloaded nothing from elsewhere). By hand: download **Update.bat** from the newest [release](https://github.com/daurosk/oot-remake-multiplayer/releases) into your kit folder and double-click it.

## Can I go back to the version I had?

Yes: `Revert.bat` in the kit folder (`bash revert.sh` on Linux) puts back the version your kit had before its last update; your saves and settings stay, and `Update.bat` goes forward again. Everyone you play with needs the same version, so tell your friends to go back too. Starting the kit does not update straight back. Any older release: `Update.bat -Version v0.10.1`. More in [setup.md](setup.md#going-back-to-an-older-version).

## My coop-options.ini is missing a line I see in the release

`Update.bat` and every start of the kit add each key a newer kit has and your file lacks, at the end of the file under "Added by Update.bat", with its comment. Your own lines stay as they are. If a line is still missing, copy the line from the release's `coop-options.ini` into yours.

## Is it safe to share the invite code?

Share it only with the people you play with. It holds your internet address, the port and the join password, so anyone who has it can join while your server window is open. It stops working when you stop hosting, and each Host makes a new password (unless you set `server_password`). More in [SECURITY.md](../SECURITY.md).

## Emulator or Longshot?

Releases from 0.10.1 on are Longshot only: no emulator, but it needs a graphics card with mesh shaders (NVIDIA RTX 20 or newer, AMD RX 6000 or newer), 16 GB of RAM and a long first build ([setup.md](setup.md)). Emulator players keep the last emulator release, the early test release of 2026-11-12 on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases). The two cannot play together: every player needs the same release.

## Is my game traffic encrypted?

Yes, always. Every session has a join password, which Host makes by itself each time (or the host sets with `server_password`), and the traffic is encrypted with it. The co-op panel's first line ends in `, encrypted`. There is no plain mode: the server does not start without a password, and the game does not join a server that does not encrypt.

## The other players are all green, or my colours do not show

The colours are drawn with the puppet pack, which the kit makes from your own game by itself (on the first start, and again after an update that changes it). If the start said it could not make the pack, start the kit again; the log `longshot_log.txt` (in `hookshot\game\bin\`) says which pack the game loaded. Pick your own colours in the Multiplayer tab, **Customize Link**.
