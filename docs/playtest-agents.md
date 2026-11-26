# For agents: play-testing the game with the kit

For an AI session (or a person) that was handed **running test games** with the play-test bridge on. It says how to
start, drive, read, shoot and replay, and what you may write down for the next tester. The tool list and the bridge
are in [playtest.md](playtest.md); this page is the order to do things in and the rules.

A coding session never starts games itself: the maintainer's test lane holds the two test copies, and only a run the
maintainer gave you may start them ([playtest.md](playtest.md), "On a test lane"). If no games are running, you
cannot play-test: write the check down in the pull request's "Check in game" list instead ([AGENTS.md](../AGENTS.md)).

## The hard rule

**Nothing that came out of the game goes into the repository, an issue or a pull request.** That includes
screenshots, game logs, saves, extracted data and builds. Screenshots and logs stay on your PC (`<log dir>/shots/`,
the session folder). What you may commit is information about the game (a position, a menu path, a flag name) and
calls you wrote. `tools/check_content.py` enforces it and must pass; never bypass it or the git hook.

- A log goes to the maintainer only through the `/bugreport` command on the Squadala! Discord, never as an
  attachment on a public issue or pull request ([setup.md](setup.md)). `check_logs` masks lines for you to quote;
  `log` is raw: never paste it.
- Never write an address, IP address, user name, password or local path from a run into a file, an issue or a commit.
- A run writes into the working folder by default (`playtest-logs/`, `shots/*.png`, `calls.jsonl`, `.mcp.json`, a
  `sessions/` folder). `.gitignore` and `check_content` keep these out, but do not rely on them: put `--log-dir` and
  `--session` outside the repository when you can, and **add only your `tools/playtest/` file** (`git add
  tools/playtest/...`), never `git add -A` or `git add .`.

## 1. Start

1. **Check the setup.** `python -I tools/autotest/kit_doctor.py --lane N` prints one `OK`, `WARN` or `FAIL` line per
   check and changes nothing. Fix every `FAIL` first ([playtest.md](playtest.md), "Kit doctor").
