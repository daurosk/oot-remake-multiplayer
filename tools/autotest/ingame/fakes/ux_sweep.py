"""Fake-game reactions for ux-sweep-0120's rows (ingame/test_ux_sweep.py; owner ux-sweep-0120). Hooks: fakegame.py
docstring. Each reacts only to its own flag or scene.

- Gift (rows 171/179): HavePlayedEvent.Dm_Title_0096 is a kOnceGifts Bool, so the share line and the relay are
  fakes/gifts_237.py's (the ONE gifts fake, its share(); its BT_FAKE_GIFTS=off/back modes are this row's bugs too):
  this file adds nothing for it.
- Scrub (row 100), LostWood_Child: the rear and front Business Scrubs (Banc LostWood_Child_Static) in the world list,
  owner null (CoopObjects::IsOwnPuzzle). After a `hit` the rear one offers 'Talk'; A starts a talk, the talk ends
  after 3 more presses.
- Lock (row 73), ForestTemple: adult Link; the first door's lock (Banc ForestTemple_Static) in each game until that
  game opens it: leaning in with a key spends it (the count is shared: the other game's goes down too); with no key
  and the lock already opened by the other game, the lent key opens it (docs/dungeon.md "Locked doors").
BT_FAKE_UX (the bugs the tests must catch):
scrub-shared (the scrubs have owner 2 in A), scrub-softlock (the second talk never ends), lock-stuck (no lent key),
lock-double (the lent key reaches the group as a found key: +1 in both).
- Final blow (row 283), TwinrovaStage: Koume and Kotake (coop_boss.cpp:41) alive in the world list until a `kill`
  names them; the frame windows after `resume` are fakes/row83.py's (BT_FAKE_FPS_B=40: the guest drops frames).
"""
import math
import os

KEYS = "DungeonInfo.ForestTemple.SmallKeyNum"
REAR, FRONT = (8.79, 82.89, -89.41), (7.30, 82.89, -85.59)
LOCK = (-0.04, 103.03, 44.59)


def _mode():
    return os.environ.get("BT_FAKE_UX", "")


def setup(game):
    game.ux_talk, game.ux_talks, game.ux_offer, game.ux_lock_open = 0, 0, False, False
    game.ux_dead, game.ux_strike = set(), None


def after(game, m, r):
    if m.get("cmd") == "flag" and m.get("name") == KEYS:
        o, v = game.other, m.get("value")
        o.later(12, lambda: o.flags.__setitem__(KEYS, v))      # a set count reaches the group


def act(game, words):
    if words[0] == "hit" and game.scene == "LostWood_Child":
        # the module's parser: 'hit [ID] ACTOR N' (coop_testinput.cpp:557-566); anything else is refused, no strike
        if words[1:] == ["Enemy_Scrub_Business", "1"]:
            game.ux_strike = {"seq": 1, "result": "done", "name": words[1]}
            game.later(60, lambda: setattr(game, "ux_offer", True))
        return True
    if words[0] == "kill" and _mine_boss(game) and len(words) == 3 and words[1].isdigit():
        seq = (game.ux_strike or {}).get("seq", 0) + 1
        game.ux_strike = {"seq": seq, "result": "done", "name": words[2], "id": int(words[1])}
        if _mode() != "kill-miss":      # kill-miss: the strike is made but the part lives on (a blow that did not land)
            game.ux_dead.add(words[2])  # 'kill ID ACTOR'
        return True
    return False


def _mine_boss(game):
    """Row 283's Twinrova only without boss_awake.py's switch (that fake has its own sisters)."""
    return game.scene == "TwinrovaStage" and not game.env_on("LICHII_COOP_DEBUG_BOSS=1")


def _near(game, at, m):
    return math.dist((game.pos[0], game.pos[2]), (at[0], at[2])) < m


def press(game, buttons):
    if "A" not in buttons:
        return False
    if game.scene == "LostWood_Child" and game.ux_offer:
        if game.ux_talk == 0:
            game.ux_talk = 1
        else:
            game.ux_talk += 1
            stuck = _mode() == "scrub-softlock" and game.ux_talks >= 1
            if game.ux_talk > 4 and not stuck:
                game.ux_talk, game.ux_talks = 0, game.ux_talks + 1
        return True
    if game.scene == "ForestTemple" and not game.ux_lock_open and _near(game, LOCK, 2.5):
        keys = game.flags.get(KEYS, 0) or 0
        o = game.other
        if keys > 0:
            game.flags[KEYS] = keys - 1
            o.later(12, lambda: o.flags.__setitem__(KEYS, max(0, (o.flags.get(KEYS, 0) or 0) - 1)))
            game.ux_lock_open = True
        elif o.ux_lock_open and _mode() != "lock-stuck":
            game.ux_lock_open = True
            if _mode() == "lock-double":
                for g in (game, o):
                    g.later(60, lambda g=g: g.flags.__setitem__(KEYS, (g.flags.get(KEYS, 0) or 0) + 1))
        return True
    return False


def actors(game):
    out = []
    if game.scene == "LostWood_Child":
        own = 2 if (_mode() == "scrub-shared" and game.tag == "A") else None
        for i, p in enumerate((REAR, FRONT)):
            out.append({"id": 900 + i, "name": "Enemy_Scrub_Business", "kind": "enemy", "pos": list(p), "owner": own,
                        "alive": True})
    if game.scene == "ForestTemple" and not game.ux_lock_open:
        out.append({"id": 950, "name": "Obj_DoorChainLock_A_01", "kind": "object", "pos": list(LOCK), "owner": None})
    if _mine_boss(game):
        for i, n in enumerate(("Enemy_DungeonBoss_Koume", "Enemy_DungeonBoss_Kotake")):
            out.append({"id": 970 + i, "name": n, "kind": "enemy", "pos": [3.0 * i, 0.0, 10.0], "owner": 1,
                        "alive": n not in game.ux_dead})
    return out


def state(game, st):
    if not st.get("scene"):
        return
    if game.scene == "LostWood_Child":
        st["event"] = game.ux_talk > 0
        st["prompt"] = "Talk" if game.ux_offer and not game.ux_talk else None
        if game.ux_strike:
            st["strike"] = game.ux_strike
    if _mine_boss(game) and game.ux_strike:
        st["strike"] = game.ux_strike
    if game.scene == "ForestTemple" or _mine_boss(game):
        st["age"] = "adult"
