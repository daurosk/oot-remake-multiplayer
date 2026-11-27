"""Fake-game reactions for row 286's wake test (ingame/test_boss_wake.py; owner row-286-wake-3). Hooks: fakegame.py
docstring. Named to sort BEFORE boss_shots.py: its warp hook only notes the warp and never answers it, so a fake that
answers boss-stage warps (boss_shots.py) still does, and this one sees every warp.

Reacts only with LICHII_COOP_DEBUG_BOSS=1 and only in TwinrovaStage. On arrival (the first state after a warp there):
the arrival event for 300 frames (the intro; the state's 'event'), the sync notes (coop_boss_native.cpp:527, A ours,
B a copy), on the copy after 30 frames "the owner's verbs came" (:1512) and "battle orders: the owner's came"
(coop_boss_orders.cpp:188), the sisters in the world list (owner 1, alive), and every 300 frames a boss counts line
(coop_hookshot.cpp:601): A sends, B puts in.
BT_FAKE_BOSS_WAKE=late: a copy that comes in after the owner's intro ended (300 frames or more after it) never takes
the owner's fight (no verbs, no orders, no put-ins) and its sisters are not in its world list (the bug as reported).
=copy: the copy never takes it, also when both come in together. =server: B's world list names the server (OwnerJson
"server") as the sisters' owner. The wake tests must FAIL in those modes.
"""
import os

SWITCH = "LICHII_COOP_DEBUG_BOSS=1"
STAGE = "TwinrovaStage"
SISTERS = ("Enemy_DungeonBoss_Koume", "Enemy_DungeonBoss_Kotake")
INTRO = 300


def _on(game):
    return game.env_on(SWITCH)


def setup(game):
    game.awake_warp, game.awake_at, game.awake_ok, game.awake_gen = False, None, False, 0
    game.awake_counts = [0, 0, 0, 0]   # brain sent, action sent, brain put in, action put in
    # (live-twinrova-room is the lane's child save, as every boot: no age set here. It was set adult, after the
    # scenario's wrong 'adult save' header; the real boot is child, row215-309d7180 game-out-A.txt:4801/:8416)


def command(game, m):
    if _on(game) and m.get("cmd") == "warp":
        game.awake_warp = m.get("scene") == STAGE
        if not game.awake_warp:
            game.awake_at = None
    return None


def _counts_line(game):
    c = game.awake_counts
    game.say("coop: bosses: ours sent %d brain + %d action verbs (0 by plug); copies: 0 own skipped, %d brain + %d "
             "action verbs put in (0 waited for a node; 0 answered a busy slot, 0 of them asked again next frame)"
             % (c[0], c[1], c[2], c[3]))


def _tick(game, gen):
    if gen != game.awake_gen or game.scene != STAGE:
        return
    if game.tag == "A":
        game.awake_counts[0] += 1
        game.awake_counts[1] += 3
    elif game.awake_ok:
        game.awake_counts[2] += 1
        game.awake_counts[3] += 3
    else:
        return
    _counts_line(game)
    game.later(300, lambda: _tick(game, gen))


def _arrive(game):
    game.awake_warp, game.awake_at = False, game.frame
    game.awake_gen += 1
    gen = game.awake_gen
    for i, boss in enumerate(SISTERS):
        game.say("coop: bosses: %s: native boss sync on (BSA 0x%x, %s)" % (
            boss, 0x300 + i, "ours" if game.tag == "A" else "a copy"))
    mode = os.environ.get("BT_FAKE_BOSS_WAKE")
    o = game.other
    late = o is not None and o.awake_at is not None and game.awake_at - o.awake_at >= INTRO
    game.awake_ok = game.tag == "A" or not (mode == "copy" or (mode == "late" and late))
    if game.tag == "B" and game.awake_ok:
        def came():
            for boss in SISTERS:
                game.say("coop: bosses: %s: the owner's verbs came (brain 3): the copy's own brain is skipped from "
                         "now" % boss)
                game.say("coop: bosses: %s: battle orders: the owner's came: our own manager's orders are skipped"
                         % boss)
        game.later(30, came)
    game.later(60, lambda: _tick(game, gen))


def state(game, st):
    if not _on(game):
        return
    me = 1 if game.tag == "A" else 2       # (fakegame.py: A is player 1, B player 2)
    for p in (st.get("coop") or {}).get("players") or []:
        p.setdefault("self", p.get("number") == me)
    if game.awake_warp and game.scene == STAGE:
        _arrive(game)
    if game.scene != STAGE or game.awake_at is None:
        return
    if game.frame - game.awake_at < INTRO:
        st["event"] = True
    if game.awake_ok:
        server = game.tag == "B" and os.environ.get("BT_FAKE_BOSS_WAKE") == "server"
        for i, boss in enumerate(SISTERS):
            st["world"]["actors"].append({"id": 0x300 + i, "addr": hex(0x300 + i), "name": boss, "kind": "enemy",
                                          "pos": [-5.0 + 10 * i, 150.0, -160.0], "dist": 12.0, "life": 24,
                                          "max_life": 24, "alive": True,
                                          "owner": "server" if server else 1})
