"""PROBE (not a row; test_probe_pg_bow_287.py): do REAL arrow hits start Phantom Ganon's phase 2 (knock-down, Fly)?
And boss-stuck-303-4's case: life-only test hits down to 1, then one real arrow: phase 2 or a soft-lock?
(row 287, phantom-ganon-287-6, B #173 GO 08:43 UTC after pganon-probe-8bf5f256.)

Why: pganon-probe-8bf5f256 (reports/phantom-ganon-287-5.md RESULT) showed a sword cannot reach him in phase 1: he rides
inside the paintings (move probe y 82 over the floor's 74.7) and comes out only for his ride-jump attack ('Appear',
'RideJumpAttack_Start' / '_Attack' / '_End', game-out-A.txt:6555-6786, attack verb 115). In real play he is hit there
from range. The Forest Temple's own item is the Fairy Bow, an ADULT item, so this probe plays adult Link with the bow.

Runs only with --env BT_PROBE_PG_BOW=1 (needs_env) on the boot live-phantom-ganon (its switches: LICHII_COOP_CHEATS=1
for give/flag/hit, LICHII_COOP_MOVE_PROBE=1, the boss and object debug lines: live-phantom-ganon.A.txt:8).
Set-up (each value cited):
- adult: Sequence_IsChildhood 0 after the load, 60 frames, then the warp (test_gear.py AGE_FLAG, hs-gear-adult.A.txt;
  test_epona.py to_field), the warp = test_boss_shots.warp_in(PhantomGanonStage) (the map change event, start point
  coop_start_points.inc:112); SETUP-FAIL unless both games' `body` is "adult" (docs/playtest.md state.body).
- the bow: bridge `give bow` (docs/playtest.md:123 `give`; lichii/hook/coop_cheat_give.cpp:88 Names: 'bow' ->
  PouchData.Weapon_Range_Bow.StockNum; coop_cheat_items.cpp:25-28 kIntItems: Weapon_Range_Bow 1, Item_Quiver_Small 1,
  Item_Arrow_Normal 30, capacity Arrow 30) = the Fairy Bow, the Forest Temple's own item, on A and B; SETUP-FAIL unless
  A's inventory shows PouchData.Item_Arrow_Normal.StockNum > 0 (kit_move.ammo_of). Topped up the same way under 10.
- his intro: 3900 frames after the arrival (live-phantom-ganon.A.txt 'wait 3900'), then the row's 600 (test_bosses.py).
- Links kept at full life every chunk (test_enemy_turn.keep_up: the game's own setter): a downed shooter holds nothing.
Shooting = the player's way (kit_move: `assign Bow` on ZR, turn, ZL, ZR held to draw, kit_move.aim_view turns the view
with a stick until the point sits under the reticle, let go) plus the test `aim X Y Z` assist (coop_testinput.h:48-54,
coop_objects.cpp AimStep:1576-1630: the arrow is moved sideways onto the point only when its own flight is within 4
degrees, CoopTestInput::kAimNudgeDeg). The assist ONLY POINTS THE ARROW: its one write is the arrow's position
(coop_objects.cpp:1616 guest.Write(arrow + kActorPosition)) and the set-matrix call; no life, damage or boss state is
written; the game's own NormalArrow flies on with its own way and speed and hits him (or not) by the game's collision,
and his life / phase change is the game's own. In c2 the `hit PG N` life-only hits are a test means to reach life 1;
the real arrow afterwards decides phase 2 or a soft-lock. The bridge wrapper _Both steps the other game every frame A runs (Pair.press rules).
Where to aim is MEASURED in this run, never typed: his world-list place when A's log shows his 'Appear' (in the
painting) and his 'RideJumpAttack_Attack' (out of it), per painting (a new painting = an Appear place more than PAINT_M
from every known one). Height over that place: the candidates DY_LADDER in turn (the probe's question, each shot's dy
recorded; which one his hit box takes is not known: kept as found). A shot at an 'appear' place is drawn and aimed in
advance and let go at his next Appear in that painting; an 'attack' place the same at his next RideJumpAttack_Attack.
Case 1 (full life): shots until phase 2 (an anim with 'Fly' or 'FallDown' in its name, a move probe '-> 1' or an
'attack starts (action verbs 139' line: test_probe_pg_falldown_287 regexes) or CASE1_FRAMES. Then a fresh fight
(warp_in again + intro). Case 2: `hit PG N` (life step, never past its stops: coop_testinput.h 'hit') down to life 1
or its stop, then shots until his life changes or a hit anim, then FLY_WAIT frames: 'phase 2' or 'soft-lock' (no
phase-2 line, his anims and the module's 'stuck' lines listed).
Ends NO-REF (Pair.unproven), never PASS: <out>/probe-pg-bow.json (every shot: moment, point, dy, aim reply and the
module's aim line, life before/after, anims after) + one summary line per case. No shots of the screen (a probe).
"""
import json
import math
import os
import re
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pair as P                                                  # noqa: E402
import kit_move as km                                             # noqa: E402  (tools/autotest, on the path)
import playtest_helpers as ph                                     # noqa: E402
from test_bosses import PG, SCENE, boss, dead                     # noqa: E402
from test_boss_shots import warp_in                               # noqa: E402
from test_enemy_turn import keep_up                               # noqa: E402
from test_gear import AGE_FLAG                                    # noqa: E402
from test_probe_pg_falldown_287 import ANIM, ATTACK_139, MOVE_TO_1  # noqa: E402

