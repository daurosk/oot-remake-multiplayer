"""Plays our autotest scripts in Hookshot games, through Hookshot's control bridge.

Hookshot (the static recomp, docs/hookshot.md) has a local control socket (its docs/control.md): a named pipe on
Windows, a Unix socket on Linux, newline-delimited JSON. This module is our client for it (standard library only):

- `Bridge`: one connection; requests matched to replies by id.
- `parse`: our test script format (lichii/hook/coop_testinput.h), the same files the emulator plays.
- `Player`: plays a parsed script into one game in real time (1 tick = 1/60 s, as the emulator's hook counts):
  wait, hold, press, stick and rstick as bridge inputs let go by `release` when their time is up; `mark` logged and
  shot; `warp` as the bridge's teleport (retried until the game takes it, as the hook does); `goto` and `toward`
  steered from Link's position read through guest memory (`HOS_CONTROL_MEM=ro`). Steps that need the co-op module
  inside the game (chat, give, wear, assign, place, kill, hit, stun, drop, verb, hurt, setflag, setenum, menu) are written, as the script has them, to
  the game's LICHII_COOP_TEST_COMMANDS file: the module (lichii/hook/coop_testinput.cpp) plays them on its next
  check (4 a second) and empties the file, so the player knows they were taken. Without that file they are logged as
  skipped.
- `run`: the two-game test that tools/autotest/run.ps1 -Runtime hookshot starts: the server, both games with the bridge
  on, the save loaded (A pressed until the game plays), the scenario's A and B scripts at once, shots of both games
  at every mark and every few seconds, the logs, then everything closed.

Each player writes the lines the emulator's hook writes (`coop: test mark NAME`, `coop: test script done`, ...) to
its own log, so the runner reads both the same way. Marks also go through the module's commands file, so the game's
log has them in order with its own lines (groups.py grades that log); a warp is done when its map is reached.

    python -I tools/autotest/hookshot.py play --sock coop-auto-A --script walk.A.txt --log a.txt --shots out
    python -I tools/autotest/hookshot.py run --game-a <folder> --game-b <folder> --server coop_server.exe --out <dir>
                                             --script-a walk.A.txt [--script-b walk.B.txt] [--load load.txt]
                                             [--kit-a <kit> --kit-b <kit>]   (the kits' own start, see kit_start)
"""
import argparse
import base64
import json
import math
import os
import queue
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import placement  # noqa: E402  (where the games' windows go: one monitor, no focus stolen)
import lanes  # noqa: E402  (brings tools/hookshot/hs.py: run_quiet and popen_quiet start a console child with no window)
import ocarina_notes  # noqa: E402  (the Ocarina's keys and songs, the ONE table: the runner's `song` step)

WINDOWS = os.name == "nt"
TICKS = 60.0              # script ticks a second
PRESS_TICKS = 6           # press KEYS: held this long, then let go as long
GOTO_TICKS = 600          # goto gives up after 10 s
MAX_STEP_TICKS = 60 * 60 * 10  # a played step's TICKS: 1 up to this (coop_testinput.cpp kMaxStepTicks, 10 min)
MAX_NUMBER = 1e6          # a coordinate, radius or stick value: at most this far from 0 (coop_testinput.cpp Number)
MAIN_BASE = 0x80020000    # where Hookshot maps main (docs/hookshot.md)
GAMEUI_SLOT = 0x4e7cfa8   # main offset of the GameUI module pointer (integration docs/player-state-map.md)
POS_VALID = 0x58a         # GameUI +0x58a bit 0: the position below is Link's
POS = 0x590               # GameUI +0x590: Link's position, x y z floats

# Our key names (coop_testinput.h) to the bridge's (control.md, HOS_INPUT_SCRIPT's names).
KEYS = {
    "A": "A", "B": "B", "X": "X", "Y": "Y", "L": "L", "R": "R", "ZL": "ZL", "ZR": "ZR",
    "PLUS": "PLUS", "MINUS": "MINUS", "LCLICK": "LS", "RCLICK": "RS",
    "DUP": "UP", "DDOWN": "DOWN", "DLEFT": "LEFT", "DRIGHT": "RIGHT",
}
STICK_KEYS = {  # full stick directions: (stick, x, y)
    "LSTICK_UP": ("L", 0, 1), "LSTICK_DOWN": ("L", 0, -1), "LSTICK_LEFT": ("L", -1, 0), "LSTICK_RIGHT": ("L", 1, 0),
    "RSTICK_UP": ("R", 0, 1), "RSTICK_DOWN": ("R", 0, -1), "RSTICK_LEFT": ("R", -1, 0), "RSTICK_RIGHT": ("R", 1, 0),
}
WEAR_SLOTS = ("weapon", "shield", "head", "armor", "boots", "hand")
# The `menu` step's words: lichii/hook/coop_testinput.h kMenuWords and kMenuPages, the one lists
# (tests/test_ingame_menu_pages.py checks these copies against them).
MENU_WORDS = ("customize", "close", "host", "paste", "leave", "main")
MENU_PAGES = ("main", "host", "join", "local", "settings", "commands", "customize", "find")
MODULE_ONLY = ("chat", "give", "wear", "dress", "assign", "place", "face", "aim", "kill", "hit", "stun", "drop", "verb", "hurt", "setflag", "setenum", "menu",
               "dump", "event", "autosave")


class ScriptError(Exception):
    pass


# The scenes the module's test warp writes into the game (lichii/hook/coop_teleport.cpp IsKnownScene: the scenes of
# coop_start_points.inc, which this reads from the repo's source, plus the one literal below). A scene it does not know
# is refused there, with nothing written, and the runner would wait out its whole warp for a map that never comes:
# `warp Market` (the scenes are Market_Child, Market_Adult) crashed a game worker before the module checked. So parse()
# refuses it at once. Without the source (a kit) there is no list and nothing is checked.
START_POINTS_INC = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                                "lichii", "hook", "coop_start_points.inc")
EXTRA_SCENES = ("BossRushIntervalRoom",)   # (a test keeps this and IsKnownScene's literal the same)
_scenes = {}


def known_scenes(path=START_POINTS_INC):
    """The set of scene names the module's test warp knows, or None when the source file is not there."""
    if path not in _scenes:
        try:
            with open(path, encoding="utf-8") as f:
                text = f.read()
        except OSError:
            _scenes[path] = None
        else:
            found = set(re.findall(r'^\s*\{"([A-Za-z0-9_]+)",', text, re.M))
            _scenes[path] = (found | set(EXTRA_SCENES)) if found else None
    return _scenes[path]


def scene_problem(name, scenes=None):
    """Why `warp NAME` would be refused by the module ("" when it would not, or when the list is not known).
    A number is the bridge's destination index and is left to the bridge."""
    scenes = known_scenes() if scenes is None else scenes
    if scenes is None or name in scenes or name.isdigit():
        return ""
    why = "unknown scene '%s'" % name
    like = sorted(x for x in scenes if x.startswith(name))[:6]
    if like:
        why += " (did you mean %s?)" % ", ".join(like)
    return why


INCLUDE_DEPTH = 4


def header_switches(path):
    """The `#! switches NAME=v,NAME=v` header lines of a scenario file (its A script) as [(NAME, v)], as
    run.ps1 reads them. Here too, so a caller that starts this file without run.ps1 (--script-a) gets them:
    row 234's run 20261125-114351 came from big-run-01113.ps1 that way, LICHII_COOP_CHEATS=1 never reached
    the games and every `give` logged 'NOT given (LICHII_COOP_CHEATS is off)'."""
    out = []
    if not path:
        return out
    with open(path, encoding="utf-8-sig") as f:
        for line in f.read().split("\n"):
            m = re.match(r"^#!\s*switches\s+(.+?)\s*$", line)
            if not m:
                continue
            for kv in m.group(1).split(","):
                kv = kv.strip()
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    out.append((k.strip(), v.strip()))
    return out


def read_script(path, _depth=0):
    """A scenario file's text with each `include FILE` line replaced by that file's text (FILE beside it; an
    include may include, at most INCLUDE_DEPTH deep). One copy of a step several scenarios share (the Gohma kill
    timing: scenarios/hs-gohma-kill.inc.txt). Raises ScriptError("FILE line N: ...")."""
    with open(path, encoding="utf-8-sig") as f:
        text = f.read()
    out = []
    for number, raw in enumerate(text.split("\n"), 1):
        w = raw.split("#", 1)[0].split()
        if not w or w[0].upper() != "INCLUDE":
            out.append(raw)
            continue
        where = "%s line %d" % (os.path.basename(path), number)
        if len(w) != 2:
            raise ScriptError(where + ": want: include FILE")
        if _depth >= INCLUDE_DEPTH:
            raise ScriptError(where + ": includes nest deeper than %d" % INCLUDE_DEPTH)
        inner = os.path.join(os.path.dirname(os.path.abspath(path)), w[1])
        if not os.path.isfile(inner):
            raise ScriptError(where + ": no " + w[1])
        out.append(read_script(inner, _depth + 1))
    return "\n".join(out)


class BridgeError(Exception):
    def __init__(self, reply):
        super().__init__("%s: %s" % (reply.get("error"), reply.get("message", "")))
        self.reply = reply
        self.code = reply.get("error")


# ---------------------------------------------------------------------------------------------- the script format

class Step:
    def __init__(self, kind, line, ticks=0):
        self.kind = kind          # timed, mark, goto, toward, warp, await, burst, or one of MODULE_ONLY
        self.line = line
        self.ticks = ticks
        self.buttons = []         # the bridge's names
        self.left = None          # (x, y), each -1..1
        self.right = None
        self.name = ""            # mark / warp scene / chat text / item ...
        self.start = ""
        self.x = self.y = self.z = 0.0
        self.radius = 1.0
        self.text = ""            # the line as written (no comment), for the module's commands file

    def __repr__(self):
        return "Step(%s line %d)" % (self.kind, self.line)


# One scenario grammar for both readers (row 325 review): the module's parser (lichii/hook/coop_testinput.cpp Parse,
# LICHII_COOP_TEST_SCRIPT and the C++ test that reads every scenario) and this one take the same words for TICKS and
# numbers, so a line one of them plays is never refused by the other. Whole numbers are plain digits (no sign, no
# underscore), numbers plain decimals (no hex, no underscore, no inf/nan).
_WHOLE = re.compile(r"[0-9]+")
_BURST_NAME = re.compile(r"[A-Za-z0-9_.-]+")   # a burst's shots are files: NAME-NNN
BURST_MAX = 600           # shots in one burst (each a PNG)
BURST_BEFORE = 30         # a burst on a line: the shots kept from before it
_DECIMAL = re.compile(r"[-+]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][-+]?[0-9]+)?")


def _ticks(text):
    """A played step's TICKS (wait, hold, press, stick, goto, toward): 1 up to MAX_STEP_TICKS (module Ticks)."""
    if len(text) > 9 or not _WHOLE.fullmatch(text):
        return None
    v = int(text)
    return v if 1 <= v <= MAX_STEP_TICKS else None


def _whole_in(text, low, high):
    """hit N, drop METRES, hurt QUARTER_HEARTS: plain digits from low to high (module from_chars + its range), or None.
    (str.isdigit also takes digits int() refuses, such as superscripts: a ValueError, not the line's error.)"""
    return int(text) if _WHOLE.fullmatch(text) and low <= int(text) <= high else None


def _int32(text):
    """setflag VALUE: a whole number, '-' allowed, that fits 32 bits (module from_chars into int32_t), or None."""
    if not re.fullmatch(r"-?[0-9]+", text):
        return None
    v = int(text)
    return v if -2 ** 31 <= v < 2 ** 31 else None


def _await_ticks(text):
    """await's TICKS, the runner's own step: any whole number from 0 up, 0 = no wait (module RunnerTicks)."""
    return int(text) if _WHOLE.fullmatch(text) else None


