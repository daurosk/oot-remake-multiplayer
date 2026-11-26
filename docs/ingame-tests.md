# In-game tests (one system, over the play-test bridge)

Every in-game check is a pytest test in `tools/autotest/ingame/`, written once and run every rc. Tests drive the
games only through the play-test bridge ([playtest.md](playtest.md), "Bridge protocol"): set-up by `warp`, `place`,
`setflag`/`flag`, `give`; input by the bridge's `input` with frame steps. No OS key presses, no sleeps, no window
focus, no screen grabs, so the monitor, the display and windows on top cannot break a run.

Maintainer only, like the rest of `tools/autotest`: it needs a test lane (`tools/autotest/lanes.py`) installed by
`offline/lane-game-for.ps1` (its `lane-server.txt`).

## Running

```sh
python -I -m pytest tools/autotest/ingame --lanes 1 --expect-commit <sha> --out <dir>   # lane 1
python -I -m pytest tools/autotest/ingame --lanes 1 --expect-commit <sha> --lane-check-only --out <dir>  # setup proof, no boot
python -I -m pytest tools/autotest/ingame -n 2 --lanes 1,2 --out <dir>             # both lanes at once
python -I -m pytest tools/autotest/ingame -m smoke --lanes 1 --out <dir>           # the rc smoke tier only
python -I -m pytest tools/autotest/ingame --lanes 1 --repeat 10 --no-smoke-gate --out <dir>   # flake rate
python -I -m pytest tools/autotest/ingame --fake --out <dir>                       # no game: fake bridges
```

One game pair is booted per lane (`live.py start --lane L --playtest`, the lane's own server from `lane-server.txt`)
and every test of that lane runs on it. With `-n 2` xdist worker N takes the Nth lane of `--lanes`. Ports: lane L,
player N on `5000 + 10*L + N` (playtest.md). The boot's setup proof is `pair.lane_check` (the same rule as
`offline/lib/lane-server.ps1` + `lane-pair.ps1`: both games' module is the one installed, from the same patches
build, the server exists), then `hello` on both bridges with Longshot's control answering. `--expect-commit` (the lane's module sha, from
`lane-server.txt`'s `commit=`) is required on a real lane. `--lane-check-only` runs that proof, the boot command
(`live.py start ... --dry-run`: the exact `hookshot.py run` argv through the runner's own parser and checks plus a parse
of the boot scripts, `run --check-args`; nothing starts) and the collection only (no boot, no test): one `SETUP-OK`/`SETUP-FAIL` line per lane, `<dir>/setup.json`, exit 0 only if all are OK
(one process: it refuses `-n` and checks every `--lanes` lane itself).

Game switches are per boot: `--env NAME=VALUE` (both games) or `--env A:NAME=VALUE` / `--env B:NAME=VALUE` (one
game), repeatable, set at the game launch (`live.py start --env`) next to the base `LICHII_COOP_PUPPET=1` and
`LICHII_COOP_TEST_TO_TITLE=1`; the boot's whole list is `summary.json` `identity.env`. A test that needs a switch says
so with `@pytest.mark.needs_env("NAME=VALUE", ...)`: a run whose `--env` lacks it DESELECTS it, with a
`deselected: needs_env <switches> <test>` line and a `summary.json` `deselected` entry (never hidden; the rc summary
makes it NOT-RUN if no env group ran it), so the default run has no SETUP-FAIL from it. A
malformed `--env` stops the run before any boot (pytest usage error, exit 4).

A look test sets its colours by switch, never by a lane's `coop-options.ini` (it keeps whatever earlier runs left):
`LICHII_COOP_COLOR=<colour>` is the tunic, `LICHII_COOP_LOOK=<key>:<colour>[,...]` every other part (its settings key
without `look_`, e.g. `cap_trim:yellow`). With `LICHII_COOP_LOOK` the module takes only those parts, not the file's, and
logs `coop: look: the parts are LICHII_COOP_LOOK's (...)` (`hookshot/src/coop_look.h` `EnvParts`). Example:
`test_look.py::test_kokiri_hood_band`.

## Results

