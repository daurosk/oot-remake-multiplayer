"""Fake-game reaction for row 313's cloth probe (ingame/test_puppet_cloth.py; owner cloth-puppet-313).

Only with the boot's LICHII_COOP_DEBUG_CLOTH=1: every 60 frames (coop_puppet.h kClothProbeFrames) the line
lichii/hook/coop_puppet.cpp Director::ClothProbe writes for the copy of the other player, then for our own Link
('cloth: <who> physics <p> set <s> holder <C> n <n> list <O> entries <m> e0 flags <16 hex>: <30 floats>', each Say
line 'coop: puppet: ' + text). The copy's first entry's words follow the other player's place (they move while it
runs, stay while it stands).
The copy's holder is 0 (row 313's bug) on a boot with the --pack no_cloth control in (testpack.py's manifest
coop-test-pack.json in the game folder names it; not coop_puppet.json, which other fakes write) or with
BT_FAKE_CLOTH=none.
Selftest variable BT_FAKE_CLOTH: none (the copy's holder 0), own-none (our own Link's holder 0 too: the probe finds
no cloth at all), still (the copy's words never change: the NO-REF path).
"""
import json
import os

ENV = "LICHII_COOP_DEBUG_CLOTH=1"   # (coop_puppet.cpp Director::ClothProbe's switch)
EVERY = 60                        # (coop_puppet.h kClothProbeFrames)
MANIFEST = "coop-test-pack.json"   # (testpack.MANIFEST)


def _pack_cloth(game):
    """False while testpack's manifest in the game folder names the no_cloth pack (the control boot)."""
    try:
        with open(os.path.join(game.folder, MANIFEST), encoding="utf-8") as f:
            return '"no_cloth"' not in json.dumps(json.load(f))
    except (OSError, ValueError):
        return True


def _line(who, holder, words):
    if holder == "0":
        return "coop: puppet: cloth: %s physics 0x1000 set 0x2000 holder 0 n 0 list 0 entries 0" % who
    return ("coop: puppet: cloth: %s physics 0x1000 set 0x2000 holder %s n 1 list 0x4000 entries 12 e0 flags "
            "0000000000000001: %s" % (who, holder, " ".join("%.4g" % w for w in words)))


def after(game, m, r):
    if m.get("cmd") not in ("step", "input") or not game.env_on(ENV) or game.other is None:
        return
    start = r.get("from")
    if start is None:
        return
    for f in range(int(start) + 1, int(game.frame) + 1):
        if f % EVERY:
            continue
        o = game.other
        who = "P%d" % (1 if o.tag == "A" else 2)
        mode = os.environ.get("BT_FAKE_CLOTH", "")
        copy = "0" if mode in ("none", "own-none") or not _pack_cloth(game) else "0x3000"
        words = [0.5] * 30 if mode == "still" else [p * 0.1 for p in o.pos] + [0.5] * 27
        game.say(_line(who, copy, words))
        game.say(_line(who + " our own Link's", "0" if mode == "own-none" else "0x5000",
                       [p * 0.1 for p in game.pos] + [0.5] * 27))
