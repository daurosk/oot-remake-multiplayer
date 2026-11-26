"""Pieces of the play-test kit that need no game and no build (docs/playtest.md, "Kit helpers"). Standard library only.

    step_until(client, "event == false")      step the game until a state field matches (or the frames run out)
    StateHistory                              the last states a tester looked at: `since` gives only what changed
    CallLog, replay                           every kit call in a file, and `live.py replay` to run them again
"""
import json
import os
import re
import time

# --- step_until: a condition on one state field -----------------------------------------------------------------

OPS = ("==", "!=", "<=", ">=", "<", ">")
CONDITION = re.compile(r"^(?P<neg>!\s*)?(?P<path>[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)*)"
                       r"(?:\s*(?P<op>==|!=|<=|>=|<|>)\s*(?P<val>.+?))?\s*$")
MAX_FRAMES = 3600
CHUNK = 8       # frames per look: a state is read after each (the game is held between them)
MAX_CHUNK = 60


class ConditionError(ValueError):
    """A condition that cannot be read, or that names a field the state does not have."""


def parse_condition(text):
    """`path`, `!path`, or `path OP value` (OP: == != < <= > >=) as (path parts, op, value, negated). The path is dotted
    (`event`, `equips.weapon`, `pos.1`); the value is JSON (true, false, null, 12, "Name") or a bare word (a string).
    A bare path means "is not null, false, 0 or empty"; `!path` the opposite."""
    m = CONDITION.match(str(text or ""))
    if not m:
        raise ConditionError("condition: `field`, `!field` or `field OP value` with OP one of %s (got %r)"
                             % (" ".join(OPS), text))
    if (m.group("neg") and m.group("op")) or (m.group("op") and m.group("val") is None):
        raise ConditionError("condition: `!` goes with a bare field only (got %r)" % text)
    val = None
    if m.group("op"):
        raw = m.group("val")
        try:
            val = json.loads(raw)
        except ValueError:
            val = raw.strip("'\"")
    return m.group("path").split("."), m.group("op"), val, bool(m.group("neg"))


def lookup(state, parts):
    """(found, value) of a dotted path in a state; a number part indexes a list."""
    cur = state
    for p in parts:
        if isinstance(cur, dict) and p in cur:
            cur = cur[p]
        elif isinstance(cur, list) and p.isdigit() and int(p) < len(cur):
            cur = cur[int(p)]
        else:
            return False, None
    return True, cur


