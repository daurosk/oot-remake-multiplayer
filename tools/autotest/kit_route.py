"""`go_to` and `record_route`: walk along routes written down per scene (tools/playtest/data/*.json), and write a new
route from a played walk. Standard library only. A route is a list of named positions Link walked through; the game's
own collision is not read (no game here), so a place with no route is walked in a straight line and says so.

    plan(data, pos, goal)       -> [(name, x, z), ...] or None
    go_to(client, data, goal)   -> reply (arrived, reason, at, plan, stuck_note)
    Recorder, save_route(...)   -> a new data file entry, checked by kit_library.check_data
"""
import heapq
import json
import math
import os
import re

import kit_library
import playtest_client as pc

SNAP_M = 6.0        # a route starts at the named position nearest to Link, within this many metres
LEG_FRAMES = 240    # one waypoint leg
ARRIVE_M = 0.8
SPACING_M = 2.0     # a recorded route keeps a point at least this far from the last kept one
MAX_POINTS = 40
DATA_DIR = os.path.join(kit_library.ROOT, "data")


def slug(scene):
    """InsideTheDekuTree -> inside-the-deku-tree (the data file's name)."""
    return re.sub(r"-+", "-", re.sub(r"(?<![A-Z_-])(?<!^)(?=[A-Z])", "-", scene).replace("_", "-")).lower()


def load(scene, folder=DATA_DIR):
    """The scene's data file as a dict (positions, routes, stuck_spots), or None."""
    path = os.path.join(folder, slug(scene) + ".json")
    try:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError):
        return None
    return d if isinstance(d, dict) and d.get("scene") == scene else None


def _xz(data, name):
    p = data["positions"][name]
    return p["x"], p["z"]


def plan(data, pos, goal):
    """The named positions to walk through, from the nearest one within SNAP_M of `pos` (x, y, z or x, z) to `goal`
    (a position name). Routes are directed: a route is walked start to end only. None when no chain exists."""
    positions = (data or {}).get("positions") or {}
    if goal not in positions:
        return None
    edges = {}
    for r in ((data or {}).get("routes") or {}).values():
        chain = [r["start"]] + list(r["steps"])
        for a, b in zip(chain, chain[1:]):
            edges.setdefault(a, set()).add(b)
    px, pz = pos[0], pos[-1]
    best, prev, queue = {}, {}, []
    for n in positions:
        d = math.hypot(_xz(data, n)[0] - px, _xz(data, n)[1] - pz)
        if d <= SNAP_M:
            best[n] = d
            queue.append((d, n))
    heapq.heapify(queue)
    while queue:
        d, n = heapq.heappop(queue)
        if d > best.get(n, 1e18):
            continue
        if n == goal:
            break
        for m in edges.get(n, ()):
            nd = d + math.hypot(_xz(data, m)[0] - _xz(data, n)[0], _xz(data, m)[1] - _xz(data, n)[1])
            if nd < best.get(m, 1e18):
                best[m], prev[m] = nd, n
                heapq.heappush(queue, (nd, m))
    if goal not in best:
        return None
    chain = [goal]
    while chain[-1] in prev:
        chain.append(prev[chain[-1]])
    return [(n,) + _xz(data, n) for n in reversed(chain)]


def stuck_note(data, x, z):
    """The note and fix of a recorded stuck spot that covers (x, z), or None."""
    for s in (data or {}).get("stuck_spots") or []:
        if math.hypot(s["x"] - x, s["z"] - z) <= s.get("radius", 1):
            return "%s%s" % (s.get("note"), (" Fix: " + s["fix"]) if s.get("fix") else "")
    return None


