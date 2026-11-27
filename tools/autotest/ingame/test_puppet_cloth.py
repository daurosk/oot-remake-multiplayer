"""Row 313 (owner 2026-11-27: "link puppets have no cloth physics so things like his cap are static"). The fix under
test: cloth-puppet-313 (tools/puppet_pack/build.py add_cloth: every puppet's Physics holds the player's four cloth
keys; docs/puppet.md "Cloth (row 313)"). The lane's pack is the tested sha's own (offline/lib/lane-pack.ps1).

| Test | Checks | Kind |
|---|---|---|
| test_cloth_child | the game holds a cloth for the other player's child puppet (on each game), the cloth words move while it runs and settle while it stands, shots of the puppet running | log + LOOK |
| test_cloth_adult | the same for an adult puppet, then again after both warp to another scene (a puppet's jump to a new place) | log + LOOK |

Judged by the module's read-only probe (LICHII_COOP_DEBUG_CLOTH=1, lichii/hook/coop_puppet.cpp Director::ClothProbe):
once a second per shown copy the chain the game's own hair code reads (main+0x4b8438..0x4b84b4), then the same for
our own Link. Before the fix a puppet's holder is 0 (its Physics was InterestSpotActor's, whose set is empty) while our
own Link's is not. Every pattern below is that function's own line.
"""
import math
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_dbno_crawl as dc   # noqa: E402  (to_place, stick_move, shots: the shared set-up and moves)
import test_enemies as en      # noqa: E402  (MARKET, MARKET_ENTRANCE: a cited warp target)

ENV = "LICHII_COOP_DEBUG_CLOTH=1"   # coop_puppet.cpp Director::ClothProbe's switch
# coop_puppet.cpp Director::ClothProbe: "cloth: <who> physics <p> set <s> holder <C> n <n> list <O> entries <m>"
# [+ " e0 flags <16 hex>:" + 30 floats]; <who> is "P<n>" (a copy) or "P<n> our own Link's"; Hex() prints 0 as "0".
# Every Say line is "coop: puppet: " + text (Director::Say).
CLOTH = re.compile(r"coop: puppet: cloth: (P\d+)( our own Link's)? physics (\S+) set (\S+) holder (\S+) n (-?\d+) "
                   r"list (\S+) entries (-?\d+)(?: e0 flags ([0-9a-f]{16}):(.*))?$")
ANY_COPY = r"coop: puppet: cloth: P\d+ physics "          # a copy's line (not "our own Link's")
PROBE_FRAMES = 60          # coop_puppet.h kClothProbeFrames: one line a second per copy
STILL_FRAMES = 240         # 4 probe lines standing (the first after the move is left out: the cloth still settles)
RUN_FRAMES = 180           # 3 probe lines running (stick held, test_dbno_crawl.stick_move)
APART_M = 4.0              # B this far in front of A, A facing it: A's camera behind A holds B's puppet
SCENE_CHILD, AT_CHILD = "KokiriForest_Child", "FrontDekuTree"                    # hs-dbno-child (test_downed_move)
SCENE_ADULT, AT_ADULT = "HyruleField_Adult", "HyruleField_FrontOfKakarikoVillage"   # hs-dbno-adult (test_downed_move)


def parse(line):
    m = CLOTH.search(line)
    if not m:
        return None
    words = [float(w) for w in m.group(10).split()] if m.group(10) else []
    return {"who": m.group(1), "own": bool(m.group(2)), "physics": m.group(3), "set": m.group(4),
            "holder": m.group(5), "n": int(m.group(6)), "list": m.group(7), "entries": int(m.group(8)),
            "flags": m.group(9), "words": words}


def lines(pair, x, pos, own=False):
    out = [parse(ln) for ln in pair.log[x].since(pos)]
    return [c for c in out if c and c["own"] == own]


def change(cs):
    """The largest word-by-word change between two probe lines in a row (metres or whatever the words are)."""
    best = 0.0
    for a, b in zip(cs, cs[1:]):
        if a["words"] and len(a["words"]) == len(b["words"]):
            best = max(best, max(abs(p - q) for p, q in zip(a["words"], b["words"]) if math.isfinite(p - q)))
    return best


def face_other(pair):
    """B APART_M in front of A, both facing each other (test_epona.face_rider: A faces B, ZL puts the camera behind A)."""
    a = pair.state("A")["pos"]
    pair.act("B", "place %.2f %.2f %.2f" % (a[0], a[1], a[2] + APART_M))
    pair.act("B", "face %.2f %.2f" % (a[0], a[2]))
    pair.step_both(10)
    b = pair.state("B")["pos"]
    pair.act("A", "face %.2f %.2f" % (b[0], b[2]))
    pair.press("A", ["ZL"], frames=8)


