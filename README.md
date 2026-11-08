# oot-remake-multiplayer

Online co-op for the Ocarina of Time remake on PC.

The goal of this project is working multiplayer in two stages:

1. **Emulator first.** Add co-op to the Lichii emulator (Frizti's build), where the game already runs well. Two to four players see each other in the world, share progress, and fight the same enemies.
2. **Then a recomp.** Carry the same co-op design into a native PC recompilation of the game, once that port is playable.

Both stages share one design, one wire protocol and one server, so what is learned on the emulator carries straight over to the recomp.

## Status

Planning. No co-op code yet. The plan is in [docs/plan.md](docs/plan.md).

| Stage | State |
|---|---|
| 0. Transport, server and two clients on one PC | Not started |
| 1. Presence: other players shown as markers | Not started |
| 2. Puppet Link in the world | Not started |
| 3. Scenes, following and cutscenes | Not started |
| 4. Shared progress (chests, keys, doors, songs) | Not started |
| 5. Shared enemies and objects | Not started |
| 6. Chat, player list, reconnects | Not started |
| Recomp port of stages 0 to 6 | Waits for a playable recomp |

## What you need

- Your own legally obtained copy of the game, dumped from your own console. This repository has no game files, keys or firmware and never will.
- Every player needs the same game version and the same co-op build.

## Rules for this repository

- **No game content.** No game files, keys, firmware, saves, shader caches or builds that contain them. `tools/check_content.py` runs on every push and pull request and fails if any are found.
- **Our own code only.** Other projects (Lichii, Eden, the TOTK Online mod, recomp projects) may be read to understand how they behave. Their code is not copied in unless its licence allows it, and then it is credited.
- **Plain English** in commits, pull requests and docs.

## Licence

GPL-3.0, see [LICENSE](LICENSE). This keeps the project compatible with Lichii and Eden, which are GPL-3.0.

This is a fan project. It is not affiliated with or endorsed by Nintendo. The Legend of Zelda and Ocarina of Time are trademarks of Nintendo.