One summary per run: `<dir>/summary.json` and `summary.md` (add `--junitxml <file>` for JUnit); each test entry of `summary.json` carries its `notes` (`pair.notes`: the set-up time and what the test wrote down, such as where Link landed). The run also prints one
line to stdout for the queue log: `RESULT: PASS (...)`, `RESULT: SETUP-FAIL <why>` (nothing got past set-up) or
`RESULT: NOT PASS (<counts>); first: <test> <result> <why>`.

The boot (`bt_plugin.boot_args`, the one list the boot and the check use): `live.py start --lane L --playtest
--server <lane-server.txt's> --boot-a/--boot-b scenarios/live-kakariko-child.A/B.txt --no-load --env ...`: the
documented `live.py start` boot (child save, Kakariko by the well, through the title). `live.py start` checks those
runner arguments before it spawns its watchdog or takes the lane lock, so a refused start leaves no lock.
Cheats are ON in every lane boot: `live-kakariko-child.A.txt` has `#! switches LICHII_COOP_CHEATS=1` (the A script's
switches go to both games), which the bridge's `act`/`warp`/`flag`/`give` need. `summary.json` `identity.boot` keeps
each boot script's path, sha256 and switches, so a run with another boot never counts as the same proof.

Another boot: `--boot NAME` boots `scenarios/NAME.A.txt` + `NAME.B.txt` (e.g. `live-twinrova-room` for a boss stage,
where the bridge's `warp` answers `not_in_event` in free play). Each test runs with ONE boot: `@pytest.mark.needs_boot(NAME)`,
unmarked = `live-kakariko-child`; a run with another `--boot` deselects it (`deselected: needs_boot NAME <test>`, in
`summary.json` `deselected`). An unknown `--boot` is a usage error before any boot.

Pack files: `--pack NAME[,NAME]` (repeatable) makes co-op pack files with the TESTED sha's tools and puts them into
BOTH lane games' co-op pack (`mods/contents/0400486024DEC000/coop-puppet/<rel>`) after the lane check and before
the boot. They come out again after `live.py stop` (a file a pack replaced is put back). The maker is
`tools/autotest/ingame/packs/NAME.py`: `make(out, ctx) -> {rel: src}`, where rel is '/' separated under coop-puppet
(`coop_hud/map-arrow.png`). Its `ctx` has `tools`, `romfs` (lane A's extracted romfs, read only), `game_nup`
(`--pack-game`, else the .nup beside the kit root), `lane` and `say`. `packs/hud.py` makes `coop_hud/` with
`puppet_pack/hud.py` (fonts, hearts, map arrow, place names; about 60 s from the romfs). Every file is checked by
sha256 on each game, and `summary.json` `identity.pack` is `{name: {rel: sha256}}`. Each game folder gets a
`coop-test-pack.json` written before any file moves, so a run that dies leaves something the next boot undoes, with
or without `--pack`. A test that needs a pack: `@pytest.mark.needs_pack(NAME)`; a run without that `--pack`
deselects it (`deselected: needs_pack NAME <test>`). A maker that fails is a SETUP-FAIL, and the lane is left as it was.

Save profiles: `--save NAME` sets the flags of `tools/autotest/ingame/saves/NAME.py` in every
`autotest-save/slot_NN/progress.sav` of BOTH lane games. It does this after the lane check and before the boot. At
its start the game copies that baseline over its own saves (hookshot.py `restore_save`). `FLAGS` holds strings in
`save_flags.py set` form (`bool:0x6b093b51=0`), and each one cites where its hash comes from. Only flags already in
the save are changed; a missing flag is a SETUP-FAIL. Each save is kept in `coop-test-save-kept/`. A
`coop-test-save.json` is written before any change, so every boot undoes a run that died. After `live.py stop` the
baseline is put back. Every flag is read back on every save. A test that needs a profile:
`@pytest.mark.needs_save(NAME)`. A run with another `--save`, or none, deselects it. `summary.json` `identity.save`
is `{name, sha256: {game: {rel: sha256}}}`. `saves/gohma-alive.py` puts Queen Gohma back alive, with the quest at
her room (rows 137/138).

SOLO boot (owner rule: features that are not multiplayer must also work solo, with co-op idle): `--solo` boots only
lane game A, with no co-op session. That means `live.py start --only A` without `--server`, so hookshot.py run gives
it no `LICHII_COOP_SERVER`, and the lane's `coop.ini` has no server line. Only tests marked `@solo.MARK`
(`tools/autotest/ingame/solo.py`, marker `needs_solo`) run. Every other test is deselected (`needs_pair`), and a
run without `--solo` deselects the solo tests (`needs_solo`). The pair has game A only (`pair.c == {'A'}`), and
`step_both`, `press` and `warp_both` run on A. Setup proof, before any test: game A's module log has
`solo.IDLE` (`coop: no server in coop.ini: co-op idle until Host or Join ...`, hookshot/src/coop_hookshot.cpp).
It must also have no session line (`coop: on, server`, `connecting`, `welcomed`, `connected`). Otherwise every
test is a SETUP-FAIL. `--solo` with the no-CS switch is refused. `identity.solo` is true.

Shader precompile: test games start with `RX_VK_PRECOMPILE=0`, so Longshot builds no pipelines before the title and
loads each one when it is first drawn, from the pipeline caches (`hookshot.py` `TEST_LANE_PRECOMPILE`, applied by
`hookshot.py run` and `hs_steps.py run`). With Longshot's own start, every noted pipeline (about 42,700) stays in the
driver for the whole session. One game measured 29.5 GB of commit in play that way and 18.5 GB without the precompile,
with frame times no worse (game-commit-c141, run-8dd1d8af). `--precompile default` starts the games the way players
run them; an rc keeps one smoke run that way. `identity.precompile` says which start was used. An
`--env RX_VK_PRECOMPILE=<v>` wins over both. A player's kit never starts through these tools.

Game B without Character Select: `--env B:COOP_TEST_NO_CHARACTER_SELECT=1` (the switch in
`tools/autotest/ingame/no_cs.py`) boots game B from `lane<N>-B-nocs` instead of `lane<N>-B`. That folder is made
only when someone asks for it, with `python -I tools/autotest/lanes.py nocs --lane N` (`--dry-run` first). It is
made from lane B: hard links for every file no game writes, copies of the rest, but without the
`mods/OoT Character Select` folder and its `coop-mods.list` rows. It gets its own co-op pack, made with the lane
module commit's pack tool (`wt-module-<sha8>`), which then has co-op's own Characters tab and its code patch. It
also gets the game from the hs cache that was built with that patch instead of his. A cache miss is an error,
because `nocs` never builds a game. Its `lane-server.txt` is written last and names lane B's module, commit and
server, plus `nocs_of=lane<N>-B`. The lane check (`pair.lane_check(..., nocs=True)`) then also requires that this
module, commit and server are lane B's, that the game's `exefs=` line and `exefs_patches.txt` do not name Character
Select, and that its folder is gone. `summary.json` `identity` keeps `nocs` and `game_b`
`{lane: {folder, exefs}}`. Make the folder again after every lane prep: the check fails until you do.

| Result | When |
|---|---|
| PASS | every assert held |
| FAIL | an assert failed, or the test ran past its `budget` (FAIL `timeout`); a failed test reruns ONCE |
| FLAKY | failed, then passed on the rerun: never a pass |
| SETUP-FAIL | the lane, the boot, or the test's own set-up (`with pair.setup():`, under 30 s) did not hold; not rerun |
| NO-REF | a look check has no stored reference yet (its shot is kept under `<dir>/lane<L>/new/`), or the test called `pair.unproven(reason)` (it cannot prove its row in this run); judged by the exception type `pair.NoRef`, never by message text |
| NOT-RUN | `--deadline-min N` passed before it started (never a pass) |

A game that crashes while the lane boots or loads (its module log has a `CRASH` line) makes every test of the
lane FAIL with that line, not SETUP-FAIL: that window is what crash rows guard.

A smoke test (`@pytest.mark.smoke`) that FAILs or SETUP-FAILs stops the run: the rc is broken.

`--deadline-min N` is the run's wall-clock limit: set it below the queue job's Minutes (the queue's TIMEOUT kills
the process tree, which would lose `summary.json` and leave the games and the lane lock). Past N no test starts and
no FAIL is rerun; each test not started is NOT-RUN, the lane's games are closed at once (`live.py stop`: each game
quits through its bridge, is killed only if still up, the lane lock is let go), then the summary is written
(`stopped`, `lane_closed`). Every rc pass sets it. A run whose verdict is NOT PASS never exits 0.

Each test has a hard deadline (`@pytest.mark.budget(seconds)`, default 120): every bridge call checks it, and every
wait is counted in game frames with a ceiling (`pair.until`, `pair.expect_log`), so a test ends by itself.
pytest-timeout (pytest.ini, 1800 s) is only the backstop for a call that never returns.

## Judging

- By state: the bridge's state JSON (`scene`, `pos`, `equips`, `coop.players`, `world`, flags by `flag_read`), the
  module's log lines (`pair.expect_log`: the game's `game-out-X.txt`, only lines written since the test began), and
  checkpoints with `desync_check.py`.
