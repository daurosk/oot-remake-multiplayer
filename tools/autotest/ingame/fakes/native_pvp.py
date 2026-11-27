"""Fake-game reactions for the true PvP probe (ingame/test_native_pvp.py, which cites each line's writer). Scoped
(B #149): it acts only in a game started with LICHII_COOP_PVP_NATIVE=1 (the probe's own switch, no other test sets it)
and only in the probe's scene.

- A 'chat /pvp on' / 'off': both games log the mode (coop_pvp_game.cpp:152), then their _Pvp copies (coop_pvp_native_game.cpp
  :232).
- A pressing the sword (Y), the jump (A) or the item (ZR) with ZL, within 3 m of B: A logs the copy's U2 record and its
  strike 4 frames later, B its outcome 6 frames after that: kept_off (6, PathBLive false on this base), or guarded
  (1) with its U5 line when B pressed ZL (the shield) in the last 12 frames. One strike per press (2-frame
  feeds of one press are one swing).
Selftest switch BT_FAKE_PVP_BUG (tests/test_ingame_native_pvp.py): nostrike -> A never strikes (no record, no
strike line); nopack -> both games say the pack has no _Pvp copies.
"""
import math
import os

SWITCH = "LICHII_COOP_PVP_NATIVE=1"
SCENE = "HyruleField_Child"
REACH_M = 3.0
GUARD_FRAMES = 12
PRESS_GAP = 3      # an attack press this soon after the last one is the same press (the test feeds 2 frames at a time)


def _bug():
    return os.environ.get("BT_FAKE_PVP_BUG", "")


def _on(game):
    return game.env_on(SWITCH) and game.scene == SCENE


def setup(game):
    game.pvp = {"seq": 0, "guard_at": -1000, "last": -1000}


def act(game, w):
    if game.tag != "A" or w[:2] != ["chat", "/pvp"] or not _on(game):
        return False
    if w[2:3] == ["off"]:
        for g in (game, game.other):
            g.later(4, lambda g=g: g.say("coop: pvp: off (set by 1)"))
        return False
    if w[2:3] != ["on"]:
        return False
    for g, other in ((game, 2), (game.other, 1)):
        g.later(4, lambda g=g: g.say("coop: pvp: on (set by 1)"))
        if _bug() == "nopack":
            g.later(6, lambda g=g: g.say("coop: pvp native: on; the puppet pack has no _Pvp copies"))
        else:
            line = "coop: pvp native: _Pvp copies for players %#x (PvP on)" % (1 << (other - 1))
            g.later(8, lambda g=g, line=line: g.say(line))
    return False


def press(game, buttons):
    if not _on(game):
        return False
    if game.tag == "B":
        if "ZL" in buttons:
            game.pvp["guard_at"] = game.frame
        return False
    if "ZL" not in buttons or not ({"Y", "A", "ZR"} & set(buttons)) or _bug() == "nostrike":
        return False
    held, game.pvp["last"] = game.frame - game.pvp["last"] <= PRESS_GAP, game.frame
    if held:
        return False
    b = game.other
    if b.scene != SCENE or math.hypot(game.pos[0] - b.pos[0], game.pos[2] - b.pos[2]) > REACH_M:
        return False
    game.pvp["seq"] += 1
    seq = game.pvp["seq"]
    words = " ".join("%08x" % (0x100 * seq + i) for i in range(38))
    game.later(4, lambda: game.say("coop: pvp native: U2 record of our hit on P2's copy (id %#x, +0x228 4): %s"
                                   % (0x40 + seq, words)))
    game.later(4, lambda: game.say("coop: pvp native: strike %d on P2 (source 0, our game counted 4) seen at 1000; "
                                   "U7: our main AS Attack_S1, weapon 1" % seq))

    def answer():
        guard = b.frame - b.pvp["guard_at"] <= GUARD_FRAMES
        if guard:
            b.say("coop: pvp native: U5: our Link's main AS at a strike: GuardWait (guard yes)")
        b.say("coop: pvp native: U5 bits at P1's strike %d: player +0x32 %#04x (bit 4 %d), EquipmentUser +0x13a0 "
              "0x00000000 (bit 5 0), AS guard %s" % (seq, 0x10 if guard else 0, 1 if guard else 0,
                                                       "yes" if guard else "no"))
        b.say("coop: pvp native: P1's strike %d: outcome %d, take 0 (attacker said 4), life 12"
              % (seq, 1 if guard else 6))
    game.later(10, answer)
    return False