def _number(text):
    """A coordinate, radius or stick value: a plain decimal, finite, at most MAX_NUMBER from 0 (module Number)."""
    if not _DECIMAL.fullmatch(text):
        return None
    v = float(text)
    return v if math.isfinite(v) and abs(v) <= MAX_NUMBER else None


def _axis(text):
    v = _number(text)
    return v if v is not None and -1.0 <= v <= 1.0 else None


def _keys(text, step):
    """KEYS joined with + into step.buttons / step.left / step.right. The bad key, or None."""
    for k in text.upper().split("+"):
        if k in KEYS:
            if KEYS[k] not in step.buttons:
                step.buttons.append(KEYS[k])
        elif k in STICK_KEYS:
            stick, x, y = STICK_KEYS[k]
            if stick == "L":
                step.left = (x, y)
            else:
                step.right = (x, y)
        else:
            return k
    return None


# `nofairy`: every bottle's item (PlayerStatus.AssignableItem.BottleInfo.Bottle<A..E>.BottleItem, as hashes:
# coop/host/game_items.hpp letters::kBottles) set to Empty through the game's own queue (setenum). A fairy in a
# bottle makes a lethal hit the game's own death and revive, never down-but-out (coop_dbno: "a fairy in our
# bottle"), so a scenario that downs a player starts with it (the lane saves have one: 3 VOID runs, 2026-11-20).
BOTTLE_ITEMS = ("0x7d48df76", "0x1edd164a", "0xb80c70a8", "0xe4c1b1fc", "0x365e1953")


def parse(text):
    """Our test script (coop_testinput.h) as a list of Steps. Raises ScriptError("line N: ...")."""
    steps = []
    try:
        lines = ocarina_notes.expand(text)   # `song NAME [HOLD [GAP]]`: the song's presses from the ONE key table
    except ocarina_notes.SongError as e:
        raise ScriptError(str(e))
    for number, raw in lines:
        line = raw.split("#", 1)[0]
        w = line.split()
        if not w:
            continue
        cmd = w[0].upper()

        def fail(why):
            raise ScriptError("line %d: %s" % (number, why))

        s = Step("timed", number)
        s.text = line.strip()
        if cmd == "WAIT":
            if len(w) != 2 or _ticks(w[1]) is None:
                fail("want: wait TICKS")
            s.ticks = _ticks(w[1])
        elif cmd == "HOLD":
            if len(w) not in (3, 4) or _ticks(w[2]) is None:
                fail("want: hold KEYS TICKS [MARK]")
            s.ticks = _ticks(w[2])
            if len(w) == 4:
                s.name = w[3]
            bad = _keys(w[1], s)
            if bad:
                fail("unknown key " + bad)
        elif cmd == "PRESS":
            ticks = PRESS_TICKS
            if len(w) not in (2, 3) or (len(w) == 3 and _ticks(w[2]) is None):
                fail("want: press KEYS [TICKS]")
            if len(w) == 3:
                ticks = _ticks(w[2])
            bad = _keys(w[1], s)
            if bad:
                fail("unknown key " + bad)
            s.ticks = ticks
            steps.append(s)
            s = Step("timed", number, ticks)
        elif cmd in ("STICK", "RSTICK"):
            if len(w) not in (4, 5) or _axis(w[1]) is None or _axis(w[2]) is None or _ticks(w[3]) is None:
                fail("want: %s X Y TICKS [KEYS], X and Y from -1 to 1" % cmd)
            if len(w) == 5:
                bad = _keys(w[4], s)
                if bad:
                    fail("unknown key " + bad)
            if cmd == "STICK":
                s.left = (_axis(w[1]), _axis(w[2]))
            else:
                s.right = (_axis(w[1]), _axis(w[2]))
            s.ticks = _ticks(w[3])
        elif cmd == "GOTO":
            s.kind, s.ticks = "goto", GOTO_TICKS
            if len(w) < 3 or len(w) > 5 or _number(w[1]) is None or _number(w[2]) is None:
                fail("want: goto X Z [RADIUS] [TICKS], radius at least 0.2")
            s.x, s.z = _number(w[1]), _number(w[2])
            if len(w) >= 4:
                r = _number(w[3])
                if r is None or r < 0.2:
                    fail("want: goto X Z [RADIUS] [TICKS], radius at least 0.2")
                s.radius = r
            if len(w) == 5:
                if _ticks(w[4]) is None:
                    fail("want: goto X Z [RADIUS] [TICKS], radius at least 0.2")
                s.ticks = _ticks(w[4])
        elif cmd == "TOWARD":
            s.kind = "toward"
            if len(w) not in (4, 5) or _number(w[1]) is None or _number(w[2]) is None or _ticks(w[3]) is None:
                fail("want: toward X Z TICKS [KEYS]")
            s.x, s.z, s.ticks = _number(w[1]), _number(w[2]), _ticks(w[3])
            if len(w) == 5:
                bad = _keys(w[4], s)
                if bad:
                    fail("unknown key " + bad)
        elif cmd == "WARP":
            if len(w) not in (2, 3):
                fail("want: warp SCENE [START]")
            s.kind, s.name = "warp", w[1]
            s.start = w[2] if len(w) == 3 else ""
            if scene_problem(s.name):
                fail(scene_problem(s.name))
        elif cmd == "PLACE":
            # #196: "place PLATFORM UP": UP metres over the live place of that moving platform (the module's
            # TestPlatformPlace), for a rider on a floor that waves.
            if len(w) == 3 and _number(w[1]) is None and _number(w[2]) is not None:
                s.kind, s.name, s.y = "place", w[1], _number(w[2])
            elif len(w) != 4 or any(_number(v) is None for v in w[1:]):
                fail("want: place X Y Z, or place PLATFORM UP")
            else:
                s.kind = "place"
                s.x, s.y, s.z = (_number(v) for v in w[1:])
        elif cmd == "FACE":
            # Row 175: Link turned on the spot to face world X Z (the module's CoopBoss::FaceLink).
            if len(w) != 3 or any(_number(v) is None for v in w[1:]):
                fail("want: face X Z")
            s.kind = "face"
            s.x, s.z = (_number(v) for v in w[1:])
        elif cmd == "AIM":
            # Row 173: our next arrow is watched and nudged the last bit when its flight already points within 4 degrees
            # and 0.5 m of X Y Z (CoopObjects::QueueTestAim). Link is not turned.
            if len(w) != 4 or any(_number(v) is None for v in w[1:]):
                fail("want: aim X Y Z")
            s.kind = "aim"
            s.x, s.y, s.z = (_number(v) for v in w[1:])
        elif cmd == "CHAT":
            if len(w) < 2:
                fail("want: chat TEXT")
            s.kind = "chat"
            s.name = line.strip()[len(w[0]):].strip()
        elif cmd in ("KILL", "GIVE"):
            if len(w) != 2:
                fail("want: %s %s" % (cmd.lower(), "ACTOR" if cmd == "KILL" else "ITEM"))
            s.kind, s.name = cmd.lower(), w[1]
        elif cmd == "HIT":
            if len(w) != 3 or _whole_in(w[2], 1, 80) is None:
                fail("want: hit ACTOR N (1 to 80)")
            s.kind, s.name, s.start = "hit", w[1], w[2]
        elif cmd == "DROP":
            # Row 178: the nearest boss of that name that this game owns goes METRES straight down (test only).
            if len(w) != 3 or _whole_in(w[2], 1, 500) is None:
                fail("want: drop ACTOR METRES (1 to 500)")
            s.kind, s.name, s.start = "drop", w[1], w[2]
        elif cmd == "VERB":
            # Row 287: the boss of that name that this game owns runs action verb N through the game's own request.
            if len(w) != 3 or _whole_in(w[2], 1, 500) is None:
                fail("want: verb ACTOR N (1 to 500)")
            s.kind, s.name, s.start = "verb", w[1], w[2]
        elif cmd == "STUN":
            if len(w) != 2:
                fail("want: stun BOSS")
            s.kind, s.name = "stun", w[1]
        elif cmd == "HURT":
            if len(w) != 2 or _whole_in(w[1], 1, 80) is None:
                fail("want: hurt QUARTER_HEARTS (1 to 80)")
            s.kind, s.name = "hurt", w[1]
        elif cmd == "SETFLAG":
            if len(w) != 3 or _int32(w[2]) is None:
                fail("want: setflag NAME VALUE (an Int or Bool flag, a whole number that fits 32 bits)")
            s.kind, s.name, s.start = "setflag", w[1], w[2]
        elif cmd == "NOFAIRY":
            if len(w) != 1:
                fail("want: nofairy (no arguments)")
            for item in BOTTLE_ITEMS[:-1]:
                e = Step("timed", number)
                e.kind, e.name, e.start = "setenum", item, "Empty"
                e.text = "setenum %s Empty" % item
                steps.append(e)
            s.kind, s.name, s.start = "setenum", BOTTLE_ITEMS[-1], "Empty"
            s.text = "setenum %s Empty" % BOTTLE_ITEMS[-1]
        elif cmd == "DUMP":
            if len(w) != 2 or not re.fullmatch(r"[A-Za-z0-9_-]+", w[1]):
                fail("want: dump TAG (letters, digits, - and _)")
            s.kind, s.name = "dump", w[1]
        elif cmd == "EVENT":
            # event NAME: our game asked to play that event setting by name (the module's live-event requester).
            # boss-route-01112: a boss rush stage reached by `warp` misses its rematch boot event; Barinade and
            # Ganondorf appear only through theirs (DreamBossStage_Rematch_Barinade_Start / _Ganondorf_Start).
            if len(w) != 2 or not re.fullmatch(r"[A-Za-z0-9_]+", w[1]):
                fail("want: event NAME (an event setting's name: letters, digits and _)")
            s.kind, s.name = "event", w[1]
        elif cmd == "AUTOSAVE":
            # autosave: our game asked for its own autosave (the module's CoopSave::AskOwnAutoSave, rejoin-host-273-2):
            # a warp never saves, so a script that checks progress on disk asks for one.
            if len(w) != 1:
                fail("want: autosave")
            s.kind = "autosave"
        elif cmd == "SETENUM":
            if len(w) != 3:
                fail("want: setenum NAME VALUE (an Enum flag, or 0xHASH, and a value's name)")
            s.kind, s.name, s.start = "setenum", w[1], w[2]
        elif cmd == "WEAR":
            if len(w) != 3 or w[1].lower() not in WEAR_SLOTS:
                fail("want: wear weapon|shield|head|armor|boots|hand NAME")
            s.kind, s.start, s.name = "wear", w[1], w[2]
        elif cmd == "DRESS":
            if len(w) != 2 or w[1].lower() not in WEAR_SLOTS + ("all",):
                fail("want: dress weapon|shield|head|armor|boots|hand|all")
            s.kind, s.name = "dress", w[1].lower()
        elif cmd == "ASSIGN":
            if len(w) != 2:
                fail("want: assign ITEM")
            s.kind, s.name = "assign", w[1]
        elif cmd == "MENU":
            # (gear-link: "menu look KEY=VALUE", the module's coop_testinput.cpp MENU: a look part chosen and worn)
            if len(w) == 3 and w[1].lower() == "look" and "=" in w[2]:
                s.kind, s.name = "menu", " ".join(w[1:]).lower()
            elif len(w) == 3 and w[1].lower() == "open":     # `menu open PAGE` (B #138): any page, nothing pressed
                if w[2].lower() not in MENU_PAGES:
                    fail("want: menu open " + "|".join(MENU_PAGES))
                s.kind, s.name = "menu", "open " + w[2].lower()
            else:
                if len(w) != 2 or w[1].lower() not in MENU_WORDS:
                    fail("want: menu " + "|".join(MENU_WORDS) + ", menu open " + "|".join(MENU_PAGES)
                         + ", or menu look KEY=VALUE")
                s.kind, s.name = "menu", w[1].lower()
        elif cmd == "AWAIT":
            # Row 325: "await TICKS REGEX": until a NEW line of this game's own log (written after the step began)
            # matches REGEX (the rest of the line, spaces kept), at most TICKS. The runner reads the log itself: no
            # module step, so it plays on any build. A timeout is logged and the script goes on.
            pattern = line.strip()[len(w[0]):].strip()
            pattern = pattern[len(w[1]):].strip() if len(w) >= 2 else ""
            if len(w) < 3 or _await_ticks(w[1]) is None or not pattern:
                fail("want: await TICKS REGEX (the rest of the line, matched in this game's log)")
            try:
                re.compile(pattern)
            except re.error as e:
                fail("await: bad REGEX: %s" % e)
            s.kind, s.ticks, s.name = "await", _await_ticks(w[1]), pattern
        elif cmd == "SKIPTO":
            # epona-seen: "skipto MARK REGEX": when this game's own log has a line matching REGEX (the rest of the
            # line) written since the last mark played (the try's start), the script goes on at the later
            # `mark MARK`, the steps between are not played; else it goes on with the next step. The runner's own step, as await: a try that must
            # stop once it worked (get on a called horse: one more A press would get off again).
            pattern = line.strip()[len(w[0]):].strip()
            pattern = pattern[len(w[1]):].strip() if len(w) >= 2 else ""
            if len(w) < 3 or not pattern:
                fail("want: skipto MARK REGEX (the rest of the line, matched in this game's log)")
            try:
                re.compile(pattern)
            except re.error as e:
                fail("skipto: bad REGEX: %s" % e)
            s.kind, s.name, s.start = "skipto", w[1], pattern
        elif cmd == "BURST":
            # Issue 166: "burst NAME FRAMES [TICKS REGEX]": this game held and a shot of it every frame or two (one
            # frame run, then the shot), FRAMES shots; with REGEX they start when a NEW line of this game's log
            # matches, at most TICKS frames on, and the last BURST_BEFORE shots before that line are kept. The
            # runner's own step (no module step): an effect a mark's quarter-second shot misses is seen frame by frame.
            pattern = ""
            if len(w) >= 5:
                rest = line.strip()
                for word in w[:4]:
                    rest = rest[rest.index(word) + len(word):]
                pattern = rest.strip()
            if not (len(w) == 3 or len(w) >= 5) or not _BURST_NAME.fullmatch(w[1]) or _await_ticks(w[2]) is None                     or not 1 <= int(w[2]) <= BURST_MAX or (len(w) >= 5 and _await_ticks(w[3]) is None):
                fail("want: burst NAME FRAMES [TICKS REGEX] (FRAMES 1..%d)" % BURST_MAX)
            if pattern:
                try:
                    re.compile(pattern)
                except re.error as e:
                    fail("burst: bad REGEX: %s" % e)
            s.kind, s.name, s.start = "burst", w[1], pattern
            s.ticks = int(w[3]) if pattern else 0
            s.radius = int(w[2])          # (the shots after the line, or all of them)
        elif cmd == "MARK":
            if len(w) != 2:
                fail("want: mark NAME (one word)")
            s.kind, s.name = "mark", w[1]
        else:
            fail("unknown command " + w[0])
        steps.append(s)
    if not steps:
        raise ScriptError("no steps")
    for i, s in enumerate(steps):
        if s.kind == "skipto" and not any(t.kind == "mark" and t.name == s.name for t in steps[i + 1:]):
            raise ScriptError("line %d: skipto %s: no later 'mark %s'" % (s.line, s.name, s.name))
    return steps


