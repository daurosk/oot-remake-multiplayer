# Overlay

`coop/overlay/` (library `coop_overlay`) turns the players that [presence](presence.md) shows into a list of markers with name tags to draw over the game. It draws nothing itself: no rendering code, no fonts, no game addresses. The caller (the Lichii hook, later the recomp) fills in the camera each frame, gets a list of plain `DrawItem`s back, and draws them however its renderer likes. Plan stage 1 step 1, drawing side.

## Use

```cpp
coop::overlay::Overlay overlay;                       // default settings
coop::overlay::Camera camera;                          // filled by the caller each frame
camera.view = ...;                                     // world -> view, column-major 4x4
camera.projection = ...;                               // view -> clip, column-major 4x4
camera.viewport_width = 1280; camera.viewport_height = 720;

auto shown = coop::overlay::collect(presence, client.server_now_us());
auto result = overlay.build(camera, local_scene, shown);
for (const auto& item : result.items) draw(item);     // far to near
```

`collect` calls `Presence::shown` for every remote player, so call it once a frame instead of `shown` (it advances what presence has shown). A caller with its own source of players can fill `Shown` itself.

## The camera

`Camera` holds the view and projection matrices (4x4 floats, column-major: element `[col * 4 + row]`) and the viewport size in pixels. **The address of the game's camera is not yet known**, so nothing here reads it; finding it is the hook's job. Any projection whose clip w is the distance in front of the camera works (OpenGL, Direct3D and the usual game perspective matrices all do this). `look_at` and `perspective` (OpenGL style) are there for tests and tools.

## What `build` does

For each player, in order:

1. A NaN or infinite position or fade: skipped as `invalid`. A fade of 0 or below: `faded`.
2. Not in `local_scene`, or its scene is not known: `other_scene`.
3. Projected with `projection * view`. Clip w is the player's **depth**, the distance in front of the camera. Depth 0 or below is `behind`; under `near_distance` is `too_near`; over `far_distance` is `too_far`.
4. Screen position from the normalised device coordinates: pixels from the top left, y down. A point exactly on the edge is on screen.
5. Just off screen (within `edge_reach` normalised units of the edge, 1 by default, so up to half a screen beyond it) the marker is moved along the line from the screen centre onto a rectangle `edge_margin` pixels inside the edges, with `edge = true` and `arrow_angle` (radians in screen space, 0 right, pi/2 down) pointing towards the player. Further off, or with `edge_arrows` off, it is skipped as `off_screen`.
6. Marker size is `marker_size * reference_distance / depth`, kept within `min_size` and `max_size`.
7. The name tag (the nickname, or `P<number>` when there is none) is anchored at the bottom centre of the text, `tag_gap` pixels above the marker's top. When the player has a head height (`Shown::head` above 0, world units above the feet) and is on screen, the tag is instead anchored `tag_gap` pixels above that projected point (`head = true`), so it floats over the head while the marker stays at the feet. When either would be above the top of the screen it goes below the marker instead (`below = true`, anchor at the top centre of the text).
8. `alpha` is the presence fade (kept at 1 at most), for both marker and tag, times the distance fade: 1 up to `fade_start` depth, falling to 0 at `fade_end`. A player on screen beyond `fade_end` is skipped as `too_far`; an edge arrow never goes below `edge_min_alpha`, so an off-screen player can still be found. `fade_end` 0 (the default) turns the distance fade off.

Items are sorted far to near (by depth), then lowest player number first, so drawing them in order puts nearer players on top. `Result::skipped` says why each other player was left out. A camera with a NaN or infinite entry or a viewport not above 0 gives `Error::camera`, and invalid settings `Error::settings`; both draw nothing.

## Settings

| Setting | Default | What |
|---|---|---|
| `near_distance` | 1 | nearer than this (world units of depth) is not drawn |
| `far_distance` | 10000 | further than this is not drawn |
| `marker_size` | 24 px | marker size at `reference_distance` |
| `reference_distance` | 300 | depth at which the marker is `marker_size` |
| `min_size`, `max_size` | 8, 48 px | the size is kept within these |
| `tag_gap` | 6 px | between marker and name tag |
| `edge_arrows` | on | draw players just off screen as edge arrows |
| `edge_reach` | 1 | how far off screen (normalised units) still gets an arrow |
| `edge_margin` | 16 px | how far inside the screen edge the arrow sits |
| `fade_start`, `fade_end` | 0, 0 (off) | depth where the distance fade starts and where it reaches 0 |
| `edge_min_alpha` | 0.6 | the least alpha an edge arrow fades to with distance |

