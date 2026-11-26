"""Row 223 (the - menu's Multiplayer page shows the key items) as a bridge test: the port of the scenario key-items-223
(tools/autotest/scenarios/key-items-223.A/B.txt; the solo variant key-items-223-solo.A.txt is not a pair boot and is
not ported). The details list on the right of the Multiplayer page has two pages, turned with ZL / ZR: page 1 the
session's lines, page 2 "Key items" (Songs, Medallions, Spiritual Stones, Dungeon items, each "n of N";
hookshot/src/coop_pause_tab.cpp DetailLines). A guest's counts come from its own flag table (the counts are never
sent): the flags are shared flags, so after A sets them B must read the same full list.

| Test | Old row | Boot | Kind |
|---|---|---|---|
| test_key_items_pages | key-items-223 (row 223) | live-key-items-223 (its own) | A: page 1, page 2 as the save has it, every counted flag set -> page 2 full, ZL -> page 1; B (guest): page 2 full from its own flags; shots on each game |

How it is judged (game state, then shots):
 1. The bridge's `menu.minus` (hookshot/src/coop_menu_state.h Minus, written by CoopPauseTab::MinusWords as the page is
    drawn; docs/playtest.md "menu"): `open`, `tab` "multiplayer", `view` "main", `details_page` 0 / 1, `details_pages` 2
    (the game's key items readable: CoopHookshot::KeyItemTallyNow), and on page 1 `key_items` = the four lines.
 2. The lines' values are judged against the game's own flags read through the bridge (`flag_read`), one by one: the
    counted flags are the boolean entries of coop/host/src/game_items.cpp (item_flags, the ONE table the module counts
    from) that key_item_tally.cpp tally_group_of puts in a group (its rules :16-29, mirrored in GROUP_OF; the game's
    own totals "of N" are checked against this list, so a rule that differs FAILs). Labels: key_item_tally.cpp
    tally_group_label :54-57 (read from that file, English: the lane's language).
 3. Shots (user-facing: the page a player reads) on each game at page1, page2-start, page2-full, page1-again (A) and
    page2-full (B): the - menu's pages are the game's own Layout panes, so they are in the framebuffer (bridge shot).
    NO-REF until the owner approves refs.
 Set-up: the module answers `menu.minus` (older module: SETUP-FAIL), both games in play, the page shut, every
 counted flag readable on both games. The flags are shared (game_items.hpp Reduce::any: "true for everyone once true
 for anyone"), so they cannot be put back: the test has its own boot (needs_boot, steps byte-same as the default
 boot) and nothing runs after it on that boot.
Every press and wait is the old scenario's (cited); MENU_TIMEOUT only bounds a wait (a short one = FAIL, never PASS).
"""
import os
import re

import pytest

BOOT = "live-key-items-223"
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
ITEMS_CPP = os.path.join(REPO, "coop", "host", "src", "game_items.cpp")
TALLY_CPP = os.path.join(REPO, "coop", "host", "src", "key_item_tally.cpp")

# key-items-223.A.txt: 'press MINUS; wait 150; mark minus-open; press R; wait 120; mark page1; press ZR; wait 90;
# mark page2-start; setflag ... (every counted flag); wait 90; press ZL; wait 60; press ZR; wait 120; mark page2-full;
# press ZL; wait 90; mark page1-again'
OPEN_WAIT, TAB_WAIT, PAGE_WAIT = 150, 120, 90
SET_WAIT, BACK_WAIT, FULL_WAIT, AGAIN_WAIT = 90, 60, 120, 90
PRESS = 8                      # (as test_map_arrow: a press held 8 frames)
SHARE_FRAMES = 1500            # key-items-223.B.txt 'wait 1500' before B's MINUS (it waits out A's script)
GROUPS = ("songs", "medallions", "stones", "dungeon_items")   # DetailLines' order (coop_pause_tab.cpp)
COUNTED = 64   # key-items-223.A.txt's setflag lines (every counted flag: 13 + 6 + 3 + 42, B's "13 of 13 ... 42 of 42")
TOL = 0.90