# ---------------------------------------------------------------------------------------------- the bridge

def pipe_name(name):
    return name if name.startswith("\\\\.\\pipe\\") else "\\\\.\\pipe\\" + name


def open_transport(path, timeout=10.0):
    """A file-like object for the bridge at path: the named pipe on Windows, the Unix socket elsewhere."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            if WINDOWS:
                return open(pipe_name(path), "r+b", buffering=0)
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.2)


class Bridge:
    """One connection to a game's bridge. `transport` is a connected socket or an open pipe (binary, unbuffered).
    A reader thread hands each reply to the request with its id; events (subscribe) are dropped."""

    def __init__(self, transport):
        self.t = transport
        self.is_sock = isinstance(transport, socket.socket)
        self.lock = threading.Lock()
        self.waiting = {}
        self.next_id = 1
        self.closed = False
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    @classmethod
    def connect(cls, path, timeout=10.0):
        return cls(open_transport(path, timeout))

    def _pipe_waiting(self):
        """(Windows) bytes waiting in the pipe, -1 when it is broken. A blocking read on a pipe opened without overlapped
        I/O holds the handle, so a write from another thread would wait for it: the reader peeks first."""
        import ctypes
        import msvcrt
        from ctypes import wintypes
        n = wintypes.DWORD()
        h = wintypes.HANDLE(msvcrt.get_osfhandle(self.t.fileno()))
        if not ctypes.windll.kernel32.PeekNamedPipe(h, None, 0, None, ctypes.byref(n), None):
            return -1
        return n.value

    def _chunk(self):
        if self.is_sock:
            return self.t.recv(65536)
        while not self.closed:
            n = self._pipe_waiting()
            if n < 0:
                return b""
            if n:
                return self.t.read(min(n, 65536))
            time.sleep(0.005)
        return b""

    def _read(self):
        buf = b""
        try:
            while True:
                chunk = self._chunk()
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    try:
                        msg = json.loads(line)
                    except ValueError:
                        continue
                    if "ok" not in msg:
                        continue
                    with self.lock:
                        q = self.waiting.pop(msg.get("id"), None)
                    if q:
                        q.put(msg)
        except (OSError, ValueError):
            pass
        self.closed = True
        with self.lock:
            for q in self.waiting.values():
                q.put(None)
            self.waiting.clear()

    def request(self, cmd, reply_timeout=30.0, check=True, **args):
        """Sends one request and waits for its reply (a dict). check: an error reply raises BridgeError."""
        q = queue.Queue()
        with self.lock:
            if self.closed:
                raise ConnectionError("bridge closed")
            rid = self.next_id
            self.next_id += 1
            self.waiting[rid] = q
            data = (json.dumps(dict(args, id=rid, cmd=cmd), separators=(",", ":")) + "\n").encode()
            if self.is_sock:
                self.t.sendall(data)
            else:
                self.t.write(data)
        try:
            reply = q.get(timeout=reply_timeout)
        except queue.Empty:
            with self.lock:
                self.waiting.pop(rid, None)
            raise TimeoutError("%s: no reply in %.0f s" % (cmd, reply_timeout))
        if reply is None:
            raise ConnectionError("bridge closed during " + cmd)
        if check and not reply.get("ok"):
            raise BridgeError(reply)
        return reply

    def close(self):
        self.closed = True
        try:
            if self.is_sock:
                self.t.shutdown(socket.SHUT_RDWR)
            self.t.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------------------------- the player

class Log:
    """The player's log: lines like the emulator hook's, each with the local time."""

    def __init__(self, path=None, echo=None):
        self.path = path
        self.echo = echo
        self.lock = threading.Lock()
        self.lines = []

    def __call__(self, text):
        line = "%s %s" % (time.strftime("%H:%M:%S"), text)
        with self.lock:
            self.lines.append(text)
            if self.path:
                with open(self.path, "a", encoding="utf-8") as f:
                    f.write(line + "\n")
        if self.echo:
            self.echo(line)


def _wrap(a):
    while a > math.pi:
        a -= 2 * math.pi
    while a < -math.pi:
        a += 2 * math.pi
    return a


class Player:
    """Plays steps into one game. clock and sleep are for the tests."""

    def __init__(self, bridge, log, shot=None, connect=None, clock=time.monotonic, sleep=time.sleep,
                 main_base=MAIN_BASE, steer_every=0.1, warp_for=90.0, commands=None, take_for=5.0, read_log=None,
                 await_every=0.25, burst_path=None):
        self.b = bridge
        self.burst_path = burst_path  # burst_path(name) -> a PNG path for one of this game's burst shots, or None
        self.read_log = read_log      # () -> this game's log text so far ('' if none yet), or None: await skipped
        self.await_every = await_every
        self.commands = commands      # the game's LICHII_COOP_TEST_COMMANDS file, or None: module steps skipped
        self.take_for = take_for      # seconds the module has to take them (it checks 4 times a second)
        self.sent = 0
        self.send_lock = threading.Lock()   # the commands file: the script's batches and a warp's hand-over
        self.log = log
        self.shot = shot              # shot(name): shots at a mark
        self.connect = connect        # a second connection (the warp's own), or None: warps share this one
        self.clock = clock
        self.sleep = sleep
        self.main_base = main_base
        self.steer_every = steer_every
        self.warp_for = warp_for
        self.mem_ok = None            # guest memory reads answered (None: not tried)
        self.warps = []
        self.warp_failed = []         # "ASKED -> GOT": warps the bridge sent to another map (the run FAILs on them)
        self.not_taken = []           # marks / test commands the module never read (the run FAILs on them: B #137)
        self.angle = 0.0              # camera model: world direction = stick direction turned by this
        self.learned = False

    # time
    def _until(self, t):
        while True:
            left = t - self.clock()
            if left <= 0:
                return
            self.sleep(min(left, 0.25))

    # inputs
    def _hold(self, buttons, left, right, ticks):
        """Starts the inputs (held well past ticks: let go by release)."""
        frames = max(1, min(216000, int(ticks * 4) + 60))
        sent = False
        if left is not None:
            self.b.request("stick", stick="L", x=left[0], y=left[1], buttons=buttons or [], frames=frames,
                           wait=False)
            sent = True
        if right is not None:
            self.b.request("stick", stick="R", x=right[0], y=right[1],
                           buttons=[] if sent else (buttons or []), frames=frames, wait=False)
            sent = True
        if not sent and buttons:
            self.b.request("press", buttons=buttons, frames=frames, wait=False)
            sent = True
        return sent

    def _release(self):
        self.b.request("release")

    def timed(self, s, t0):
        """A wait, hold, press half or stick: inputs from t0 for s.ticks, let go at the end."""
        end = t0 + s.ticks / TICKS
        held = self._hold(s.buttons, s.left, s.right, s.ticks) if s.ticks else False
        if s.name:   # hold KEYS TICKS MARK: the mark halfway, keys still down
            self._until(t0 + s.ticks / TICKS / 2)
            self.mark(s.name)
        self._until(end)
        if held:
            self._release()
        return end

    def mark(self, name):
        self.log("coop: test mark " + name)
        # The same mark in the game's own log (the module writes "coop: test mark NAME (Link at x y z)"), so a
        # grading line like "test mark near.*voice: ..." finds it in order with the game's lines: groups.py reads
        # hookshot_log-X.txt, as it reads the emulator's log.
        if self.commands and not self._send(["mark " + name]):
            self.log("coop: test mark %s NOT taken (no module reading LICHII_COOP_TEST_COMMANDS?)" % name)
            self.not_taken.append("mark " + name)
        if self.shot:
            try:
                self.shot(name)
            except Exception as e:   # (a shot that fails does not stop the script)
                self.log("coop: test shot %s failed: %s" % (name, e))

    # Link's position
    def link_pos(self):
        """(x, y, z) of Link from guest memory, or None (no memory access, or not known yet)."""
        if self.mem_ok is False:
            return None
        try:
            r = self.b.request("mem_read", addr=self.main_base + GAMEUI_SLOT, size=8)
            ptr = struct.unpack("<Q", bytes.fromhex(r["data"]))[0]
            if not ptr:
                return None
            r = self.b.request("mem_read", addr=ptr + POS_VALID, size=POS + 12 - POS_VALID)
            data = bytes.fromhex(r["data"])
        except BridgeError as e:
            if e.code in ("forbidden", "unavailable"):
                self.mem_ok = False
                self.log("coop: test no guest memory (%s): start the game with HOS_CONTROL_MEM=ro" % e.code)
                return None
            if e.code == "out_of_range":
                return None
            raise
        self.mem_ok = True
        if not data[0] & 1:
            return None
        return struct.unpack("<3f", data[POS - POS_VALID:])

    def steer(self, s, t0):
        """goto (until within s.radius or s.ticks) or toward (for s.ticks). The stick is camera-relative: the camera
        model (an angle) is learnt from how Link moves for a push; a goto that gets nowhere for a second steps aside
        (a toward never does: it pushes straight on, into a block, a grave or a crawl hole)."""
        end = t0 + s.ticks / TICKS
        if self.link_pos() is None and self.mem_ok is False:
            self.log("coop: test %s skipped (no position on hookshot without guest memory)" % s.kind)
            return t0
        # The step's keys (toward X Z TICKS A: a grab) are held for the whole step on a connection of their own:
        # the stick below is let go and sent again every steer_every, and a release lets go of every input of
        # its connection (Longshot docs/control.md "release": another client's inputs stay). With the keys on
        # the stick's connection the grab was dropped and pressed again 4 times a second, so a pull never
        # happened (deku-block-147-probe, #147: B's Link let go and walked 3 m back from the block).
        keys = self.connect() if s.buttons and self.connect else None
        stick_keys = None if keys else s.buttons
        if keys:
            keys.request("press", buttons=s.buttons, frames=max(1, min(216000, int(s.ticks * 4) + 60)), wait=False)
        last_pos, last_push, stuck_since, detour_until, detour = None, None, None, 0.0, 1
        held = False
        try:
            while True:
                now = self.clock()
                if now >= end:
                    if s.kind == "goto":
                        self.log("coop: test goto timed out")
                    break
                pos = self.link_pos()
                if pos is None:
                    self._until(now + self.steer_every)
                    continue
                dx, dz = s.x - pos[0], s.z - pos[2]
                dist = math.hypot(dx, dz)
                if s.kind == "goto" and dist <= s.radius:
                    self.log("coop: test goto reached %.1f %.1f" % (pos[0], pos[2]))
                    break
                if last_pos is not None and last_push is not None:
                    mx, mz = pos[0] - last_pos[0], pos[2] - last_pos[2]
                    moved = math.hypot(mx, mz)
                    if moved > 0.05:
                        # world (x, -z) is the stick's frame turned by self.angle (stick up = away from the camera)
                        seen = _wrap(math.atan2(-mz, mx) - math.atan2(last_push[1], last_push[0]))
                        self.angle = seen if not self.learned else _wrap(self.angle + 0.5 * _wrap(seen - self.angle))
                        self.learned = True
                        stuck_since = None
                    elif stuck_since is None:
                        stuck_since = now
                    elif s.kind == "goto" and now - stuck_since >= 1.0 and now >= detour_until:
                        # Only goto walks round what stops it. A toward that stops is pushing (a block, a grave,
                        # into a crawl hole): the game needs the stick held straight on, and a step aside slid
                        # Link off the Deku block's face (big-run-0115 deku-block, #147: the block never moved).
                        detour_until, detour, stuck_since = now + 0.5, -detour, None
                want = math.atan2(-dz, dx) - self.angle
                if now < detour_until:
                    want += detour * math.pi / 2
                mag = 1.0 if s.kind == "toward" else max(0.45, min(1.0, dist / 3.0))
                push = (round(math.cos(want) * mag, 3), round(math.sin(want) * mag, 3))
                if held:
                    self._release()
                self._hold(stick_keys, push, None, self.steer_every * TICKS * 2)
                held = True
                last_pos, last_push = pos, push
                self._until(min(end, now + self.steer_every))
        finally:
            if held:
                self._release()
            if keys:
                keys.request("release")
                keys.close()
        return self.clock()

    # warps: the bridge's teleport, asked again until the game takes it (it refuses during a load or an event).
    # The bridge knows only its own list of maps (Longshot host/hos_main.c: the fields, villages and dungeon
    # entrances), each at the map's own start. A map it does not know (not_found: the boss rush's <Boss>Stage
    # scenes in Banc/DreamWorld, big-run-0114-3 and big-run-0115: all ten native bosses never got there) goes to
    # the module's own warp instead (lichii/hook/coop_testinput.cpp Warp: the scene and the start written into
    # the game's next-scene request, asked again every 0.5 s for 90 s until Link is there; it logs "coop: test
    # warp done after N s" itself). The script's line goes as written, so its start is used there.
    def warp(self, s):
        said_start = [False]

        def by_module(why):
            if not self.commands:
                self.log("coop: test warp %s gave up: the bridge has no such map (%s) and there is no module "
                         "commands file for the module's own warp" % (s.name, why))
                return
            taken = self._send([s.text])
            self.log("coop: test warp %s: not one of the bridge's maps (%s): %s the module's own warp (scene %s, "
                     "start %s; its log says 'test warp done')" % (s.name, why, "asked through" if taken else
                                                                   "NOT taken by", s.name, s.start or "-"))

        def ask():
            b = self.connect() if self.connect else self.b
            deadline = self.clock() + self.warp_for
            why = "no reply"
            try:
                while self.clock() < deadline:
                    try:
                        r = b.request("teleport", reply_timeout=15.0, to=s.name, timeout=2)
                    except BridgeError as e:
                        why = e.code
                        if e.code == "bad_request":
                            self.log("coop: test warp %s failed: %s" % (s.name, e))
                            return
                        if e.code == "not_found":   # (not a map of the bridge's list: never will be)
                            by_module(e.code)
                            return
                        self.sleep(0.5)
                        continue
                    if r.get("sent"):
                        to = r.get("map") or s.name
                        if to.lower() != s.name.lower():
                            # The bridge's teleport keeps the game's age: `warp HyruleField_Adult` in a child game
                            # goes to HyruleField_Child ('sent (map HyruleField_Child, not accepted)'), and waiting for
                            # that map used to say 'warp done' (epona-sync-1db5e721-r2: B child, no Epona, graded as
                            # if the scenario had played). Another map than the one asked is a failed warp.
                            self.warp_failed.append("%s -> %s" % (s.name, to))
                            self.log("coop: test warp %s FAILED: the bridge sends the game to map %s, not the one "
                                     "asked (it keeps the game's age: set Sequence_IsChildhood after the save is "
                                     "loaded, then warp)" % (s.name, to))
                            return
                        if s.start and not said_start[0]:
                            said_start[0] = True
                            self.log("coop: test warp %s: start %s not used on hookshot (the map's own)"
                                     % (s.name, s.start))
                        self.log("coop: test warp %s: sent (map %s%s)" % (
                            s.name, to, ", not accepted" if r.get("accepted") is False else ""))
                        # "sent" is not "moved": a teleport asked while the arrival event of the save's load still
                        # plays is dropped by the game (Longshot rc.3: "result 0", Link stays where the save put
                        # him). So wait for the map, and ask again when it does not come.
                        # Not accepted: usually dropped, so ask again soon (the game refuses a teleport within 8 s
                        # of the last one); the map may still come (seen once), so wait for it that long first.
                        if self._arrived(b, to, deadline, 9.0 if r.get("accepted") is False else 30.0):
                            # In the map is not Link at its age: epona-sync-2195cbd1 landed in HyruleField_Adult
                            # (Epona there) with the flag set and Link still a child. The age is read back from the
                            # bridge's status (game.age) for up to 20 s; a Link of the other age FAILs the warp.
                            got = self._age_after(b, to)
                            if got:
                                self.warp_failed.append("%s -> Link is %s" % (s.name, got))
                                self.log("coop: test warp %s FAILED: in the map, but Link is still a %s (the map's "
                                         "age is the other one): the age flag did not take" % (s.name, got))
                                return
                            self.log("coop: test warp done (map %s)" % to)
                            return
                        why = "map %s not reached" % to
                        self.log("coop: test warp %s: %s, asking again" % (s.name, why))
                        continue
                    self.sleep(0.5)
                self.log("coop: test warp %s gave up after %.0f s (%s)" % (s.name, self.warp_for, why))
            finally:
                if b is not self.b:
                    b.close()

        t = threading.Thread(target=ask, daemon=True)
        t.start()
        self.warps.append(t)

    def _age_after(self, b, to, within=20.0):
        """The age Link has when it is not the one `to`'s name ends with (_Child / _Adult) after `within` s; "" when it
        is, when the name has no age, or when the bridge's status has no readable game.age (not judged)."""
        want = "child" if to.endswith("_Child") else "adult" if to.endswith("_Adult") else ""
        if not want:
            return ""
        end, got = self.clock() + within, ""
        while True:
            try:
                age = ((b.request("status") or {}).get("game") or {}).get("age")
            except (BridgeError, TimeoutError):
                age = None
            if not isinstance(age, str) or not age:
                return ""
            if age.lower() == want:
                return ""
            got = age.lower()
            if self.clock() >= end:
                return got
            self.sleep(0.5)

    def _arrived(self, b, to, deadline, arrive_for=30.0):
        """True once the game is in map `to` (the bridge's `wait map`); False if it is not within arrive_for s."""
        left = min(arrive_for, deadline - self.clock())
        if left <= 0:
            return False
        try:
            b.request("wait", reply_timeout=left + 10.0, event="map", map=to, timeout=left)
            return True
        except BridgeError as e:
            if e.code == "timeout":
                return False
            if e.code in ("bad_request", "unknown_cmd"):   # a bridge without `wait map`: trust "sent"
                return True
            self.log("coop: test warp: wait for map %s: %s" % (to, e))
            return False   # asked again until warp_for, then the "gave up" line
        except TimeoutError:
            return False

    # the module's test commands: written to its file, taken when it is empty again
    def module(self, batch):
        if not self.commands:
            for s in batch:
                self.log("coop: test %s skipped on hookshot (line %d: needs the co-op module's test commands)"
                         % (s.kind, s.line))
            return
        taken = self._send([s.text for s in batch])
        if not taken:
            self.not_taken.extend("%s (line %d)" % (s.kind, s.line) for s in batch)
        for s in batch:
            self.log("coop: test %s %s (line %d: %s)" % (s.kind, "sent" if taken else "NOT taken", s.line,
                                                         "the module's log says what it did" if taken else
                                                         "no module reading LICHII_COOP_TEST_COMMANDS?"))

    def _send(self, lines):
        """Writes lines to the module's commands file; True once the module took them (emptied the file)."""
        with self.send_lock:   # (a warp's thread hands its line over too)
            return self._send_locked(lines)

    def _send_locked(self, lines):
        deadline = self.clock() + self.take_for
        while self._waiting() and self.clock() < deadline:   # the last batch not taken yet
            self.sleep(0.05)
        self.sent += 1
        tmp = "%s.%d.tmp" % (self.commands, self.sent)
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write("# hookshot.py batch %d\n" % self.sent)
            for line in lines:
                f.write(line + "\n")
        os.replace(tmp, self.commands)
        deadline = self.clock() + self.take_for
        while self._waiting() and self.clock() < deadline:
            self.sleep(0.05)
        return not self._waiting()

    def _waiting(self):
        try:
            return os.path.getsize(self.commands) > 0
        except OSError:
            return False

    def _log_matches(self, rx):
        try:
            text = self.read_log() or ""
        except OSError:
            return 0, ""
        found = [ln.rstrip("\r") for ln in text.split("\n") if rx.search(ln.rstrip("\r"))]
        return len(found), (found[-1] if found else "")

    def await_line(self, s, t0):
        """await TICKS REGEX (row 325): from t0 until this game's log has more lines matching REGEX than it had at t0
        (a line written since), at most s.ticks. Logs the line seen or the timeout; the script goes on either way."""
        if self.read_log is None:
            self.log("coop: test await skipped (line %d: this run reads no game log)" % s.line)
            return t0
        self._until(t0)
        rx = re.compile(s.name)
        before, _ = self._log_matches(rx)
        end = t0 + s.ticks / TICKS
        while True:
            n, last = self._log_matches(rx)
            now = self.clock()
            if n > before:
                self.log("coop: test await seen after %.1f s: %s" % (now - t0, last.strip()))
                return now
            if now >= end:
                self.log("coop: test await timed out after %.1f s (line %d: %s)" % (now - t0, s.line, s.name))
                return now
            self._until(min(end, now + self.await_every))

    def burst(self, s, t0):
        """burst NAME FRAMES [TICKS REGEX] (issue 166): this game held, then one frame run and one shot at a time.
        No REGEX: FRAMES shots. REGEX: shots from t0 on, the last BURST_BEFORE kept (older ones deleted), until a new
        line of this game's log matches (at most TICKS frames), then FRAMES more. Let go after, whatever happened."""
        self._until(t0)
        if self.burst_path is None:
            self.log("coop: test burst %s skipped (line %d: this run takes no shots)" % (s.name, s.line))
            return t0
        rx = re.compile(s.start) if s.start else None
        if rx is not None and self.read_log is None:
            self.log("coop: test burst %s skipped (line %d: this run reads no game log)" % (s.name, s.line))
            return t0
        before = self._log_matches(rx)[0] if rx is not None else 0
        kept, taken, seen_at, after, frame = [], 0, None if rx is not None else 0, 0, None
        try:
            self.b.request("pause")
            while True:
                if seen_at is None and taken > s.ticks:
                    self.log("coop: test burst %s: no line after %d frames (line %d: %s)" % (s.name, taken, s.line, s.start))
                    break
                if seen_at is not None and after >= s.radius:
                    break
                r = self.b.request("step", frames=1)
                frame = r.get("frame", frame)
                path = self.burst_path("%s-%03d" % (s.name, taken))
                self.b.request("shot", reply_timeout=30.0, path=path, timeout=20)
                taken += 1
                if seen_at is None:
                    kept.append(path)
                    if len(kept) > BURST_BEFORE:
                        old = kept.pop(0)
                        try:
                            os.remove(old)
                        except OSError:
                            pass
                    n, last = self._log_matches(rx)
                    if n > before:
                        seen_at = taken
                        self.log("coop: test burst %s: line seen at shot %d (frame %s): %s" % (s.name, taken - 1, frame,
                                                                                         last.strip()))
                else:
                    after += 1
        except Exception as e:   # a shot or step that fails ends the burst, not the script
            self.log("coop: test burst %s failed at shot %d: %s" % (s.name, taken, e))
        finally:
            try:
                self.b.request("resume")
            except Exception as e:
                self.log("coop: test burst %s: resume failed: %s" % (s.name, e))
        self.log("coop: test burst %s: %d shots, last frame %s" % (s.name, taken, frame))
        return self.clock()

    def skip_to(self, s, steps, i, since):
        """skipto MARK REGEX: the index of the later `mark MARK` when this game's log has more lines matching REGEX
        than it had at the last mark played (since[REGEX]), else None. No log: never skips."""
        if self.read_log is None:
            self.log("coop: test skipto skipped (line %d: this run reads no game log)" % s.line)
            return None
        n, last = self._log_matches(re.compile(s.start))
        if n <= since.get(s.start, 0):
            self.log("coop: test skipto %s: not seen, going on (line %d)" % (s.name, s.line))
            return None
        to = next(j for j in range(i, len(steps)) if steps[j].kind == "mark" and steps[j].name == s.name)
        self.log("coop: test skipto %s: seen (%s), %d steps not played" % (s.name, last.strip(), to - i))
        return to

    def play(self, steps):
        self.log("coop: test script %d steps" % len(steps))
        # skipto counts only lines written since the last mark played: a try starts at a mark (getting on again
        # logs the same lines as getting on the first time). Before the first mark: since the script began.
        since = {}
        rxs = sorted({s.start for s in steps if s.kind == "skipto"}) if self.read_log is not None else []

        def snapshot():
            for rx in rxs:
                since[rx] = self._log_matches(re.compile(rx))[0]

        snapshot()
        t = self.clock()
        i = 0
        while i < len(steps):
            s = steps[i]
            i += 1
            if s.kind in MODULE_ONLY:
                # Steps in a row go together: the emulator plays them in one frame.
                batch = [s]
                while i < len(steps) and steps[i].kind in MODULE_ONLY:
                    batch.append(steps[i])
                    i += 1
                self._until(t)
                self.module(batch)
                t = max(t, self.clock())
                continue
            if s.kind == "timed":
                t = self.timed(s, t)
            elif s.kind == "mark":
                self._until(t)
                self.mark(s.name)
                snapshot()
                # The mark waits for the module to take it (usually a quarter second, at most 2 x take_for): the
                # next step starts after it, or a short press scheduled before it is let go at once (a title press lost).
                t = max(t, self.clock())
            elif s.kind in ("goto", "toward"):
                t = max(t, self.steer(s, max(t, self.clock())))
            elif s.kind == "await":
                t = max(t, self.await_line(s, max(t, self.clock())))
            elif s.kind == "skipto":
                self._until(t)
                to = self.skip_to(s, steps, i, since)
                if to is not None:
                    i = to
                t = max(t, self.clock())
            elif s.kind == "burst":
                t = max(t, self.burst(s, max(t, self.clock())))
            elif s.kind == "warp":
                self.warp(s)
        self.log("coop: test script done")
        return t


