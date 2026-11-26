# Agent state: what is done, open and waiting

Public coordination state for anyone working on this repository, including the maintainer's agent. It holds facts
only: what is done, what is open, and what we wait on. The handoff for the maintainer's agent is in
[agent-handoff.md](agent-handoff.md). Its address list is in [addresses.md](addresses.md).

Last updated: 2026-11-22.

## Integration and pull request #315

- Pull request [#315](https://github.com/daurosk/oot-remake-multiplayer/pull/315) (the play-test kit, the agent
  handoff, the address index and a build fix for the llvm-mingw toolchain) is reviewed and merged into the 0.11.12
  integration branch as authored; our fixes sit on top. Its branch `test-kit-0117` is no longer the integration branch.
- The play-test kit has two layers: the live kit ([playtest.md](playtest.md)) and the play-test bridge with one MCP
  server per game ("Playing through the bridge" there). The rules for an agent tester are in [AGENTS.md](../AGENTS.md),
  section "Play-testing the game"; this page does not repeat them.
- Nothing is merged to main without the maintainer's go.

## Work in progress: Gold Skulltulas shared or not

Branch `token-share` (worker token-share). A host setting, `coop-options.ini` `gold_skulltulas = own`, sent as
`MOD_SETTINGS` mod `coop` key `gold_skulltula`. Shared: the owed token (`WorldSync::OweToken`). Not shared:
`FlagWatcher::keep_local`. Unit tests done; the lane run (a kill in A, the pickup in B, both modes) is open.
See [world.md](world.md), "Gold Skulltulas: shared or not".

## Work in progress: Character Select pick per age

Branch `char-ages`, local and not yet pushed.

- Done: the logic in `lichii/hook/coop_char_ages.h`, with its tests in `tests/lichii/char_ages_test.cpp`; the frame
  glue in `coop_hook.cpp` (`CharAgesFrame`, file `coop-character-ages.txt`); the Settings rows in the pause menu
  ("Character by age", "As a child", "As an adult").
- Open: handle the new request kind in `hookshot/src/coop_pause.cpp`; read the pickable characters from the mod's
  `characters.json`, filtered to the files co-op knows; pause menu tests; docs in [mods.md](mods.md) and
  [puppet.md](puppet.md); a game check (a pick per age switches at the Master Sword); then a pull request.
- Wish: the picks belong on the Characters screen. Today the picker sets the current age's pick; the Settings rows
  set either age.

## Character Select v0.2.2 in the kit

- The mod author released v0.2.2. It has the same characters as v0.2.1, but it ships as a zip that builds the mod on
  the player's own PC from the player's own game files. Its tool needs the game's romfs and four Python packages.
- Done in the 0.11.12 line (pull request #312): the kit installs v0.2.2 by running that tool on the kit's own romfs. The kit's pinned version
  (`CHARACTER-SELECT.txt`) changes with it. Tested on the kit's romfs: 2287 files in 155 s, no errors.
- The kit's script runs the mod's tool on the player's own game files; the NPC and default parts depend on it.

## Open report: Zelda's hair after a spin attack

- Report: an adult Zelda's back hair flips in front of her face after a spin attack.
- Not reproduced in game yet: charged, quick and running spins, with the Master and Kokiri swords, on flat ground.
- Cause, as far as the code shows: her cloth config lacks the guard that the NPC hair code has. The guard resets
  the cloth when the head-to-hair angle goes past 80 degrees. Her back hair's max distance is 60.
- The fix belongs to the mod author: add the guard or lower the range. We wait for exact repro steps before we
  report it.

## Mod author material (models and voices)

Material for a submission to the mod author and to the Squadala community. Nothing is posted publicly yet.

- A. Cow run animation: a fix in the rig tool's leg plan (skip the leg plan when the hip and knee pivot error is
  over 0.06 m), and the Cow and Wolfos added to the legs-as-they-are list, with the Cow's front legs set absolute.
- B. New characters: Stalfos, Stalchild, Stalchild Big and Dinolfos. Their rig needs a neck rename and hair bones.
- C. Voice rows for more characters: Sheik, Impa, Ganondorf (also as a child and as Phantom Ganon), Darunia, King
  Zora, Nabooru, Talon, Ingo, Dampe, Biggoron, the Gerudo, Goron, Zora and Hylian types, Skull Kid, the Twinrova
  sisters, the Cow and Kaepora.
- D. Direct enemy voices (for example Dark Link, Stalfos and Iron Knuckle) that skip the voice conversion step.
- Possible co-op issue: puppets may speak with the viewer's character voice. To check.

## Open reports for co-op

- Customize colours do not reach Character Select characters on the player's own model. Cause: co-op dressed only
  Link's own eyes and hair (`LinkOnly` in `lichii/hook/coop_char_colours.h`, called from `DressCharacters` in
  `hookshot/src/coop_hookshot.cpp`). Fixed in the code on branch `dress-colours`, checked in a two-game run on
  2026-11-23: the Eye Colour (and the ini's hair colour, real hair only) goes on every player model that has the
  material for it (your own, the pause menu's model of it, the other players' character puppets); Customize offers a
  character an Eyes page unless its model has no iris; an age or character switch dresses the new model (timing not
  measured). The tunics and the Cap trim stay Link's. Seen in the game: the colour on your own character (field and
  pause picture) and on its puppet, after an age and a character switch. The test set the pick by its flag; that flag
  with `CharacterMenu_FieldSwapPending` and a Customize open and close did not rebuild your own model, only an age
  change did ([puppet.md](puppet.md)'s rule). The Character Select menu's own swap was not tried. For a character with
  no iris (Phantom Ganon) the log says it has none (log only). Open: the Customize page itself (Eyes page, the no-iris
  line) was not seen, the test screenshots do not show the co-op menu; Impa's red iris showed a plain green for green,
  other colours not tried; the ini's hair colour not checked. Details in [puppet.md](puppet.md), "Co-op's colours on a
  character".
- The Cow's run animation (see A above).
- A crash at start while the PC's memory ran out (a `std::bad_alloc` thrown out of the look worker's thread). Fixed in
  the code on branch `look-crash`, not yet checked in a game: the texture compare no longer allocates a heap buffer
  per texture, and a pass of the look worker that throws is logged (`coop: look: the look worker's pass failed`) and
  tried again at the next pass. Two things are left open on purpose:
  - The fix to `GameTextures::List` (the `filled` count when all four tries are short of room) has no unit test: the
    function reads game memory, so only a game run shows it. To check in a game: several cold boots, no
    `coop: look:` failed line and no crash record.
  - The frame thread has no such guard. Under the same memory exhaustion a throw on the frame thread would still end
    the game. Not changed here: a guard there touches every frame call of the module and needs its own review.
  - The memory growth itself is not ours to fix. The maintainer is fixing the host's memory growth at the root, across
    all host-only paths. The look fix above (the compare in `Same`, `hookshot/src/coop_look_tunic.cpp`, reads into a
    stack buffer instead of the heap) stays as it is and is not extended to other paths; it is already on the
    integration branch.

## Deferred engine items

Listed in [boss-multiplayer.md](boss-multiplayer.md), section "Whole-game survey". They are: the Redead freeze, the
Meg decoys, the Dead Hand hands, presence-gated placements, and the Volvagia rocks.

## Waiting on

- Exact repro steps for the Zelda hair report.
- The maintainer's go before any public post or any change to main.
