"""Rows 292 and 170: hosting through the game's own Multiplayer > Host (CoopPause::TestHost, the autotest's Host row).

Row 292 (player report 11-26, 0.11.12): "While hosting through the Multiplayer menu, I get 1% low frametimes above 150ms
in 30 second intervals ... My frametime spikes end when i stop hosting." Cause (coord reports/playerbug-292.md): the
host's LAN announce re-read the network adapters ON THE FRAME every 30 s (CoopLanNet::Announce -> ReadNetworks,
kNetworksEveryMs = 30000); fix playerbug-292 moved that read off the frame (CoopLan::NetworkTargets on a
CoopOffFrame::Slot). The player's log shows the stall as one present "over 40" in every 6th 5 s window.

| Check | Source |
|---|---|
| A hosts by its own menu, co-op off at its start | the boot live-kakariko-host (bt_plugin HOST_IN_GAME_BOOTS: live.py start --host-in-game, no --server); A log TEST_HOST_ON + LISTENING |
| B joins on 127.0.0.1 (B #138: not by the LAN list; that path is test_lan251's) | B log WELCOMED on 127.0.0.1:<port> |
| the announce runs (the 30 s read is the announce's) | A log ANNOUNCED (hookshot/src/coop_lan_net.cpp:115) |
| no 30 s spikes while hosting | Longshot's 5 s windows on A (test_unfocus WINDOW/BUCKETS, ls31-src host/nn_vi.c:704-705), WINDOWS full windows = 65 s, two reads |

The pass line is the PORT AUTHOR'S DECISION (regress-port-a-3), not the player's: the player's signature is >= 2 of 13
windows with an over-40 present (one in every 6th); here at most MAX_SPIKY windows of A may have one, and A's mean
fps stays >= 90 % of the 60 cap (test_unfocus MIN_SHARE). On a PC whose adapter read is 2-4 ms (playerbug-292 bench) the
old code does not spike either: this test proves the hosting path runs clean and the announce is on, not the
player's ~850 ms read; the fix's own proof is tests/hookshot/off_frame_test.cpp.

Row 170 (the related case of proof-rows-0120-2, testhost-170): a game whose server line names no server running holds
co-op that came with the game's start but is in no game; the player's Host row ends that co-op and hosts (HostNow), and
TestHost must do the same (it checked g_coop_on: refused). On the default boot: B starts again with
LICHII_COOP_SERVER=127.0.0.1:<a free port> and LICHII_COOP_TEST_HOST=<another free port>:<password>, `menu host`,
then A starts again on B's host and is welcomed there; the session is put back after. Opt-in: --env OOT_TEST_ROW170=1.
Needs a module with testhost-170 (715803341); before it, TestHost refuses and the test FAILS at TEST_HOST_ON (the job's
--expect-commit must contain 715803341).
Only the Host path is driven. The Join rows on a dead line (JoinPasteNow / JoinLanNow / JoinRoomNow / RejoinNow ->
Join() EndLauncher) share the same LauncherLive rule but are NOT proven in game here.
The player-facing part, B's Multiplayer page after the host (the Session view hosting, not 'Co-op is on'), is a bridge
shot of B: `menu open main` (testhost-170 d06ef04bb, C #145: co-op's Multiplayer page where a player has it, in play the
- menu's Multiplayer tab; presses nothing), state.menu.minus view 'main' (test_menu_pages.page_shown),
check_shot B 'row170-host-page' (NO-REF until a ref is stored), then shut with B (test_menu_pages.close_menu).
--expect-commit must contain d06ef04bb.
"""
import json
import os
import re
import socket
import time

import pytest

from pair import SetupFail
from test_menu_pages import close_menu, open_page
from test_rows import need_child_in_play
from test_unfocus import CAP_FPS, MIN_SHARE, POLL_S, windows