# ---------------------------------------------------------------------------------------------- the two-game run

def wait_warps(players, slack=30.0):
    """Waits for every warp thread of each Player (`hookshot.py run`, after its scripts ended): a warp asks on until it
    is done, failed or gave up (warp_for s), and a run that ended first left it unasked (test-bridge-pilot-fa63e90f).
    RuntimeError for a warp still alive after warp_for + slack s."""
    for x, p in sorted(players.items()):
        for t in p.warps:
            t.join(p.warp_for + slack)
            if t.is_alive():
                raise RuntimeError("game %s: a warp still asking after %.0f s" % (x, p.warp_for + slack))


def to_play(bridge, log, timeout, clock=time.monotonic, sleep=time.sleep):
    """Title and file select to the world: A pressed every 2 s until the bridge says play with a map known."""
    deadline = clock() + timeout
    last, said = {}, clock()
    while clock() < deadline:
        try:
            s = bridge.request("status")
        except BridgeError:
            s = {}
        last = s
        game = s.get("game") or {}
        if s.get("state") == "play" and s.get("map") and not game.get("loading") and not game.get("event"):
            log("coop: test loaded (map %s)" % s.get("map"))
            return s
        # (what it waits for, every 30 s: a game that is drawn but never "play with a map, not loading, no event" is
        # told apart from one that is not there; player-<x>.txt had nothing when the load failed)
        if clock() - said >= 30:
            said = clock()
            log("coop: test waiting for play: state=%s map=%s loading=%s event=%s frame=%s" %
                (s.get("state"), s.get("map"), game.get("loading"), game.get("event"), s.get("frame")))
        if s.get("frame", 0) > 0:
            bridge.request("press", buttons=["A"], frames=8, wait=False, check=False)
        sleep(2.0)
    raise TimeoutError("not in play after %d s (last status: state=%s map=%s loading=%s event=%s)" %
                       (timeout, last.get("state"), last.get("map"), (last.get("game") or {}).get("loading"),
                        (last.get("game") or {}).get("event")))


