"""Fake-game reactions for row 150's port (ingame/test_epona.py; owner port-sweep-c131-3). Each line cites the module.

- age: HyruleField_Adult = adult (the rows' age flag + warp), as fakes/gear.py does for Kakariko.
- Get_Epona: `flag` with the value name Complete keeps it; `flag_read` answers it as an enum hash (0x4c0a63f4 =
  Complete, the game's own quest line); a game at Complete makes the other Complete too (docs/flags.md, shared by its
  end); a game at Complete logs 'coop: ride: Epona is ours (Get_Epona Complete): ...' ~60 frames on (coop_ride.cpp:468,
  asked once a second), on each change.
- the horse (B's world list, Animal_Epona_Adult): the field Epona at -100.47 150.60 264.55 while B has not won her
  (hs-epona.B.txt header); once won she is where B left her, or near B once called by Epona's Song (X Y A X Y A on
  the ocarina, ocarina_notes.py) and then logs 'our Epona is out with nobody on her at ...'.
- A pressed within 2.5 m of her (ocarina away) gets on / off; the ride lines on B and on A are coop_ride.cpp's
  (:323-336 for A's 'P2 rides ... (horse copy CoopRide_Epona[, the same copy])' / 'waits' / 'is here' / 'off the
  horse', :490-503 for B's), A's 'coop: spawn: CoopRide_Epona: create copy N' once when its copy is made
  (coop_spawn.cpp:2031).
Selftest switches (tests/test_ingame_epona.py): BT_FAKE_EPONA_BUG=blink -> A makes a second horse copy at the
remount (its line still says the same copy);
=gone -> A drops B's waiting horse at the get-off (the copy deleted, no 'waits'); =nosong -> the song brings nobody.
The field Epona is in B's list only near her spot (gate-ui-epona-0121: from the entrance-less warp B stood at
HyruleField_FrontOfLostWoodBridge, S 309d7180c game-out-B 4806, and B's list had none): within NEAR_M of her, or after
the rows' own entrance HyruleField_FrontOfMarket (hs-epona-sync.B.txt:24). NEAR_M models the old rows' 'place beside
her' only; the real list's reach is measured by the probe (gate-probe-0121 test_probe_epona).
A Get_Epona written lower than Complete comes back to Complete 30 frames on (so a --repeat round 2 finds her won).
SCOPED (B #149 rule): the whole module acts only with BT_FAKE_ROW150=1, which only tests/test_ingame_epona.py sets
(review-tests-153-6 06:56: its field Epona, age and song reached row 294's runs too; their age comes from the game's
own flag, fakegame.age, and their horse lines from fakes/epona294.py).
Since the quest is its real Enum Step_Get_Epona, fakes/epona294.py (row 294) reacts to the same flag; unscoped, both
answered every run (CI 46110930507: test_ingame_row294 FAIL 2 != 1, Suite desync OwnedHorseList.ActorName[0]).
"""
import math
import os

FIELD = "HyruleField_Adult"
FIELD_EPONA = [-100.47, 150.60, 264.55]
COMPLETE_HASH = "0x4c0a63f4"
QUEST = "Step_Get_Epona"   # the quest's Enum name (test_epona.GET_EPONA); "Get_Epona" is no flag: kind unknown
MARKET = "HyruleField_FrontOfMarket"
NEAR_M = 5.0
SONG = ["X", "Y", "A", "X", "Y", "A"]
B_ON = "coop: ride: we ride Animal_Epona_Adult, sending PLAYER_RIDE"


def _bug():
    return os.environ.get("BT_FAKE_EPONA_BUG", "")


def _field(game):
    return game.scene == FIELD and not game.at_title


def _row():
    return os.environ.get("BT_FAKE_ROW150") == "1"


def _won(game):
    return game.flags.get(QUEST) in ("Complete", COMPLETE_HASH)


def setup(game):
    game.horse, game.riding, game.song, game.a_copy, game.a_shown = None, False, [], 0, None
    game.owns_last = None


def _a_sees(a, how):
    """A's ride module (coop_ride.cpp:316-337): a line on each change of B's horse as shown."""
    before = a.a_shown
    if before == how:
        return
    if (how and before is None) or (_bug() == "blink" and how == "riding" and before == "free"):
        a.a_copy += 1                                   # a new copy each time one appears (the last one deleted)
        n = a.a_copy
        a.later(2, lambda: a.say("coop: spawn: CoopRide_Epona: create copy %d" % n))
    same = ", the same copy)" if before else ")"
    if how == "riding":
        a.later(4, lambda: a.say("coop: ride: P2 rides Animal_Epona_Adult (horse copy CoopRide_Epona%s" % same))
    elif how == "free":
        a.later(4, lambda: a.say("coop: ride: P2's Animal_Epona_Adult " + (
            "waits, nobody on her (the same copy)" if before else "is here, nobody on her (horse copy CoopRide_Epona)")))
    else:
        a.later(4, lambda: a.say("coop: ride: P2 off the horse (copy deleted)" if before == "riding"
                                 else "coop: ride: P2's horse gone (copy deleted)"))
    a.a_shown = how


