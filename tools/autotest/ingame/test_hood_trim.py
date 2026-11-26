"""Row 278 (C4-4): the Cap trim on the Goron and Zora hoods only when asked, A's own Link and B's puppet of A. The port of
coord offline/cap-trim-c44-run.ps1 (scenario tools/autotest/scenarios/hat-trim-278.A/B.txt, passes OFF and ON) to the
bridge (reports/port-sweep-c131.md G1d). Owner: port-sweep-c131-2.

Pass OFF (A: look_cap_trim only): the hoods follow their tunic, as in 0.11.12. Pass ON (+ look_cap_trim_hoods = on):
the Goron and Zora hoods take the trim. Each pass is its own boot (its A switch, below): the rc runs both. Marks as the old
scenario: child-kokiri, adult-kokiri, adult-goron, adult-zora; a shot of each game at each (A's own Link on A, B's puppet
of A on B): user-facing.
"""
import re

import pytest

from test_gear import AGE_FLAG, GATE, GORON, SCENE_ADULT, saved_gear, way_back, wear
from test_rows import need_child_in_play
from test_tunic import puppet_at, stand_apart

# The old run's coop-options.ini lines as the test's switch (LICHII_COOP_LOOK: hookshot/src/coop_look.h EnvParts, key:value
# by the settings keys look_cap_trim / look_cap_trim_hoods of coop_look.cpp Parts(); 'on' is the switch's word,
# coop_look.cpp ReadParts). Yellow, not the scenario comment's red: the Goron tunic is red (cap-trim-c44-run.ps1 sources).
# No LICHII_COOP_COLOR: A's tunic stays the Kokiri green (hat-trim-278.A.txt "no `color`").
OFF = "A:LICHII_COOP_LOOK=cap_trim:yellow"
ON = "A:LICHII_COOP_LOOK=cap_trim:yellow,cap_trim_hoods:on"
ROW_ENV = ("LICHII_COOP_DEBUG_ANIM=1", "LICHII_COOP_CHEATS=1")          # hat-trim-278.A.txt '#! switches'
ZORA, NORMAL = "Armor_Adult_Zora", "Armor_Adult_Normal"                 # docs/items.md:93-97
# A's look lines (hookshot/src/coop_look_live.cpp:768 StartModule; lichii/hook/coop_look_share.cpp:355/334 PLAYER_LOOK).
# The pack's cap.bin must hold the hood trims: 'the Cap trim paints N blocks of 6 tunic textures (M of the Goron and
# Zora tunics)' with M > 0 (cap-trim-c44-run.ps1's pack check; the form: test_look.py CAP_LINE).
CAP_RX = r"the Cap trim paints \d+ blocks of 6 tunic textures \((\d+) of the Goron and Zora tunics\)"

# The trim check (bandcheck.measure: trim-yellow pixels in a Link's box, less the side box). Per mark: A's own Link on A's
# shot, B's puppet of A on B's shot. The Kokiri marks must show the trim in both passes (the measure's control); the
# hood marks show it only in pass ON; the puppet as A's own Link at every mark (hat-trim-278.A.txt "A's puppet the same
# at each step"). NOT MEASURED: TRIM_MIN, HOOD_ON_SHARE / HOOD_OFF_SHARE (a hood mark's trim per pixel of its box against
# the same Link's adult-kokiri mark: the Goron hood's own gold stitching may count as trim), PUPPET_SHARE, and
# bandcheck's ranges. Until MEASURED cites lane runs of both passes, a run whose checks all hold ends NO-REF, never PASS.
TRIM_MIN = 4
HOOD_ON_SHARE = 0.35
HOOD_OFF_SHARE = 0.10
PUPPET_SHARE = 0.35
MEASURED = None   # e.g. "runs <off> + <on>: adult-goron own share .. (OFF) vs .. (ON), ..."
MARKS = ("child-kokiri", "adult-kokiri", "adult-goron", "adult-zora")


def trim_at(pair, mark):
    """Each game's shot at `mark`; {('A'|'B'): bandcheck.measure of A's Link (own on A, puppet on B)}."""
    import bandcheck
    out = {}
    for x in ("A", "B"):
        o = "B" if x == "A" else "A"
        if x == "A":
            target = pair.state("A")["pos"]
        else:
            a = pair.state("A")["pos"]
            target = pair.until("B's puppet of A", lambda: puppet_at(pair, "B", a), max_frames=120)
        s = pair.state(x)
        res = pair.check_shot(x, "hood-trim-%s" % mark, tolerance=0.80)
        other = s["pos"] if x == "B" else pair.state(o)["pos"]
        m = bandcheck.measure(res["path"], s["view"], target, other=other)
        mo = bandcheck.measure(res["path"], s["view"], other, other=target)
        line = "trimcheck %s %s: A's Link %s, the other %s" % (mark, x, m, mo)
        print(line)
        pair.notes.append(line)
        if m and mo:
            clash = bandcheck.disjoint(m["box"], m["side"], mo["box"], mo["side"])
            if clash:
                import pair as P
                raise P.SetupFail("%s %s: the trim check's boxes overlap (%s, %s)" % (mark, x, clash[0], clash[1]))
        assert m, "%s: A's Link is off %s's shot" % (mark, x)
        out[x] = m
    return out