def GROUP_OF(name):
    """key_item_tally.cpp tally_group_of (:16-29): the group a boolean item flag counts in, or None."""
    if name.startswith("IsGet.Item_Melody_"):
        return "songs"
    if name.startswith("IsGet.Item_Medal_"):
        return "medallions"
    if name.startswith("IsGet.Item_Stone_"):
        return "stones"
    if name.startswith("DungeonInfo.") and name.endswith((".IsGetMap", ".IsGetCompass", ".IsGetBossKey")):
        return "dungeon_items"
    return None


def c_source(path):
    """A C/C++ file's text with adjacent string literals joined ("a" "b" -> "ab"), as the compiler sees them (C #124:
    a source check never reads the raw text where a literal can be split)."""
    with open(path, encoding="utf-8") as f:
        return re.sub(r'"\s+"', "", f.read())


def tally_flags():
    """{group: [flag names]} from game_items.cpp's boolean entries (the table the module counts from)."""
    names = re.findall(r'\{"([^"]+)",\s*0x[0-9a-fA-F]+,\s*K::boolean\b', c_source(ITEMS_CPP))
    out = {g: [] for g in GROUPS}
    for n in names:
        g = GROUP_OF(n)
        if g and n not in out[g]:
            out[g].append(n)
    return out


def labels():
    """{group: label} from key_item_tally.cpp tally_group_label."""
    found = dict(re.findall(r'case TallyGroup::(\w+): return "([^"]+)";', c_source(TALLY_CPP)))
    assert set(found) >= set(GROUPS), "key_item_tally.cpp tally_group_label: groups %s, want %s" % (sorted(found), GROUPS)
    return {g: found[g] for g in GROUPS}


def minus(pair, x):
    s = pair.state(x)
    m = (s.get("menu") or {}).get("minus")
    assert isinstance(m, dict), "%s: the state's menu has no 'minus' member (a module older than row 223's state): %s" % (
        x, s.get("menu_error") or s.get("menu"))
    return m


def owned(pair, x, flags):
    """{group: count of the flags the game holds}, each read through the bridge."""
    out = {}
    for g in GROUPS:
        n = 0
        for name in flags[g]:
            r = pair.flag_read(x, name)
            assert "value" in r, "%s: flag_read %s gave no value: %s" % (x, name, r)
            n += 1 if r.get("value") else 0
        out[g] = n
    return out


def holds_all(pair, x, flags):
    """True once game x holds every counted flag (stops at the first one it lacks: one read a poll while none came)."""
    return all(pair.flag_read(x, name).get("value") for g in GROUPS for name in flags[g])


def want_lines(flags, have, names):
    return [{"name": names[g], "value": "%d of %d" % (have[g], len(flags[g]))} for g in GROUPS]


def check_page(pair, x, page, what):
    m = minus(pair, x)
    assert m.get("open") and m.get("tab") == "multiplayer" and m.get("view") == "main", (
        "%s %s: the - menu's Multiplayer page (main view) not shown: %s" % (x, what, m))
    assert m.get("details_pages") == 2, "%s %s: details list has %s pages, want 2 (key items unreadable?): %s" % (
        x, what, m.get("details_pages"), m)
    assert m.get("details_page") == page, "%s %s: details page %s, want %d: %s" % (x, what, m.get("details_page"), page, m)
    if page == 0:
        assert m.get("key_items") is None, "%s %s: key items given on the session's page: %s" % (x, what, m)
    return m


def check_counts(pair, x, flags, names, what):
    m = check_page(pair, x, 1, what)
    have = owned(pair, x, flags)
    want = want_lines(flags, have, names)
    got = m.get("key_items")
    assert got == want, "%s %s: key items %s, want %s (from %s's own flags)" % (x, what, got, want, x)
    pair.notes.append("%s %s: %s" % (x, what, ", ".join("%s %s" % (l["name"], l["value"]) for l in got)))
    return have


