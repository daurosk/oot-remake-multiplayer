"""Parallel Hookshot autotest groups: each group on its own game pair, ports, control pipes and run folders, then
one PASS/FAIL table for all of them.

    python -I tools/autotest/groups.py plan    <groups.txt> [--kit-root DIR] [--server EXE]      (what would run)
    python -I tools/autotest/groups.py prepare <groups.txt> [--kit-root DIR] [--dry-run]         (pair folders)
    python -I tools/autotest/groups.py run     <groups.txt> [--kit-root DIR] [--server EXE] [--dry-run]
    python -I tools/autotest/groups.py merge   <run root or run folders...> [--out table.md]

A group file:

    # comment
    [g1]
    walk
    teleport-same  switches=LICHII_COOP_CHEATS=1  expect="A:coop: connected"  label="#178 door"
    [g2]
    chest          expect="B:got item"  timeout=900

Each line under a [group] is one scenario (tools/autotest/scenarios/<name>.A.txt), played in order. Optional words:
switches= (run.ps1 -Switches), expect= (a regex that must be in the run's hookshot_log-A.txt, -B.txt with an "A:" or
"B:" in front, either log without; repeatable), timeout= (s), label= (the table's text for the run),
see= (a screenshot check of tools/autotest/proofs.txt, graded by proof.py from the run's shots; repeatable) and
logonly= (why this run checks nothing visible, e.g. logonly="connect line only": then log expects alone may pass).

Group N (the Nth [group] in the file) uses the game folders <kit root>\\hookshot-A<N> and -B<N>, server port
27560 + 10*(N-1) (run.ps1's relays use +1 and +2), control pipes coop-auto-g<N>-A/-B, and run folders under
<kit root>\\shots-hookshot\\groups-<time>\\<group>\\. `prepare` fills those folders: the cached game
(tools/hookshot/hs.py), plus the base pair's (hookshot-A / -B) autotest-save, mods\\ and coop-options.ini.

With --lane N (tools/autotest/lanes.py) every group runs in that test lane instead, one after another on the lane's
pair, port and pipes (run.ps1 -Lane N, which holds the lane's lock for each run), run folders under
<kit root>\\ls\\shots-laneN\\groups-<time>\\. The other lane can run another group file at the same time.
With --lane N --kit <kit A>,<kit B> every game is started by that kit's own start, as a player's (run.ps1 -Kit;
tools/autotest/kit_pair.py setup makes the two kits from a release zip and points them at the lane's pair).

Result of one run (merge): FAILED.txt in the run folder -> FAIL; a FAIL line in counts.txt -> FAIL; run.ps1's exit code
not 0 -> FAIL; one expect= missing -> FAIL; a see= check whose shot shows the wrong result -> FAIL; every expect=
found and every see= OK -> PASS; no expect= -> PLAYED (it ran to the end; look at the shots).
Owner rule (2026-11-18): NO PASS FROM LOG LINES ALONE. A run whose expects (or a steps file's counts) all pass but that
has no see= check, or a see= check that is unclear or has no shot/references yet -> UNPROVEN, which is not a pass
(merge's verdict is UNPROVEN, exit 1). Only a line marked logonly="reason" (nothing visible to prove: a connect line,
a sent note's own log) passes on log lines alone. `merge --regrade` takes see=/logonly= from the group files of today
for run folders made before (their run.json has none). Nothing here starts a game unless `run` is called without
--dry-run.
"""
import argparse
import datetime
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BASE_PORT = 27560
PORT_STEP = 10
COPY_FROM_BASE = ("autotest-save", "mods", "coop-options.ini")


class Fail(Exception):
    pass


