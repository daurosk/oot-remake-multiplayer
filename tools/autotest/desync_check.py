"""Compares the two games' checkpoints of a live play-test session (tools/autotest/live.py checkpoint TAG).

Each checkpoint holds both games' shared dungeon and story flags (the module's `dump TAG`) and their bridge status.
A flag the two games disagree on is a desync: co-op shares every one of them. The checkpoints are read in the order
they were taken; the first one with a difference is named, with every flag that differs in it. Standard library only.

    python -I tools/autotest/desync_check.py SESSION_DIR [--all]

With --clients it also compares the games' actors (the `world` list a checkpoint keeps in its state), by what
docs/sync-coverage.md says about each actor's name (sync_rows.py); --flags-only leaves that out. Rules:

- Only when both games are in the same scene and their Links are within 40 m; otherwise "not compared (apart)".
- Actors are matched one to one by name and nearest place (a game's addresses differ). Compared: alive, owner, life; the
  place (0.2 m) only for placed static actors (chests, doors, switches) and bosses; a moving enemy's place never. An
  actor in one game and not the other is a count difference, reported by name.
- The page's row decides: Live `live` = a difference is a desync; `?` = "live not proven: check it twice, then report
  what you saw"; `reload` = "shared, shown on reload"; `-` or Path `none` = not shared, left out; no row = "no row in
  sync-coverage: not compared". Only `live` rows can make the exit status 1.
- A world list the snapshot cut at 200 actors may lack actors of kind `other` (crates, rocks) beyond the farthest one it
  kept. When either list has 200 actors, an actor is compared only when it lies within the first game's kept distance of
  the first game's Link AND within the second game's kept distance of the second game's Link (a list that was not cut
  has no limit; an actor with no place is left out when a list is cut), and the line says so.
- A DESYNC line names the actor (id and place in both games), the first tag and frames it was seen at, the confirming
  tag and frames, and the page's row (line, Path, Live). The key that a second sighting must match includes the ids.
- A `live` difference counts only when it is in two checkpoints in a row with both games 60 or more frames further on in
  the second; the first time it is "recheck after step 60" and the exit status is 3, never 0.

With --clients it reads a bridge run's log folder instead (mcp_playtest.py --player, one log per game, p1 to p4):
the checkpoints are matched by tag across every game (the second use of a tag goes with the other games' second),
each game's frame count at the tag is printed, and every game's flags are compared.

    python -I tools/autotest/desync_check.py LOG_DIR --clients [--all]

Exit status: 0 when every checkpoint was compared and agrees, 1 when one does not, 2 when there is nothing to compare
(no checkpoint, or none where both games wrote their flags), 3 when some agree but another could not be compared (a game
wrote no flags): never "agree" for a checkpoint that was not compared.
"""
import argparse
import glob
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sync_rows  # noqa: E402

TOL = {"static_m": 0.2, "area_m": 40.0, "life": 0, "settle_frames": 60, "cut_total": 200}     # (every tolerance is here)
STATIC_KINDS = ("chest", "door", "switch")           # placed actors that do not move: their place is compared
BOSS_NAME = "Enemy_*Boss*"                            # a boss's place is shared (BOSS_ROOM)


def compare(cp):
    """(differences, note): the flags whose values differ between A and B ({name: (a, b)}), and why not compared."""
    a, b = cp.get("A") or {}, cp.get("B") or {}
    fa, fb = a.get("flags"), b.get("flags")
    if fa is None or fb is None:
        return {}, "a game wrote no flags (%s)" % (a.get("error") or b.get("error") or "no dump")
    diff = {}
    for name in sorted(set(fa) | set(fb)):
        va, vb = fa.get(name), fb.get(name)
        if va is not None and vb is not None and va != vb:
            diff[name] = (va, vb)
    return diff, None


