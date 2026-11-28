"""One rc pass of the in-game bridge tests (docs/ingame-tests.md) over both test lanes: plan, then one summary.

    python -I tools/autotest/rc_tests.py plan --out-dir DIR [--ingame DIR] [--lanes 1,2] [--history ROOT...]
                                              [--skip-proven ROOT... --identity KEY...] [--env NAME=VALUE ...]
    python -I tools/autotest/rc_tests.py summary --plan DIR/plan.json --runs ROOT [--session ID DIR ...] --out DIR
    python -I tools/autotest/rc_tests.py collect [DIR] --out FILE      (what plan runs: nodeid + markers, as JSON)

B #129 (2026-11-26, owner: everything uses the bridge test system): the rc rows are the pytest bridge tests, not .ps1
group lines. Interface agreed with test-bridge-c127-3: bt_plugin --plan FILE keeps exactly the nodeids of
FILE's lanes[L] and deselects the rest, matching on BASE nodeids (proven.base_nodeid: no --repeat [rN]); the plan's
skipped tests become summary rows with result SKIP; summary.json holds "identity" (proven.identity_of of the lane's
pair at boot and at session end, and env = bt_plugin boot_env()) and each row a "test_hash" (proven.test_hash).

SESSIONS (B #129 on review-c123-7's point 1 for party-hud-test-c126): a test marked needs_env('NAME=VALUE', ...)
(bt_plugin) needs game switches that would change what every other test sees (the party HUD probe's made-up hearts in
every HUD/pause shot), and a test marked needs_boot(NAME) runs only under its own boot (bt_plugin --boot NAME,
86711283c: boss stages the free-play warp cannot reach), so tests run grouped by (needs_boot, needs_env set): one
pytest session per group, each its own boot with that group's --boot and --env. The default group (no needs_env) gets no extra --env. A lane plays its sessions one after
another (one game pair); the lanes play at once. Each session has its own plan file (DIR/<id>.plan.json) and out
folder (<runs>/<id>); plan prints each session's pytest command.

plan: collects tools/autotest/ingame (rc_tests.py collect: pytest --collect-only with a plugin that writes each test's
base nodeid, smoke marker and needs_env switches). Smoke tests (marker smoke, no needs_env) go into EVERY lane's
default session (each lane's boot proves itself; smoke gate). The default group's other tests are balanced over the
lanes one by one by measured seconds (LPT); each needs_env group is one unit (one more boot: its tests stay
together) given to the lane with the fewest seconds. Seconds: the median of a test's earlier PASS/FAIL/FLAKY rows in
summary.json files under --history (SETUP-FAIL/SKIP/NO-REF/NOT-RUN rows are cut short or not played: not timings),
else the median of every measured test, else DEFAULT_S; a session's boot costs BOOT_S.

--skip-proven (B #128 rule, unchanged): a test is not played again when, on EVERY --identity key, a summary.json
with that identity key holds a row of it (same base nodeid, same test_hash as now) and every such row is PASS. A
FLAKY, FAIL, SETUP-FAIL, NO-REF or NOT-RUN row there, a summary without an identity key, a summary from a boot with
other game switches (identity.env vs bt_plugin BASE_ENV + the test's group --env + plan --env, per game), a summary
from another lane boot or with none (identity.boot vs bt_plugin boot_identity(): per game the boot script, its
sha256 and its '#! switches'), or a test whose module is gone proves nothing. A smoke test is never skipped. Each skipped test is listed in plan.json
"skipped" with its proof folders, and the rc summary lists it as SKIP.

summary: every session's summary.json (<runs>/<id>, or --session ID DIR) and the plan's skipped tests ->
DIR/summary.json + summary.md. A session without a readable summary.json is a SETUP-FAIL row, and every test the plan
gave a session that has no row in its summary.json is a NOT-RUN row (a --plan mismatch, a deselect bug, the smoke
gate, a session that never ran): never hidden. Verdict PASS only when at least one test passed and none is FAIL,
FLAKY, SETUP-FAIL, NO-REF or NOT-RUN.
"""
import argparse
import json
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import proven as P  # noqa: E402