def parse_groups(text):
    """[(group name, [run dict])] in file order. A run: scenario, switches, expects [(side or '', regex)], timeout,
    label. A leading UTF-8 BOM is dropped (Windows PowerShell 5.1's Out-File -Encoding utf8 writes one: the first
    [group] was then not seen)."""
    text = text.lstrip("﻿")
    groups, cur, seen = [], None, set()
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^\[([A-Za-z0-9_-]+)\]$", line)
        if m:
            if m.group(1) in seen:
                raise Fail("line %d: group [%s] twice" % (n, m.group(1)))
            seen.add(m.group(1))
            cur = (m.group(1), [])
            groups.append(cur)
            continue
        if cur is None:
            raise Fail("line %d: a scenario before the first [group]" % n)
        try:
            words = shlex.split(line, posix=True)
        except ValueError as e:
            raise Fail("line %d: %s" % (n, e))
        run = {"scenario": words[0], "switches": "", "expects": [], "timeout": None, "label": "", "password": "",
               "see": [], "logonly": ""}
        if not re.match(r"^[A-Za-z0-9_.-]+$", run["scenario"]):
            raise Fail("line %d: bad scenario name %r" % (n, run["scenario"]))
        for w in words[1:]:
            k, eq, v = w.partition("=")
            if not eq:
                raise Fail("line %d: %r is not key=value" % (n, w))
            if k == "switches":
                run["switches"] = v
            elif k == "expect":
                side = v[0] if v[:2] in ("A:", "B:") else ""
                rx = v[2:] if side else v
                try:
                    re.compile(rx)
                except re.error as e:
                    raise Fail("line %d: expect %r: %s" % (n, rx, e))
                run["expects"].append((side, rx))
            elif k == "timeout":
                if not v.isdigit():
                    raise Fail("line %d: timeout=%s" % (n, v))
                run["timeout"] = int(v)
            elif k == "label":
                run["label"] = v
            elif k == "see":
                if not re.match(r"^[A-Za-z0-9_.-]+$", v):
                    raise Fail("line %d: see=%r is not a check name" % (n, v))
                run["see"].append(v)
            elif k == "logonly":
                if not v.strip():
                    raise Fail("line %d: logonly= needs the reason" % n)
                run["logonly"] = v
            elif k == "password":   # run.ps1 -ServerPassword: a steps file's password/try lines (join-password)
                run["password"] = v
            else:
                raise Fail("line %d: unknown %s=" % (n, k))
        cur[1].append(run)
    if not groups:
        raise Fail("no [group] in the file")
    for name, runs in groups:
        if not runs:
            raise Fail("group [%s] has no scenario" % name)
    return groups


def group_setup(index, name, kit_root, run_root):
    """Folders, port and pipe of the group at 1-based index."""
    return {
        "name": name,
        "index": index,
        "a": os.path.join(kit_root, "hookshot-A%d" % index),
        "b": os.path.join(kit_root, "hookshot-B%d" % index),
        "port": BASE_PORT + PORT_STEP * (index - 1),
        "pipe": "coop-auto-g%d" % index,
        "shots": os.path.join(run_root, name),
    }


def check_distinct(setups):
    """Two groups must never share a folder, port (+0..+2) or pipe."""
    seen = {}
    for s in setups:
        for k in [("dir", os.path.normcase(s["a"])), ("dir", os.path.normcase(s["b"])), ("pipe", s["pipe"]),
                  ("dir", os.path.normcase(s["shots"]))] + [("port", s["port"] + i) for i in range(3)]:
            if k in seen and seen[k] != s["name"]:
                raise Fail("groups %s and %s share %s %s" % (seen[k], s["name"], k[0], k[1]))
            seen[k] = s["name"]


