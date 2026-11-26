"""The play-test bridge's proof in two running games (docs/playtest.md, "Bridge protocol").

Talks to both games' bridges (standard library only) and checks, in this order, so a failed hold is known first:
  1. hold: both paused for --hold seconds (past the server's 10 s silence timeout); the frame count and Link's place
     unchanged on both, and both still connected in co-op;
  2. step: 60 frames on each advance exactly 60;
  3. shot: one screenshot of each game;
  4. input: A walks (stick up, 30 frames): Link moved, and the frame advanced 30;
  5. warp: both to --scene (free play: Longshot's teleport); arrived, held;
  6. flag + dump: --flag set to 1 in A; after a few frames both dumps have it 1 (synced);
  7. give: bombs in A (needs LICHII_COOP_CHEATS=1);
  8. a client killed in the middle of a long step (3600) lets game A run again at once;
  9. resume: both run again;
 10. (Windows, with --session) while held each game's window answers (not hung), and last: B held, its window
     closed, the process gone within 15 s.
Each check prints PASS or FAIL with what was seen; the whole log is written as JSON to --out. Exit 0 all passed.

    python -I tools/autotest/bridge_proof.py --port-a 5011 --port-b 5012 --out <dir> [--hold 30]
        [--scene KokiriForest_Child] [--flag <a shared flag name>] [--session <live.py session folder>]
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time

WINDOWS = os.name == "nt"


def windows_of(pid):
    """(Windows) the visible top-level windows of a process."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    found = []
    proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def each(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    user32.EnumWindows(proc(each), 0)
    return found


def answers(hwnd):
    """(Windows) the window's thread takes a message within 2 s and Windows does not call it hung."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    result = ctypes.c_size_t()
    ok = user32.SendMessageTimeoutW(wintypes.HWND(hwnd), 0, 0, 0, 0x0002, 2000, ctypes.byref(result))  # WM_NULL, SMTO_ABORTIFHUNG
    return bool(ok) and not user32.IsHungAppWindow(wintypes.HWND(hwnd))


def close_window(hwnd):
    import ctypes
    from ctypes import wintypes
    ctypes.windll.user32.PostMessageW(wintypes.HWND(hwnd), 0x0010, 0, 0)  # WM_CLOSE


def pid_alive(pid):
    if WINDOWS:
        import ctypes
        h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = ctypes.c_ulong()
        ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(h)
        return code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


class Bridge:
    def __init__(self, port, name):
        self.name = name
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=120)
        self.file = self.sock.makefile("rwb")
        self.next_id = 0

    def call(self, cmd, **fields):
        self.next_id += 1
        msg = dict(fields, cmd=cmd, id=self.next_id)
        self.file.write((json.dumps(msg) + "\n").encode())
        self.file.flush()
        line = self.file.readline()
        if not line:
            raise RuntimeError("%s: the bridge closed the connection" % self.name)
        reply = json.loads(line)
        if reply.get("id") != self.next_id:
            raise RuntimeError("%s: reply out of order: %r" % (self.name, reply))
        return reply

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def players_connected(state):
    """Both players are still in the session. "quiet" counts: the other game, held 30 s, sends nothing for a while and the
    list calls it quiet; it is not disconnected (S8: both games showed it, the session was fine)."""
    players = ((state or {}).get("coop") or {}).get("players") or []
    return len(players) >= 2 and all(p.get("state") in ("connected", "quiet") for p in players)


def scene_matches(in_state, scene):
    """The module's rule (WarpArrived): the same name in any case, or `scene` with its `_Child`/`_Adult` ending is the
    map (the map may not carry the age)."""
    have, want = str(in_state or "").lower(), str(scene or "").lower()
    if not have or not want:
        return False
    if have == want:
        return True
    return any(want.endswith(x) and want[:-len(x)] == have for x in ("_child", "_adult"))


def warp_verdict(reply, scene):
    """(pass, seen) for the warp check: it arrived in `scene` and the game is held. In free play the warp goes by
    Longshot's teleport (a map of its list, no entrance); a refusal (`not_in_event`: the scene is not in that list) is a
    FAIL here, with its message in `seen`, so a wrong --scene is told apart from a hang."""
    st = reply.get("state") or {}
    seen = {"ok": reply.get("ok"), "error": reply.get("error"), "scene": st.get("scene"), "frame": reply.get("frame"),
            "via": reply.get("via"), "map": reply.get("map"), "message": reply.get("message")}
    # Longshot's teleport picks the map for Link's age: the reply's `map` (what it sent) is what Link should be in
    want = reply.get("map") or scene
    return bool(reply.get("ok") and reply.get("arrived") and scene_matches(st.get("scene"), want)
                and reply.get("paused")), seen


def moved_verdict(before, after, reply):
    """(pass, seen) for the input check. Both places must be read (a null place is a stale snapshot, F1: the check says
    so, it does not call it "did not move")."""
    p0, p1 = before.get("pos"), after.get("pos")
    seen = {"from": p0, "to": p1, "frames": [reply.get("from"), reply.get("frame")],
            "snapshot_stale": [before.get("snapshot_stale"), after.get("snapshot_stale")]}
    if not (p0 and p1):
        return False, dict(seen, why="no place read (stale snapshot)")
    moved = abs(p0[0] - p1[0]) + abs(p0[2] - p1[2]) > 0.3
    return bool(reply.get("ok") and moved and reply.get("frame") - reply.get("from", -1) == 30), seen


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port-a", type=int, required=True)
    ap.add_argument("--port-b", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--hold", type=float, default=30.0)
    ap.add_argument("--scene", default="KokiriForest_Child")
    ap.add_argument("--flag", default="")
    ap.add_argument("--session", help="live.py's session folder (pids.json: the games' processes, for the window checks)")
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    log = []
    results = []

    def note(check, ok, seen):
        results.append({"check": check, "pass": bool(ok), "seen": seen})
        print("%s %s: %s" % ("PASS" if ok else "FAIL", check, json.dumps(seen)[:400]), flush=True)

    pids = {}
    if a.session:
        try:
            pids = {k: int(v) for k, v in (json.load(open(os.path.join(a.session, "pids.json"), encoding="utf-8"))
                                           .get("games") or {}).items() if v}
        except (OSError, ValueError) as e:
            print("no pids.json in %s (%s): the window checks are skipped" % (a.session, e))
    A, B = Bridge(a.port_a, "A"), Bridge(a.port_b, "B")
    games = {"A": A, "B": B}

    def call(g, cmd, **f):
        r = games[g].call(cmd, **f)
        log.append({"game": g, "cmd": cmd, "fields": f, "reply": r, "t": time.time()})
        return r

    try:
        for g in games:
            r = call(g, "hello")
            note("hello " + g, r.get("ok") and r.get("protocol") == 1 and r.get("control"), r)

        # 1. the hold, first
        before = {}
        for g in games:
            r = call(g, "pause")
            note("pause " + g, r.get("ok") and r.get("paused"), r)
            before[g] = call(g, "state")
        time.sleep(a.hold / 2)
        if WINDOWS and pids:
            for g in games:
                hw = windows_of(pids.get(g, 0))
                note("held %s: its window answers (not hung)" % g, hw and all(answers(h) for h in hw),
                     {"pid": pids.get(g), "windows": len(hw)})
        time.sleep(a.hold / 2)
        for g in games:
            s = call(g, "state")
            b = before[g]
            same = s.get("frame") == b.get("frame") and s.get("pos") == b.get("pos")
            note("hold %ds %s: frame and place unchanged" % (a.hold, g), same and s.get("paused"),
                 {"frame": [b.get("frame"), s.get("frame")], "pos": [b.get("pos"), s.get("pos")]})
            note("hold %s: co-op still connected" % g, players_connected(s), (s.get("coop") or {}))

        # 2. step exact
        for g in games:
            r = call(g, "step", frames=60)
            note("step 60 " + g, r.get("ok") and r.get("frame") - r.get("from", -1) == 60 and r.get("paused"), r)

        # 3. shots
        for g in games:
            r = call(g, "shot", name="proof-hold-" + g)
            note("shot " + g, r.get("ok") and os.path.isfile(r.get("path", "")), r)

        # 4. input moves Link
        s0 = call("A", "state")
        r = call("A", "input", stick=[0, 1.0], frames=30)
        ok, seen = moved_verdict(s0, r.get("state") or {}, r)
        seen.update(camera=(r.get("state") or {}).get("camera"))
        note("input stick 30 frames A: Link moved, frame +30", ok, seen)

        # 5. warp both. In free play it goes by Longshot's teleport (a map of its list, no entrance); unproven through
        # this verb until a lane run (docs/playtest.md).
        for g in games:
            r = call(g, "warp", scene=a.scene, timeout=60)
            ok, seen = warp_verdict(r, a.scene)
            note("warp %s %s" % (a.scene, g), ok, seen)

        # 6. flag in A, synced to B (both stepped), dumps agree
        if a.flag:
            r = call("A", "flag", name=a.flag, value=1)
            note("flag %s=1 A" % a.flag, r.get("ok"), r)  # (the queue applies it at the next frame: the dumps prove it)
            for _ in range(6):  # ~3 s of game, both stepped the same
                for g in games:
                    call(g, "step", frames=30)
            dumps = {}
            for g in games:
                r = call(g, "dump", tag="proof-flag")
                ok = r.get("ok") and os.path.isfile(r.get("path", ""))
                dumps[g] = json.load(open(r["path"], encoding="utf-8")) if ok else {}
                note("dump " + g, ok and dumps[g].get("frame") == r.get("frame"), {"path": r.get("path"), "flags": r.get("flags")})
            fa = (dumps.get("A", {}).get("flags") or {}).get(a.flag)
            fb = (dumps.get("B", {}).get("flags") or {}).get(a.flag)
            note("flag %s synced: A and B dumps both 1" % a.flag, fa == 1 and fb == 1, {"A": fa, "B": fb})

        # 7. give
        r = call("A", "give", item="bombs")
        # (the give is written inside the held call: no step is needed; the state lists the per-player counts too)
        s = call("A", "state")
        inv = s.get("inventory") or {}
        bag = inv.get("PouchData.Item_BombBag_Small.StockNum")
        bombs = inv.get("PouchData.Item_Bomb.StockNum")
        note("give bombs A", r.get("ok") and (bag or 0) > 0 and (bombs or 0) > 0,
             {"reply": r, "bag": bag, "bombs": bombs, "keys": sorted(inv)[:12]})

        for g in games:
            r = call(g, "shot", name="proof-end-" + g)
            note("shot end " + g, r.get("ok") and os.path.isfile(r.get("path", "")), {"path": r.get("path")})

        # 8. a client killed in the middle of a long step: A runs again at once. (One client a game: ours lets go
        # first, which resumes A; the stand-in client then holds it and steps 3600 frames, and is killed.)
        A.close()
        msgs = "".join(json.dumps(m) + chr(10) for m in ({"cmd": "pause"}, {"cmd": "step", "frames": 3600})).encode()
        code = chr(10).join(["import socket, time",
                             "s = socket.create_connection(('127.0.0.1', %d))" % a.port_a,
                             "s.sendall(%r)" % msgs,
                             "time.sleep(600)"])
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "hookshot"))
        import hs
        child = hs.popen_quiet([sys.executable, "-I", "-c", code])
        time.sleep(3)
        child.kill()
        child.wait()
        time.sleep(0.5)
        A = games["A"] = Bridge(a.port_a, "A")
        s1 = call("A", "state")
        time.sleep(1.0)
        s2 = call("A", "state")
        note("client killed mid-step: A runs again", not s2.get("paused") and s2.get("frame", 0) - s1.get("frame", 0) > 20,
             {"frames": [s1.get("frame"), s2.get("frame")], "paused": s2.get("paused")})
    except Exception as e:  # (a broken connection, a refused reply: a FAIL, and the games are let go below)
        note("exception", False, repr(e))
    finally:
        # 10. last: B held and its window closed: the game exits promptly (its hold must not keep it alive).
        if WINDOWS and pids.get("B"):
            try:
                call("B", "pause")
                hw = windows_of(pids["B"])
                for h in hw:
                    close_window(h)
                t0 = time.time()
                while pid_alive(pids["B"]) and time.time() - t0 < 15:
                    time.sleep(0.25)
                gone = not pid_alive(pids["B"])
                note("held B, window closed: the game exits within 15 s", gone and hw,
                     {"windows": len(hw), "seconds": round(time.time() - t0, 1)})
                if gone:
                    games.pop("B").close()
            except Exception as e:
                note("held B, window closed", False, repr(e))
        for g in games:
            try:
                r = call(g, "resume")
                note("resume " + g, r.get("ok") and not r.get("paused"), r)
            except Exception as e:
                note("resume " + g, False, repr(e))
            games[g].close()
        with open(os.path.join(a.out, "bridge-proof.json"), "w", encoding="utf-8") as f:
            json.dump({"results": results, "log": log}, f, indent=1)
    failed = [x["check"] for x in results if not x["pass"]]
    print("bridge proof: %d checks, %d failed%s" % (len(results), len(failed), (": " + ", ".join(failed)) if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
