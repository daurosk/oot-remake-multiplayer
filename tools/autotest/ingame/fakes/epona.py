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
A Get_Epona written lower than Complete comes back to Complete 30 frames on (so a --repeat round 2 finds her won).
"""
import math
import os

FIELD = "HyruleField_Adult"
FIELD_EPONA = [-100.47, 150.60, 264.55]
COMPLETE_HASH = "0x4c0a63f4"
SONG = ["X", "Y", "A", "X", "Y", "A"]
B_ON = "coop: ride: we ride Animal_Epona_Adult, sending PLAYER_RIDE"


def _bug():
    return os.environ.get("BT_FAKE_EPONA_BUG", "")


def _field(game):
    return game.scene == FIELD and not game.at_title


def _won(game):
    return game.flags.get("Get_Epona") in ("Complete", COMPLETE_HASH)


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
    cmd = m.get("cmd")
    if cmd == "flag" and m.get("name") == "Get_Epona" and m.get("value") in ("Complete", COMPLETE_HASH):
        game.other.flags["Get_Epona"] = "Complete"          # shared by its end
    elif cmd == "flag" and m.get("name") == "Get_Epona" and r.get("was") in ("Complete", COMPLETE_HASH):
        # written lower: the group's Complete comes back (co-op never lowers it, docs/flags.md "Shared by its end")
        game.later(30, lambda: game.flags.__setitem__("Get_Epona", "Complete"))
    elif cmd == "flag_read" and m.get("name") == "Get_Epona":
        v = game.flags.get("Get_Epona")
        r.update(kind="enum", value=COMPLETE_HASH if v in ("Complete", COMPLETE_HASH) else (v or "0x00000000"))
    elif cmd == "warp" and game.tag == "B":
        game.horse = list(FIELD_EPONA) if m.get("scene") == FIELD and not _won(game) else None
        game.riding = False
        if m.get("scene") != FIELD:
            game.other.a_shown = None


def state(game, st):
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
    if game.tag == "B" and _field(game) and game.horse:
        return [{"id": 77, "name": "Animal_Epona_Adult", "kind": "other", "pos": list(game.horse), "dist": 1.0}]
    return []


def press(game, buttons):
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