PROBE_ENV = ("BT_PROBE_PG_BOW=1",)
INTRO_FRAMES = 3900     # live-phantom-ganon.A.txt 'wait 3900' (the intro after the arrival)
ROW_START = 600         # test_bosses.py: the row's frames before its first ask
AGE_WAIT = 60           # test_epona.py to_field / test_gear.py: 'wait 60' between the age flag and the warp
CHUNK = 6               # frames between two looks at the logs while waiting for him
KEEP_EVERY = 300        # frames between two life refills
CASE1_FRAMES = 12000    # case 1's frames at most (~6.7 min of game; ride-outs came every ~25 s in 8bf5f256)
CASE2_FRAMES = 9000     # case 2's shots at most
FLY_WAIT = 1800         # frames after case 2's hit to see phase 2 (the row's phase-2 frames x 3)
WATCH = 60              # frames after a let-go before his life and anims are read
PAINT_M = 3.0           # Appear places closer than this are the same painting (the 3 are ~10 m+ apart: end-A.png)
DY_LADDER = (1.5, 2.5, 0.5)   # metres over his place, tried in turn (the probe's question, not a known value)
LOW_ARROWS = 10
ARROWS = "Item_Arrow_Normal"   # the arrows' pouch item (lichii/hook/coop_cheat_items.cpp:27, docs/items.md:74)
HOLD_MAX = 2400         # frames drawn waiting for his painting before trying another target
CHECK_EVERY = 60        # frames between two checks that the draw still points at the target
DRIFT = 0.1             # screen units off the reticle (kit_move.AIM_TOL 0.02 x 5): the draw was knocked away
RETRY_FRAMES = 300      # frames after an aim that failed before the next draw
HIT_ANIM = re.compile(r"Damage|Down|Fall|Fly", re.I)
OUT_ANIM = re.compile(r"objects: anim %s 0x[0-9a-f]+ '(Appear|RideJumpAttack_Attack)'" % PG)
STUCK = re.compile(r"%s.*(stuck|no hit)" % PG)
AIM_LINE = re.compile(r"coop: test aim .*(nudged|not put).*")       # coop_objects.cpp AimStep
PG_ANIMS = ("Fly", "FallDown")


class _Both:
    """A playtest client for game x whose frames are the pair's: every step/input/move of x runs the other game the
    same frames (Pair.press / step_both), so kit_move's aim never lets one game run alone."""

    def __init__(self, pair, x):
        self.p, self.x = pair, x

    def state(self):
        return self.p.state(self.x)

    def step(self, frames=1):
        self.p.step_both(int(frames))
        return {"state": self.p.state(self.x)}

    def input(self, stick=None, buttons=(), frames=1, rstick=None):
        return self.p.press(self.x, buttons, stick=stick, frames=int(frames), rstick=rstick)

    def move(self, yaw, strength=1.0, frames=1, check=True):
        r = self.p.call(self.x, "move", yaw=round(float(yaw), 2), strength=round(max(0.0, min(1.0, strength)), 3),
                        frames=int(frames), check=check)
        for y in self.p.c:
            if y != self.x:
                self.p.call(y, "step", frames=int(frames))
        return r

    def act(self, line):
        return self.p.act(self.x, line)

    def world(self):
        return self.p.c[self.x].world()

    def text(self, *a, **k):
        return self.p.c[self.x].text(*a, **k)


