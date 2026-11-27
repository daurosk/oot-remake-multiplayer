"""v1 P0 PROOF (not an rc row): co-op's first hook on a STOCK Longshot game, no Longshot rebuild (hookshot/dropin/).

The bridge pytest port of hookshot/dropin/proof-run.ps1's game start (that script now only installs the official tools
and builds the stock game: -SetupOnly / -BuildOnly). A stock game has no play-test bridge (it is ours, in the module),
so this test boots no lane pair: it judges by our proxy's own log (squadala-dropin.log, hookshot/dropin/dropin.c) and
keeps one window shot through the shared helper (offline/lib/screen-shot.ps1 via hs_steps.window_shot_command).

Runs only with --env BT_DROPIN=1 (needs_env: every other run deselects it, no rc pass carries it). No lane, no
--expect-commit: the stock game is the one the official builder made at C:\\lsb\\<tag>\\game (proof-run.ps1 -BuildOnly;
BT_DROPIN_ROOT changes C:\\lsb), <tag> = hookshot/PIN's tag (the Longshot release co-op ships on).

Steps (each value's source in the comment beside it):
1. SETUP (< 30 s; SETUP-FAIL): the stock game folder, its longshot_version.txt version= == the PIN tag; hookshot/dropin/
   test.ps1 PASS on THIS tree (builds out/winmm.dll + out/selftest.exe from it and runs the stand-in game 4 ways);
   Longshot's telemetry addresses blanked in the stock stamp (owner rule 2026-11-17: nothing goes to Longshot; the stock
   game reads crash_reports= / perf_reports= from <game>/longshot_version.txt: Longshot host/hos_crash_dialog.c
   find_addresses, gpu/vk/perf_report.cpp perf_report_init, as hookshot/patches/0027 shows); a monitor connected.
2. selftest.exe on the stock exe (offline): every symbol found, every prologue hookable (FAIL otherwise).
3. Drop out/winmm.dll beside bin/longshot.exe, start it (cwd = the game folder, as the builder's launcher does), place
   its window on the test display; 'dropin: attached' within ATTACH_S, then 'dropin: frame 600 ' within FRAME_S, the
   guest word line "the game's pacibsp: hook on", exactly ONE 'hooked at' (the game; its --crash-watch helper loads the
   same winmm.dll and must only forward: run 20261126-064809 hooked both), the game still running.
4. One window shot of the game (uncovered, not one colour; SETUP-FAIL otherwise). Co-op draws nothing on a stock game
   yet, so there is no reference picture: the shot is kept for the owner under <out>/dropin/.
BT_DROPIN_SETUP_ONLY=1 in the environment: steps 1-2 only, no game (ends NO-REF): the set-up run a preflight cites.
Every path: the game closed (WM_CLOSE, then killed after CLOSE_S), its helper gone, winmm.dll removed again, the logs kept.
"""
import ctypes
import os
import re
import shutil
import subprocess
import time

import pytest

import pair as P

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DROPIN = os.path.join(REPO, "hookshot", "dropin")
PIN = os.path.join(REPO, "hookshot", "PIN")
ROOT_ENV = "BT_DROPIN_ROOT"
SETUP_ONLY_ENV = "BT_DROPIN_SETUP_ONLY"   # =1: steps 1-2 only (no game start), ends NO-REF: the preflight's real set-up run
DEFAULT_ROOT = r"C:\lsb"          # proof-run.ps1 -ShortRoot default (MAX_PATH: reports/v1-longshot-dropin-2.md)
ATTACH_S = 90                     # proof-run.ps1's wait for 'dropin: attached' (seen at +0.1 s, run 20261126-064809)
FRAME_S = 240                     # proof-run.ps1's wait for frame 600 (seen at +23 s on v3.2.0, same run)
CLOSE_S = 30                      # proof-run.ps1: CloseMainWindow, WaitForExit(30000), then Kill
HELPER_S = 15                     # the --crash-watch helper ends with its game; then it is killed and named
TELEMETRY = ("crash_reports", "perf_reports")
LOG = "squadala-dropin.log"       # dropin.c attach(): beside the exe
WM_CLOSE = 0x0010


def pin_tag():
    with open(PIN, encoding="utf-8") as f:
        for line in f:
            if line.startswith("tag="):
                return line.split("=", 1)[1].strip()
    raise P.SetupFail("hookshot/PIN has no tag= line")


def stamp_value(text, key):
    m = re.search(r"^%s=(.*)$" % re.escape(key), text, re.M)
    return None if m is None else m.group(1).strip()