def holds_cloth(pair, x, pos, what):
    """Game x's copy of the other player has the game's cloth (holder, its count and its list's entries), and its own
    Link has one too (the reference: the game's own player). Returns (copy lines, own lines)."""
    copies, own = lines(pair, x, pos), lines(pair, x, pos, own=True)
    assert copies, "%s %s: no 'cloth:' line of a copy (is %s on and the copy shown?)" % (x, what, ENV)
    assert own and own[-1]["holder"] != "0", "%s %s: our own Link has no cloth holder: %s" % (x, what, own[-1:])
    last = copies[-1]
    pair.notes.append("%s %s: copy %s holder %s n %d entries %d; our Link holder %s n %d entries %d" % (
        x, what, last["who"], last["holder"], last["n"], last["entries"], own[-1]["holder"], own[-1]["n"],
        own[-1]["entries"]))
    assert last["holder"] != "0", "%s %s: the game holds NO cloth for %s (holder 0: the pack's Physics has no " \
        "cloth; row 313's bug)" % (x, what, last["who"])
    assert last["n"] >= 1 and last["entries"] >= 1, "%s %s: %s's cloth is empty (n %d, entries %d)" % (
        x, what, last["who"], last["n"], last["entries"])
    return copies, own


def swings(pair, what):
    """B stands, then runs, then stands: A's probe of B's copy (the first cloth entry's words) and shots of it running.
    The words moving more while it runs than while it stands is the finding; an equal change is NO-REF (the first
    entry may not be a moving piece), never PASS on the log alone: the shots show the swing."""
    pair.step_both(PROBE_FRAMES)                     # the move's cloth settles before the still sample
    at = pair.log["A"].mark()
    pair.step_both(STILL_FRAMES)
    still = change(lines(pair, "A", at))
    at = pair.log["A"].mark()
    ran = dc.stick_move(pair, "B", 90, RUN_FRAMES // 2)       # across A's view (B faces A: stick right = A's left)
    dc.shots(pair, "cloth-%s-running" % what)                 # (2 shots per game, 6 frames apart, while it swings)
    ran += dc.stick_move(pair, "B", 270, RUN_FRAMES // 2)
    moving = change(lines(pair, "A", at))
    pair.notes.append("%s: B ran %.2f m; A's copy's first cloth entry changed up to %.4g a second running, %.4g "
                      "standing" % (what, ran, moving, still))
    assert ran >= 1.0, "%s: B did not run (%.2f m)" % (what, ran)
    return moving, still


@pytest.mark.budget(300)
@pytest.mark.look
@pytest.mark.needs_env(ENV)
@pytest.mark.row("#313 other players' Link puppets have cloth (cap and hair move): child")
def test_cloth_child(pair):
    with dc.restored(pair, {}):          # (back to the boot's age and place after: test_dbno_crawl's own restore)
        with pair.setup():
            pair.need_boot_place("child")
            dc.to_place(pair, "child", SCENE_CHILD, AT_CHILD)
            face_other(pair)
            start = {x: pair.log[x].mark() for x in ("A", "B")}
            for x in ("A", "B"):
                pair.expect_log(x, ANY_COPY, max_frames=PROBE_FRAMES * 3)
        for x in ("A", "B"):
            holds_cloth(pair, x, start[x], "child")
        moving, still = swings(pair, "child")
        if moving <= still:
            pair.unproven("the probe's first cloth entry did not move more running (%.4g) than standing (%.4g): the "
                          "shots decide the swing" % (moving, still))


@pytest.mark.budget(420)
@pytest.mark.look
@pytest.mark.needs_env(ENV)
@pytest.mark.row("#313 other players' Link puppets have cloth: adult, and after a warp")
def test_cloth_adult(pair):
    with dc.restored(pair, {}):          # (back to the boot's age and place after: test_dbno_crawl's own restore)
        with pair.setup():
            pair.need_boot_place("child")
            dc.to_place(pair, "adult", SCENE_ADULT, AT_ADULT)
            face_other(pair)
            start = {x: pair.log[x].mark() for x in ("A", "B")}
            for x in ("A", "B"):
                pair.expect_log(x, ANY_COPY, max_frames=PROBE_FRAMES * 3)
        for x in ("A", "B"):
            holds_cloth(pair, x, start[x], "adult")
        moving, still = swings(pair, "adult")
        # A puppet's jump to a new place: both warp, the copy comes back in the new scene with its cloth.
        pair.warp_both(en.MARKET, entrance=en.MARKET_ENTRANCE)
        pair.step_both(30)
        face_other(pair)
        after = {x: pair.log[x].mark() for x in ("A", "B")}
        for x in ("A", "B"):
            pair.expect_log(x, ANY_COPY, max_frames=PROBE_FRAMES * 3)
            holds_cloth(pair, x, after[x], "adult after the warp")
        pair.step_both(PROBE_FRAMES)
        dc.shots(pair, "cloth-adult-after-warp", twice=False)   # a stretched cloth after the jump would show here
        if moving <= still:
            pair.unproven("the probe's first cloth entry did not move more running (%.4g) than standing (%.4g): the "
                          "shots decide the swing" % (moving, still))
