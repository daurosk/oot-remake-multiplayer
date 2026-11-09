# oot-remake-multiplayer

Online co-op for the Ocarina of Time remake on PC.

The goal of this project is working multiplayer in two stages:

1. **Emulator first.** Add co-op to the Lichii emulator (Frizti's build), where the game already runs well. Two to four players see each other in the world, share progress, and fight the same enemies.
2. **Then a recomp.** Carry the same co-op design into a native PC recompilation of the game, once that port is playable.

Both stages share one design, one wire protocol and one server, so what is learned on the emulator carries straight over to the recomp.

**Discord:** https://discord.gg/GwtRKAqng5, for players, testers and contributors.

## Status

Stage 0 is done and stage 1 (presence in the emulator) is in progress. Nothing has been tested with the game running yet; the first test release for two PCs is being put together. The plan is in [docs/plan.md](docs/plan.md) and the wire protocol in [docs/protocol.md](docs/protocol.md).

| Stage | State |
|---|---|
| 0. Transport, server and two clients on one PC | Done: ENet server and client, wire protocol, `tools/coop_test` (10 minute soak runs pass on one PC). Two-PC run not yet done. |
| 1. Presence: other players shown as markers | In progress: player sampling, smoothing and extrapolation ([docs/presence.md](docs/presence.md)), the emulator hook with `coop.ini` settings and a status panel, and markers drawn from the game's camera ([docs/lichii.md](docs/lichii.md)). Not yet checked in game. |
| 2. Puppet Link in the world | Not started |
| 3. Scenes, following and cutscenes | Not started |
| 4. Shared progress (chests, keys, doors, songs) | Started: server host choice and flag table, client flag sync and a flag watcher ([docs/host.md](docs/host.md)). The game's flag addresses are not yet found. |
| 5. Shared enemies and objects | Not started |
| 6. Chat, player list, reconnects | Started: chat, player list and join/leave events in the client; the host moves to another player when one leaves. |
| Recomp port of stages 0 to 6 | Waits for a playable recomp |

The emulator side is a small patch on Frizti's Fritchii, pinned in [lichii/PIN](lichii/PIN); `lichii/fetch.ps1` fetches that source and applies the patches. See [lichii/README.md](lichii/README.md).

## Building

Needs CMake 3.24 or newer and a C++20 compiler (Visual Studio 2022 on Windows). ENet and doctest are fetched by CMake at pinned versions. `cmake -S . -B build`, then `cmake --build build --config Release` and `ctest --test-dir build -C Release`.

## What you need

- Your own legally obtained copy of the game, dumped from your own console. This repository has no game files, keys or firmware and never will.
- Every player needs the same game version and the same co-op build.

## Contributing

Contributions are welcome: fork, branch, pull request. Read [CONTRIBUTING.md](CONTRIBUTING.md) first. Questions go to [Discussions](https://github.com/daurosk/oot-remake-multiplayer/discussions) or [Discord](https://discord.gg/GwtRKAqng5).

## Rules for this repository

- **No game content.** No game files, keys, firmware, saves, shader caches or builds that contain them. `tools/check_content.py` runs on every push and pull request and fails if any are found.
- **Our own code only.** Other projects (Lichii, Eden, the TOTK Online mod, recomp projects) may be read to understand how they behave. Their code is not copied in unless its licence allows it, and then it is credited.
- **Plain English** in commits, pull requests and docs.

## Licence

GPL-3.0, see [LICENSE](LICENSE). This keeps the project compatible with Lichii and Eden, which are GPL-3.0.

This is a fan project. It is not affiliated with or endorsed by Nintendo. The Legend of Zelda and Ocarina of Time are trademarks of Nintendo.