2. **Start the games** (maintainer's lane): `python -I tools/autotest/live.py start --playtest --lane N --session DIR
   --server coop_server.exe`. It plays each game's boot script (a warp past the title), leaves both games open and
   starts the watchdog that ends the session after `--max-minutes` (30) or `--idle-minutes` (10) without a tool call.
3. **Connect the tools.** `python -I tools/autotest/kit_doctor.py --write-mcp .mcp.json --lane N --session DIR
   --log-dir LOGDIR` writes one MCP server per game (`oot-p1`, `oot-p2`) from the template, plus `oot-playtest` for the
   session. Register them before your session starts (`claude mcp list` shows `Connected`). All servers share one
   `--log-dir` and one `--session`.
4. **Hold every game first.** Call `get_state` on each game before anything else: a game whose tools you never called
   still runs in real time. After that **the games are paused between your calls**: nothing happens unless you act.

## 2. Drive

- Set a test up with `warp`, `place`, `flag` and `give`. Do not walk across Hyrule. `warp` in free play goes by
  Longshot's teleport (a map of its list, no entrance); for an entrance or another map, start the session with a boot
  script that warps ([playtest.md](playtest.md), "Rules for a tester").
- Move with `walk_to x z` (short legs, a clear line: it does no pathfinding), `go_to_actor id name`, `press`. After a
  `warp`, `step_until "event == false"` instead of a `step`, `get_state`, `step` loop.
- Climb, push and jump with `climb` (ladder or vines, to a height), `push` (a block, a distance) and `jump` (run off
  an edge toward a point). `push` holds A to grab the block first. Fire with `shoot item` at an `id` or a point: it
  turns the view with a stick until the target is under the reticle, as a player aims, and says `not aimed` (no shot)
  when it cannot; `hit`, `miss` or `not fired` comes from the target's life and the ammo (`assist` only nudges a shot
  that was already within 4 degrees and 0.5 m). Link turns with the stick, never by a written yaw. Each says why it
  stopped (`stuck`, `not climbing`, `slipped`, `respawned` ...).
- Act with the helpers: `attack`, `face`, `talk`, `open`, `use_item`, `kill`/`hit` (the **name** from `get_world`, and
  the `id` when there are several).
- Talk with `talk`: it reads the speakers and the reply buttons and presses A to the end. When buttons show and no
  `reply` is left it stops and lists them: talk again with `reply` (a text, or a list in order) and the `since` it
  gave. `boxes` hold each box's words (the markup cut out, kept in `raw`) with its speaker; `get_text` reads the texts the game set since a `last`. Both need a game built with patch
  0041 (`hooked` true); without it `talk` presses A once and says so ([playtest.md](playtest.md), "Text boxes").
- Fight with `fight id name`: it walks up, locks on (`ZL`), slashes (`Y`) and guards (`ZL` alone) until the enemy's
  life is 0, Link is low, or the frames run out, then answers `dead`, `gone`, `low`, `stopped` or `alive` with a `look`.
  It stops (`stopped: event`) during a cutscene and presses nothing: wait it out first (`step_until "event == false"`;
  a dungeon's title card after a warp is one). Then `desync_check` and look in the other game. A boss that needs a stun first (Gohma: a Deku Seed in her eye) needs
  `use_item` before `fight`.
- Let the other game run (`step 30`) after an action that should sync: a held game receives nothing.
- Stuck? Try the data first ([tools/playtest/data](../tools/playtest/data/README.md): known stuck spots and how to
  get round them). If a step fails twice the same way, stop, write down what you saw and go to the next goal. Do not
  retry for ever, and **never edit the mod's source during a run**: report, do not fix. A fix is another session.

## 3. Read state

- `get_state`: scene, Link's place `[x, y, z]` in metres, facing (0 = +z, 90 = +x), life, event, loading, the A
  prompt, `room` (a dungeon room, guessed from the puppet pack's rooms table) and `body` (child or adult, from our Link
  actor: trust it over `age` right after an area change). Pass the last `state_id` as `since` and only what changed
  comes back.
- `get_world [kind] [within_m]`: the actors near Link (enemies, chests, doors, switches), with an `id` you pass to the
  helpers, solo too (`solo` true: there `kill`/`hit` take the name only). An empty list with a `why` is not "nothing
  here".
- `check_logs`: the oracle for "did anything go wrong": flagged `coop:` lines, **masked**. A clean verdict is not proof.
- `session_status`: is the run still sound (bridge connected, session not ended, both games alive).
- Pos and facing are the game's last running snapshot: after steps on a held game they can be old
  (`snapshot_stale`); read `get_state` again after a `step`.

## 4. Shoot and compare

- `checkpoint TAG` on **every** game with the same tag after each action that should sync, then `desync_check`. A
  `live` difference counts after two looks 60 frames apart ([playtest.md](playtest.md), "What `desync_check` compares").
- `screenshot` only when the picture matters (a puppet, a door, a chest lid). Anything you can see is proven by a
  shot, never by flags alone. The shot stays on your PC; name its path in your report.
- A bug report has: the scenario, the steps (the tool calls), what was expected, what happened, the first tag where
  the games differ with each game's frame, and the shot paths. Quote only masked lines.

## 5. Replay and share

Every kit call is written to `calls.jsonl` in the session folder, one JSON line each. Run a log again with
`python -I tools/autotest/live.py replay --session DIR [--dry-run] [FILE]`: looks are skipped, it stops at the first call
that fails. A replay starts from the state the first run started from; a walk may land on other frames.

When a run found a good way to set something up, **save it as a script** for the next tester:
`python -I tools/autotest/kit_library.py save DIR/calls.jsonl --out tools/playtest/scripts/NAME.jsonl --what "..."
--scene Map --start "..." --expect "..."`. Read the file, take out wrong turns, add the expected result.

Positions, routes, menu paths and stuck spots you found go in [tools/playtest/data](../tools/playtest/data/README.md),
one JSON file per scene, with where each number came from.

Both folders, the formats and the check are in [tools/playtest/README.md](../tools/playtest/README.md).
`python tools/autotest/kit_library.py check` reads every file; CI runs it and `tools/autotest/tests/test_kit_library.py`.
Share them like any change: fork, branch, a normal pull request ([AGENTS.md](../AGENTS.md), "How to contribute"). Say
which game version and kit version the file was made with and whether you ran it in a game or only checked it.
