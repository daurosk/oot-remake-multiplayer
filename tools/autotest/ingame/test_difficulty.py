"""Difficulty #114 (C #114; old rows difficulty-lobby-run.ps1 = hs-difficulty-lobby, host-difficulty-run.ps1 =
hs-host-difficulty), on the bridge. Both tests boot `--boot live-lobby-hard` (scenarios/live-lobby-hard.A/B.txt:
live-kakariko-child with '#! server --difficulty hard', hookshot.py header_server_args): the lobby is made with Hard
before the first player joins, as hs-difficulty-lobby.steps.txt's '#! server --difficulty hard' did.

Run (one job, this file alone, --repeat 1: the second test leaves A's session, so nothing may run after it on this
boot; each set-up asserts both games list two connected players, so a wrong order or a repeat is SETUP-FAIL, not a
FAIL half-way: review-rows-c136 15:09):
    pytest test_difficulty.py --boot live-lobby-hard --lane 1 --repeat 1 --expect-commit <sha>

Sources of every value (worker-rules "no guessed values"):
- 'coop: difficulty: Hard' / '... Automatic': hs-difficulty-lobby.steps.txt lines hard-A/hard-B/no-auto-A; the module's
  line is lichii/hook/coop_difficulty_game.cpp:205 'coop: difficulty: {Describe} (set by N)', Describe =
  coop_difficulty.cpp:109 (a preset's name first, 'Automatic (N players)' when automatic).
- 'kept for the next lobby' / 'refused': hs-host-difficulty.steps.txt lines kept-A (min=3), kept-custom-A (min=4),
  refused-A (max=0); the module's line hookshot/src/coop_pause.cpp:1230 (Request::Kind::kDifficulty).
- the title's rows line: hs-host-difficulty.steps.txt rows-A.
- the keys: the old scripts' (hs-difficulty-lobby.A/B.txt, hs-title-menu.down/press.txt, hs-host-difficulty.hard/
  custom.txt); bridge names hookshot.py KEYS (DUP -> UP ...). A script `press K` holds PRESS_TICKS = 6 ticks and lets go
  6 (hookshot.py:51), a tick is 1/60 s (TICKS = 60.0) and a game frame 1/30 s (pair.FPS): press = 3 frames held, then
  (6 + wait) / 2 frames before the next key.
- menu pages: the state's `coop.page` word (docs/playtest.md "State"; CoopPauseMenu::PageWord: main, host, settings);
  the Difficulty row's name 'Difficulty' (coop_pause_menu.cpp DifficultyLines: Choice("Difficulty", ...)).
- `menu leave` (the Multiplayer page's Leave row) and `menu close`: lichii/hook/coop_testinput.cpp MENU, bridge `act`.
- the kept file: coop-difficulty.ini in the game's folder (coop_difficulty_game.cpp File(): ConfigDir; on the lanes the
  game folder, coop-kit/ls/lane1-A/coop-difficulty.ini beside coop.ini). A kept choice is OFFERED to every later
  automatic session where the player may edit (coop_difficulty_game.cpp:271 OffersKept): left behind it turns later lane
  runs to that difficulty (2026-11-26: lane1-A/lane2-A kept 'custom hp=300' from an old run; lane logs said 'Custom:
  life 300%'), so test_host_page_difficulty_kept puts the file back as it found it, always. The module reads it once
  at game start and the idle Host page starts on it, so a file there before the boot is SETUP-FAIL: in lane_check
  (pair.kept_difficulty, every lane run) and again in each set-up here (B #148 / review-rows-c130-6 02:22).
"""
import os
import re

import pytest

from pair import SetupFail, kept_difficulty

BOOT = "live-lobby-hard"
HARD = r"coop: difficulty: Hard"
AUTO = r"coop: difficulty: Automatic"
KEPT = r"coop: multiplayer tab: difficulty kept for the next lobby"
REFUSED = r"coop: multiplayer tab: difficulty refused"
TITLE_ROWS = r"coop: title: the title's own rows Host, Join, Co-op Settings \(buttons 4 to 6\)$"
KEPT_FILE = "coop-difficulty.ini"
PRESS_FRAMES = 3          # PRESS_TICKS 6 at 60 ticks/s, the game at 30 frames/s