def run_command(setup, run, server, run_ps1=None, kits=None):
    """The run.ps1 command line for one scenario of a group (kits: the A and B kit folders, run.ps1 -Kit)."""
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
           run_ps1 or os.path.join(HERE, "run.ps1"), "-Runtime", "hookshot", "-Scenario", run["scenario"],
           "-HookshotA", setup["a"], "-HookshotB", setup["b"], "-Port", str(setup["port"]), "-Pipe", setup["pipe"],
           "-Shots", setup["shots"]]
    if server:
        cmd += ["-Server", server]
    if run["switches"]:
        cmd += ["-Switches", run["switches"]]
    if run["timeout"]:
        cmd += ["-Timeout", str(run["timeout"])]
    if run.get("password"):
        cmd += ["-ServerPassword", run["password"]]
    if kits:
        cmd += ["-Kit", ",".join(kits)]
    return cmd


def quote(cmd):
    return " ".join('"%s"' % c if (" " in c or not c) else c for c in cmd)


def lane_setup(lane, name, kit_root, run_root):
    """A group run in test lane `lane`: the lane's pair, port and pipe (lanes.py)."""
    sys.path.insert(0, HERE)
    import lanes
    L = lanes.lane(lane, kit_root)
    return {"name": name, "index": lane, "lane": lane, "a": L["a"], "b": L["b"], "port": L["port"], "pipe": L["pipe"],
            "shots": os.path.join(run_root, name)}


def plan(groups, kit_root, run_root, server, lane=0, kits=None, lag=0, jitter=0, renderer=(0, 0, 0)):
    """lag / jitter: run.ps1 -Lag / -Jitter for every run (on Longshot the server holds the packets, TEST ONLY).
    renderer: run.ps1 -RendererWait / -RendererStall / -RendererLimit for every run (0: the harness default)."""
    if kits and not lane:
        raise Fail("--kit runs in a test lane only (--lane N): the kits start that lane's pair (kit_pair.py setup)")
    if kits and len(kits) != 2:
        raise Fail("--kit: two kit folders, A,B")
    if lane:   # every group on the lane's one pair: run_groups plays them one after another
        setups = [lane_setup(lane, name, kit_root, run_root) for name, _ in groups]
    else:
        setups = [group_setup(i, name, kit_root, run_root) for i, (name, _) in enumerate(groups, 1)]
        check_distinct(setups)
    planned = []
    for s, (_, runs) in zip(setups, groups):
        cmds = [run_command(s, r, server, kits=kits) for r in runs]
        if lane:
            cmds = [c + ["-Lane", str(lane)] for c in cmds]
        if lag or jitter:
            cmds = [c + ["-Lag", str(int(lag)), "-Jitter", str(int(jitter))] for c in cmds]
        for flag, v in zip(("-RendererWait", "-RendererStall", "-RendererLimit"), renderer):
            if v:
                cmds = [c + [flag, str(int(v))] for c in cmds]
        planned.append((s, runs, cmds))
    return planned


def print_plan(planned, say=print):
    for s, runs, cmds in planned:
        say("[%s] pair %s + %s, port %d, pipe %s-A/-B, runs in %s" % (s["name"], s["a"], s["b"], s["port"],
                                                                       s["pipe"], s["shots"]))
        for r, c in zip(runs, cmds):
            say("  %s%s" % (r["scenario"], ("  expect " + "; ".join((x + ":" if x else "") + rx for x, rx in
                                                                  r["expects"])) if r["expects"] else ""))
            say("    " + quote(c))


# --- prepare -------------------------------------------------------------------------------------------------------

def prepare(planned, kit_root, dry=False, say=print, restore=None):
    """Each group's pair folders: the cached game (restore(folder, dry) -> bool) and the base pair's per-test files."""
    ok = True
    for s, _, _ in planned:
        for side, folder in (("A", s["a"]), ("B", s["b"])):
            base = os.path.join(kit_root, "hookshot-" + side)
            if restore is not None and not restore(folder, dry):
                say("%s: no cached game (tools/hookshot/hs.py build or cache store first)" % folder)
                ok = False
            for name in COPY_FROM_BASE:
                src, dst = os.path.join(base, name), os.path.join(folder, name)
                if not os.path.exists(src) or os.path.exists(dst):
                    continue
                say("copy %s -> %s" % (src, dst))
                if not dry:
                    os.makedirs(folder, exist_ok=True)
                    (shutil.copytree if os.path.isdir(src) else shutil.copy2)(src, dst)
    return ok