# The renderer's start is done: its pipelines are made (from the caches or compiled now). Until then the game shows
# nothing and reads no controller; with a cold driver cache that is minutes (~170 s seen), so a run waits for it.
# Longshot rc.4 builds pipelines at their create (RX_VK_LOAD=create) and logs "vk: start-up: the shaders took N s"
# instead of rc.3's precompile total.
# The renderer is up once Longshot's rx_start logs "rx: renderer: Vulkan (longshot_vk.dll)": after the back end's start()
# returned, on every path (window or not; host/rx_backend.c rx_start, v3.0.0-rc.4 to v3.2.0). NOT the precompile's
# summary line: that one comes BEFORE the start-up ends (the pipeline cache's save, the worker's end, the start screen's
# fade; gpu/vk/vk_main.cpp prepare_with_screen). party-hud-bridge-85e3b255 (lane 2, B #137 14:57): B logged the summary,
# then its start-up never ended (no "start-up: the shaders took", prep_running still set at the quit), but it was called
# ready, its window placed and its boot sent; the module never started and live.py start said ok.
RENDERER_READY = re.compile(r"rx: renderer: Vulkan \(")
RENDERER_FAILED = re.compile(r"rx: .*nothing is drawn|rx: the Vulkan back end could not start")
RENDERER_PRECOMPILED = re.compile(r"vk: precompile: \d+ programs, .* in \d+ ms")


def read_text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


# While it compiles, the game logs its progress about every 10 s ("vk: start-up: pipelines 6442 of 36753, 18% of all",
# "vk: noted pipelines: 6651 of 32625 pipelines in 124 s"). How long the compile takes is not fixed: on Windows NVIDIA
# the driver's shader cache (one GLCache folder for the user) serves only the first game that opens it, so every other
# game compiles cold at every start (~440 s on Longshot rc.7, ~230 s on rc.4, longer with several compiling at once).
# A fixed deadline is wrong for some of them, so the wait follows the progress: it fails only when the count stops
# rising for RENDERER_STALL s, or at RENDERER_LIMIT s in all.
# Longshot rc.4 to rc.7 write "start-up: pipelines N of M"; later builds write "start-up: shaders N of M".
RENDERER_PROGRESS = re.compile(r"vk: (start-up: (?:pipelines|shaders)|noted pipelines:) (\d+) of (\d+)")
RENDERER_STALL = 180    # s without a new progress count (it comes every ~10 s while the compile moves)
RENDERER_LIMIT = 3600   # s in all, progress or not (a new game folder's first start: ~18 min)


def renderer_progress(text):
    """(sum of the newest count of each progress kind, the newest progress line or '') of a game's log so far."""
    last, line = {}, ""
    for m in RENDERER_PROGRESS.finditer(text):
        last[m.group(1)] = int(m.group(2))
        line = m.group(0)
    return sum(last.values()), line


def wait_renderer(read_log, alive, timeout, stall=RENDERER_STALL, limit=RENDERER_LIMIT, clock=time.monotonic,
                  sleep=time.sleep):
    """Until read_log() has the renderer's ready line (RENDERER_READY); the seconds waited. 0 = no wait.
    timeout: the wait with no compile progress logged at all (a build without progress lines waits this long);
    while the progress count rises it waits on, until `stall` s pass without a rise, and never past `limit` s."""
    if timeout <= 0:
        return 0.0
    t0 = clock()
    done, rose, line = 0, None, ""
    while True:
        text = read_log()
        if RENDERER_READY.search(text):
            return clock() - t0
        bad = RENDERER_FAILED.search(text)
        if bad:
            raise RuntimeError("the renderer did not start: %s" % bad.group(0))
        if not alive():
            raise RuntimeError("the game closed before its renderer started")
        now = clock()
        count, newest = renderer_progress(text)
        if count > done:
            done, rose, line = count, now, newest
        deadline = t0 + timeout if rose is None else max(t0 + timeout, rose + stall)
        if now - t0 > max(limit, timeout):
            raise TimeoutError("renderer not started in %d s (still compiling: %s)" % (max(limit, timeout),
                                                                                       line or "no progress line"))
        if now > deadline:
            if RENDERER_PRECOMPILED.search(text):
                raise TimeoutError("renderer not started: the precompile ended but Longshot's start-up did not (no "
                                   "'rx: renderer: Vulkan' line for %d s after the last progress; party-hud-bridge-85e3b255)"
                                   % stall)
            if rose is None:
                raise TimeoutError("renderer not started in %d s (no compile progress logged)" % timeout)
            raise TimeoutError("renderer not started: no compile progress for %d s (last: %s, %.0f s in)"
                               % (stall, line, now - t0))
        sleep(0.5)


def add_renderer_options(p, wait=600):
    """--renderer-wait/-stall/-limit (wait_renderer): hookshot.py run and hs_steps.py run."""
    p.add_argument("--renderer-wait", type=int, default=wait,
                   help="seconds each game may take to start its renderer with no compile progress logged "
                        "(0: no wait)")
    p.add_argument("--renderer-stall", type=int, default=RENDERER_STALL,
                   help="while the compile logs progress: seconds without a rise before it fails")
    p.add_argument("--renderer-limit", type=int, default=RENDERER_LIMIT,
                   help="seconds in all for one game's renderer start, progress or not")


