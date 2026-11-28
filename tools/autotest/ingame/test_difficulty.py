"""Difficulty #114 (C #114; old rows difficulty-lobby-run.ps1 = hs-difficulty-lobby, host-difficulty-run.ps1 =
hs-host-difficulty), on the bridge. Both tests boot `--boot live-lobby-hard` (scenarios/live-lobby-hard.A/B.txt:
live-kakariko-child with '#! server --difficulty hard', hookshot.py header_server_args): the lobby is made with Hard
before the first player joins, as hs-difficulty-lobby.steps.txt's '#! server --difficulty hard' did.

Run (one job, this file alone, --repeat 1: the second test leaves A's session, so nothing may run after it on this
boot; each set-up asserts both games list two connected players, so a wrong order or a repeat is SETUP-FAIL, not a
FAIL half-way: review-rows-c136 15:09):
    pytest test_difficulty.py --boot live-lobby-hard --lane 1 --repeat 1 --expect-commit <sha>

port-g6-rest-5 (difficulty-309d7180 FAIL 22:36, B #156): the first port opened co-op's page by the pre-0.11.9 + menu
keys (PLUS -> System -> a Multiplayer button that is gone: coop_pause_tab_module.cpp:1621) and read state.coop.page;
co-op's menu state is state.menu.coop / state.menu.minus (coop_menu_state.cpp:84/102). Now every page is reached and
read by state: in play by test_menu_pages.open_page (the - menu's Multiplayer tab, F1's rule), on the title by the
title's own menu (its logged focus), and a row by menu.coop.focused_name. stale_paths.py SETUP-FAILs the old ways.

Sources of every value (worker-rules "no guessed values"):
- 'coop: difficulty: Hard' / '... Automatic': hs-difficulty-lobby.steps.txt lines hard-A/hard-B/no-auto-A; the module's
  line is lichii/hook/coop_difficulty_game.cpp:205 'coop: difficulty: {Describe} (set by N)', Describe =
  coop_difficulty.cpp:109 (a preset's name first, 'Automatic (N players)' when automatic).
- 'kept for the next lobby' / 'refused': hs-host-difficulty.steps.txt lines kept-A (min=3), kept-custom-A (min=4),
  refused-A (max=0); the module's line hookshot/src/coop_pause.cpp:1230 (Request::Kind::kDifficulty).
- the Difficulty row: coop_pause_menu.cpp DifficultyLines Choice("Difficulty", ...) on the Settings page (:808) and the
  Host page; the Custom sliders' first row 'Enemy life' (the same lines).
- in play: Co-op settings = test_menu_pages.open_page(x, "settings") -> ("minus", "settings"); A gives its list the pad
  (RowAction::kMenuTab); the moves and the presses: test_menu_typing (PRESS, WAIT, MOST_STEPS, SETTINGS_ROW, tap).
- the title (the lanes' pack): 'coop: title: the title's own menu: Continue, New Game, Multiplayer, Options'
  (coop_title.cpp:1492; hookshot_log-A.txt:705 of difficulty-309d7180); its rows CoopTitleMenu::MenuFor
  (coop_title_menu.cpp:1120): with a save Continue 0, New Game 1, Options 2, Multiplayer 3; Multiplayer's rows Join 0,
  Host 1, Co-op Settings 2, Back 3, the cursor on Join. The focus: 'coop: title: the title's focus is on button N'
  (coop_title.cpp:1517, logged on each change); A on a row: 'coop: title: menu: row N pressed' (:1972); Host opens co-op's
  Host page on the title's own buttons (CoopPause::OpenNative, coop_pause.cpp:2125: 'its page on the title's own rows')
  and the state's menu.coop then has page "host" (CoopPause::MenuTab: g_native, coop_pause.cpp:2617); B there leaves it
  ('coop: title: menu: the page left: Multiplayer's rows again', coop_title.cpp:1771) and CloseNative shuts menu.coop.
- `menu leave` (the Multiplayer page's Leave row): coop_pause.cpp:2696, bridge `act`.
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
from test_menu_pages import open_page, close_menu, page_shown
from test_menu_typing import MOST_STEPS, SETTINGS_ROW, minus, coop, tap

BOOT = "live-lobby-hard"
HARD = r"coop: difficulty: Hard"
AUTO = r"coop: difficulty: Automatic"
KEPT = r"coop: multiplayer tab: difficulty kept for the next lobby"
REFUSED = r"coop: multiplayer tab: difficulty refused"
TITLE_MENU = r"coop: title: the title's own menu: Continue, New Game, Multiplayer, Options"
TITLE_FOCUS = re.compile(r"coop: title: the title's focus is on button (\d+)")
ROW_PRESSED = r"coop: title: menu: row %d pressed"
PAGE_ON_TITLE = r"coop: multiplayer tab: its page on the title's own rows"
PAGE_LEFT = r"coop: title: menu: the page left: Multiplayer's rows again"
TITLE_MULTIPLAYER, SUB_HOST = 3, 1      # (CoopTitleMenu::MenuFor: the rows with a save; Multiplayer's rows)
DIFFICULTY = "Difficulty"
KEPT_FILE = "coop-difficulty.ini"


def count(pair, x, pattern, pos):
    rx = re.compile(pattern)
    return sum(1 for ln in pair.log[x].since(pos) if rx.search(ln))


def connected(pair, x):
    return [p for p in ((pair.state(x).get("coop") or {}).get("players") or []) if p.get("state") == "connected"]


def row_names(pair, x):
    return [r.get("name") or "" for r in (coop(pair.state(x)).get("rows") or [])]


def to_row(pair, x, name):
    """Game x's focus on co-op's page's row `name` by the state's menu.coop.focused_name: Down first, then Up
    (test_menu_typing.to_room_name's way). AssertionError naming the rows seen when it is not reached."""
    seen = []
    for button in ("DOWN", "UP"):
        for _ in range(MOST_STEPS):
            now = coop(pair.state(x)).get("focused_name")
            if now == name:
                return
            seen.append(now)
            tap(pair, x, button)
    raise AssertionError("%s: %r never focused on co-op's %s page (focused: %s)" % (x, name,
                                                                                coop(pair.state(x)).get("page"), seen))


def title_focus(pair, x):
    """The title's focused button now: the last 'focus is on button N' line of the whole session (logged on each
    change only, so the last one holds), or None."""
    last = None
    for ln in pair.log[x].since(0):
        m = TITLE_FOCUS.search(ln)
        if m:
            last = int(m.group(1))
    return last


def title_to(pair, x, button):
    """Game x's title focus on `button` by Down presses (at most 4, the title's rows), judged by its focus line."""
    for _ in range(5):
        if title_focus(pair, x) == button:
            return
        tap(pair, x, "DOWN")
    raise AssertionError("%s: the title's focus never came to button %d (now %s)" % (x, button, title_focus(pair, x)))


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
    pair.step_both(60)
    for x in both:
        pair.act(x, "chat /difficulty")
    pair.step_both(45)
    for x in both:
        pair.check_shot(x, "dl-chat", tolerance=0.80)         # (the chat over the moving world)
    for x in both:
        home, page = open_page(pair, x, "settings")
        assert (home, page) == ("minus", "settings"), "%s: Co-op settings in free play showed in %s %s" % (x, home, page)
        m = minus(pair.state(x))
        assert m.get("row") == SETTINGS_ROW, "%s: the rows' cursor is not on Co-op settings: menu.minus %s" % (x, m)
        tap(pair, x, "A")     # (RowAction::kMenuTab: its rows on the list, which has the pad)
        pair.until("%s: the list has the pad on Co-op settings (state menu.minus.list)" % x,
                   lambda: minus(pair.state(x)).get("list") and page_shown(pair.state(x)) == ("minus", "settings")
                   or None, max_frames=60)
        assert DIFFICULTY in row_names(pair, x), "%s: Co-op settings has no Difficulty row (%s)" % (x, row_names(pair, x))
        to_row(pair, x, DIFFICULTY)
        pair.check_shot(x, "dl-settings", tolerance=0.88)    # (still UI: the page, its Difficulty row on Hard)
        close_menu(pair, x)
    for x in both:
        never_since_start(pair, x, AUTO)


@pytest.mark.needs_boot(BOOT)
@pytest.mark.budget(360)
@pytest.mark.look
@pytest.mark.row("#114 the title's Host page: Difficulty Automatic -> Hard -> Custom kept for the next lobby "
                 "(hs-host-difficulty)")
def test_host_page_difficulty_kept(pair):
    """hs-host-difficulty started A co-op idle (`start A idle`); here A leaves the lane's session first (menu leave)
    and goes to the title (to-title.now), so the Host page is the one a player sees before hosting: the title's own
    menu, Multiplayer, Host. Last test of the boot: A stays idle on the title."""
    path = os.path.join(pair.games["A"]["folder"], KEPT_FILE)
    before = open(path, "rb").read() if os.path.isfile(path) else None
    try:
        with pair.setup():
            need_start(pair)
            pair.act("A", "menu leave")
            pair.until("A left the session (no connected players listed)",
                       lambda: not connected(pair, "A"), max_frames=300)
        mark = pair.log["A"].mark()
        with open(os.path.join(pair.games["A"]["folder"], "to-title.now"), "w") as f:
            f.write("bt\n")
        pair.until("A on the title", lambda: not pair.in_play(pair.state("A")), max_frames=900)
        pair.until("A: the title's own menu (%s)" % TITLE_MENU, lambda: pair.log["A"].find(TITLE_MENU, pos=mark),
                   max_frames=900)
        pair.step_both(60)                                    # (the title's step runs: its focus line)
        title_to(pair, "A", TITLE_MULTIPLAYER)
        rows_at = pair.log["A"].mark()
        tap(pair, "A", "A")
        pair.until("A: Multiplayer pressed", lambda: pair.log["A"].find(ROW_PRESSED % TITLE_MULTIPLAYER, pos=rows_at),
                   max_frames=60)
        pair.step_both(20)
        title_to(pair, "A", SUB_HOST)
        host_at = pair.log["A"].mark()
        tap(pair, "A", "A")
        pair.until("A: Host pressed", lambda: pair.log["A"].find(ROW_PRESSED % SUB_HOST, pos=host_at), max_frames=60)
        pair.until("A: co-op's Host page on the title's own buttons (log)",
                   lambda: pair.log["A"].find(PAGE_ON_TITLE, pos=host_at), max_frames=60)
        pair.until("A: the Host page in the state (menu.coop.page host)",
                   lambda: coop(pair.state("A")).get("open") and coop(pair.state("A")).get("page") == "host" or None,
                   max_frames=60)
        assert DIFFICULTY in row_names(pair, "A"), "A: the Host page has no Difficulty row (%s)" % row_names(pair, "A")
        pair.check_shot("A", "hd-page", tolerance=0.88)
        to_row(pair, "A", DIFFICULTY)
        kept_at = pair.log["A"].mark()
        for n, name in ((1, "Easy"), (2, "Normal"), (3, "Hard"), (4, "Custom")):
            tap(pair, "A", "RIGHT")
            pair.until("'kept for the next lobby' line %d (%s)" % (n, name),
                       lambda: count(pair, "A", KEPT, kept_at) >= n, max_frames=60)
            got = count(pair, "A", KEPT, kept_at)
            assert got == n, "A: one RIGHT made %d difficulty steps (kept lines %d, want %d)" % (got - n + 1, got, n)
            if name == "Hard":
                pair.check_shot("A", "hd-hard", tolerance=0.88)
        pair.until("A: Custom shows its sliders (row 'Enemy life')", lambda: "Enemy life" in row_names(pair, "A") or None,
                   max_frames=60)
        pair.check_shot("A", "hd-custom", tolerance=0.88)    # (the three sliders and Start hosting on screen)
        assert count(pair, "A", REFUSED, kept_at) == 0, "A: a difficulty change was refused while idle"
        assert os.path.isfile(path), "A kept the choice but %s is not there (the restore below would miss it)" % path
        left_at = pair.log["A"].mark()
        tap(pair, "A", "B")                                   # (B on the page: Multiplayer's rows again)
        pair.until("A: the page left (log)", lambda: pair.log["A"].find(PAGE_LEFT, pos=left_at), max_frames=60)
        pair.until("A: co-op's page shut (state menu.coop)", lambda: not coop(pair.state("A")).get("open"),
                   max_frames=60)
        pair.check_shot("A", "hd-closed", tolerance=0.85)
    finally:
        if before is None:
            if os.path.isfile(path):
                os.remove(path)
        else:
            with open(path, "wb") as f:
                f.write(before)