The distances are in the game's world units, which are not yet measured (see [host.md](host.md), `units_per_metre`); the defaults are guesses to tune in a run.

## The look

The Lichii window draws the overlay like the game's own HUD (what it looks like: [lichii.md](lichii.md#the-game-look)). The font, the heart pictures and the place names are the game's, so they cannot be in git or in the kit: `make-puppet-pack.cmd` makes them on each PC from the player's own game files, right after the puppets (`tools/puppet_pack/hud.py`, run by `build.py`; `--no-hud` skips it, `--hud-lang EUfr` picks another language), into the puppet mod's folder `load/0400486024DEC000/coop-puppet/coop_hud/` (outside `romfs/`, so the game never sees it):

| File | Made from |
|---|---|
| `font-regular.otf`, `font-medium.otf` | `Font2/Scalable/CetraDisplayPro-Regular` and `-Medium` (`.bfotf`, the HUD's Latin font per `Font2/Font.bfarc` `Normal.buif`). A `.bfotf` is 8 header bytes, then the OpenType font XORed with the 4 bytes `a6 01 85 02` (checked on all 29 fonts of the game). |
| `heart-0.png` ... `heart-4.png` | `UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx`: `HeartW_00_ASTC4x4` (the red heart), `HeartBase_00` (its shape, drawn dark for empty), `HeartSh_00` (shadow), `HeartGlowBase_01` to `_03` (the 1/4, 2/4, 3/4 masks), composed into a heart with 0 to 4 quarters filled, 96 x 96. |
| `places.json` | `Mals/USen.Product.110.sarc` `LocationMsg/Location.msbt` (36 place names, UTF-8): each label as it is, and every scene of a `Banc/<folder>` whose folder has a label (the boss rush's `GohmaStage` is in `Banc/DreamWorld`, "Hall of Fallen Foes"). |
| `hud.json` | what was made, the format number and the language |

The pictures are read with `tools/puppet_pack/texture.py`: the `.bntx` header of this game says platform `Ounc` (Switch 2), textures are block-linear like the Switch's, but inside each 512-byte GOB x bit 4 and y bit 1 trade places (found by decoding the HUD's heart shadow; every heart then decodes cleanly); formats BC4 and ASTC 4x4 (LDR, sRGB), decoded in plain Python. A failure prints a note and leaves the puppets made; the window then keeps its plain look.

In the emulator, `CoopHud::Folder()` (`lichii/hook/coop_hud.cpp`) finds the folder once (the default mod name, then any mod folder under the title; `LICHII_COOP_HUD_DIR` for tests, `LICHII_COOP_HUD=0` to turn it off) and logs `coop: hud: the game look from ...` or `coop: hud: plain look (why)`. The window loads the fonts with `QFontDatabase` and the hearts as pixmaps; one missing file means the plain look (`coop: hud: ... could not be loaded; plain look`). A scene missing from `places.json` shows as its name made readable (`SageRoom` -> `Sage Room`).

Sizes are in the pixels of the game's 720-line picture and scale with it: blocks start 40 left and 0.38 of the picture height down (`CoopHudLayout::kBlocksTop`, `lichii/hook/coop_hud_layout.h`: clear of the game's hearts, magic bar and d-pad item row, which end at about 0.30 at most; the same at 16:9 and ultrawide, where the 16:9 picture is pillarboxed), name 25 px, place 19 px, hearts 38 px pictures (about 20 px of heart) 0.55 of that apart, ten to a row; chat 23 px.

## Not yet proven

- The camera address and the game's matrix layout (whether it is column-major, and its clip w convention) are not known; the hook must check them in a run.
- The distance defaults have not been tried against the game's units.
- Nothing has been drawn yet; this is tested only by unit tests (`tests/overlay/`).
