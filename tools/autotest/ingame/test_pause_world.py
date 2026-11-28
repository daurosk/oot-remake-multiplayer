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
    line = {"label": label, "scene": st2.get("scene"), "age": st2.get("age"),   # (actors_moved means nothing without the scene)
            "snapshot_stale": st2.get("snapshot_stale"), "pos": st2.get("pos"),
            "holds": held_keys(pw2), "words": pw2.get("words"), "actors_moved": n, "actors_common": common,
            "walk_frames": [f0, f1]}
    pair.notes.append("probe: " + json.dumps(line, sort_keys=True))
    return st2, pw2, line


@pytest.mark.budget(240)
@pytest.mark.row("299 phase 0 probe: the + menu's PauseMgr hold, and whether the world runs with it let go "
                 "(docs/pause-world.md)")
def test_pause_world_probe(pair):
    with pair.setup():
        pair.need_boot_place()                # both stepped to a fresh in-play sample in the boot's scene (82f9a7a5a)
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
        # Every end of the module's Release (coop_pause_world_module.cpp Release): the done line ('key K: count c -> a,
        # notify ... called|NOT called') or one of its three refusals; never its 'waits for' line (asked, not done yet).
        line = pair.expect_log("A", r"coop: pause world: test release %d(?: key |: no such hold|: count moved|"
                                    r": no notify)" % i, max_frames=60)
        pair.notes.append("probe: release %d: %s" % (i, line))
        assert " key " in line, "A: the menu's hold %d was not let go: %s" % (i, line)
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


# ---- Row 299 phase 1: the host option "World runs in menus" (docs/pause-world.md "Phase 1") ----------------------
# Game A works the menus; B only runs (and reads A's status). The option is turned on with the module's own test step
# `act menu world on|off` (hookshot/src/coop_pause.cpp TestMenu -> CoopPauseWorldMode::TestSet, which puts it in force
# on that game at once: lichii/hook/coop_pause_world_mode.cpp). Its way to the guests (the host's MOD_SETTINGS) is the
# unit tests' (tests/lichii/pause_world_mode_test.cpp), not this test's: the lane's games join a server, neither hosts.
#
# Sources (worker-rules "no guessed values"):
# - the room: live-stalfos-weak-room (scenarios/live-stalfos-weak-room.A/B.txt): two Enemy_Stalfos_Weak that walk at
#   Link (test_enemy_turn.py docstring); its helpers in_stage / keep_up / stalfos / own_number are reused from there;
# - log lines: 'coop: test menu: world runs in menus on|off (in force: on|off)' coop_pause.cpp TestMenu; 'coop: pause
#   world: menu hold N key 0x..: count c -> a, notify 0x.. called' coop_pause_world_module.cpp Release (who "menu hold");
# - state pause_world {held, in_force, world_runs, menu_up} coop_pause_world_module.cpp StateJson;
# - B's view of A: coop.players[].status, the PLAYER_STATUS bits B last received from A (hookshot/src/coop_playtest.cpp,
#   coop/protocol/messages.hpp status: paused 1, in_menu 8);
# - the Settings page's row: menu.coop.rows[].name "World runs in menus" (coop_pause_menu.cpp HostRuleLines), opened as
#   a player has it (test_menu_pages.open_page);
# - the - menu's pages: MINUS opens the Map (menu.map_page, test_map_back_295), R onto co-op's Multiplayer tab
#   (menu.minus tab "multiplayer": test_key_items.py, key-items-223.A.txt 'press MINUS; wait 150; ... press R;
#   wait 120'), L back to the Map (test_map_back_295), MINUS shuts it.
from test_enemy_turn import BOOT as STALFOS_BOOT, STAGE as STALFOS_STAGE, SWITCHES as STALFOS_SWITCHES  # noqa: E402
from test_enemy_turn import in_stage, keep_up, own_number, stalfos  # noqa: E402
from test_menu_pages import close_menu, open_page  # noqa: E402

