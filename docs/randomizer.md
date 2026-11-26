# The randomizer in co-op: step by step

For 0.11.2 and later. OOTRR comes with the kit: 0.6.0 in 0.11.2, 0.6.1 in 0.11.3, 0.6.2 in 0.11.4, 0.7.3 in 0.11.5 to 0.11.7, 0.8.0 in 0.11.8, 0.8.1 in 0.11.9, 0.8.2 in 0.11.11. The full reference is [mods.md](mods.md#the-randomizer); this page is only the steps.

The randomizer is **off** until the host turns it on. Each game builds the seed by itself in the background, so every player (the host too) needs **one restart** after their build. Nobody has to download or install anything: do not put OOTRR into `mods` by hand.

## Before you start

- **Everyone on the same kit.** All players update to the same release (Update.bat). Another kit has another OOTRR, and the same seed is then another world (other items in chests). Since 0.11.2 the chat says so: "... has the randomizer seed X built unlike the host's ...".
- **Start the game fresh.** Every player closes the game fully and starts it again with `Start Squadala.bat`. Nobody keeps an old save loaded.

## Host

1. Open the game's **−** menu (or press **F1**; on the title screen **−**) and go right to the **Mods** tab, after **Multiplayer** and **Lobby**. Press **Settings** on the **OoT Remake Randomizer** row.
2. Turn **Randomizer** on. The row that shows "on" in the mod list is only the mod itself; the **Randomizer** switch inside Settings is what turns the randomizer on.
3. Choose the world:
   - **Seed**: leave it empty for a new one, or type a seed word to play a seed again.
   - **Settings**: one of the presets the kit's OOTRR builds (the row lists only those; the game tries each once per OOTRR):
     - **Default**: chests and the story and NPC rewards are shuffled; keys stay in their own dungeon.
     - **Keysanity**: every dungeon key (small keys, boss keys, Ganon's boss key) can be anywhere in the world; you start with the maps and compasses.
     - **Open areas** (`open`, from 0.11.12): Kokiri Forest, the Kakariko gate, Zora's Fountain, the Door of Time and Gerudo Fortress are open from the start, Ganon's trials are skipped and boss keys are removed. Small keys stay in their dungeons.
     - **Open areas, fast fortress** (`open_fast`, from 0.11.12): the same, but Gerudo Fortress is "fast" (one carpenter to free) instead of open.
     - OOTRR's own **easy**, **open_world** and **full_shuffle** presets are not offered: OOTRR 0.8.x cannot build them yet (they ask for settings its build cannot apply). The two Open presets are made only of the opening settings it does build.
   - Or **Settings string**: copy a string from OOTRR's page (it starts with `OOTRR1-`), select the **Settings string** row, and press **Left** or **Right** on a pad (the hint at the bottom says **Paste**) or click the sides of the row. The row takes the whole clipboard at once (line breaks and spaces at the ends are dropped; a string longer than the row takes is refused, never cut). On a keyboard you can also press Enter on the row and **Ctrl+V** (or Shift+Insert) while typing. A long string shows its start and its end (`OOTRR1-EYAQ...HIHE`); while typing it shows the end and a line under it counts the characters, so you can check it. It replaces Settings.
4. The chat says "Randomizer on: keep the game open until this chat says the seed is ready ...". **Keep the game open** for about a minute.
5. The chat says "Randomizer seed X is ready: RESTART the game now and start a NEW save". Close the game and start it again.
6. Host the session (Multiplayer tab), then start a **New Game**. An old save plays on, but its chests are not this seed's.

## Each guest

1. Start the game fresh and **join from the title screen**. Do not load a save yet.
2. The host's switch and seed reach your game by themselves; there is nothing to set. The chat says "Randomizer on: keep the game open ...". Stay on the title for about a minute while your game builds the seed.
3. The chat says "Randomizer seed X is ready: RESTART the game now ...". Close the game and start it again.
4. Join again from the title. Wait for "Success, load any save.", then load any save: you play the host's file.

Guests can join one after another. Each one waits for its own build and restarts once. A seed is built once and kept, so a guest who built it before skips the wait.

## Sharing the seed: spoiler log and settings string

From 0.11.4, every built seed folder has two text files for sharing, on the host's PC and on each guest's (every player builds the same seed, so the files are the same):

- `mods\randomizer-<seed>\settings.txt`: the seed's settings string (`OOTRR1-...`), the one OOTRR's page and the randomizer Discord use. With the seed word, anyone on the same OOTRR can make the same world. It is the string even when you picked a preset (**default**, **keysanity**, **Open areas**, **Open areas, fast fortress**). The seed's folder name carries the preset (`randomizer-<seed>-keysanity`, `randomizer-<seed>-open`, `randomizer-<seed>-open_fast`; none for default, `-s<hash>` for a Settings string), so the same seed word with other settings is another folder and another world.
- `mods\randomizer-<seed>\spoiler.txt`: OOTRR's own spoiler log, in OOTRR's format: what is in each chest and check, and the spheres (since OOTRR 0.6.2, in 0.11.4, the Bow, Bomb Bag, Slingshot, Magic Meter and Ocarina are "Progressive ...", with their tiers at the top). **spoiler.txt spoils the item locations**, so open it only when you want to know, and share it after the run.

`mods` is the game's mods folder in your kit (the same folder the size-table and last-build lines name). A copy of the spoiler log also stays in `mods\oot-remake-randomizer\randomizer-<seed>.spoiler.txt`, next to the seed file. Both files stay when the kit builds the seed again for the co-op puppets. A seed built with 0.11.3 or older gets them at its next start: the game makes the same seed again in the background (about a minute, no restart needed, your save stays).

## Sharing a seed you built yourself

From 0.11.8, a seed made outside the kit (OOTRR built from source, or OOTRR's own page) plays in co-op when every player has the same folder:

1. Build the seed for Hookshot into a folder named `randomizer-<seed>` (`ootrr build ... randomizer-<seed> --host hookshot`).
2. Zip that folder and send it to every player. Each one unzips it into their own `mods` folder, with the same name, unchanged.
3. The host leaves the **Randomizer** switch off and hosts. Everyone starts the game from the kit (its start fits the seed for the co-op puppets).
4. Guests join from the title as usual, then everyone starts a new save.

Each guest's chat says "Randomizer: the host plays the seed randomizer-X, and yours is the same: it stays on". A guest without the folder, or with a different copy, plays the base game's chests in the host's world; its chat says what to copy, and the host's chat names that player. Delete your copy, copy the host's whole folder in again (unchanged) and restart. Details: `docs/mods.md`, "Sharing a seed you built yourself".

## What the chat lines mean

| Line | Meaning | What to do |
|---|---|---|
| Randomizer on: keep the game open until this chat says the seed is ready ... | The build has started. | Wait. A restart now stops the build. |
| Randomizer seed X is ready: RESTART the game now and start a NEW save ... | Built. This run still plays the base game. | Restart. Host: New Game. Guest: rejoin and load. It repeats every 3 minutes and at each join until you do. |
| Randomizer off: restart the game to play without it | The host turned it off. | Restart. |
| Randomizer: NO SEED - you play the BASE GAME, not the host's seed X ... | Your game could not build it. | Start the game from the kit again (it fetches OOTRR again). If it still fails, the host picks other Settings, or send `mods\oot-remake-randomizer\last-build.txt` with a report. |
| [randomizer] I play without the host's seed: ... | Seen by the host: that guest's build failed, with the reason. | That guest follows the line above. |
| Mods: player N runs WITHOUT the randomizer seed X | That player plays the base game (not built yet, or not restarted). | Wait for their restart. "... runs the host's randomizer seed now" means they are in. |
| Mods: player N has the randomizer seed X built unlike the host's ... | That player is on another kit (another OOTRR), so they play another world. | That player updates to the host's kit. |
| Randomizer seed X was made again for the co-op puppets: RESTART ... | A kit update changed the co-op puppets' files. | Restart. Your save stays. |
| Randomizer: the host plays the seed randomizer-X, and yours is the same: it stays on | You have the host's hand-put seed folder (above). | Nothing; "... RESTART" in the same line: restart, it was off in your Mods list. |
| Randomizer: the host plays the seed randomizer-X, which you don't have / and yours is not the same as the host's ... | You play the base game's chests in the host's world. | Delete your `randomizer-X` folder (if you have one), copy the host's whole folder into your `mods`, then restart. |
| [randomizer] I play without your seed randomizer-X: ... | Seen by the host: that guest lacks your hand-put seed, or has another copy. | Send that player the folder. |
| "name" is a mod with its own size table (e.g. a randomizer seed from OOTRR's site) ... | A seed was made on OOTRR's site and put into `mods` by hand. Only you have it, and other players can't be shown. | Make the seed in the Mods tab instead, or turn that mod off. |

## Common mistakes (from player reports)

- **The randomizer was never switched on.** In the logs of reports #96 and #97 the **Randomizer** switch was off in every session. The chests were vanilla because no seed was played. Turn it on inside **Settings**, not just the mod's row.
- **Guests loading old saves.** A guest who joined with an older save loaded brought that save's items into the host's new file (fixed in 0.11.3). On 0.11.2 or older: guests close the game, start it fresh, join from the title, and load only after "Success, load any save.". Deleting your save folder does not help, because the items came from another player's running game.
- **Not restarting, or restarting too early.** The seed plays only after the restart that follows "is ready". A restart before that line stops the build; it starts again at the next start.
- **No new save.** The host starts a New Game after the restart.
- **Different kits.** A guest on another release (for example 0.11.1 with OOTRR 0.5.0 next to a 0.11.2 host) builds another world from the same seed word.
- **After a kit update that brings another OOTRR** (0.11.1 to 0.11.2, 0.11.2 to 0.11.3, 0.11.3 to 0.11.4, 0.11.4 to 0.11.5): the new OOTRR cannot use the old seeds, so the kit removes them. Each game builds the seed again at its next start, then needs one more restart, and the host needs a New Game.
- **Ruto's Letter on a seed with Zora's Fountain open.** In a co-op session the letter turns into an empty bottle as soon as you find it: King Zora has already moved, so the letter has no use. Playing alone, you keep the letter as before.
- **Even a randomized seed leaves many spots vanilla.** OOTRR does not shuffle everything yet, so some vanilla chests are normal.
