"""Compares the sync digest lines of two to four games' logs and names every desync (sync-architect D6, step S0).

Each game's module writes, every 2 s of server time, what it holds of every shared object of its scene
(lichii/hook/coop_digest.h):

    coop: digest: t=<server ms> scene=<s> self=<n> obj=<id> name=<actor> owner=<n> epoch=<e> life=<l> ... h=<hex>
    coop: digest: t=<server ms> scene=<s> self=<n> obj=<id> gone
    coop: digest: t=<server ms> scene=<s> self=<n> room objects=<count> h=<hex>

An object line is written when its fields change (and every 10 s); the room line every tick. So a game's view at a
time is, per scene, the last line of each object up to then, less the gone ones.

Rules:
- Two games are compared at each room line of the first, when the second wrote a room line of the SAME scene within
  --near seconds (default 2.5) of it: both games were in that scene then. Apart, nothing is compared.
- Only objects both games hold are compared (one may be out of the other's load range: counted, never a desync).
- Every field is compared but t, scene, self, obj, h and the `local.*` ones (a game's own view: a copy's last owner
  sample). Those are shown with a desync: a copy whose local.owner_life equals the owner's life took the sample and
  its game did not follow it; one that differs never got it.
- A difference is a DESYNC only when it lasts --hold seconds (default 4: two ticks plus the network); a shorter one
  is a hit or a hand-over on its way and is counted as "settled".

    python -I tools/autotest/compare_digest.py A.log B.log [C.log D.log] [--hold 4] [--near 2.5] [--all]

Output: one DESYNC line per object and field set (its first and last time, both games' values), then a summary.
Exit status: 0 when the games were compared and agree, 1 when there is a desync, 2 when nothing could be compared
(no digest lines, or the games never were in one scene at once). Standard library only.
"""
import argparse
import bisect
import itertools
import re
import sys

PREFIX = "coop: digest: "
CONTEXT = ("t", "scene", "self", "obj", "h")
LINE = re.compile(r"coop: digest: (.*)$")


def parse_line(text):
    """One digest line's fields as a dict, or None. 'gone' and 'room' become keys with value '1'."""
    m = LINE.search(text.rstrip("\r\n"))
    if not m:
        return None
    out = {}
    for word in m.group(1).split():
        if "=" in word:
            k, v = word.split("=", 1)
            out[k] = v
        else:
            out[word] = "1"
    if "t" not in out or "scene" not in out:
        return None
    try:
        out["t"] = int(out["t"])
    except ValueError:
        return None
    return out


class Game:
    """One game's digest: its room ticks and, per (scene, object), its lines in time order."""

    def __init__(self, name, lines):
        self.name = name
        self.self = None
        self.ticks = []  # (t, scene)
        self.objects = {}  # (scene, obj) -> [(t, fields or None for gone)]
        for text in lines:
            f = parse_line(text)
            if f is None:
                continue
            self.self = f.get("self", self.self)
            if "room" in f:
                self.ticks.append((f["t"], f["scene"]))
            elif "obj" in f:
                self.objects.setdefault((f["scene"], f["obj"]), []).append((f["t"], None if "gone" in f else f))
        self.ticks.sort()
        self.tick_times = [t for t, _ in self.ticks]
        for v in self.objects.values():
            v.sort(key=lambda x: x[0])
        # A game that left a scene and came back writes all its objects again; one that left holds none of it
        # until then: a scene's objects are valid only while its ticks go on (visits).
        self.visits = {}
        for t, scene in self.ticks:
            spans = self.visits.setdefault(scene, [])
            if spans and t - spans[-1][1] <= 2 * 2500 + 1:
                spans[-1][1] = t
            else:
                spans.append([t, t])

    def label(self):
        return "%s (player %s)" % (self.name, self.self or "?")

    def tick_near(self, t, scene, near_ms):
        i = bisect.bisect_left(self.tick_times, t - near_ms)
        while i < len(self.ticks) and self.ticks[i][0] <= t + near_ms:
            if self.ticks[i][1] == scene:
                return True
            i += 1
        return False

    def visit_start(self, scene, t):
        for a, b in self.visits.get(scene, []):
            if a <= t <= b + 2500:
                return a
        return None

    def view(self, scene, t):
        """Every object this game held in `scene` at time t (its last line at or before t in this visit)."""
        start = self.visit_start(scene, t)
        if start is None:
            return {}
        out = {}
        for (s, obj), lines in self.objects.items():
            if s != scene:
                continue
            i = bisect.bisect_right([x[0] for x in lines], t) - 1
            if i >= 0 and lines[i][0] >= start and lines[i][1] is not None:
                out[obj] = lines[i][1]
        return out


