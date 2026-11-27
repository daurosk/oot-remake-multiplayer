"""Fake-game reactions for row 215's finish test (ingame/test_row215.py; owner row-215-confirm). Hooks: fakegame.py
docstring. Sorts after boss_awake.py / boss_shots.py: they answer the TwinrovaStage warp, log the sisters' sync notes
and list the sisters alive (owner 1); this one only adds the strikes, the merge, the finish and the server's lines.

Reacts only with LICHII_COOP_CHEATS=1, only in TwinrovaStage and only while BT_FAKE_ROW215 is set (keep, handover,
noask, nomerge): test_boss_shots.py's fake (boss_shots.py) answers the same `kill ... Kotake` line with its own merge, so
this one stays off for every other selftest. Its strikes are answered in the `command` hook (before any `act` hook).
A is player 1 and owns the boss (as boss_awake.py).
- `kill [ID] NAME` / `hit [ID] NAME N` (docs/playtest.md "act"): the reply carries strike_seq, state.strike the result
  ({seq, result, name, id}; docs/playtest.md "strike") from the next frame on.
- A kills both sisters: 60 frames later the sisters are gone and Enemy_DungeonBoss_Twinrova (merged, phase 2) is in
  both games' world lists, alive, life 24, owner 1. A kills her: life 0 for FINISH frames (her death), then gone.
- B hits her at life 0: the server (a fake server-out.txt next to the games' folders, coop_server's own lines) says
  the row 215 keep line once per finish (server/session.cpp Session::keeps_finish) and its counts line (host_service.cpp
  say_counts) counts it.
BT_FAKE_ROW215=handover: the server hands her to player 2 instead (session.cpp change_owner's 'hand-over:' line) and
both lists name owner 2: the bug as reported; the test must FAIL. =noask: B's hits make no ask (no server line): the
test must say NO-REF (keeps_finish not exercised), never PASS. =nomerge: the sisters die but no Twinrova comes: NO-REF.
"""
import os

SWITCH = "LICHII_COOP_CHEATS=1"
STAGE = "TwinrovaStage"
SISTERS = ("Enemy_DungeonBoss_Koume", "Enemy_DungeonBoss_Kotake")
ROOT = "Enemy_DungeonBoss_Twinrova"
ROOT_ID = 0x3A0
MERGE = 60
FINISH = 600


def _on(game):
    return game.env_on(SWITCH) and bool(os.environ.get("BT_FAKE_ROW215"))


def _mode():
    return os.environ.get("BT_FAKE_ROW215", "")


def _boss(game):
    """The lane's one boss record (kept on A: one fight for both games)."""
    a = game if game.tag == "A" else game.other
    if not hasattr(a, "r215"):
        a.r215 = {"dead": set(), "root": None, "owner": 1, "kept": 0, "noted": False, "seq": 0}
    return a.r215


def _server(game, line):
    path = os.path.join(os.path.dirname(game.folder), "server-out.txt")
    with open(path, "a", encoding="utf-8") as f:
        f.write("coop_server: %s\n" % line)


def setup(game):
    game.r215_strike, game.r215_seq = None, None
    if game.tag == "A":
        _server(game, "listening on port 27015 (all network adapters, datagrams of 1200 bytes)")


def _strike(game, words):
    b = _boss(game)
    b["seq"] += 1
    seq = b["seq"]
    game.r215_seq = seq
    rest = words[1:]
    aid = int(rest[0]) if rest and rest[0].isdigit() else None
    if aid is not None:
        rest = rest[1:]
    name = rest[0] if rest else ""
    result = "none with a life"
    if name in SISTERS and name not in b["dead"] and game.tag == "A":
        result = "done"
        b["dead"].add(name)
        if b["dead"] >= set(SISTERS) and _mode() != "nomerge":
            def merge():
                b["root"] = {"life": 24, "gone": False}
                game.say("coop: bosses: %s: native boss sync on (BSA 0x3a0, ours)" % ROOT)
                game.other.say("coop: bosses: %s: native boss sync on (BSA 0x3a0, a copy)" % ROOT)
            game.later(MERGE, merge)
    elif name == ROOT and b["root"] and not b["root"]["gone"]:
        r = b["root"]
        result = "done"
        if game.tag == "A" and words[0] == "kill":
            r["life"] = 0
            b["noted"] = False

            def gone():
                r["gone"] = True
            game.later(FINISH, gone)
        elif game.tag == "B" and r["life"] == 0:
            if _mode() == "handover":
                if b["owner"] == 1:
                    b["owner"] = 2
                    _server(game, "hand-over: %s player 1 -> player 2: the hitter is nearer than its owner" % ROOT)
            elif _mode() != "noask":
                b["kept"] += 1
                if not b["noted"]:
                    b["noted"] = True
                    _server(game, "boss finish: %s in SpiritTemple at life 0 stays with player 1 (not to player 2): "
                                  "no hand-over until it is gone (row 215)" % ROOT)
                _server(game, "enemies: group entries taken 3, refused 0; damage relayed %d, dropped 0, given back "
                              "across a hand-over 0, kept through the owner hold 0; kills told late 0; boss turns 0, "
                              "enemy turns 0; keyed values not kept (list full) 0; boss finishes kept %d"
                        % (b["kept"], b["kept"]))
    game.later(1, lambda: setattr(game, "r215_strike", {"seq": seq, "result": result, "name": name,
                                                        "id": aid if aid is not None else 0}))


def command(game, m):
    """A strike line (`act` kill/hit): answered here, before boss_shots.py's `act` hook takes the Kotake kill."""
    if not _on(game) or m.get("cmd") != "act" or game.scene != STAGE:
        return None
    words = (m.get("lines") or "").split()
    if not words or words[0] not in ("kill", "hit"):
        return None
    _strike(game, words)
    seq, game.r215_seq = game.r215_seq, None
    return {"ok": True, "lines": 1, "note": "ok", "strike_seq": seq}


def state(game, st):
    if not _on(game) or game.scene != STAGE:
        return
    b = _boss(game)
    acts = st["world"]["actors"]
    acts[:] = [a for a in acts if a.get("name") not in b["dead"]]
    r = b["root"]
    if r and not r["gone"]:
        acts.append({"id": ROOT_ID, "addr": hex(ROOT_ID), "name": ROOT, "kind": "enemy", "pos": [0.0, 142.0, -158.0],
                     "dist": 10.0, "life": r["life"], "max_life": 24, "alive": r["life"] > 0, "owner": b["owner"]})
    if game.r215_strike:
        st["strike"] = dict(game.r215_strike)