AUTOTEST = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT_A = os.path.join(AUTOTEST, "scenarios", "live-kakariko-child.A.txt")   # the lane's default boot (bt_plugin DEFAULT_BOOT)
BOOT_B = os.path.join(AUTOTEST, "scenarios", "live-kakariko-child.B.txt")
HOST_BOOT = "live-kakariko-host"                                            # bt_plugin HOST_IN_GAME_BOOTS
OPT_IN_170 = "OOT_TEST_ROW170=1"   # (opt-in, as test_lan251/test_rejoin: 4 rejoins; the fakes have no live.py session)

TEST_HOST_ON = r"coop: multiplayer tab: test host on port %d "    # coop_pause.cpp:2586 (:2588 with testhost-170)
LISTENING = r"coop: host: test host \(LICHII_COOP_TEST_HOST\): listening on 127\.0\.0\.1 only"   # coop_host_menu.cpp:404
NOT_HOSTING = r"coop: multiplayer tab: test host: .*not hosting"   # coop_pause.cpp:2554/2560; testhost-170 :2562
                   # ("... came with the game's start and is in a game: not hosting")
START_ENDED = r"coop: multiplayer tab: co-op that came with the game's start ended"   # coop_pause.cpp:213
ANNOUNCED = r"coop: lan: this game is announced on the local network"             # coop_lan_net.cpp:115
WELCOMED = r"coop: welcomed as player \d+ on "                                    # lichii/hook/coop_hook.cpp:2779
NO_CRASH = r"CRASH: exception|unhandled exception|fault at"                      # test_lan251 NO_CRASH
VCORES = re.compile(r"vcores: .*waited (\d+) ms for a core")                     # the player's stall line (playerbug-292.md)

WINDOWS = 13          # full 5 s windows of A judged: 65 s, two of the 30 s network reads (kNetworksEveryMs)
MAX_SPIKY = 1         # windows of A with an over-40 present allowed (the player's signature: >= 2 of 13)


def session_runner(pair):
    with open(os.path.join(pair.session, "session.json"), encoding="utf-8") as f:
        return json.load(f).get("runner") or {}


def judge_spikes(win, vcore_lines=()):
    """(bad, note) for A's full windows `win` ([(fps, s, buckets)], buckets[5] = presents over 40 ms)."""
    spiky = [i for i, (_, _, b) in enumerate(win) if b[5] > 0]
    mean = sum(f for f, _, _ in win) / max(1, len(win))
    waits = [int(m.group(1)) for m in map(VCORES.search, vcore_lines) if m]
    note = "A hosting: %d windows, fps mean %.1f, windows with an over-40 present %s, vcores waits max %s ms" % (
        len(win), mean, spiky or "none", max(waits) if waits else "none")
    bad = []
    if len(spiky) > MAX_SPIKY:
        bad.append("%s: more than %d spiky windows (the player's 30 s signature)" % (note, MAX_SPIKY))
    if mean < MIN_SHARE * CAP_FPS:
        bad.append("%s: mean < %.0f fps" % (note, MIN_SHARE * CAP_FPS))
    return bad, note


@pytest.mark.needs_boot(HOST_BOOT)
@pytest.mark.budget(240)
@pytest.mark.row("#292: no frame spikes every 30 s while hosting through the Multiplayer menu (playerbug-292)")
def test_host_menu_no_spikes(pair):
    with pair.setup():
        need_child_in_play(pair)
        port = int(session_runner(pair).get("port") or 0)
        if not port:
            raise SetupFail("292: no port in the session's runner")
        for x, rx in (("A", TEST_HOST_ON % port), ("A", LISTENING), ("A", ANNOUNCED),
                      ("B", WELCOMED + re.escape("127.0.0.1:%d" % port))):
            if not any(re.search(rx, ln) for ln in pair.log[x].since(0)):
                raise SetupFail("292: the host-in-game boot gave no %s log /%s/" % (x, rx))
        mark = pair.log["A"].mark()
    for x in ("A", "B"):
        pair.call(x, "resume")
    got = []
    try:
        while True:
            got = windows(pair.log["A"].since(mark))[1:]       # [1:]: the partial first one
            if len(got) >= WINDOWS:
                break
            pair.deadline.check("%d full vi windows of A (have %d)" % (WINDOWS, len(got)))
            time.sleep(POLL_S)
    finally:
        for x in ("A", "B"):
            pair.call(x, "pause")
    bad, note = judge_spikes(got[:WINDOWS], pair.log["A"].since(mark))
    pair.notes.append(note)
    for x in ("A", "B"):
        pair.never_log(x, NO_CRASH)
    assert not bad, "; ".join(bad)