def tap(pair, games, key, wait_ticks):
    """One old script line pair `press KEY` + `wait N` on each game in `games`."""
    for x in games:
        pair.press(x, [key], frames=PRESS_FRAMES)
    pair.step_both((6 + wait_ticks + 1) // 2)


def page(pair, x):
    return (pair.state(x).get("coop") or {}).get("page")


def rows(pair, x):
    return [r.get("name") or "" for r in ((pair.state(x).get("coop") or {}).get("rows") or [])]


def count(pair, x, pattern, pos):
    rx = re.compile(pattern)
    return sum(1 for ln in pair.log[x].since(pos) if rx.search(ln))


def connected(pair, x):
    return [p for p in ((pair.state(x).get("coop") or {}).get("players") or []) if p.get("state") == "connected"]


def need_start(pair):
    """Each test's start (inside `with pair.setup():`): both games in play at the boot's place, stepped to a fresh
    sample (pair.need_boot_place, harness-c133-scene; never one read right after the boot), and both in the lane's
    session with two connected players (the state's coop.players, as test_smoke.test_connect reads). Run after
    test_host_page_difficulty_kept (A left the session) or on a --repeat r2, this is SETUP-FAIL naming the counts."""
    for x in ("A", "B"):     # (before any input; lane_check checks it at the boot too: pair.kept_difficulty)
        kept_difficulty(pair.games[x]["folder"])
    pair.need_boot_place()
    n = {x: len(connected(pair, x)) for x in ("A", "B")}
    if any(v < 2 for v in n.values()):
        raise SetupFail("not both in the session: connected players A=%d B=%d (need 2 each; test_difficulty.py runs "
                        "alone with --repeat 1, the Host page test last)" % (n["A"], n["B"]))


def never_since_start(pair, x, pattern):
    """The old rows' `lines ... max=0`: none in the whole session (pos 0; pair.never_log reads only this test's)."""
    line = pair.log[x].find(pattern, pos=0)
    assert line is None, "%s log has /%s/ since the start: %s" % (x, pattern, line)


@pytest.mark.needs_boot(BOOT)
@pytest.mark.budget(420)
@pytest.mark.look
@pytest.mark.row("#114 a lobby made with a difficulty has it from the first player (hs-difficulty-lobby)")
def test_lobby_difficulty_from_first_player(pair):
    with pair.setup():
        need_start(pair)
    # (hard-A / hard-B: min=1 since the start; no-auto-A: max=0. Both games here: B joined the same lobby.)
    for x in ("A", "B"):
        assert pair.log[x].find(HARD, pos=0), "%s: no '%s' line since the start (the lobby's Hard)" % (x, HARD)
        never_since_start(pair, x, AUTO)
    both = ("A", "B")
    pair.step_both(60)                                        # wait 120
    for x in both:
        pair.act(x, "chat /difficulty")
    pair.step_both(45)                                        # wait 90
    for x in both:
        pair.check_shot(x, "dl-chat", tolerance=0.80)         # (the chat over the moving world)
    tap(pair, both, "PLUS", 240)
    for _ in range(2):
        tap(pair, both, "R", 90)
    tap(pair, both, "R", 120)
    for _ in range(4):
        tap(pair, both, "DOWN", 30)
    tap(pair, both, "A", 120)                                 # System's Multiplayer button: our page
    pair.until("both games on co-op's Multiplayer page", lambda: all(page(pair, x) == "main" for x in both),
               max_frames=120)
    for _ in range(5):
        tap(pair, both, "UP", 30)
    for _ in range(4):
        tap(pair, both, "DOWN", 30)
    tap(pair, both, "A", 90)                                  # Co-op settings
    pair.until("both games on Co-op settings", lambda: all(page(pair, x) == "settings" for x in both),
               max_frames=120)
    for x in both:
        assert "Difficulty" in rows(pair, x), "%s: Co-op settings has no Difficulty row (%s)" % (x, rows(pair, x))
        pair.check_shot(x, "dl-settings", tolerance=0.88)    # (still UI: the page, its Difficulty row on Hard)
    for n in (2, 3):
        for _ in range(3):
            tap(pair, both, "DOWN", 30)
        tap(pair, both, "DOWN", 60)
        for x in both:
            pair.check_shot(x, "dl-settings-%d" % n, tolerance=0.88)
    for _ in range(2):
        tap(pair, both, "B", 60)
    tap(pair, both, "B", 120)
    tap(pair, both, "PLUS", 150)
    for x in both:
        never_since_start(pair, x, AUTO)


@pytest.mark.needs_boot(BOOT)
@pytest.mark.budget(360)
@pytest.mark.look
@pytest.mark.row("#114 the title's Host page: Difficulty Automatic -> Hard -> Custom kept for the next lobby "
                 "(hs-host-difficulty)")
def test_host_page_difficulty_kept(pair):
    """hs-host-difficulty started A co-op idle (`start A idle`); here A leaves the lane's session first (menu leave)
    and goes to the title (to-title.now), so the Host page is the one a player sees before hosting. Last test of
    the boot: A stays idle on the title."""
    path = os.path.join(pair.games["A"]["folder"], KEPT_FILE)
    before = open(path, "rb").read() if os.path.isfile(path) else None
    try:
        with pair.setup():
            need_start(pair)
            assert pair.log["A"].find(TITLE_ROWS, pos=0), "A: no title rows line since the start (%s)" % TITLE_ROWS
            pair.act("A", "menu leave")
            pair.until("A left the session (no connected players listed)",
                       lambda: not connected(pair, "A"), max_frames=300)
        with open(os.path.join(pair.games["A"]["folder"], "to-title.now"), "w") as f:
            f.write("bt\n")
        pair.until("A on the title", lambda: not pair.in_play(pair.state("A")), max_frames=900)
        pair.step_both(300)                                   # (the title's own start, as test_smoke's way back)
        mark = pair.log["A"].mark()
        for _ in range(2):                                    # hs-title-menu.down.txt: three downs reach Host
            tap(pair, "A", "DOWN", 30)
        tap(pair, "A", "DOWN", 60)
        tap(pair, "A", "A", 150)                              # hs-title-menu.press.txt
        pair.until("A on the Host page", lambda: page(pair, "A") == "host", max_frames=120)
        assert "Difficulty" in rows(pair, "A"), "A: the Host page has no Difficulty row (%s)" % rows(pair, "A")
        pair.check_shot("A", "hd-page", tolerance=0.88)
        for _ in range(3):                                    # hs-host-difficulty.hard.txt
            tap(pair, "A", "UP", 30)
        for _ in range(2):
            tap(pair, "A", "DOWN", 30)
        for _ in range(2):
            tap(pair, "A", "RIGHT", 30)
        tap(pair, "A", "RIGHT", 90)
        pair.until("three 'kept for the next lobby' lines (Automatic -> Easy -> Normal -> Hard)",
                   lambda: count(pair, "A", KEPT, mark) >= 3, max_frames=120)
        pair.check_shot("A", "hd-hard", tolerance=0.88)
        tap(pair, "A", "RIGHT", 120)                          # hs-host-difficulty.custom.txt: Hard -> Custom
        pair.until("a fourth 'kept' line (Custom)", lambda: count(pair, "A", KEPT, mark) >= 4, max_frames=120)
        assert "Enemy life" in rows(pair, "A"), "A: Custom shows no sliders (%s)" % rows(pair, "A")
        pair.check_shot("A", "hd-custom", tolerance=0.88)    # (the three sliders and Start hosting on screen)
        assert count(pair, "A", REFUSED, mark) == 0, "A: a difficulty change was refused while idle"
        assert os.path.isfile(path), "A kept the choice but %s is not there (the restore below would miss it)" % path
        pair.act("A", "menu close")                           # hs-title-menu.close.txt
        pair.step_both(60)
        pair.until("A's menu closed", lambda: page(pair, "A") is None, max_frames=120)
        pair.check_shot("A", "hd-closed", tolerance=0.85)
    finally:
        if before is None:
            if os.path.isfile(path):
                os.remove(path)
        else:
            with open(path, "wb") as f:
                f.write(before)
