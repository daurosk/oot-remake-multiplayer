# Security

## Reporting a problem

If you find a way the co-op could harm a player's PC, their game or their save through the network or a downloaded file, please tell us privately first:

- On GitHub, open the repository's **Security** tab and choose **Report a vulnerability**.
- If that button is not there, open an ordinary issue that says only "security problem, please contact me" and how to reach you. Do not put the details in a public issue.

Say which release you used (its `VERSION.txt`), what you did, and what happened. We answer in the issue or report, fix it on a branch with a test that feeds the bad input, and say in the release notes which release has the fix.

## What the co-op opens

- **The host** runs `coop_server.exe`. It listens on **one UDP port, 27510** (or the port in `emulator\user\config\coop.ini`). Nothing else is opened. Nothing is opened on your router unless you forward the port yourself.
- **Players who join** open no port. Their game connects out to the host's server and only talks to it.
- **`Update.bat`** (and the update check when `Host.bat`, `Join.bat` or `play.cmd` start) asks GitHub for this repository's releases over HTTPS, downloads only from `https://github.com/daurosk/oot-remake-multiplayer/releases/download/`, and checks the zip's SHA-256 against the `.sha256` file of the same release before it changes anything. Nothing in the download is run before that check.

## What the server and the games accept

- Every message is checked before it is used: lengths, counts and indexes against fixed limits, strings for valid UTF-8 without control characters, numbers for range. A message that fails is dropped. The readers are fuzz-tested (`tests/protocol/fuzz_test.cpp`).
- A message never names a memory address, offset or size. The games write only to places they find themselves in their own memory, for things on fixed allow-lists (shared items and flags, objects that may be shared, the words of a sign that may be cut).
- The server writes each message's sender itself and never trusts the one in the message. Each player may send at most 60 reliable and 240 fast messages a second and 1 MB a second (bursts of twice that); the rest is dropped, so one player cannot flood the others through the server. One address may hold at most 4 connections that have not said hello, and those are closed after 10 seconds. At most 4 players.
- Names that are already taken, blank, or that hold invisible or right-to-left characters are refused, so nobody can pass for another player.
- Nothing from the network is used as a file name, a command, or a format string. Chat and names are shown as text only.

## What another player can do

The people you play with share your game's progress: items, songs, flags, enemies, the host's save. A player who means harm can spoil that game (take or give items, open doors, kill enemies) as any co-op partner could. They cannot reach anything outside the game through the co-op. Play with people you trust, and keep a copy of saves you care about (the Saves window can export them).

## Safe hosting

- Prefer a virtual LAN (Radmin VPN, Hamachi, ZeroTier, Tailscale) over port forwarding: only the people in your private network can reach the server. Only join VPN networks of people you know.
- Open the firewall only for the addresses that need it: `remoteip=localsubnet` on a LAN, `remoteip=25.0.0.0/8` for Hamachi, `26.0.0.0/8` for Radmin VPN ([play-lan.md](docs/play-lan.md#firewall), [play-online.md](docs/play-online.md)). Never allow the port on Public networks for every address.
- In the host's `coop-options.ini`, `server_allow` lists the addresses that may join and `server_bind` makes the server listen on your VPN address only ([Who can join](docs/play-online.md#who-can-join)).
- With port forwarding, share your public address only with the people you play with, and remove the forwarding rule when you stop playing for a while.
- Close the server window when you are done: nothing listens once it is closed.
- Download the kit and updates only from this repository's [releases](https://github.com/daurosk/oot-remake-multiplayer/releases).