def share(m):
    return m["net"] / float(m["area"])


def dress_armor(pair, name):
    wear(pair, "armor", name)
    pair.step_both(30)
    pair.act("A", "dress armor")      # hat-trim-278.A.txt: the game's own equip call (coop_testinput.cpp Do: Kind::dress)
    pair.step_both(300)


def judge(pair, got, hoods_on):
    kok = got["adult-kokiri"]
    for mark in MARKS:
        for x, who in (("A", "A's own Link"), ("B", "B's puppet of A")):
            m = got[mark][x]
            if mark.endswith("kokiri"):
                assert m["net"] >= TRIM_MIN, "%s: %s shows no Cap trim (%s): the measure is wrong" % (mark, who, m)
            elif hoods_on:
                assert m["net"] >= TRIM_MIN and share(m) >= HOOD_ON_SHARE * share(kok[x]), (
                    "%s: %s's hood has no Cap trim with look_cap_trim_hoods on (row 278): %d trim px in %d" % (
                        mark, who, m["net"], m["area"]))
            else:
                assert share(m) <= HOOD_OFF_SHARE * share(kok[x]), (
                    "%s: %s's hood takes the Cap trim with look_cap_trim_hoods off (C4-4): %d trim px in %d" % (
                        mark, who, m["net"], m["area"]))
        own, pup = got[mark]["A"], got[mark]["B"]
        if own["net"] >= TRIM_MIN:
            assert share(pup) >= PUPPET_SHARE * share(own), (
                "%s: B's puppet of A shows less trim than A's own Link (%d in %d vs %d in %d)" % (
                    mark, pup["net"], pup["area"], own["net"], own["area"]))


def run_row(pair, hoods_on):
    with pair.setup():
        need_child_in_play(pair)
        want = "cap_trim:yellow" + (",cap_trim_hoods:on" if hoods_on else "")
        bad = pair.log["A"].find(r"LICHII_COOP_LOOK: '", pos=0)        # (EnvParts: a pair it could not take)
        assert bad is None, "A: %s" % bad
        assert pair.log["A"].find(r"the parts are LICHII_COOP_LOOK's \(%s\)" % re.escape(want), pos=0), \
            "A: the module did not take LICHII_COOP_LOOK=%s" % want
        cap = pair.log["A"].find(CAP_RX, pos=0)
        assert cap and int(re.search(CAP_RX, cap).group(1)) > 0, "A: the pack's cap.bin has no hood trims (%s)" % cap
        assert pair.log["B"].find(r"P\d+ wears %d part colour\(s\)" % (2 if hoods_on else 1), pos=0), \
            "B did not get A's look (PLAYER_LOOK)"
        gear = saved_gear(pair)
        stand_apart(pair)             # hat-trim-278.B.txt: B faces up the path, camera behind (ZL); A beside
    done = False
    got = {}
    try:
        pair.step_both(60)
        got["child-kokiri"] = trim_at(pair, "child-kokiri")
        for x in ("A", "B"):          # hat-trim-278 A/B: age flag after the load, wait 60, then the warp
            pair.flag(x, AGE_FLAG, 0)
        pair.step_both(60)
        pair.warp_both(SCENE_ADULT, entrance=GATE)
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s) and s.get("age") == "adult", "%s not an adult in play (%s)" % (x, s.get("age"))
        stand_apart(pair)
        pair.step_both(120)
        got["adult-kokiri"] = trim_at(pair, "adult-kokiri")
        pair.give("A", "PouchData.%s.StockNum" % GORON)
        pair.give("A", "PouchData.%s.StockNum" % ZORA)
        pair.step_both(120)
        dress_armor(pair, GORON)
        got["adult-goron"] = trim_at(pair, "adult-goron")
        dress_armor(pair, ZORA)
        got["adult-zora"] = trim_at(pair, "adult-zora")
        dress_armor(pair, NORMAL)
        judge(pair, got, hoods_on)
        done = True
    finally:
        way_back(pair, gear, done, warp_child=True)
    if not MEASURED:
        pair.unproven("the trim check's values are not measured yet (MEASURED: lane runs of both passes); the trimcheck "
                      "lines are in the notes")


@pytest.mark.budget(300)
@pytest.mark.look
@pytest.mark.needs_env(OFF, *ROW_ENV)
@pytest.mark.row("278 C4-4 pass OFF: the Goron and Zora hoods follow their tunic (cap-trim-c44, hat-trim-278)")
def test_hood_trim_off(pair):
    run_row(pair, hoods_on=False)


@pytest.mark.budget(300)
@pytest.mark.look
@pytest.mark.needs_env(ON, *ROW_ENV)
@pytest.mark.row("278 pass ON: look_cap_trim_hoods = on puts the Cap trim on the Goron and Zora hoods (cap-trim-c44)")
def test_hood_trim_on(pair):
    run_row(pair, hoods_on=True)
