# Questions testers ask

Short answers. Setting up is in [setup.md](setup.md), playing over the internet in [play-online.md](play-online.md), what works in [known-issues.md](known-issues.md).

## Who is the host?

The **first player to join** the server is the host. The host's game is the one the others follow: time of day, and the shared progress when the host loads a save ([Saves](setup.md#saves)).

When the host leaves, the host role **moves to another player** who is still in (the one with the lowest player number). Nobody needs to do anything. A player who joins later never takes the role by joining.

## Host.bat or start-server.cmd?

- **`Host.bat`** starts the co-op server in its own window and then your game. Use it when you play and run the server on the same PC. It shows an invite code (and copies it); the others paste it into `Join.bat` ([play-online.md](play-online.md)). On the same network they can type your address instead.
- **`start-server.cmd`** starts only the server, with no game. Everyone, you included, then plays with `Join.bat` (the server window shows the invite code too).

The PC that runs the server and the host are two separate things. The server only passes messages on; the host is still whoever joins first.

## How do I keep a permanent server?

Run `start-server.cmd` on a PC that stays on (it can be a PC nobody plays on) and leave its window open. Everyone uses `Join.bat` with that PC's address. Opening it to the internet works as for `Host.bat`: [play-online.md](play-online.md), and list who may join with `server_allow` in that kit's `coop-options.ini` ([Who can join](play-online.md#who-can-join)).

The server keeps the shared progress only while someone is in. When the last player leaves it forgets it, and the next session starts from the host's save. Progress lives in the players' saves, so save often, especially the host.

## The game stutters. What can I do?

That is the emulator building shaders: the first time it sees an effect or an area, it has to prepare it for your graphics card. It is worst in the first minutes and in new areas, and gets better as you play, because the emulator keeps what it built in its shader cache (`emulator\user\cache\shader\` in the kit) and reuses it next time.

- In the emulator's graphics settings (Emulation > Configure > Graphics, and its Advanced tab), leave the disk pipeline cache on and try asynchronous shader building: shaders are built in the background, so the game stutters less but an effect can be missing for a moment. Names may differ a little between emulator versions.
- An update that brings a new emulator starts a new cache (the old one is moved to the backup folder, because an old cache can crash a new emulator), so the stutter comes back once after such an update.
- Do not copy shader caches from other people or other emulators: we never share them, and one from another build can crash the game.

## Where are my saves?

In the kit: `emulator\user\nand\user\save\`. They are your own and stay on your PC. `Update.bat` never touches `emulator\user\`, so updates keep them. Back up that folder if you want a copy.

We never ship save files, keys, firmware or the game in a release or in this repository: each player uses their own.

## The update did not happen

`Host.bat`, `Join.bat` and `Update.bat` say why and how to update by hand. Most often: the emulator is still open (close it), there is no network, or GitHub turned the check away for a while (it allows 60 checks an hour from one internet address). If it says the release signature does not check, the release was published moments ago and is not signed yet: try again later (only set `verify_signature = no` in `coop-options.ini` if it keeps failing and you downloaded nothing from elsewhere). By hand: download the newest `oot-coop-....zip` from the [releases](https://github.com/daurosk/oot-remake-multiplayer/releases), unzip it to a new short folder, copy `emulator\user` and `coop-options.ini` from your old kit into it, and play from there.

## My coop-options.ini is missing a line I see in the release

`Update.bat` and every start of `Host.bat` / `Join.bat` add each key a newer kit has and your file lacks, at the end of the file under "Added by Update.bat", with its comment. Your own lines stay as they are. If a line is still missing (kits from before this was added could miss keys such as `server_allow`), copy the line from the release's `coop-options.ini` into yours, or start the kit once after updating.

## Is it safe to share the invite code?

Share it only with the people you play with. It holds your internet address, the port and the join password, so anyone who has it can join while your server window is open. It stops working when you close that window, and `Host.bat` makes a new password each time it starts (unless you set `server_password`). More in [SECURITY.md](../SECURITY.md).

## Emulator or Hookshot?

The emulator kit is the usual way and has every co-op feature. [Hookshot](setup.md#play-on-hookshot-experimental) is experimental: no emulator, but it needs a graphics card with mesh shaders (NVIDIA RTX 20 or newer, AMD RX 6000 or newer), 16 GB of RAM and a long first build. It has the same co-op features, with some keys of its own (T for chat, F6 for the player list). Both can join one session and see each other as Link. <!-- TBC big run: Hookshot rows + mixed run -->

## Is my game traffic encrypted?

Yes, when the session has a join password, which `Host.bat` makes by itself each time. The co-op panel's first line ends in `, encrypted`. Only a host who set `server_password =` (empty) plays without one; then the traffic is plain.

## The other players are all green

Your puppet pack was made before the tunic colours. Run `make-puppet-pack.cmd` once more (`Update.bat` does not rebuild it); `Host.bat` and `Join.bat` say so at start when the pack has no colours.