- By picture ONLY for what a player sees (worker-rules TEST SYSTEM: other players' Epona animated, menu UI, HUD UI,
  name tags, balloons, pause pages, a new or changed look): `pair.check_shot(game, name, tolerance)` on EACH game,
  twice a few frames apart where animation must show. The shot is the game's framebuffer (the bridge's `shot`), and is
  compared to `tools/autotest/ingame/refs/<name>-<game>.png` by SSIM (scikit-image) on EACH colour channel: the
  score is the worst of 8 x 8 tiles of the SSIM map, each tile's worst channel, so one small missing thing fails its
  tile and so does a recoloured one (a grey compare is blind to a tunic, trim or HUD tint fault: green (0,91,0) vs
  blue (0,60,160) at the same grey scores grey 1.0000, colour 0.1128). Shots are kept with the result
  (`<dir>/lane<L>/shots/`). A test never writes a reference: a person checks a NO-REF candidate and copies it into
  `refs/`.
- Longshot's ImGui overlays (co-op's map arrows, tabs, name tags, its menu) are drawn after the bridge's `shot` is
  taken, so they are never in it. For those, `pair.window_shot(game, name, tolerance)`: the screen's copy of the
  game's window through the shared `coord-multiplayer/offline/lib/screen-shot.ps1` (game raised, cover check;
  helper from `OOT_WINDOW_SHOT`, else that default path), cut to the window's client area (no title bar, no
  desktop), compared the same way to `refs/<name>-window-<game>.png`. A covered window, a failed copy, no helper or
  no pid is SETUP-FAIL, never PASS. It shows the last frame the game presented: step first when a newer one is
  needed. Code: `ingame/windowshot.py`; selftests `tests/test_ingame_windowshot.py`.