def _number(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def evaluate(cond, state):
    """(met, value, why): met is True or False; why says when the field was not there or not comparable (a null is
    "do not know", never a match for an ordering: docs/playtest.md, "State")."""
    parts, op, want, neg = cond
    found, got = lookup(state, parts)
    if not found:
        return False, None, "no field %s in the state" % ".".join(parts)
    if op is None:
        return (not got) if neg else bool(got), got, None
    if op in ("==", "!="):
        if _number(got) and _number(want):
            same = float(got) == float(want)
        else:
            same = type(got) is type(want) and got == want
        return (same if op == "==" else not same), got, None
    if not (_number(got) and _number(want)):
        return False, got, "%s is %r: %s needs numbers" % (".".join(parts), got, op)
    ok = {"<": got < want, "<=": got <= want, ">": got > want, ">=": got >= want}[op]
    return ok, got, None


def step_until(client, condition, max_frames=300, chunk=CHUNK):
    """Steps the game `chunk` frames at a time (it is held between them) until the condition holds on the state, or
    `max_frames` frames have passed. Looks first, so a condition that holds already costs no frame. The reply says
    `met`, the field's `value` now, the frames run and the last state. A field the first state does not have is an error
    (a typo is not a time-out); a null later is "do not know" and the wait goes on."""
    cond = parse_condition(condition)
    max_frames, chunk = int(max_frames), int(chunk)
    if not 1 <= max_frames <= MAX_FRAMES:
        raise ValueError("max_frames is 1 to %d" % MAX_FRAMES)
    if not 1 <= chunk <= MAX_CHUNK:
        raise ValueError("chunk is 1 to %d" % MAX_CHUNK)
    used, looks = 0, 0
    while True:
        st = client.state()
        looks += 1
        met, got, why = evaluate(cond, st)
        if looks == 1 and why and why.startswith("no field"):
            raise ConditionError("%s (fields: %s)" % (why, ", ".join(sorted(k for k in st if not k.startswith("_")))))
        out = {"met": met, "condition": str(condition).strip(), "value": got, "frames_run": used, "state": st}
        if met:
            return out
        if used >= max_frames:
            out["why"] = "not met after %d frames%s" % (used, " (%s)" % why if why else "")
            return out
        n = min(chunk, max_frames - used)
        client.step(n)
        used += n


# --- delta state: only what changed since an earlier look ---------------------------------------------------------

VOLATILE = ("frame", "id", "ok", "paused", "snapshot_present", "reconnected", "state_id", "log", "world_frame",
            "inventory")   # (`inventory_count` says it changed)
KEEP = 16   # looks kept


def flatten(d, prefix=""):
    """{dotted path: value} of a state: nested objects are walked, lists and scalars are leaves."""
    out = {}
    for k, v in d.items():
        if not prefix and k in VOLATILE:
            continue
        path = prefix + k
        if isinstance(v, dict) and v:
            out.update(flatten(v, path + "."))
        else:
            out[path] = v
    return out


def delta(old, new):
    """What changed from state `old` to `new`: {"changed": {path: new value}, "removed": [paths]}. `frame` and the
    other counters that move on every look are left out."""
    a, b = flatten(old), flatten(new)
    return {"changed": {k: v for k, v in b.items() if k not in a or a[k] != v},
            "removed": sorted(k for k in a if k not in b)}


def _write(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f)
    for i in range(20):   # (Windows: a reader holding the old file a moment refuses the replace)
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if i == 19:
                raise
            time.sleep(0.05)


class StateHistory:
    """The last KEEP looks: an id for each, and what every player showed then. In memory (an MCP server), or in a file
    (`path`: live.py runs once per call, so the history lives beside the session)."""

    def __init__(self, path=None, keep=KEEP, start=1):
        """`start`: the first id. Each player's server starts at its own number, so an id from one player's server
        is "not kept" on another's and cannot pass for one of its looks."""
        self.path, self.keep = path, keep
        self.data = {"next": start, "looks": {}}
        if path:
            try:
                with open(path, encoding="utf-8") as f:
                    got = json.load(f)
                if isinstance(got, dict) and isinstance(got.get("looks"), dict):
                    self.data = got
            except (OSError, ValueError):
                pass

    def add(self, states):
        """Keeps one look ({player: state}) and returns its id."""
        n = int(self.data["next"])
        self.data["next"] = n + 1
        self.data["looks"][str(n)] = {k: {a: b for a, b in v.items() if a not in ("log",)} for k, v in states.items()}
        for old in sorted(self.data["looks"], key=int)[:-self.keep]:
            del self.data["looks"][old]
        if self.path:
            _write(self.path, self.data)
        return n

    def get(self, n):
        return self.data["looks"].get(str(n))

    def kept(self):
        ids = sorted(int(i) for i in self.data["looks"])
        return (ids[0], ids[-1]) if ids else None

    def reply(self, states, since=None):
        """{player: reply}: with no `since` each state whole and the look's `state_id`; with one, only what changed
        since that look. An id that is not kept (too old, or from another run) gives the whole state and says why."""
        old = self.get(since) if since is not None else None
        n = self.add(states)
        out = {}
        for k, st in states.items():
            if since is None:
                out[k] = dict(st, state_id=n)
            elif old is not None and k in old:
                out[k] = dict(delta(old[k], st), state_id=n, since=int(since), frame=st.get("frame"))
                if "log" in st:
                    out[k]["log"] = st["log"]   # (the log lines are new since the last look already)
            else:
                kept = self.kept()
                out[k] = dict(st, state_id=n, why="no look %s kept (kept: %s): the whole state" % (
                    since, "%d to %d" % kept if kept else "none"))
        return out


# --- the call log and its replay ------------------------------------------------------------------------------------

READ_ONLY = frozenset(("state", "get_state", "get_world", "get_actor", "log", "check_logs", "session_status", "pause",
                       "resume", "shot", "screenshot", "look"))   # nothing to run again: they only look (or move a log cursor)
NOT_REPLAYED = frozenset(("record_route",))   # writes a repo data file, not the game: a replay would only be refused
NAME = re.compile(r"^(oot-p[1-4]|oot-pair|oot-playtest|live)$")   # oot-pair: mcp_playtest --pair (p1 and p2)


class CallLog:
    """One JSON line per kit call: {"t" (when it began), "server", "tool", "args", "ok", "error"}. Every server of a session appends to
    one file (`calls.jsonl` in the session folder), a line in one write, so the lines never mix."""

    def __init__(self, path, server):
        self.path, self.server = path, server

    def write(self, tool, args, ok=True, error=None, t=None):
        line = {"t": round(time.time() if t is None else t, 3), "server": self.server, "tool": tool, "args": args or {}, "ok": bool(ok)}
        if error:
            line["error"] = str(error)[:200]
        try:
            with open(self.path, "ab") as f:
                f.write((json.dumps(line, separators=(",", ":")) + "\n").encode("utf-8"))
        except OSError:
            pass   # (a log that cannot be written never stops a test)


def read_calls(paths):
    """The calls of one or more log files, oldest first. A line that is not a call (a cut last line) is left out."""
    out = []
    for p in ([paths] if isinstance(paths, str) else paths):
        with open(p, encoding="utf-8") as f:
            for n, ln in enumerate(f, 1):
                try:
                    c = json.loads(ln)
                except ValueError:
                    continue
                if isinstance(c, dict) and NAME.match(str(c.get("server"))) and isinstance(c.get("tool"), str):
                    c["_at"] = "%s:%d" % (os.path.basename(p), n)
                    out.append(c)
    out.sort(key=lambda c: c["t"] if isinstance(c.get("t"), (int, float)) and not isinstance(c.get("t"), bool) else 0)
    # (stable: lines of one file keep their order; a missing or odd t sorts first)
    return out


def replay(files, session=None, lane=None, log_dir=None, include_reads=False, include_failed=False, keep_going=False,
           dry_run=False, delay=0.0, say=print, runner=None):
    """Runs the calls of a call log again, in order. Calls that only look are left out (`include_reads`), and so are the
    ones that failed the first time (`include_failed`). It stops at the first call that fails now (`keep_going`: runs on).
    `runner(server, tool, args)` is what runs one call (a test gives its own); the default drives the games of `session`
    (server oot-p1..p4: that game's bridge, on `lane`'s ports; oot-pair: p1 and p2 in step, as --pair runs them;
    oot-playtest and live: the session's own tools).
    Returns {"ran", "skipped", "failed": [[position, error]]}."""
    calls = read_calls(files)
    runner = runner or _Runner(session, lane, log_dir)
    ran, skipped, failed = 0, 0, []
    try:
        for c in calls:
            tool = c["tool"]
            if tool in NOT_REPLAYED or (tool in READ_ONLY and not include_reads) or (not c.get("ok", True) and not include_failed):
                skipped += 1
                continue
            say("%s %s %s %s" % (c["_at"], c["server"], tool, json.dumps(c.get("args") or {}, separators=(",", ":"))))
            if dry_run:
                ran += 1
                continue
            try:
                runner(c["server"], tool, c.get("args") or {})
                ran += 1
            except (Exception, SystemExit) as e:
                failed.append([c["_at"], "%s: %s" % (e.__class__.__name__, e)])
                say("  failed: %s" % failed[-1][1])
                if not keep_going:
                    break
            if delay:
                time.sleep(delay)
    finally:
        close = getattr(runner, "close", None)
        if close:
            close()
    return {"ran": ran, "skipped": skipped, "failed": failed}


class _Runner:
    """Runs a logged call against the live games (imports the rest of the kit when first used)."""

    def __init__(self, session, lane, log_dir):
        self.session, self.lane, self.log_dir = session, lane, log_dir or "playtest-logs"
        self.games = {}
        self.pair = None   # the oot-pair calls' Pair, over the same Games

    def __call__(self, server, tool, args):
        import mcp_playtest
        import playtest_client as pc
        if server in ("oot-playtest", "live"):
            if not self.session:
                raise ValueError("a session folder is needed (--session)")
            return mcp_playtest.call(self.session, tool, args)
        def game(player):   # one client per game: a bridge serves one at a time, so oot-p1 and oot-pair share it
            if player not in self.games:
                self.games[player] = mcp_playtest.Game(player, pc.Client(pc.port_for(player, self.lane)),
                                                       os.path.abspath(self.log_dir), self.session)
            return self.games[player]
        if server == "oot-pair":
            if self.pair is None:
                self.pair = mcp_playtest.Pair({q: game(q) for q in ("p1", "p2")})
            return self.pair.answer(tool, args)
        return game(server[4:]).call(tool, args)

    def close(self):
        for g in self.games.values():
            g.client.close()