def after(game, m, r):
    if not _row():
        return
    cmd = m.get("cmd")
    if cmd == "flag" and m.get("name") == QUEST and m.get("value") in ("Complete", COMPLETE_HASH):
        game.other.flags[QUEST] = "Complete"          # shared by its end
    elif cmd == "flag" and m.get("name") == QUEST and r.get("was") in ("Complete", COMPLETE_HASH):
        # written lower: the group's Complete comes back (co-op never lowers it, docs/flags.md "Shared by its end")
        game.later(30, lambda: game.flags.__setitem__(QUEST, "Complete"))
    elif cmd == "flag_read" and m.get("name") == QUEST:
        v = game.flags.get(QUEST)
        r.update(kind="enum", value=COMPLETE_HASH if v in ("Complete", COMPLETE_HASH) else (v or "0x00000000"))
    elif cmd == "flag_read" and m.get("name") == "Get_Epona":
        # (the real game's answer to the old wrong name, probe0121e-16ac9701 05:07: no flag of that name)
        r.update(kind="unknown", value=None)
    elif cmd == "warp" and game.tag == "B":
        game.horse = list(FIELD_EPONA) if m.get("scene") == FIELD and not _won(game) else None
        game.epona_entrance = m.get("entrance")
        game.riding = False
        if m.get("scene") != FIELD:
            game.other.a_shown = None


def state(game, st):
    if not _row():
        return
    if _field(game):
        st["age"] = "adult"
    if not game.at_title and _won(game) != game.owns_last:     # logged on each change (coop_ride.cpp:470-474)
        game.owns_last = _won(game)
        game.later(60, lambda w=game.owns_last: game.say(
            "coop: ride: Epona is ours (Get_Epona Complete): she is sent while nobody rides her too" if w
            else "coop: ride: Epona not won in this game: a horse is sent only while we ride"))
    if game.tag == "B" and game.riding:
        game.horse = list(game.pos)


def actors(game):
    if not _row():
        return []
    far = (game.horse == FIELD_EPONA and getattr(game, "epona_entrance", None) != MARKET
           and math.dist(game.pos[::2], FIELD_EPONA[::2]) > NEAR_M)   # (the field Epona, out of B's list's reach)
    if game.tag == "B" and _field(game) and game.horse and not far:
        return [{"id": 77, "name": "Animal_Epona_Adult", "kind": "other", "pos": list(game.horse), "dist": 1.0}]
    return []


def press(game, buttons):
    if not _row():
        return False
    if game.tag != "B" or not _field(game):
        return False
    a = game.other
    if game.ocarina:
        for b in buttons:
            if b in ("X", "Y", "A"):
                game.song = (game.song + [b])[-6:]
        if game.song == SONG and _won(game) and _bug() != "nosong":
            game.song = []
            game.horse = [game.pos[0] + 3.0, game.pos[1], game.pos[2] + 3.0]
            game.later(200, lambda: game.say("coop: ride: our Epona is out with nobody on her at %.1f %.1f %.1f, "
                                             "sending PLAYER_RIDE free" % tuple(game.horse)))
            game.later(200, lambda: _a_sees(a, "free"))
        return False                                   # the built-in ocarina notes
    if "A" not in buttons:
        return False
    if game.riding:
        game.riding = False
        game.say("coop: ride: we got off, riding off sent")
        if _won(game):
            game.later(2, lambda: game.say("coop: ride: our Epona is out with nobody on her at %.1f %.1f %.1f, "
                                           "sending PLAYER_RIDE free" % tuple(game.horse)))
            _a_sees(a, None if _bug() == "gone" else "free")
        else:
            game.horse = None
            _a_sees(a, None)
        return True
    if game.horse and math.dist(game.pos[::2], game.horse[::2]) < 2.5:
        game.riding = True
        if _won(game) and a.a_shown == "free":          # rider_.Free() changed (coop_ride.cpp:489-494) ...
            game.say("coop: ride: our Epona has us on her again")
        game.say(B_ON + " (1 horse seen, 'Ride')")      # ... and rider_.Riding() changed (:497-509): both lines
        _a_sees(a, "riding")
        return True
    return False
