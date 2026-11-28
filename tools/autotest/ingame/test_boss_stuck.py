"""Row 303 (owner 2026-11-27: "There are still bugs with bosses like gohma getting stuck") as a bridge test: a whole
two-player Queen Gohma fight, watched for her getting stuck, on 0.12.x (boss turns, carried move state, orders).

| Test | Proves |
|---|---|
| test_gohma_fight_not_stuck | A walks in first and owns her and stands aside; B walks in after, under her ceiling spot, looks up and targets her (she must leave the ceiling as for a solo player: 'ceiling'; if not, A finds her so the fight goes on); both shoot her eye with the slingshot in turn (each game's own shot, a real player's aim: kit_move.shoot) and slash her while she is down; she is sampled in BOTH world lists every second the whole fight. Then she is killed and the death, Gohma_DefeatBoss and the warp must come in both games |

"Stuck" is judged from game state only (both world lists, each game's module log), as each kind a player can see:
- CEILING: B (not her owner) looked up and targeted her from under her ceiling spot and she did not leave the ceiling
  (her y in both lists stays >= CEILING_Y) within FIND_FRAMES: her pre-battle wait ('Pre_Battle_Ceiling_Wait') ends
  only for a Link her own game's AI perceives (lane1 hs-gohma-downed 20261120-085401 A: 'ours held
  'Pre_Battle_Ceiling_Wait' ... stuck' 3 of 3, then the same at life 1, with B under her).
- STILL: her place in neither game moved STILL_M in STILL_S of game while her life is above 0 (outside her intro and
  death): a frozen AI, a body held on the ceiling or in one move.
- APART: the two games show her more than APART_M apart for APART_S of game: a copy stuck while its owner fights on
  (or the other way round).
- GONE: a game's world list lost her while the other still has her alive, for GONE_S.
- The module's own stuck lines (coop_boss_native.cpp): 'ours held ... stuck', 'fell ... below its fight start',
  'its AI did not run its body as a copy'. These are noted (they are the module's own recovery), and FAIL when the
  same game logs one for her more than STALL_LINES_MAX times (its recovery did not get her out).
- END: no death line, no Gohma_DefeatBoss or a live Gohma in a game after the kill: stuck in her death.
Every sample is written to <out>/gohma-fight.json (both games' place, life, owner per second) and her boss lines of
each game to <out>/gohma-boss-<X>.log, so a FAIL names the moment and the module's lines around it.

Sources (worker-rules "no guessed values"):
- Boot, scene, places, gotos, waits, the trigger and the look up: test_intro_seen.py / test_boss_away.py (imported,
  never copied): scenarios/live-deku-gohma.A/B.txt (her set alive, the quest at GoToBossRoom, read back here).
- Slingshot: 'give PouchData.Weapon_Range_SlingShot.StockNum' + 'assign SlingShot' (scenarios/deku-ladder.A.txt:43-45);
  the aim and shot: kit_move.shoot (docs/playtest.md `shoot item`), stepped in lockstep with the other game (Lockstep).
- Hearts: test_boss_respawn.give_hearts (ten containers, life 56, read back), so neither Link goes down in the fight.
- Kill: three 'kill Enemy_DungeonBoss_Gohma' 150/120/120 frames apart (hs-gohma-kill.inc.txt via test_boss_away.KILLS).
- Death line: test_boss_away.DEATH (coop_boss_scenes.cpp:458); owner lines: coop_boss_native.cpp:393 ('now ours' /
  'now a copy'), :464, :666, :1284/:1292 (the stuck lines above).
- Thresholds: STILL_S 20 s is below the module's own stall check (kStallSteps 1500 = 25 s, coop_boss_native.h) so a
  freeze the module never sees is caught too, and above her longest still move read in the game files (her stun
  ChanceTime, about 8 s, docs/boss-multiplayer.md Gohma: claw, jump, crawl). APART_M 5 m: the drift a copy may show
  (docs/dungeons-boss.md: the mover snaps a copy more than 2 m off). These are probe bounds; the report cites the
  measured values of the run next to them.
"""
import json
import math
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import pair as P                                    # noqa: E402
import kit_move                                     # noqa: E402  (shoot: the player's aim)
import test_boss_respawn as BR                      # noqa: E402  (give_hearts)
from test_enemies import actors, alive, face, place, walk_to   # noqa: E402
from test_boss_away import (BOOT, CORRIDOR, DEATH, DEFEAT, DOOR, ENV, GOHMA, KILLS, QUEST,   # noqa: E402
                            ALIVE_STEP)