# What every test game gets unless the run's switches say otherwise (hs_steps.py TEST_GAME_DEFAULTS, the same):
# - LICHII_COOP_TEST_OWN_SAVE=1: each game plays the save its scenario put in (the joiner would load the host's).
# - LICHII_COOP_PROBLEM_ASK=0: no "The game crashed last time" page on the title. A test game folder keeps the crash
#   records of earlier runs (a killed or crashed proof), so the page came up at a run's first title and took the
#   load's A presses (smoke hs-players, kit-0119: B left on the title's Host page). A scenario that tests the ask
#   turns it on in its switches.
# Row 260: what game A gets for --host-in-game: co-op off at its start (no LICHII_COOP_SERVER), the in-game host's
# port and password for its `menu host` step (hookshot/src/coop_host_menu.h ParseTestHost) and its name.
def host_in_game_env(port, password):
    return {"LICHII_COOP_TEST_HOST": "%d:%s" % (port, password), "LICHII_COOP_NAME": "A"}


TEST_GAME_DEFAULTS = (("LICHII_COOP_TEST_OWN_SAVE", "1"), ("LICHII_COOP_PROBLEM_ASK", "0"))


def test_game_defaults(env):
    """env with TEST_GAME_DEFAULTS set where it has none of its own (the caller's environment or a switch wins)."""
    for k, v in TEST_GAME_DEFAULTS:
        env.setdefault(k, v)
    return env


def popen_flags():
    """Popen keywords for a game: its own process group, its window shown without being activated (placement.py).
    Kit mode (--kit-a/--kit-b) starts the game in the kit's play.ps1, which does the same (Start-KitTestGame)."""
    return placement.popen_flags()


def place_games(pids, display, say, wait=0.0, topmost=False):
    """Puts the games' windows side by side on the chosen monitor (placement.py): A in the first place, B in the
    second, whichever is placed first. pids: {tag: process id}. A game whose window is not there yet (after wait
    seconds) is left for the next call; a failure is said, never fatal."""
    if not WINDOWS or not pids:
        return
    tags = sorted(pids)
    try:
        placed = placement.place_pids([pids[x] for x in tags], display, topmost=topmost, wait=wait,
                                      slots=["AB".index(x) for x in tags], count=2)
    except Exception as e:   # (a placement problem must not end a run that has started: said in the steps)
        say("window placement: %s" % e)
        return
    by_pid = {pids[x]: x for x in tags}
    used = placement.check_display(display)["name"]   # (the monitor really used: the primary one after a fallback)
    for pid, rect in placed:
        say("%s: window placed at %d,%d %dx%d on %s" % (by_pid[pid], rect[0], rect[1], rect[2], rect[3],
                                                         used))


SAVE_USER = "5a3c9e1f0b7d24680000000000000001"   # the user folder a Hookshot game reads its saves from


def restore_save(game):
    """<game>/autotest-save (the baseline) copied over the game's saves, as the emulator kits' saves are. The baseline
    is either a copy of <game>/user/save (user folders inside) or the save files themselves (account.sav, option.sav,
    slot_NN/: the same files as an emulator kit's autotest-save), which go to user/save/<SAVE_USER>."""
    base = os.path.join(game, "autotest-save")
    if not os.path.isdir(base):
        return False
    names = os.listdir(base)
    flat = any(n.endswith(".sav") or n.startswith("slot_") for n in names)
    dest = os.path.join(game, "user", "save", SAVE_USER) if flat else os.path.join(game, "user", "save")
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    shutil.copytree(base, dest)
    return True


def exe_dir(game):
    return os.path.join(game, "bin") if os.path.isfile(os.path.join(game, "bin", "longshot.exe")) else game


def log_candidates(game):
    """Where the game's longshot_log.txt can be: beside the exe or in the game folder (rc.3), or in their logs\\ (rc.4)."""
    out = []
    for d in (exe_dir(game), game):
        for p in (os.path.join(d, "longshot_log.txt"), os.path.join(d, "logs", "longshot_log.txt")):
            if p not in out:
                out.append(p)
    return out


def find_log(game):
    """The newest of the game's longshot_log.txt files (an older one elsewhere is from another build), else None."""
    found = [p for p in log_candidates(game) if os.path.isfile(p)]
    return max(found, key=os.path.getmtime) if found else None


# ------------------------------------------------------------------------------- the pack's start, as the kit's
# What the kit's start does to the puppet pack before each game (play.ps1: Use-KitPuppetPack, then
# Invoke-KitSizeTableMerge; start.sh the same), for a game this file starts itself: your own Link's tunic files in
# the colour it will wear (tools/puppet_pack/self_tunic.py), THEN the merge (tools/puppet_pack/merge_tables.py),
# whose row-202 step gives another mod's Link model (Character Select's) co-op's own tunic names only when those
# files are there. Row 265 (0.11.12): a lane started without them wore Character Select's model on the game's shared
# tunic textures, so Customize's Kokiri row recoloured only the belt and boots. Never stops the start.
PACK_REL = ("mods", "contents", "0400486024DEC000", "coop-puppet")


def pack_folders(game):
    """(mods folder, the pack, the game's romfs or '') of a game folder, as Invoke-KitSizeTableMerge finds them."""
    exe = exe_dir(game)
    mods = os.path.join(exe, "mods")
    if not os.path.isdir(mods):
        mods = os.path.join(game, "mods")
    pack = os.path.join(os.path.dirname(mods), *PACK_REL)
    romfs = next((r for r in (os.path.join(exe, "romfs"), os.path.join(os.path.dirname(exe), "romfs"),
                              os.path.join(game, "romfs")) if os.path.isdir(os.path.join(r, "Pack"))), "")
    return mods, pack, romfs


def pack_start(game, env, say):
    """The kit's start of the pack for one game (env: the game's switches). Returns the lines said."""
    mods, pack, romfs = pack_folders(game)
    if not os.path.isfile(os.path.join(pack, "coop_puppet.json")):
        return []
    tools = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if not os.path.isfile(os.path.join(tools, "puppet_pack", "self_tunic.py")):
        # (bigrun-0115: a copy of tools/autotest alone kept the game in green files)
        line = "own tunic files NOT set: tools/puppet_pack is missing beside tools/autotest"
        say(line)
        return [line]
    if tools not in sys.path:
        sys.path.insert(0, tools)
    lines = []
    try:
        from puppet_pack import merge_tables, self_tunic
        ini = next((p for p in (os.path.join(game, "coop-options.ini"), os.path.join(exe_dir(game), "coop-options.ini"))
                    if os.path.isfile(p)), os.path.join(game, "coop-options.ini"))
        color = self_tunic.start_color(env.get("LICHII_COOP_COLOR", ""), ini, os.path.join(game, "coop.ini"),
                                       env.get("LICHII_COOP_TUNICS", ""))
        got = self_tunic.put(pack, color)
        lines.append("own tunic files: %s" % (got or "none (the game's green)"))
        if env.get("OOT_TEST_NO_MERGE", "") == "1":
            # (row 288's proof: a game started again without the kit's merge, as the Insert menu's Restart now)
            lines.append("merge NOT run (OOT_TEST_NO_MERGE=1)")
        elif os.path.isdir(mods):
            merge_tables.main(["--mods", mods] + (["--romfs", romfs] if romfs else []))
    except Exception as e:  # (the kit's start never stops on it either)
        lines.append("pack start failed (%s: %s)" % (type(e).__name__, e))
    for line in lines:
        say(line)
    return lines


# ---------------------------------------------------------------------------------------------- the kit's own start
# Kit mode (run --kit-a/--kit-b, run.ps1 -Kit): each game is started by its kit's own start, play.ps1 choose -Hookshot
# as "Start Squadala.bat" runs it, so what a player's start does runs for real before the game: the update check
# aside (-NoUpdate), the module step (Get-KitHookshotNeed: co-op module, puppet pack, randomizer, seeds built again),
# coop-options.ini (the colour) and the pack's own-tunic step. The start answers 'n' to every question (a long game
# build is never started by a test: the start fails and says so). OOT_COOP_TEST_KIT keeps the test server and join
# password in the game's variables (the title menu's Join otherwise) and gets the game's process id back.

def kit_game_folder(kit):
    """The game folder the kit's start uses (kit-paths.ps1 Get-KitHookshotGame, without the other-kit check)."""
    where = os.path.join(kit, "hookshot", "game-folder.txt")
    g = ""
    if os.path.isfile(where):
        with open(where, encoding="utf-8-sig") as f:
            g = (f.readline() or "").strip()
    if not g:
        return os.path.join(kit, "hookshot", "game")
    return g if os.path.isabs(g) else os.path.join(kit, g)


def same_path(a, b):
    return os.path.normcase(os.path.abspath(a)).rstrip("\\/") == os.path.normcase(os.path.abspath(b)).rstrip("\\/")


def kit_start_command(kit):
    ps = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "WindowsPowerShell", "v1.0",
                      "powershell.exe") if WINDOWS else "pwsh"
    return [ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", os.path.join(kit, "scripts", "play.ps1"),
            "-Mode", "choose", "-Hookshot", "-NoUpdate"]


# A start may build the game without asking (a mod's code patches changed: 5 to 25 minutes, more on a slow PC), so the
# wait is a real player's. Past it the start's whole process tree is stopped (hs.run_tree): a build left running wrote
# its old game into the folder after the next build had (u2-fix: U2 from v0.11.10 ended on Longshot v3.0.0).
KIT_START_TIMEOUT = 3600