def free_port(taken=()):
    """A UDP port free on 127.0.0.1 now (bound to port 0, read, closed), not one of `taken`."""
    for _ in range(20):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind(("127.0.0.1", 0))
            p = s.getsockname()[1]
        finally:
            s.close()
        if p not in taken:
            return p
    raise SetupFail("170: no free UDP port on 127.0.0.1")


def run_170(pair, dead, host, password, boot_a=BOOT_A, boot_b=BOOT_B):
    """The step itself, after the set-up (the selftests drive it with a fake live.py)."""
    pair.rejoin("B", boot=boot_b, env=["LICHII_COOP_SERVER=127.0.0.1:%d" % dead,
                                       "LICHII_COOP_TEST_HOST=%d:%s" % (host, password)])
    pair.until("170: B in play on its dead server line", lambda: pair.ready("B"), max_frames=1800)
    pair.never_log("B", WELCOMED)                     # (no server runs on that port)
    pair.act("B", "menu host")
    pair.expect_log("B", TEST_HOST_ON % host, max_frames=600)
    pair.expect_log("B", LISTENING, max_frames=600)
    pair.expect_log("B", START_ENDED, max_frames=600)
    pair.never_log("B", NOT_HOSTING)
    # (B's Multiplayer page opened, nothing pressed, where a player has it now: in play, the - menu's Multiplayer tab,
    # the game's own page, so the framebuffer shot has it; test_menu_pages.open_page, F1's rule, C #145)
    home, _ = open_page(pair, "B", "main")
    assert home == "minus", "170: B's Multiplayer page showed in %s, not the - menu (B is in play)" % home
    pair.check_shot("B", "row170-host-page")
    close_menu(pair, "B")

    pair.rejoin("A", boot=boot_a, env=["LICHII_COOP_SERVER=127.0.0.1:%d" % host, "LICHII_COOP_PASSWORD=%s" % password])
    pair.expect_log("A", WELCOMED + re.escape("127.0.0.1:%d" % host), max_frames=900)
    for x in ("A", "B"):
        pair.never_log(x, NO_CRASH)

    # the session back: both games on its own server and env
    pair.rejoin("B", boot=boot_b)
    pair.rejoin("A", boot=boot_a)
    for x in ("A", "B"):
        pair.expect_log(x, WELCOMED, max_frames=900)
    pair.until("back: both in play", lambda: all(pair.ready(x) for x in ("A", "B")), max_frames=1800)
    pair.notes.append("170: B's dead line 127.0.0.1:%d, B hosted on %d, A welcomed there; session back" % (dead, host))


@pytest.mark.needs_env(OPT_IN_170)
@pytest.mark.look
@pytest.mark.budget(2400)   # 4 rejoins (<= 60 s quit + <= 420 s boot each, Pair.REJOIN_*) + the waits
@pytest.mark.row("#170 related: TestHost hosts on a server line naming no server, as the Host row (testhost-170)")
def test_testhost_on_a_dead_server_line(pair):
    with pair.setup():
        need_child_in_play(pair)
        if not (pair.session and pair.live):
            raise SetupFail("170: no live.py session (Pair.session / Pair.live): rejoins need one")
        r = session_runner(pair)
        if not r.get("server") or not r.get("port") or r.get("host_in_game"):
            raise SetupFail("170: the default boot (a coop_server session) is needed (runner %s)" % r)
        dead = free_port({int(r["port"])})
        host = free_port({int(r["port"]), dead})
        password = r.get("password") or "autotest-pass"
    run_170(pair, dead, host, password)