def hs_restore(folder, dry):
    sys.path.insert(0, os.path.join(REPO, "tools", "hookshot"))
    import hs
    if not dry:
        os.makedirs(folder, exist_ok=True)
    return hs.cache_restore(folder, dry=dry)


# --- run -------------------------------------------------------------------------------------------------------------

def run_groups(planned, dry=False, say=print, runner=None):
    """Each group in its own thread, its scenarios one after another. Writes run.json into each run folder (run.ps1
    prints the folder as its last line). Returns [(group, run, folder or None, exit code)]."""
    runner = runner or (lambda cmd: subprocess.run(cmd, capture_output=True, text=True, errors="replace"))
    results, lock = [], threading.Lock()

    def one_run(s, r, cmd):
        """(folder or None, exit code). A run that leaves no folder, or fails here, leaves <scenario>-no-folder.txt,
        which merge counts as a FAIL row."""
        code = -1
        try:
            p = runner(cmd)
            code = p.returncode
            lines = [line.strip() for line in (p.stdout or "").splitlines() if line.strip()]
            folder = next((line for line in reversed(lines) if os.path.isdir(line)), None)
            if folder:
                with open(os.path.join(folder, "run.json"), "w", encoding="utf-8") as f:
                    json.dump({"group": s["name"], "scenario": r["scenario"], "label": r["label"],
                               "expects": r["expects"], "see": r.get("see", []), "logonly": r.get("logonly", ""),
                               "exit": code, "port": s["port"]}, f, indent=1)
                return folder, code
            note = "exit %d\n%s\n%s" % (code, p.stdout or "", p.stderr or "")
        except Exception as e:   # one broken run must not end its group or vanish from the table
            note = "error: %s: %s\n" % (e.__class__.__name__, e)
        try:
            os.makedirs(s["shots"], exist_ok=True)
            with open(os.path.join(s["shots"], "%s-no-folder.txt" % r["scenario"]), "w", encoding="utf-8") as f:
                f.write(note)
        except OSError:
            pass   # merge's expected count still makes it a FAIL row
        return None, code

    def one_group(s, runs, cmds):
        for r, cmd in zip(runs, cmds):
            with lock:
                say("[%s] %s: start" % (s["name"], r["scenario"]))
            folder, code = one_run(s, r, cmd)
            with lock:
                results.append((s["name"], r, folder, code))
                say("[%s] %s: exit %d %s" % (s["name"], r["scenario"], code, folder or "(no run folder)"))

    if dry:
        print_plan(planned, say)
        return []
    if any(s.get("lane") for s, _, _ in planned):   # one pair: the groups one after another
        for g in planned:
            one_group(*g)
        return results
    threads = [threading.Thread(target=one_group, args=g, daemon=True) for g in planned]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return results


# --- merge -----------------------------------------------------------------------------------------------------------

def read_text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def judge(folder, meta=None, kit_root=None, checks=None):
    """(result, evidence) of one run folder: the log verdict (judge_logs), then the screenshot proof (prove)."""
    meta = meta or json.loads(read_text(os.path.join(folder, "run.json")) or "{}")
    result, evidence = judge_logs(folder, meta)
    if result != "PASS":
        return result, evidence
    return prove(folder, meta, evidence, kit_root, checks)