def check(session, show_all=False, out=print):
    paths = sorted(glob.glob(os.path.join(session, "checkpoints", "*.json")), key=os.path.getmtime)
    if not paths:
        out("no checkpoints in %s" % os.path.join(session, "checkpoints"))
        return 2
    bad = compared = skipped = 0
    for path in paths:
        with open(path, encoding="utf-8") as f:
            cp = json.load(f)
        tag = cp.get("tag") or os.path.splitext(os.path.basename(path))[0]
        maps = "A %s, B %s" % ((cp.get("A") or {}).get("status", {}).get("map"),
                               (cp.get("B") or {}).get("status", {}).get("map"))
        diff, note = compare(cp)
        if note:
            out("%s: not compared: %s" % (tag, note))
            skipped += 1
            continue
        compared += 1
        shots = [s for s in ((cp.get(x) or {}).get("shot") for x in ("A", "B")) if s]
        if shots:
            maps += "; shots " + ", ".join(shots)
        if not diff:
            out("%s: agree (%s)" % (tag, maps))
            continue
        bad += 1
        out("%s: %d flag(s) differ (%s)%s" % (tag, len(diff), maps, "" if bad > 1 else "  <- first desync"))
        for name, (va, vb) in diff.items():
            out("    %s: A %s, B %s" % (name, va, vb))
        if not show_all:
            break
    if bad:
        return 1
    if not compared:
        out("nothing compared: no checkpoint has both games' flags")
        return 2
    if skipped:
        out("incomplete: %d checkpoint(s) not compared" % skipped)
        return 3
    return 0


RESULTS = ["agree", "desync", "nothing to compare", "incomplete"]


def read_clients(log_dir):
    """{player: [checkpoint, ...]} from each game's checkpoint log (<player>.checkpoints.jsonl, one JSON per line,
    written by playtest_client.checkpoint), each game's in frame order. A line that is not JSON is skipped."""
    out = {}
    for path in sorted(glob.glob(os.path.join(log_dir, "*.checkpoints.jsonl"))):
        player = os.path.basename(path).split(".")[0]
        rows = []
        with open(path, encoding="utf-8") as f:
            for line in f:
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    continue
        rows.sort(key=lambda r: (r.get("frame") is None, r.get("frame") or 0))
        out[player] = rows
    return out


def align(clients):
    """[(key, {player: checkpoint})]: the n-th checkpoint of a tag in each game goes with the n-th of that tag in
    every other game (a tag taken twice is two keys, TAG and TAG#2), in the order of the first game's frames."""
    keyed, order = {}, []
    for player in sorted(clients):
        seen = {}
        for cp in clients[player]:
            tag = cp.get("tag")
            seen[tag] = seen.get(tag, 0) + 1
            key = tag if seen[tag] == 1 else "%s#%d" % (tag, seen[tag])
            if key not in keyed:
                keyed[key] = {}
                order.append(key)
            keyed[key][player] = cp
    return [(k, keyed[k]) for k in order]


def compare_many(cps):
    """(differences, note) for one tag across games: {flag: {player: value}} where the games disagree."""
    missing = [p for p, cp in sorted(cps.items()) if cp.get("flags") is None]
    if missing:
        return {}, "%s wrote no flags (%s)" % (", ".join(missing), cps[missing[0]].get("error") or "no dump")
    diff = {}
    names = set()
    for cp in cps.values():
        names |= set(cp["flags"])
    for name in sorted(names):
        vals = {p: cp["flags"].get(name) for p, cp in sorted(cps.items())}
        known = {v for v in vals.values() if v is not None}
        if len(known) > 1:
            diff[name] = vals
    return diff, None


def _dist(p, q):
    return math.dist(p, q) if p and q and len(p) == len(q) else math.inf


def world_usable(w):
    """Why a checkpoint's world list cannot be compared, or None."""
    if not isinstance(w, dict):
        return "no world list in the checkpoint"
    if w.get("stale"):
        return "its world list is stale"
    why = str(w.get("why") or "")
    if why.startswith("no co-op session"):
        return why
    return None


def pair_up(xs, ys):
    """([(x, y)], [x left], [y left]): one to one, the nearest places first (an actor with no place goes last)."""
    cand = sorted((_dist(x.get("pos"), y.get("pos")), i, j) for i, x in enumerate(xs) for j, y in enumerate(ys))
    used_x, used_y, pairs = set(), set(), []
    for _, i, j in cand:
        if i not in used_x and j not in used_y:
            used_x.add(i)
            used_y.add(j)
            pairs.append((xs[i], ys[j]))
    return (pairs, [x for i, x in enumerate(xs) if i not in used_x], [y for j, y in enumerate(ys) if j not in used_y])


def _where(pos):
    return "(%s)" % ", ".join("%.1f" % v for v in pos) if isinstance(pos, (list, tuple)) and pos else "no place"


def _who(side, act):
    return "%s id %s at %s" % (side, act.get("id"), _where(act.get("pos")))


