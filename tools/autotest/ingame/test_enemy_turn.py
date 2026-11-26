"""Regress vs 0.11.12, step 9 (reports/rc-release-0120.md section 4; old step: coord offline/regress-01113.ps1 step 9 =
regress-01113-steps/s9-enemy-switch.ps1, owner call O4 "enemy switch KEPT, must be proven", reports/regress-01113.md:27):
"an enemy fights A, then switches to B" as a bridge test.

| Test | Old step | Boot | Proves |
|---|---|---|---|
| test_stalfos_turns_to_the_other_player | s9-enemy-switch (O4) | live-stalfos-weak-room | a big melee enemy the owner game fights passes to the other player near it at the end of an attack (#308 part 2), stays there, no raw life write |

Why not the old enemy (s9-enemy-switch.A/B.txt: a Deku Baba in InsideTheDekuTree): the O4 rule is for the kinds the hook
lists only (lichii/hook/coop_objects_sync.cpp TurnsAsEnemy: Moblin, Lizalfos, Dinolfos, Stalfos, Like Like, Iron
Knuckle; tests/lichii/objects_test.cpp "ai-turns: TurnsAsEnemy lists the big melee kinds only"); every other plain
enemy keeps the player it fights (server/session.hpp enemy_turns comment), and two players at one spot never flip it by a
hit (docs/objects.md "A hit can take it": 10 m nearer). So the old run could never show the switch it was named for
(its own NotProven: "no log line known for the owner change; shots decide"). This test takes a listed kind in a room a
boot reaches, and judges the owner from each game's world list (docs/playtest.md "world": owner = a player number).
Not covered by port-g3-save's G3b (row 138 (2)(7), enemy-owner-0115-probe.ps1: King Dodongo's reaction to a copy's hit,
a boss, no turn rule) nor test_enemies.py (rows 230-232: Redead, Meg, Dead Hand).

Sources (worker-rules "no guessed values"):
- The room: romfs Banc/DreamWorld/StalfosWeakStage_Static.bcett.byml.zs places Enemy_Stalfos_Weak_77dd and
  Enemy_Stalfos_Weak_ff21 (read 2026-11-26 from the lane's dump, zstd + the romfs's own bcett.byml.zsdic); start point
  lichii/hook/coop_start_points.inc:119 {"StalfosWeakStage", "StalfosWeakStage", -0.01, 105.50, -51.54}. A boss rush
  stage: reached by the boot (scenarios/live-stalfos-weak-room.A/B.txt) with the rush's arrival event
  DreamBossStage_Rematch_Arrive (an RSDB EventSetting row; live-meg-room asks the same one: a bare warp places no rush
  enemy, hs-meg-decoys-231 run 1). Enemy_Stalfos_Weak: TurnsAsEnemy, not a boss (objects_test.cpp ai-turns).
- The rule (server/session.cpp next_turn + session.hpp): a turn only in a fight (in_fight: a life change or the owner's
  targeting report, 12 s on), at least enemy_turn_gap_us = 15 s after the fight began or the last owner change, right
  at the owner's attack end (boss_turn_edge_us), 3 s after the last hit (boss_turn_quiet_us), owner and the next
  player both up and within enemy_turn_m = 12 m, and to the near player owning fewer fighting enemies than the owner.
  Then no hand-back for handover_back_us = 15 s (docs/objects.md "No quick hand-back").
- Places: the owner's Link OWNER_GAP m from the Stalfos, the other's Link OTHER_GAP m (both inside 12 m), on the line
  from the Stalfos toward the stage's start point (its own pos from the owner's world list, never a typed one).
- One hit to begin the fight: the bridge's `act hit <id> Enemy_Stalfos_Weak 1` (docs/playtest.md act: hit takes an
  optional actor id), the owner game's world id. After it nobody hits (a hit within 3 s holds the turn).
- Wait: TURN_WAIT frames at most (the 15 s gap + attacks of a Stalfos; 80 s of game, pair.FPS 30); ESTIMATED margin,
  no old run measured it. Life refilled to the max each chunk (the game's own setter, 'setflag PlayerStatus.Life';
  test_boss_respawn.py give_hearts' flags): a downed Link is 'busy' and holds every turn.
- "Must not": 'life written' (s9-enemy-switch.ps1 -Not; coop_objects.cpp:4186 "its Life step did not take the change:
  life written"). Switch from the old A header: LICHII_COOP_DEBUG_OBJECTS=1 (the boot's), CHEATS for `hit` (the gate).
- Shots (user-facing: which Link the Stalfos fights, on each game): before the turn and after it, two frames apart
  each, compared to refs/ at 0.80 (a scene with moving things, docs/ingame-tests.md step 6). NO-REF until approved.
"""
import math
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pair as P   # noqa: E402