def go_to(client, data, goal=None, x=None, z=None):
    """Walks to a named position of the scene's data (through its recorded routes), or to x, z. Stops on trouble:
    a scene change, a load, an event, hurt, a stuck walk. Returns arrived, reason, plan (the names), at (the waypoint
    it was walking to), stuck_note, frames, state."""
    st = pc.game_of(client.state())
    pos = st.get("pos")
    if not pos:
        return {"arrived": False, "reason": pc.no_position_reason(st), "plan": [], "frames": 0, "state": st}
    note = None
    if goal is not None:
        if not data or goal not in (data.get("positions") or {}):
            return {"arrived": False, "reason": "no position %r in this scene's data" % goal, "plan": [], "frames": 0,
                    "state": st}
        way = plan(data, pos, goal)
        if way is None:   # no chain: straight to the goal, and say so
            way, note = [(goal,) + _xz(data, goal)], "no recorded route from here: a straight walk"
    else:
        way = [("target", float(x), float(z))]
    life, frames, scene = st.get("life"), 0, st.get("scene")
    out = {"arrived": False, "reason": "", "plan": [w[0] for w in way], "at": None, "frames": 0, "note": note}
    for name, wx, wz in way:
        out["at"] = name
        r = pc.walk_to(client, wx, wz, tolerance=ARRIVE_M, max_frames=LEG_FRAMES)
        frames += r.get("frames", 0)
        st = pc.game_of(r["state"]) if isinstance(r.get("state"), dict) else pc.game_of(client.state())
        out.update(frames=frames, state=st)
        why = r.get("reason", "")
        if why == "scene changed" or st.get("scene") != scene:
            return dict(out, reason="scene changed", scene=st.get("scene"))
        if isinstance(life, (int, float)) and isinstance(st.get("life"), (int, float)) and st["life"] < life:
            return dict(out, reason="hurt (life %s -> %s)" % (life, st["life"]))   # (after a leg, arrived or not)
        if r.get("arrived"):
            continue
        pos = st.get("pos") or [wx, 0, wz]
        out["stuck_note"] = stuck_note(data, pos[0], pos[2])
        return dict(out, reason=why or "stopped")
    return dict(out, arrived=True, reason="arrived")


class Recorder:
    """Samples Link's place during a played walk (start ... save). Keeps a point once it is SPACING_M from the last
    kept one, at most MAX_POINTS."""

    def __init__(self, scene):
        self.scene, self.points = scene, []

    def add(self, pos, scene):
        if scene != self.scene or not pos or len(self.points) >= MAX_POINTS:
            return False
        if self.points and math.hypot(pos[0] - self.points[-1][0], pos[2] - self.points[-1][2]) < SPACING_M:
            return False
        self.points.append((round(pos[0], 2), round(pos[1], 2), round(pos[2], 2)))
        return True


class Sampling:
    """The game's client while a route is recorded: every move, step and input reply that carries Link's place adds it
    to the recorder, so the corners of a walk are kept whichever tool walked it. Anything else goes to the client."""

    def __init__(self, client, recorder):
        self.client, self.recorder = client, recorder

    def __getattr__(self, name):
        return getattr(self.client, name)

    def _take(self, r):
        st = pc.game_of(r) if isinstance(r, dict) else None
        if st and st.get("pos"):
            self.recorder.add(st["pos"], st.get("scene"))
        return r

    def move(self, *a, **k):
        return self._take(self.client.move(*a, **k))

    def step(self, *a, **k):
        return self._take(self.client.step(*a, **k))

    def input(self, *a, **k):
        return self._take(self.client.input(*a, **k))


def save_route(rec, name, note, folder=DATA_DIR, title=None):
    """Writes the recorded points as a route `name` into the scene's data file (made if it is not there). Points are
    named NAME-1, NAME-2 ...; an existing route of that name is refused. The file must pass check_data."""
    if len(rec.points) < 2:
        raise ValueError("a route needs at least 2 points (walk %g m or more between them); got %d"
                         % (SPACING_M, len(rec.points)))
    if not re.match(r"^[a-z0-9][a-z0-9-]*$", str(name)):
        raise ValueError("a route name is lowercase words with hyphens")
    path = os.path.join(folder, slug(rec.scene) + ".json")
    data = load(rec.scene, folder) or {"kit": 1, "title": title or "Routes in %s" % rec.scene, "scene": rec.scene,
                                       "positions": {}}
    data.setdefault("positions", {})
    data.setdefault("routes", {})
    if name in data["routes"]:
        raise ValueError("a route %r is already in %s" % (name, os.path.basename(path)))
    names = []
    for i, (x, y, z) in enumerate(rec.points, 1):
        n = "%s-%d" % (name, i)
        if n in data["positions"]:
            raise ValueError("a position %r is already in the file" % n)
        data["positions"][n] = {"x": x, "y": y, "z": z}
        names.append(n)
    data["routes"][name] = {"start": names[0], "steps": names[1:], "note": (note or "A recorded walk.")[:290]}
    tmp = os.path.join(folder, slug(rec.scene) + ".tmp.json")
    try:
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, indent=1)
            f.write("\n")
        problems = [p for p in kit_library.check_data(tmp) if "the name is lowercase words" not in p]
        if problems:
            raise ValueError("the route would break the data file: " + "; ".join(problems[:3]))
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return {"saved": os.path.basename(path), "route": name, "points": len(names)}
