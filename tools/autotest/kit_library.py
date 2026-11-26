"""The shared play-test library: scripts and data that testers (people and agents) write down for the next one.

    tools/playtest/scripts/NAME.jsonl   a script: a header of `# key: value` lines, then one kit call per line
    tools/playtest/data/NAME.json       facts about the game: named positions, routes, menu paths, stuck spots

Only information about the game goes in (numbers we wrote down), never a game file, a log or a shot (AGENTS.md).
This module checks both kinds of file, so a broken one fails CI, and turns a call log into a script:

    python tools/autotest/kit_library.py check [--root tools/playtest]
    python tools/autotest/kit_library.py save CALLS.jsonl --out tools/playtest/scripts/NAME.jsonl
           --what TEXT --scene NAME --start TEXT --expect TEXT [--keep-reads]
"""

import argparse
import json
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit_tools  # noqa: E402

KIT_VERSION = 1   # raised when a call's name or arguments change in a way an older script does not survive
ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "tools", "playtest")
FILE_NAME = re.compile(r"^[a-z0-9][a-z0-9-]*$")
SCENE = re.compile(r"^[A-Za-z0-9_]+$")
HEADER = re.compile(r"^#\s*([a-z]+)\s*:\s*(.*)$")
HEADER_KEYS = ("what", "scene", "start", "expect", "kit")   # all required, in this order
MAX_TEXT = 300       # a longer text is a pasted log, not a note
MAX_NUMBER = 1e6
CALL_KEYS = {"t", "server", "tool", "args", "ok", "error"}


# --- a small JSON-schema check for a call's arguments --------------------------------------------------------------

def _type_ok(kind, v):
    if kind == "string":
        return isinstance(v, str)
    if kind == "boolean":
        return isinstance(v, bool)
    if kind == "integer":
        return isinstance(v, int) and not isinstance(v, bool)
    if kind == "number":
        return isinstance(v, (int, float)) and not isinstance(v, bool)
    if kind == "array":
        return isinstance(v, list)
    if kind == "object":
        return isinstance(v, dict)
    return True


def check_value(schema, v, where):
    """The problems of one value against a (small) JSON schema: type, enum, minimum, maximum, array items."""
    out = []
    kind = schema.get("type")
    if kind and not _type_ok(kind, v):
        return ["%s: should be a %s" % (where, kind)]
    if isinstance(v, str) and len(v) > MAX_TEXT:
        out.append("%s: longer than %d characters (a note, not a log)" % (where, MAX_TEXT))
    if "enum" in schema and v not in schema["enum"]:
        out.append("%s: %r is not one of %s" % (where, v, ", ".join(map(str, schema["enum"]))))
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        if "minimum" in schema and v < schema["minimum"]:
            out.append("%s: %s is below %s" % (where, v, schema["minimum"]))
        if "maximum" in schema and v > schema["maximum"]:
            out.append("%s: %s is above %s" % (where, v, schema["maximum"]))
    if isinstance(v, list) and "items" in schema:
        for i, item in enumerate(v):
            out += check_value(schema["items"], item, "%s[%d]" % (where, i))
    return out


def tool_table():
    """{server kind: {tool name: input schema}}: "player" (oot-p1..p4), "pair" (oot-pair: the player tools with a `player`)
    and "session" (oot-playtest, live)."""
    import mcp_playtest
    return {"player": {t["name"]: t.get("inputSchema", {}) for t in mcp_playtest.PLAYER_TOOLS},
            "pair": {t["name"]: mcp_playtest._pair_tool(t, ("p1", "p2")).get("inputSchema", {})
                     for t in mcp_playtest.PLAYER_TOOLS},
            "session": {t["name"]: t.get("inputSchema", {}) for t in mcp_playtest.TOOLS}}


def server_kind(server):
    return "session" if server in ("oot-playtest", "live") else "pair" if server == "oot-pair" else "player"


