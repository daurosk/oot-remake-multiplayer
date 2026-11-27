"""Fake-game reactions for C6-2's first-night test (ingame/test_first_night.py; owner port-g7). Hooks: fakegame.py
docstring. Reacts only with LICHII_COOP_DEBUG_BOOL=0x8592aa82 in the boot's env.

What a real game logs, each from the source test_first_night.py cites:
- a longshot.exe with the fix's log text in the game's folder (ls32-src/host/hos_main.c oot_first_night_fix), the
  watch's start line "coop: flags debug off, watching bool 0x8592aa82" (lichii/hook/coop_flags.cpp FlagDebug);
- `flag` HrlField_HasEnteredFirstTime_Adult -> "coop: test setflag ... (bool) V: set" (coop_testinput.cpp SetFlag) and,
  5 frames on, the watch "coop: flags debug: get_bool(0x8592aa82) = V (was W)" at a change;
- Sequence_IsChildhood 0 -> the state's age "adult";
- a warp to a map that is not HyruleField* with the flag at 1 -> 30 frames on, Longshot's clear: the flag 0, "oot:
  cleared the first adult night's time lock (the save was in MAP, outside Hyrule Field)" and the watch "= 0 (was 1)";
- with LICHII_COOP_DEBUG_TIME=1 on A: "coop: time: sent HH:MM" every 300 frames (lichii/hook/coop_time.cpp).
BT_FAKE_FIRST_NIGHT=leak: B's clear also reaches A (A's flag 0 and its watch line 60 frames later): the test must FAIL.
=nofix: no fix text in longshot.exe (a Longshot before 3.2): SETUP-FAIL.
"""
import os

ON = "LICHII_COOP_DEBUG_BOOL=0x8592aa82"
TIME = "LICHII_COOP_DEBUG_TIME=1"
FLAG = "HrlField_HasEnteredFirstTime_Adult"
AGE = "Sequence_IsChildhood"
HASH = "0x8592aa82"
FIX = b"oot: cleared the first adult night's time lock (the save was in %s, outside Hyrule Field)"


def _on(game):
    return game.env_on(ON)


def setup(game):
    game.fn_adult, game.fn_time_at = False, None
    if not _on(game):
        return
    game.flags.setdefault(FLAG, 0)
    with open(os.path.join(game.folder, "longshot.exe"), "wb") as f:
        f.write(b"MZ fake " + (b"" if os.environ.get("BT_FAKE_FIRST_NIGHT") == "nofix" else FIX) + b"\0")
    game.say("coop: flags debug off, watching bool %s" % HASH)


def _watch(game, now, was):
    if now != was:
        game.say("coop: flags debug: get_bool(%s) = %d (was %d)" % (HASH, int(now), int(was)))


def after(game, m, r):
    if not _on(game) or not r.get("ok"):
        return
    cmd = m.get("cmd")
    if cmd == "flag" and m.get("name") == FLAG:
        now, was = int(m["value"]), int(r.get("was") or 0)
        game.say("coop: test setflag %s (bool) %d: set" % (FLAG, now))
        game.later(5, lambda: _watch(game, now, was))
    elif cmd == "flag" and m.get("name") == AGE:
        game.fn_adult = int(m["value"]) == 0
    elif cmd == "warp":
        scene = m.get("scene") or ""
        if not scene.startswith("HyruleField") and int(game.flags.get(FLAG) or 0) == 1:
            game.later(30, lambda: _clear(game, scene))


def _clear(game, scene):
    game.flags[FLAG] = 0
    game.say((FIX % scene.encode()).decode())
    _watch(game, 0, 1)
    if os.environ.get("BT_FAKE_FIRST_NIGHT") == "leak":
        o = game.other

        def leak():
            if int(o.flags.get(FLAG) or 0) == 1:
                o.flags[FLAG] = 0
                _watch(o, 0, 1)
        o.later(60, leak)


def state(game, st):
    if not _on(game):
        return
    if game.fn_adult and st.get("age"):
        st["age"] = "adult"
    if game.tag == "A" and game.env_on(TIME):
        if game.fn_time_at is None or game.frame - game.fn_time_at >= 300:
            game.fn_time_at = game.frame
            game.say("coop: time: sent 12:%02d" % (game.frame // 300 % 60))
