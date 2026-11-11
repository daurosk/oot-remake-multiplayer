# Plan

Two stages: co-op on the Lichii emulator first, then the same co-op in a native recomp. One design, one wire protocol and one server serve both.

## Why the emulator first

- The game already runs well there (Frizti's Lichii build).
- Lichii loads extra code modules, patches and cheats for this game, and already ships built-in patches for it. That gives co-op the hooks it needs.
- Presence and shared progress need no game code: the emulator reads and writes game memory each frame and does the networking on the PC side.

## Why not the TOTK Online mod

The TOTK Online mod (kirbymimi) is a closed module that hooks Tears of the Kingdom 1.2.1 at fixed addresses and creates TotK-only actors. It cannot load into this game. What carries over are the lessons: the ways its sync breaks are written down in the TotK port's notes (owners that flip with no hand-over, samples with no time or velocity, components with no length, hits on copies that reach nobody). This design avoids each of them from the start.

## Design

- **Transport:** ENet (MIT). Reliable channels for events and progress, fast unreliable channels for positions. Runs on the PC side, never inside guest code. LAN or Tailscale first, a relay later.
- **Server:** our own, small, in this repository. The host's game decides shared progress.
- **Wire format:** every sample carries a timestamp and a velocity; every component carries its own length; the same layout in both directions; a protocol version checked on connect.
- **Players:** 2 to 4.
- **Full protocol:** [protocol.md](protocol.md).

## Code layout

C++20 and CMake, the same as Lichii, so the co-op code builds inside it and inside the recomp without changes.

| Folder | What |
|---|---|
| `coop/protocol/` | Message layouts, encoder and decoder. No sockets, no game. Shared by everything below. |
| `coop/net/` | The ENet client and server wrappers. |
| `server/` | The headless server program. |
| `tools/coop_test/` | The stage 0 test tool: starts a server and clients in one process or several. |
| `tests/` | Unit tests for the protocol (round trips, unknown components skipped, bad lengths refused). |
| `lichii/` | The emulator pin (`PIN`), `fetch.ps1` that fetches and patches it, the patches (hook points only, as small as possible) and `hook/`, the co-op code the patch builds into the emulator. See [lichii/README.md](../lichii/README.md). |

Lichii is now Frizti's Fritchii (github.com/Frizti/Fritchii, GPL-3.0). Decided 2026-11-08: its source is not committed here and not forked on GitHub. `lichii/fetch.ps1` clones it at the pinned commit to `fritchii-src` beside this repo (outside every repo) and applies our patches as local commits. A new Fritchii release means a new pin and, if they no longer apply, refreshed patches.

ENet comes in as a pinned git submodule or CMake `FetchContent` at a fixed version, never copied into the tree.

## Game facts we start from

Read from the game's main module (build e0f3ccfaf50065ae, version 1.1.1). Offsets are into `main`; the emulator has to add the base address where it maps `main`.

| What | Where |
|---|---|
| Actor position | actor +0x31c, 3 floats |
| Actor rotation | actor +0x328, 3x3 matrix |
| Player position, GameUI copy | GameUI module (pointer at main+0x4e7cfa8) +0x590 |
| Is child | GameUI module +0x5c0 |
| Current scene | Banc module +0x40, a string |
| Event running | event manager +0x68 not zero |
| Flag hash | Murmur3 x86 32-bit, seed 0 |
| Scene change | sequence graphs to 0x9b6ad0 and the load slots in 0x9b7d10 |

Most of these were read from the code and not yet checked in a run.

## Stage 1: emulator (Lichii)

0. **Transport and two clients on one PC.** Needs no game and no Lichii, so it can start now. In order:
   1. CMake project, ENet pinned, a build on Windows in CI next to the content guard.
   2. `coop/protocol`: the stage 0 and 1 messages from [protocol.md](protocol.md), with unit tests.
   3. `server/`: accepts `HELLO`, refuses wrong versions and a fifth player, relays, sends `PLAYER_JOINED` and `PLAYER_LEFT`.
   4. `coop/net` client: connect, handshake, ping and clock offset, reconnect after a drop.
   5. `tools/coop_test`: two to four clients send `TEST_PAYLOAD` at 60 a second on the unreliable channel and 1 a second on the reliable one, and fake `PLAYER_SAMPLE`s along a known path.
   6. A report: round trip, clock offset, reliable messages lost (must be 0), unreliable messages lost, and the shown error of the fake path after interpolation.

   Done when two clients run 10 minutes on one PC with no reliable message lost, then the same over LAN or Tailscale between two PCs, and four clients for 10 minutes on one PC.
1. **Presence.** Read position, rotation, age, scene and event state each frame and send them 60 times a second. Draw the other players as markers with name tags in an overlay. No guest actor yet.
2. **Puppet Link.** Find this game's actor-creation function (the engine family's `ActorInstanceMgr::requestCreateActorAsync`; search for the string and its callers). Patch one per-frame function to call into the emulator, which spawns a Link that takes no input (`Player_Event` first, otherwise a small pack built on the `Player_Child` and `Player_Adult` models). Drive its transform every frame with smoothing; sync the animation name and frame; child or adult model to match.
3. **Scenes.** Show only players in the same scene. A "join partner" warp through the scene-change path. Hold or hide puppets while a cutscene runs.
4. **Shared progress.** Sync world flags through the host: chests, small keys, doors, heart pieces, songs, dungeon state. Decide what stays per player (likely inventory and hearts).
5. **Shared enemies and objects.** One owner per object with a real hand-over. Hits on a copy, arrows and bombs included, go to the owner as damage events.
6. **Polish.** Chat, player list, reconnects, settings.

