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

0. **Transport and two clients on one PC.** ENet, the server, a lobby and a heartbeat. A test tool runs two clients against the server. Done when two clients exchange messages for 10 minutes with no drops. Needs no game.
1. **Presence.** Read position, rotation, age, scene and event state each frame and send them 60 times a second. Draw the other players as markers with name tags in an overlay. No guest actor yet.
2. **Puppet Link.** Find this game's actor-creation function (the engine family's `ActorInstanceMgr::requestCreateActorAsync`; search for the string and its callers). Patch one per-frame function to call into the emulator, which spawns a Link that takes no input (`Player_Event` first, otherwise a small pack built on the `Player_Child` and `Player_Adult` models). Drive its transform every frame with smoothing; sync the animation name and frame; child or adult model to match.
3. **Scenes.** Show only players in the same scene. A "join partner" warp through the scene-change path. Hold or hide puppets while a cutscene runs.
4. **Shared progress.** Sync world flags through the host: chests, small keys, doors, heart pieces, songs, dungeon state. Decide what stays per player (likely inventory and hearts).
5. **Shared enemies and objects.** One owner per object with a real hand-over. Hits on a copy, arrows and bombs included, go to the owner as damage events.
6. **Polish.** Chat, player list, reconnects, settings.

## Stage 2: recomp

The same stages in a native recomp of the game, once it is playable. The runtime owns the sockets and can hook game functions at build time, so stages 1 to 5 move from emulator hooks to build-time hooks; the protocol and server stay as they are, and emulator and recomp players can play together.

## Risks

- Lichii keeps changing. Co-op code stays in its own folder with few hook points, so new Lichii releases are easy to take.
- Every player needs the same co-op build and game version.
- Anything built on Lichii is GPL-3.0: its source must be shared with every build that is shared.
