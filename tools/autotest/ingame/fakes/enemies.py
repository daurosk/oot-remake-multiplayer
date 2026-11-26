"""Fake-game reactions for rows 230-232's port (ingame/test_enemies.py; owner port-sweep-c131-4). Each line cites the
module; places are the old scenarios' (test_enemies.py constants).

- Market_Adult: adult; a standing Enemy_ReDead at -3.17 100.15 13.13 in both world lists. A Link that comes within 3 m
  of it is screamed at once per round (a round starts when A is placed back at its 6 m spot): round 1's owner is A,
  round 2's B (the old rerun's hand-over). The owner logs 'share scream: our Enemy_ReDead froze our Link ('Shout', 0
  quarters): shared ...' (coop_objects_sync.cpp:3403), the other game 'share scream: the owner's Enemy_ReDead froze its
  Link; ours is in its reach (X m): frozen here too' (:4418) inside 5.5 m, else 'is out of its reach (X m)' (:4421),
  then 'scream: our Link plays 'Paralysis' 0.5 s after the shared scream's hit' (coop_objects.cpp:2384) 30 frames on.
- Boss boots (BT_FAKE_BOOT=live-meg-room / live-deadhand-room / live-deadhand-room-b, as the selftest's --boot): the game starts in that stage
  and its state lists coop.players with self (docs/playtest.md state).
- MegStage: Meg (owner 1); a split 600 frames after the first look, then every 900: three decoys of the owner at the run's seats
  (shots-lane1/ep230-hs-meg-decoys-231/20261123-090930) on both games, each logged 'coop: objects: + ..._Dummy ... at'.
  `kill Enemy_MiddleBoss_Poe_Meg` kills her on both; the copy game (B) logs 'coop: bosses: Enemy_MiddleBoss_Poe_Meg: its
  owner's died: the copy's own Life step lets it die now' once (coop_boss_native.cpp:614).
- DeadHandStage: Dead Hand (owner 1) at -0.16 92.31 -105.06. A Link placed within 1.5 m of a hand (0,-108.43) or
  (0,-98.43) is held 120 frames on while no other is held ('share grab: our Enemy_DeadHand's piece holds our Link:
  Enemy_DeadHand wanted here', coop_objects_sync.cpp:3555), let go 300 frames later ('... no piece ... now', :3560); a
  held game that is not his owner gets him 120 frames on (server/session.cpp:3225 ask_handover), and he stands at it.
Selftest switch BT_FAKE_ENEMY_BUG (tests/test_ingame_enemies.py): noshare -> the other game says 'ours not seen';
nofreeze -> its Link plays 'Wait'; noscream -> no scream at all; extra -> B keeps its own 3 decoys beside the copies;
loop -> B logs the minions loop line; twice -> B logs the death twice; nohandover -> Dead Hand stays with his owner;
apart -> B's copy of Dead Hand stands at the other hand; bowns -> B owns Dead Hand from the start (a boot an earlier
Dead Hand test used: review-rows-c130-2's empty-PASS case).
"""
import math
import os

MARKET, RD, A_FAR = "Market_Adult", [-3.17, 100.15, 13.13], [0.67, 100.2, 8.57]
MEG_STAGE, DH_STAGE = "MegStage", "DeadHandStage"
SEATS = [[-1.54, 100.0, 1.21], [-2.54, 100.0, 5.34], [2.59, 100.0, 2.22]]
OWN_ROLL = [[-1.98, 100.0, 7.54], [4.22, 100.0, 5.78], [0.26, 100.0, -1.78]]
DH = [-0.16, 92.31, -105.06]
HANDS = [[0.0, 92.4, -108.43], [0.0, 92.4, -98.43]]
BOOTS = {"live-meg-room": (MEG_STAGE, [0.0, 100.0, 6.0]), "live-deadhand-room": (DH_STAGE, [0.0, 92.4, -112.0]),
         "live-deadhand-room-b": (DH_STAGE, [0.0, 92.4, -112.0])}


def _bug():
    return os.environ.get("BT_FAKE_ENEMY_BUG", "")


def _flat(p, q):
    return math.hypot(p[0] - q[0], p[2] - q[2])


def _num(game):
    return 1 if game.tag == "A" else 2


def _a(game):
    return game if game.tag == "A" else game.other


def setup(game):
    game.en = {"round": 0, "screamed": 0, "meg_alive": True, "split_at": None, "splits": 0,
               "dh_owner": 2 if _bug() == "bowns" else 1,
               "held": None, "grab_due": None, "dh_at": list(DH)}
    boot = BOOTS.get(os.environ.get("BT_FAKE_BOOT", ""))
    if boot:
        game.scene, game.pos = boot[0], list(boot[1])


def _here(game, scene):
    return game.scene == scene and not game.at_title