def painting_of(paintings, at):
    """The index of the known painting whose Appear place is within PAINT_M of `at` (x, z), else None."""
    for i, p in enumerate(paintings):
        if p.get("appear") and math.hypot(p["appear"][0] - at[0], p["appear"][2] - at[2]) <= PAINT_M:
            return i
    return None


def next_target(paintings, n):
    """Shot n's (painting, moment): the paintings and moments known so far in turn; None when none is known."""
    opts = [(i, m) for i, p in enumerate(paintings) for m in ("appear", "attack") if p.get(m)]
    return opts[n % len(opts)] if opts else None


def phase2(lines):
    """The phase-2 / fall-down lines among `lines` (any game)."""
    out = []
    for ln in lines:
        m = ANIM.search(ln)
        if (m and any(k in m.group(1) for k in PG_ANIMS)) or MOVE_TO_1.search(ln) or ATTACK_139.search(ln):
            out.append(ln.strip())
    return out


class _Hunt:
    """One case's shots at him from game A, by the places measured in this fight."""

    def __init__(self, pair, rec, name):
        self.p, self.rec, self.name = pair, rec, name
        self.cl = _Both(pair, "A")
        self.paintings = []
        self.shots = []
        self.pos = {x: pair.log[x].mark() for x in pair.c}
        self.lines = {x: [] for x in pair.c}
        self.ready = None          # (painting, moment, point, dy) drawn and aimed at, or None
        self.frames = 0
        self.since_keep = 0
        self.cur = None            # the painting of his last Appear
        self.held = 0              # frames drawn and waiting
        self.retry_at = 0          # no new draw before this frame count (after a miss to aim)
        self.p2 = []               # phase-2 lines seen in this case (phase2), both games
        rec["cases"][name] = {"paintings": self.paintings, "shots": self.shots, "notes": []}

    def note(self, s):
        self.rec["cases"][self.name]["notes"].append(s)

    def new_lines(self):
        """Lines since the last look, per game (also kept whole for the case's verdict)."""
        out = {}
        for x in self.p.c:
            out[x] = self.p.log[x].since(self.pos[x])
            self.pos[x] = self.p.log[x].mark()
            self.lines[x] += out[x]
            self.p2 += phase2(out[x])
        return out

    def run(self, frames):
        """CHUNK frames: ZR held while drawn, else no input; lives kept up."""
        if self.ready:
            self.cl.input(None, ["ZR"], frames)
            self.held += frames
        else:
            self.p.step_both(frames)
        self.frames += frames
        self.since_keep += frames
        if self.since_keep >= KEEP_EVERY:
            self.since_keep = 0
            keep_up(self.p)
            arrows = km.ammo_of(self.p.state("A"), ARROWS)
            if arrows is not None and arrows < LOW_ARROWS:
                self.p.give("A", "bow")

    def boss_at(self):
        b = boss(self.p, "A")
        return None if dead(b) or not b.get("pos") else list(b["pos"])

    def draw_at(self, target):
        """Draw and aim the bow at `target` = (painting, moment, point, dy); None when aimed, else why not."""
        i, moment, point, dy = target
        aim = [point[0], point[1] + dy, point[2]]
        out = {"aim": {}, "frames": 0, "trace": []}
        self.cl.act("assign Bow")
        self.p.step_both(2)
        t = ph.turn_to(self.cl, aim[0], aim[2])
        self.frames += t["frames"] + 12 + km.DRAW_FRAMES
        if not t["turned"]:
            return "the turn: %s" % t["reason"]
        self.cl.input(None, ["ZL"], 2)
        self.p.step_both(8)
        self.cl.input(None, ["ZR"], km.DRAW_FRAMES)
        why = km.aim_view(self.cl, aim, out)
        if why:
            self.cl.input(None, ["ZR", "B"], 4)     # put away (kit_move.shoot's rule: no shot unaimed)
        self.ready = None if why else (i, moment, aim, dy)
        self.held = 0
        self.frames += out.get("frames") or 0
        return why

    def put_away(self):
        self.cl.input(None, ["ZR", "B"], 4)
        self.ready = None
        self.frames += 4

    def let_go(self, moment, b_at):
        i, want, aim, dy = self.ready
        life0 = {x: (boss(self.p, x) or {}).get("life") for x in self.p.c}
        reply = self.cl.act("aim %.2f %.2f %.2f" % tuple(aim))
        self.cl.input(None, ["ZR"], 2)
        self.cl.input(None, [], km.RELEASE_FRAMES)
        self.ready = None
        self.new_lines()
        mark = {x: len(self.lines[x]) for x in self.p.c}
        self.p.step_both(WATCH)
        self.frames += WATCH + 2 + km.RELEASE_FRAMES
        self.new_lines()
        after = {x: self.lines[x][mark[x]:] for x in self.p.c}
        life1 = {x: (boss(self.p, x) or {}).get("life") for x in self.p.c}
        anims = sorted({m.group(1) for x in after for m in map(ANIM.search, after[x]) if m})
        shot = {"n": len(self.shots) + 1, "painting": i, "moment": moment, "aim": [round(v, 2) for v in aim],
                "dy": dy, "boss_at_let_go": b_at, "aim_reply": (reply or {}).get("note"),
                "aim_line": next((m.group(0) for m in map(AIM_LINE.search, after["A"]) if m), None),
                "life_before": life0, "life_after": life1, "anims_after": anims,
                "hit": any(life0[x] is not None and life1[x] is not None and life1[x] < life0[x] for x in life0)
                or any(HIT_ANIM.search(a) for a in anims)}
        self.shots.append(shot)
        return shot

    def step(self):
        """One look: run CHUNK frames, learn places from A's 'Appear' / 'RideJumpAttack_Attack', let go when ready
        for this painting and moment, else draw for the next target. Returns the let-go shot or None."""
        self.run(CHUNK)
        lines = self.new_lines()
        for ln in lines["A"]:
            m = OUT_ANIM.search(ln)
            if not m:
                continue
            moment = "appear" if m.group(1) == "Appear" else "attack"
            at = self.boss_at()
            if at is None:
                continue
            if moment == "appear":
                i = painting_of(self.paintings, at)
                if i is None:
                    self.paintings.append({"appear": [round(v, 2) for v in at], "attack": None, "seen": 0})
                    i = len(self.paintings) - 1
                self.paintings[i]["seen"] += 1
                self.cur = i
            else:
                i = getattr(self, "cur", None)
                if i is not None and self.paintings[i].get("attack") is None:
                    self.paintings[i]["attack"] = [round(v, 2) for v in at]
            if self.ready and self.ready[0] == i and moment == self.ready[1]:
                return self.let_go(moment, [round(v, 2) for v in at])
        if self.ready and self.held >= HOLD_MAX:
            self.note("shot %d at painting %d %s: held %d frames, he did not come: put away" % (
                len(self.shots) + 1, self.ready[0], self.ready[1], self.held))
            self.put_away()
        elif self.ready and self.held % CHECK_EVERY < CHUNK:
            _, err = km._aim_error(self.cl, self.ready[2])
            if err is None or math.hypot(*err) > DRIFT:
                self.note("shot %d: the draw was lost or drifted (%s): drawn again" % (len(self.shots) + 1, err))
                self.put_away()
        if not self.ready and self.frames >= self.retry_at:
            t = next_target(self.paintings, len(self.shots) + len(self.rec["cases"][self.name]["notes"]))
            if t:
                i, moment = t
                dy = DY_LADDER[len(self.shots) % len(DY_LADDER)]
                why = self.draw_at((i, moment, self.paintings[i][moment], dy))
                if why:
                    self.retry_at = self.frames + RETRY_FRAMES
                    self.note("shot %d not aimed at painting %d %s: %s" % (len(self.shots) + 1, i, moment, why))
        return None