BOOT = "live-stalfos-weak-room"
STAGE = "StalfosWeakStage"
START = (-0.01, 105.50, -51.54)    # coop_start_points.inc:119
STALFOS = "Enemy_Stalfos_Weak"     # romfs StalfosWeakStage_Static; coop_objects_sync.cpp TurnsAsEnemy
SWITCHES = ("LICHII_COOP_DEBUG_OBJECTS=1", "LICHII_COOP_CHEATS=1")   # the boot's '#! switches'
OWNER_GAP, OTHER_GAP = 2.5, 6.0    # m from the Stalfos: both inside enemy_turn_m (12 m, session.hpp)
TURN_WAIT = 2400                   # frames (module docstring: ESTIMATED)
HOLD = 150                         # frames the new owner is checked after the turn (5 s; hand-back held 15 s)
CHUNK = 30                         # frames between owner reads (1 s of game)
LIFE_WRITTEN = r"its Life step did not take the change: life written"   # coop_objects.cpp:4186
SHOT_TOLERANCE = 0.80
READY_FRAMES = 300                 # harness-c133-scene 82f9a7a5a test_rows READY_FRAMES (need_boot_place)


def other(x):
    return "B" if x == "A" else "A"


def own_number(st):
    """The state's coop.players entry with self true (docs/playtest.md state), its number; None if none."""
    for p in ((st.get("coop") or {}).get("players") or []):
        if p.get("self") is True:
            return p.get("number")
    return None


def in_stage(pair, stage):
    """Both games in play in the boot's `stage`. The games are held right after the lane boot with no Link sample yet
    (state not_ready 'no fresh Link sample': gifts-237-3d17b642 SETUP-FAIL 16:20 UTC, review-rows-c136 16:25), so both
    are stepped until each has a fresh in-play sample (READY_FRAMES, the bound harness-c133-scene 82f9a7a5a gives
    pair.until_in_play), then the scene is checked. Shared by test_regress_bosses.py."""
    pair.until("both games in play (a fresh Link sample) after the boot",
               lambda: all(pair.in_play(pair.state(x)) for x in ("A", "B")), max_frames=READY_FRAMES)
    for x in ("A", "B"):
        s = pair.state(x)
        assert (s.get("scene") or "").lower() == stage.lower(), "%s is in %s, not %s (the boot's stage)" % (
            x, s.get("scene"), stage)


def stalfos(pair, x):
    """Game x's alive Enemy_Stalfos_Weak with a pos, from its whole world list (playtest_client.Client.world)."""
    w = pair.c[x].world().get("world") or {}
    return [a for a in w.get("actors", []) if a.get("name") == STALFOS and a.get("alive") is not False and a.get("pos")]


def keep_up(pair):
    """Both Links at full life (a downed Link is 'busy' and holds every turn): Life = MaxLife, the game's own setter."""
    for x in ("A", "B"):
        mx = pair.flag_read(x, "PlayerStatus.MaxLife").get("value")
        if isinstance(mx, int) and mx > 0:
            pair.flag(x, "PlayerStatus.Life", mx)


def owners(pair, sid_by_game):
    """{game: owner} of the Stalfos each game knows by its own id (None = not listed / unknown)."""
    out = {}
    for x in ("A", "B"):
        e = [a for a in stalfos(pair, x) if a.get("id") == sid_by_game[x]]
        out[x] = e[0].get("owner") if e else None
    return out


def nearest(acts, p):
    return min(acts, key=lambda a: math.hypot(a["pos"][0] - p[0], a["pos"][2] - p[2]))


def look_both(pair, name, missed):
    """Paired looks a couple of frames apart on each game (check_shot); a miss is kept and asserted last."""
    for i in (1, 2):
        for x in ("A", "B"):
            try:
                pair.check_shot(x, "%s-%d" % (name, i), tolerance=SHOT_TOLERANCE)
            except AssertionError as e:
                missed.append(str(e))
        pair.step_both(2)