def prove(folder, meta, evidence, kit_root=None, checks=None):
    """A log PASS becomes PASS only with every see= check OK (proof.py), FAIL when a shot shows the wrong result,
    else UNPROVEN. A run marked logonly= (nothing visible to prove) keeps its log PASS."""
    see = list(meta.get("see") or [])
    if not see:
        if meta.get("logonly"):
            return "PASS", evidence + " [log only: %s]" % meta["logonly"]
        return "UNPROVEN", evidence + " [no screenshot check (see=): log lines alone prove no visible result]"
    sys.path.insert(0, HERE)
    import proof
    try:
        checks = checks if checks is not None else proof.load_proofs()
    except (OSError, proof.ProofError) as e:
        return "UNPROVEN", evidence + " [proofs.txt: %s]" % e
    states = []
    for name in see:
        if name not in checks:
            states.append(("MISSING", "%s: not in proofs.txt" % name))
        else:
            states.append(proof.grade(folder, checks[name], kit_root))
    text = "; ".join("%s %s" % st for st in states)
    if any(st == "BAD" for st, _ in states):
        return "FAIL", "shot shows the wrong result: " + text + " (logs: " + evidence[:120] + ")"
    if all(st == "OK" for st, _ in states):
        return "PASS", evidence + " [seen: " + text + "]"
    return "UNPROVEN", evidence + " [screenshot check not passed: " + text + "]"


def judge_logs(folder, meta):
    """(result, evidence) of one run folder from its files and logs alone (no screenshot proof)."""
    failed = read_text(os.path.join(folder, "FAILED.txt"))
    if failed is not None:
        return "FAIL", "FAILED.txt: " + (failed.strip().splitlines() or ["(empty)"])[0][:160]
    counts = read_text(os.path.join(folder, "counts.txt"))
    if counts:
        bad = [line.strip() for line in counts.splitlines() if line.startswith("FAIL")]
        if bad:
            return "FAIL", "counts.txt: %d FAIL, first: %s" % (len(bad), bad[0][:140])
    code = meta.get("exit", 0)
    if code:
        return "FAIL", "run.ps1 exit %s" % code
    good = [line for line in (counts or "").splitlines() if line.startswith("PASS")]
    if good and not meta.get("expects"):
        # A steps file's run (hs_steps.py): its counts are the verdict, it plays no "scenario played" line.
        return "PASS", "counts.txt: %d PASS" % len(good)
    expects = [tuple(e) for e in meta.get("expects", [])]
    if not expects:
        played = read_text(os.path.join(folder, "steps.txt")) or ""
        if "scenario played" not in played and os.path.isfile(os.path.join(folder, "steps.txt")):
            return "FAIL", "steps.txt has no 'scenario played'"
        return "PLAYED", "no expect=; look at the shots"
    logs = {x: read_text(os.path.join(folder, "hookshot_log-%s.txt" % x)) or "" for x in "AB"}
    found, missing = [], []
    for side, rx in expects:
        hit = None
        for x in ([side] if side else ["A", "B"]):
            m = re.search(rx, logs[x], re.M)
            if m:
                line = logs[x][logs[x].rfind("\n", 0, m.start()) + 1:].split("\n", 1)[0]
                hit = "%s: '%s'" % (x, line.strip()[:80])
                break
        (found if hit else missing).append(hit or "%s'%s'" % (side + ":" if side else "", rx))
    if missing:
        return "FAIL", "missing " + ", ".join(missing)
    return "PASS", "; ".join(found)


def run_folders(paths):
    """Run folders under the given paths: a folder with run.json, steps.txt or FAILED.txt."""
    out = []
    for p in paths:
        for d, dirs, files in os.walk(p):
            if {"run.json", "steps.txt", "FAILED.txt"} & set(files):
                out.append(d)
                dirs[:] = []
            dirs.sort()
    return sorted(set(out))


def no_folder_notes(paths):
    """<group folder>/<scenario>-no-folder.txt files: runs that left no run folder."""
    out = []
    for p in paths:
        for d, dirs, files in os.walk(p):
            dirs.sort()
            out += [os.path.join(d, f) for f in sorted(files) if f.endswith("-no-folder.txt")]
    return out