import boss_lines as BL                             # noqa: E402  (the native boss table's lines: one table)
from test_intro_seen import (A_MOVED, B_MOVED, CORRIDOR_WAIT, INTRO, LOOK_UP, SYNC, TRIGGER, UNDER)  # noqa: E402

SLING_GIVE = "PouchData.Weapon_Range_SlingShot.StockNum"   # deku-ladder.A.txt:43
SAMPLE_EVERY = 60                    # frames between samples (one second of game)
ROUNDS = 6                           # stun rounds: A, B, A, B, A, B
ROUND_IDLE = 900                     # frames she fights between rounds (15 s: crawl, climb, eggs, claw)
SLASH_N, SLASH_HOLD, SLASH_WAIT = 4, 8, 30   # four sword slashes (Y) per stun
SLASH_FROM_M = 2.5                   # the slasher is put this far from her (on the line to its own Link)
STILL_M, STILL_S = 0.3, 20.0
APART_M, APART_S = 5.0, 10.0
GONE_S = 10.0
STALL_LINES_MAX = 3
# Her ceiling spot: 'objects: + Enemy_DungeonBoss_Gohma ... at 18.49 59.97 67.79' (lane1 hs-gohma-downed 20261120-085401
# A :6917, also fakes/boss_away.py); the room's floor y 47.0 (coop_boss.cpp kRooms Gohma start 6.7 47.0 67.6). Left the
# ceiling = her y in a world list below CEILING_Y (5 m under the spot).
CEILING_Y = 55.0
FIND_EXTRA = 900                     # frames watched after the trigger's own wait (TRIGGER 900: a solo find drops her in it)
FIND_FRAMES = TRIGGER                # a find that drops her later than the solo trigger's wait is 'ceiling' too
DEATH_WAIT = 120 + 2400              # test_boss_away (B 'wait 120; mark b-kill; wait 2400')
WARP_WAIT = 600                      # test_boss_away ROOM_WAIT

# Her lines: the native boss table's notes of her (boss_lines.py: 'coop: bosses: <boss>: ...'; 'coop: boss: <boss>' matched
# none, so the stuck lines below were never seen) and the row 323 probe's (coop_brain_state.cpp 'probe 323: Gohma ...').
BOSS_LINES = (BL.PREFIX + GOHMA + ": ", BL.PREFIX + "probe 323: Gohma ")
STUCK_LINES = [r"%s: ours held '.*' stuck" % GOHMA, r"%s: fell \d+ m below its fight start" % GOHMA,
               r"%s: its AI did not run its body as a copy" % GOHMA]
OWNER_LINE = r"%s: now (ours|a copy)" % GOHMA

FIGHT_FRAMES = (ROUNDS * (ROUND_IDLE + 1700 + SLASH_N * (SLASH_HOLD + SLASH_WAIT)))
BUDGET_S = int(2 * (8000 + FIGHT_FRAMES + sum(KILLS) + DEATH_WAIT + WARP_WAIT) / P.FPS + 120)


class Lockstep:
    """Game x's bridge client for kit_move, with every frame it runs also run by the other game (the pair stays in
    step: a held other game would read as paused to the server and change the boss's turns)."""

    def __init__(self, pair, x):
        self._pair, self._x = pair, x
        self._c = pair.c[x]

    def _others(self, frames):
        for y in self._pair.c:
            if y != self._x and frames:
                self._pair.call(y, "step", frames=int(frames))

    def state(self):
        return self._c.state()

    def world(self):
        return self._c.world()

    def act(self, line):
        return self._c.act(line)

    def step(self, frames=1):
        r = self._c.step(frames)
        self._others(frames)
        return r

    def input(self, stick=None, buttons=(), frames=1, rstick=None):
        r = self._c.input(stick, buttons, frames, rstick=rstick)
        self._others(frames)
        return r

    def move(self, yaw, strength=1.0, frames=1, check=True):
        r = self._c.move(yaw, strength, frames, check=check)
        self._others(frames)
        return r