def check_call(call, table=None):
    """The problems of one call line {"server", "tool", "args"}."""
    table = table or tool_table()
    server, tool, args = call.get("server"), call.get("tool"), call.get("args", {})
    if not isinstance(server, str) or not kit_tools.NAME.match(server):
        return ["server %r is not oot-p1 to oot-p4, oot-pair, oot-playtest or live" % (server,)]
    if not isinstance(args, dict):
        return ["args must be an object"]
    extra = sorted(set(call) - CALL_KEYS)
    if extra:
        return ["unknown key %s (a call has %s)" % (", ".join(extra), ", ".join(sorted(CALL_KEYS)))]
    tools = table[server_kind(server)]
    if tool not in tools:
        return ["%s has no tool %r" % (server, tool)]
    schema = tools[tool]
    props = schema.get("properties", {})
    out = ["%s: missing argument %s" % (tool, k) for k in schema.get("required", []) if k not in args]
    for k, v in args.items():
        if k not in props:
            out.append("%s: no argument %s (it has %s)" % (tool, k, ", ".join(sorted(props)) or "none"))
        else:
            out += check_value(props[k], v, "%s.%s" % (tool, k))
    return out


# --- scripts --------------------------------------------------------------------------------------------------------

def read_script(path):
    """(header dict, [(line number, text)]) of a script file: `# key: value` lines, then the calls."""
    header, lines = {}, []
    with open(path, encoding="utf-8") as f:
        for n, ln in enumerate(f, 1):
            ln = ln.rstrip("\r\n")
            if not ln.strip():
                continue
            if ln.lstrip().startswith("#"):
                m = HEADER.match(ln.strip())
                if m and m.group(1) in HEADER_KEYS and m.group(1) not in header:
                    header[m.group(1)] = m.group(2).strip()
                continue
            lines.append((n, ln))
    return header, lines


def check_script(path, table=None):
    """The problems of one script file, each with its line."""
    name = os.path.basename(path)
    out = []
    if not path.endswith(".jsonl") or not FILE_NAME.match(name[:-6]):
        out.append("%s: the name is lowercase words with hyphens and ends in .jsonl" % name)
    try:
        header, lines = read_script(path)
    except (OSError, UnicodeDecodeError) as e:
        return out + ["%s: cannot read (%s)" % (name, e)]
    for k in HEADER_KEYS:
        if not header.get(k):
            out.append("%s: the header has no `# %s: ...` line" % (name, k))
    for k, v in header.items():
        if len(v) > MAX_TEXT:
            out.append("%s: header %s is longer than %d characters" % (name, k, MAX_TEXT))
    if header.get("scene") and not SCENE.match(header["scene"]):
        out.append("%s: header scene is one map name (letters, digits, underscore)" % name)
    if header.get("kit"):
        if not header["kit"].isdigit() or not 1 <= int(header["kit"]) <= KIT_VERSION:
            out.append("%s: header kit is a number from 1 to %d" % (name, KIT_VERSION))
    if not lines:
        out.append("%s: no calls" % name)
    table = table or tool_table()
    for n, text in lines:
        try:
            call = json.loads(text)
        except ValueError as e:
            out.append("%s:%d: not JSON (%s)" % (name, n, e))
            continue
        if not isinstance(call, dict):
            out.append("%s:%d: a call is a JSON object" % (name, n))
            continue
        out += ["%s:%d: %s" % (name, n, p) for p in check_call(call, table)]
    return out


def save_script(calls_files, out_path, what, scene, start, expect, keep_reads=False, keep_failed=False):
    """Writes the calls of one or more call logs as a script (looks and failed calls left out by default).
    Returns the number of calls written."""
    calls = kit_tools.read_calls(calls_files)
    keep = []
    for c in calls:
        if c["tool"] in kit_tools.NOT_REPLAYED or (c["tool"] in kit_tools.READ_ONLY and not keep_reads) or (not c.get("ok", True) and not keep_failed):
            continue
        keep.append({"server": c["server"], "tool": c["tool"], "args": c.get("args") or {}})
    head = ["# what: %s" % what, "# scene: %s" % scene, "# start: %s" % start, "# expect: %s" % expect,
            "# kit: %d" % KIT_VERSION]
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(head + [json.dumps(c, separators=(",", ":")) for c in keep]) + "\n")
    return len(keep)