def blank_telemetry(stamp):
    """Blanks Longshot's report addresses in the stock stamp (the game then has nowhere to send). Returns what was
    blanked; SETUP-FAIL when a key is still set after the write."""
    with open(stamp, encoding="utf-8") as f:
        text = f.read()
    was = [k for k in TELEMETRY if stamp_value(text, k)]
    for k in was:
        text = re.sub(r"^%s=.*$" % re.escape(k), "%s=" % k, text, flags=re.M)
    if was:
        with open(stamp, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    with open(stamp, encoding="utf-8") as f:
        left = [k for k in TELEMETRY if stamp_value(f.read(), k)]
    if left:
        raise P.SetupFail("telemetry address still set in %s: %s" % (stamp, ", ".join(left)))
    return was


def folder_processes(exe):
    """{pid: command line} of every process running exe (the game and its --crash-watch helper)."""
    q = ("Get-CimInstance Win32_Process -Filter \"Name='longshot.exe'\" | Where-Object { $_.ExecutablePath -eq '%s' } | "
         "ForEach-Object { '{0} {1}' -f $_.ProcessId, $_.CommandLine }" % exe.replace("'", "''"))
    r = subprocess.run(["powershell", "-NoProfile", "-Command", q], capture_output=True, text=True, timeout=60)
    out = {}
    for line in (r.stdout or "").splitlines():
        pid, _, cmd = line.strip().partition(" ")
        if pid.isdigit():
            out[int(pid)] = cmd
    return out


def close_game(proc, note):
    if proc.poll() is not None:
        return
    import placement
    h = placement.game_window(proc.pid)
    if h:
        ctypes.windll.user32.PostMessageW(ctypes.c_void_p(h), WM_CLOSE, 0, 0)
    try:
        proc.wait(CLOSE_S)
        note.append("game closed by WM_CLOSE")
    except subprocess.TimeoutExpired:
        proc.kill()
        note.append("game did not close in %d s: killed" % CLOSE_S)


def one_colour(path):
    from PIL import Image
    with Image.open(path) as im:
        lo, hi = im.convert("L").getextrema()
    return hi - lo < 8


@pytest.mark.needs_env("BT_DROPIN=1")
@pytest.mark.budget(480)
@pytest.mark.row("v1 P0: co-op hook on a stock Longshot game (drop-in winmm.dll)")
def test_dropin_stock_longshot(request):
    tag = pin_tag()
    game = os.path.join(os.environ.get(ROOT_ENV) or DEFAULT_ROOT, tag, "game")
    bindir = os.path.join(game, "bin")
    exe = os.path.join(bindir, "longshot.exe")
    stamp = os.path.join(game, "longshot_version.txt")
    out = os.path.join(request.config.getoption("out") or os.path.join(os.getcwd(), "ingame-out"), "dropin")
    os.makedirs(out, exist_ok=True)
    note = []

    def say(m):
        note.append(m)
        with open(os.path.join(out, "dropin-run.txt"), "a", encoding="utf-8") as f:
            f.write(time.strftime("%H:%M:%S ", time.gmtime()) + m + "\n")

    # ---- 1. set-up proof ----
    t0 = time.time()
    for p in (exe, stamp):
        if not os.path.isfile(p):
            raise P.SetupFail("no stock game file %s (build it: hookshot/dropin/proof-run.ps1 -Versions %s -BuildOnly)" % (p, tag))
    with open(stamp, encoding="utf-8") as f:
        ver = stamp_value(f.read(), "version")
    if ver != tag:
        raise P.SetupFail("%s says version=%s, hookshot/PIN says %s" % (stamp, ver, tag))
    if folder_processes(exe):
        raise P.SetupFail("a longshot.exe of %s is already running" % bindir)
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", os.path.join(DROPIN, "test.ps1")],
                       capture_output=True, text=True, timeout=180)
    with open(os.path.join(out, "dropin-test-ps1.txt"), "w", encoding="utf-8") as f:
        f.write((r.stdout or "") + (r.stderr or ""))
    if r.returncode != 0 or "TEST PASS" not in (r.stdout or ""):
        raise P.SetupFail("hookshot/dropin/test.ps1 not PASS on this tree (exit %s): see dropin-test-ps1.txt" % r.returncode)
    dll, selftest = os.path.join(DROPIN, "out", "winmm.dll"), os.path.join(DROPIN, "out", "selftest.exe")
    say("stock %s game %s; test.ps1 PASS; winmm.dll %d bytes" % (tag, game, os.path.getsize(dll)))
    was = blank_telemetry(stamp)
    say("telemetry addresses blank in the stamp%s" % (" (blanked now: %s)" % ", ".join(was) if was else ""))
    import placement
    try:
        mon = placement.check_display()
    except placement.PlacementError as e:
        raise P.SetupFail("test display: %s" % e)
    say("display %s%s" % (mon["name"], " (%s)" % mon["note"] if mon.get("note") else ""))
    say("set-up proof %.1f s" % (time.time() - t0))

    # ---- 2. offline: the stock exe's own symbols and prologues ----
    st = subprocess.run([selftest, exe], capture_output=True, text=True, timeout=120)
    say("selftest exit %d: %s" % (st.returncode, " | ".join((st.stdout or "").splitlines()[-1:])))
    assert st.returncode == 0 and "SELFTEST PASS" in (st.stdout or ""), "selftest on the stock exe: %s" % st.stdout
    if os.environ.get(SETUP_ONLY_ENV) == "1":
        say("SETUP-ONLY: set-up proof + selftest done, no game start")
        raise P.NoRef("NO-REF: %s=1 (set-up proof + selftest only, no game start)" % SETUP_ONLY_ENV)

    # ---- 3. drop in, start, our lines ----
    placed, log = os.path.join(bindir, "winmm.dll"), os.path.join(bindir, LOG)
    proc, text = None, ""
    env = dict(os.environ)
    env.pop("SQUADALA_DROPIN", None)
    try:
        shutil.copyfile(dll, placed)
        if os.path.exists(log):
            os.remove(log)
        t1 = time.time()
        proc = subprocess.Popen([exe], cwd=game, env=env, **placement.popen_flags())
        say("game pid %d" % proc.pid)

        def read():
            try:
                with open(log, encoding="utf-8", errors="replace") as f:
                    return f.read()
            except OSError:
                return ""

        def wait_for(needle, secs):
            end = time.time() + secs
            while time.time() < end and proc.poll() is None:
                if needle in read():
                    return True
                time.sleep(1)
            return needle in read()

        assert wait_for("dropin: attached", ATTACH_S), \
            "no 'dropin: attached' in %d s (exited: %s): our winmm.dll was not loaded" % (ATTACH_S, proc.poll() is not None)
        placement.place_pids([proc.pid], wait=30)
        framed = wait_for("dropin: frame 600 ", FRAME_S)
        secs = time.time() - t1
        procs = folder_processes(exe)
        text = read()
        say("frame 600 %s at %.0f s; processes %s" % ("seen" if framed else "NOT seen", secs, procs))
        assert proc.poll() is None, "the game exited by itself (exit %s)" % proc.poll()
        assert framed, "no 'dropin: frame 600' line in %d s" % FRAME_S
        assert "the game's pacibsp: hook on" in text, "guest word check: no \"hook on\" line"
        hooks = text.count("hooked at")
        assert hooks == 1, "%d 'hooked at' lines (want 1: the game only, its --crash-watch helper forwards only)" % hooks
        helpers = [pid for pid, cmd in procs.items() if "--crash-watch" in cmd]
        if helpers:
            assert "--crash-watch helper" in text, "the --crash-watch helper %s ran without its forward-only line" % helpers

        # ---- 4. one window shot (uncovered, not one colour) ----
        import hs_steps as hs
        import windowshot
        shot = os.path.join(out, "dropin-%s-game.png" % tag)
        code, why = hs.run_window_shot(hs.window_shot_command(windowshot.helper(), proc.pid, shot,
                                                              os.path.join(out, windowshot.LOG)))
        say("window shot exit %s: %s" % (code, why))
        if code != 0 or not os.path.isfile(shot):
            raise P.SetupFail("window shot not usable (exit %s: %s)" % (code, why))
        if one_colour(shot):
            raise P.SetupFail("window shot %s is one colour (no picture yet)" % shot)
    finally:
        if proc is not None:
            close_game(proc, note)
            end = time.time() + HELPER_S
            left = folder_processes(exe)
            while left and time.time() < end:
                time.sleep(1)
                left = folder_processes(exe)
            for pid in left:
                subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
                note.append("left process %d killed" % pid)
        if os.path.exists(log):
            shutil.copyfile(log, os.path.join(out, "dropin-%s-%s" % (tag, LOG)))
        glog = os.path.join(game, "logs", "longshot_log.txt")
        if os.path.exists(glog):
            shutil.copyfile(glog, os.path.join(out, "dropin-%s-longshot_log.txt" % tag))
        if os.path.exists(placed):
            os.remove(placed)
            note.append("winmm.dll removed from %s" % bindir)
        say("end: " + "; ".join(note[-4:]))