INGAME_DIR = os.path.join(HERE, "ingame")
DEFAULT_S = 120.0          # bt_plugin DEFAULT_BUDGET: a test's deadline when it names none
BOOT_S = 120.0             # one more session = one more game-pair boot + its lane check (live.py start)
TIMED = ("PASS", "FAIL", "FLAKY")
NOT_PASS = ("FAIL", "FLAKY", "SETUP-FAIL", "NO-REF", "VOID", "NOT-RUN")   # (VOID: gpu_reset.py, never a pass)
RESULTS = ("PASS", "FAIL", "FLAKY", "SETUP-FAIL", "NO-REF", "VOID", "NOT-RUN", "SKIP")


class Fail(Exception):
    pass


# -- collection --------------------------------------------------------------------------------------------------

def _collector():
    """pytest plugin: each collected test's base nodeid, smoke marker, needs_env switches (in marker order) and
    needs_boot name ("" = the default boot). It reads the items BEFORE bt_plugin's own collection_modifyitems, which
    deselects the tests this run's --env / --boot cannot run (86711283c), even on --collect-only."""
    import pytest

    class Collector:
        def __init__(self):
            self.items = []
            self.errors = []

        def pytest_collectreport(self, report):
            if report.failed:
                self.errors.append("%s: %s" % (report.nodeid, str(report.longrepr).strip().splitlines()[-1:]))

        @pytest.hookimpl(tryfirst=True)
        def pytest_collection_modifyitems(self, config, items):
            for it in items:
                env = [str(a).strip() for m in it.iter_markers("needs_env") for a in m.args]
                m = it.get_closest_marker("needs_boot")
                t = P.base_nodeid(it.nodeid)
                if all(x["test"] != t for x in self.items):
                    self.items.append({"test": t, "smoke": it.get_closest_marker("smoke") is not None,
                                       "env": list(dict.fromkeys(env)), "boot": str(m.args[0]) if m and m.args else ""})
    return Collector()


def collect_here(folder, out):
    import pytest
    c = _collector()
    code = pytest.main([folder, "--collect-only", "-q", "-p", "no:cacheprovider"], plugins=[c])
    # Judged by the collection itself: bt_plugin's session end calls a run with no test NOT PASS (exit 1, 06784ad34),
    # also on --collect-only; a collection error, an internal/usage error or nothing collected is a Fail.
    if c.errors or code not in (0, 1) or not c.items:
        raise Fail("pytest --collect-only %s: exit %s, %d test(s), errors: %s" % (folder, code, len(c.items),
                                                                                  "; ".join(c.errors) or "-"))
    with open(out, "w", encoding="utf-8") as f:
        json.dump(c.items, f, indent=1)
    return c.items


def collect(folder, work):
    """[{test, smoke, env}] of the in-game tests under folder, collected in a pytest process of its own."""
    out = os.path.join(work, "collect.json")
    cwd = os.path.join(work, "collect")   # (bt_plugin's session end writes ingame-out/ into the current folder)
    os.makedirs(cwd, exist_ok=True)
    p = P.hs().run_quiet([sys.executable, "-I", os.path.abspath(__file__), "collect", os.path.abspath(folder),
                          "--out", os.path.abspath(out)], capture_output=True, text=True, cwd=cwd)
    if p.returncode != 0 or not os.path.isfile(out):
        raise Fail("collect %s: exit %s: %s" % (folder, p.returncode, (p.stdout + p.stderr)[-600:]))
    with open(out, encoding="utf-8") as f:
        return json.load(f)


# -- history and proofs ------------------------------------------------------------------------------------------