# --- data files -----------------------------------------------------------------------------------------------------

DATA_KEYS = {"kit", "title", "scene", "positions", "routes", "menu_paths", "stuck_spots", "notes"}
POSITION_KEYS = {"x", "y", "z", "yaw", "note"}


def _num(v, where, lo=-MAX_NUMBER, hi=MAX_NUMBER):
    if not isinstance(v, (int, float)) or isinstance(v, bool) or not math.isfinite(v) or not lo <= v <= hi:
        return ["%s: should be a number from %g to %g" % (where, lo, hi)]
    return []


def _text(v, where, required=True):
    if not isinstance(v, str) or (required and not v.strip()):
        return ["%s: should be a text" % where]
    return ["%s: longer than %d characters (a note, not a log)" % (where, MAX_TEXT)] if len(v) > MAX_TEXT else []


def _name(k, where):
    return [] if isinstance(k, str) and FILE_NAME.match(k.replace("_", "-")) else ["%s: the name %r is lowercase words with hyphens" % (where, k)]


def _point(p, where, keys):
    if not isinstance(p, dict):
        return ["%s: should be an object" % where]
    out = ["%s: no key %s (it has %s)" % (where, k, ", ".join(sorted(keys))) for k in sorted(set(p) - keys)]
    for k in ("x", "z"):
        out += _num(p[k], "%s.%s" % (where, k)) if k in p else ["%s: has no %s" % (where, k)]
    for k in ("y",):
        if k in p:
            out += _num(p[k], "%s.%s" % (where, k))
    if "yaw" in p:
        out += _num(p["yaw"], where + ".yaw", -180, 360)
    if "note" in p:
        out += _text(p["note"], where + ".note")
    return out


def check_data(path):
    """The problems of one data file."""
    name = os.path.basename(path)
    out = []
    if not path.endswith(".json") or not FILE_NAME.match(name[:-5]):
        out.append("%s: the name is lowercase words with hyphens and ends in .json" % name)
    try:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError) as e:
        return out + ["%s: cannot read as JSON (%s)" % (name, e)]
    if not isinstance(d, dict):
        return out + ["%s: should be a JSON object" % name]
    out += ["%s: no key %s (it has %s)" % (name, k, ", ".join(sorted(DATA_KEYS))) for k in sorted(set(d) - DATA_KEYS)]
    if not isinstance(d.get("scene"), str) or not SCENE.match(d.get("scene", "")):
        out.append("%s: scene is one map name (letters, digits, underscore)" % name)
    if not (isinstance(d.get("kit"), int) and not isinstance(d["kit"], bool) and 1 <= d["kit"] <= KIT_VERSION):
        out.append("%s: kit is a number from 1 to %d" % (name, KIT_VERSION))
    if "title" in d:
        out += ["%s: %s" % (name, p) for p in _text(d["title"], "title")]
    pos = d.get("positions", {})
    if not isinstance(pos, dict):
        out.append("%s: positions should be an object" % name)
        pos = {}
    for k, p in pos.items():
        out += ["%s: %s" % (name, p2) for p2 in _name(k, "positions") + _point(p, "positions.%s" % k, POSITION_KEYS)]
    routes = d.get("routes", {})
    if not isinstance(routes, dict):
        out.append("%s: routes should be an object" % name)
        routes = {}
    for k, r in routes.items():
        w = "routes.%s" % k
        probs = _name(k, "routes")
        if not isinstance(r, dict):
            probs.append("%s: should be an object" % w)
        else:
            probs += ["%s: no key %s (it has start, steps, note)" % (w, x) for x in sorted(set(r) - {"start", "steps", "note"})]
            if r.get("start") not in pos:
                probs.append("%s.start: %r is not one of this file's positions" % (w, r.get("start")))
            steps = r.get("steps")
            if not isinstance(steps, list) or not steps:
                probs.append("%s.steps: should be a list of position names" % w)
            else:
                probs += ["%s.steps[%d]: %r is not one of this file's positions" % (w, i, s) for i, s in enumerate(steps) if s not in pos]
            if "note" in r:
                probs += _text(r["note"], w + ".note")
        out += ["%s: %s" % (name, p) for p in probs]
    menus = d.get("menu_paths", {})
    if not isinstance(menus, dict):
        out.append("%s: menu_paths should be an object" % name)
        menus = {}
    import playtest_client as pc
    for k, m in menus.items():
        w = "menu_paths.%s" % k
        probs = _name(k, "menu_paths")
        if not isinstance(m, dict):
            probs.append("%s: should be an object" % w)
        else:
            probs += ["%s: no key %s (it has from, presses, note)" % (w, x) for x in sorted(set(m) - {"from", "presses", "note"})]
            probs += _text(m.get("from"), w + ".from")
            pr = m.get("presses")
            if not isinstance(pr, list) or not pr:
                probs.append("%s.presses: should be a list of buttons" % w)
            else:
                probs += ["%s.presses[%d]: %r is not one of %s" % (w, i, b, " ".join(pc.BUTTONS)) for i, b in enumerate(pr) if b not in pc.BUTTONS]
            if "note" in m:
                probs += _text(m["note"], w + ".note")
        out += ["%s: %s" % (name, p) for p in probs]
    stuck = d.get("stuck_spots", [])
    if not isinstance(stuck, list):
        out.append("%s: stuck_spots should be a list" % name)
        stuck = []
    for i, s in enumerate(stuck):
        w = "stuck_spots[%d]" % i
        probs = _point({k: v for k, v in s.items() if k in POSITION_KEYS} if isinstance(s, dict) else s, w, POSITION_KEYS)
        if isinstance(s, dict):
            probs += ["%s: no key %s (it has x, y, z, radius, note, fix)" % (w, x) for x in sorted(set(s) - POSITION_KEYS - {"radius", "fix"})]
            probs += _num(s.get("radius"), w + ".radius", 0.1, 50)
            probs += _text(s.get("note"), w + ".note")
            if "fix" in s:
                probs += _text(s["fix"], w + ".fix")
        out += ["%s: %s" % (name, p) for p in probs]
    notes = d.get("notes", [])
    if not isinstance(notes, list):
        out.append("%s: notes should be a list of texts" % name)
    else:
        out += ["%s: %s" % (name, p) for i, n in enumerate(notes) for p in _text(n, "notes[%d]" % i)]
    return out