def cut_radius(w, tol=TOL):
    """None when a game's world list is whole; else how far from its own Link the list is complete (m). The snapshot
    keeps 200 actors, the known kinds first and then the nearest to its own Link (CoopWorldSnapshot::Select), so a list
    with 200 actors may lack actors of kind `other` (crates, rocks) beyond the farthest one it kept."""
    acts = w.get("actors") or []
    if max(w.get("total") or 0, len(acts)) < tol["cut_total"]:
        return None
    def dists(kinds):
        return [a["dist"] for a in acts if isinstance(a.get("dist"), (int, float))
                and (kinds is None or a.get("kind") in kinds)]
    return max(dists(("other",)) or dists(None) or [0.0])


def _inside_cuts(pos, link_a, ra, link_b, rb):
    """True when an actor at `pos` (world metres) is inside A's kept radius round A's Link and inside B's kept radius
    round B's Link. A radius of None (a list that was not cut) never limits. No place, no answer: False."""
    if not pos:
        return False
    return all(r is None or _dist(pos, link) <= r for link, r in ((link_a, ra), (link_b, rb)))


def compare_world(cp_a, cp_b, rows, tol=TOL):
    """Compare two checkpoints' actors by what sync-coverage says about each name. Replies {note, diffs, compared,
    skipped, no_row, no_place, cut, cut_left_out}: `note` says why nothing was compared (else None); each diff is
    {name, field, a, b, label, key, text} (`text` names both actors by id and place, and the page's row by line, Path and
    Live) with `label` the page's (live, not proven, reload); `skipped` counts actors of not shared rows; `no_row` is
    {name: count} of the actors no row speaks for; `no_place` is {name: count} of placed actors whose place is missing.
    When a world list was cut at 200 actors (`cut` = {"a": radius or None, "b": radius or None}, in metres from that
    game's own Link), an actor is compared only when it is within A's radius of A's Link AND within B's radius of B's
    Link (a whole list has no radius), and never without a place: a count or a missing actor beyond a radius may be the
    cut, not a desync. Never touches a game."""
    out = {"note": None, "diffs": [], "compared": 0, "skipped": 0, "no_row": {}, "no_place": {}, "cut": None,
           "cut_left_out": 0}
    sa, sb = cp_a.get("state") or {}, cp_b.get("state") or {}
    for who, w in (("A", sa.get("world")), ("B", sb.get("world"))):
        why = world_usable(w)
        if why:
            out["note"] = "not compared (%s: %s)" % (who, why)
            return out
    if sa.get("scene") is None or sa.get("scene") != sb.get("scene"):
        out["note"] = "not compared (scenes: A %s, B %s)" % (sa.get("scene"), sb.get("scene"))
        return out
    gap = _dist(sa.get("pos"), sb.get("pos"))
    if gap == math.inf:
        out["note"] = "not compared (a Link's place is unknown)"
        return out
    if gap > tol["area_m"]:
        out["note"] = "not compared (apart: the Links are %.0f m apart, the limit is %.0f m)" % (gap, tol["area_m"])
        return out
    ra, rb = cut_radius(sa["world"], tol), cut_radius(sb["world"], tol)
    cut = ra is not None or rb is not None
    out["cut"] = {"a": ra, "b": rb} if cut else None
    names = {}
    for side, w in (("a", sa["world"]), ("b", sb["world"])):
        for act in w.get("actors") or []:
            if act.get("kind") == "player" or not act.get("name"):
                continue
            if cut and not _inside_cuts(act.get("pos"), sa.get("pos"), ra, sb.get("pos"), rb):
                out["cut_left_out"] += 1
                continue
            names.setdefault(act["name"], {"a": [], "b": []})[side].append(act)
    for name in sorted(names):
        xs, ys = names[name]["a"], names[name]["b"]
        m = sync_rows.match(rows, name)
        if m is None:
            out["no_row"][name] = len(xs) + len(ys)
            continue
        if m["label"] == "not shared":
            out["skipped"] += len(xs) + len(ys)
            continue
        pairs, left_a, left_b = pair_up(xs, ys)
        out["compared"] += len(pairs)
        row = " / ".join("docs/sync-coverage.md:%d \"%s\", Path: %s, Live: %s"
                         % (r["line"], r["name"].split(" (")[0], r["path"], r["live"]) for r in m["rows"])

        def add(field, a, b, what, who, key):
            out["diffs"].append({"name": name, "field": field, "a": a, "b": b, "label": m["label"], "key": key,
                                 "text": "%s: %s (%s)  [row %s; %s]" % (name, what, who, row, m["text"])})
        for x, y in pairs:
            who = "%s, %s" % (_who("A", x), _who("B", y))
            ids = (x.get("id"), y.get("id"))
            for f in ("alive", "owner"):
                if x.get(f) is not None and y.get(f) is not None and x[f] != y[f]:
                    add(f, x[f], y[f], "%s differs (A %s, B %s)" % (f, x[f], y[f]), who, (name, f) + ids)
            lx, ly = x.get("life"), y.get("life")
            if isinstance(lx, (int, float)) and isinstance(ly, (int, float)) and abs(lx - ly) > tol["life"]:
                add("life", lx, ly, "life differs (A %s, B %s)" % (lx, ly), who, (name, "life") + ids)
            if x.get("kind") in STATIC_KINDS or sync_rows.fnmatch.fnmatchcase(name, BOSS_NAME):
                d = _dist(x.get("pos"), y.get("pos"))
                if d == math.inf:
                    out["no_place"][name] = out["no_place"].get(name, 0) + 1
                elif d > tol["static_m"]:
                    add("pos", x.get("pos"), y.get("pos"), "place differs by %.2f m (the limit is %.2f m)"
                        % (d, tol["static_m"]), who, (name, "pos") + ids)
        if left_a or left_b:
            na, nb = len(xs), len(ys)
            who = "; ".join(["no match in B: " + ", ".join(_who("A", x) for x in left_a[:4])] * bool(left_a)
                            + ["no match in A: " + ", ".join(_who("B", y) for y in left_b[:4])] * bool(left_b))
            add("count", na, nb, "%d in A, %d in B" % (na, nb), who,
                (name, "count", tuple(sorted(str(x.get("id")) for x in left_a)),
                 tuple(sorted(str(y.get("id")) for y in left_b))))
    return out


