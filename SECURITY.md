# Security

## Supported versions

Only the newest release on the [releases page](https://github.com/daurosk/oot-remake-multiplayer/releases) is supported. Older kits get no fixes: a fix ships in the next release, and `Update.bat` (or the update check when `Host.bat` or `Join.bat` starts) moves a kit to it. All players in one session need the same release; the server turns away a game with a different protocol version.

## Reporting a problem

If you find a way the co-op could harm a player's PC, their game or their save through the network or a downloaded file, please tell us privately first:

- On GitHub, open the repository's **Security** tab and choose **Report a vulnerability**.
- If that button is not there, open an ordinary issue that says only "security problem, please contact me" and how to reach you. Do not put the details in a public issue.

Say which release you used (its `VERSION.txt`), what you did, and what happened. We answer in the issue or report, fix it on a branch with a test that feeds the bad input, and say in the release notes which release has the fix.

Out of scope: what a player you let in does inside the shared game (see [What another player can do](#what-another-player-can-do)), problems in the emulator or the game that the co-op does not touch (report those to their own projects), kits from anywhere but this repository's releases, and PCs where someone else already runs programs as you.

## What the co-op opens

- **The host** runs `coop_server.exe`. It listens on **one UDP port, 27510** (or the port in `emulator\user\config\coop.ini`) while its window is open, and closes it when it exits.
- **Join password.** Unless `server_password` is set in `coop-options.ini`, `Host.bat` makes a new random 20-letter password (100 bits) each time it starts. The password never crosses the network: a joining game answers a challenge with a proof made from it. An address that gives 5 wrong proofs in a short time is turned away for a while. The password is not written to `logs\server-*.log` or to what `collect-logs` gathers.
- **UPnP (on by default, `upnp = no` turns it off).** Only with a join password, `Host.bat` asks the router to forward UDP port 27510 to this PC, with a 10-minute lease it renews, and removes the forward when the server stops or its window closes. Nothing else on the router is changed, and nothing changes the Windows firewall. A firewall rule or port forward you add by hand stays until you delete it.
- **Invite code (on by default, `invite = no` turns it off).** `Host.bat` asks a public STUN server (Google's, then Cloudflare's, or the ones in `stun_servers`) for this PC's internet address, from the game's port. That server sees your internet address and nothing of the game. The invite code holds your internet address, the port and the join password, so share it only with the people you play with; it stops working when the server window closes. When a direct connection fails, the friend's `Join.bat` shows an answer code for the host to paste; the server then sends a few "punch" packets toward the address in that code only. Punches are signed (HMAC-SHA256 with a key made from the password) and expire after 60 seconds, the server never answers an unsigned packet, and its replies are never bigger than the request.
- **Relay (off unless you set `relay =`).** The host's server connects out to a relay someone runs with `start-relay.cmd` (UDP port 27530 on the relay's PC) and gets a join code. The relay owner sees every player's address and when and how much is sent; the game traffic itself is encrypted (the password is required), so the relay cannot read or change it. A join password is required: the code only finds the server. Details: [play-online.md](docs/play-online.md#advanced-relay-with-join-codes).
- **Players who join** open no port. Their game connects out to the host's server (or the relay) and only talks to it.
- **Game traffic is encrypted with a join password.** The game and the server agree on keys (X25519) bound to the join password and the exact join messages, then seal every packet after the password check (XChaCha20-Poly1305, Monocypher; replays are dropped). Someone on the path (a shared Wi-Fi, a VPN, a relay) can neither read nor change it, and without the password nobody can make the keys. A game that has a password never plays unencrypted, and a server with one refuses a game that offers no key. Without a password (`server_password =` empty) the traffic is plain and the co-op panel says `not encrypted`. Everyone with the password, and the host's server, can read the session. Details: [docs/protocol.md, "Encrypted traffic"](docs/protocol.md#encrypted-traffic-protocol-10-no-new-types).
- **Updates.** `Update.bat` (and the update check when `Host.bat`, `Join.bat` or `play.cmd` start; with `auto_update = no` it only says a release is out) asks GitHub for this repository's releases over HTTPS, downloads only from `https://github.com/daurosk/oot-remake-multiplayer/releases/download/`, and checks the zip's SHA-256 against the `.sha256` file of the same release before it changes anything. When the GitHub CLI (`gh`) is installed and signed in, it also checks the release signature with the installed kit's own `scripts\verify_kit.ps1` (never the one in the download) and refuses a zip that fails; without `gh` it says so and relies on the SHA-256. `verify_signature = no` in `coop-options.ini` turns the signature check off (the SHA-256 check stays); that is not recommended, because then a zip that fails the signature is installed anyway. Use it only if the check keeps failing on a release you know is ours. Nothing in the download is run before these checks.

## How releases are signed

Each release's kit zip and `Update.bat` are signed by this repository's `Release attest` workflow (GitHub artifact attestations, Sigstore) from the files as published. To check a download yourself, with the GitHub CLI:

```
gh attestation verify oot-coop-<version>.zip --repo daurosk/oot-remake-multiplayer --signer-workflow daurosk/oot-remake-multiplayer/.github/workflows/release-attest.yml
```

or run `tools\release\verify_kit.ps1 -Zip <file>` from this repository, which also checks the `.sha256` file. It prints `OK: signed by our release workflow`. The signature proves the file is what our workflow saw at publish time; it does not protect against someone with write access to this repository. Every GitHub Action our workflows use is pinned to a full commit hash (checked by `tools/check_actions.py`). More in [setup.md](docs/setup.md#check-the-download).

## What the server and the games accept

- Every message is checked before it is used: lengths, counts and indexes against fixed limits, strings for valid UTF-8 without control characters, numbers for range. A message that fails is dropped. The readers are fuzz-tested (`tests/protocol/fuzz_test.cpp`).
- A message never names a memory address, offset or size. The games write only to places they find themselves in their own memory, for things on fixed allow-lists (shared items and flags, objects that may be shared, the words of a sign that may be cut).
- The server writes each message's sender itself and never trusts the one in the message. Each player may send at most 60 reliable and 240 fast messages a second and 1 MB a second (bursts of twice that; fast messages leave the last 256 KB for the reliable ones); the rest is dropped, so one player cannot flood the others through the server. One address may hold at most 4 connections that have not said hello, and those are closed after 10 seconds. At most 4 players.
- Names that are already taken, blank, or that hold invisible or right-to-left characters are refused, so nobody can pass for another player.
- Nothing from the network is used as a file name, a command, or a format string. Chat and names are shown as text only.

## What another player can do

The people you play with share your game's progress: items, songs, flags, enemies, the host's save. A player who means harm can spoil that game (take or give items, open doors, kill enemies) as any co-op partner could. They cannot reach anything outside the game through the co-op. Play with people you trust, and keep a copy of saves you care about (the Saves window can export them).

## Safe hosting

- Keep a join password: `Host.bat` makes one unless you set your own, and UPnP and the relay refuse to run without one. If you set `server_password` yourself, make it long and not one you use anywhere else.
- A password-protected Radmin VPN or Hamachi network (or ZeroTier, Tailscale) is the safest way when the invite code does not work: only the people in your private network can reach the server. Only join VPN networks of people you know.
- Make the firewall rule as narrow as it can be: for the program (`program=...\server\coop_server.exe`, not only the port, so nothing else that later listens on 27510 is let in), on Private networks where possible (`profile=private`), and only from the addresses that need it (`remoteip=localsubnet` on a LAN, `25.0.0.0/8` for Hamachi, `26.0.0.0/8` for Radmin VPN). The commands are in [play-lan.md](docs/play-lan.md#firewall) and [play-online.md](docs/play-online.md). Never allow the port on Public networks for every address.
- When you are done, delete the firewall rule and any router port forward you made by hand: our programs never touch firewall rules, and the router forward Host.bat asks for (UPnP, only with a join password) ends when the server window closes ([When you are done](docs/play-online.md#when-you-are-done)).
- In the host's `coop-options.ini`, `server_allow` lists the addresses that may join and `server_bind` makes the server listen on your VPN address only ([Who can join](docs/play-online.md#who-can-join)).
- Share invite codes, join codes and your public address only with the people you play with.
- Close the server window when you are done: nothing listens once it is closed.
- Download the kit and updates only from this repository's [releases](https://github.com/daurosk/oot-remake-multiplayer/releases).