ROW_NAME = "World runs in menus"   # coop_pause_menu.cpp HostRuleLines' Toggle
PAUSED, IN_MENU = 1, 8             # coop/protocol/messages.hpp namespace status
MINUS_WAIT = 150                   # key-items-223.A.txt 'press MINUS; wait 150'
TAB_WAIT = 120                     # key-items-223.A.txt 'press R; wait 120'
MAP_FRAMES = 90                    # test_map_back_295 MAP_FRAMES (L back to the Map)
SHUT_FRAMES = 150                  # test_map_back_295 SHUT_FRAMES (MINUS shuts the - menu)
# ESTIMATED bound (a short one FAILs, never PASSes): PLAYER_STATUS goes at the next sample after a change
# (coop/host/src/sampler.cpp); one second of frames is far over a sample's gap.
STATUS_FRAMES = 60
STILL_M = 0.05                     # Link "stood": under 5 cm over the held stick (as MOVED_M)
QUICK = 20                         # frames R is held for the quick-switch probe: NOTES ONLY, any length (no assert)
STICK_TRIES = 3                    # stick windows at most: one with a Stalfos hit (Life fell) is done again


def world_line(pair, x, label):
    """reading()'s line plus phase 1's words and the Stalfos that moved over its WATCH frames on game x."""
    first = stalfos(pair, x)
    a = {s["id"]: s["pos"] for s in first}
    st2, pw2, line = reading(pair, x, label)
    n, common = moved(a, {s["id"]: s["pos"] for s in stalfos(pair, x)})
    line.update(stalfos_moved=n, stalfos_common=common, stalfos_owners={s["id"]: s.get("owner") for s in first},
                in_force=pw2.get("in_force"), world_runs=pw2.get("world_runs"), menu_up=pw2.get("menu_up"))
    pair.notes.append("phase1: " + json.dumps(line, sort_keys=True))
    return st2, pw2, line


def status_of(pair, seer, number):
    """The PLAYER_STATUS bits game `seer` last received from player `number` (None: no row, or none received)."""
    for p in ((pair.state(seer).get("coop") or {}).get("players") or []):
        if p.get("number") == number and not p.get("self"):
            return p.get("status")
    return None


def world_switch(pair, on):
    word = "on" if on else "off"
    pair.act("A", "menu world %s" % word)
    pair.expect_log("A", r"coop: test menu: world runs in menus %s \(in force: %s\)" % (word, word), max_frames=30)
    _, pw = pause_world(pair, "A")
    assert pw.get("in_force") is on, "A: pause_world.in_force %r after 'menu world %s': %r" % (pw.get("in_force"), word, pw)


def plus_open(pair):
    pair.press("A", ["PLUS"], frames=PRESS)
    pair.step_both(OPEN_WAIT)


def plus_shut(pair, free_held):
    pair.press("A", ["PLUS"], frames=PRESS)
    pair.step_both(OPEN_WAIT)
    _, pw = pause_world(pair, "A")
    assert held_keys(pw) == free_held, "A: holds after + shut %s, free play's %s" % (held_keys(pw), free_held)
    assert not pw.get("world_runs"), "A: world_runs still true with the menu shut: %r" % pw


def map_up(pair, x):
    m = ((pair.state(x).get("menu") or {}).get("map_page")) or {}
    return m if m.get("up") and m.get("panes", 0) > 0 and m.get("shown") == m.get("panes") else None


def minus_tab(pair, x):
    m = ((pair.state(x).get("menu") or {}).get("minus")) or {}
    return m.get("tab") if m.get("open") else None


def life(pair, x):
    return pair.flag_read(x, "PlayerStatus.Life").get("value")


def stood_under_stick(pair):
    """Link's place over STICK frames of held stick under the open menu, on a window with no hit: a Stalfos hit's
    knockback moves Link for a reason that is not the pad (review-rows-c130-12 FIX 1), so a window where Life fell is
    done again after keep_up (STICK_TRIES at most). Returns (before, after); asserts a window without a hit came."""
    for i in range(STICK_TRIES):
        keep_up(pair)
        l0, before = life(pair, "A"), pair.state("A").get("pos")
        pair.press("A", [], stick=[0, 1.0], frames=STICK)
        l1, after = life(pair, "A"), pair.state("A").get("pos")
        hit = isinstance(l0, int) and isinstance(l1, int) and l1 < l0
        pair.notes.append("phase1: stick try %d under the + menu, option on: Life %s -> %s, pos %s -> %s%s" % (
            i + 1, l0, l1, before, after, " (hit: again)" if hit else ""))
        if not hit:
            return before, after
    raise AssertionError("A: a Stalfos hit Link in each of %d stick windows: Link's standing not judged" % STICK_TRIES)