def her(pair, x):
    """Game x's Gohma (alive, with a place) in its world list, or None."""
    g = alive(actors(pair, x, GOHMA))
    return g[0] if g else None


def dist(p, q):
    return math.sqrt(sum((p[i] - q[i]) ** 2 for i in range(3)))


class Watch:
    """Both games' Gohma every SAMPLE_EVERY frames: the timeline and the stuck judgements."""

    def __init__(self, pair):
        self.pair = pair
        self.frame = 0
        self.rows = []
        self.found = []              # (kind, frame, text)
        self.since = {}              # kind -> first frame of the current stretch
        self.anchor = None           # the sample the 'still' stretch is measured from

    def sample(self, what=""):
        row = {"f": self.frame, "what": what}
        for x in ("A", "B"):
            g = her(self.pair, x)
            row[x] = None if g is None else {"pos": g.get("pos"), "life": g.get("life"), "owner": g.get("owner")}
        self.rows.append(row)
        self._judge(row)

    def run(self, frames, what=""):
        left = int(frames)
        while left > 0:
            n = min(SAMPLE_EVERY, left)
            self.pair.step_both(n)
            self.frame += n
            left -= n
            self.sample(what)

    def add(self, frames):
        """Frames a kit_move call ran (both games, Lockstep)."""
        self.frame += int(frames)
        self.sample("after shot")

    def _stretch(self, kind, on, text):
        if not on:
            self.since.pop(kind, None)
            return
        start = self.since.setdefault(kind, self.frame)
        limit = {"still": STILL_S, "apart": APART_S, "gone-A": GONE_S, "gone-B": GONE_S}[kind]
        if (self.frame - start) / P.FPS >= limit and not any(k == kind and f == start for k, f, _ in self.found):
            self.found.append((kind, start, text))

    def _judge(self, row):
        a, b = row["A"], row["B"]
        live = [g for g in (a, b) if g and isinstance(g.get("life"), int) and g["life"] > 0]
        # still: no game's place moved STILL_M from the anchor (the stretch's first sample) in STILL_S
        judged = live and not row["what"].startswith(("kill", "after the death", "B under her", "B looked up",
                                                      "A looked up"))
        anchor = self.anchor if judged else None
        moved = anchor is None or any(
            row[x] and anchor.get(x) and dist(row[x]["pos"], anchor[x]["pos"]) > STILL_M for x in ("A", "B"))
        if not judged:
            self.anchor = None
            self.since.pop("still", None)
        elif moved:
            self.anchor = row
            self.since["still"] = self.frame
        else:
            self._stretch("still", True, "her place moved under %.1f m in both games (A %s, B %s, life %s)" % (
                STILL_M, a and a["pos"], b and b["pos"], [g.get("life") for g in live]))
        if a and b:
            d = dist(a["pos"], b["pos"])
            self._stretch("apart", d > APART_M, "A shows her at %s, B at %s (%.1f m apart)" % (a["pos"], b["pos"], d))
        for x, y in (("A", "B"), ("B", "A")):
            mine, theirs = row[x], row[y]
            self._stretch("gone-" + x, mine is None and theirs is not None and (theirs.get("life") or 0) > 0,
                          "%s's world list has no live Gohma while %s's has her (life %s)" % (
                              x, y, theirs and theirs.get("life")))

    def write(self, out_dir):
        if not out_dir:
            return
        os.makedirs(out_dir, exist_ok=True)
        with open(os.path.join(out_dir, "gohma-fight.json"), "w") as f:
            json.dump({"rows": self.rows, "found": self.found}, f, indent=0)


def dropped(watch, since):
    """Frames from `since` to the first sample (at or after it) where a game's world list has her below CEILING_Y."""
    for r in watch.rows:
        if r["f"] < since:
            continue
        if any(r[x] and r[x]["pos"] and r[x]["pos"][1] < CEILING_Y for x in ("A", "B")):
            return r["f"] - since
    return None


def arm(pair):
    with pair.setup():
        BR.give_hearts(pair)
        for x in ("A", "B"):
            r = pair.give(x, SLING_GIVE)
            assert "failed" not in str(r.get("result", "")), "%s: give %s: %s" % (x, SLING_GIVE, r)


