# Local co-op (split screen)

Status: in work, not in any release. What is in the code today: the layout maths, the switchboard
the menu talks to, and the second player's own Player and controller on Hookshot (below), with unit
tests. Nothing here has run in the game yet. The views (one picture per player) come next.

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
  Director with the number 17 - k (16, 15, 14) while there is no session to give it one, made
  beside the first Link as `Player_<Age>` and then left to the game's player code
  ([native-player.md](native-player.md)). After a scene change the Director makes it again, and the
  input follows the new actor.
- **The pad** (Hookshot patch 0010, `host/pad_sdl.c`, `host/nn_hid.c`): the exe exports
  `hos_coop_pad_info`, `hos_coop_pad_read` and `hos_coop_pad_take`
  (`hookshot/src/hos_coop_pads.h`), found by name as patch 0008's. A taken pad leaves the first
  player's merged pad, its rumble stops, and the game's reads of Npad No.(k+1) get it (with the
  menu's button binds). The keyboard and mouse stay the first player's. Without the patch, local
  co-op is not offered.
- **The read.** The Player's two pad reads (wraps 16 and 17) ask the controller manager's
  getController (main+0x3b25e00, wrap 19) for controller 0. While the Player of local player k
  reads, that ask becomes controller k, if the manager has one; every other reader (menus, the
  Ocarina, the first Link) still gets controller 0. So the game makes its own held, pressed and
  released words for that pad.
- **Not yet seen in a run:** that the game reads Npad No.2 once its style says connected (the log
  says `hid: the game supports npads ...` and `hid: the game reads npad 0x1`), that the manager's
  controller k is Npad No.(k+1), and that controller 0 does not follow every Npad (else the second
  pad would move both Links). If the manager has no controller k, the log says
  `local co-op: player 2: the game has no controller 1` and the second Link follows the first pad.
- **Test switch:** `LICHII_COOP_LOCAL=<pad slot>` starts player 2 on that pad at the first frame in
  play, without the menu.

## The views

`lichii/hook/coop_local_layout.h` (no game code; `tests/lichii/local_test.cpp`):

- Two players: side by side or stacked (a setting). Three or four: a quarter each; with three, the
  third sits in the middle of the bottom row, so all views have one shape.
- The views never overlap and stay inside the picture; with an odd number of pixels the second
  column or row is the pixel larger.
- **View scale**, 50 to 100 percent: the size a view is rendered at, in steps of 8 pixels, never
  under 160 x 88. Two views at 100% render one full picture's pixels; at 70% about half.

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
