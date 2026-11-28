"""Regress step 2 (row 251): a player on this network joins without the join password; loopback still asks for it.

This replaces coord-multiplayer/offline/regress-01113-steps/s2-lan251.ps1 (scenario tools/autotest/scenarios/
lan-nopw.A.txt), the regress-vs-0.11.12 step "own network, no password", on the lane's live session:

| Old check (s2-lan251.ps1) | Here |
|---|---|
| a run with coop_server --same-network-open, both games on <LAN-IP>:27520 with LICHII_COOP_PASSWORD= (empty) | Pair.server_restart("--same-network-open", "--bind", <LAN IP>) (hookshot/hs.py server_bind binds 127.0.0.1 unless the args name a --bind: lan-nopw.A.txt:6), then Pair.rejoin of A and B with LICHII_COOP_SERVER=<LAN IP>:<session port>, LICHII_COOP_PASSWORD= |
| LAN IP: the first Preferred private IPv4 (s2-lan251.ps1:59) | lan_ip(): this PC's IPv4 addresses in the server's same-network ranges (coop/protocol/src/join_password.cpp:187-190), link-local 169.254 left out as the old script did |
| R1 server-out "a player on this network joins without the join password" x2, no "reason 6" | the same in the restarted server's server-out-N.txt (server/session.cpp:294, :3780) |
| R1 each game "coop: welcomed as player" | the same, and "on <LAN IP>:" in it (lichii/hook/coop_hook.cpp:2779 logs the server it joined) |
| R1 shots joined/joined-later show both players | check_shot "lan251-joined" on A and B (NO-REF until the owner approves a ref) |
| R2 B on 127.0.0.1, empty password: server "refused a connection, reason 6: wrong join password", B "refused: wrong join password", A still welcomed | a server with --same-network-open on its default 127.0.0.1 bind (one --bind only: server/options.cpp:119-126, so the LAN-bound R1 server could not be reached at 127.0.0.1), A rejoins on the session's own line and password, B with LICHII_COOP_PASSWORD= only: the same three lines (server/session.cpp:305 + :3780, RefuseReason::bad_password = 6 coop/protocol/include/coop/protocol/messages.hpp:315; coop_hook.cpp:2784-2786 + coop/net/src/client.cpp:151-152) and no same-network join |
| no "CRASH: exception" | never_log of the module's crash lines on both games |
| by hand: Co-op settings row 'Password on this network' = 'Not needed' | NOT here: state.menu rows give names only, never values (docs/playtest.md:228-230); a note |

Afterwards the session is put back: a server restart without the extra args, A and B rejoin on the session's own env.

The one test that opens a listener on a LAN address (worker rules: test listeners bind 127.0.0.1): it runs only on
purpose, with --env OOT_TEST_LAN251=1 (needs_env; a run without it deselects it), e.g. on lane 2:
  pytest tools/autotest/ingame --lanes 2 -k test_lan_join_without_password --env OOT_TEST_LAN251=1 ...
"""
import json
import os
import re
import socket

import pytest

from pair import SetupFail
from test_rows import need_child_in_play

AUTOTEST = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT_A = os.path.join(AUTOTEST, "scenarios", "live-kakariko-child.A.txt")   # the lane's default boot (bt_plugin DEFAULT_BOOT)
BOOT_B = os.path.join(AUTOTEST, "scenarios", "live-kakariko-child.B.txt")
OPT_IN = "OOT_TEST_LAN251=1"

SAME_NET = r"a player on this network joins without the join password"          # server/session.cpp:294
REASON6 = r"refused a connection, reason 6\b"                                    # server/session.cpp:3780
REASON6_PW = r"refused a connection, reason 6: wrong join password"              # session.cpp:305 + :3780
WELCOMED = r"coop: welcomed as player \d+ on "                                   # lichii/hook/coop_hook.cpp:2779
REFUSED_PW = r"coop: server refused: wrong join password"                        # coop_hook.cpp:2784-2786, client.cpp:152
NO_CRASH = r"CRASH: exception|unhandled exception|fault at"                      # bt_plugin.py CRASH_LINE + test_rejoin


def same_network(ip):
    """The server's rule for a private source address (coop/protocol/src/join_password.cpp:187-190), without the
    link-local 169.254/16 (s2-lan251.ps1:59 took only 10/8, 172.16/12, 192.168/16)."""
    try:
        a, b = [int(p) for p in ip.split(".")[:2]]
    except ValueError:
        return False
    return a == 10 or (a == 172 and (b & 0xF0) == 16) or (a == 192 and b == 168)