def summaries(roots):
    """[(folder, summary.json dict)] under the roots."""
    out = []
    for root in roots or []:
        for d, _, files in os.walk(root):
            if "summary.json" in files:
                try:
                    with open(os.path.join(d, "summary.json"), encoding="utf-8") as f:
                        doc = json.load(f)
                except (OSError, ValueError):
                    continue
                if isinstance(doc, dict) and isinstance(doc.get("tests"), list):
                    out.append((d, doc))
    return out


def history(roots):
    """{base nodeid: [seconds]} of the timed rows."""
    h = {}
    for _, doc in summaries(roots):
        for r in doc["tests"]:
            if r.get("result") in TIMED and isinstance(r.get("seconds"), (int, float)) and r.get("test"):
                h.setdefault(P.base_nodeid(r["test"]), []).append(float(r["seconds"]))
    return h


def estimate(test, hist):
    if hist.get(test):
        return statistics.median(hist[test]), "measured (%d run(s))" % len(hist[test])
    every = [s for v in hist.values() for s in v]
    if every:
        return statistics.median(every), "median of every measured test"
    return DEFAULT_S, "default %d s" % DEFAULT_S


def proofs(test, keys, docs, repo=None, env=None, boot=None):
    """(proven, [{folder, key, result}]): see the module text. env: proven.env_key of the boot this test gets; a
    summary from a boot with other switches (identity.env) proves nothing (review-c123-6 08:41: a PASS on the HUD-off
    probe boot must not prove a normal boot). boot: proven.boot_key of the lane boot this pass makes; a summary with
    another identity.boot (other boot scripts, other script text, other '#! switches') or none proves nothing
    (review-c123-7)."""
    th = P.test_hash(test, repo)
    seen = {k: [] for k in keys}
    if not th or env is None or boot is None:
        return False, []
    for folder, doc in docs:
        ident = doc.get("identity") or {}
        k = ident.get("key")
        if k not in seen or P.env_key(ident.get("env")) != env or P.boot_key(ident.get("boot")) != boot:
            continue
        for r in doc["tests"]:
            if r.get("test") and P.base_nodeid(r["test"]) == test and r.get("test_hash") == th:
                seen[k].append({"folder": folder, "key": k, "result": r.get("result")})
    ok = bool(keys) and all(seen[k] and all(x["result"] == "PASS" for x in seen[k]) for k in keys)
    return ok, [x for k in keys for x in seen[k]]


# -- plan --------------------------------------------------------------------------------------------------------

def is_smoke(it):
    return bool(it.get("smoke")) and not it.get("env") and not it.get("boot")


def groups_of(items):
    """[{name, boot, env, tests}]: the default group (default boot, no needs_env) first, then one group per
    (needs_boot, needs_env set) (the same switches per game = the same set, proven.env_key), in collection order.
    Names: "default", "envN" (default boot), "<boot>" / "<boot>-envN". Smoke tests are not in any group."""
    out = [{"name": "default", "boot": "", "env": [], "tests": []}]
    by_key = {("", P.env_key([])): out[0]}
    for it in items:
        if is_smoke(it):
            continue
        boot, env = it.get("boot") or "", list(it.get("env") or [])
        k = (boot, P.env_key(env))
        if k not in by_key:
            n = len(by_key)
            name = ("env%d" % n) if not boot else (boot if not env else "%s-env%d" % (boot, n))
            by_key[k] = {"name": name, "boot": boot, "env": env, "tests": []}
            out.append(by_key[k])
        by_key[k]["tests"].append(it["test"])
    return out


