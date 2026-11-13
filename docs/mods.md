# Mods

Co-op on Hookshot has its own mod loader. It sits in the pause menu's **Mods** tab, right after **Multiplayer**. It loads two kinds of mods from Hookshot's own `mods` folder:

- **Game file mods** (RomFS: textures, models, text, a randomizer seed). Hookshot reads them when the game starts.
- **Code mods**: a library (`.dll`) that the co-op module loads when the game starts. It can read and write the game's memory, run something every frame, draw over the picture, take keys and hook game functions.

The randomizer (OOTRR) is built in.

## For players

### Where mods go

Put each mod in its own folder under `mods` beside `hookshot.exe` (or the folder `HOS_MODS` names):

```
mods\
  better-hud\romfs\...            a game file mod (as made for emulators: works as it is)
  camera-tweaks\mod.json          a code mod: its mod.json and its .dll
  camera-tweaks\camera.dll
  oot-remake-randomizer\mod.json  the randomizer's switch and settings (made by the game)
  randomizer-AB12CD34\romfs\...   a seed the randomizer built
```

A mod made for an emulator's `load\<title id>\<name>\romfs` folder works the same: copy the `<name>` folder into `mods`. Code patches (`exefs`) are left out: Hookshot's game is compiled ahead of time.

### The Mods tab

- Each mod is a row: its name, version and author, whether it is on, and a note when something is wrong.
- **On / off** and **up / down**. The first row wins a file two mods have.
- **Settings**: a mod's own page (toggles, choices, numbers, text).
- **Restart needed**: game files and code are read only when the game starts. The tab says what changes at the next start; nothing is ever rebuilt.

The tab writes two files into the mods folder: `coop-mods.list` (order and switches) and `coop-mods.settings.json` (settings). Hookshot reads the list at its start (our patch 0011). A `hookshot.exe` without that patch loads every mod in its own order, and the tab cannot switch or move game file mods (code mods and settings still work).

### A mod that fails

A mod whose `mod.json` is broken, whose library does not load, that says no at its start, or that crashes, is switched off for this run with one line in `hookshot_log.txt` (`coop: mod <id>: ...`) and a note in the tab. The game and the other mods go on. `LICHII_COOP_MODS_CODE=0` loads no code mod at all.

### Mods in co-op

Every game tells the others which mods it runs (ids, versions and a hash of their files). When yours differ from the host's, the chat says so once and the Mods tab lists what differs: a mod you miss, one the host does not have, another version, or other files. The host is told which player differs. You can still play: a mod that changes what is in the world (items, chests, enemies) may then make your games disagree.

A mod marked `"coop": "own"` (a HUD, a camera) is your own business and is not compared.

A setting marked `"shared"` is the host's in a co-op game: your tab shows the host's value, locked. The randomizer's seed is one.

### The randomizer

The OoT Remake randomizer is [OOTRR](https://github.com/REALAZK/ootrr) (MIT, by REALAZK). It is not part of the game: it reads your own extracted game files and builds a mod from a seed.

1. Get OOTRR (its releases page) and put `ootrr.exe` in `tools\ootrr\` beside `hookshot.exe`, or name it in the tab's **OOTRR program** row.
2. In the Mods tab, open **OoT Remake Randomizer**: set **Game folder** to your extracted game (version 1.1.1: it holds `Pack`, `Banc`, `Event` and `System`). Without it, `OOT_ROMFS` is used.
3. Turn **Randomizer** on. With no **Seed** a new one is made. Pick **Settings** (OOTRR's presets: default, easy, open world, keysanity, full shuffle).
4. The game builds the seed in the background (about a minute; OOTRR's output is in `mods\oot-remake-randomizer\last-build.txt`) into `mods\randomizer-<seed>` and switches it on, first row.
5. Start the game again, and start a new save.

In co-op the host's switch, seed and settings are everyone's: a guest's game builds the host's seed by itself and asks for a restart. Every pickup is shared as without the randomizer. Each seed is built once and kept; switching back to a seed built before needs no build.

Known limits: a seed is built with the other mods that are on at that time (OOTRR keeps their larger size-table entries); turn other mods on or off before building, or build again. Starting items other than the Kokiri Sword, Deku Shield and Slingshot are not applied yet (OOTRR's own list).

## For mod authors

### mod.json

```json
{
  "id": "camera-tweaks",
  "name": "Camera tweaks",
  "version": "1.0.0",
  "author": "you",
  "description": "One line for the tab.",
  "coop": "own",
  "code": {"windows": "camera.dll"},
  "api": 1,
  "settings": [
    {"key": "distance", "name": "Distance", "type": "number", "min": 1, "max": 9, "step": 1, "default": 4},
    {"key": "mode", "type": "choice", "choices": ["near", {"value": "far", "name": "Far away"}]},
    {"key": "label", "type": "text", "max_length": 16},
    {"key": "smooth", "name": "Smooth", "type": "toggle", "restart": true, "help": "Shown under the row."}
  ]
}
```

- `id`: 1 to 48 of `a-z 0-9 . _ -`, starting with a letter or digit. Without a `mod.json` the folder's name gives it.
- `coop`: `everyone` (default: every player needs the same mod) or `own`.
- `code`: a path inside the mod's folder, or one per system. `api`: the interface version the code was written for; a loader with an older one refuses the mod.
- Settings: `toggle` ("0"/"1"), `choice`, `number`, `text`. `restart`: a change counts after a restart. `shared`: the host's value in co-op.

### Writing a code mod

Include `hookshot/src/coop_mod_api.h` (plain C, no other header) and export `coop_mod_start`:

```c
#include "coop_mod_api.h"

static const CoopModApi *api;

static void frame(void *user) { /* every frame, on the game's calc thread */ }

static int key(void *user, uint32_t vk, int down, int repeat) {
    if (vk == 0x76 && down && !repeat) { api->log(api, "F7"); return 1; }  /* 1: taken */
    return 0;
}

COOP_MOD_EXPORT int coop_mod_start(const CoopModApi *a) {
    api = a;
    char v[16];
    if (api->setting(api, "distance", v, sizeof v) < 0) return 0;
    api->on_frame(api, frame, 0);
    api->on_key(api, key, 0);
    return 1;
}
```

The table (version 1): `log`, `main_base`, `read`, `write` (guest addresses; offsets in our docs are from `main_base`), `setting`, `on_frame`, `on_overlay` (rectangles and text over the picture), `on_key` (key downs on Hookshot), `on_setting`, `hook` and `has_hook` (game functions of the runtime's hook list, before and after, with the registers), `restart_needed`. Register only inside `coop_mod_start`. Keep each callback short; never wait on the game. Calls that take `self` want the table you were given. A later version only adds entries at the end: check `api->version`.

Mods are loaded first row first and started at the first frame. `coop_mod_stop` (optional) runs at exit.

The interface is ours, modelled on NX2navi's plugin interface, and does not depend on Hookshot: the same mod is meant to load on Longshot once our module runs there.
