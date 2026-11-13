# Local co-op (split screen)

Status: in work, not in any release. What is in the code today is the layout maths and the
switchboard the menu talks to (below), with unit tests. Nothing here has run in the game yet.

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
| Input | The Player reads its pad in two small functions; a local player's reads are answered from its own device. The first player keeps every device nobody else took. |
| Views | The game's renderer draws in layers; the world is the layer `Main_3D_0`, and layer numbers 1 to 4 are free. The plan is one more world layer and one more camera per local player, each with its own part of the picture, made the game's own way, so culling and drawing per view are the engine's, and everything per scene is done once. |
| HUD | The first player keeps the game's HUD inside their view. The others get co-op's own HUD (hearts and items) first. |
| Online | One connection; every local player joins as their own player through it (types 460 to 469, [protocol.md](protocol.md#local-players-protocol-11)). |

The facts this stands on were read from the game's code (build `e0f3ccfa`), not yet seen in play:
where a second world layer keeps its camera and viewport, and the camera actor's name, come from a
first run with a log.

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
