"""An MCP server (stdio) that gives Claude Code the controls of a live play-test session (tools/autotest/live.py).

Register it once from the repository folder, pointing at a session started with `live.py start`:

    claude mcp add oot-playtest -- python -I tools/autotest/mcp_playtest.py --session <DIR>

The tools act on the session's two games (A and B): look (`state`), act (`act`: test script lines), see (`shot`),
checkpoint both games' shared flags (`checkpoint`) and compare them (`desync_check`). The MCP protocol is spoken by
hand (newline-delimited JSON-RPC 2.0 on stdin/stdout: initialize, tools/list, tools/call), standard library only.
"""
import argparse
import io
import math
import json
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import desync_check  # noqa: E402
import live  # noqa: E402
import kit_fight  # noqa: E402
import kit_look  # noqa: E402
import kit_move  # noqa: E402
import kit_route  # noqa: E402
import kit_setup  # noqa: E402
import kit_tools  # noqa: E402
import kit_wait  # noqa: E402
import playtest_client as pc  # noqa: E402
import playtest_helpers as ph  # noqa: E402

SCRIPT_HELP = (
    "Test script lines (tools/autotest/hookshot.py's format), ';' between them. Times are game frames (60 a second). "
    "Movement: `goto X Z TOLERANCE FRAMES` (walk to a point, metres), `toward X Z FRAMES [BUTTONS]` (walk that way, "
    "holding buttons), `stick LX LY FRAMES`, `wait FRAMES`. Buttons: `press A 8`, `hold ZL+B 30` (A B X Y L R ZL ZR "
    "PLUS MINUS UP DOWN LEFT RIGHT). Co-op test commands (need LICHII_COOP_CHEATS=1): `warp SCENE` (e.g. "
    "ForestTemple, SacredForestMeadow_Adult), `place X Y Z`, `give ITEM`, `setflag NAME VALUE`, "
    "`kill ACTOR`, `hit ACTOR N`, `hurt N`, `chat TEXT`, `mark NAME`. No `face` line: a written yaw is not play "
    "(turn Link with the `face` tool, which uses the stick, or set him up with `place` and a facing).")

FACE_REFUSED = ("`face` is refused in act: no player can set Link's yaw. Turn him with the `face` tool (the stick) "
                "or put him there with `place` and a facing (set-up).")


def refuse_face(script):
    """Raises ValueError when a line of the act script is the test `face X Z` line (a written yaw)."""
    for line in str(script).replace(";", "\n").split("\n"):
        w = line.split("#", 1)[0].split()
        if w and w[0].lower() == "face":
            raise ValueError(FACE_REFUSED)

TOOLS = [
    {"name": "state", "description": "What a game shows now: map, Link's position [x,y,z] in metres, facing and "
     "camera yaw (0 = +z, 90 = +x), life and max life (quarter hearts), rupees, age, whether an event or a load is "
     "running, the A-button prompt, what co-op's menus show (`menu`: a shot leaves the co-op pages out) and the new "
     "co-op log lines since the last look. Both games without `player`. "
     "Every look has a `state_id`; pass it as `since` on the next call to get only what changed (`changed`, `removed`).",
     "inputSchema": {"type": "object", "properties": {"player": {"type": "string", "enum": ["A", "B"]},
                                                      "since": {"type": "integer", "minimum": 1}}}},
    {"name": "act", "description": "Play test script lines in one game, then return its state. " + SCRIPT_HELP,
     "inputSchema": {"type": "object", "required": ["player", "script"],
                     "properties": {"player": {"type": "string", "enum": ["A", "B"]},
                                    "script": {"type": "string"}}}},
    {"name": "shot", "description": "Screenshot one game or both: the PNG paths (open them to see the screen). "
     "Use sparingly; state is cheaper.",
     "inputSchema": {"type": "object", "properties": {"player": {"type": "string", "enum": ["A", "B"]}}}},
    {"name": "checkpoint", "description": "Write both games' shared dungeon and story flags and a screenshot of "
     "each game under a tag (letters, digits, - and _). Take one after every action that should sync, then run "
     "desync_check; open the shots for anything visible (flags alone are no proof of what a player sees).",
     "inputSchema": {"type": "object", "required": ["tag"], "properties": {"tag": {"type": "string"}}}},
    {"name": "desync_check", "description": "Compare every checkpoint of the session: the first tag where the two "
     "games' shared flags differ, and which flags.",
     "inputSchema": {"type": "object", "properties": {"all": {"type": "boolean"}}}},
    {"name": "quit_game", "description": "Close one game in the middle of the session (asked to quit, or `kill`: at "
     "once, as a crash ends it). A planned exit: the session goes on (live.py quit).",
     "inputSchema": {"type": "object", "required": ["player"],
                     "properties": {"player": {"type": "string", "enum": ["A", "B"]}, "kill": {"type": "boolean"}}}},
    {"name": "server", "description": "Stop the session's server, or start it again (the boot's exe, port and "
     "password). The games stay open (live.py server).",
     "inputSchema": {"type": "object", "required": ["action"],
                     "properties": {"action": {"type": "string", "enum": ["stop", "start"]}}}},
    {"name": "relaunch", "description": "Start a game that quit or crashed again; it joins the session's server and "
     "keeps its save unless `restore` (live.py relaunch). `boot`: a boot script; `no_load`: the script takes the title.",
     "inputSchema": {"type": "object", "required": ["player"],
                     "properties": {"player": {"type": "string", "enum": ["A", "B"]}, "boot": {"type": "string"},
                                    "no_load": {"type": "boolean"}, "restore": {"type": "boolean"}}}},
    {"name": "wait_log", "description": "Wait until a game's log (this run of the game) has at least `min` lines "
     "matching `pattern` (a regular expression; default 1), at most `timeout_s` seconds (default 60). With `max`, "
     "also fails when more lines than that match (`max` 0: none may). A failed check stops a script (a proof's line).",
     "inputSchema": {"type": "object", "required": ["player", "pattern"],
                     "properties": {"player": {"type": "string", "enum": ["A", "B"]}, "pattern": {"type": "string"},
                                    "min": {"type": "integer", "minimum": 0}, "max": {"type": "integer", "minimum": 0},
                                    "timeout_s": {"type": "number", "minimum": 0, "maximum": 1800}}}},
    {"name": "sleep", "description": "Wait this many seconds of real time (the games run as they are: a held game "
     "stays held), e.g. while the server is down.",
     "inputSchema": {"type": "object", "required": ["seconds"],
                     "properties": {"seconds": {"type": "number", "minimum": 0, "maximum": 600}}}},
]


PLAYER = {"type": "string", "enum": list(pc.PLAYERS)}
FRAMES = {"type": "integer", "minimum": 1, "maximum": 3600}
PAUSED = ("Every reply has the game's frame count; the game is paused between calls and only runs the frames a "
          "call asks for.")

# The verbs that run one test line through the bridge's `act` (docs/playtest.md, "act"). Each is checked here before
# it is sent; the bridge checks the kind again.
WORD = re.compile(r"^[A-Za-z0-9_.:+-]{1,96}$")
PARTS = ("weapon", "shield", "head", "armor", "boots", "hand")


def _word(args, key):
    v = str(args.get(key, ""))
    if not WORD.match(v):
        raise ValueError("%s: letters, digits and _ . : + - only (no spaces)" % key)
    return v


def _ident(args):
    """The optional actor `id` of kill and hit (the number get_world gives, or its 0x address) as an integer, or None.
    It needs the actor's name beside it (the walk checks both); 0 and numbers past 64 bits are refused."""
    v = args.get("id")
    if v is None or v == "":
        return None
    if isinstance(v, bool) or not isinstance(v, (int, str)):
        raise ValueError("id: the number get_world gives (or its 0x address)")
    try:
        n = v if isinstance(v, int) else int(v.strip(), 0)
    except ValueError:
        raise ValueError("id: the number get_world gives (or its 0x address)")
    if not 0 < n < 2 ** 64:
        raise ValueError("id: the number get_world gives (more than 0, less than 2^64)")
    return n


def _need_id(args):
    n = _ident(args)
    if n is None:
        raise ValueError("id: the number get_world gives (with `name`, the name it gives for that id)")
    return n


def _button(args, default="Y"):
    b = str(args.get("button", default)).upper()
    if b not in pc.BUTTONS:
        raise ValueError("button: one of " + " ".join(pc.BUTTONS))
    return b


def _since(args):
    """The `since` of get_state and state: the state_id of an earlier look (a whole number from 1), or None."""
    v = args.get("since")
    if v is None:
        return None
    if isinstance(v, bool) or not isinstance(v, int) or v < 1:
        raise ValueError("since: the state_id of an earlier look (a whole number, 1 or more)")
    return v


def _bounded(args, key, default, lo, hi):
    if key not in args:
        return default
    v = _num(args, key, lo, hi)
    return float(v) if "." in v else int(v)


def _num(args, key, lo=None, hi=None):
    try:
        v = float(args[key])
    except (KeyError, TypeError, ValueError):
        raise ValueError("%s: a number" % key)
    if v != v or v in (float("inf"), float("-inf")) or (lo is not None and v < lo) or (hi is not None and v > hi):
        raise ValueError("%s: a number from %s to %s" % (key, lo, hi))
    return ("%g" % v) if v != int(v) else str(int(v))


