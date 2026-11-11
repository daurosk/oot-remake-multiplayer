# Co-op in the emulator

How presence (plan stage 1 step 1) runs inside the patched emulator. The code is in [`lichii/hook/`](../lichii/hook); how to fetch, patch and build is in [lichii/README.md](../lichii/README.md). The emulator is Fritchii at the tag in [`lichii/PIN`](../lichii/PIN) (`fritchii-v5.8.4` since 2026-11-11; the notes below that name v5 were found on v5 and hold on v5.5 to v5.8, whose changes do not touch the parts the hook uses; what v5.6, v5.7 and v5.8 changed that matters is in lichii/README.md). A co-op build has no Fritchii updater (lichii/README.md, "The emulator's own updater").

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

Shared flags (plan stage 1 step 4, [flags.md](flags.md)): `LICHII_COOP_FLAGS=<file>` names a list of shared world flags, one a line in the `SharedFlags` format ([host.md](host.md)). The player's items ([items.md](items.md), `item_shared_flags()`) are always added to it, also without a file; `LICHII_COOP_ITEMS=0` leaves them out (then, without a file, no flags are shared). A bad list is logged and turns flag sharing off, not co-op. For testing item sync, `LICHII_COOP_CHEATS=1` opens a debug window, "Co-op item cheats", that gives the local player any shared item flag (a Bool becomes 1, an Int goes up by one; a pouch item's `IsGet` flag too), written to the game's own flags on the next frame so the sync sends it like a pickup; each gift is logged as `coop: cheat:`. For checking flags (see [flags.md, "Live check"](flags.md#live-check)): `LICHII_COOP_DEBUG_FLAGS=1` logs every change in the game's Bool, Int and 64-bit-key tables a few times a second, `LICHII_COOP_DEBUG_KEY64=<hex>` and `LICHII_COOP_DEBUG_BOOL=<hex>` log one 64-bit key or Bool flag, `LICHII_COOP_SET_KEY64=<hex>` and `LICHII_COOP_SET_BOOL=<hex>` set one key or Bool flag to 1 once the game is up, and `LICHII_COOP_SET_AFTER=<seconds>` waits that long first (a write before the save has loaded is lost).

Any mistake (an unknown key, a bad port, a name too long) is written to the log and turns co-op off, so a typo never connects somewhere unintended. The log shows `coop: on, server ...` or `coop: off (...)` at start.

## What runs where