- Frame rate, the one wall-clock check (`test_unfocus.py`, row 83): fps is real time, so that test alone lets both
  games run (`resume`), reads Longshot's own 5 s present windows from the logs (`vi: N frames presented (X fps ...)`,
  `vi: present intervals: ...`) with no bridge call in between, and holds them again (`pause`). It needs each game's
  window behind (Longshot's `hid: game window not in front` line; `live.py` opens the games without the focus).

## The lane's boot proof, recovery and memory

- Boot proof: after `live.py start` and the hello check, inside lane_pair's try (so a miss stops the session),
  `Pair.boot_ready` steps both games out of the boot's load, warps a game in another scene than the boot scripts'
  `warp` line there by the bridge, and needs both in play (a fresh Link sample, no load, no event) in that scene.
  Anything else makes every test of the lane `SETUP-FAIL lane<L> boot: ...`, naming each game's scene, load, event
  and not_ready. (Pilot test-bridge-pilot-fa63e90f r1: B was left in Graveyard_Child in an event because
  `hookshot.py run` ended the boot script without waiting for its warp; it now waits, `wait_warps`.)
- Set-up checks step frames until ready (`pair.until_in_play`, 300 frames at most), never one state read: a stale
  sample right after a boot or a pause is a game not ready yet, not a FAIL.
- Recovery: after a test that did not pass, or one marked `leaves_play` (the title test, which runs last in its
  round), the next test first runs the boot proof again (`Pair.recover`): a miss cannot cascade. It waits on game
  state, not on a fixed time: 30 s for the stepped part plus 31 s for each game it has to warp back to the boot
  scene (the worst warp measured, `warp_both` 26.9-31.0 s for both games on a full lane, party-hud-bridge-6947aa3d),
  so 92 s at most. It is the lane's boot proof, not a test's set-up: a set-up that warps cannot fit 30 s.
- Module proof, before any bridge call: each game's log must have the module's `coop: playtest: listening on
  127.0.0.1:<its port>` line, else every test is SETUP-FAIL `lane N boot: X: the module never started` (with the
  log's last line). `hookshot.py run` counts a game ready only at Longshot's `rx: renderer: Vulkan (` line, not the
  precompile summary that comes before the start-up ends; a mark or test command the module never read fails the
  run, so `live.py start` cannot answer ok for a game with no module (party-hud-bridge-85e3b255).
- A test that PASSES but leaves a game away from the boot's place (not in play, or not in the boot's scene: a
  forgotten put-back) is caught after it: one state read per game (`Pair.away_from_boot`). The test's notes name it
  ("left the boot's place"), and the next test starts with the recovery, so it never runs in the wrong map (row 285).
  Put the boot back in the test anyway (guarded `finally`): the recovery costs the next test up to 92 s.
- The start precondition of a default-boot test is `pair.need_boot_place(age)` inside `with pair.setup():`. It
  checks that each game is in play (stepped), in the boot's scene, and of that age. Never check the age alone: a
  child Link in another map passes that.
- A warp with `arrive_in_event` (a boss intro) is stepped until the game is in the scene, not loading, in the event
  once the bridge's own 12 s wait is over; a warp takes longer than that under load. A boot whose scene starts an
  event on arrival (a boss stage boot) is taken the same way by the boot proof, then stepped until the event ends.
- No game runs free (boss-respawn-bridge-e3e6e253): a call is never sent with less reply wait than it needs
  (`REPLY_MIN_S` 2 s, plus its frames at 20 a second); when the deadline cannot give that, the test FAILs
  `timeout ... not sent` with the connection and the hold kept. A reply that is lost anyway (a dropped connection,
  a hung bridge) holds the game again at once (noted), and the next test starts after the recovery. A warp that
  meets Longshot's one teleport per 8 s per game ('still in progress') waits the gap out, stepping, and asks once more.
- Title test: `load_both` presses A only on a game whose log shows the title line since the way back was asked,
  never on a game in play (an A there opens and shuts the menu); `back to the title: given up` FAILs it at once.
- Memory: each game's commit charge is sampled every 30 s from the boot start into `<dir>/lane<L>/commit.csv` and
  `commit.json` (first, peak and last GB per game), also in summary.json `commit`.

## Writing a test

```python
@pytest.mark.budget(120)
@pytest.mark.row("#91 masks on the other player's puppet")
def test_x(pair):
    with pair.setup():                         # SETUP-FAIL if anything here fails or takes > 30 s
        pair.give("A", "PouchData.Armor_Child_Mask_Skull.StockNum")
    pair.act("A", "wear head Armor_Child_Mask_Skull")
    pair.expect_log("B", r"P1 wears head 'Armor_Child_Mask_Skull'", max_frames=300)
```

Every key, flag, line and log pattern cites its source in the test (worker-rules "no guessed values"); shared tables
are imported (`ocarina_notes.KEY_NOTE`), never copied. `pair.step_both(n)` runs both games in step; never let one game
run alone. The selftests (`tools/autotest/tests/test_ingame.py`, in CI) run the whole suite on two fake bridges
(`ingame/fakegame.py`).

## Porting a .ps1 row (row workers: follow this)

Owner 2026-11-26 08:35: every in-game check is a test here; no new or re-added `.ps1` run script. One row = one test
function (or a few, one per thing it proves) in a `test_<area>.py` file in `tools/autotest/ingame/`. Worked examples:
`test_rows.py` (each pilot row cites the old row it replaces).

1. **Read the old row, cite it.** Its group line (`tools/autotest/groups/*.txt`: `expect=`, `label=`, `see=`,
   `logonly=`), its scenario files (`tools/autotest/scenarios/<name>.A.txt` / `.B.txt`, the `#! switches` line) and its
   `.ps1` (what it proves, what counts as pass). Put `@pytest.mark.row("<#issue> <label> (<old name>)")` on the test.
2. **Fixture and budget.** Take the `pair` fixture (both games of the lane, already booted, in play after the smoke
   tier). Give `@pytest.mark.budget(seconds)`: the whole test's hard limit, a little over what it needs. Switches from
   `#! switches` -> `@pytest.mark.needs_env("LICHII_COOP_X=1")` (and the run gets `--env`); `A:`/`B:` for one game.
   A row whose place cannot be reached by `warp` in free play (a boss stage) -> `@pytest.mark.needs_boot(NAME)` with a
   `scenarios/NAME.A/B.txt` boot (the run gets `--boot NAME`).
3. **Set-up only by the bridge, inside `with pair.setup():`** (SETUP-FAIL if anything fails or it passes 30 s):
   - place: `pair.warp_both(scene)`, `pair.act(x, "place X Y Z")`, `pair.act(x, "face X Z")`; never walk to a spot
     (scenario `goto` / `toward` / `hold LSTICK_*` used only to get somewhere -> `place`/`face`).
   - items and flags: `pair.give(x, item)`, `pair.flag(x, name, value)` (scenario `give` / `setflag` / `setenum`);
     read back with `pair.flag_read` / `pair.state(x)` and assert it took.
   - other scenario lines the bridge runs as they are: `pair.act(x, line)` for `kill hit hurt place face aim wear dress
     assign chat menu setflag setenum` (docs/playtest.md "Commands").
   - prove the start state: asserts on `pair.state(x)` (scene, age, equips; `test_rows.py`'s `need_child_in_play`).
4. **Inputs = the old row's keys, in frames.** `press DLEFT 12` -> `pair.press("A", ["LEFT"], frames=12)` (bridge button
   names: `hookshot.py` KEYS); `hold L 30` -> `pair.press("A", ["L"], frames=30)`; `stick`/`rstick` -> `stick=[angle,
   strength]`; `wait N` -> `pair.step_both(N)`. Both games always run the same frames (`press`/`step_both` do it). No
   `sleep`, no OS key, no window focus, no wall-clock wait. Copy the old row's waits in frames unless you cite why not.
5. **Judge by state.** Each old `expect=` becomes `pair.expect_log(game, pattern, max_frames=N)` with the SAME pattern
   (old and new judge the same thing); "must not" lines -> `pair.never_log`. Positions, scene, equips, life, players ->
   `pair.until("what", lambda: <state check>, max_frames=N)`. Shared flags -> `pc.checkpoint` + `desync_check.py`
   (see `test_story_flag_shared`). `logonly=` rows are state/log only: no shot.
6. **Shots only for what a player sees** (old `see=` rows that are about looks: other players' Epona animated, menu UI,
   HUD UI, name tags, balloons, pause pages). Mark the test `@pytest.mark.look`, then at the assert:
   `pair.check_shot("A", "<name>-1", tolerance=0.80)` and the same on `"B"`, again a few frames later
   (`pair.step_both(6)`, `<name>-2`) where animation must show. The first run gives NO-REF and keeps candidates under
   `<out>/lane<L>/new/`; a person looks and copies the good ones into `tools/autotest/ingame/refs/` (never the test).
   Tolerance: start at 0.80 for scenes with moving things, 0.85-0.90 for still UI; say why in the test.
7. **Fake game + selftest, in your own files only** (one owner per file; never edit `fakegame.py`, `pair.py`,
   `bt_plugin.py` or `tests/test_ingame.py`: ask test-bridge for a harness change). The fake's reactions for your row go
   in `tools/autotest/ingame/fakes/<row>.py` (hooks `setup lane command after press act actors state`, listed in
   `fakegame.py`'s docstring; react only in your row's scene / switch / `BT_FAKE_*` variable), each line cited to the
   module source. Your row's selftests (e.g. that the test FAILs under the bug) go in
   `tools/autotest/tests/test_ingame_<row>.py`: CI's `tests/test_ingame.py` runs every such file (`load_tests`).
8. **Prove it, then retire the old row.** `--lane-check-only` (setup proof) on the lane, then one lane run
   (`--repeat 3` for a new row) through preflight + the queue. When it PASSes, say so in COORD-LOG; retiring the old
   `.ps1` and its group line is the coordinator's call.

## Tools (yes / no)

| Tool | | Why |
|---|---|---|
| pytest, pytest-xdist, pytest-timeout | yes | owner rule; xdist workers = lanes; timeout as the backstop only (on Windows it can only end the worker) |
| scikit-image, Pillow | yes | SSIM of shots against references |
| a virtual display driver | not needed now | bridge shots come from the framebuffer, not the screen; windows still open somewhere (live.py `--display`); worth it only so no window ever lands on the owner's monitor |
| Toxiproxy / clumsy | later, yes | lag and loss tests (playtest.md "Lag on purpose" has run.ps1's relays today) |