def face_point(args):
    """The (x, z) a `place` turns Link to: `face_x` and `face_z`, or a point 1 m from the place along `facing` degrees
    (0 = +z, 90 = +x). The two ways together, or half of the first, are refused."""
    has_pt = "face_x" in args or "face_z" in args
    if has_pt and "facing" in args:
        raise ValueError("facing, or face_x and face_z: not both")
    if has_pt:
        if "face_x" not in args or "face_z" not in args:
            raise ValueError("face_x and face_z come together")
        return float(_num(args, "face_x")), float(_num(args, "face_z"))
    deg = math.radians(float(_num(args, "facing", -360, 360)))
    return float(_num(args, "x")) + math.sin(deg), float(_num(args, "z")) + math.cos(deg)


def act_line(name, args):
    """The one test line a verb tool sends (None: not a verb tool)."""
    if name == "kill":
        who = _ident(args)
        return "kill %s%s" % ("%d " % who if who else "", _word(args, "actor"))
    if name == "hit":
        who = _ident(args)
        return "hit %s%s %s" % ("%d " % who if who else "", _word(args, "actor"), _num(args, "amount", 1, 80))
    if name == "hurt":
        return "hurt " + _num(args, "quarter_hearts", 1, 80)
    if name == "place":
        return "place %s %s %s" % (_num(args, "x"), _num(args, "y"), _num(args, "z"))
    if name == "wear":
        part = str(args.get("part", ""))
        if part not in PARTS:
            raise ValueError("part: one of " + ", ".join(PARTS))
        return "wear %s %s" % (part, _word(args, "name"))
    if name == "dress":
        part = str(args.get("part", "all"))
        if part not in PARTS + ("all",):
            raise ValueError("part: one of all, " + ", ".join(PARTS))
        return "dress " + part
    if name == "assign":
        return "assign " + _word(args, "item")
    if name == "chat":
        text = str(args.get("text", ""))
        if not text.strip() or len(text) > 200 or any(ord(c) < 32 or c in ";#" for c in text):
            raise ValueError("text: 1 to 200 characters, no new line, ';' or '#'")
        return "chat " + text
    if name == "menu":
        page = str(args.get("page", ""))
        if page not in ("customize", "close"):
            raise ValueError("page: customize or close")
        return "menu " + page
    return None


ACT_NOTE = ("Needs a test-runner start with cheats on (the bridge refuses otherwise). It reaches the other players' games, so "
            "use it on the test server only. The game does it on its next frames: step a few, then get_state.")
ACT_TOOLS = [
    {"name": "kill", "description": "Kill an enemy actor: `actor` is its NAME from get_world. With no `id` the game "
     "takes the nearest actor of that name with a life. With `id` (the id get_world gave, or its 0x address) it takes "
     "exactly that actor, and refuses when no actor with that id and that name is there any more (it never falls back "
     "to another of the name); an id is refused in solo. Up to 12 frames pass while it waits for the game's answer: "
     "ok, `refused: actor 7 (NAME) is gone`, or `unknown: no result after 12 frames`. With an `id`, `ok` also needs "
     "the actor seen dead within `wait_frames` more frames (default 36, a guess: S8 saw one die only after Link came "
     "near it); else `struck: ...` with `died` false. " + ACT_NOTE,
     "inputSchema": {"type": "object", "required": ["actor"],
                     "properties": {"actor": {"type": "string"}, "id": {"type": ["integer", "string"]},
                                    "wait_frames": {"type": "integer", "minimum": 0, "maximum": 600}}}},
    {"name": "hit", "description": "Hit an actor for N quarter hearts: `actor` is its NAME from get_world. `id` and the "
     "answer are as for kill (no `id`: the nearest of that name with a life; up to 12 frames pass). " + ACT_NOTE,
     "inputSchema": {"type": "object", "required": ["actor", "amount"],
                     "properties": {"actor": {"type": "string"}, "id": {"type": ["integer", "string"]},
                                    "amount": {"type": "integer", "minimum": 1, "maximum": 80}}}},
    {"name": "hurt", "description": "Hurt Link for N quarter hearts (a hit from a metre east). " + ACT_NOTE,
     "inputSchema": {"type": "object", "required": ["quarter_hearts"],
                     "properties": {"quarter_hearts": {"type": "integer", "minimum": 1, "maximum": 80}}}},
    {"name": "place", "description": "Put Link at a point (x, y, z in metres). With `facing` (degrees: 0 = +z, "
     "90 = +x) or `face_x` and `face_z` (a point to look at) he is also turned that way, so a chest or a talk needs "
     "no walk or guess; then 4 frames pass and the reply has `placed` and `faced`. " + ACT_NOTE,
     "inputSchema": {"type": "object", "required": ["x", "y", "z"],
                     "properties": {"x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"},
                                    "facing": {"type": "number", "minimum": -360, "maximum": 360},
                                    "face_x": {"type": "number"}, "face_z": {"type": "number"}}}},
    {"name": "wear", "description": "Set what Link wears in a slot (the flags only; `dress` puts it on). " + ACT_NOTE,
     "inputSchema": {"type": "object", "required": ["part", "name"],
                     "properties": {"part": {"type": "string", "enum": list(PARTS)}, "name": {"type": "string"}}}},
    {"name": "dress", "description": "Put on what `wear` set, through the game's own equip call. " + ACT_NOTE,
     "inputSchema": {"type": "object", "properties": {"part": {"type": "string", "enum": ["all"] + list(PARTS)}}}},
    {"name": "assign", "description": "Put an item on a button (Bow, Hookshot, Hammer, SlingShot, Bomb, ...). " + ACT_NOTE,
     "inputSchema": {"type": "object", "required": ["item"], "properties": {"item": {"type": "string"}}}},
    {"name": "chat", "description": "Send a chat message as this player (1 to 200 characters; no ';', '#' or new "
     "line). " + ACT_NOTE,
     "inputSchema": {"type": "object", "required": ["text"], "properties": {"text": {"type": "string"}}}},
    {"name": "menu", "description": "Open co-op's Customize page, or close co-op's menu. get_state's `coop_menu` "
     "member says which co-op page is open and `menu` its rows (a screenshot leaves the co-op pages out); neither "
     "sees the game's own pause screen (use screenshot). " + ACT_NOTE,
     "inputSchema": {"type": "object", "required": ["page"],
                     "properties": {"page": {"type": "string", "enum": ["customize", "close"]}}}},
]

# The helpers of playtest_helpers.py as tools: a few calls in one, each bounded to 120 frames, each saying why it stopped.
# They take the `id` and the `name` get_world gave for the actor and look it up again on every leg and every press.
ACTOR_ARGS = {"id": {"type": ["integer", "string"]}, "name": {"type": "string"}}
HELPER_NOTE = ("At most 120 frames pass (`max_frames` lowers it). The actor is looked up in the world list on every leg: it "
               "stops with `gone` when the id is not there any more or has another name, and never goes for another "
               "actor of that name. It needs the world list (connected and sharing): without one it says why. "
               "It acts on this game only: step the other game by the same frames yourself.")