| Thread | What |
|---|---|
| Qt (GUI) | At start, `CoopWindow::Attach` reads the settings and starts co-op. A small see-through panel in the top-left corner of the game lists the players. On close, `CoopHook::Shutdown` says goodbye to the server. |
| Network | Runs `coop::host::Session::service` ([host.md](host.md#session)) for the `coop::net::Client`: connects, reconnects, sends what the frame queued, feeds what comes in to presence. Wakes every 2 ms or as soon as something is queued. |
| Present (`OnFrame`) | Once per emulated frame: reads the local player through `coop_host` (`PlayerReader` over guest memory) and hands it to `Session::frame`, which runs the `Sampler` and queues `PLAYER_STATUS` and `PLAYER_SAMPLE`; then asks the session's presence where the others are shown, and projects them with the camera the GPU thread found. With a shared flag list, `Session::frame` also gets a `GameFlagStore` over guest memory (`lichii/hook/coop_flags.cpp`), so the flag watcher reads and writes the game's flags in the same call. |
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

## Chat and player list

Two keys, taken only while the game's render window has the keyboard (in the game list or a dialog they do what they always did) and only with no Ctrl, Alt or Shift held, so the emulator's hotkeys still work:

| Key | What |
|---|---|
| Enter | Opens a one-line chat box at the bottom left of the game. Enter sends and closes it, Esc closes it, and so does clicking back on the game. While it is open it has the keyboard, so the game gets no input (the render window lets go of every held key when it loses focus). A message that could not be sent (not connected, or characters chat does not allow) turns the box red and stays in it. At most 200 characters. |
| Tab (hold) | Shows the player list in the upper middle of the game while held. |

Neither key is a button in the emulator's default keymap (A, B, X, Y, the stick clicks, L, R, ZL and ZR are C X V Z F G Q E R T, + and - are M and N, the d-pad the arrow keys, the sticks WASD and IJKL): Enter and Tab only reach the emulated USB keyboard, which Ocarina does not read. Both are eaten, so they do not reach it either. T, which many games use for chat, is ZR here.

Chat lines show at the bottom left, above where the box opens: chat as `Saria: hello`, your own as `you: hello` (the server does not echo chat back), and room notices in yellow: `Saria joined`, `Saria left`, `joined as player 1`, `connection lost, reconnecting`, `reconnected as player 1`, `server refused: ...`. Each line shows for 8 s, then fades out over 1 s; the last 6 are kept, and all 6 show again while the chat box is open. The lines come from `coop_chat.h`, filled on the network thread from the client's events.

The player list:

```
#   Player        State       Ping    Scene
P1  Saria (you)   connected   12 ms   KokiriForest_Child
P2  Mido          connected   ~45 ms  KokiriForest_Child
P3  Darunia       quiet       ~1840 ms HyruleField_Child   (grey)
```

- Ping is your own round trip to the server. Nobody's ping to the server is sent to the others, so for them it is marked `~`: how old their newest sample is on the shared clock, which is their way to the server, the relay, your way from it, and up to one sample interval.
- State, for you: `connected`, then `reconnecting` after a lost connection (the room is then not known, so the list shows only you), or the client's own state before the first welcome (`connecting`, `handshaking`, `waiting_retry`, `refused`). For the others: `connected`, `quiet` when their newest sample is a second old or more (a menu, a load, or a connection the server has not timed out yet), `no samples` before the first, `joining` before presence knows them.
- Scene is the scene name from their samples; yours from your own game.

The player name and the server are set in `coop.ini` (see [Settings](#settings)); there is no window for them.

## Markers

When the renderer has found the game's camera, the other players are also drawn in the world: a name tag floating over the player's head, or an arrow at the screen edge with the name beside it for a player just off screen (`coop_overlay` decides what, see [overlay.md](overlay.md)). A second see-through tool window the size of the render area draws them, refreshed every 16 ms and only repainted when they move. Without a camera it hides and the panel is all there is.

### Name tags

- **Where:** the tag's bottom sits 4 px above the top of the head. The head height comes from the player's age, which every sample carries: 1.35 units above the feet for child Link, 1.9 for adult (`kHeadChild`, `kHeadAdult` in `coop_hook.cpp`). The puppet's skeleton is not read for this; the pose copy (docs/puppet.md, Pose) only has bone-local transforms.
- **Look:** bold white text with a dark outline, 3.5% of the picture's height (14 to 32 px), so it reads on sky and on grass. There is no dot under a tag on screen. Yourself (`LICHII_COOP_DEBUG_CAMERA=1`) is tagged `you` in yellow.
- **Distance:** solid up to 10 units of depth (about the width of a room), fading out by 40 and not drawn beyond (`MarkerSettings`).
- **Off screen:** just off screen, a blue arrow at the edge points at the player, with the name beside it. It fades with distance too, but never below 60%.
- **Off the top:** a tag that would leave the top of the screen goes under the player instead.

Seen in the autotest kits (2026-11-09, `tools\autotest\run.ps1 -Scenario nametag -ScreenCopy` with `LICHII_COOP_PUPPET=1` and `LICHII_COOP_DEBUG_CAMERA=1`, Graveyard, both child): the camera is found in both games; in B the tag `A` floats just over A's puppet at 3, 13 and 16 units, and each game tags its own Link `you` just over the hat; in A, B standing behind the camera shows as an edge arrow at the bottom left with `B` above it. That run used 13 px text and a fade from 25 to 60 units. The final 3.5% text and the 10 to 40 unit fade are unit-tested only. `-ScreenCopy` is needed: the window's own print leaves the overlay windows out.

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

## Mods (LayeredFS)

Fritchii v5 applies LayeredFS mods to a `.nup` as it does to any game, with no patch of ours: `<load folder>\0400486024DEC000\<mod>\romfs\` is layered over the game's romfs, and the log says `RomFS: LayeredFS patches applied successfully`. The puppet pack (`docs/puppet.md`, Pack) is such a mod.

The load folder is the log's `LoadDir:` line. The emulator stores it (and the NAND and SD folders) in `user\config\qt-config.ini` as absolute paths, so a copied or moved emulator folder still reads the old folder's mods and saves. After copying a kit for a test, point `load_directory`, `nand_directory` and `sdmc_directory` at the copy.

## Not proven yet

- Two PCs over a real network (the first run was two instances on one PC).
- Other scenes than Kakariko Village, adult Link, and scene changes on the panel.
- No other Link is drawn yet (no puppet): the dot marks where the other player is.