# --- the whole folder -----------------------------------------------------------------------------------------------

def check_all(root=ROOT):
    """(problems, number of scripts, number of data files) of a playtest folder."""
    table = tool_table()
    problems, scripts, data = [], 0, 0
    for sub, ext, check in (("scripts", ".jsonl", lambda p: check_script(p, table)), ("data", ".json", check_data)):
        folder = os.path.join(root, sub)
        if not os.path.isdir(folder):
            problems.append("%s: the folder is missing" % sub)
            continue
        for name in sorted(os.listdir(folder)):
            full = os.path.join(folder, name)
            if name == "README.md" or not os.path.isfile(full):
                continue
            if not name.endswith(ext):
                problems.append("%s/%s: only %s files and the README.md go here" % (sub, name, ext))
                continue
            problems += check(full)
            scripts += sub == "scripts"
            data += sub == "data"
    return problems, scripts, data


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check", help="check every script and data file")
    c.add_argument("--root", default=ROOT)
    s = sub.add_parser("save", help="write a call log as a script")
    s.add_argument("calls", nargs="+", help="call log files (calls.jsonl)")
    s.add_argument("--out", required=True)
    for k in ("what", "scene", "start", "expect"):
        s.add_argument("--" + k, required=True)
    s.add_argument("--keep-reads", action="store_true")
    s.add_argument("--keep-failed", action="store_true")
    a = ap.parse_args(argv)
    if a.cmd == "check":
        problems, scripts, data = check_all(a.root)
        for p in problems:
            print(p)
        print("%d scripts, %d data files, %d problems" % (scripts, data, len(problems)))
        return 1 if problems else 0
    n = save_script(a.calls, a.out, a.what, a.scene, a.start, a.expect, a.keep_reads, a.keep_failed)
    problems = check_script(a.out)
    print("%d calls written to %s" % (n, a.out))
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