@pytest.mark.needs_boot(BOOT)
@pytest.mark.budget(600)        # ESTIMATED, no old run: <= 2550 frames (85 s) + world reads, shots; the first lane run replaces it
@pytest.mark.look
@pytest.mark.needs_env(*SWITCHES)
@pytest.mark.row("O4 #308 an enemy fights one player, then switches to the other near it (regress-01113 step 9, s9-enemy-switch)")
def test_stalfos_turns_to_the_other_player(pair):
    missed = []
    with pair.setup():
        in_stage(pair, STAGE)
        num = {x: own_number(pair.state(x)) for x in ("A", "B")}
        assert all(isinstance(v, int) for v in num.values()) and num["A"] != num["B"], "own player numbers: %s" % num
        listed = pair.until("an alive %s with an owner in both games' world lists" % STALFOS,
                            lambda: (lambda l: l if all(any(isinstance(a.get("owner"), int) for a in l[x])
                                                        for x in l) else None)(
                                {x: stalfos(pair, x) for x in ("A", "B")}), max_frames=300)
        first = nearest(listed["A"], START)
        owner_num = first["owner"]
        owner = [x for x in ("A", "B") if num[x] == owner_num]
        assert owner, "%s's owner is player %s, neither game (%s)" % (STALFOS, owner_num, num)
        O = owner[0]
        N = other(O)
        s_o = nearest(listed[O], first["pos"])      # the same Stalfos in each game: the nearest to A's pick
        s_n = nearest(listed[N], s_o["pos"])
        assert s_n.get("owner") == num[O], "the games name different owners: %s says %s, %s says %s" % (
            O, s_o.get("owner"), N, s_n.get("owner"))
        sid = {O: s_o["id"], N: s_n["id"]}
        sp = s_o["pos"]
        dx, dz = START[0] - sp[0], START[2] - sp[2]
        d = math.hypot(dx, dz) or 1.0
        ux, uz = dx / d, dz / d
        for x, gap in ((O, OWNER_GAP), (N, OTHER_GAP)):
            pair.act(x, "place %.2f %.2f %.2f" % (sp[0] + ux * gap, sp[1], sp[2] + uz * gap))
            pair.act(x, "face %.2f %.2f" % (sp[0], sp[2]))
        keep_up(pair)
        pair.notes.append("owner %s (player %d), other %s (player %d); Stalfos at %s (ids %s)" % (
            O, num[O], N, num[N], ["%.2f" % v for v in sp], sid))
    pair.act(O, "hit %s %s 1" % (sid[O], STALFOS))   # the fight begins (a life change: in_fight)
    pair.step_both(CHUNK)
    look_both(pair, "enemy-turn-owner", missed)        # it fights the owner's Link
    ran, turned = 0, None
    while ran < TURN_WAIT:
        keep_up(pair)
        o = owners(pair, sid)
        if o[O] == num[N] and o[N] == num[N]:
            turned = ran
            break
        pair.step_both(CHUNK)
        ran += CHUNK
    o = owners(pair, sid)
    assert turned is not None, "no turn in %d frames: owner in %s's list %s, in %s's %s (want player %d, %s)" % (
        TURN_WAIT, O, o[O], N, o[N], num[N], N)
    pair.notes.append("turned to %s after %d frames (~%d s)" % (N, turned, turned // P.FPS))
    for x in ("A", "B"):
        e = [a for a in stalfos(pair, x) if a.get("id") == sid[x]]
        assert e and e[0].get("alive") is not False, "%s: the Stalfos is not alive in the list after the turn" % x
    look_both(pair, "enemy-turn-other", missed)        # it fights the other Link now
    held = 0
    while held < HOLD:                                  # no hand-back (15 s rule): it stays with the new owner
        keep_up(pair)
        pair.step_both(CHUNK)
        held += CHUNK
        o = owners(pair, sid)
        assert o[O] == num[N] and o[N] == num[N], "handed back within %d frames of the turn: %s" % (held, o)
    for x in ("A", "B"):
        pair.never_log(x, LIFE_WRITTEN)
    assert not missed, "%d look miss(es): %s" % (len(missed), "; ".join(missed[:4]))