def plan(items, lanes, roots=None, proven_roots=None, keys=None, repo=None, env=None, base_env=None, boot=None):
    """items: collect() rows. env: --env for every session (on top of each group's own). boot: the lane boot
    (bt_plugin boot_identity; default asked of bt_plugin by proven.ingame_boot_identity). See the module text."""
    if not lanes:
        raise Fail("--lanes: at least one lane")
    if not items:
        raise Fail("no in-game test collected")
    tests = [it["test"] for it in items]
    smoke = [it["test"] for it in items if is_smoke(it)]
    groups = groups_of(items)
    extra = [e.strip() for e in (env or [])]
    skipped = []
    if proven_roots:
        if not keys:
            raise Fail("--skip-proven needs --identity (each lane's build key: proven.py identity --lane N)")
        docs = summaries(proven_roots)
        base = P.ingame_base_env(repo) if base_env is None else list(base_env)
        boots = {}
        for g in groups:
            if g["boot"] not in boots:
                b = (P.ingame_boot_identity(repo, g["boot"] or None) if boot is None
                     else (boot.get(g["boot"]) if "A" not in boot else (boot if not g["boot"] else None)))
                boots[g["boot"]] = P.boot_key(b)
                if boots[g["boot"]] is None:
                    raise Fail("--skip-proven: the lane boot %s is not known (bt_plugin boot_identity)"
                               % (g["boot"] or "(default)"))
        for g in groups:
            want = P.env_key(base + g["env"] + extra)
            keep = []
            for t in g["tests"]:
                ok, proof = proofs(t, keys, docs, repo, want, boots[g["boot"]])
                if ok:
                    skipped.append({"test": t, "group": g["name"], "keys": list(keys), "proof": proof})
                else:
                    keep.append(t)
            g["tests"] = keep
    hist = history(roots)
    est = {t: estimate(t, hist) for t in tests}
    order = {t: i for i, t in enumerate(tests)}
    smoke_s = sum(est[t][0] for t in smoke)
    load = {n: BOOT_S + smoke_s for n in lanes}   # every lane boots its default session and plays the smoke tier
    default = {n: [] for n in lanes}
    extra_sessions = {n: [] for n in lanes}
    units = [(est[t][0], order[t], None, t) for t in groups[0]["tests"]]
    units += [(BOOT_S + sum(est[t][0] for t in g["tests"]), order[g["tests"][0]], g, None)
              for g in groups[1:] if g["tests"]]
    for secs, _, g, t in sorted(units, key=lambda u: (-u[0], u[1])):
        best = min(lanes, key=lambda n: (load[n], n))
        load[best] += secs
        if g is None:
            default[best].append(t)
        else:
            extra_sessions[best].append(g)
    sessions = []
    for n in lanes:
        sessions.append({"id": "lane%d-default" % n, "lane": n, "group": "default", "boot": "", "env": list(extra),
                         "tests": smoke + sorted(default[n], key=lambda t: order[t])})
        for g in sorted(extra_sessions[n], key=lambda g: order[g["tests"][0]]):
            sessions.append({"id": "lane%d-%s" % (n, g["name"]), "lane": n, "group": g["name"], "boot": g["boot"],
                             "env": list(g["env"]) + list(extra), "tests": sorted(g["tests"], key=lambda t: order[t])})
    for s in sessions:
        s["seconds"] = round(BOOT_S + sum(est[t][0] for t in s["tests"]), 1)
    return {"sessions": sessions, "smoke": smoke, "skipped": skipped,
            "groups": [{"name": g["name"], "boot": g["boot"], "env": g["env"]} for g in groups],
            "lane_seconds": {str(n): round(load[n], 1) for n in lanes},
            "estimates": {t: {"seconds": round(est[t][0], 1), "from": est[t][1]} for t in tests},
            "history": list(roots or []), "env": extra,
            "skip_proven": {"roots": list(proven_roots or []), "keys": list(keys or [])},
            "longest_lane_minutes": round(max(load.values()) / 60, 1)}


def session_plan(s):
    """The plan file one pytest session reads (bt_plugin --plan): lanes[L] = exactly the tests it plays."""
    return {"session": s["id"], "lanes": {str(s["lane"]): s["tests"]}, "env": s["env"], "boot": s.get("boot") or "",
            "skipped": []}


