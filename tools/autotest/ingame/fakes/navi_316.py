"""Fake-game reactions for row 316's Navi probe (ingame/test_probe_navi_316.py; lichii/hook/coop_navi_probe.h). Hooks:
fakegame.py docstring.

Acts ONLY in a boot whose --env has BT_PROBE_NAVI=1 (the probe's own needs_env switch, B #149 scope rule): no other
row's test boots with it, so no other test sees the `navi` key.
- state: `navi` as coop_navi_probe.cpp Json gives it: our Npc_Navi with 3 AS slots (slot 0 `Wait`, or `LockOn_St`
  while the last press held ZL; slot 1 `Status_Red` while our Link's life is 4 quarter hearts or less, else empty;
  slot 2 empty), and one live fairy (the other game's player number).
BT_FAKE_NAVI=null: `navi` stays null (the module without coop_navi_probe, or fairies off): the probe must SETUP-FAIL
naming the null member.
"""
import os

ON = "BT_PROBE_NAVI=1"


def setup(game):
    game.navi_lock = False


def press(game, buttons):
    if game.env_on(ON):
        game.navi_lock = "ZL" in buttons
    return False


def _actor(actor, number, slots):
    return {"actor": actor, "number": number, "read": True, "state": 4, "present": True, "pos": [1.0, 2.0, 3.0],
            "rot": [1, 0, 0, 0, 1, 0, 0, 0, 1], "slot_count": len(slots),
            "slots": [{"name": n, "frame": 1.0} for n in slots]}


def state(game, st):
    if not game.env_on(ON):
        return
    if os.environ.get("BT_FAKE_NAVI", "") == "null":
        st["navi"] = None
        return
    low = game.link_life["PlayerStatus.Life"] <= 4
    navi = _actor("0x81000000", 0, ["LockOn_St" if game.navi_lock else "Wait", "Status_Red" if low else "", ""])
    fairy = _actor("0x82000000", 2 if game.tag == "A" else 1, ["", ""])
    st["navi"] = {"ms": game.frame * 16, "frames": game.frame, "navi": navi, "fairies": [fairy]}