def _fresh_fight(pair):
    """Both adult in a fresh fight: the age flag, AGE_WAIT, the warp (map change event; inside the set-up, C #145),
    then the intro and the row's start outside it, then the checks (SetupFail)."""
    with pair.setup():
        for x in pair.c:
            pair.flag(x, AGE_FLAG, 0)
        pair.step_both(AGE_WAIT)
        warp_in(pair, SCENE)
    pair.step_both(INTRO_FRAMES)
    for x in pair.c:
        st = pair.state(x)
        if st.get("body") != "adult":
            raise P.SetupFail("%s's Link is %s (body), not adult, after the age flag and the warp" % (x, st.get("body")))
        r = pair.give(x, "bow")
        pair.notes.append("%s give bow: %s" % (x, (r or {}).get("result")))
    arrows = km.ammo_of(pair.state("A"), ARROWS)
    if not arrows:
        raise P.SetupFail("A has no arrows after 'give bow' (PouchData.%s.StockNum %s)" % (ARROWS, arrows))
    keep_up(pair)
    pair.step_both(ROW_START)
    if dead(boss(pair, "A")):
        raise P.SetupFail("no living %s in A's world list after the intro" % PG)


def _case1(pair, rec):
    h = _Hunt(pair, rec, "c1-full-life")
    out = None
    while h.frames < CASE1_FRAMES:
        h.step()
        p2 = h.p2
        if p2 or dead(boss(pair, "A")):
            out = "phase 2 after %d shots (%d hits): %s" % (len(h.shots), sum(s["hit"] for s in h.shots),
                                                          " | ".join(p2[:3]) or "he died")
            break
    if out is None:
        out = "no phase 2 in %d frames: %d shots, %d hits, %d paintings seen" % (
            h.frames, len(h.shots), sum(s["hit"] for s in h.shots), len(h.paintings))
    rec["cases"]["c1-full-life"]["result"] = out
    return out


