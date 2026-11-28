"""Stale menu paths / state keys in an in-game test file (B #156, after difficulty-309d7180 22:36: test_difficulty.py
still opened co-op's page by the pre-0.11.9 '+ menu -> System -> Multiplayer button' keys and read state.coop.page;
its fake modelled the same wrong idea, so the selftests passed and the lane run FAILed after a 120-frame wait).

ONE table: each row names a way of reaching or reading co-op's menu that the module no longer has, why, and the source
that proves it. bt_plugin scans every collected test file once: a hit makes each test of that file SETUP-FAIL
'stale path <name> at <file>:<line>: <why>' at its set-up (no game input), and --lane-check-only prints the same as a
SETUP-FAIL line, so a -chk job shows it in < 30 s without a boot. tests/test_ingame_stale_paths.py keeps the repo at 0 hits.

A row: name, regex (searched per source line), unless (a regex that, on the same line, clears the hit or None),
why, source.
"""
import os
import re

STALE = [
    {"name": "state-coop-menu",
     "regex": r'\.get\("coop"\)\s*or\s*\{\}\)\.get\("(page|rows|focused|focused_name|typing)"\)',
     "unless": r'\.get\("menu"\)',
     "why": "co-op's menu state is state.menu.coop / state.menu.minus, not state.coop (read it with "
            "test_menu_pages.coop_tab / page_shown)",
     "source": "hookshot/src/coop_menu_state.cpp:84/102"},
    {"name": "plus-menu-multiplayer",
     "regex": r"System'?s Multiplayer (button|page)|\+ menu\W{1,4}System\W{1,4}Multiplayer",
     "unless": None,
     "why": "since 0.11.9 co-op's pages are the - menu's tabs: the + menu has no Multiplayer page and System no "
            "Multiplayer button (open co-op's pages with test_menu_pages.open_page)",
     "source": "hookshot/src/coop_pause_tab_module.cpp:1621"},
    {"name": "title-host-join-rows",
     "regex": r"Host, Join, Co-op Settings",
     "unless": None,
     "why": "the title's own menu is 'Continue, New Game, Multiplayer, Options'; Host is in Multiplayer's submenu "
            "(the title logs \"Multiplayer's rows, N of M texts\")",
     "source": "hookshot/src/coop_title.cpp:1492/1524/1223"},
]

_COMPILED = [(r, re.compile(r["regex"]), re.compile(r["unless"]) if r["unless"] else None) for r in STALE]


def scan_text(text, path="<text>"):
    """Every hit in `text`: [{'name', 'file', 'line', 'why', 'source', 'text'}]."""
    hits = []
    for n, ln in enumerate(text.splitlines(), 1):
        for row, rx, unless in _COMPILED:
            if rx.search(ln) and not (unless and unless.search(ln)):
                hits.append({"name": row["name"], "file": os.path.basename(path), "line": n, "why": row["why"],
                             "source": row["source"], "text": ln.strip()})
    return hits


def scan_file(path):
    with open(path, encoding="utf-8") as f:
        return scan_text(f.read(), path)


def line_of(hit):
    return "stale path %s at %s:%d: %s (%s)" % (hit["name"], hit["file"], hit["line"], hit["why"], hit["source"])
