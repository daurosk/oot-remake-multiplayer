"""Fake-game reactions for regress-port-b's tests (ingame/test_regress_bosses.py, ingame/test_enemy_turn.py; owner
regress-port-b). Hooks: fakegame.py docstring. Reacts ONLY when the selftest names one of the three boots in
BT_FAKE_REGRESS_BOOT (the fake lane does not know --boot): the games then start in that boot's stage, past it, as the
real boot leaves them (each line copied from the tests' own patterns, cited there at their writers).

Every boot: the first state reads say not_ready (no Link sample) until 5 frames ran (review-rows-c136 16:25).
live-barinade-room: both games' boot lines (sync on, Barinade 'ours' on A, 'a copy' on B, B's "owner's verbs came"),
  Barinade in both world lists (owner 1), a boss counts line every 300 frames (A sends, B puts in).
  BT_FAKE_REGRESS_MODE=off: 'native sync off' and no boss lines (a hookshot.exe without the wraps: SETUP-FAIL).
  =copy: B's copy never takes the owner's verbs (no verbs line, nothing put in): FAIL.
live-ganondorf-room: B owns Ganondorf (B entered first). Counting each game's swings (input with Y): B's 40th swing
  says its rally ball line, A's 10th the ball kept here; B's 120th Parry_Ready then LightBallDamage_Start (ours); A's
  anim LightBallDamage_Start right after. =noreflect: no Parry_Ready (NO-REF). =nostun: A never goes down (FAIL).
  =noball: no rally line (FAIL).
live-stalfos-weak-room: two Enemy_Stalfos_Weak owned by player 1 (A); A's `hit <id> Enemy_Stalfos_Weak 1` begins the
  fight in both games; 450 frames later (15 s) both lists name player 2. =stay: never (FAIL). =back: back to player 1
  90 frames after the turn (FAIL: a quick hand-back).
"""
import os

STAGES = {"live-barinade-room": "BarinadeStage", "live-ganondorf-room": "GanondorfStage",
          "live-stalfos-weak-room": "StalfosWeakStage"}
BARINADE, GANONDORF, STALFOS = "Enemy_DungeonBoss_Barinade", "Enemy_DungeonBoss_Ganondorf", "Enemy_Stalfos_Weak"
TURN, BACK = 450, 90


def _boot():
    return os.environ.get("BT_FAKE_REGRESS_BOOT")


def _mode():
    return os.environ.get("BT_FAKE_REGRESS_MODE", "")


def _counts(game):
    c = game.rp_counts
    game.say("coop: bosses: ours sent %d brain + %d action verbs (0 by plug); copies: 0 own skipped, %d brain + %d "
             "action verbs put in (0 waited for a node; 0 answered a busy slot, 0 of them asked again next frame)"
             % tuple(c))


def _tick(game):
    if game.tag == "A":
        game.rp_counts[0] += 1
        game.rp_counts[1] += 3
    elif _mode() != "copy":
        game.rp_counts[2] += 1
        game.rp_counts[3] += 3
    _counts(game)
    game.later(300, lambda: _tick(game))


def setup(game):
    boot = _boot()
    game.rp_on = boot in STAGES
    if not game.rp_on:
        return
    game.scene = STAGES[boot]
    game.rp_ready = game.frame + 5
    game.flags["PlayerStatus.MaxLife"] = 48
    game.rp_counts, game.rp_swings, game.rp_fight, game.rp_down = [0, 0, 0, 0], 0, None, False
    if boot == "live-barinade-room":
        if _mode() == "off":
            game.say("coop: bosses: native sync off (brain verbs not wrapped in this hookshot.exe, action verbs not "
                     "wrapped): bosses as before (owner's animations, gohma-sync)")
            return
        game.say("coop: bosses: native sync on (brain verbs wrapped, action verbs wrapped)")
        game.say("coop: bosses: %s: native boss sync on (BSA 0x51a0, %s)" % (BARINADE, "ours" if game.tag == "A" else "a copy"))
        if game.tag == "B" and _mode() != "copy":
            game.say("coop: bosses: %s: the owner's verbs came (brain 3): the copy's own brain is skipped from now" % BARINADE)
        game.later(300, lambda: _tick(game))
    elif boot == "live-ganondorf-room":
        game.say("coop: bosses: native sync on (brain verbs wrapped, action verbs wrapped)")
        game.say("coop: bosses: %s: native boss sync on (BSA 0x6230, %s)" % (GANONDORF, "ours" if game.tag == "B" else "a copy"))


def state(game, st):
    if not getattr(game, "rp_on", False):
        return
    if game.frame < game.rp_ready:     # held after the boot: no Link sample until a frame runs (as the real lane)
        st.update(pos=None, loading=None, event=None, not_ready="no fresh Link sample")
    me = 1 if game.tag == "A" else 2
    for p in st["coop"]["players"]:
        p["self"] = p["number"] == me


def _stalfos_owner(game):
    if game.rp_fight is None:
        return 1
    t = game.frame - game.rp_fight
    if _mode() == "stay" or t < TURN:
        return 1
    if _mode() == "back" and t >= TURN + BACK:
        return 1
    return 2


def actors(game):
    if not getattr(game, "rp_on", False):
        return []
    boot = _boot()
    if boot == "live-barinade-room" and _mode() != "off":
        return [{"id": 300, "name": BARINADE, "kind": "enemy", "pos": [42.4, 102.0, -88.0], "life": 20, "alive": True,
                 "owner": 1}]
    if boot == "live-ganondorf-room":
        return [{"id": 310, "name": GANONDORF, "kind": "enemy", "pos": [0.0, 171.0, 0.0], "life": 40, "alive": True,
                 "owner": 2}]
    if boot == "live-stalfos-weak-room":
        base = 100 if game.tag == "A" else 200
        o = _stalfos_owner(game)
        return [{"id": base + 1, "name": STALFOS, "kind": "enemy", "pos": [2.0, 105.5, -40.0], "life": 8, "alive": True,
                 "owner": o},
                {"id": base + 2, "name": STALFOS, "kind": "enemy", "pos": [-2.0, 105.5, -40.0], "life": 8,
                 "alive": True, "owner": o}]
    return []


def act(game, w):
    if getattr(game, "rp_on", False) and _boot() == "live-stalfos-weak-room" and w[:1] == ["hit"] and STALFOS in w:
        if game.rp_fight is None:
            game.rp_fight, game.other.rp_fight = game.frame, game.other.frame
        return True
    return False


def press(game, buttons):
    if not (getattr(game, "rp_on", False) and _boot() == "live-ganondorf-room" and "Y" in buttons):
        return False
    game.rp_swings += 1
    n, mode = game.rp_swings, _mode()
    if game.tag == "B":
        if n == 40 and mode != "noball":
            game.say("coop: objects: share %s: its own rally ball flies here: its attack goes on, no boss turn (row 211)"
                     % GANONDORF)
        if n == 120 and mode != "noreflect":
            game.say("coop: objects: anim %s 0x6230 'Parry_Ready' (ours)" % GANONDORF)
            game.say("coop: objects: anim %s 0x6230 'LightBallDamage_Start' (ours)" % GANONDORF)
            if mode != "nostun":
                o = game.other
                o.later(20, lambda: o.say("coop: objects: anim %s 0x7110 'LightBallDamage_Start'" % GANONDORF))
    elif n == 10:
        game.say("coop: shots: enemy shot DungeonBossGanondrof_Bullet_LightBall at 0.10 172.00 -3.00: kept here (a "
                 "split shot: each game's own, never sent)")
    return True
