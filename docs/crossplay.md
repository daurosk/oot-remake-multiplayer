# Crossplay: players on different clients in one game

Squadala! runs in more than one client: the Windows kit's Longshot, a Hookshot built with this repository's patches, the
Fritchii emulator with `lichii/patches` (stage 1, [lichii.md](lichii.md); the same `lichii/hook` code), and other
launchers that build this module into their own copy of the game (for example Navi, which ships `hookshot_coop.dll`
built from this repository with a few changes of its own). Players on any of them can host and join each other with the
same invite codes, addresses and join passwords. This page says what makes that work, what keeps it working, and how to
check a client.

## What the server checks

A game that joins is refused only for these (`server/session.cpp` `admit`, `check_hello`):

- **The protocol version** (`kProtocolVersion`, `coop/protocol/include/coop/protocol/messages.hpp`). Every 0.11 release
  speaks 12.
- **The game build** (`game_build_id`): the session's first player decides it, or `coop_server --game-build`. Every
  supported client runs the same build, `e0f3ccfaf50065ae` (1.1.1).
- **The join password**, and the usual limits: a full server, a name in use or a bad one (blank, or with hidden
  characters), a ban, and a traffic key share the server cannot use (refused as a protocol mismatch).

- **The co-op release** its HELLO's `build_id` names (`coop_release` in `messages.hpp`: the first word that is a
  version, `v0.11.7` from `coop hookshot v0.11.7-86-gab12cd34`): the session's first player decides it, and a game of
  another release is refused (`REFUSED` 10), older or newer, so two kits never play together on different releases.
  Builds of one release from different commits are let in, and so is a `build_id` that names no release.

Otherwise what the game runs the module in (its HELLO's `build_id`) is never a reason to refuse it. Each build names itself there
(`lichii/hook/coop_hook.cpp` `kBuildId`): the Hookshot/Longshot module `coop hookshot <version>`, the Fritchii emulator
`coop fritchii-v5 <version>` (before, every build sent the same `lichii fritchii-v5 coop`). It is cut to HELLO's limit of
64 characters (`kMaxBuildIdChars`, `clamp_build_id` in `messages.hpp`), at a character, so a long version text (a long
`git describe`, a launcher's own version) never keeps the HELLO from being sent. The server writes it in its log
beside each player's name, so a problem between two clients can be told apart:

```
player 2 joined: Zelda (client: coop hookshot 0.11.3)
player 3 joined: Saria (client: coop fritchii-v5 0.11.3)
```

`tests/server/session_test.cpp` holds both: players whose HELLOs name four different clients (the kit's module, a
launcher's build of it, a Hookshot build and Fritchii) share one session, and the client's name in the log is printable
and at most 48 characters, whatever another game sends. `tests/protocol/codec_test.cpp` checks that a long version is
cut so the HELLO can always be sent.

A split-screen player who joins with another (`... joined: <name> (split screen with player N)`) runs in that player's
game, so that line names no client of its own: it is the client of player N's line.

## Keeping a client compatible

A launcher or a fork that builds this module into its own client stays in the same games as everyone else as long as:

1. **It speaks the same protocol.** No new message type, and no changed message layout or meaning, without a new protocol
   version in this repository (and `docs/protocol.md` changed with it). A fork's own feature that needs the network goes
   inside an existing message's meaning only if every other client already reads it the same way (for example a different
   difficulty value from the host's server: `DIFFICULTY` carries values every client applies).
2. **Its own features stay in its own game.** Save handling, menus, extra checks before a join and its own logs change
   nothing on the wire.
3. **It runs the same game build.**
4. **It follows this repository's releases.** Clients on different 0.11 releases connect (the same protocol), but each
   plays with its own release's rules, so two of them can see a boss, a door or an item differently. The newest release on
   every client is the one combination that plays as tested.

A plain Longshot, a plain Hookshot or a plain Fritchii, without this repository's patches and module, has no co-op at
all: it cannot join or host. Other emulators: only once players ask for one.

## Checking a client

Build the stock server and test client from this repository (`COOP_BUILD_PROGRAMS=ON`), then, with the client to check:

- **The client joins a stock host:** `coop_server --port 27520 --password <pw>`, and the client joins `127.0.0.1:27520`
  with that password. The server's log says `player 1 joined: <name> (client: ...)`, the client's says it was welcomed.
- **Stock players join the client's host:** host in the client, then
  `coop_test --connect 127.0.0.1:27510 --password <pw> --clients 2 --seconds 15`. It must end with `result: PASS`
  (no client refused or dropped, no reliable message lost).

Recorded (2026-11-16): Navi's client (this repository's integrate-0114 with Navi's changes, on Navi's Hookshot/Longshot
engine) against the stock `coop_server` and `coop_test` built from integrate-0114: the stock server welcomed it as
player 1; hosting at a raised difficulty, it took two stock clients with `result: PASS` (0 refused, 0 dropped, 0 lost),
and they applied its `DIFFICULTY` values.
