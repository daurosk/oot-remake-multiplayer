# oot-remake-multiplayer

Online co-op for the Ocarina of Time remake on PC.

The goal of this project is working multiplayer in two stages:

1. **Emulator first.** Add co-op to the Lichii emulator (Frizti's build), where the game already runs well. Two to four players see each other in the world, share progress, and fight the same enemies.
2. **Then a recomp.** Carry the same co-op design into a native PC recompilation of the game, once that port is playable.

Both stages share one design, one wire protocol and one server, so what is learned on the emulator carries straight over to the recomp.

**Discord:** https://discord.gg/GwtRKAqng5, for players, testers and contributors.

## Status

Stage 0 is done and stage 1 (co-op in the emulator) is well under way: each part is checked on one PC with the real game in two emulator instances, and the two-PC test comes once stage 1 is complete. The plan is in [docs/plan.md](docs/plan.md), the wire protocol in [docs/protocol.md](docs/protocol.md), and what a test kit can show in [docs/testing.md](docs/testing.md).

| Stage | State |
|---|---|
| 0. Transport, server and two clients on one PC | Done: ENet server and client, wire protocol, `tools/coop_test` (10 minute soak runs pass on one PC). Two-PC run not yet done. |
| 1. Presence: other players shown as markers | Done on one PC: player sampling and smoothing ([docs/presence.md](docs/presence.md)), the emulator hook with `coop.ini` settings, a status panel and markers drawn from the game's camera ([docs/lichii.md](docs/lichii.md)). |
| 2. Puppet Link in the world | In progress: the puppet pack is built from the player's own game files ([docs/puppet.md](docs/puppet.md)); each game shows the other player's Link where they stand, it follows them and goes when they leave (checked in game on one PC). It stands still in a T-pose: playing Link's animation is on a branch. What each player wears is read and sent, not yet put on the puppet. |
| 3. Scenes, following and cutscenes | In progress: the rule (puppets only in your scene, held or hidden in cutscenes) is in and tested; the puppet uses it on a branch. The join-partner warp was dropped. |
| 4. Shared progress (chests, keys, doors, songs) | Done on one PC: world flags shared through the host ([docs/flags.md](docs/flags.md)); items, songs and one-time pickups shared while rupees and the rest stay per player ([docs/items.md](docs/items.md)); small keys, Skulltula tokens and heart pieces add up across players. Checked live between two games. |
| 5. Shared enemies and objects | In progress: the game's actor list, life and damage read live ([docs/objects.md](docs/objects.md)); the protocol and server (owner and damage routing) are in; the emulator side is on a branch. |
| 6. Chat, player list, reconnects | Done on one PC: reconnects and rejoining after a crash; the host moves to another player when one leaves; an in-game chat box (Enter) and player list (Tab). |
| Recomp port of stages 0 to 6 | Waits for a playable recomp |

The emulator side is a small patch on Frizti's Fritchii, pinned in [lichii/PIN](lichii/PIN); `lichii/fetch.ps1` fetches that source and applies the patches. See [lichii/README.md](lichii/README.md).

## Building

Needs CMake 3.24 or newer and a C++20 compiler (Visual Studio 2022 on Windows). ENet and doctest are fetched by CMake at pinned versions. `cmake -S . -B build`, then `cmake --build build --config Release` and `ctest --test-dir build -C Release`.

To test co-op on two PCs with the test kit, see [docs/testing.md](docs/testing.md).

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