def command(s, plan_file, runs, expect="<sha>"):
    cmd = ["python", "-I", "-m", "pytest", "tools/autotest/ingame", "--lanes", str(s["lane"]), "--plan", plan_file,
           "--out", os.path.join(runs, s["id"]), "--expect-commit", expect]
    if s.get("boot"):
        cmd += ["--boot", s["boot"]]      # (bt_plugin 86711283c: a test runs only under its own boot)
    for e in s["env"]:
        cmd += ["--env", e]
    return cmd


def write_plan(doc, out_dir, runs=None):
    os.makedirs(out_dir, exist_ok=True)
    for s in doc["sessions"]:
        s["plan_file"] = os.path.join(out_dir, s["id"] + ".plan.json")
        with open(s["plan_file"], "w", encoding="utf-8") as f:
            json.dump(session_plan(s), f, indent=1)
        s["command"] = command(s, s["plan_file"], runs or os.path.join(out_dir, "runs"))
    with open(os.path.join(out_dir, "plan.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)


# -- summary -----------------------------------------------------------------------------------------------------

def merge(session_dirs, plan_doc=None):
    """One rc summary. session_dirs: [dir] (no plan) or [(session id, dir)]; with a plan, every session of the plan
    is checked (a session not given counts as not run). See the module text."""
    pairs = [(None, d) if isinstance(d, str) else (str(d[0]), d[1]) for d in session_dirs]
    planned = (plan_doc or {}).get("sessions")
    if plan_doc is not None and planned is None:
        raise Fail("--plan: no 'sessions' (a plan.json from rc_tests.py plan)")
    if planned is not None and any(n is None for n, _ in pairs):
        raise Fail("summary --plan needs each session as ID DIR (--runs ROOT or --session ID DIR)")
    rows, lanes, ran = [], [], {}
    for n, d in pairs:
        try:
            with open(os.path.join(d, "summary.json"), encoding="utf-8") as f:
                doc = json.load(f)
            if not isinstance(doc.get("tests"), list):
                raise ValueError("no tests list")
        except (OSError, ValueError, AttributeError) as e:
            rows.append({"test": "(session %s)" % (n or d), "result": "SETUP-FAIL", "lane": d,
                         "why": "no readable summary.json: %s" % e})
            lanes.append({"session": n, "dir": d, "identity": None, "verdict": "SETUP-FAIL"})
            continue
        lanes.append({"session": n, "dir": d, "identity": doc.get("identity"), "verdict": doc.get("verdict"),
                      "minutes": doc.get("minutes")})
        rows += [dict(r, lane=d, session=n) for r in doc["tests"]]
        ran[n] = {P.base_nodeid(r["test"]) for r in doc["tests"] if r.get("test")}
    given = dict(pairs)
    for s in planned or []:
        for t in s["tests"]:
            if t not in ran.get(s["id"], ()):
                rows.append({"test": t, "result": "NOT-RUN", "lane": given.get(s["id"], ""), "session": s["id"],
                             "why": "planned in session %s, no row in its summary.json%s" % (
                                 s["id"], "" if s["id"] in given else " (session not given)")})
    played = {P.base_nodeid(r["test"]) for r in rows if r.get("test")}
    for s in (plan_doc or {}).get("skipped", []):
        if s["test"] not in played:
            rows.append({"test": s["test"], "result": "SKIP", "lane": "",
                         "why": "proven: " + "; ".join("%s %s" % (x["result"], x["folder"]) for x in s["proof"])})
    counts = {k: sum(1 for r in rows if r.get("result") == k) for k in RESULTS}
    bad = sum(counts[k] for k in NOT_PASS) + sum(1 for r in rows if r.get("result") not in counts)
    return {"sessions": lanes, "counts": counts, "tests": rows,
            "minutes": max([x.get("minutes") or 0 for x in lanes] or [0]),
            "verdict": "PASS" if counts["PASS"] and not bad else "NOT PASS"}


def summary_md(doc):
    c = doc["counts"]
    out = ["# rc in-game tests: %s" % doc["verdict"], "",
           " ".join("%s %d" % (k, v) for k, v in c.items()) + ", ~%s min (longest session)" % doc["minutes"], "",
           "| result | test | session | why |", "|---|---|---|---|"]
    order = {"FAIL": 0, "SETUP-FAIL": 1, "VOID": 2, "NOT-RUN": 3, "FLAKY": 4, "NO-REF": 5, "PASS": 6, "SKIP": 7}
    for r in sorted(doc["tests"], key=lambda r: order.get(r.get("result"), -1)):
        out.append("| %s | %s | %s | %s |" % (r.get("result"), r.get("test"),
                                             r.get("session") or os.path.basename(r.get("lane") or ""),
                                             (r.get("why") or "").replace("|", "/").replace("\n", " ")[:300]))
    return "\n".join(out) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("collect")
    c.add_argument("folder", nargs="?", default=INGAME_DIR)
    c.add_argument("--out", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--ingame", default=INGAME_DIR, help="the in-game tests folder (default tools/autotest/ingame)")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--runs", help="where the sessions' out folders go (default OUT_DIR/runs): shown in the commands")
    p.add_argument("--lanes", default="1,2")
    p.add_argument("--history", nargs="*", default=[])
    p.add_argument("--skip-proven", nargs="*", default=[])
    p.add_argument("--identity", nargs="*", default=[])
    p.add_argument("--env", action="append", default=[], help="an --env switch for every session (each one)")
    s = sub.add_parser("summary")
    s.add_argument("lane_dirs", nargs="*", help="session out dirs (no --plan)")
    s.add_argument("--plan")
    s.add_argument("--runs", help="ROOT: each plan session's out dir is ROOT/<id>")
    s.add_argument("--session", nargs=2, action="append", default=[], metavar=("ID", "DIR"))
    s.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "collect":
            collect_here(a.folder, a.out)
            return 0
        if a.cmd == "plan":
            os.makedirs(a.out_dir, exist_ok=True)
            items = collect(a.ingame, a.out_dir)
            lanes = [int(x) for x in a.lanes.split(",") if x.strip()]
            doc = plan(items, lanes, a.history, a.skip_proven, a.identity, env=a.env)
            write_plan(doc, a.out_dir, a.runs)
            for t in doc["skipped"]:
                print("skip %s (proven: %s)" % (t["test"], "; ".join("%s %s" % (x["result"], x["folder"])
                                                                    for x in t["proof"])))
            for x in doc["sessions"]:
                print("%s: %d test(s), ~%.1f min, env %s\n  %s" % (x["id"], len(x["tests"]), x["seconds"] / 60,
                                                                    " ".join(x["env"]) or "-", " ".join(x["command"])))
            print("longest lane ~%.1f min, %d skipped (proven on this build)" % (doc["longest_lane_minutes"],
                                                                                len(doc["skipped"])))
            return 0
        pd = None
        dirs = list(a.lane_dirs) + [tuple(x) for x in a.session]
        if a.plan:
            with open(a.plan, encoding="utf-8") as f:
                pd = json.load(f)
            have = {x[0] for x in dirs if isinstance(x, tuple)}
            if a.runs:
                dirs += [(x["id"], os.path.join(a.runs, x["id"])) for x in pd.get("sessions") or []
                         if x["id"] not in have]
        if not dirs:
            raise Fail("summary: no session (DIR..., or --plan with --runs ROOT / --session ID DIR)")
        doc = merge(dirs, pd)
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, "summary.json"), "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
        with open(os.path.join(a.out, "summary.md"), "w", encoding="utf-8") as f:
            f.write(summary_md(doc))
        print("rc in-game tests: %s (%s)" % (doc["verdict"], ", ".join("%s %d" % kv for kv in doc["counts"].items())))
        return 0 if doc["verdict"] == "PASS" else 1
    except (Fail, P.Fail, OSError, ValueError) as e:
        print("rc_tests.py: %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
