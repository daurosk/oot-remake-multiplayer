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

Guest memory: `main` is found by name in the loader's own module list (`AppLoader::ReadNSOModules`, as Fritchii's own Ocarina code does), else in the modules found in memory (`Core::FindModules`), else as the emulator's `FindMainModuleEntrypoint`. On Fritchii v5 only the loader's list works: the memory search finds nothing and the last fallback gives the code region start (0x8000000), not `main`, which is loaded at a different address each run (0x80702000, 0x80993000). Each read checks the range first (`IsValidVirtualAddressRange`) and uses `ReadBlockUnsafe`, so an unmapped address fails quietly and the present thread never waits on the GPU.

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

### The first run (2026-11-08, one PC, two instances)

Checked with `LICHII_COOP_DEBUG_CAMERA=1` (it marks yourself in yellow, labelled `you`, and every 120 frames logs `coop: camera debug: you at (x, y, z) clip (...) at px,py of WxH` and one `P<n> at (x, y, z) at px,py` line per other player), both instances in Kakariko Village:

- The camera is found (`camera found (2560x1440, clip y up)`); no flip is needed.
- The `you` dot sits on Link's feet and stays there when the camera turns (an edge arrow at the bottom when his feet are just off screen).
- The other player's dot sits where that player stands in their own game, stays there when the camera turns, and moves and shrinks as they walk away. With both at the same spot, the two lines match to the pixel: `you at (-119.1, 96.7, 58.7) at 1280,1253` and `P2 at (-119.1, 96.7, 58.7) at 1280,1253`.
- The world unit is about a metre (a child Link walks about 2.5 units a second; the camera is about 4 behind him). The markers are 24 px across at 10 units and not drawn nearer than 0.5 (`MarkerSettings` in `coop_hook.cpp`; the overlay's own defaults are for centimetres).

To check again, set `LICHII_COOP_DEBUG_CAMERA=1` before starting the emulator. If the dot is ever mirrored top to bottom, `LICHII_COOP_CAMERA_FLIP_Y=1` turns it.

## Not proven yet

- Two PCs over a real network (the first run was two instances on one PC).
- Other scenes than Kakariko Village, adult Link, and scene changes on the panel.
- No other Link is drawn yet (no puppet): the dot marks where the other player is.