def lan_ip(addresses=None):
    """This PC's first private IPv4 address (sorted, so the same each call); None if it has none."""
    if addresses is None:
        try:
            addresses = sorted({i[4][0] for i in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)})
        except OSError:
            addresses = []
    return next((ip for ip in addresses if same_network(ip)), None)


def count(log, pattern):
    rx = re.compile(pattern)
    return sum(1 for ln in log.since(0) if rx.search(ln))


def session_runner(pair):
    with open(os.path.join(pair.session, "session.json"), encoding="utf-8") as f:
        return json.load(f).get("runner") or {}


def run_lan251(pair, ip, port, boot_a=BOOT_A, boot_b=BOOT_B):
    """The step itself, after the set-up (the selftests drive it with a fake live.py). R1, R2, then the session back."""
    lan = ["LICHII_COOP_SERVER=%s:%d" % (ip, port), "LICHII_COOP_PASSWORD="]

    # R1: a server open to this network, at the LAN address; both games on it with an empty password
    r1 = pair.server_restart(["--same-network-open", "--bind", ip])
    pair.rejoin("A", boot=boot_a, env=lan)
    pair.rejoin("B", boot=boot_b, env=lan)
    srv = pair.server_log(r1["out"])
    pair.until("R1: two same-network joins in %s" % r1["out"], lambda: count(srv, SAME_NET) >= 2, max_frames=1800)
    for x in ("A", "B"):
        pair.expect_log(x, WELCOMED + re.escape("%s:%d" % (ip, port)), max_frames=600)
    assert count(srv, REASON6) == 0, "R1: the LAN server refused a password: %s" % srv.find(REASON6, 0)
    pair.until("R1: both in play", lambda: all(pair.ready(x) for x in ("A", "B")), max_frames=1800)
    for x in ("A", "B"):
        pair.check_shot(x, "lan251-joined")

    # R2: loopback is never this network (the relay and the join helper hand internet players in there)
    r2 = pair.server_restart(["--same-network-open"])
    pair.rejoin("A", boot=boot_a)                                   # the session's own line and password
    pair.rejoin("B", env=["LICHII_COOP_PASSWORD="])                 # 127.0.0.1, no password: refused, plays alone
    srv = pair.server_log(r2["out"])
    pair.until("R2: reason 6 in %s" % r2["out"], lambda: count(srv, REASON6_PW) >= 1, max_frames=1800)
    pair.expect_log("B", REFUSED_PW, max_frames=600)
    pair.expect_log("A", WELCOMED, max_frames=600)
    assert count(srv, SAME_NET) == 0, "R2: loopback joined as this network: %s" % srv.find(SAME_NET, 0)
    for x in ("A", "B"):
        pair.never_log(x, NO_CRASH)

    # the session back: its own server, both games on its own env
    r3 = pair.server_restart()
    pair.rejoin("A", boot=boot_a)
    pair.rejoin("B", boot=boot_b)
    for x in ("A", "B"):
        pair.expect_log(x, WELCOMED, max_frames=900)
    pair.until("back: both in play", lambda: all(pair.ready(x) for x in ("A", "B")), max_frames=1800)
    pair.notes.append("lan251 on %s:%d: R1 %s, R2 %s, back %s; settings row 'Password on this network' not read "
                      "(state.menu gives row names only)" % (ip, port, r1["out"], r2["out"], r3.get("out")))


@pytest.mark.row("regress step 2 / #251: own network joins without the join password, loopback asks (lan-nopw)")
@pytest.mark.needs_env(OPT_IN)
@pytest.mark.look
@pytest.mark.budget(3000)   # 3 server restarts (<= 30 s each) + 6 rejoins (<= 60 s quit + <= 420 s boot each, Pair.REJOIN_*)
def test_lan_join_without_password(pair):
    with pair.setup():
        need_child_in_play(pair)
        if not (pair.session and pair.live):
            raise SetupFail("lan251: no live.py session (Pair.session / Pair.live): server restarts need one")
        r = session_runner(pair)
        if not r.get("server") or not r.get("port"):
            raise SetupFail("lan251: the session started no coop_server (runner server/port %s/%s)"
                            % (r.get("server"), r.get("port")))
        ip = lan_ip()
        if not ip:
            raise SetupFail("lan251: no private IPv4 address on this PC (join_password.cpp:187 ranges)")
        pair.notes.append("LAN address %s (server %s, port %s)" % (ip, r["server"], r["port"]))
    run_lan251(pair, ip, int(r["port"]))
