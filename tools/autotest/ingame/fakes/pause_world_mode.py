"""Fake-game reactions for row 299 phase 1 (ingame/test_pause_world.py test_pause_world_mode; the module's side:
hookshot/src/coop_pause_world_module.cpp, lichii/hook/coop_pause_world_mode.cpp). Hooks: fakegame.py docstring.

Reacts ONLY while BT_FAKE_PAUSE_WORLD_MODE is set (its selftest, tests/test_ingame_pause_world.py), so no other row's
test sees a change (B #149). Then:
- the games start in StalfosWeakStage (the test's boot) with two Enemy_Stalfos_Weak owned by A (player 1) that walk
  while A's world runs; coop.players says `self`, and each other player's row its PLAYER_STATUS bits (`status`);
- act `menu world on|off`: coop_pause.cpp TestMenu's line, pause_world.in_force;
- the + menu (fakegame's menu_pause, PLUS) and the - menu (MINUS: the Map page; R co-op's Multiplayer tab; L the Map;
  MINUS shuts) each take one hold (+: fakes/pause_world.py's index 1, key 0x5a5a; -: index 2, key 0x6b6b); with the
  option in force the module's 'menu hold' release line comes, the hold is let go, `world_runs` is true, Link's
  snapshot is fresh under the menu (pos given, snapshot_stale false), the pad never walks Link, and B reads A's
  status without in_menu/paused (8/1). Option off: the hold stays, snapshot_stale true, B reads in_menu|paused (9).
- the Settings page (fakes/menu_pages.py's menu.coop page "settings") lists the "World runs in menus" row.
BT_FAKE_PAUSE_WORLD_MODE=held: the option in force lets no hold go (the test must FAIL "still held");
=walks: Link walks under the menu with the option on (FAIL "Link moved"); =status: B still reads A in a menu (FAIL
"neither in a menu nor paused"). Any other value: the good game.
"""
import os

STAGE = "StalfosWeakStage"
STALFOS = "Enemy_Stalfos_Weak"
PLUS_HOLD, MINUS_HOLD = (1, 0x5a5a), (2, 0x6b6b)
ROW = {"name": "World runs in menus", "kind": "toggle"}


def _mode():
    return os.environ.get("BT_FAKE_PAUSE_WORLD_MODE", "")


def _on(game):
    return bool(_mode()) and getattr(game, "pwm", None) is not None


def setup(game):
    game.pwm = None
    if not _mode():
        return
    game.pwm = {"force": False, "minus": None, "walked": 0, "seen": game.frame, "drift": 0.0, "stood": None}
    game.scene = STAGE


def _menu_up(game):
    return bool(game.menu_pause or game.pwm["minus"])


def _runs(game):
    """The module's WorldRuns: a menu up, the option in force, its hold let go (never in mode 'held')."""
    return _menu_up(game) and game.pwm["force"] and _mode() != "held"


def act(game, words):
    if not _on(game):
        return False
    w = [x.lower() for x in words]
    if w[:2] != ["menu", "world"] or len(w) != 3 or w[2] not in ("on", "off"):
        return False
    game.pwm["force"] = w[2] == "on"
    game.say("coop: test menu: world runs in menus %s (in force: %s)" % (w[2], w[2]))
    return True


def _say_release(game, hold):
    if game.pwm["force"]:   # (mode 'held' too: the line comes, yet the hold stays, as a menu that takes it again)
        game.later(2, lambda: game.say("coop: pause world: menu hold %d key 0x%x: count 1 -> 0, notify 0x80001000 "
                                       "called" % hold))


def press(game, buttons):
    if not _on(game):
        return False
    p = game.pwm
    if "PLUS" in buttons and not game.menu_pause:   # (fakegame toggles menu_pause after the hooks: this one opens it)
        _say_release(game, PLUS_HOLD)
    if "MINUS" in buttons:
        if p["minus"]:
            p["minus"] = None
        else:
            p["minus"] = "map"
            _say_release(game, MINUS_HOLD)
    elif "R" in buttons and p["minus"] == "map":
        p["minus"] = "multiplayer"
    elif "L" in buttons and p["minus"] == "multiplayer":
        p["minus"] = "map"
    return False


def actors(game):
    if not _on(game):
        return []
    a = game.other if game.tag == "B" else game   # (A owns both: they walk when A's world runs)
    q = a.pwm
    walk = 0.01 * q["walked"]
    return [{"id": 77, "name": STALFOS, "kind": "enemy", "alive": True, "owner": 1, "pos": [2.0 + walk, 105.5, -50.0]},
            {"id": 78, "name": STALFOS, "kind": "enemy", "alive": True, "owner": 1, "pos": [-2.0, 105.5, -50.0 - walk]}]


def _status(game):
    """PLAYER_STATUS bits game `game` sends now: in_menu|paused (9) while a menu holds its world, else 0."""
    if not _menu_up(game):
        return 0
    if _runs(game) and _mode() != "status":
        return 0
    return 9


def state(game, st):
    if not _on(game):
        return
    p = game.pwm
    if not (_menu_up(game) and not _runs(game)):
        p["walked"] += max(0, game.frame - p["seen"])   # (A's world ran since the last look)
    p["seen"] = game.frame
    for row in st["coop"]["players"]:
        me = row["number"] == (1 if game.tag == "A" else 2)
        row["self"] = me
        row["status"] = None if me else _status(game.other)
    pw = st.get("pause_world")
    if isinstance(pw, dict):
        held = []
        if game.menu_pause and not _runs(game):
            held.append({"index": PLUS_HOLD[0], "key": "0x%x" % PLUS_HOLD[1], "count": 1})
        elif game.menu_pause:
            held = []   # (let go: fakes/pause_world.py's hold 1 is the + menu's)
        if p["minus"] and not _runs(game):
            held.append({"index": MINUS_HOLD[0], "key": "0x%x" % MINUS_HOLD[1], "count": 1})
        pw.update(held=held, in_force=p["force"], world_runs=_runs(game), menu_up=_menu_up(game))
    if _menu_up(game):
        if _runs(game):
            if _mode() == "walks":
                p["drift"] += 0.5
            if p.get("stood") is None:   # (the fake's own stick walks Link: the input-off wrap keeps him where he stood)
                p["stood"] = list(game.pos)
            at = p["stood"]
            st.update(pos=[at[0], at[1], at[2] + p["drift"]], not_ready=None, snapshot_stale=False)
        else:
            st.update(pos=None, snapshot_stale=True)
    else:
        p["stood"] = None
        st["snapshot_stale"] = False
    menu = st.setdefault("menu", {})
    if p["minus"] == "map":
        menu["map_page"] = {"up": True, "panes": 6, "shown": 6}
    elif p["minus"] == "multiplayer":
        menu["minus"] = {"open": True, "tab": "multiplayer", "view": "main"}
    coop = menu.get("coop") or {}
    if coop.get("open") and coop.get("page") == "settings":
        coop["rows"] = [{"name": "Shared floor switches", "kind": "toggle", "on": False},
                        dict(ROW, on=p["force"])]