HELPER_TOOLS = [
    {"name": "go_to_actor", "description": "Walk Link to an actor, in legs of a few metres. `id` and `name` are the ones "
     "get_world gave. Stops near it (arrived), or says why: gone, stuck (no 0.3 m closer over 60 frames; `moved_m` is how "
     "far Link went), timed out, scene changed, loading, event. " + HELPER_NOTE,
     "inputSchema": {"type": "object", "required": ["id", "name"],
                     "properties": dict(ACTOR_ARGS, near_m={"type": "number", "minimum": 0.5, "maximum": 10},
                                        max_frames={"type": "integer", "minimum": 1, "maximum": 120})}},
    {"name": "face", "description": "Turn Link toward an actor (`id` and `name` from get_world) with light pushes of the left stick, "
     "until his facing is within 10 degrees (12 pushes of 2 frames at most). `turn` says the pushes, the degrees left "
     "and how far he stepped.",
     "inputSchema": {"type": "object", "required": ["id", "name"], "properties": ACTOR_ARGS}},
    {"name": "attack", "description": "go_to_actor, face it, then press a button (the sword; `button` Y by default: "
     "Longshot's attack button) in rounds until its life is 0 or the frames run out. Returns `result` "
     "(dead, gone, alive, or stopped with `stopped` saying why) and its life before and after. " + HELPER_NOTE,
     "inputSchema": {"type": "object", "required": ["id", "name"],
                     "properties": dict(ACTOR_ARGS, button={"type": "string", "enum": list(pc.BUTTONS)},
                                        max_frames={"type": "integer", "minimum": 1, "maximum": 120})}},
    {"name": "use_item", "description": "Put an item on a button (`assign`), press that button, and say what changed in "
     "the state (life, rupees, event, prompt, equips, inventory). `button` is the one pressed: ZR by default, the "
     "button `assign` puts the item on (Y is the sword). About 14 frames pass. " + ACT_NOTE,
     "inputSchema": {"type": "object", "required": ["item"],
                     "properties": {"item": {"type": "string"}, "button": {"type": "string", "enum": list(pc.BUTTONS)}}}},
    {"name": "talk", "description": "Talk to whoever the A prompt names (or, with `id` and `name` from get_world, when no "
     "prompt shows yet, walk to that actor and face it first): press A, read the speakers and the reply buttons the "
     "game shows, press A to go on, and when buttons show pick the next of `reply` (a text, or a list: one per set of "
     "buttons, in order; Down to it, then A). With no reply left it stops and lists the buttons, the talk left open: "
     "call again with `reply` and `since` (from that answer) to go on, no new A. With no prompt nothing is pressed. "
     "Returns `speakers`, `boxes` [{speaker, text, markup, raw}], `choices`, `picked`, `since`, `done` (the talk "
     "ended) or `result` saying why it stopped. A box's `text` is its words with the game's markup cut out (a variable stays as `{Name}`; `raw` "
     "keeps the whole when something was cut); `speaker` is null only when the game shows no name. `boxes` is empty "
     "without patch 0042 (take a screenshot to read it). A shop's item list is not buttons: move to the item "
     "yourself (press), then talk with since. At most `max_boxes` boxes (20); it keeps going while boxes come and "
     "gives up after 1200 frames with nothing new (7200 in all). It acts on this game only.",
     "inputSchema": {"type": "object",
                     "properties": dict(ACTOR_ARGS,
                                        reply={"anyOf": [{"type": "string", "maxLength": 200},
                                                         {"type": "array", "maxItems": 8,
                                                          "items": {"type": "string", "maxLength": 200}}]},
                                        since={"type": "integer", "minimum": 0},
                                        max_boxes={"type": "integer", "minimum": 1, "maximum": 60})}},
    {"name": "get_text", "description": "The texts the game set in its text panes since `since` (the `last` of an "
     "earlier get_text; 0: the last 256 kept), oldest first: {hooked, last, lost, texts: [{seq, frame, layout, pane, "
     "text, length, cut, markup}]}. `layout` keeps one layout (a talk is MessageDialog_00: T_Name_00 the "
     "speaker; its reply buttons PaMessageBtn_00, one T_Btn_00 set per button). Bytes that are not plain text (the game's markup) show as "
     "JSON \\u escapes and set `markup`. Read only; no frame runs.",
     "inputSchema": {"type": "object",
                     "properties": {"since": {"type": "integer", "minimum": 0},
                                    "layout": {"type": "string", "maxLength": 64}}}},
    {"name": "open", "description": "As talk, for a chest or a door: press A when the `prompt` says there is something to "
     "open; with no prompt nothing is pressed.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "interact", "description": "go_to_actor to within 1 m (`near_m`), face it, then as talk: press A only when "
     "the `prompt` says an action is there. `walk` is go_to_actor's answer; a walk or turn that did not get there says "
     "why and presses nothing. " + HELPER_NOTE,
     "inputSchema": {"type": "object", "required": ["id", "name"],
                     "properties": dict(ACTOR_ARGS, near_m={"type": "number", "minimum": 0.5, "maximum": 10},
                                        max_frames={"type": "integer", "minimum": 1, "maximum": 120})}},
]

