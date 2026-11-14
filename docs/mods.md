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

A mod whose `mod.json` is broken, whose library does not load, that says no at its start, or that crashes, is switched off for this run with one line in `hookshot_log.txt` (`coop: mod <id>: ...`) and a note in the tab. The game and the other mods go on. `HOS_COOP_MODS_CODE=0` loads no code mod at all.

A mod whose code crashes the whole game or hangs it while it starts (a crash the guard cannot catch: a stack overflow, a fast fail, its own thread) is skipped at the next start and switched off, with a line in the log and a note in the tab; switch it on to try again (the game writes `mods\coop-mods.starting` while each mod starts). After the guard catches a crash the game goes on without that mod, but the mod may have left the game odd until the next start.

### Mods in co-op

Every game tells the others which mods it runs (ids, versions and a hash of their files). When yours differ from the host's, the chat says so once and the Mods tab lists what differs: a mod you miss, one the host does not have, another version, or other files. The host is told which player differs. You can still play: a mod that changes what is in the world (items, chests, enemies) may then make your games disagree.

A mod marked `"coop": "own"` (a HUD, a camera) is your own business and is not compared.

A setting marked `"shared"` is the host's in a co-op game: your tab shows the host's value, locked. The randomizer's seed is one. A guest keeps the host's values as its own afterwards, so a restart before joining again plays the same seed; change them in the tab to play your own.

### The randomizer

The OoT Remake randomizer is [OOTRR](https://github.com/REALAZK/ootrr) (by REALAZK, GPL-3.0-or-later since 0.5.0). The kit brings it: there is nothing to download or set.

- **OOTRR comes with the kit.** The kit's game build (`Build-Hookshot-Coop`, or the launcher's "putting the updated co-op into the game" step) downloads the OOTRR release named in `hookshot\OOTRR.txt` (0.5.0), checks its SHA-256 and puts it in `tools\ootrr\` beside the game (`hookshot\game\tools\ootrr\ootrr.exe`, with its `LICENSE` and `NOTICE`). The Linux kit does the same into `data/game/tools/ootrr/`. When that download fails the build only warns, co-op plays without the randomizer, and the next start tries again.
- **Your game files are the kit's own.** The kit's build already unpacks your game beside the program (`hookshot\game\romfs`, or `data/game/romfs` on Linux). OOTRR reads them there and never changes them; nothing is copied. They are the game version co-op needs (program 0400486024DEC000, update v131072, which is the 1.1.1 OOTRR asks for), so they always pass OOTRR's version check.

1. In the Mods tab, open **OoT Remake Randomizer** and turn **Randomizer** on. With no **Seed** a new one is made. Pick **Settings** (OOTRR's presets: default, easy, open world, keysanity, full shuffle).
2. The game builds the seed in the background (about a minute; OOTRR's output is in `mods\oot-remake-randomizer\last-build.txt`) into `mods\randomizer-<seed>` and switches it on, first row. An OOTRR that runs longer than 10 minutes, or while the game closes, is stopped; the seed is built again at the next start.
3. Start the game again, and start a new save.

Two rows are only for players who want something else: **Game folder** (other extracted game files, version 1.1.1, with `Pack`, `Banc`, `Event` and `System`; `OOT_ROMFS` does the same) and **OOTRR program** (another OOTRR). Leave them empty to use the kit's own. A row that names a folder or program that is not there says so; it does not fall back to the kit's.

When a seed cannot be built, the tab says why in plain words: OOTRR missing (start the game from the kit again, its setup downloads it), the game files missing (build the kit's game again), or game files of another version ("not version 1.1.1": dump the game with update v131072 and build again).


In co-op the host's switch, seed and settings are everyone's: a guest's game builds the host's seed by itself, with its own kit's OOTRR and game files and nothing to set, and asks for a restart. A guest whose game still cannot build it (the download failed, for example) plays without the seed and sends one chat line, so the host's chat shows which player and why: "[randomizer] I play without the host's seed: <why>". Until that player builds it, their game differs from the host's (the Mods tab lists it). Every pickup is shared as without the randomizer. Each seed is built once and kept; switching back to a seed built before needs no build.

**Off by default.** The **Randomizer** switch starts off: a new kit, a new mods folder or a kit put over an older one builds no seed and plays none until you (or, in co-op, the host) turn it on. A seed folder left from an earlier kit is switched off while the switch is off. Turning it on or off says so in the tab and in the chat: "Randomizer on: restart the game and start a new save to play it" (or "Randomizer off: restart the game to play without it"); the same when the host's switch reaches a guest. The game never restarts by itself. A guest whose host plays without the randomizer (switched off, or a game without it) plays without it too; its own switch is kept for a game alone.

**Presets and logic.** All five presets keep OOTRR's default logic, "Glitchless": every item the seed needs can be reached without glitches, so the Bomb Bag comes before a wall that needs bombs, the Bow before the magic arrows, and progressive items (Hookshot then Longshot, bracelet then gauntlets, scales, wallets, magic) come in order. OOTRR's "No logic" and "Bombchus in logic (experimental)" are not in any preset and not offered here.

**Items in a randomized seed, in co-op.** An item from a randomized check is shared as any other ([items.md](items.md)), and comes with what makes it usable: a bag or quiver with its capacity and a fill, the Slingshot and Bow with their ammo, a bracelet or gauntlets with the lift grade (bomb flowers, bombs), an ocarina equipped and playable, and the magic meter (`Item_FairyBless_MP`, shared since branch `rando-items`) with a full meter, 240 a level as OOTRR gives it. A shared Silver or Golden Scale also raises `PlayerStatus.DiveDepth` (a Float) to 6.0 or 9.0, as OOTRR writes it, through the game's own Float setter. The Giant's Wallet (`Item_Wallet_Large`) gives room for 1000 rupees, as its own game data says. Magic Beans and Bombchus stay per player by choice: a shared bean or bombchu pack gives the others none ([items.md](items.md)).

Known limits: a seed is built with the other mods that are on at that time (OOTRR keeps their larger size-table entries); turn other mods on or off before building, or build again. Starting items are OOTRR's (its starting module, on a new save, for each player who plays the seed): older OOTRR builds applied only the Kokiri Sword, Deku Shield and Slingshot; newer ones also give bags, strength, scales, magic, ocarinas and bottles (`start_items/give.rs`). Not checked with the OOTRR in the kit.

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

A whole sample is in `hookshot/samples/hello-mod` (`mod.json` + `hello_mod.c`; the tests build it as `hello_mod.dll`): copy both files into `modshello-mod`, start the game, and F7 shows its message in the corner.

The interface is ours, modelled on NX2navi's plugin interface, and does not depend on Hookshot: the same mod is meant to load on Longshot once our module runs there.
