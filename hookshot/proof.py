"""Two Hookshot games with co-op on one PC: both in Kokiri Forest, each should show the other's puppet Link.

    python -I hookshot/proof.py --hookshot <hookshot-src> --game-a <folder> --game-b <folder> --server <coop_server.exe>
                                --module <hookshot_coop.dll> --save <save folder> --shots <folder> [--port 27570]

Each game folder is a Hookshot game built with our patch (hookshot/README.md). This script puts the module, a coop.ini and
the save (account.sav, option.sav, slot_NN; a copy) into each folder, starts the server and both games with Hookshot's
control bridge (scripts/hookshot_ctl.py), presses A until each game plays, teleports both to Kokiri Forest, walks A
around while it takes shots of both pictures, prints the co-op lines of both logs, and closes everything. Nothing from the
game leaves the game folders and the shots folder."""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

SAVE_USER = "5a3c9e1f0b7d24680000000000000001"   # (hookshot_ctl.py's: the user folder a Hookshot game reads first)


def ctl(a, sock, *args, timeout=None, check=True):
    cmd = [sys.executable, "-I", os.path.join(a.hookshot, "scripts", "hookshot_ctl.py"), "--json", "--sock", sock] + \
        [str(x) for x in args]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if check and r.returncode != 0:
        raise SystemExit("hookshot_ctl %s: %s %s" % (" ".join(map(str, args)), r.stdout.strip(), r.stderr.strip()))
    lines = [json.loads(x) for x in r.stdout.splitlines() if x.startswith("{")]
    return lines[-1] if lines else {}


def prepare(a, game, name):
    exe_dir = os.path.join(game, "bin") if os.path.isfile(os.path.join(game, "bin", "hookshot.exe")) else game
    shutil.copy2(a.module, os.path.join(exe_dir, "hookshot_coop.dll"))
    with open(os.path.join(exe_dir, "coop.ini"), "w", encoding="utf-8") as f:
        f.write("server = 127.0.0.1:%d\nname = %s\n" % (a.port, name))
    dest = os.path.join(game, "user", "save", SAVE_USER)
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    shutil.copytree(a.save, dest)


def launch(a, game, tag):
    run = os.path.join(a.shots, "run-" + tag)
    os.makedirs(run, exist_ok=True)
    cmd = [sys.executable, "-I", os.path.join(a.hookshot, "scripts", "hookshot_ctl.py"), "--json", "--sock", "coop-proof-" + tag,
           "launch", game, "--run", run]
    for kv in a.env:
        cmd += ["--env", kv]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("launch %s: %s %s" % (tag, r.stdout.strip(), r.stderr.strip()))
    info = json.loads(r.stdout.strip().splitlines()[-1])
    print("%s: pid %s, socket %s" % (tag, info["pid"], info["socket"]), flush=True)
    return info


def to_play(a, sock, tag):
    ctl(a, sock, "wait", "frame", "--frame", 1, "--timeout", 1800, timeout=1900)
    deadline = time.monotonic() + 600
    while time.monotonic() < deadline:
        s = ctl(a, sock, "status")
        if s.get("state") == "play":
            print("%s: playing in %s" % (tag, s.get("map")), flush=True)
            return s
        ctl(a, sock, "press", "A", "--frames", 8, check=False, timeout=60)
        ctl(a, sock, "wait", "present", "--frames", 60, check=False, timeout=60)
    raise SystemExit("%s: not in play after 10 minutes" % tag)


def coop_lines(game):
    for p in (os.path.join(game, "hookshot_log.txt"), os.path.join(game, "bin", "hookshot_log.txt")):
        if os.path.isfile(p):
            with open(p, encoding="utf-8", errors="replace") as f:
                return [ln.rstrip() for ln in f if "coop" in ln or "co-op" in ln]
    return []


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for k in ("hookshot", "game-a", "game-b", "server", "module", "save", "shots"):
        p.add_argument("--" + k, required=True)
    p.add_argument("--port", type=int, default=27570)
    p.add_argument("--teleport", default="KokiriForest_Child", help="where both go: a teleport index or map")
    p.add_argument("--env", action="append", default=["HOS_AUDIO=0"])
    a = p.parse_args()
    os.makedirs(a.shots, exist_ok=True)
    prepare(a, a.game_a, "HookA")
    prepare(a, a.game_b, "HookB")
    with open(os.path.join(a.shots, "server-out.txt"), "w") as out:
        server = subprocess.Popen([a.server, "--port", str(a.port)], stdout=out, stderr=subprocess.STDOUT)
    games = {}
    try:
        games["A"] = launch(a, a.game_a, "A")
        games["B"] = launch(a, a.game_b, "B")
        for tag, g in games.items():
            to_play(a, g["socket"], tag)
        for tag, g in games.items():
            print("%s teleport: %s" % (tag, ctl(a, g["socket"], "teleport", a.teleport, "--arrive", 120, check=False,
                                               timeout=200)), flush=True)
        for tag, g in games.items():
            ctl(a, g["socket"], "wait", "in_play", "--timeout", 300, check=False, timeout=320)
        time.sleep(10)
        for i in range(4):
            ctl(a, games["A"]["socket"], "stick", "L", "0", "1", "--frames", 60, "--no-wait", check=False, timeout=30)
            time.sleep(0.5)
            for tag, g in games.items():
                path = os.path.abspath(os.path.join(a.shots, "%s-%d.png" % (tag, i)))
                print("%s shot %d: %s" % (tag, i, ctl(a, g["socket"], "shot", path, check=False, timeout=60)), flush=True)
            time.sleep(1.5)
        for tag, g in games.items():
            print("%s status: %s" % (tag, ctl(a, g["socket"], "status", check=False)), flush=True)
    finally:
        for tag, g in games.items():
            try:
                ctl(a, g["socket"], "quit", check=False, timeout=30)
            except Exception as e:   # (closed below)
                print("%s quit: %s" % (tag, e))
        time.sleep(5)
        for g in games.values():
            subprocess.run(["taskkill", "/PID", str(g["pid"]), "/T", "/F"], capture_output=True)
        server.terminate()
    for tag, game in (("A", a.game_a), ("B", a.game_b)):
        print("---- %s log (co-op lines)" % tag)
        for ln in coop_lines(game)[-60:]:
            print(ln)


if __name__ == "__main__":
    main()