def compared(fields):
    return {k: v for k, v in fields.items() if k not in CONTEXT and not k.startswith("local.")}


def differences(a, b):
    fa, fb = compared(a), compared(b)
    return sorted(k for k in set(fa) | set(fb) if fa.get(k) != fb.get(k))


def compare_pair(ga, gb, hold_ms, near_ms):
    """Returns (points, desyncs, settled, one_side). A desync: dict obj, scene, keys, first, last, a, b."""
    points = 0
    open_ = {}  # (scene, obj, keys) -> run
    runs = []
    one_side = set()
    for t, scene in ga.ticks:
        if not gb.tick_near(t, scene, near_ms):
            for key in list(open_):  # apart: every running difference ends here
                runs.append(open_.pop(key))
            continue
        points += 1
        va, vb = ga.view(scene, t), gb.view(scene, t)
        seen = set()
        for obj in sorted(set(va) | set(vb)):
            if obj not in va or obj not in vb:
                one_side.add((scene, obj))
                continue
            keys = tuple(differences(va[obj], vb[obj]))
            if not keys:
                continue
            key = (scene, obj, keys)
            seen.add(key)
            run = open_.setdefault(key, {"scene": scene, "obj": obj, "keys": keys, "first": t})
            run["last"] = t
            run["a"], run["b"] = va[obj], vb[obj]
        for key in [k for k in open_ if k not in seen]:  # (another scene's too: we are in this one)
            runs.append(open_.pop(key))
    runs.extend(open_.values())
    desyncs = [r for r in runs if r["last"] - r["first"] >= hold_ms]
    settled = len(runs) - len(desyncs)
    return points, desyncs, settled, one_side


def show(fields, keys):
    parts = ["%s=%s" % (k, fields.get(k, "(none)")) for k in keys]
    parts += ["%s=%s" % (k, v) for k, v in sorted(fields.items()) if k.startswith("local.")]
    return " ".join(parts)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("logs", nargs="+", help="two to four game logs")
    p.add_argument("--hold", type=float, default=4.0, help="seconds a difference must last to be a desync")
    p.add_argument("--near", type=float, default=2.5, help="seconds apart two games' room ticks may be")
    p.add_argument("--all", action="store_true", help="also list each object only one game held")
    a = p.parse_args(argv)
    if not 2 <= len(a.logs) <= 4:
        p.error("give two to four logs")
    games = []
    for path in a.logs:
        with open(path, encoding="utf-8", errors="replace") as f:
            games.append(Game(path, f))
    for g in games:
        print("%s: %d ticks, %d objects" % (g.label(), len(g.ticks), len(g.objects)))
    total_points = 0
    total_desyncs = 0
    for ga, gb in itertools.combinations(games, 2):
        points, desyncs, settled, one_side = compare_pair(ga, gb, int(a.hold * 1000), int(a.near * 1000))
        total_points += points
        total_desyncs += len(desyncs)
        print("%s vs %s: %d ticks compared, %d desync(s), %d settled difference(s), %d object(s) held by one only"
              % (ga.label(), gb.label(), points, len(desyncs), settled, len(one_side)))
        for d in sorted(desyncs, key=lambda r: r["first"]):
            name = d["a"].get("name") or d["b"].get("name") or "?"
            print("DESYNC %s %s %s: %s from t=%d to t=%d (%.1f s) | A: %s | B: %s"
                  % (d["scene"], d["obj"], name, ",".join(d["keys"]), d["first"], d["last"],
                     (d["last"] - d["first"]) / 1000.0, show(d["a"], d["keys"]), show(d["b"], d["keys"])))
        if a.all:
            for scene, obj in sorted(one_side):
                print("  one side only: %s %s" % (scene, obj))
    if total_points == 0:
        print("NOTHING COMPARED: no digest lines, or the games were never in one scene at once")
        return 2
    print("RESULT: %s" % ("DESYNC" if total_desyncs else "AGREE"))
    return 1 if total_desyncs else 0


if __name__ == "__main__":
    sys.exit(main())