def _radius_text(r):
    return "whole list" if r is None else "%.0f m" % r


def _frames(fr):
    return ", ".join("%s frame %s" % (p, fr[p]) for p in sorted(fr))


def world_many(cps, rows, prev, tag="", tol=TOL):
    """Compare the first game's world with each other game's (games p1.. in order). `prev` is {key: {frames, tag,
    first}} of the `live` differences seen at the last compared tag (key: games pair + the actor's name, field and ids):
    updated here. `tag` is this checkpoint's tag. Replies None (no checkpoint has a world: an older log), or {lines,
    confirmed, pending}."""
    if not any(isinstance((cp.get("state") or {}).get("world"), dict) or "world" in (cp.get("state") or {})
               for cp in cps.values()):
        return None
    if rows is None:
        return {"lines": ["    world: not compared (docs/sync-coverage.md was not found)"], "confirmed": 0, "pending": 0}
    games = sorted(cps)
    lines, confirmed, pending, now, compared_any = [], 0, 0, {}, False
    for other in games[1:]:
        res = compare_world(cps[games[0]], cps[other], rows, tol)
        pair = "%s~%s" % (games[0], other)
        if res["note"]:
            lines.append("    world %s: %s" % (pair, res["note"]))
            continue
        compared_any = True
        counts = {"desync": 0, "recheck": 0, "finding": 0}
        body = []
        for d in res["diffs"]:
            if d["label"] != "live":
                counts["finding"] += 1
                body.append("    world %s: %s" % (pair, d["text"]))
                continue
            k = (pair,) + d["key"]
            fr = {p: cps[p].get("frame") for p in (games[0], other)}
            was = prev.get(k)
            now[k] = {"frames": fr, "tag": tag, "first": was["first"] if was else {"tag": tag, "frames": fr}}
            if was and all(isinstance(fr[p], (int, float)) and isinstance(was["frames"].get(p), (int, float))
                           and fr[p] - was["frames"][p] >= tol["settle_frames"] for p in fr):
                counts["desync"] += 1
                body.append("    world %s: DESYNC %s; first seen at %s (%s), confirmed at %s (%s)"
                            % (pair, d["text"], was["first"]["tag"], _frames(was["first"]["frames"]), tag, _frames(fr)))
            else:
                counts["recheck"] += 1
                body.append("    world %s: recheck after both games step %d frames: %s"
                            % (pair, tol["settle_frames"], d["text"]))
        confirmed += counts["desync"]
        pending += counts["recheck"]
        bits = ["%d desync" % counts["desync"] if counts["desync"] else "", "%d to recheck" % counts["recheck"]
                if counts["recheck"] else "", "%d finding(s) not a desync (not proven or reload rows)"
                % counts["finding"] if counts["finding"] else ""]
        bits = [b for b in bits if b]

        def named(d, what):
            return "%d %s (%s)" % (len(d), what, ", ".join(sorted(d)[:6]) + (", ..." if len(d) > 6 else ""))
        extra = ["%d actor(s) of not shared rows left out" % res["skipped"] if res["skipped"] else "",
                 named(res["no_row"], "name(s) with no row in sync-coverage: not compared") if res["no_row"] else "",
                 named(res["no_place"], "name(s) with no place in one game: place not compared")
                 if res["no_place"] else "",
                 "a list was cut at %d actors: only actors inside the kept distance of each game's own Link compared "
                 "(A %s, B %s; %d left out)"
                 % (tol["cut_total"], _radius_text(res["cut"]["a"]), _radius_text(res["cut"]["b"]),
                    res["cut_left_out"]) if res["cut"] else ""]
        extra = [e for e in extra if e]
        lines.append("    world %s: %s (%d actor(s) compared%s)" % (
            pair, ", ".join(bits) if bits else "agree", res["compared"], "; " + "; ".join(extra) if extra else ""))
        lines.extend(body)
    if compared_any:
        prev.clear()
        prev.update(now)
    return {"lines": lines, "confirmed": confirmed, "pending": pending}