def a_status_on_b(pair, num_a, menu):
    """{"bits": B's status bits of A} once they say `menu` (True: in_menu or paused; False: neither), else None (a
    dict: bits 0 is an answer, never "not yet")."""
    s = status_of(pair, "B", num_a)
    if s is None:
        return None
    return {"bits": s} if bool(s & (PAUSED | IN_MENU)) is menu else None


@pytest.mark.needs_boot(STALFOS_BOOT)
@pytest.mark.needs_env(*STALFOS_SWITCHES)
@pytest.mark.look
@pytest.mark.budget(420)   # ESTIMATED, no lane run yet: ~3'000 frames (menus, waits, 3 x WATCH) + world reads + 5 shots
@pytest.mark.row("299 phase 1: the host option 'World runs in menus': off, the menu holds the world as today; on, the "
                 "menu's holds are let go, enemies move, Link stands under the stick, the menus still page and shut, "
                 "and the others see no in_menu/paused (docs/pause-world.md Phase 1)")
def test_pause_world_mode(pair):
    with pair.setup():
        in_stage(pair, STALFOS_STAGE)
        _, pw = pause_world(pair, "A")
        assert pw.get("ok"), "A: no PauseMgr read: %r" % pw
        assert "in_force" in pw and "menu_up" in pw, "A: pause_world without phase 1's words (older module): %r" % pw
        assert pw.get("in_force") is False, "A: the option is in force before the test turned it on: %r" % pw
        num_a = own_number(pair.state("A"))
        assert isinstance(num_a, int), "A: no own player number in coop.players"
        assert stalfos(pair, "A"), "A: no living Stalfos in the world list (the boot's arrival event)"
    keep_up(pair)
    free = held_keys(pause_world(pair, "A")[1])

    # Probe (notes only): which holds the - menu and the quick-switch menu take, option off.
    pair.press("A", ["MINUS"], frames=PRESS)
    pair.step_both(MINUS_WAIT)
    pair.notes.append("probe: - menu holds (option off): %s" % json.dumps(held_keys(pause_world(pair, "A")[1]),
                                                                          sort_keys=True))
    pair.press("A", ["MINUS"], frames=PRESS)
    pair.step_both(SHUT_FRAMES)
    pair.press("A", ["R"], frames=QUICK)
    pair.notes.append("probe: R held %d frames (quick-switch), holds: %s" % (
        QUICK, json.dumps(held_keys(pause_world(pair, "A")[1]), sort_keys=True)))
    pair.step_both(OPEN_WAIT)

    # Option off: the + menu holds the world as today, and B sees A in a menu.
    plus_open(pair)
    st, pw, _ = world_line(pair, "A", "option off, + menu open")
    menu_keys = {i: kc for i, kc in held_keys(pw).items() if i not in free or kc[1] > free[i][1]}
    assert menu_keys, "A: option off, the + menu took no hold: %r" % pw
    assert pw.get("menu_up") is True, "A: the + menu up but pause_world.menu_up %r" % pw.get("menu_up")
    assert not pw.get("world_runs"), "A: option off, world_runs true: %r" % pw
    assert st.get("snapshot_stale") is True, "A: option off, Link's update ran under the + menu: %r" % st.get("pos")
    s = pair.until("B sees A in a menu (option off: in_menu or paused)", lambda: a_status_on_b(pair, num_a, True),
                   max_frames=STATUS_FRAMES)
    pair.notes.append("phase1: B's status bits of A, option off, + menu: %d" % s["bits"])
    plus_shut(pair, free)

    # Option on: the Settings page shows the row (a shot on each game), then the + menu with the world running.
    world_switch(pair, True)
    for x in ("A", "B"):
        open_page(pair, x, "settings")
        rows = ((pair.state(x).get("menu") or {}).get("coop") or {}).get("rows") or []
        row = [r for r in rows if r.get("name") == ROW_NAME]
        assert row, "%s: no '%s' row on the Settings page: %s" % (x, ROW_NAME, [r.get("name") for r in rows])
        pair.notes.append("phase1: %s Settings row %s" % (x, json.dumps(row[0], sort_keys=True)))
        pair.check_shot(x, "pause-world-settings")
        close_menu(pair, x)
    keep_up(pair)
    plus_open(pair)
    pair.expect_log("A", r"coop: pause world: menu hold \d+ key 0x[0-9a-f]+: count \d+ -> \d+, notify 0x[0-9a-f]+ "
                         r"called", max_frames=OPEN_WAIT)
    st, pw, on = world_line(pair, "A", "option on, + menu open")
    assert pw.get("menu_up") is True, "A: the + menu up but pause_world.menu_up %r" % pw.get("menu_up")
    still = {i: kc for i, kc in held_keys(pw).items() if i in menu_keys}
    assert not still, "A: option on, the + menu's hold still held: %s" % still
    assert pw.get("world_runs") is True, "A: option on, the + menu up, world_runs not true: %r" % pw
    assert st.get("snapshot_stale") is False, "A: option on, Link's update stands under the + menu: %r" % st
    assert on["stalfos_moved"] >= 1, "A: option on, no Stalfos moved over %d frames under the + menu: %s" % (WATCH, on)
    s = pair.until("B sees A neither in a menu nor paused (option on)", lambda: a_status_on_b(pair, num_a, False),
                   max_frames=STATUS_FRAMES)
    pair.notes.append("phase1: B's status bits of A, option on, + menu: %d" % s["bits"])
    before, after = stood_under_stick(pair)
    assert before and after and math.dist(before, after) < STILL_M, (
        "A: Link moved under the + menu with the option on: %s -> %s" % (before, after))
    for x in ("A", "B"):
        pair.check_shot(x, "pause-world-plus-on")
    plus_shut(pair, free)

    # Option on, the - menu: its hold let go, and it still pages (Map, R Multiplayer, L Map) and shuts.
    keep_up(pair)
    pair.press("A", ["MINUS"], frames=PRESS)
    pair.until("A: MINUS opens the Map page in full", lambda: map_up(pair, "A"), max_frames=MINUS_WAIT)
    _, pw, _ = world_line(pair, "A", "option on, - menu (Map) open")
    assert pw.get("world_runs") is True, "A: option on, the - menu up, world_runs not true: %r" % pw
    pair.check_shot("A", "pause-world-minus-on")
    pair.press("A", ["R"], frames=PRESS)
    pair.until("A: R onto co-op's Multiplayer tab (menu.minus)", lambda: minus_tab(pair, "A") == "multiplayer" or None,
               max_frames=TAB_WAIT)
    pair.press("A", ["L"], frames=PRESS)
    pair.until("A: L back to the Map page in full", lambda: map_up(pair, "A"), max_frames=MAP_FRAMES)
    pair.press("A", ["MINUS"], frames=PRESS)
    pair.until("A: MINUS shuts the - menu (pause_world.menu_up false, no Map page)",
               lambda: (not pause_world(pair, "A")[1].get("menu_up") and not map_up(pair, "A")) or None,
               max_frames=SHUT_FRAMES)
    assert not pause_world(pair, "A")[1].get("world_runs"), "A: world_runs still true with the - menu shut"

    # Option off again: the + menu holds the world as today.
    world_switch(pair, False)
    plus_open(pair)
    st, pw, _ = world_line(pair, "A", "option off again, + menu open")
    assert set(menu_keys) <= set(held_keys(pw)), "A: option off again, the + menu's hold not taken: %r" % pw
    assert not pw.get("world_runs") and st.get("snapshot_stale") is True, "A: option off again, the world ran: %r" % pw
    plus_shut(pair, free)