def _case2(pair, rec):
    h = _Hunt(pair, rec, "c2-life1-then-arrow")
    lives = []
    for _ in range(6):
        b = boss(pair, "A")
        life = (b or {}).get("life") or 0
        lives.append(life)
        if life <= 1:
            break
        pair.act("A", "hit %s %d" % (PG, life - 1))
        pair.step_both(20)
        if len(lives) >= 3 and lives[-1] == lives[-2] == lives[-3]:
            break
    lives.append((boss(pair, "A") or {}).get("life"))
    rec["cases"]["c2-life1-then-arrow"]["test_hits_lives"] = lives
    hit = None
    while h.frames < CASE2_FRAMES and not hit:
        s = h.step()
        if s and s["hit"]:
            hit = s
    rec["cases"]["c2-life1-then-arrow"]["phase2_before_arrow_hit"] = list(h.p2)
    if not hit:
        out = "life-only hits %s; no arrow hit in %d frames (%d shots)" % (lives, h.frames, len(h.shots))
    else:
        mark = {x: len(h.lines[x]) for x in pair.c}
        for _ in range(FLY_WAIT // CHUNK):
            h.run(CHUNK)
            h.new_lines()
        after = {x: h.lines[x][mark[x]:] for x in pair.c}
        p2 = phase2(after["A"] + after["B"])
        stuck = [ln.strip() for x in after for ln in after[x] if STUCK.search(ln)][:4]
        anims = sorted({m.group(1) for x in after for m in map(ANIM.search, after[x]) if m})
        out = "life-only hits %s, arrow shot %d hit (life %s -> %s): %s" % (
            lives, hit["n"], hit["life_before"], hit["life_after"],
            ("PHASE 2: " + " | ".join(p2[:3])) if p2 else
            ("SOFT-LOCK? no phase-2 line in %d frames; anims %s; stuck lines %s" % (FLY_WAIT, anims, stuck)))
    rec["cases"]["c2-life1-then-arrow"]["result"] = out
    return out


@pytest.mark.budget(2700)       # ~2 x (3900 + 600) intro frames + 12000 + 9000 + 1800 at ~15 a second + the warps
@pytest.mark.needs_env(*PROBE_ENV)
@pytest.mark.needs_boot("live-phantom-ganon")
@pytest.mark.row("PROBE Phantom Ganon real arrow hits (row 287, B #173; not a row)")
def test_probe_pg_bow(pair):
    rec = {"cases": {}}
    res = []
    try:
        with pair.setup():
            if (pair.info or {}).get("fake"):
                pair.warp_both(SCENE)               # (the fake lane has no boot scripts)
            st = pair.need_boot_place()
            for x in ("A", "B"):
                assert st[x].get("scene") == SCENE, "%s is in %s, not %s (the boot live-phantom-ganon)" % (
                    x, st[x].get("scene"), SCENE)
        _fresh_fight(pair)
        res.append("c1: " + _case1(pair, rec))
        _fresh_fight(pair)
        res.append("c2: " + _case2(pair, rec))
    except P.SetupFail as e:
        rec["error"] = "SetupFail: %s" % e
        raise
    except Exception as e:                   # noqa: BLE001 (recorded; the JSON is the product)
        rec["error"] = "%s: %s" % (type(e).__name__, e)
    finally:
        with open(os.path.join(pair.out_dir or ".", "probe-pg-bow.json"), "w", encoding="utf-8") as f:
            json.dump(rec, f, indent=1)
    out = "; ".join(res) or "no case ran"
    if rec.get("error"):
        out += "; error " + rec["error"]
    print(out)
    pair.notes.append(out)
    pair.unproven("probe only (probe-pg-bow.json): " + out)
