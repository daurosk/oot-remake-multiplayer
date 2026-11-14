# Local co-op (split screen)

Status: in work, not in any release. What is in the code today: the layout maths, the switchboard
the menu talks to, the second player's own Player and controller on Hookshot, and each local player
as its own player online (below), with unit tests. Nothing here has run in the game yet except the research run. Player 1's view goes into its part of the picture (step 3a); the other players' views come next.

## The goal

Two to four players on one PC, each with a controller (or the keyboard and mouse) and a view of
their own, in **one** running game: one world simulation, not one game per player. A split-screen
game can host or join an online session, where each local player is a player of their own (number,
name, colour), so the others see every local Link and the local players see everyone else.
The cost over single player should stay small: the views together draw about as many pixels as one
full picture.

## How it is built

| Part | How |
|---|---|
| Players | Each local player after the first is the game's own `Player_Child` / `Player_Adult`, made as the other players' real Link is (the native player work) and run by the game's own player code. |
| Input | The game's own controller code: a pad a local player takes is the game's Npad No.2 to No.4, and that player's Player asks the game for that controller (below). The first player keeps every device nobody else took. |
| Views | The game's renderer draws in layers; the world is the layer `Main_3D_0`, and layer numbers 1 to 4 are free. The plan is one more world layer and one more camera per local player, each with its own part of the picture, made the game's own way, so culling and drawing per view are the engine's, and everything per scene is done once. |
| HUD | The first player keeps the game's HUD inside their view. The others get co-op's own HUD (hearts and items) first. |
| Online | One connection; every local player joins as their own player through it (types 460 to 469, [protocol.md](protocol.md#local-players-protocol-11)). |

The facts this stands on were read from the game's code (build `e0f3ccfa`), not yet seen in play:
where a second world layer keeps its camera and viewport, and the camera actor's name, come from a
first run with a log.

## The second player (Hookshot)

`lichii/hook/coop_local_game.{h,cpp}` is `CoopLocal::Game` on Hookshot; `coop_local_input.h` is the
input routing, with no game code (`tests/lichii/local_game_test.cpp`).

- **The Player.** Local player k (2 to 4 on screen, 1 to 3 in the code) is a target of the puppet
  Director with the number 16 + k (17 to 19: past every session number, offline and online), made
  beside the first Link as `Player_<Age>` and then left to the game's player code
  ([native-player.md](native-player.md)). After a scene change the Director makes it again, and the
  input follows the new actor.
- **The pad** (Hookshot patch 0016, on top of pad-select's 0015, `host/pad_sdl.c`, `host/nn_hid.c`):
  the exe exports `hos_coop_pad_info`, `hos_coop_pad_read` and `hos_coop_pad_take`
  (`hookshot/src/hos_coop_pads.h`), found by name as patch 0008's. Pads are named as 0015 names
  them, by their place in SDL's list (from 1); 0015's filter of this game's pads keeps a taken pad
  too. A taken pad leaves the first player's merged pad (and its motion), its rumble stops, and the
  game's reads of Npad No.(k+1) get it (with the menu's button binds). The keyboard and mouse stay the
  first player's. Without the patch, local co-op is not offered. Patch 0017 is native-puppets-b2's
  wraps alone (its draft 0010, renumbered so 0010 stays b2's number; after 0005 and 0007). On the Longshot pin (hookshot/PIN) 0016 is ported (only its context: Longshot's comments and 0015's HD-rumble block); 0001-0005, 0007-0009, 0012, 0015, 0016 and 0017 apply there in number order. A pad that reconnects (a new opening) goes back to the first
  player in Hookshot; the seat takes it again at the next frame.
- **The read.** The Player's two pad reads (wraps 16 and 17) ask the controller manager's
  getController (main+0x3b25e00, wrap 19) for controller 0. While the Player of local player k
  reads, that ask becomes controller k, if the manager has one; every other reader (menus, the
  Ocarina, the first Link) still gets controller 0. So the game makes its own held, pressed and
  released words for that pad.
- **Not yet seen in a run:** that the game reads Npad No.2 once its style says connected (the log
  says `hid: the game supports npads ...` and `hid: the game reads npad 0x1`), that the manager's
  controller k is Npad No.(k+1), and that controller 0 does not follow every Npad (else the second
  pad would move both Links). If the manager has no controller k, the log says
  `local co-op: player 2: the game has no controller 1, so that Link gets no input`: the Player's
  input-off check (wrap 18) answers off for it, so it never reads the first player's pad.
- **Test switch:** `LICHII_COOP_LOCAL=<pad slot>` starts player 2 on that pad at the first frame in
  play, without the menu.

## Online

`lichii/hook/coop_local_net.{h,cpp}` (tests in `tests/lichii/local_game_test.cpp`; the server's in
`tests/server/session_test.cpp`): while the game is connected, each extra player in play asks the
server for a number of its own (`LOCAL_JOIN`, [protocol.md](protocol.md#local-players-protocol-11)),
again after a reconnect. A joined seat's samples, status and equipment are read from its own Player
(the same reader and sampler as the first Link's, pointed at that actor; scene, age and cutscene are
the first Link's) and go out in envelopes. The others see it as any player. Its colour is the seat's.
A refused seat (name taken, session full) plays on this PC only, and the log says why. Hosting or
joining is the same connection, so a split-screen game can do either.

## The research run

`LICHII_COOP_LOCAL_PROBE=1` (`lichii/hook/coop_local_probe.h`, module only, reads only) logs
`coop: local probe n: ...` every 10 s, five times:

- **Layers:** the object behind main+0x4e81cf0 (count at +0x20, array at +0x28 by layer id; the
  game's layer creator checks a new id is free there). Hookshot's FOV code reads the same object
  as the cameras' manager, with the gameplay `sead::LookAtCamera` at element 0's +0x78; so each
  layer likely holds its own camera there. Per layer: vtable, flags (+0x88), name (+0x8b0), the
  camera at +0x78 and whether it is a `LookAtCamera`, and where four floats look like a viewport.
- **Controllers:** the manager behind main+0x4e92f10 and its controllers 0 to 4 as getController
  picks them; a controller 1 that is there means a second player can read its own.

Pass: the report gets the layer fields (camera, viewport, flags) and whether controllers 1 to 3
exist; then the views (step 3) are code.

What it found (Hookshot, 2026-11-13): controllers 0 to 4 are all there (one type), so the game's
manager is built for five pads. The world layer is id 0 (`gsys::Layer3D`, its own `LookAtCamera`
at +0x78); `UI_3D_0` (id 6) is made later the same way with its own camera. Every layer has the
picture's rect, 0 0 1280 720, at +0x30 and +0x58, and 0 0 1 1 at +0x380. Ids 1 to 4 stay empty.

## The views

`lichii/hook/coop_local_layout.h` (no game code; `tests/lichii/local_test.cpp`):

- Two players: side by side or stacked (a setting). Three or four: a quarter each; with three, the
  third sits in the middle of the bottom row, so all views have one shape.
- The views never overlap and stay inside the picture; with an odd number of pixels the second
  column or row is the pixel larger.
- **View scale**, 50 to 100 percent: the size a view is rendered at, in steps of 8 pixels, never
  under 160 x 88. Two views at 100% render one full picture's pixels; at 70% about half.

### Player 1's view (step 3a, `lichii/hook/coop_local_view.h`)

The game sets a layer's viewport itself, with its AI node `OneShotLayerViewport`
(main+0x28e70e0). It finds the layer by its tag, then writes min x and min y at +0x30 and max x and
max y at +0x38. On the layer's projection (+0x80) it writes the ratio w / h at +0xb0 and 1 at +0x8,
which makes the projection again. The module does the same write each frame for the world layer
while local co-op is on, so player 1's view is its rect of the split. It writes only when the rect
is not the one wanted, and gives the whole picture back once when local co-op stops.
`LICHII_COOP_LOCAL_VIEWS=2` (tests) splits the picture with no second player. Seen in the game
(Longshot, 2026-11-14): the world is drawn in the left half at the half's ratio (Link is not
stretched); the HUD stays on the whole picture. Tests:
`tests/lichii/local_view_test.cpp`.

### The other players' views (step 3b, not built yet)

Static facts for the next step:

- The game makes its 3D layers in one place (main+0xe35a50, once): the creator main+0x1174420
  (kind, scene) finds a layer by its name first and makes one only when the name is new. Its name
  table (main+0x3f085b0) has only `Main_3D_0`, `UI_2D`, `UI_3D_0`, `UI_2D_Overlay`, `Tool_2D` and
  `Tool_2D_Super`, so a second world layer needs its own name and a direct call of the maker
  main+0x1174640. Its descriptor is 0x28 bytes (heap, a 0x100-byte part with the scene name `Main`,
  1, the layer name, the id). Then main+0x11745c0 (layer, 0) and main+0x336f5a0, which ties the layer
  to the render pipeline `GameScene` with the setup `Master_Field` through a 0x180-byte descriptor.
- `gsys::SystemTask::DrawMainScreen` (main+0x36ae750) calls the renderer's draw (its object's +0xe0,
  vtable slot 5) once a frame; each layer is drawn from there.
- Two ways remain open: a second `Main_3D_1` layer made at that same set-up (a wrap of main+0xe35a50
  that runs the maker again, in a game patch), or the one world layer drawn a second time in the
  renderer's draw with player 2's camera and rect. Both need a run to see whether the `GameScene`
  pipeline takes a second layer.
- The probe saw `UI_3D_0` appear during play, so the engine makes layers while the game runs.

Research switch `LICHII_COOP_LOCAL_LAYER=1` (`lichii/hook/coop_local_layer.h`, module only, needs
patch 0004's call and scratch): 5 s into play it makes one more world layer, once, as the set-up
does. It calls the maker main+0x1174620 (named `3D_0`, the tail of the game's `Main_3D_0` string, id
1), then main+0x11745c0 and main+0x336f5a0 with the set-up's own descriptors. After that, each frame
the layer gets the right half and its camera (+0x78) looks at the first Link from in front. Every
step and pointer goes to the log as `local co-op: layer research: ...`. A pass is a second picture
of the world in the right half.

Seen in the game (Longshot, 2026-11-14): the maker makes the layer (id 1, its own `LookAtCamera`
at +0x78 and its own projection at +0x80), the binder runs, and the game runs on for a minute with
it. But nothing is drawn in the right half: the layer is not in the pipeline's draw yet. Next: what
the set-up does after the binder for the first layer, and the layer's flags (+0x88).

### With no session

Split screen needs no server. With co-op off, `lichii/hook/coop_hook.cpp`'s `LocalFrameOff` reads
the first Link itself for local co-op's frame step (views, seats, the layer research). While
local co-op has extra players, it also runs the puppet Director, which makes their Players.

## The switchboard

`lichii/hook/coop_local.h` (`namespace CoopLocal`) is what the pause menu's Local co-op page calls.
Any thread may call it; nothing of the game is touched from the menu's thread.

| Call | What it does |
|---|---|
| `Devices()` | The input devices a player can take: slot, name, keyboard or pad. Empty when this build has no local co-op. |
| `Start(config)` | Seats (device, name, colour for 2 to 4 players), split, view scale. Answers at once: `Ok`, or why not (`Text(why)` is a line for the menu). |
| `Stop()` | Turns it off, also while it is starting. |
| `Get()` | `Off`, `Starting`, `On` or `Stopping`; the seats; which seat's controller is unplugged; why the last start failed. |
| `SetSplit`, `SetScale`, `SetDevice` | Changes while it is on. |

The game side implements `CoopLocal::Game` and calls `Core::Frame()` at the frame hook. It makes
the extra players one at a time; if the game cannot make one (or it does not appear within 600
frame hooks), every player made so far is removed again and `Get()` says `Failed`. A controller
that is unplugged leaves its player in the game, marked lost, until it is back or the seat gets
another device.

## What stays shared in the first version

- One save: items, rupees and the world's flags are the first player's.
- Menus, talks and cutscenes belong to the whole screen, and the world pauses for everyone.
- A loading zone taken by one local player takes all of them.