def group_defs(files):
    """{(scenario, label): (see list, logonly)} from group files (merge --regrade). A (scenario, label) that two lines
    give different proofs is left out (ambiguous)."""
    defs, clash = {}, set()
    for path in files:
        with open(path, encoding="utf-8-sig") as f:
            groups = parse_groups(f.read())
        for _, runs in groups:
            for r in runs:
                k, v = (r["scenario"], r["label"]), (tuple(r["see"]), r["logonly"])
                if k in defs and defs[k] != v:
                    clash.add(k)
                defs[k] = v
    for k in clash:
        del defs[k]
    return defs


def merge(paths, rel_to=None, expected=None, kit_root=None, defs=None, checks=None):
    """The markdown table of every run under paths and a summary line. A run with no run folder is a FAIL row; with
    `expected` (the number of planned runs), each planned run that left nothing at all is counted as FAIL too.
    defs (group_defs): see=/logonly= for each run instead of its run.json's (re-grading older runs)."""
    rows, tally = [], {}
    for n in no_folder_notes(paths):
        note = (read_text(n) or "").strip().splitlines()
        tally["FAIL"] = tally.get("FAIL", 0) + 1
        where = os.path.relpath(n, rel_to) if rel_to else n
        rows.append("| %s %s | FAIL | %s: no run folder (%s) |" % (
            os.path.basename(os.path.dirname(n)), os.path.basename(n)[:-len("-no-folder.txt")],
            where.replace("\\", "/"), (note[0] if note else "no note")[:140].replace("|", "/")))
    for f in run_folders(paths):
        meta = json.loads(read_text(os.path.join(f, "run.json")) or "{}")
        if defs is not None:
            key = (meta.get("scenario") or os.path.basename(os.path.dirname(f)), meta.get("label", ""))
            meta["see"], meta["logonly"] = list(defs.get(key, ((), ""))[0]), defs.get(key, ((), ""))[1]
        result, evidence = judge(f, meta, kit_root, checks)
        tally[result] = tally.get(result, 0) + 1
        name = meta.get("scenario") or os.path.basename(os.path.dirname(f))
        run = "%s %s" % (meta["group"], name) if meta.get("group") else name
        if meta.get("label"):
            run += " (%s)" % meta["label"]
        where = os.path.relpath(f, rel_to) if rel_to else f
        rows.append("| %s | %s | %s: %s |" % (run.replace("|", "/"), result, where.replace("\\", "/"),
                                              evidence.replace("|", "/")))
    if expected is not None and sum(tally.values()) < expected:
        lost = expected - sum(tally.values())
        tally["FAIL"] = tally.get("FAIL", 0) + lost
        rows.append("| (%d planned run(s)) | FAIL | no result at all: no run folder and no note |" % lost)
    total = sum(tally.values())
    summary = "%d run(s): %s" % (total, ", ".join("%d %s" % (tally[k], k) for k in ("PASS", "PLAYED", "UNPROVEN",
                                                                                   "FAIL") if k in tally) or "none")
    verdict = "FAIL" if tally.get("FAIL") or not total else "UNPROVEN" if tally.get("UNPROVEN") else "PASS"
    return "\n".join(["| Run | Result | Evidence |", "|---|---|---|"] + rows + ["", "%s -> %s" % (summary, verdict)])


# --- command line ----------------------------------------------------------------------------------------------------

