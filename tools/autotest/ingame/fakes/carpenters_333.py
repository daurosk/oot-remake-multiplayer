"""Fake-game reactions for row 333's carpenter tests (ingame/test_carpenters_333.py; owner carpenter-333). Hooks:
fakegame.py docstring. Reacts only to row 333's own flags and to a warp into GerudoFortress, so no other row sees it.

- A 0 -> 1 of a cell's end flag is set in the other game 12 frames later (coop_story.cpp, Kind::world: 0 -> 1 only).
- 20 frames after any cell flag write, each game's two count Ints follow its rescues (coop_carpenters.h FollowRescues,
  coop_hook.cpp FollowCarpenters: 'carpenters freed: our count -> N (the shared rescues; row 333)').
- A warp into a GerudoFortress scene with all four cells rescued and the card scene unplayed: 90 frames later the
  mender's line (coop_hook.cpp, Row 333 mender), the card scene played and the card held once (IsGet + StockNum 1),
  shared to the other game (coop_story.cpp Kind::scene; game_items.cpp:38 R::any, :333 R::max).
- `flag_read` of the Ints answers kind "int" (docs/playtest.md "Commands": flag_read).
BT_FAKE_CARPENTERS=old: the counts never follow and no mender (the module before row 333): the tests FAIL.
"""
import os

HELPED = "GerudoFortress_Meet_FourCarpenter_NumOfCarpenterHelped"
LEFT = "GerudoFortress_Meet_FourCarpenter_NumOfCarpenterHelped_02"
CARD_PLAYED = "HavePlayedEvent.Meet_FourCarpenter_ClearEvent"
CARD_GOT = "IsGet.Item_Token_Gerudo"
CARD_NUM = "PouchData.Item_Token_Gerudo.StockNum"
CELLS = [("GerudoFortress_IsAfterHelpCarpenter_Npc_Hylia_0%d_Adult" % (12 + n), "GerudoFortress_WarpPoint0%d_ClearEvent" % n)
         for n in (1, 2, 3, 4)]
CELL_FLAGS = {f for c in CELLS for f in c}
INTS = (HELPED, LEFT, CARD_NUM)


def _old():
    return os.environ.get("BT_FAKE_CARPENTERS") == "old"


def setup(game):
    game.flags.setdefault(HELPED, 0)
    game.flags.setdefault(LEFT, 4)
    game.flags.setdefault(CARD_NUM, 0)


def _rescued(game):
    return sum(1 for a, w in CELLS if game.flags.get(a) in (1, True) or game.flags.get(w) in (1, True))


def _follow(game):
    n = _rescued(game)
    if game.flags.get(HELPED) != n:
        game.flags[HELPED] = n
        game.say("coop: story: carpenters freed: our count -> %d (the shared rescues; row 333)" % n)
    if game.flags.get(LEFT) != 4 - n:
        game.flags[LEFT] = 4 - n
        game.say("coop: story: carpenters left: our count -> %d (row 333)" % (4 - n))


def _mend(game):
    if _rescued(game) != 4 or game.flags.get(CARD_PLAYED) in (1, True) or not str(game.scene).startswith("GerudoFortress"):
        return
    game.say("coop: story: all four carpenters are freed here but the card scene never played (row 333): "
             "IsInFirstPlace set, Meet_FourCarpenter_ClearEvent asked")
    for g in (game, game.other):
        if g is not None:
            g.flags[CARD_PLAYED] = 1
            g.flags[CARD_GOT] = 1
            g.flags[CARD_NUM] = max(1, int(g.flags.get(CARD_NUM) or 0))


def command(game, m):
    if m.get("cmd") != "flag" or m.get("name") not in CELL_FLAGS:
        return None
    name, value = m["name"], m["value"]
    was = game.flags.get(name, 0)
    o = game.other
    if o is not None and value in (1, True) and was in (0, False, None):
        o.later(12, lambda: o.flags.__setitem__(name, 1))
    if not _old():
        for g in (game, o):
            if g is not None:
                g.later(20, lambda g=g: _follow(g))
    return None   # (the built-in answer sets this game's own flag and replies `was`)


def after(game, m, r):
    if m.get("cmd") == "flag_read" and m.get("name") in INTS:
        r["kind"] = "int"
        r["value"] = int(game.flags.get(m["name"]) or 0)
    elif m.get("cmd") == "warp" and str(m.get("scene", "")).startswith("GerudoFortress") and not _old():
        game.later(90, lambda: _mend(game))