MOVE_NOTE = (" Each ends with a `look`. It acts on this game only (in a pair, the other game runs the same frames).")
MOVE_TOOLS = [
    {"name": "shoot", "description": "Take out an aimed item and fire it at a target the way a player aims: `assign` "
     "puts the item on ZR, Link turns to the target with the stick and ZL puts the camera behind him, ZR is held 60 frames to "
     "draw, then a stick turns the view until the target is under the reticle (from the state's `view`; which stick and "
     "which way is learnt by a small push of each), ZR is held 30 more, the aim is checked again, ZR is let go, and the "
     "target is watched for `watch_frames`. `tilt_frames` (+ up, - down) tilts by hand after the aim. "
     "The target is `id` + `name` (from get_world) or a point `at` [x, y, z]. `result`: hit (its life fell, it died or "
     "left the list), changed (another field it shows changed), miss (the ammo went down, the target did not change), "
     "fired (a point, or an item with no ammo count), not fired (with why: no ammo, a help box, not Link's item), "
     "not aimed (the view could not be turned onto it; B puts the item away, no shot) or a stop. `aim` has the world "
     "yaw and pitch from Link to the target, the stick used, the turns and the last `error` on the screen. `assist` "
     "true adds the test `aim X Y Z` line: a shot whose flight is within 4 degrees and 0.5 m of the target is moved sideways onto "
     "its line (a nudge; farther off it is left alone). " + ACT_NOTE
     + MOVE_NOTE,
     "inputSchema": {"type": "object", "required": ["item"], "properties": dict(
         ACTOR_ARGS, item={"type": "string", "enum": list(kit_move.ITEMS)},
         at={"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
         assist={"type": "boolean"}, tilt_frames={"type": "integer", "minimum": -60, "maximum": 60},
         watch_frames={"type": "integer", "minimum": 4, "maximum": 240})}},
    {"name": "climb", "description": "Climb a ladder or vines: the stick held up (`up` true, the default) or down, a "
     "look every 8 frames, until Link's height reaches `to_y` (metres), or with no `to_y` until it stops moving (the "
     "top or the bottom). `face_x`/`face_z`: turn to the wall first and lean into it to grab on; going down with an A "
     "prompt presses A first. `result`: arrived, stopped (the top or bottom), stuck (the height did not move for 4 "
     "looks: not on it, or something in the way), not climbing (Link moved across, not up: walking, not on the wall), "
     "or a stop (hurt: a Skulltula on the vines, an event, a scene change, timed out). `trace` is [frames, height]."
     + MOVE_NOTE,
     "inputSchema": {"type": "object", "properties": {
         "up": {"type": "boolean"}, "to_y": {"type": "number"}, "face_x": {"type": "number"},
         "face_z": {"type": "number"}, "max_frames": {"type": "integer", "minimum": 8, "maximum": kit_move.MAX_FRAMES}}}},
    {"name": "push", "description": "Push a block (`id` + `name` from get_world) `distance_m` metres along world yaw "
     "`yaw` (0 = +z, 90 = +x; none: from Link toward the block). Link walks to the block's far side from the push, "
     "turns to it with the stick, leans on it until A offers \"Grab\", holds A 30 frames to grab it, then pushes at full "
     "stick. `grab` says what A offered and whether it was held. `result`: pushed, stuck (it did not move for 48 frames "
     "while Link pushed: blocked, at its end, not a moving block; or Link went up onto it), slipped (Link slid off its side), stopped (a move was refused: no camera; no "
     "frame ran), not reached (the walk to its side stopped), gone, or a stop. `moved_m` is how far it went along the push. Needs the world list." + MOVE_NOTE,
     "inputSchema": {"type": "object", "required": ["id", "name", "distance_m"], "properties": dict(
         ACTOR_ARGS, distance_m={"type": "number", "minimum": 0.1, "maximum": 20}, yaw={"type": "number"},
         max_frames={"type": "integer", "minimum": 8, "maximum": kit_move.MAX_FRAMES})}},
    {"name": "jump", "description": "Run straight at (`x`, `z`) at full stick so Link jumps off the edge by himself (a "
     "run off a ledge jumps; a walk only drops), with no side steps, until he is on the ground near the point. "
     "`land_y` is the far side's height. `result`: landed, fell (on the ground over 1 m below `land_y`), respawned "
     "(put back: a pit or a void), stuck (no nearer for 4 looks on the ground: a wall, or no jump there; a fall past the point is not stuck), stopped (a move was "
     "refused: no camera; no frame ran), or a stop (an event, a "
     "scene change, timed out). `airborne` says whether the height moved on the way; `trace` is [frames, x, y, z]."
     + MOVE_NOTE,
     "inputSchema": {"type": "object", "required": ["x", "z"], "properties": {
         "x": {"type": "number"}, "z": {"type": "number"}, "land_y": {"type": "number"},
         "tolerance": {"type": "number", "minimum": 0.2, "maximum": 5}, "max_frames": {"type": "integer", "minimum": 4,
                                                                                    "maximum": kit_move.MAX_FRAMES}}}},
]

# One game's tools (--player): that game's play-test bridge (docs/playtest.md, "Bridge protocol").
PLAYER_TOOLS = [
    {"name": "get_state", "description": "This game's state: scene and room, Link's position [x,y,z] in metres, "
     "facing and camera yaw (degrees: 0 = +z, 90 = +x), life and max life (quarter hearts), rupees, age, equips, "
     "co-op status, event/loading and the A prompt, plus words: `life_word` (full, ok, low, critical), "
     "`enemy_near` (living enemies within 10 m) and `danger` (one within 4 m); the last two are null when there is "
     "no world list (solo). The inventory is NOT in the reply unless you ask: `inventory_count` says how many items "
     "are set; `inventory` true adds those entries (about 290 on a full save: 10k tokens, ask once). The nearest "
     "actors are in get_world. `menu` is what co-op's menus show as text (its Insert-menu tab: page, row names, "
     "focused row; Character Select's Customize screen: options, focused, page_no of pages): a screenshot leaves out "
     "Longshot's overlays (the co-op pages), so read `menu` instead. Every reply has a `state_id`; pass it as `since` on a later call and only what "
     "changed comes back: `changed` (dotted path: new value, e.g. `pos`, `equips.weapon`), `removed`, the `frame` "
     "and a new `state_id` (an id that is no longer kept, the last 16 are, gives the whole state and says why). "
     "`frame`, `snapshot_present` and the like are left out of a delta: they move on every look. The `inventory` "
     "is left out too (`inventory_count` changing says it did: ask for the whole state with `inventory` true). "
     + PAUSED,
     "inputSchema": {"type": "object", "properties": {"inventory": {"type": "boolean"},
                                                      "since": {"type": "integer", "minimum": 1}}}},
    {"name": "get_world", "description": "The actors near Link in this game, nearest first: id, name, kind, position "
     "[x,y,z] in metres, distance, life, owner (\"server\", a player number or null = unknown), alive. `kind` and "
     "`within_m` filter; at most `limit` (50 by default, 200 at most) come back, with `matched` and `truncated`. The "
     "list exists only while the games are connected and sharing: an EMPTY list with a `why` is not 'nothing here'. "
     "Read `why`. `kill` and `hit` take the `name`, and with the `id` the exact actor (not in solo). No frame passes.",
     "inputSchema": {"type": "object", "properties": {"kind": {"type": "string", "enum": list(pc.KINDS)},
                                                       "within_m": {"type": "number", "minimum": 0},
                                                       "limit": {"type": "integer", "minimum": 1, "maximum": 200}}}},
    {"name": "look", "description": "ONE call for the whole situation, as short text (about 20 to 40 lines, no picture): "
     "scene, Link (age, life, place, facing, sword and shield, ammo), doors, enemies (life, metres, world yaw and "
     "ahead/left/right/behind), chests, switches, items, NPCs near, a running event, what A and B would do, and what "
     "CHANGED since your last look. Yaw is the one `move` and `walk_to` use (0 = +z, 90 = +x). The text of a text box "
     "and the C buttons are not readable through the kit yet and it says so. " + PAUSED,
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "wait_until", "description": "Let the game run (in chunks of 8 frames) until `what` happens, then reply with "
     "`met`, why it stopped and a `look` text. `what`: event_over, scene_changed, enemy_dead (with `id` from look), "
     "enemies_clear (none alive within 10 m), on_ground (height steady for two looks), prompt (A has something to do) or "
     "condition (with `condition`, as step_until). It stops early, and says so in `stopped`, when Link is hurt, dies, "
     "a scene loads, or an event starts (unless that is what you wait for). One call instead of a step loop. " + PAUSED,
     "inputSchema": {"type": "object", "required": ["what"], "properties": {
         "what": {"type": "string", "enum": list(kit_wait.WHATS)}, "id": {"type": ["integer", "string"]},
         "condition": {"type": "string"}, "max_frames": {"type": "integer", "minimum": 1, "maximum": kit_wait.MAX_FRAMES}}}},
    {"name": "go_to", "description": "Walk to a place and end with a `look`. Give ONE of: `place` (a position name from the "
     "scene's recorded data, tools/playtest/data; it walks the recorded routes, nearest start within 6 m, else a "
     "straight line and says so), `id` + `name` (an actor, as go_to_actor), or `x` + `z`. Stops early and says why: a "
     "scene change (through a door: that is the way out), a load, an event, hurt, a stuck walk (with the recorded "
     "stuck-spot note if there is one). It does not climb, jump or read the game's collision. " + HELPER_NOTE,
     "inputSchema": {"type": "object", "properties": dict(ACTOR_ARGS, place={"type": "string"}, x={"type": "number"},
                                                          z={"type": "number"}, near_m={"type": "number"})}},
    {"name": "record_route", "description": "Save a played walk as a route for the next tester. `action` start (begin "
     "sampling Link's place from every reply of this game), save (needs `name`, lowercase words with hyphens, and an "
     "optional `note`; writes tools/playtest/data/<scene>.json with points named NAME-1, NAME-2 ...; a point is kept "
     "every 2 m) or cancel. Walk with walk_to, press or go_to between start and save. Only the places Link walked "
     "through are saved, never anything from the game's files.",
     "inputSchema": {"type": "object", "required": ["action"], "properties": {
         "action": {"type": "string", "enum": ["start", "save", "cancel"]}, "name": {"type": "string"},
         "note": {"type": "string", "maxLength": 290}}}},
    {"name": "fight", "description": "Fight ONE enemy (`id` and `name` from look or get_world): walk up to sword "
     "reach, then slash with the lock-on held (ZL + Y, then Y let go), guard (ZL alone: the shield) for one round after Link "
     "is hit, in short rounds until it is dead, "
     "gone, Link is low (life 4 quarter hearts or less: he backs off and the fight ends as `low`), a scene change or "
     "load, an event (a cutscene: it waits for no event, so wait it out first), a walk-up the game refuses, or four "
     "walk-ups that get no nearer (`stuck`) stop it, or `max_frames` run out. Ends with a `look`. The buttons are Longshot's map (`lock` ZL, `sword` "
     "Y, `shield` ZL: lock-on and shield are one button). Longshot's Toggle ZL/ZR setting must be off. When the "
     "enemy's life never moves, `hint` says why it may be. `max_frames` is 600 by default, 1200 at most; "
     "no round starts that would pass it (a back-off when low adds 14). The enemy is looked up in the world list every round and never swapped for another "
     "of its name; it needs the world list. It acts on this game only.",
     "inputSchema": {"type": "object", "required": ["id", "name"], "properties": dict(
         ACTOR_ARGS, max_frames={"type": "integer", "minimum": 1, "maximum": kit_fight.MAX_FRAMES},
         lock={"type": "string", "enum": list(pc.BUTTONS)}, sword={"type": "string", "enum": list(pc.BUTTONS)},
         shield={"type": "string", "enum": list(pc.BUTTONS)}, min_life={"type": "integer", "minimum": 0})}},
    {"name": "get_actor", "description": "One actor from the world list by `id` (the number get_world gives, or its "
     "`0x...` address). Not found says why (gone, outside the 200 nearest, or no list at all). No frame passes.",
     "inputSchema": {"type": "object", "required": ["id"], "properties": {"id": {"type": ["integer", "string"]}}}},
    {"name": "walk_to", "description": "Walk Link to a point (x, z in metres) by world direction (the module "
     "makes the stick from each frame's camera), a few frames at a time. Stops on arrival (within tolerance metres), a scene change, a load or an event, or "
     "after max_frames; says why, the frames used and the distance left.",
     "inputSchema": {"type": "object", "required": ["x", "z"],
                     "properties": {"x": {"type": "number"}, "z": {"type": "number"},
                                    "tolerance": {"type": "number", "minimum": 0.05}, "max_frames": FRAMES}}},
    {"name": "press", "description": "Hold buttons (" + " ".join(pc.BUTTONS) + ") and optionally the stick "
     "(angle in degrees: 0 = away from the camera, 90 = right; strength 0..1) for N frames (default 2), then "
     "pause. Returns the state after.",
     "inputSchema": {"type": "object", "properties": {
         "buttons": {"type": "array", "items": {"type": "string", "enum": list(pc.BUTTONS)}},
         "stick_angle": {"type": "number"}, "stick_strength": {"type": "number", "minimum": 0, "maximum": 1},
         "frames": FRAMES}}},
    {"name": "step", "description": "Let the game run N frames with no input, then pause. " + PAUSED,
     "inputSchema": {"type": "object", "required": ["frames"], "properties": {"frames": FRAMES}}},
    {"name": "step_until", "description": "Let the game run in steps of `chunk` frames (8 by default, up to 60) with no "
     "input until a state field matches, then pause. `condition` is `field`, `!field` or `field OP value` (OP: == != < "
     "<= > >=; the value is JSON or a bare word; a dotted path such as `equips.weapon` or `pos.1`): `event == false` "
     "(a talk is over), `loading == false`, `scene == KokiriForest_Child`, `prompt` (something to talk to or open), "
     "`life == 0`, `rupees >= 50`. It looks first, so a condition that holds already costs no frame. At most `max_frames` "
     "(300 by default, up to 3600) pass. Reply: `met`, the field's `value`, `frames_run`, the last `state` and, "
     "when not met, `why`. A field the state does not have is an error (a typo is not a time-out); a null field is 'do "
     "not know' (a stale snapshot), never a match for an ordering. " + PAUSED,
     "inputSchema": {"type": "object", "required": ["condition"],
                     "properties": {"condition": {"type": "string"}, "max_frames": FRAMES,
                                    "chunk": {"type": "integer", "minimum": 1, "maximum": kit_tools.MAX_CHUNK}}}},
    {"name": "warp", "description": "Warp to a scene (a map name, e.g. KokiriForest_Child, ForestTemple) and "
     "optionally an entrance (a start position name in it), through the game's own scene change. In free play without an entrance it goes by Longshot's teleport (the maps of its "
     "own list; it lands at the map's own start). With an entrance, or for a map not in that list (a boss stage such as "
     "GohmaStage), the module opens the game's own map change event (needs LICHII_COOP_TELEPORT_EVENT; else `not_in_event` "
     "with the reason). Longshot allows one teleport every 8 s per game and refuses one while an event plays (`failed`); a "
     "name ending in the other age than Link's sets that age first (30 frames) and goes by the map change event. It runs until Link is there with no event "
     "or load (up to `timeout` seconds, default 60), then is held. A warp asked while a talk is open waits until the "
     "talk ends and answers `timeout` meanwhile: end the talk first (A or B until `event` is false).",
     "inputSchema": {"type": "object", "required": ["scene"],
                     "properties": {"scene": {"type": "string"}, "entrance": {"type": "string"},
                                    "timeout": {"type": "number", "minimum": 1, "maximum": 600}}}},
    {"name": "flag", "description": "Set a game flag (a GameData flag name or a 0x hash) to a value: a number, "
     "true/false, or an enum value's name. Done inside the held frame; replies with the old value (was).",
     "inputSchema": {"type": "object", "required": ["name"],
                     "properties": {"name": {"type": "string"},
                                    "value": {"type": ["integer", "boolean", "string"]}}}},
    {"name": "flag_read", "description": "Read one game flag (a GameData flag name, a 0x hash or key64:0xKEY); "
     "writes nothing. Replies with kind (int, bool, enum, uint, key64, text, unknown) and value (an enum as its value's "
     "hash, which `flag` takes back for that flag in this game run; an int has pending: true while a write waits).",
     "inputSchema": {"type": "object", "required": ["name"], "properties": {"name": {"type": "string"}}}},
    {"name": "setup", "description": "Set a test up in ONE call (instead of many flag/give/warp calls): hearts "
     "(current life, 1-20), max_hearts (3-20), magic (0 none, 1 meter, 2 double), rupees, small_keys {Dungeon: n}, "
     "boss_keys / maps / compasses [Dungeon], items [give names], flags {name: value}, then a warp {scene, entrance} "
     "and a place {x, y, z} after it (Link put at a point of the new scene). Dungeons: DekuTree, DodongoCavern, JabuJabu, ForestTemple, "
     "FireTemple, WaterTemple, ShadowTemple, SpiritTemple, BottomOfTheWell, IceCavern, GerudoTrainingGround, "
     "GerudoFortress, GanonsCastle, FinalBattleField. Runs in that order, stops at the first refused call and says which. "
     "Needs the verb gate (LICHII_COOP_PLAYTEST_VERBS, LICHII_COOP_CHEATS). MaxLife may be written back by the game "
     "(docs/items.md): read it with get_state after. In a pair, with no `player` it runs on BOTH games, one after the "
     "other. What reaches the other game by itself (co-op sharing, docs/items.md, flags.md): items (max), small keys "
     "(count deltas), maps, compasses, boss keys, story and dungeon flags, MaxLife and MPMax (max). Per player and NOT "
     "shared: Life, MP, rupees, ammo. So set per-player parts on each game.",
     "inputSchema": {"type": "object",
                     "properties": {"hearts": {"type": "integer"}, "max_hearts": {"type": "integer"},
                                    "magic": {"type": "integer"}, "rupees": {"type": "integer"},
                                    "small_keys": {"type": "object"}, "boss_keys": {"type": "array"},
                                    "maps": {"type": "array"}, "compasses": {"type": "array"},
                                    "items": {"type": "array"}, "flags": {"type": "object"},
                                    "warp": {"type": "object"}, "place": {"type": "object"},
                                    "age": {"type": "string", "enum": ["child", "adult"]}}}},
    {"name": "preset", "description": "Apply a named test start (tools/playtest/data/presets/*.json) with one call: the same "
     "steps as `setup`, from a versioned data file. deku-start (child, Deku Tree entrance), deku-before-gohma (child, the corridor before Gohma's door: walk "
     "through it), before-dodongo and before-barinade (child), "
     "before-phantom-ganon, before-volvagia, before-morpha, before-bongo-bongo, before-twinrova, before-ganondorf, "
     "before-ganon (adult): the dungeon's entrance, then Link at the boss room's arrival point, the boss put back. "
     "A preset sets its age first. A wrong name lists the right ones. Needs the verb gate.",
     "inputSchema": {"type": "object", "required": ["name"], "properties": {"name": {"type": "string"}}}},
    {"name": "save_point", "description": "Save a test point under a tag (letters, digits, - and _): the module's shared flags "
     "and Link's state (scene, place, life), in the run's checkpoint log. Retry a boss with load_point. Only what `dump` "
     "lists is saved (shared dungeon, story and item flags).",
     "inputSchema": {"type": "object", "required": ["tag"], "properties": {"tag": {"type": "string"}}}},
    {"name": "load_point", "description": "Restore the last test point saved under a tag by THIS game: writes every "
     "flag that differs FIRST, then loads the scene again (also when Link is already in it: a room reads its boss state "
     "when it loads), places Link and sets his life, then dumps again and replies `reloaded` (false when no load could be "
     "told), `exact` (true only when reloaded and the dump equals the saved one) and `left` (the flags still different). A scene outside Longshot's teleport list "
     "needs the map change event on (see warp). Needs the verb gate.",
     "inputSchema": {"type": "object", "required": ["tag"], "properties": {"tag": {"type": "string"}}}},
    {"name": "give", "description": "Give Link an item (bombs, bow, hookshot, a bare item name such as Item_Boomerang or "
     "Weapon_Sword_Kokiri, or a full flag name such as IsGet.Item_Boomerang). Written at "
     "once, with its IsGet flag and the ammo the game gives with it: the reply's `result` says each flag's old and "
     "new value (`no change: ... already N` when Link had it; `failed` with the reason when it was not given; `partial` "
     "true when the item is in but its IsGet or a grant was not; `not_ready` while a load runs). "
     "get_state with inventory=true shows it without a step. Bombs and arrows are counts per player: `bombs` gives the small "
     "bag and 20 bombs; a bomb that can be lifted also needs PlayerStatus.LiftGrade (docs/items.md).",
     "inputSchema": {"type": "object", "required": ["item"], "properties": {"item": {"type": "string"}}}},
] + ACT_TOOLS + HELPER_TOOLS + MOVE_TOOLS + [
    {"name": "checkpoint", "description": "Write this game's shared flags and state under a tag (letters, digits, "
     "- and _) with its frame count, to the run's checkpoint log. Take the same tag on EVERY game after each "
     "action that should sync, then run desync_check.",
     "inputSchema": {"type": "object", "required": ["tag"], "properties": {"tag": {"type": "string"}}}},
    {"name": "desync_check", "description": "Compare every game's checkpoints in the run's log folder, matched by "
     "tag: the first tag where the games' shared flags differ, which flags, and each game's frame. It also compares "
     "the games' actors (needs the Links in one scene within 40 m), by what docs/sync-coverage.md says about each "
     "name: only a `live` row can be a desync, and only when the same difference is in two checkpoints in a row with "
     "both games 60 or more frames further on (the first time it says recheck, result `incomplete`); a `?` row is "
     "'live not proven', a `reload` row 'shown on reload', a name with no row 'not compared'. `flags_only` leaves "
     "the actors out.",
     "inputSchema": {"type": "object", "properties": {"all": {"type": "boolean"}, "flags_only": {"type": "boolean"}}}},
    {"name": "pause", "description": "Hold the game (it is held from the first call anyway). " + PAUSED,
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "resume", "description": "Let the game run in real time again (a person takes over). The next call "
     "holds it again.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "log", "description": "This game's new co-op log lines (`coop:`) since the last call, the newest "
     "%d at most; `match` keeps only lines containing that text (the oldest %d of them; a `match` has its "
     "own cursor, so the lines it left out are still there for the next call without it). Needs --session (games from live.py start: p1 = A, "
     "p2 = B). No frame passes." % (live.MAX_LINES, live.MAX_LINES),
     "inputSchema": {"type": "object", "properties": {"match": {"type": "string"}}}},
    {"name": "check_logs", "description": "The oracle for 'did anything go wrong': this game's new `coop:` log lines "
     "that are a warning, an error or a critical line (the game's level prefix) or say failed, error, refused or "
     "resync, masked (IP addresses, host names, user names in paths, memory addresses, keys, player names, quoted "
     "text are replaced; the players' names the games print are hidden by default). Each line has its `why`. `since` "
     "is `last_check` (the default: since the last check_logs, or the boot) or `boot`. `names` are extra words to "
     "hide. verdict: clean, flagged, or no log. A clean log is not proof that nothing is wrong: it only says the game "
     "printed none of these. Works after the session ended. Needs --session. No frame passes.",
     "inputSchema": {"type": "object", "properties": {"since": {"type": "string", "enum": ["last_check", "boot"]},
                                                       "names": {"type": "array", "items": {"type": "string"},
                                                                 "maxItems": 20}}}},
    {"name": "session_status", "description": "Is the run still sound: this game's bridge (connected, held, last "
     "frame) and, with --session, the session (running or why it ended, each game's process alive, minutes left of "
     "the maximum and of the idle limit). Answers also after the session ended. It does not touch the game and "
     "does not count as a tester command for the idle watchdog. No frame passes.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "screenshot", "description": "A PNG of this game's screen (the path: open it to see). Use sparingly; "
     "get_state is cheaper. Anything visible is proven only by a picture. A held game runs 1 frame for it.",
     "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}}}},
]


class Game:
    """One game's tools: its bridge client, which player it is, and the run's log folder (shared by every game's
    server, so desync_check sees them all)."""

    def __init__(self, player, client, log_dir, session=None):
        self.player, self.client, self.log_dir, self.session = player.lower(), client, log_dir, session
        kit_setup.forget_setup(log_dir, self.player)   # a new server starts with no setup to put back
        self.match_offsets = {}   # one cursor per `log` match (the plain cursor is not moved by a match)
        self.recorder = None   # a route being recorded (record_route)
        self.route_dir = kit_route.DATA_DIR
        self.last_look = None   # what the last `look` saw (the next one says what changed)
        self.offsets = None   # this server's own place in the game's log (two servers never write one session file)
        self.check_offset = None   # where check_logs read to (None: from the boot)
        self.alive = live.lanes.hs.pid_alive   # (a test gives its own)
        # the last looks (get_state `since`); ids start at 100000 x the player's number, so p1's id means nothing to p2
        self.history = kit_tools.StateHistory(start=pc.player_index(self.player) * 100000 + 1)

    def log_lines(self, match=None):
        """The game's new co-op log lines since the last call (live.py's session log: p1 = A, p2 = B)."""
        if not self.session:
            raise ValueError("log needs --session (games from live.py start); read the game's own log otherwise")
        i = pc.player_index(self.player) - 1
        if i >= len(live.TAGS):
            raise ValueError("live.py starts games A and B only (p1, p2): %s has no session log" % self.player)
        x = live.TAGS[i]
        s = live.load(self.session)
        if self.offsets is None:
            self.offsets = {x: (s.get("offsets") or {}).get(x, 0)}   # from the end of the boot
        if match:
            # a match has a cursor of its own (it starts where the plain one is): the plain cursor stays, so the lines
            # the match left out are still there for the next plain `log`
            cur = self.match_offsets.setdefault(match, dict(self.offsets))
            lines = live.new_lines(self.session, {"games": s["games"], "offsets": cur}, x, match)
            return {"frame": self.client.last_frame, "lines": lines}
        lines = live.new_lines(self.session, {"games": s["games"], "offsets": self.offsets}, x)
        return {"frame": self.client.last_frame, "lines": lines}

    def check_logs(self, since="last_check", names=()):
        """The new flagged `coop:` lines of this game's log, masked (its own offsets: `log` and check_logs never take
        lines from each other)."""
        if not self.session:
            raise ValueError("check_logs needs --session (games from live.py start); read the game's own log otherwise")
        if since not in ("last_check", "boot"):
            raise ValueError("since: last_check or boot")
        if not isinstance(names, (list, tuple)) or len(names) > 20 or not all(isinstance(n, str) for n in names):
            raise ValueError("names: up to 20 words")
        i = pc.player_index(self.player) - 1
        if i >= len(live.TAGS):
            raise ValueError("live.py starts games A and B only (p1, p2): %s has no session log" % self.player)
        x = live.TAGS[i]
        s = live.load(self.session)
        path = (s.get("games") or {}).get(x, {}).get("log")
        boot = (s.get("offsets") or {}).get(x, 0)
        start = boot if since == "boot" or self.check_offset is None else self.check_offset
        if not path or not os.path.isfile(path):
            return {"frame": self.client.last_frame, "verdict": "no log", "flagged": 0, "lines": [], "left_out": 0,
                    "why": "this game's log file is not there (the session has no log path for %s)" % x}
        lines, self.check_offset = pc.read_log_since(path, start)
        hide = list(names) + [n for n in pc.session_names([g.get("log") for g in (s.get("games") or {}).values()])
                              if n not in names]
        out = pc.flagged_lines(lines, hide)
        out.update(frame=self.client.last_frame, read=len(lines), names_hidden=len(hide),
                   verdict="flagged" if out["flagged"] else "clean")
        return out

    def session_status(self):
        """This game's bridge, and the session's state from its files (no request to the game: it stays as it is)."""
        c = self.client
        out = {"player": self.player, "bridge": {"connected": c.sock is not None, "held": c.held,
                                                 "last_frame": c.last_frame, "connects": c.connects}}
        if not self.session:
            out["session"] = None
            out["why"] = "no --session: only this game's bridge is known"
            return out
        d = self.session
        s = live.load(d)
        why = live.ended(d)
        pids = live.game_pids(d)[0]
        now = time.time()
        sess = {"running": not why, "ended": why or None,
                "games": {x: {"pid": p, "alive": bool(self.alive(p))} for x, p in sorted(pids.items())}}
        if s.get("max_s"):
            sess["max_minutes_left"] = max(0, round((s.get("started", now) + s["max_s"] - now) / 60, 1))
        if s.get("booted") and s.get("idle_s"):
            sess["idle_minutes_left"] = max(0, round((live.activity(d, s) + s["idle_s"] - now) / 60, 1))
        sess["booted"] = bool(s.get("booted"))
        out["session"] = sess
        return out

    def call(self, name, args):
        if name == "session_status":
            return self.session_status()   # (also for an ended session; never counts as a tester command)
        if name == "check_logs":
            # it only reads the log file, so it answers after the session ended too (that is when it is needed)
            return self.check_logs(args.get("since", "last_check"), args.get("names") or ())
        if self.session:
            live.open_session(self.session)   # (an ended session's games are gone: say so, do not drive them)
            live.touch(self.session)          # the session's watchdog counts this as a tester command
        since = _since(args) if name == "get_state" else None   # (refused before anything is asked of the game)
        try:
            # no reply carries the whole inventory or the world list unless asked for (get_state inventory=true)
            raw = self._call(name, args)
            out = pc.slim_state(raw, inventory=name == "get_state" and bool(args.get("inventory")))
            if name == "get_state":
                out = self.history.reply({self.player: out}, since)[self.player]
            return out
        finally:
            if self.session:
                live.touch(self.session)      # (a long walk_to or warp is not idle time)

    def _go_to(self, args):
        c = self._driver()
        if args.get("place") is not None:
            st = pc.game_of(c.state())
            data = kit_route.load(st.get("scene") or "", self.route_dir)
            out = kit_route.go_to(c, data, goal=_word(args, "place"))
        elif args.get("id") is not None:
            out = ph.go_to_actor(c, _need_id(args), _word(args, "name"), _bounded(args, "near_m", ph.NEAR_M, 0.5, 10),
                                 int(_bounded(args, "max_frames", ph.MAX_FRAMES, 1, ph.MAX_FRAMES)))
        elif "x" in args and "z" in args:
            out = kit_route.go_to(c, None, x=_num(args, "x"), z=_num(args, "z"))
        else:
            raise ValueError("go_to: give `place`, or `id` and `name`, or `x` and `z`")
        st = c.state()
        out.pop("state", None)
        out["look"], self.last_look = kit_look.look(st, self.last_look)
        return out

    def _record_route(self, args):
        action = args.get("action")
        if action == "start":
            st = pc.game_of(self.client.state())
            if not st.get("scene"):
                raise ValueError("no scene in the state yet")
            self.recorder = kit_route.Recorder(st["scene"])
            self.recorder.add(st.get("pos"), st["scene"])
            return {"recording": True, "scene": st["scene"], "points": len(self.recorder.points)}
        if self.recorder is None:
            raise ValueError("no route is being recorded: call record_route start first")
        if action == "cancel":
            self.recorder = None
            return {"recording": False}
        if action == "save":
            st = pc.game_of(self.client.state())
            self.recorder.add(st.get("pos"), st.get("scene"))
            out = kit_route.save_route(self.recorder, args.get("name"), args.get("note"), self.route_dir)
            self.recorder = None
            return out
        raise ValueError("action: start, save or cancel")

    def _move(self, name, args):
        c = self._driver()

        def num(key, lo=None, hi=None, default=None):
            return default if args.get(key) is None else float(_num(args, key, lo, hi))

        frames = int(_bounded(args, "max_frames", 240 if name == "jump" else kit_move.MAX_FRAMES, 1, kit_move.MAX_FRAMES))
        who = _word(args, "name") if args.get("name") is not None else None
        if name == "shoot":
            at = args.get("at")
            if at is not None:
                if not isinstance(at, list) or len(at) != 3:
                    raise ValueError("at: [x, y, z] in metres")
                at = [float(_num({"at": v}, "at")) for v in at]
            out = kit_move.shoot(c, _word(args, "item"), ident=_ident(args), name=who, at=at,
                                 assist=bool(args.get("assist")), tilt_frames=int(num("tilt_frames", -60, 60, 0)),
                                 watch_frames=int(num("watch_frames", 4, 240, kit_move.WATCH_FRAMES)))
        elif name == "climb":
            face = None
            if args.get("face_x") is not None or args.get("face_z") is not None:
                face = (num("face_x"), num("face_z"))
            out = kit_move.climb(c, up=args.get("up", True) is not False, to_y=num("to_y"), face=face, max_frames=frames)
        elif name == "push":
            out = kit_move.push(c, _need_id(args), num("distance_m", 0.1, 20), who, yaw=num("yaw"), max_frames=frames)
        else:
            out = kit_move.jump(c, num("x"), num("z"), land_y=num("land_y"), tolerance=num("tolerance", 0.2, 5, 0.6),
                                max_frames=frames)
        out["look"], self.last_look = kit_look.look(c.state(), self.last_look)
        return out

    def _driver(self):
        """The client, wrapped to sample Link's place into the route being recorded (if any)."""
        return kit_route.Sampling(self.client, self.recorder) if self.recorder is not None else self.client

    def _call(self, name, args):
        c = self._driver()
        if name == "get_state":
            s = c.state()
            s.update(pc.world_words(s))   # life_word, enemy_near, danger beside the numbers
            return s
        if name == "get_world":
            r = c.world()
            if r["world"] is None:
                return {"frame": r["frame"], "actors": [], "why": "this module has no world list (an older build)"}
            return dict(pc.select_actors(r["world"], args.get("kind"), args.get("within_m"),
                                         args.get("limit", 50)), frame=r["frame"], world_frame=r["world"].get("frame"))
        if name == "look":
            text, self.last_look = kit_look.look(c.state(), self.last_look)
            return text
        if name == "wait_until":
            out, st = kit_wait.wait_until(c, args.get("what"), _bounded(args, "max_frames", 300, 1, kit_wait.MAX_FRAMES),
                                          ident=args.get("id"), condition=args.get("condition"))
            out["look"], self.last_look = kit_look.look(st, self.last_look)
            return out
        if name == "go_to":
            return self._go_to(args)
        if name == "record_route":
            return self._record_route(args)
        if name == "fight":
            def button(key, default):
                b = str(args.get(key, default)).upper()
                if b not in pc.BUTTONS:
                    raise ValueError("%s: one of %s" % (key, " ".join(pc.BUTTONS)))
                return b
            out = kit_fight.fight(c, _need_id(args), _word(args, "name"),
                                  int(_bounded(args, "max_frames", kit_fight.MCP_FRAMES, 1, kit_fight.MAX_FRAMES)),
                                  button("lock", kit_fight.LOCK), button("sword", kit_fight.SWORD),
                                  button("shield", kit_fight.SHIELD),
                                  int(_bounded(args, "min_life", kit_fight.MIN_LIFE, 0, 80)))
            out["look"], self.last_look = kit_look.look(c.state(), self.last_look)
            return out
        if name == "get_actor":
            r = c.world()
            w = r["world"] or {}
            a = pc.find_actor(w, args["id"])
            out = {"frame": r["frame"], "world_frame": w.get("frame"), "found": a is not None, "actor": a}
            if a is None:
                out["why"] = w.get("why") or ("not in the list: it is gone, or not among the 200 nearest"
                                              if r["world"] is not None else "this module has no world list")
            return out
        if name == "walk_to":
            return pc.walk_to(c, float(args["x"]), float(args["z"]), float(args.get("tolerance", 0.5)),
                              int(args.get("max_frames", 600)))
        if name == "press":
            buttons = [str(b).upper() for b in args.get("buttons") or []]
            bad = [b for b in buttons if b not in pc.BUTTONS]
            if bad:
                raise ValueError("no button %s" % ", ".join(bad))
            strength = float(args.get("stick_strength", 0))
            stick = [float(args.get("stick_angle", 0)), strength] if strength > 0 else None
            return c.input(stick, buttons, int(args.get("frames", 2)))
        if name == "step":
            return c.step(int(args["frames"]))
        if name == "step_until":
            return kit_tools.step_until(c, args.get("condition"), int(_bounded(args, "max_frames", 300, 1, 3600)),
                                        int(_bounded(args, "chunk", kit_tools.CHUNK, 1, kit_tools.MAX_CHUNK)))
        if name == "warp":
            return c.warp(args["scene"], args.get("entrance"), _bounded(args, "timeout", 60, 1, 600))
        if name == "flag":
            return c.flag(args["name"], args.get("value", 1))
        if name == "flag_read":
            return c.flag_read(args["name"])
        if name == "give":
            return c.give(args["item"])
        if name == "preset":
            return {"preset": args["name"], "steps": kit_setup.apply(c, kit_setup.load_preset(args["name"]),
                                                                      shared=args.get("_shared", True),
                                                                      log_dir=self.log_dir, player=self.player)}
        if name == "save_point":
            return kit_setup.save_point(c, self.log_dir, self.player, args["tag"])
        if name == "load_point":
            return kit_setup.load_point(c, self.log_dir, self.player, args["tag"], shared=args.get("_shared", True))
        if name == "setup":
            return {"steps": kit_setup.apply(c, {k: v for k, v in args.items() if k not in ("player", "_shared")},
                                             shared=args.get("_shared", True),
                                             log_dir=self.log_dir, player=self.player)}
        if name == "place" and any(k in args for k in ("facing", "face_x", "face_z")):
            return ph.place_facing(c, act_line(name, args), face_point(args))
        line = act_line(name, args)
        if line is not None:
            if name in ("kill", "hit"):
                wait = int(_bounded(args, "wait_frames", ph.DEATH_FRAMES, 0, 600)) if name == "kill" else None
                return ph.strike(c, line, _word(args, "actor"), _ident(args), death_frames=wait)
            return c.act(line)
        if name == "go_to_actor":
            return ph.go_to_actor(c, _need_id(args), _word(args, "name"), _bounded(args, "near_m", ph.NEAR_M, 0.5, 10),
                                  int(_bounded(args, "max_frames", ph.MAX_FRAMES, 1, ph.MAX_FRAMES)))
        if name in kit_move.VERBS:
            return self._move(name, args)
        if name == "face":
            return ph.face(c, _need_id(args), _word(args, "name"))
        if name == "attack":
            return ph.attack(c, _need_id(args), _button(args), _word(args, "name"),
                             int(_bounded(args, "max_frames", ph.MAX_FRAMES, 1, ph.MAX_FRAMES)))
        if name == "use_item":
            return ph.use_item(c, _word(args, "item"), _button(args, ph.ITEM_BUTTON))
        if name == "talk":
            reply = args.get("reply")
            many = reply if isinstance(reply, list) else [reply]
            if reply is not None and (not 1 <= len(many) <= 8 or any(
                    not isinstance(r, str) or not r.strip() or len(r) > 200 for r in many)):
                raise ValueError("reply: the text of the choice to pick (1 to 200 characters), or a list of 1 to 8")
            since = args.get("since")
            if since is not None and (isinstance(since, bool) or not isinstance(since, int) or since < 0):
                raise ValueError("since: the `since` of an earlier talk (a whole number, 0 or more)")
            ident = _ident(args)
            return ph.talk(c, ident, _word(args, "name") if ident is not None else None, reply,
                           int(_bounded(args, "max_boxes", ph.TALK_BOXES, 1, 60)), since=since)
        if name == "get_text":
            since = args.get("since", 0)
            if isinstance(since, bool) or not isinstance(since, int) or since < 0:
                raise ValueError("since: the `last` of an earlier get_text (a whole number, 0 or more)")
            layout = args.get("layout")
            if layout is not None and (not isinstance(layout, str) or not re.match(r"^[A-Za-z0-9_]{1,64}$", layout)):
                raise ValueError("layout: part of a layout's name (letters, digits and _)")
            return c.text(since, layout)
        if name == "open":
            return ph.interact(c, name)
        if name == "interact":
            return ph.interact_with(c, _need_id(args), _word(args, "name"),
                                    _bounded(args, "near_m", ph.INTERACT_M, 0.5, 10),
                                    int(_bounded(args, "max_frames", ph.MAX_FRAMES, 1, ph.MAX_FRAMES)))
        if name == "pause":
            return c.pause()
        if name == "resume":
            return c.resume()
        if name == "checkpoint":
            return pc.checkpoint(c, self.log_dir, self.player, args["tag"])
        if name == "desync_check":
            buf = io.StringIO()
            code = desync_check.check_clients(self.log_dir, bool(args.get("all")),
                                              out=lambda s: buf.write(s + "\n"), flags_only=bool(args.get("flags_only")))
            return {"result": desync_check.RESULTS[code], "report": buf.getvalue()}
        if name == "log":
            return self.log_lines(args.get("match"))
        if name == "screenshot":
            label = args.get("name") or "shot"
            if not pc.TAG.match(label):
                raise ValueError("a name is letters, digits, - and _")
            r = c.shot("%s-%s-%d" % (self.player, label, int(time.time() * 1000)))
            got = r.get("path")
            if got and os.path.isfile(got):   # a copy beside the run's logs: every game's shots in one place
                folder = os.path.join(self.log_dir, "shots")
                os.makedirs(folder, exist_ok=True)
                r["copy"] = shutil.copy(got, os.path.join(folder, os.path.basename(got)))
            return r
        raise ValueError("no tool %s" % name)


CALLS_FILE = "calls.jsonl"


# Two games from one server (--pair): every PLAYER_TOOLS tool with a `player`; the frames one game runs, the other runs too.
NO_FRAME_TOOLS = frozenset(("get_state", "look", "get_world", "get_actor", "get_text", "checkpoint", "desync_check",
                            "pause", "resume", "log", "check_logs", "session_status"))
BOTH_TOOLS = frozenset(("get_state", "step", "setup", "preset", "save_point", "load_point"))   # with no `player`: both games
ALWAYS_BOTH = frozenset(("pause", "resume"))    # both games always: one running free would leave the other behind
IN_STEP = (" Both games run the same frames: every frame this player's game runs, the other game runs too (walks, "
           "presses and steps as they happen; a warp's or a test line's frames right after it). `player` is the game. ")


def _pair_tool(tool, players):
    t = json.loads(json.dumps(tool))
    schema = t["inputSchema"]
    schema.setdefault("properties", {})["player"] = {"type": "string", "enum": list(players)}
    if t["name"] in ALWAYS_BOTH:
        t["description"] += " Both games, always (`games`, by player): a pair is held or runs together."
        del schema["properties"]["player"]
    elif t["name"] in BOTH_TOOLS:
        t["description"] += " With no `player`: both games (`games`, by player)."
    else:
        schema["required"] = ["player"] + list(schema.get("required", []))
    if t["name"] not in NO_FRAME_TOOLS:
        t["description"] += IN_STEP
    return t


class Pair:
    """Two games' tools in one server. A call that can run frames goes to its player's game through an InStep, so the
    other game runs the same frames as they happen; what ran outside input, move and step (a warp, a test line, a
    shot) is caught up after the call from the game's frame counts. Each reply says what the other game ran
    (`other`)."""

    def __init__(self, games):
        self.games = dict(games)   # player -> Game, two of them
        if len(self.games) != 2:
            raise ValueError("a pair is two games")
        self.tools = [_pair_tool(t, self.games) for t in PLAYER_TOOLS]

    def _other(self, player):
        return next(g for p, g in self.games.items() if p != player)

    def call(self, name, args):
        rest = {k: v for k, v in args.items() if k != "player"}
        player = args.get("player")
        if name in ALWAYS_BOTH or (player is None and name in BOTH_TOOLS):
            if name in ("setup", "preset", "load_point"):
                # Small keys are shared by count deltas: written in both games they would be added twice. The first
                # game writes them; the second leaves them out and gets them by the share.
                return {"games": {p: g.call(name, dict(rest, _shared=(i == 0))) for i, (p, g) in enumerate(self.games.items())}}
            return {"games": {p: g.call(name, dict(rest)) for p, g in self.games.items()}}
        if player not in self.games:
            raise ValueError("player is one of %s" % ", ".join(self.games))
        game, other = self.games[player], self._other(player)
        if name in NO_FRAME_TOOLS:
            return game.call(name, rest)
        own = game.client
        start = own.hello()["frame"]
        pair = ph.InStep(own, other.client, start)
        game.client = pair
        note = {"player": other.player}
        try:
            return game.call(name, rest)
        finally:
            game.client = own
            try:
                note["caught_up"] = pair.catch_up()
            except (OSError, ValueError, pc.BridgeError) as e:
                note["error"] = "%s: %s" % (e.__class__.__name__, e)
            note["frames"] = pair.stepped
            if pair.short:
                note["short"] = pair.short   # frames asked that this game did not run: the other did not run them
            note["in_step"] = "error" not in note
            self.last_other = note

    def answer(self, name, args):
        self.last_other = None
        out = self.call(name, args)
        if self.last_other is not None and isinstance(out, dict):
            out["other"] = self.last_other
        return out


class Args:
    def __init__(self, **kw):
        self.__dict__.update(kw)


def call(session, name, args):
    if name == "state":
        return live.state(Args(session=session, player=args.get("player"), since=_since(args)))
    if name == "act":
        refuse_face(args["script"])
        return live.do(Args(session=session, player=args["player"], script=args["script"]))
    if name == "shot":
        return live.shot(Args(session=session, player=args.get("player")))
    if name == "checkpoint":
        return live.checkpoint(Args(session=session, tag=args["tag"]))
    if name == "desync_check":
        buf = io.StringIO()
        code = desync_check.check(session, bool(args.get("all")), out=lambda s: buf.write(s + "\n"))
        return {"result": ["agree", "desync", "nothing to compare", "incomplete"][code], "report": buf.getvalue()}
    if name == "quit_game":
        return _done(live.quit_one(Args(session=session, player=args["player"], kill=bool(args.get("kill")))))
    if name == "server":
        return _done(live.server_cmd(Args(session=session, action=args["action"])))
    if name == "relaunch":
        return _done(live.relaunch(Args(session=session, player=args["player"], boot=args.get("boot"),
                                        no_load=bool(args.get("no_load")), restore=bool(args.get("restore")))))
    if name == "wait_log":
        return _done(live.wait_log(session, args["player"], args["pattern"], args.get("min", 1), args.get("max"),
                                   args.get("timeout_s", 60)))
    if name == "sleep":
        live.touch(os.path.abspath(session))
        time.sleep(float(args["seconds"]))
        live.touch(os.path.abspath(session))
        return {"slept": args["seconds"]}
    raise ValueError("no tool %s" % name)


def _done(out):
    """A session tool's answer; one that did not do its job raises, so a script stops there (replay)."""
    if out.get("error") or out.get("ok") is False:
        raise RuntimeError(json.dumps(out))
    return out


def reply(id_, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": id_}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def call_log_path(session, log_dir):
    """Where a server writes its calls: `calls.jsonl` in the session folder, else in the run's log folder."""
    return os.path.join(session or log_dir or "playtest-logs", CALLS_FILE)


def serve(session, game=None, calllog=None):
    """Answers MCP requests on stdin: the two-game session's tools (session), one game's (game), or a pair's (a Pair).
    `calllog`: the file every call is also written to (a CallLog; docs/playtest.md, "Kit helpers")."""
    if isinstance(game, Pair):
        tools, name = game.tools, "oot-pair"
    else:
        tools = PLAYER_TOOLS if game else TOOLS
        name = ("oot-%s" % game.player) if game else "oot-playtest"
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError:
            reply(None, error={"code": -32700, "message": "parse error"})
            continue
        method, id_ = req.get("method"), req.get("id")
        if id_ is None:
            continue  # a notification (initialized, cancelled): nothing to answer
        if method == "initialize":
            reply(id_, {"protocolVersion": (req.get("params") or {}).get("protocolVersion", "2024-11-05"),
                        "capabilities": {"tools": {}},
                        "serverInfo": {"name": name, "version": "2"}})
        elif method == "tools/list":
            reply(id_, {"tools": tools})
        elif method == "tools/call":
            params = req.get("params") or {}
            began = time.time()
            try:
                args = params.get("arguments") or {}
                if isinstance(game, Pair):
                    out = game.answer(params.get("name"), args)
                else:
                    out = game.call(params.get("name"), args) if game else call(session, params.get("name"), args)
                if calllog:
                    calllog.write(params.get("name"), args, True, t=began)
                reply(id_, {"content": [{"type": "text", "text": out if isinstance(out, str) else json.dumps(out, indent=1)}]})
            except (Exception, SystemExit) as e:  # (SystemExit from a bad tag too: an error the tester reads)
                if calllog and isinstance(params.get("name"), str):
                    calllog.write(params["name"], params.get("arguments"), False, "%s: %s" % (e.__class__.__name__, e),
                                  t=began)
                reply(id_, {"content": [{"type": "text", "text": "%s: %s" % (e.__class__.__name__, e)}],
                            "isError": True})
        elif method == "ping":
            reply(id_, {})
        else:
            reply(id_, error={"code": -32601, "message": "no method %s" % method})


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--session", help="a two-game session (live.py start): the tools state, act, shot, checkpoint, "
                   "desync_check; with --player: the session whose watchdog each call keeps awake")
    p.add_argument("--player", choices=pc.PLAYERS, help="one game's tools, through its play-test bridge")
    p.add_argument("--pair", action="store_true", help="both games' tools (p1 and p2) from one server, each call "
                   "with a `player`; every frame one game runs, the other runs too")
    p.add_argument("--port", type=int, help="that game's bridge port (default: the player's, on --lane when given)")
    p.add_argument("--lane", type=int, help="a test lane (tools/autotest/lanes.py): its bridge ports")
    p.add_argument("--log-dir", default="playtest-logs",
                   help="the run's checkpoint logs and shots; give every game's server the same one")
    p.add_argument("--call-log", metavar="FILE", help="write every call here, for `live.py replay` (default: %s in "
                   "the session folder, else in --log-dir)" % CALLS_FILE)
    p.add_argument("--no-call-log", action="store_true", help="write no call log")
    a = p.parse_args(argv)
    if a.pair:
        if a.player or a.port:
            p.error("--pair takes both games' ports from --lane (or the default ports): no --player or --port")
        session = os.path.abspath(a.session) if a.session else None
        serve(None, Pair({q: Game(q, pc.Client(pc.port_for(q, a.lane)), os.path.abspath(a.log_dir), session)
                          for q in ("p1", "p2")}),
              calllog=None if a.no_call_log else kit_tools.CallLog(
                  a.call_log or call_log_path(session, os.path.abspath(a.log_dir)), "oot-pair"))
        return 0
    if not a.player:
        if not a.session:
            p.error("give --session, or --player (and --session when the games came from live.py start)")
        session = os.path.abspath(a.session)
        serve(session, calllog=None if a.no_call_log else kit_tools.CallLog(
            a.call_log or call_log_path(session, None), "oot-playtest"))
        return 0
    session = os.path.abspath(a.session) if a.session else None
    serve(None, Game(a.player, pc.Client(a.port or pc.port_for(a.player, a.lane)), os.path.abspath(a.log_dir),
                     session),
          calllog=None if a.no_call_log else kit_tools.CallLog(
              a.call_log or call_log_path(session, os.path.abspath(a.log_dir)), "oot-" + a.player))
    return 0


if __name__ == "__main__":
    sys.exit(main())