def open_page(pair, x):
    """MINUS (the - menu), then R onto co-op's first tab (Multiplayer): key-items-223.A.txt."""
    pair.press(x, buttons=["MINUS"], frames=PRESS)
    pair.step_both(OPEN_WAIT)
    pair.press(x, buttons=["R"], frames=PRESS)
    pair.step_both(TAB_WAIT)


def turn(pair, x, button, wait):
    pair.press(x, buttons=[button], frames=PRESS)
    pair.step_both(wait)


@pytest.mark.budget(300)   # ESTIMATED: ~2'600 game frames (the scenario's waits + B's share wait) + ~280 flag calls + 6 shots
@pytest.mark.look
@pytest.mark.needs_boot(BOOT)
@pytest.mark.row("#223 the - menu's Multiplayer page lists the key items (Songs, Medallions, Spiritual Stones, Dungeon "
                 "items as n of N) on its details list's page 2, from each game's own flags")
def test_key_items_pages(pair):
    flags, names = tally_flags(), labels()
    with pair.setup():
        for g in GROUPS:
            assert flags[g], "game_items.cpp: no boolean flag counts as %s (tally_group_of's rules)" % g
        total = sum(len(flags[g]) for g in GROUPS)
        assert total == COUNTED, ("game_items.cpp: %d counted flags %s, the scenario sets %d: the table or this "
                                  "test's reader changed" % (total, {g: len(flags[g]) for g in GROUPS}, COUNTED))
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
            m = minus(pair, x)
            assert not m.get("open"), "%s: the - menu's co-op page already shows at the start: %s" % (x, m)
        start = {x: owned(pair, x, flags) for x in ("A", "B")}
    pair.notes.append("counted flags: %s" % ", ".join("%s %d" % (g, len(flags[g])) for g in GROUPS))

    open_page(pair, "A")
    check_page(pair, "A", 0, "page1")
    pair.check_shot("A", "key-items-223-page1", tolerance=TOL)
    turn(pair, "A", "ZR", PAGE_WAIT)
    check_counts(pair, "A", flags, names, "page2-start")
    pair.check_shot("A", "key-items-223-page2-start", tolerance=TOL)

    for g in GROUPS:
        for name in flags[g]:
            pair.flag("A", name, 1)
    pair.step_both(SET_WAIT)
    turn(pair, "A", "ZL", BACK_WAIT)
    turn(pair, "A", "ZR", FULL_WAIT)
    have = check_counts(pair, "A", flags, names, "page2-full")
    full = {g: len(flags[g]) for g in GROUPS}
    assert have == full, "A page2-full: A holds %s of %s after setting every counted flag" % (have, full)
    pair.check_shot("A", "key-items-223-page2-full", tolerance=TOL)
    turn(pair, "A", "ZL", AGAIN_WAIT)
    check_page(pair, "A", 0, "page1-again")
    pair.check_shot("A", "key-items-223-page1-again", tolerance=TOL)

    # B (the guest): the shared flags reach its own flag table, then its page 2 reads them (never A's counts).
    # (pair.until polls every CHUNK = 10 frames, at most SHARE_FRAMES / 10 = 150 polls; each poll stops at B's first
    # missing flag: ~1 read a poll until they come, 64 on the last)
    pair.until("B holds every counted flag A set (shared flags)", lambda: holds_all(pair, "B", flags),
               max_frames=SHARE_FRAMES)
    open_page(pair, "B")
    check_page(pair, "B", 0, "page1")
    turn(pair, "B", "ZR", PAGE_WAIT)
    check_counts(pair, "B", flags, names, "page2-full")
    pair.check_shot("B", "key-items-223-page2-full", tolerance=TOL)
    pair.notes.append("at the start: A %s, B %s" % (start["A"], start["B"]))
