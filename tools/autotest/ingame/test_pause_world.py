"""Row 299 phase 0 probe (docs/pause-world.md): what the game's pause menu stops, and whether the world runs again when
the menu's PauseMgr hold is let go while the menu stays open. Reads only, except one step: the game's own release of
the menu's new hold (SeqOneShotReleasePause's steps, hookshot/src/coop_pause_world_module.cpp Release, test step
`menu hold release N`).

What is read (each from the bridge's `state`, docs/playtest.md "State"):
- `pause_world` (hookshot/src/coop_pause_world.h): the PauseMgr's holds with a count above 0, and its four words +0x30;
- Link's snapshot: Longshot takes it only in a running Link update (docs/playtest.md: "on a held game ... a pause ...
  no Link update runs"), so `snapshot_stale` true while the menu is up says Link's update stands; false says it runs;
- `world`: the object walk's actors (it runs on presents, and the menu still presents): how many moved between two
  reads 120 frames apart (an NPC walking: the world runs).
Then, with the hold let go and the menu still open: the left stick held 30 frames (does Link walk under the menu?).

The probe asserts only what it needs to be read at all (a manager, the + menu taking a hold); every reading goes to
the result's notes as `probe: ...` lines for docs/pause-world.md. Game A only (the host); B just runs.
"""
import json
import math

import pytest

PRESS = 8            # a press held 8 frames (as test_menu_pages)
OPEN_WAIT = 60       # the + menu's opening (the strip's slide)
WATCH = 120          # frames between the two reads that judge "the world moved"
STICK = 30           # frames of left stick under the open menu
MOVED_M = 0.05       # an actor moved: more than 5 cm between two reads (the walk's places are metres)


def pause_world(pair, x):
    st = pair.state(x)
    pw = st.get("pause_world")
    assert isinstance(pw, dict), "%s: no pause_world in state (module without the probe?): %r" % (x, pw)
    return st, pw


def held_keys(pw):
    return {h["index"]: (h["key"], h["count"]) for h in (pw.get("held") or [])}


def places(st):
    w = st.get("world") or {}
    return {a["id"]: a.get("pos") for a in (w.get("actors") or []) if a.get("pos")}, w.get("frame")


def moved(before, after):
    n = 0
    for k, p in before.items():
        q = after.get(k)
        if q and math.dist(p, q) > MOVED_M:
            n += 1
    return n, len(set(before) & set(after))


def reading(pair, x, label):
    """One probe reading: Link's snapshot, the holds, then WATCH frames, then the actors that moved."""
    st, pw = pause_world(pair, x)
    a, f0 = places(st)
    pair.step_both(WATCH)
    st2, pw2 = pause_world(pair, x)
    b, f1 = places(st2)
    n, common = moved(a, b)
    line = {"label": label, "snapshot_stale": st2.get("snapshot_stale"), "pos": st2.get("pos"),
            "holds": held_keys(pw2), "words": pw2.get("words"), "actors_moved": n, "actors_common": common,
            "walk_frames": [f0, f1]}
    pair.notes.append("probe: " + json.dumps(line, sort_keys=True))
    return st2, pw2, line


@pytest.mark.budget(240)
@pytest.mark.row("299 phase 0 probe: the + menu's PauseMgr hold, and whether the world runs with it let go "
                 "(docs/pause-world.md)")
def test_pause_world_probe(pair):
    with pair.setup():
        for x in ("A", "B"):
            assert pair.in_play(pair.state(x)), "%s not in play" % x
        _, pw = pause_world(pair, "A")
        assert pw.get("ok"), "A: no PauseMgr read: %r" % pw
    _, before, base = reading(pair, "A", "free play")
    pair.press("A", ["PLUS"], frames=PRESS)
    pair.step_both(OPEN_WAIT)
    st, menu, open_line = reading(pair, "A", "+ menu open")
    new = {i: kc for i, kc in held_keys(menu).items()
           if i not in held_keys(before) or kc[1] > held_keys(before)[i][1]}
    pair.notes.append("probe: holds the + menu took: %s" % json.dumps(new, sort_keys=True))
    try:
        pair.call("A", "shot", name="pause-world-menu-open")
    except Exception as e:                    # noqa: BLE001 (evidence only)
        pair.notes.append("probe: shot not taken: %s" % e)
    assert new, "A: the + menu took no PauseMgr hold (menu not open, or the pause is not the PauseMgr's): %r" % menu
    for i in sorted(new):
        pair.act("A", "menu hold release %d" % i)
        pair.expect_log("A", r"coop: pause world: test release %d key" % i, max_frames=60)
    _, _, released = reading(pair, "A", "+ menu open, its holds let go")
    before_stick = pair.state("A").get("pos")
    pair.press("A", [], stick=[0, 1.0], frames=STICK)
    after_stick = pair.state("A").get("pos")
    pair.notes.append("probe: " + json.dumps({"label": "stick under the open menu", "before": before_stick,
                                              "after": after_stick}, sort_keys=True))
    try:
        pair.call("A", "shot", name="pause-world-released")
    except Exception as e:                    # noqa: BLE001
        pair.notes.append("probe: shot not taken: %s" % e)
    # The way back: + again shuts the menu as a player does; its own Release then finds the count at 0 (never below).
    pair.press("A", ["PLUS"], frames=PRESS)
    pair.step_both(OPEN_WAIT)
    _, after, back = reading(pair, "A", "+ menu shut")
    pair.notes.append("probe: holds after the menu shut equal free play's: %s" % (held_keys(after) == held_keys(before)))