def _scream(a):
    """Round a.en['round']'s scream (state is A's lane-wide record)."""
    s = a.en
    if s["screamed"] >= s["round"] or _bug() == "noscream":
        return
    s["screamed"] = s["round"]
    own = a if s["round"] % 2 == 1 else a.other
    rx = own.other
    own.say("coop: objects: share scream: our Enemy_ReDead froze our Link ('Shout', 0 quarters): shared for the Links "
            "in its reach")
    d = _flat(rx.pos, RD)
    if _bug() == "noshare":
        rx.later(6, lambda: rx.say("coop: objects: share scream: the owner's Enemy_ReDead froze its Link; ours not seen: "
                                   "not frozen"))
        return
    if d > 5.5:
        rx.later(6, lambda: rx.say("coop: objects: share scream: the owner's Enemy_ReDead froze its Link; ours is out of "
                                   "its reach (%.6f m)" % d))
        return
    rx.later(6, lambda: rx.say("coop: objects: share scream: the owner's Enemy_ReDead froze its Link; ours is in its "
                               "reach (%.6f m): frozen here too" % d))
    anim = "Wait" if _bug() == "nofreeze" else "Paralysis"
    rx.later(36, lambda: rx.say("coop: objects: scream: our Link plays '%s' 0.5 s after the shared scream's hit" % anim))


def act(game, words):
    a = _a(game)
    if words[0] == "place" and _here(game, MARKET) and game.tag == "A":
        if _flat([float(v) for v in words[1:4]], A_FAR) < 0.1:
            a.en["round"] += 1
    elif words[0] == "place" and _here(game, DH_STAGE):
        p = [float(v) for v in words[1:4]]
        game.pos = p
        if any(_flat(p, h) < 1.5 for h in HANDS):
            _try_grab(game)
        return True
    elif words[0] == "kill" and words[-1] == "Enemy_MiddleBoss_Poe_Meg" and _here(game, MEG_STAGE):
        if a.en["meg_alive"]:
            a.en["meg_alive"] = False
            b = a.other
            n = 2 if _bug() == "twice" else 1
            for i in range(n):
                b.later(20 + i * 10, lambda: b.say("coop: bosses: Enemy_MiddleBoss_Poe_Meg: its owner's died: the copy's "
                                                   "own Life step lets it die now"))
        return True
    return False


def _try_grab(game):
    a = _a(game)

    def grab():
        if not any(_flat(game.pos, h) < 1.5 for h in HANDS):
            return
        if a.en["held"] not in (None, game.tag):
            game.later(60, grab)
            return
        a.en["held"] = game.tag
        game.say("coop: objects: share grab: our Enemy_DeadHand's piece holds our Link: Enemy_DeadHand wanted here")
        if a.en["dh_owner"] != _num(game) and _bug() != "nohandover":
            game.later(120, lambda: a.en.update(dh_owner=_num(game), dh_at=list(game.pos)))

        def free():
            a.en["held"] = None
            game.say("coop: objects: share grab: no piece of Enemy_DeadHand holds our Link now")
        game.later(300, free)
    game.later(120, grab)


def state(game, st):
    a = _a(game)
    if _here(game, MARKET):
        st["age"] = "adult"
        if game.tag == "A" and a.en["round"] and _flat(game.pos, RD) < 3.0:
            _scream(a)
    for scene in (MEG_STAGE, DH_STAGE):
        if _here(game, scene):
            for p in (st.get("coop") or {}).get("players") or []:
                p["self"] = p.get("number") == _num(game)
    if _here(game, MEG_STAGE) and game.tag == "A" and a.en["meg_alive"]:
        s = a.en
        if s["split_at"] is None:
            s["split_at"] = game.frame + 600
        elif game.frame >= s["split_at"] and s["splits"] < 3:
            s["splits"] += 1
            s["split_at"] = game.frame + 900   # (past the test's 'wait 600' + shots: one split per await)
            for g in (game, game.other):
                for i, p in enumerate(SEATS):
                    g.say("coop: objects: + Enemy_MiddleBoss_Poe_Meg_Dummy 0x10%08x state 4 at %.2f %.2f %.2f hash "
                          "0xffffffffffffffff" % (s["splits"] * 16 + i, p[0], p[1], p[2]))
            if _bug() == "loop":
                game.other.say("coop: objects: share minions: our copy of Enemy_MiddleBoss_Poe_Meg made 4 "
                               "Enemy_MiddleBoss_Poe_Meg_Dummy (a loop)")


def actors(game):
    a = _a(game)
    out = []
    if _here(game, MARKET):
        out.append({"id": 501, "name": "Enemy_ReDead", "kind": "enemy", "pos": list(RD), "owner": 1, "alive": True,
                    "life": 8, "max_life": 8})
    if _here(game, MEG_STAGE):
        s = a.en
        out.append({"id": 601, "name": "Enemy_MiddleBoss_Poe_Meg", "kind": "enemy", "pos": [0.03, 97.56, -3.46],
                    "owner": 1, "alive": s["meg_alive"], "life": 24 if s["meg_alive"] else 0, "max_life": 24})
        if s["splits"] and s["meg_alive"]:
            seats = SEATS + (OWN_ROLL if game.tag == "B" and _bug() == "extra" else [])
            for i, p in enumerate(seats):
                out.append({"id": 610 + i, "name": "Enemy_MiddleBoss_Poe_Meg_Dummy", "kind": "enemy", "pos": list(p),
                            "owner": 1, "alive": True, "life": 1, "max_life": 1})
    if _here(game, DH_STAGE):
        s = a.en
        at = list(s["dh_at"])
        if game.tag == "B" and _bug() == "apart":
            at = [at[0], at[1], at[2] + 10.0]
        out.append({"id": 701, "name": "Enemy_DeadHand", "kind": "enemy", "pos": at, "owner": s["dh_owner"],
                    "alive": True, "life": 40, "max_life": 40})
    return out