## Stage 2: recomp

The same stages in a native recomp of the game, once it is playable. The runtime owns the sockets and can hook game functions at build time, so stages 1 to 5 move from emulator hooks to build-time hooks; the protocol and server stay as they are, and emulator and recomp players can play together.

## Decisions

Caleb's calls, taken 2026-11-09 (the suggested answers this plan assumed, plus the item rules).

| Question | Decided |
|---|---|
| Inventory, hearts and magic: shared or per player? | Per player, with shared world flags. Key items, empty bottles and one-time pickups (Gold Skulltulas, heart pieces) are shared; rupees, ammunition and bottle contents stay per player. Rules per item in [items.md](items.md). |
| Who hosts: a player's game, or a server anyone can run? | Either: the server program runs on its own or inside a player's build, and the first player in is the host. |
| What happens when the host leaves? | The next player becomes host; progress stays with the server. |
| Do players need to own the same save? | No. Each player keeps their own save; shared flags are added to it while in a session. |
| Branch protection on `main`? | On: a pull request needs the checks `check`, `windows`, `linux (gcc)`, `linux (clang)`, `sanitize (clang, asan + ubsan)` and `sanitize (clang, tsan)` to pass. Admins are not held to it. |
| Half-open connections from one address | At most 4 at a time. |
| Bytes a player may send through the server | 1 MB a second, bursts of 2 MB, besides the 60 + 240 messages a second; `FLAG_SNAPSHOT` (server only, bounded by max_flags) is not counted. The fast (unreliable) messages leave the last 256 KB for the reliable ones, so flags, items and chat still go when poses fill the rate. See [SECURITY.md](../SECURITY.md). |
| Who may connect | Anyone who reaches the port, unless the host lists addresses (`coop_server --allow`, `server_allow` in coop-options.ini) or listens on one address (`--bind`, `server_bind`). |
| Times in `PLAYER_SAMPLE` | Server time. ENet's throttle stays at its default. |
| Player colours (tunic by slot) | Later release. |

## Risks

- Lichii keeps changing. Co-op code stays in its own folder with few hook points, so new Lichii releases are easy to take.
- Every player needs the same co-op build and game version.
- Anything built on Lichii is GPL-3.0: its source must be shared with every build that is shared.