def check_clients(log_dir, show_all=False, out=print, flags_only=False, rows=None):
    """Like check, for the per-game logs of a bridge run (mcp_playtest.py --player): the tags are matched across
    every game that wrote a log, and each game's frame at the tag is printed. Same exit codes."""
    clients = read_clients(log_dir)
    if rows is None and not flags_only:
        try:
            rows = sync_rows.load()[0]
        except OSError:
            rows = None
    prev, pending = {}, 0
    if len(clients) < 2:
        out("nothing to compare: %d game log(s) in %s (need two or more *.checkpoints.jsonl)" % (len(clients), log_dir))
        return 2
    bad = compared = skipped = 0
    for key, cps in align(clients):
        frames = ", ".join("%s frame %s %s" % (p, cp.get("frame"), cp.get("scene") or "") for p, cp in sorted(cps.items()))
        absent = sorted(set(clients) - set(cps))
        if absent:
            out("%s: not compared: no checkpoint in %s (%s)" % (key, ", ".join(absent), frames))
            skipped += 1
            continue
        diff, note = compare_many(cps)
        if note:
            out("%s: not compared: %s" % (key, note))
            skipped += 1
            continue
        compared += 1
        w = None if flags_only else world_many(cps, rows, prev, key)
        confirmed = w["confirmed"] if w else 0
        pending = w["pending"] if w else pending     # (only the last compared tag can still wait for a second look)
        if not diff and not confirmed:
            out("%s: %s (%s)" % (key, "agree" if not (w and w["pending"]) else "flags agree", frames))
            for ln in (w["lines"] if w else []):
                out(ln)
            continue
        bad += 1
        what = (["%d flag(s) differ" % len(diff)] if diff else []) + (
            ["%d world difference(s) in two checkpoints in a row" % confirmed] if confirmed else [])
        out("%s: %s (%s)%s" % (key, " and ".join(what), frames, "" if bad > 1 else "  <- first desync"))
        for name, vals in diff.items():
            out("    %s: %s" % (name, ", ".join("%s %s" % (p, v) for p, v in vals.items())))
        for ln in (w["lines"] if w else []):
            out(ln)
        if not show_all:
            break
    if bad:
        return 1
    if not compared:
        out("nothing compared: no tag has every game's flags")
        return 2
    if skipped or pending:
        out("incomplete: %s%s" % ("%d tag(s) not compared" % skipped if skipped else "",
                                  ("%s%d world difference(s) wait for a second checkpoint after both games step %d "
                                   "frames" % ("; " if skipped else "", pending, TOL["settle_frames"])) if pending else ""))
        return 3
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("session", help="a live.py session folder, or with --clients the bridge run's log folder")
    p.add_argument("--all", action="store_true", help="every checkpoint with a difference, not only the first")
    p.add_argument("--clients", action="store_true",
                   help="per-game logs (<player>.checkpoints.jsonl from mcp_playtest.py --player), matched by tag")
    p.add_argument("--flags-only", action="store_true", help="with --clients: compare the flags only, not the actors")
    a = p.parse_args(argv)
    return check_clients(a.session, a.all, flags_only=a.flags_only) if a.clients else check(a.session, a.all)


if __name__ == "__main__":
    sys.exit(main())