def find_kit_root():
    d = REPO
    while True:
        c = os.path.join(d, "coop-kit")
        if os.path.isdir(c):
            return c
        up = os.path.dirname(d)
        if up == d:
            return None
        d = up


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("plan", "prepare", "run"):
        p = sub.add_parser(name)
        p.add_argument("groups")
        p.add_argument("--kit-root", help="folder of hookshot-A/-B (default: the first coop-kit above the repo)")
        p.add_argument("--server", help="coop_server.exe (default: <kit root>\\auto-A\\server\\coop_server.exe)")
        p.add_argument("--run-root", help="default: <kit root>\\shots-hookshot\\groups-<time>")
        p.add_argument("--dry-run", action="store_true")
        p.add_argument("--lane", type=int, choices=[1, 2], help="run every group in this test lane (lanes.py)")
        p.add_argument("--lag", type=int, default=0,
                       help="run.ps1 -Lag for every run: added round trip in ms (Longshot: the server's TEST-ONLY delay)")
        p.add_argument("--jitter", type=int, default=0, help="run.ps1 -Jitter for every run: +- ms each way")
        p.add_argument("--renderer-wait", type=int, default=0, help="run.ps1 -RendererWait for every run (0: default)")
        p.add_argument("--renderer-stall", type=int, default=0,
                       help="run.ps1 -RendererStall for every run (0: default)")
        p.add_argument("--renderer-limit", type=int, default=0,
                       help="run.ps1 -RendererLimit for every run (0: default)")
        p.add_argument("--kit", help="A,B: kit folders whose own start starts the lane's games (run.ps1 -Kit; "
                                     "kit_pair.py setup)")
    m = sub.add_parser("merge")
    m.add_argument("paths", nargs="+")
    m.add_argument("--out", help="write the table here too")
    m.add_argument("--kit-root", help="the folder with proof-refs (default: the first coop-kit above the repo)")
    m.add_argument("--regrade", action="store_true",
                   help="take see=/logonly= from today's tools/autotest/groups/*.txt (runs made before see= existed)")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "merge":
            defs = None
            if a.regrade:
                gdir = os.path.join(HERE, "groups")
                defs = group_defs(sorted(os.path.join(gdir, x) for x in os.listdir(gdir) if x.endswith(".txt")))
            kit = os.path.abspath(a.kit_root) if a.kit_root else find_kit_root()
            table = merge(a.paths, rel_to=os.path.dirname(os.path.abspath(a.paths[0])), kit_root=kit, defs=defs)
            print(table)
            if a.out:
                with open(a.out, "w", encoding="utf-8") as f:
                    f.write(table + "\n")
            return 0 if table.rstrip().endswith("PASS") else 1
        kit = os.path.abspath(a.kit_root) if a.kit_root else find_kit_root()
        if not kit:
            raise Fail("no coop-kit folder found; pass --kit-root")
        server = a.server or os.path.join(kit, "auto-A", "server", "coop_server.exe")
        stamp = "groups-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        run_root = a.run_root or (os.path.join(kit, "ls", "shots-lane%d" % a.lane, stamp) if a.lane
                                  else os.path.join(kit, "shots-hookshot", stamp))
        with open(a.groups, encoding="utf-8-sig") as f:   # a file with or without a BOM
            groups = parse_groups(f.read())
        kits = [os.path.abspath(k.strip()) for k in a.kit.split(",") if k.strip()] if a.kit else None
        planned = plan(groups, kit, run_root, server, a.lane or 0, kits, a.lag, a.jitter,
                       (a.renderer_wait, a.renderer_stall, a.renderer_limit))
        if a.cmd == "prepare" and a.lane:
            raise Fail("a lane's pair is filled by lanes.py setup --lane %d (hard links for read-only files only)"
                       % a.lane)
        if a.cmd == "plan":
            print_plan(planned)
            return 0
        if a.cmd == "prepare":
            return 0 if prepare(planned, kit, a.dry_run, restore=hs_restore) else 1
        if a.dry_run:
            run_groups(planned, dry=True)
            print("then: groups.py merge %s" % run_root)
            return 0
        if not os.path.isfile(server):
            raise Fail("no server at %s" % server)
        run_groups(planned)
        table = merge([run_root], rel_to=run_root, expected=sum(len(runs) for _, runs, _ in planned), kit_root=kit)
        with open(os.path.join(run_root, "results.md"), "w", encoding="utf-8") as f:
            f.write(table + "\n")
        print(table)
        return 0 if table.rstrip().endswith("PASS") else 1
    except Fail as e:
        print("groups.py: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