def kit_start(kit, env, out, x, timeout=KIT_START_TIMEOUT, run_cmd=None):
    """Runs the kit's start for game x with env; the started game's process id. Its window's text: kit-start-<x>.txt."""
    pidfile = os.path.join(out, "kit-pid-%s.txt" % x)
    if os.path.exists(pidfile):
        os.remove(pidfile)
    env = dict(env, OOT_COOP_KIT=kit, OOT_COOP_TEST_KIT=pidfile, OOT_COOP_NO_SHORTCUT="1")
    text_path = os.path.join(out, "kit-start-%s.txt" % x)
    with open(text_path, "wb") as so:
        try:
            r = (run_cmd or lanes.hs.run_tree)(kit_start_command(kit), cwd=kit, env=env, input=b"n\r\n" * 16,
                                               stdout=so, stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            r = None
    if r is None:
        last = " | ".join(l.strip() for l in read_text(text_path).splitlines() if l.strip())[-600:]
        raise RuntimeError("kit start of %s timed out after %d s (it and everything it started stopped): %s"
                           % (x, timeout, last))
    text = read_text(text_path)
    pid = ""
    if os.path.isfile(pidfile):
        pid = read_text(pidfile).strip()
    if r.returncode != 0 or not pid.isdigit():
        last = " | ".join(l.strip() for l in text.splitlines() if l.strip())[-600:]
        why = "exit %s" % r.returncode if r.returncode != 0 else "no game process id (a kit without OOT_COOP_TEST_KIT?)"
        raise RuntimeError("kit start of %s failed (%s): %s" % (x, why, last))
    return int(pid)


class PidProcess:
    """A game the kit's start started (play.ps1's Start-Process): known by its process id, waited on like a Popen."""

    def __init__(self, pid):
        self.pid, self.returncode, self._h = pid, None, None
        if WINDOWS:
            import ctypes
            self._k = ctypes.WinDLL("kernel32", use_last_error=True)
            self._k.OpenProcess.restype = ctypes.c_void_p
            # SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION
            self._h = self._k.OpenProcess(0x00100000 | 0x0001 | 0x1000, False, pid)
            if not self._h:
                self.returncode = -1   # gone already

    def poll(self):
        if self.returncode is None and WINDOWS:
            import ctypes
            if self._k.WaitForSingleObject(ctypes.c_void_p(self._h), 0) == 0:
                code = ctypes.c_ulong()
                self._k.GetExitCodeProcess(ctypes.c_void_p(self._h), ctypes.byref(code))
                self.returncode = code.value
        elif self.returncode is None:
            try:
                os.kill(self.pid, 0)
            except OSError:
                self.returncode = -1
        return self.returncode

    def wait(self, timeout=None):
        end = None if timeout is None else time.monotonic() + timeout
        while self.poll() is None:
            if end is not None and time.monotonic() > end:
                raise subprocess.TimeoutExpired("pid %d" % self.pid, timeout)
            time.sleep(0.2)
        return self.returncode

    def kill(self):
        if self.poll() is None:
            if WINDOWS:
                import ctypes
                self._k.TerminateProcess(ctypes.c_void_p(self._h), 1)
            else:
                os.kill(self.pid, 9)


def code_mods_mismatch(folder):
    """hs.py's check (the folder's mods' code patches against its game's build), or why it could not run; None: fine."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hookshot"))
    try:
        import hs
        return hs.code_mods_mismatch(folder)
    except Exception as e:   # (a check only: the run goes on)
        return "the mods' code patches could not be checked: %s" % e


def fresh_log(game, since):
    """The game's longshot_log.txt once this start wrote it (an older one in the folder is from another run), else ''."""
    p = find_log(game)
    try:
        return read_text(p) if p and os.path.getmtime(p) >= since - 2 else ""
    except OSError:
        return ""


def run(a):
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    say = Log(os.path.join(out, "steps.txt"), echo=print)
    # One game (only --game-a or only --game-b) runs alone: its own bridge, play-test port and log, as in a pair.
    # --only: one game into a live session (live.py relaunch).
    games = {x: os.path.abspath(g) for x, g in (("A", a.game_a), ("B", a.game_b)) if g}
    tags = [a.only] if getattr(a, "only", None) else list(games)
    kits = {"A": getattr(a, "kit_a", None), "B": getattr(a, "kit_b", None)}
    for x in tags:
        if kits[x]:
            kits[x] = os.path.abspath(kits[x])
            if not same_path(kit_game_folder(kits[x]), games[x]):
                # (else the kit would start another game than the one whose save was put back and whose log is read)
                raise SystemExit("kit %s starts %s, not game %s: point its hookshot\\game-folder.txt there "
                                 "(tools/autotest/kit_pair.py setup)" % (kits[x], kit_game_folder(kits[x]), games[x]))
    if WINDOWS:   # the monitor first: a wrong name fails here, before any game window can open anywhere else
        try:
            mon = placement.check_display(a.display)
        except placement.PlacementError as e:
            raise SystemExit("window placement: %s" % e)
        if mon.get("note"):
            say("window placement: %s" % mon["note"])
        say("windows go to %s (work area %s)" % (mon["name"], mon["work"]))
    started = {}
    scripts = {"A": a.script_a, "B": a.script_b}
    parsed = {}
    try:   # (before any game starts: a bad line, a bad `song` among them, is a setup failure, not a 30-min run)
        for x in tags:
            if scripts[x]:
                parsed[x] = parse(read_script(scripts[x]))
        load = None
        if a.load:
            load = parse(read_script(a.load))   # (checked; the load itself is "A until play", see to_play)
    except ScriptError as e:
        raise SystemExit("SETUP-FAIL script: %s" % e)
    procs, bridges, server, commands = {}, {}, None, {}
    failed = None

    def keep_pids():
        # --keep-open: the games and the server outlive this run, so their pids go to pids.json in the run folder for
        # whoever closes them later (tools/autotest/live.py's watchdog). Written again as each one starts.
        if not a.keep_open:
            return
        try:
            pids = {"games": {x: p.pid for x, p in procs.items()}, "server": server.pid if server else None}
            if getattr(a, "only", None):   # the session's other game and its server stay as they are
                try:
                    with open(os.path.join(out, "pids.json"), encoding="utf-8") as f:
                        old = json.load(f)
                except (OSError, ValueError):
                    old = {}
                pids = {"games": dict(old.get("games") or {}, **pids["games"]),
                        "server": pids["server"] or old.get("server")}
            with open(os.path.join(out, "pids.json"), "w", encoding="utf-8") as f:
                json.dump(pids, f)
        except OSError as e:
            say("pids.json not written: %s" % e)

    try:
        if a.server:
            with open(os.path.join(out, "server-out.txt"), "w") as so:
                # Every session has a join password (it encrypts the traffic): in the
                # environment, never on the command line.
                senv = dict(os.environ, OOT_COOP_TEST_PASSWORD=a.password)
                delay = test_delay_args(a.test_delay_ms, a.test_jitter_ms)
                if delay:
                    senv["OOT_COOP_TEST"] = "1"   # coop_server refuses the delay without it
                server = lanes.hs.popen_quiet(lanes.hs.server_bind([a.server, "--port", str(a.port), "--password-env",
                                              "OOT_COOP_TEST_PASSWORD"] + delay + a.server_arg), stdout=so, stderr=subprocess.STDOUT, env=senv)
            keep_pids()
            time.sleep(1)
        for i, x in enumerate(tags):
            g = games[x]
            if not os.path.isfile(os.path.join(exe_dir(g), "longshot.exe" if WINDOWS else "longshot")):
                raise FileNotFoundError("no Hookshot game in %s" % g)
            if not a.no_restore:
                say("%s: save baseline %s" % (x, "restored" if restore_save(g) else "none (autotest-save missing)"))
            sock = "%s-%s" % (a.pipe, x) if WINDOWS else os.path.join(out, "control-%s.sock" % x)
            commands[x] = os.path.join(out, "commands-%s.txt" % x)
            open(commands[x], "w").close()
            env = dict(os.environ, OOT_COOP_KIT_START="1", HOS_CONTROL=sock, HOS_CONTROL_MEM="ro", LICHII_COOP_TEST_COMMANDS=commands[x])
            test_game_defaults(env)   # its own save, no crash ask on the title (TEST_GAME_DEFAULTS): a switch can undo it
            b_pastes = a.host_in_game and x == "B" and getattr(a, "b_joins_in_game", False)
            if b_pastes:
                # private-lobby-fix: B also starts with co-op off and joins through its own Multiplayer page (its
                # script's `menu paste`: the code A's host put on this PC's clipboard), as a friend's paste does.
                env["LICHII_COOP_NAME"] = x
            elif a.server or getattr(a, "only", None) or (a.host_in_game and x != "A"):   # (--only: the session's server)
                env["LICHII_COOP_SERVER"] = "127.0.0.1:%d" % a.port
                env["LICHII_COOP_NAME"] = x   # "A", "B": the scripts type /teleport A, /teleport B
                env["LICHII_COOP_PASSWORD"] = a.password
            elif a.host_in_game:
                # Row 260: A starts with co-op off and hosts through its own Multiplayer > Host (its script's
                # `menu host`), the in-process server players use; B joins it (it retries until A hosts).
                env.update(host_in_game_env(a.port, a.password))
            if a.playtest_ports:
                # the play-test bridge (docs/playtest.md "Bridge protocol"): off unless its port is given
                env["LICHII_COOP_PLAYTEST_PORT"] = str(a.playtest_ports + "AB".index(x) + 1)   # (A p1, B p2, also alone or --only)
                env["LICHII_COOP_PLAYTEST_VERBS"] = "1"   # act, warp, flag, give: a player's game never gets this
                env.setdefault("LICHII_COOP_CHEATS", "1")   # the bridge's give goes through the cheats queue
            for k, v in header_switches(a.script_a):   # both games, as run.ps1; an --env of the same name wins
                env[k] = v
            for kv in a.env:
                k, v = kv.split("=", 1)
                if k[:2] in ("A:", "B:"):
                    if k[0] != x:
                        continue
                    k = k[2:]
                env[k] = v
            exe = os.path.join(exe_dir(g), "longshot.exe" if WINDOWS else "longshot")
            started[x] = time.time()
            if kits[x]:
                say("%s: the kit's own start (%s)" % (x, kits[x]))
                procs[x] = PidProcess(kit_start(kits[x], env, out, x))
            else:
                pack_start(g, env, lambda line, x=x: say("%s: %s" % (x, line)))   # (the kit's start does it: row 265)
                mismatch = code_mods_mismatch(exe_dir(g))
                if mismatch:   # (charsel-tab-0119: a code mod's menus without its code; the run says so first)
                    say("%s: WARNING: %s" % (x, mismatch))
                with open(os.path.join(out, "game-out-%s.txt" % x), "wb") as go:
                    procs[x] = subprocess.Popen([exe], cwd=exe_dir(g), env=env, stdin=subprocess.DEVNULL, stdout=go,
                                                stderr=subprocess.STDOUT, **popen_flags())
            say("%s: started pid %d, bridge %s" % (x, procs[x].pid, sock))
            place_games({x: procs[x].pid}, a.display, say, wait=30.0)   # (its window as soon as it is there)
            keep_pids()
            bridges[x] = (sock, Bridge.connect(sock, timeout=120))
            if i == 0:
                time.sleep(3)
        def game_log(x):
            # Each game's own log so far: the kit's start writes the game's file, ours game-out-<x>.txt.
            go = os.path.join(out, "game-out-%s.txt" % x)
            return (lambda: fresh_log(games[x], started[x])) if kits[x] else (lambda: read_text(go))

        for x in tags:   # both games drawing before anything is pressed (a cold shader cache: minutes)
            log = game_log(x)
            waited = wait_renderer(log, lambda: procs[x].poll() is None, a.renderer_wait, a.renderer_stall,
                                   a.renderer_limit)
            if a.renderer_wait > 0:
                say("%s: renderer ready (%.0f s waited)" % (x, waited))
        # (the game can move or resize its window when its renderer comes up: placed again, both)
        place_games({x: procs[x].pid for x in tags}, a.display, say)

        def shooter(x, name):
            def shot(mark):
                for y in tags:
                    stem = "%s-%s-%s" % (name, x, mark) if mark else name
                    path = os.path.join(out, "%s-%s.png" % (stem, y))
                    try:
                        bridges[y][1].request("shot", reply_timeout=30.0, path=path, timeout=20)
                    except Exception as e:
                        say("%s shot %s: %s" % (y, os.path.basename(path), e))
            return shot

        def both(name):
            shooter("", name)(None)

        if not a.no_load:
            errs = {}

            def loader(x):
                try:
                    lb = Bridge.connect(bridges[x][0])
                    try:
                        lb.request("wait", reply_timeout=a.timeout + 30, event="frame", frame=1, timeout=a.timeout)
                        to_play(lb, Log(os.path.join(out, "player-%s.txt" % x)), a.timeout)
                    finally:
                        lb.close()
                except Exception as e:
                    errs[x] = e
            ts = [threading.Thread(target=loader, args=(x,)) for x in tags if x not in a.no_load_games.split(",")]
            for t in ts:
                t.start()
            for t in ts:
                t.join()
            if errs:
                raise RuntimeError("load: " + ", ".join("%s %s" % kv for kv in errs.items()))
            both("loaded")
            say("save loaded")
        errs, players = {}, {}

        def play(x):
            try:
                pb = Bridge.connect(bridges[x][0])
                try:
                    p = Player(pb, Log(os.path.join(out, "player-%s.txt" % x)), shot=shooter(x, a.name),
                               connect=lambda: Bridge.connect(bridges[x][0]), commands=commands[x],
                               read_log=game_log(x),
                               burst_path=lambda m, x=x: os.path.join(out, "%s-%s-%s-%s.png" % (a.name, x, m, x)))
                    players[x] = p
                    p.play(parsed[x])
                finally:
                    pb.close()
            except Exception as e:
                errs[x] = e
        ts = [threading.Thread(target=play, args=(x,), daemon=True) for x in tags if x in parsed]
        for t in ts:
            t.start()
        start, n = time.monotonic(), 0
        while any(t.is_alive() for t in ts):
            for x in tags:
                if procs[x].poll() is not None:
                    raise RuntimeError("game %s ended by itself (exit code %s)" % (x, procs[x].returncode))
            if time.monotonic() - start > a.scenario_timeout:
                raise TimeoutError("scenario did not finish in %d s" % a.scenario_timeout)
            if a.every > 0 and time.monotonic() - start >= n * a.every:
                both("%s-%03d" % (a.name, n))
                n += 1
            time.sleep(0.25)
        # A script's warp asks on in its own thread (Player.warp) and a script may end before the game took it: the
        # teleport is dropped while the save's arrival event plays, and was asked again only while the process lived.
        # test-bridge-pilot-fa63e90f (live.py start): B's 'warp KakarikoVillage_Child' was 'sent, not accepted' 1 s
        # before the script ended, so B stayed in Graveyard_Child in an event. So each warp is waited for (it ends by
        # itself within warp_for: done, failed or gave up), as `hookshot.py play` already did.
        wait_warps(players)
        if errs:
            raise RuntimeError("play: " + ", ".join("%s %s" % kv for kv in errs.items()))
        time.sleep(1)
        both("end")
        wrong = ["%s %s" % (x, ", ".join(p.warp_failed)) for x, p in sorted(players.items()) if p.warp_failed]
        if wrong:   # (the shots are taken: they show where the game went instead)
            raise RuntimeError("warp landed on another map than asked: " + "; ".join(wrong))
        # (a boot whose marks or test commands the module never read did not set the game up: party-hud-bridge-85e3b255's B
        # had no module at all, yet this run said 'scenario played' and exited 0, so live.py start answered ok: B #137)
        lost = ["%s %s" % (x, ", ".join(p.not_taken)) for x, p in sorted(players.items()) if p.not_taken]
        if lost:
            raise RuntimeError("NOT taken by the module (not started?): " + "; ".join(lost))
        say("scenario played")
    except Exception as e:
        failed = str(e) or e.__class__.__name__
        say("FAILED: " + failed)
        try:
            both("failed")
        except Exception:
            pass
    finally:
        if not a.keep_open:
            for x, (_, b) in bridges.items():
                try:
                    b.request("quit", reply_timeout=10.0, check=False)
                except Exception:
                    pass
                b.close()
            for x, p in procs.items():
                try:
                    p.wait(15)
                except subprocess.TimeoutExpired:
                    p.kill()
            if server:
                server.terminate()
        for x in tags:
            # Hookshot writes longshot_log.txt only when its stderr goes nowhere; here stderr is game-out-<x>.txt, so
            # that is the log (an old longshot_log.txt in the folder is from another run). The game's file otherwise.
            copy_log(out, x, games[x], say)
    if failed:
        with open(os.path.join(out, "FAILED.txt"), "w", encoding="utf-8") as f:
            f.write(failed + "\n")
        return 1
    return 0



def copy_log(out, x, game, say, tries=5, sleep=time.sleep):
    """hookshot_log-<x>.txt from game-out-<x>.txt (or the game's own log). A copy that fails (a file still held for a
    moment after the game closed) is tried again, then said in steps.txt: it never ends the run. Before, it raised
    out of run()'s finally, after "scenario played": testkit-0116-3's smoke had hs-players and hs-puppets "run.ps1
    exit 1" with no hookshot_log-B.txt, every expect= line in the game-out logs and no FAILED.txt."""
    go = os.path.join(out, "game-out-%s.txt" % x)
    last = None
    for i in range(tries):
        try:
            lp = go if os.path.isfile(go) and os.path.getsize(go) > 0 else find_log(game)
            if not lp:
                return False
            shutil.copy2(lp, os.path.join(out, "hookshot_log-%s.txt" % x))
            return True
        except OSError as e:
            last = e
            if i + 1 < tries:
                sleep(1.0)
    try:
        say("%s: game log not copied to hookshot_log-%s.txt after %d tries (%s: %s); grade from game-out-%s.txt"
            % (x, x, tries, last.__class__.__name__, last, x))
    except Exception:
        pass
    return False


def test_delay_args(delay_ms, jitter_ms):
    """coop_server's TEST-ONLY bad connection (server/test_delay.hpp) as its arguments; [] when off.
    The server also needs OOT_COOP_TEST=1 in its environment for them."""
    if not (delay_ms or jitter_ms):
        return []
    return ["--test-delay-ms", str(int(delay_ms)), "--test-jitter-ms", str(int(jitter_ms))]


def add_test_delay_options(parser):
    """--test-delay-ms / --test-jitter-ms for the server a run starts (run.ps1 -Lag / -Jitter on Longshot)."""
    parser.add_argument("--test-delay-ms", type=int, default=0,
                        help="TEST ONLY: the server holds every packet, this round trip in ms (half each way)")
    parser.add_argument("--test-jitter-ms", type=int, default=0,
                        help="TEST ONLY: +- up to this many ms on each packet, each way, order kept")

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="what", required=True)
    c = sub.add_parser("check", help="parse scripts and print their steps")
    c.add_argument("scripts", nargs="+")
    pl = sub.add_parser("play", help="play one script into one running game")
    pl.add_argument("--sock", required=True)
    pl.add_argument("--script", required=True)
    pl.add_argument("--log")
    pl.add_argument("--shots", help="folder for a shot at each mark")
    pl.add_argument("--commands", help="the game's LICHII_COOP_TEST_COMMANDS file (none: module steps skipped)")
    r = sub.add_parser("run", help="the two-game test (tools/autotest/run.ps1 -Runtime hookshot)")
    r.add_argument("--lane", type=int, choices=[1, 2],
                   help="test lane (tools/autotest/lanes.py): its pair, port, pipe and run folder unless given; holds its lock")
    r.add_argument("--game-a", help="(without --lane: this one, --game-b, or both)")
    r.add_argument("--game-b", help="(without --lane: this one, --game-a, or both)")
    r.add_argument("--server", help="coop_server to start (none: the games use their coop.ini)")
    r.add_argument("--host-in-game", action="store_true",
                   help="no coop_server: game A hosts through its own Multiplayer > Host (script step `menu host`) on "
                        "--port with --password, on 127.0.0.1 only; B joins it (row 260)")
    r.add_argument("--b-joins-in-game", action="store_true",
                   help="with --host-in-game: B too starts with co-op off and joins through its own Multiplayer page "
                        "(script step `menu paste`: the code A's host copied; private-lobby-fix)")
    r.add_argument("--port", type=int, help="default 27560 (a lane: its own)")
    r.add_argument("--password", default="autotest-pass",
                   help="the join password of the server it starts, given to both games (every session has one)")
    r.add_argument("--out", help="run folder (required without --lane; a lane: <its shots>/<name>/<time>)")
    r.add_argument("--name", default="scenario")
    r.add_argument("--script-a", help="(required without --only)")
    r.add_argument("--check-args", action="store_true",
                   help="parse and check the run's arguments and its scripts, then exit 0 without starting anything "
                        "(live.py start --dry-run; the in-game tests' --lane-check-only)")
    r.add_argument("--script-b")
    r.add_argument("--load", help="load.txt: checked only; the load presses A until the game plays")
    r.add_argument("--no-load", action="store_true")
    r.add_argument("--no-load-games", default="", help="A or B or A,B: those games skip the load (their script loads the save itself); the other plays --load")
    r.add_argument("--no-restore", action="store_true")
    r.add_argument("--keep-open", action="store_true")
    r.add_argument("--only", choices=["A", "B"],
                   help="start only this game, into a live session's folder (--out; live.py relaunch): its pids are "
                        "added to the folder's pids.json")
    r.add_argument("--display", help="the monitor the game windows go to, by device name (default $%s, else %s): "
                                     "tools/autotest/placement.py" % (placement.ENV, placement.DEFAULT_DISPLAY))
    r.add_argument("--env", action="append", default=[], help="NAME=VALUE (or A:NAME=VALUE) for the games")
    r.add_argument("--server-arg", action="append", default=[],
                   help="one more argument for the coop_server it starts (row 251: --same-network-open)")
    r.add_argument("--every", type=float, default=3.0)
    r.add_argument("--timeout", type=int, default=900, help="seconds for the boot and the load")
    r.add_argument("--scenario-timeout", type=int, default=600)
    add_renderer_options(r)
    r.add_argument("--pipe", help="default coop-auto (a lane: its own)")
    r.add_argument("--playtest-ports", type=int, default=0, metavar="BASE",
                   help="turn each game's play-test bridge on: A on BASE+1, B on BASE+2, with LICHII_COOP_CHEATS=1 "
                        "for its give (docs/playtest.md)")
    add_test_delay_options(r)
    r.add_argument("--kit-a", help="start game A through this kit's own start (play.ps1 choose -Hookshot; its "
                                   "hookshot/game-folder.txt names --game-a): tools/autotest/kit_pair.py")
    r.add_argument("--kit-b", help="the same for game B")
    a = p.parse_args(argv)
    if a.what == "check":
        bad = 0
        for path in a.scripts:
            try:
                steps = parse(read_script(path))
            except ScriptError as e:
                print("%s: %s" % (path, e))
                bad += 1
                continue
            module = sorted({s.kind for s in steps if s.kind in MODULE_ONLY})
            print("%s: %d steps%s" % (path, len(steps), (", through the module: " + " ".join(module)) if module
                                       else ""))
        return 1 if bad else 0
    if a.what == "play":
        steps = parse(read_script(a.script))
        b = Bridge.connect(a.sock)

        def shot(name):
            b.request("shot", reply_timeout=30.0, path=os.path.abspath(os.path.join(a.shots, name + ".png")))
        player = Player(b, Log(a.log, echo=print), shot=shot if a.shots else None,
                        connect=lambda: Bridge.connect(a.sock), commands=a.commands)
        player.play(steps)
        for t in player.warps:
            t.join(player.warp_for)
        b.close()
        return 0
    for kv in a.env:
        if "=" not in kv:
            p.error("--env NAME=VALUE")
    if a.what == "run" and not a.script_a and not a.only:
        p.error("--script-a (or --only B)")
    if getattr(a, "host_in_game", False):
        if a.server:
            p.error("--host-in-game hosts in game A: no --server")
        if not a.script_b:
            p.error("--host-in-game needs --script-b (B joins A)")
        if not re.fullmatch(r"[!-~]{1,64}", a.password):
            p.error("--host-in-game: --password of 1 to 64 printable letters")
    elif getattr(a, "b_joins_in_game", False):
        p.error("--b-joins-in-game needs --host-in-game")
    if a.what == "run" and a.check_args:
        for path in (a.script_a, a.script_b, getattr(a, "load", None)):
            if path:
                try:
                    parse(read_script(path))
                except (ScriptError, OSError) as e:
                    p.error("script %s: %s" % (path, e))
        print("run arguments OK (--check-args: nothing started)")
        return 0
    if not a.lane:
        if not ((a.game_a or a.game_b) and a.out):
            p.error("--game-a and/or --game-b, and --out (or --lane N)")
        a.port, a.pipe = a.port or 27560, a.pipe or "coop-auto"
        return run(a)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script's folder out)
    import lanes
    L = lanes.lane(a.lane)
    a.game_a, a.game_b = a.game_a or L["a"], a.game_b or L["b"]
    a.port, a.pipe = a.port or L["port"], a.pipe or L["pipe"]
    a.out = a.out or os.path.join(L["shots"], a.name, time.strftime("%Y%m%d-%H%M%S"))
    owner = "hookshot.py %s (pid %d)" % (a.name, os.getpid())
    if not lanes.lock_acquire(a.lane, owner, wait=1800, pid=os.getpid()):
        return 1
    try:
        print("lane %d: %s + %s, port %d, pipe %s, run folder %s" % (a.lane, a.game_a, a.game_b, a.port, a.pipe, a.out))
        return run(a)
    finally:
        lanes.hs.lock_release(lanes.hs.main_root(lanes.REPO), "lane%d" % a.lane, owner)


if __name__ == "__main__":
    sys.exit(main())
