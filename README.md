# oot-remake-multiplayer

Online co-op for the Ocarina of Time remake on PC.

The goal of this project is working multiplayer in two stages:

1. **Emulator first.** Add co-op to the Lichii emulator (Frizti's build), where the game already runs well. Two to four players see each other in the world, share progress, and fight the same enemies.
2. **Then a recomp.** Carry the same co-op design into a native PC recompilation of the game, once that port is playable.

Both stages share one design, one wire protocol and one server, so what is learned on the emulator carries straight over to the recomp.

**Discord:** https://discord.gg/GwtRKAqng5, for players, testers and contributors.

## Play the test release

The newest test build is **[v0.1.0-test7](https://github.com/daurosk/oot-remake-multiplayer/releases/tag/v0.1.0-test7)** (all builds: [releases](https://github.com/daurosk/oot-remake-multiplayer/releases)): the patched emulator (Fritchii v5.8.4, with a Saves window to export, import and copy saves), the server and start scripts in one zip. You bring your own game dump and keys. It is an early build for testers, checked in two-game tests on one PC and by players online.

1. [Setting up](docs/setup.md): unzip, your keys and firmware, the puppet pack, first start.
2. Play on the [same network (LAN)](docs/play-lan.md), or [over the internet](docs/play-online.md): the quickest safe way is a free Radmin VPN or Hamachi network (3 steps, no router changes). The host's server window shows the address to give and copies it; `check-hosting.cmd` says what to fix if friends cannot connect.
3. [Known issues](docs/known-issues.md): what works in this build and what does not yet.

What works in it: the other players as Link (child and adult) with Navi and a name tag, following them live, with their sword, shield, held items and gear (#91, #92); shared items, songs, keys, heart pieces and heart containers; a shared Slingshot or ammo bag comes with its seeds and room (#128); slingshot seeds others shoot are shown (#85); dungeon progress and room switches (#129); boss rooms entered together, and a player who walks out during the fight is pulled back in (#109); shared enemies, townspeople and Stalchildren (#124), all Deku Babas (#132); chat, a player list, reconnects (also over VPNs such as Hamachi, #143) and rejoining after a crash. Texture and model mods work; a game with a code or cheat mod stops co-op (#136). Built in, not yet checked in a game: chests opened live in every game, push blocks, locked doors that stay open for everyone, room gimmicks never marked done without their result, Business Scrub talks, Navi, Gohma, the dungeon map, order puzzles played alone.

**Saves: the host's save decides.** Progress is shared while you play; when the host loads a save, every game goes back to that save's progress, and a guest who loads a save gets the host's. To undo something, the host loads an older save; if the host restarts and loads, the guests' progress since the host's last save is undone. See [Saves](docs/setup.md#saves).

Known issues of test7: the ocarina is not shown in the other players' hands and its songs are not heard; fishing with two players is not shared; order puzzles split between two players do not count; bosses target one player at a time. See [Known issues](docs/known-issues.md) and the release notes.

**Safety.** Only the host opens a port: UDP 27510 for the co-op server; players who join open nothing. The server and the games check every message against fixed limits and never write where a message says, and the updater downloads only this project's releases over HTTPS and checks their SHA-256. Prefer a virtual LAN to port forwarding, open the firewall only for your LAN or VPN addresses, and list the players who may join with `server_allow` in the host's `coop-options.ini` ([Who can join](docs/play-online.md#who-can-join)). Details and how to report a problem: [SECURITY.md](SECURITY.md).

Every player needs the same release: a game of one release refuses a server of another. A kit from test3 on updates itself with `Update.bat` (a test2 kit: download `Update.bat` from the newest release first; see [Updating](docs/setup.md#updating)).

## Status

Stage 0 is done and stage 1 (co-op in the emulator) is well under way: each part is checked on one PC with the real game in two emulator instances, and the test releases ([above](#play-the-test-release)) are played by testers on separate PCs. The plan is in [docs/plan.md](docs/plan.md), the wire protocol in [docs/protocol.md](docs/protocol.md), and what a test kit can show in [docs/testing.md](docs/testing.md).

| Stage | State |
|---|---|
| 0. Transport, server and two clients on one PC | Done: ENet server and client, wire protocol, `tools/coop_test` (10 minute soak runs pass on one PC). Two-PC run not yet done. |
| 1. Presence: other players shown as markers | Done on one PC: player sampling and smoothing ([docs/presence.md](docs/presence.md)), the emulator hook with `coop.ini` settings, a status panel and markers drawn from the game's camera ([docs/lichii.md](docs/lichii.md)). |
| 2. Puppet Link in the world | Done on one PC: the puppet pack is built from the player's own game files ([docs/puppet.md](docs/puppet.md)); each game shows the other players as child Link, following them live and copying their whole pose every frame, with Navi beside them and a name tag overhead; a player who joins later gets a Link for everyone already there. Their sword, shield, held items (slingshot, bow, hookshot) and gear (masks, boots, tunics) show (test7, #91, #92). Adult Link shows and walks (test6, #111). Their sounds are built in (test6, #131), not yet heard in a real game. The ocarina in hand not yet. |
| 3. Scenes, following and cutscenes | Built: puppets only in your scene, hidden or held in cutscenes (unit-tested, not yet checked in game). The join-partner warp was dropped. |
| 4. Shared progress (chests, keys, doors, songs) | Done on one PC: world flags shared through the host ([docs/flags.md](docs/flags.md)); items, songs and one-time pickups shared while rupees and the rest stay per player ([docs/items.md](docs/items.md)); small keys, Skulltula tokens and heart pieces add up; a late joiner gets them; dungeon switches, doors, cleared rooms and water levels shared ([docs/dungeon.md](docs/dungeon.md)), and a save load no longer undoes them (#129); chests opened in one game are open in all (#87); a shared item comes with its ammo and room (#128, #138). Heart containers raise everyone's maximum health (#88). The host's save decides: a host's save load resets the group to it (test7, built, not yet checked in a game). |
| 5. Shared enemies and objects | In progress: enemies, townspeople and animals move, animate and die the same in every game, owned by one player ([docs/objects.md](docs/objects.md)); seeds and nuts a player makes appear in the other games ([docs/spawns.md](docs/spawns.md)); crates and thrown rocks break in every game ([docs/world.md](docs/world.md)). Built, not yet fully checked in a game: pots, the time of day following the host ([docs/time.md](docs/time.md)), bombs, arrows and other projectiles, multi-part enemies and bosses. Signs cut in one game are cut in the others (seen in the logs; the pieces on the other screen not yet). Stalchildren in Hyrule Field are shared (test7, #124); boss rooms pull a player who walks out back in (#109). |
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

Anyone can contribute: fork, branch, pull request. Every pull request is reviewed before it is merged. Read [CONTRIBUTING.md](CONTRIBUTING.md) first. Questions go to [Discussions](https://github.com/daurosk/oot-remake-multiplayer/discussions) or [Discord](https://discord.gg/GwtRKAqng5).

## Rules for this repository

- **No game content.** No game files, keys, firmware, saves, shader caches or builds that contain them. `tools/check_content.py` runs on every push and pull request and fails if any are found.
- **Our own code only.** Other projects (Lichii, Eden, the TOTK Online mod, recomp projects) may be read to understand how they behave. Their code is not copied in unless its licence allows it, and then it is credited.
- **Plain English** in commits, pull requests and docs.

## Licence

GPL-3.0, see [LICENSE](LICENSE). This keeps the project compatible with Lichii and Eden, which are GPL-3.0.

This is a fan project. It is not affiliated with or endorsed by Nintendo. The Legend of Zelda and Ocarina of Time are trademarks of Nintendo.
