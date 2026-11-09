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

A `#` or `;` starts a comment at the start of a line or after a space or tab; one with no space before it (`Link#2`) is part of the value.

Two more environment variables are for checking the markers (see [the first run](#not-checked-yet-the-first-run)): `LICHII_COOP_DEBUG_CAMERA=1` marks yourself too and logs where, `LICHII_COOP_CAMERA_FLIP_Y=1` turns the markers upside down.

Any mistake (an unknown key, a bad port, a name too long) is written to the log and turns co-op off, so a typo never connects somewhere unintended. The log shows `coop: on, server ...` or `coop: off (...)` at start.

## What runs where

| Thread | What |
|---|---|
| Qt (GUI) | At start, `CoopWindow::Attach` reads the settings and starts co-op. A small see-through panel in the top-left corner of the game lists the players. On close, `CoopHook::Shutdown` says goodbye to the server. |
| Network | Runs `coop::host::Session::service` ([host.md](host.md#session)) for the `coop::net::Client`: connects, reconnects, sends what the frame queued, feeds what comes in to presence. Wakes every 2 ms or as soon as something is queued. |
| Present (`OnFrame`) | Once per emulated frame: reads the local player through `coop_host` (`PlayerReader` over guest memory) and hands it to `Session::frame`, which runs the `Sampler` and queues `PLAYER_STATUS` and `PLAYER_SAMPLE`; then asks the session's presence where the others are shown, and projects them with the camera the GPU thread found. No shared flags yet (no `FlagStore` for the game). |
| GPU (draws) | Per draw, only while Ocarina runs with co-op on: looks for the game's camera buffer (see [Markers](#markers)) and hands the matrix over. |

The connection is made when the emulator starts, before any game, and lasts until it closes, so players stay in the room across game restarts. Nothing is read or sent unless the running game is Ocarina of Time (program 0400486024DEC000) with build id `e0f3ccfaf50065ae`, the build the addresses in `coop/host` are for. A build that does not match is logged once.

Guest memory: `main` is found by name in the process's module list (`Core::FindModules`), else as the emulator's own `FindMainModuleEntrypoint`. Each read checks the range first (`IsValidVirtualAddressRange`) and uses `ReadBlockUnsafe`, so an unmapped address fails quietly and the present thread never waits on the GPU.

## The panel

Always shown while co-op is on, with or without markers:

```
Co-op: player 1, ping 3 ms
you: KokiriForest_Child
markers: on (camera 1920x1080)      (or "markers: no camera")
P2 Saria: KokiriForest_Child, 412 units away
P3 Mido: HyruleField_Child          (grey: another scene)
P4 Darunia: no position             (grey: no sample yet, or faded)
```

Distances are in game units (`units_per_metre` is still 1, see [host.md](host.md)). The panel is a frameless tool window, like the emulator's performance overlay, because a child widget would sit under the native render surface.

## Markers

When the renderer has found the game's camera, the other players are also drawn in the world: a blue dot with the name tag above it, or an arrow at the screen edge for a player just off screen (`coop_overlay` decides what, see [overlay.md](overlay.md)). A second see-through tool window the size of the render area draws them, refreshed every 16 ms and only repainted when they move. Without a camera it hides and the panel is all there is.

### Where the camera comes from

The emulator's DLSS path already finds the game's camera (`RasterizerVulkan::TrackOcarinaCamera`, `src/video_core/renderer_vulkan/vk_rasterizer.cpp`), but only with DLSS on. `lichii/hook/coop_camera_track.h` does the same checks without DLSS, from one call the second patch adds after each draw's pipeline is configured:

- the draw goes into a colour target of at least 640x360 in `B10G11R11_FLOAT` with a depth target (the scene);
- the vertex-stage uniform buffer 3 holds that target's size at +0x120 and (-1, 0) at +0xa8, the w row of the projection;
- the world-to-clip matrix is the 16 floats at +0x30. The emulator's own motion shader (`ocarina_actor_motion.vert`) reads it as `row_major` and multiplies `matrix * vec4(world, 1)`, with clip z from -1 to 1, so it is row-major world to clip.

The largest such target of a frame wins (reflections and half-size passes use the same buffer layout and are smaller), and the hook uses the last finished frame's. Which way clip y runs on the picture comes from the draw's viewport (`scale_y`, a `NegativeY` swizzle, a lower-left window origin). All this is done only while Ocarina of the right build runs with co-op on (`CoopCamera::SetWanted`), so the renderer pays nothing otherwise.

`coop: camera found (WxH, clip y up|down), markers on` is logged when it is first found, and `coop: camera lost, markers off` when it is gone for 250 ms (menus, loading).

### Not checked yet: the first run

Whether the positions `coop_host` reads are in the same world space as that matrix is not known; nothing has been run with the game. To check it with one emulator (a `coop_server` must be set so co-op is on, but no second player is needed):

```bat
set LICHII_COOP_DEBUG_CAMERA=1
lichii.exe
```

Load a save and walk around. Look for:

1. The panel says `markers: on (camera ..., debug)`. If it says `markers: no camera`, the buffer checks did not match: the log has no `coop: camera found`.
2. A yellow dot labelled `you` sits on Link (at his feet: the position is his origin) and stays on him when the camera turns. That proves the matrix and the positions agree, and other players' markers will be right.
3. Every 120 frames (2 to 4 seconds) the log has `coop: camera debug: you at (x, y, z) clip (cx, cy, cz, cw) at px,py of WxH`, or `skipped: <why>`.

If the dot is mirrored top to bottom (right left and right, wrong up and down), set `LICHII_COOP_CAMERA_FLIP_Y=1` as well and say so, so the default can be changed. If it is far off or `skipped: behind the camera`, send the log lines: the positions and the matrix are in different spaces (camera-relative, or other units), which the clip numbers show.

## Not proven yet

- Nothing has run with the game: the reads, the samples, the panel's player lines, the distances, the camera and the markers are untested against a live game.
- The emulator was started without a game with co-op on and connected to a local `coop_server`; that proves the settings, the start, the connection and the clean stop only.
