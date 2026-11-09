# Co-op in the emulator

How presence (plan stage 1 step 1) runs inside the patched emulator. The code is in [`lichii/hook/`](../lichii/hook); how to fetch, patch and build is in [lichii/README.md](../lichii/README.md).

## Settings

Co-op is off unless a server is set. Two places, the second wins:

1. `coop.ini` in the emulator's config folder: `user\config\coop.ini` beside `lichii.exe` for a portable install, otherwise `%APPDATA%\eden\config\coop.ini`.
2. Environment variables `LICHII_COOP_SERVER`, `LICHII_COOP_PORT`, `LICHII_COOP_NAME`.

```ini
# coop.ini
server = 192.168.1.20      # IP or host name; host:port also works
port = 27510               # optional, the default
name = Saria               # 1 to 16 characters, different for each player
```

Any mistake (an unknown key, a bad port, a name too long) is written to the log and turns co-op off, so a typo never connects somewhere unintended. The log shows `coop: on, server ...` or `coop: off (...)` at start.

## What runs where

| Thread | What |
|---|---|
| Qt (GUI) | At start, `CoopWindow::Attach` reads the settings and starts co-op. A small see-through panel in the top-left corner of the game lists the players. On close, `CoopHook::Shutdown` says goodbye to the server. |
| Network | Owns the `coop::net::Client`: connects, reconnects, sends what the frame queued, feeds what comes in to `coop_presence`. Wakes every 2 ms or as soon as something is queued. |
| Present (`OnFrame`) | Once per emulated frame: reads the local player through `coop_host` (`PlayerReader` over guest memory), runs the `Sampler`, queues `PLAYER_STATUS` and `PLAYER_SAMPLE`, and asks presence where the others are shown. |

The connection is made when the emulator starts, before any game, and lasts until it closes, so players stay in the room across game restarts. Nothing is read or sent unless the running game is Ocarina of Time (program 0400486024DEC000) with build id `e0f3ccfaf50065ae`, the build the addresses in `coop/host` are for. A build that does not match is logged once.

Guest memory: `main` is found by name in the process's module list (`Core::FindModules`), else as the emulator's own `FindMainModuleEntrypoint`. Each read checks the range first (`IsValidVirtualAddressRange`) and uses `ReadBlockUnsafe`, so an unmapped address fails quietly and the present thread never waits on the GPU.

## The panel

The game's camera address is not known yet, so players are not drawn as markers in the world. The panel is the fallback the task allowed:

```
Co-op: player 1, ping 3 ms
you: KokiriForest_Child
P2 Saria: KokiriForest_Child, 412 units away
P3 Mido: HyruleField_Child          (grey: another scene)
P4 Darunia: no position             (grey: no sample yet, or faded)
```

Distances are in game units (`units_per_metre` is still 1, see [host.md](host.md)). The panel is a frameless tool window, like the emulator's performance overlay, because a child widget would sit under the native render surface.

### The camera, for later

The emulator already finds the game's camera for its DLSS path: `RasterizerVulkan::TrackOcarinaCamera` (`src/video_core/renderer_vulkan/vk_rasterizer.cpp`) reads the vertex-stage uniform buffer at index 3, checks the viewport size at +0x120 and the projection's w row at +0xa8, and takes a 4x4 view-projection matrix at +0x30 (the previous frame's at +0x180). That runs only with DLSS on and on the GPU thread, and the matrix's layout against `coop_overlay`'s column-major convention is not checked. Using it for markers means a small extra hook there and one run with the game to check the layout; the overlay library (`coop_overlay`) is ready for it.

## Not proven yet

- Nothing has run with the game: the reads, the samples, the panel's player lines and the distances are untested against a live game.
- The emulator was started without a game with co-op on and connected to a local `coop_server`; that proves the settings, the start, the connection and the clean stop only.