def stun_round(pair, watch, x):
    """Game x shoots her eye with the slingshot (its own shot), then, if it hit her or she changed, slashes her."""
    g = her(pair, x)
    if g is None:
        pair.notes.append("round %s: no live Gohma in %s's list" % (x, x))
        return None
    before = pair.c[x].last_frame or 0
    out = kit_move.shoot(Lockstep(pair, x), "slingshot", ident=g.get("id"), name=GOHMA)
    ran = max(0, (pair.c[x].last_frame or before) - before)
    watch.add(ran)
    res = out.get("result")
    pair.notes.append("round %s @%d: slingshot %s (aim %s)" % (x, watch.frame, res, (out.get("aim") or {}).get("error")))
    if res not in ("hit", "changed"):
        return res
    g = her(pair, x)
    if g is None:
        return res
    me = pair.state(x).get("pos")
    if me:
        d = dist(me, g["pos"]) or 1.0
        at = [g["pos"][i] + (me[i] - g["pos"][i]) * SLASH_FROM_M / d for i in range(3)]
        at[1] = me[1]
        place(pair, x, at)
        face(pair, x, g["pos"])
    for _ in range(SLASH_N):
        pair.press(x, ["Y"], frames=SLASH_HOLD)
        watch.run(SLASH_WAIT, "slash %s" % x)
    return res


def boss_lines(pair, x, start):
    return [ln for ln in pair.log[x].since(start[x]) if any(k in ln for k in BOSS_LINES)]


@pytest.mark.budget(BUDGET_S)
@pytest.mark.look
@pytest.mark.needs_boot(BOOT)
@pytest.mark.needs_env(*ENV)
@pytest.mark.row("#303 a boss (Queen Gohma) getting stuck in a two-player fight")
def test_gohma_fight_not_stuck(pair):
    if any(pair.log[x].find(DEATH, pos=0) for x in ("A", "B")):
        pytest.skip("Gohma died earlier on this boot: the test needs her alive")
    with pair.setup():
        for x in ("A", "B"):
            s = pair.state(x)
            assert pair.in_play(s), "%s not in play (%s)" % (x, s.get("not_ready"))
            q = pair.flag_read(x, QUEST).get("value")
            assert str(q).lower() == ALIVE_STEP, "%s: Get_StoneOfForest is %s, not GoToBossRoom" % (x, q)
            assert not pair.flag_read(x, DEFEAT).get("value"), "%s: %s set before the fight" % (x, DEFEAT)
        place(pair, "A", CORRIDOR)
    arm(pair)
    start = {x: pair.log[x].mark() for x in ("A", "B")}
    # A walks in first (her owner: the first in her room) and stays 6.5 m west of her ceiling spot, never looking up
    pair.step_both(CORRIDOR_WAIT)
    for g in DOOR:
        walk_to(pair, "A", g[0], g[1], tol=g[2], max_frames=g[3])
    pair.step_both(INTRO)
    walk_to(pair, "A", A_MOVED[0], A_MOVED[1], tol=A_MOVED[2], max_frames=A_MOVED[3])
    if not pair.log["A"].find(SYNC % "ours", pos=0):
        pair.unproven("A does not own Gohma after its walk-in (no 'native boss sync on (..., ours)')")
    # B walks in, under her ceiling spot, looks up and targets her (test_intro_seen's trigger, by B now)
    place(pair, "B", CORRIDOR)
    pair.step_both(CORRIDOR_WAIT)
    for g in DOOR:
        walk_to(pair, "B", g[0], g[1], tol=g[2], max_frames=g[3])
    pair.step_both(INTRO)
    walk_to(pair, "B", UNDER[0], UNDER[1], tol=UNDER[2], max_frames=UNDER[3])
    if not her(pair, "A") or not her(pair, "B"):
        pair.unproven("no live Gohma in both lists after both walked in (A %s, B %s)" % (her(pair, "A"), her(pair, "B")))
    for x in ("A", "B"):
        pair.check_shot(x, "gohma-stuck-ceiling", tolerance=0.80)
    watch = Watch(pair)
    watch.sample("B under her")
    pair.press("B", rstick=LOOK_UP[0], frames=LOOK_UP[1])
    pair.press("B", buttons=["ZL"], frames=LOOK_UP[2])
    t0 = watch.frame
    watch.frame += LOOK_UP[1] + LOOK_UP[2]
    watch.run(TRIGGER + FIND_EXTRA, "B looked up")      # (not judged 'still': she waits for a finder there)
    found = dropped(watch, t0)
    pair.notes.append("B (her copy's player) looked up under her: %s" % (
        "she left the ceiling %.1f s after the look-up" % (found / P.FPS) if found is not None else
        "she stayed on the ceiling %.0f s (both lists y >= %.0f)" % ((TRIGGER + FIND_EXTRA) / P.FPS, CEILING_Y)))
    for x in ("A", "B"):
        pair.check_shot(x, "gohma-stuck-found", tolerance=0.80)
    if found is None or found > FIND_FRAMES:
        watch.found.append(("ceiling", 0, "B looked up and targeted her from under her ceiling spot (A, her owner, "
                                          "6.5 m away, not looking): she %s" % (
                                              "stayed on the ceiling" if found is None else
                                              "left it only %.0f s later" % (found / P.FPS))))
        if found is None:                      # the fight goes on as the old trigger: A finds her
            walk_to(pair, "A", UNDER[0] - 2.0, UNDER[1], tol=UNDER[2], max_frames=UNDER[3])
            pair.press("A", rstick=LOOK_UP[0], frames=LOOK_UP[1])
            pair.press("A", buttons=["ZL"], frames=LOOK_UP[2])
            t1 = watch.frame
            watch.frame += LOOK_UP[1] + LOOK_UP[2]
            watch.run(TRIGGER, "A looked up")
            pair.notes.append("A looked up after: %s" % ("she left the ceiling" if dropped(watch, t1) is not None else
                                                       "she stayed on the ceiling"))
    walk_to(pair, "B", B_MOVED[0], B_MOVED[1], tol=B_MOVED[2], max_frames=B_MOVED[3])
    watch.sample("start")
    for i in range(ROUNDS):
        x = "A" if i % 2 == 0 else "B"
        stun_round(pair, watch, x)
        watch.run(ROUND_IDLE, "fight after %s's round %d" % (x, i + 1))
        for y in ("A", "B"):
            pair.check_shot(y, "gohma-stuck-r%d" % (i + 1), tolerance=0.80)

    # her death: three kills in A's game (hs-gohma-kill), then the death, the set and the warp in both
    for w in KILLS:
        watch.run(w, "kill")
        pair.act("A", "kill %s" % GOHMA)
    died = {}
    for x in ("A", "B"):
        try:
            died[x] = bool(pair.expect_log(x, DEATH, max_frames=DEATH_WAIT))
        except AssertionError:
            died[x] = False
    watch.run(WARP_WAIT, "after the death")
    watch.write(pair.out_dir)
    lines = {x: boss_lines(pair, x, start) for x in ("A", "B")}
    if pair.out_dir:
        for x in ("A", "B"):
            with open(os.path.join(pair.out_dir, "gohma-boss-%s.log" % x), "w", encoding="utf-8") as f:
                f.write("\n".join(lines[x]) + "\n")
    for x in ("A", "B"):
        own = sum(1 for ln in lines[x] if re.search(OWNER_LINE, ln))
        stuck = [ln for ln in lines[x] if any(re.search(p, ln) for p in STUCK_LINES)]
        pair.notes.append("%s: %d boss lines, %d owner changes, %d stuck lines%s" % (
            x, len(lines[x]), own, len(stuck), (": " + stuck[0].strip()) if stuck else ""))
        watch.found.extend(("module-" + x, 0, ln.strip()) for ln in stuck[STALL_LINES_MAX:STALL_LINES_MAX + 1])
    for x in ("A", "B"):
        pair.check_shot(x, "gohma-stuck-end", tolerance=0.80)
    for kind, f, text in watch.found:
        pair.notes.append("STUCK %s from frame %d (%.0f s into the fight): %s" % (kind, f, f / P.FPS, text))

    assert not watch.found, "Gohma stuck: %s" % "; ".join("%s @%.0f s: %s" % (k, f / P.FPS, t)
                                                         for k, f, t in watch.found)
    for x in ("A", "B"):
        assert died[x], "%s: no '%s' after the kill: her death did not play there" % (x, DEATH)
        assert pair.flag_read(x, DEFEAT).get("value"), "%s: %s not 1 after her death" % (x, DEFEAT)
        assert not her(pair, x), "%s: Gohma still alive after her death" % x
