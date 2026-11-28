"""Selftests of rows 292 / 170 (tools/autotest/ingame/test_host_menu.py) and the host-in-game boot (live.py start
--host-in-game, bt_plugin HOST_IN_GAME_BOOTS): no game, a fake live.py + bridge."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
AUTOTEST = os.path.dirname(HERE)
INGAME = os.path.join(AUTOTEST, "ingame")
sys.path.insert(0, INGAME)
sys.path.insert(0, AUTOTEST)
import pair as P          # noqa: E402
from test_ingame_rejoin import FakeClient   # noqa: E402


def win(over40=0, fps=59.9):
    return (fps, 5.0, [0, 299, 0, 0, 0, over40])


class Spikes(unittest.TestCase):
    def setUp(self):
        import test_host_menu as T
        self.T = T

    def test_clean_windows_pass(self):
        bad, note = self.T.judge_spikes([win() for _ in range(13)])
        self.assertEqual(bad, [])
        self.assertIn("13 windows", note)

    def test_one_spiky_window_is_allowed(self):
        bad, _ = self.T.judge_spikes([win(1 if i == 4 else 0) for i in range(13)])
        self.assertEqual(bad, [])

    def test_the_players_30s_signature_fails(self):   # one over-40 present in every 6th window
        bad, _ = self.T.judge_spikes([win(1 if i % 6 == 5 else 0) for i in range(13)])
        self.assertTrue(bad and "spiky" in bad[0])

    def test_low_fps_fails(self):
        bad, _ = self.T.judge_spikes([win(fps=50.0) for _ in range(13)])
        self.assertTrue(bad and "mean" in bad[0])

    def test_vcores_wait_is_noted(self):
        _, note = self.T.judge_spikes([win()], ["x vcores: PreActorModelCompose waited 812 ms for a core: given by y"])
        self.assertIn("max 812.0 ms", note)

    def test_vcores_wait_in_tenths_is_noted(self):
        # (Longshot writes tenths: "waited 96.4 ms"; the whole-number pattern missed every line, gate-hostmenu-309d7180)
        lines = ["[hos nn.npln.Shared] vcores: nn.npln.Shared (priority 16) waited 96.4 ms for a core: given by y",
                 "[hos x] vcores: x (priority 26) waited 62.3 ms for a core: given by z"]
        _, note = self.T.judge_spikes([win()], lines)
        self.assertIn("max 96.4 ms", note)

    def test_settle_drops_the_partial_window_and_the_holds_end(self):
        self.assertEqual(self.T.SETTLE, 2)

    def test_test_host_line_matches_the_module_text(self):
        import re
        self.assertTrue(re.search(self.T.TEST_HOST_ON % 27531, "[hos] coop: multiplayer tab: test host on port 27531 "
                                  "(Multiplayer > Host, as the page's Host row)"))
        self.assertFalse(re.search(self.T.TEST_HOST_ON % 2753, "coop: multiplayer tab: test host on port 27531 (M"))
        self.assertTrue(re.search(self.T.LISTENING, "coop: host: test host (LICHII_COOP_TEST_HOST): listening on "
                                  "127.0.0.1 only"))

    def _hosting(self, bound, welcomed):
        """prove_hosting on a fake pair whose session port is 27520 (the lines of fps-spike-182cd53b game-out-A/B)."""
        import json
        d = tempfile.mkdtemp()
        with open(os.path.join(d, "session.json"), "w", encoding="utf-8") as f:
            json.dump({"runner": {"port": 27520, "host_in_game": True}}, f)
        logs = {"A": ["[hos x] coop: multiplayer tab: test host on port 27520 (Multiplayer > Host, as the page's Host row)",
                      "[hos host] coop: host: test host (LICHII_COOP_TEST_HOST): listening on 127.0.0.1 only",
                      "[hos host] coop: host: listening on port %d (only 127.0.0.1, datagrams of 1100 bytes)" % bound,
                      "[hos x] coop: lan: this game is announced on the local network (port 27508, every 2 s)"],
                "B": ["[hos host] coop: welcomed as player 1 on 127.0.0.1:%d" % welcomed]}

        class Log:
            def __init__(self, lines):
                self.lines = lines

            def since(self, _):
                return self.lines

        class Pair:
            session = d
            log = {x: Log(v) for x, v in logs.items()}
        return self.T.prove_hosting(Pair(), "t")

    def test_prove_hosting_passes_one_session(self):
        self.assertEqual(self._hosting(27520, 27520), 27520)

    def test_prove_hosting_fails_when_the_host_bound_another_port(self):
        # fps-spike-182cd53b (review-rows-c130-12): an orphan server held :27520, A's host bound :27521, B joined the orphan
        with self.assertRaises(P.SetupFail) as e:
            self._hosting(27521, 27520)
        self.assertIn("listening on port 27520", str(e.exception))

    def test_free_port_skips_taken(self):
        p = self.T.free_port()
        self.assertNotEqual(self.T.free_port({p}), p)


class Run170(unittest.TestCase):
    def setUp(self):
        import test_host_menu as T
        self.T = T
        FakeClient.made = []
        self.tmp = tempfile.mkdtemp()
        self.logs = {x: os.path.join(self.tmp, "game-out-%s.txt" % x) for x in "AB"}
        for p in self.logs.values():
            with open(p, "w") as f:
                f.write("old line\n")
        games = {x: {"port": 40000 + i, "folder": self.tmp, "log": self.logs[x]} for i, x in enumerate("AB")}
        self.pair = P.Pair(games, client=FakeClient)
        self.pair.session = self.tmp
        self.pair.in_play = lambda st: True
        # (in play, F1's rule: `menu open main` shows the - menu's Multiplayer tab, main view; B shuts it: C #145)
        self.minus = {"A": None, "B": None}
        self.pair.state = lambda x: {"menu": {"coop": {"open": False},
                                              "minus": ({"open": True, "tab": "multiplayer", "view": self.minus[x]}
                                                        if self.minus[x] else {"open": False})}}
        acted = self.pair.act

        def act(x, line):
            r = acted(x, line)
            if line == "menu open main":
                self.minus[x] = "main"
                with open(self.logs[x], "a") as f:
                    f.write("coop: test menu open main: asked\n")
            return r
        self.pair.act = act
        self.pressed = []

        def press(x, buttons=(), stick=None, frames=8, rstick=None):
            self.pressed.append((x, list(buttons)))
            if "B" in buttons:
                self.minus[x] = None
        self.pair.press = press
        self.shots = []
        self.pair.check_shot = lambda x, name: self.shots.append((x, name))
        self.asked = []

    def fake_live(self, refuse=False):
        def run(args, timeout):
            self.asked.append(args)
            if args[0] == "quit":
                return {"player": args[3], "how": "quit asked"}
            x = args[3]
            env = dict(a[len("--env="):].split("=", 1) for a in args if a.startswith("--env="))
            lines = []
            if "LICHII_COOP_TEST_HOST" in env:      # B on a dead line: no welcome; its `menu host` answer comes below
                port = int(env["LICHII_COOP_TEST_HOST"].split(":")[0])
                if refuse:
                    lines.append("coop: multiplayer tab: test host: co-op came with the game's start: not hosting")
                else:
                    lines += ["coop: multiplayer tab: test host on port %d (Multiplayer > Host, as the page's Host row)" % port,
                              "coop: multiplayer tab: co-op that came with the game's start ended: hosting",
                              "coop: host: test host (LICHII_COOP_TEST_HOST): listening on 127.0.0.1 only"]
            else:
                lines.append("coop: welcomed as player 1 on %s" % env.get("LICHII_COOP_SERVER", "127.0.0.1:27520"))
            os.replace(self.logs[x], self.logs[x] + ".%d" % len(self.asked))
            with open(self.logs[x], "w") as f:
                f.write("\n".join(lines) + "\n")
            return {"ok": True, "boot": "played"}
        return run

    def go(self, refuse=False):
        self.pair.live = self.fake_live(refuse)
        self.pair.begin(2400)
        self.T.run_170(self.pair, 50001, 50002, "autotest-pass", boot_a="a.txt", boot_b="b.txt")

    def test_pass_flow(self):
        self.go()
        relaunch = [a for a in self.asked if a[0] == "relaunch"]
        self.assertEqual([a[3] for a in relaunch], ["B", "A", "B", "A"])
        self.assertIn("--env=LICHII_COOP_SERVER=127.0.0.1:50001", relaunch[0])
        self.assertIn("--env=LICHII_COOP_TEST_HOST=50002:autotest-pass", relaunch[0])
        self.assertIn("--env=LICHII_COOP_SERVER=127.0.0.1:50002", relaunch[1])
        self.assertFalse([a for a in relaunch[2] + relaunch[3] if a.startswith("--env")])   # (the session back)
        self.assertIn("act", sum((c.calls for c in FakeClient.made), []))   # (B's `menu host`)
        self.assertEqual(self.shots, [("B", "row170-host-page")])
        self.assertEqual(self.pressed, [("B", ["B"])])            # (the - menu shut as a player shuts it)
        self.assertIsNone(self.minus["B"])

    def test_page_in_longshots_menu_in_play_fails(self):
        """The f9f3b9c26 gate's path: in play the page wanted in Longshot's tab (a second home): FAIL, never PASS."""
        acted = self.pair.act

        def act(x, line):
            r = acted(x, line)
            if line == "menu open main":
                with open(self.logs[x], "a") as f:
                    f.write("coop: test menu open main: asked\n")
            return r
        self.pair.act = act
        self.pair.state = lambda x: {"menu": {"coop": {"open": True, "tab": "multiplayer", "page": "main"},
                                              "minus": {"open": False}}}
        with self.assertRaises(AssertionError) as e:
            self.go()
        self.assertIn("main page in the - menu", str(e.exception))
        self.assertEqual(self.shots, [])

    def test_testhost_refusal_fails(self):
        with self.assertRaises(AssertionError) as e:
            self.go(refuse=True)
        self.assertIn("test host on port 50002", str(e.exception))


class HostInGameBoot(unittest.TestCase):
    def test_live_runner_argv_host_in_game_no_server(self):
        import live

        class A:
            server, host_in_game, no_restore, boot_a, boot_b, no_load, load, playtest, env, display = (
                None, True, False, "a.txt", "b.txt", True, None, False, [], None)
            lane, boot_timeout = 1, 900
        argv = live.runner_argv(A, {"A": "ga", "B": "gb"}, 27520, "p", "D")
        self.assertIn("--host-in-game", argv)
        self.assertNotIn("--server", argv)

    def test_live_relaunch_refused_in_a_host_in_game_session(self):
        import live
        orig = live.open_session
        try:
            live.open_session = lambda d: {"runner": {"host_in_game": True}, "games": {"A": {}, "B": {}}}

            class A:
                session, player, boot, no_load, restore, env = tempfile.mkdtemp(), "B", None, False, False, []
            r = live.relaunch(A, alive=lambda pid: False, runner=lambda argv: 0)
        finally:
            live.open_session = orig
        self.assertIn("not supported", r.get("error", ""))

    def test_hookshot_accepts_the_host_boot_scripts(self):
        import hookshot
        scen = os.path.join(AUTOTEST, "scenarios")
        a, b = (os.path.join(scen, "live-kakariko-host.%s.txt" % x) for x in "AB")
        for path in (a, b):
            hookshot.parse(hookshot.read_script(path))      # (raises on a bad line)
        self.assertEqual(hookshot.main(["run", "--game-a", "ga", "--game-b", "gb", "--host-in-game", "--port", "27520",
                                        "--out", tempfile.mkdtemp(), "--script-a", a, "--script-b", b, "--no-load",
                                        "--check-args"]), 0)

    def test_bt_plugin_boot_args_per_boot(self):
        import bt_plugin

        class C:
            def __init__(self, boot):
                self.o = {"boot": boot, "env": [], "session_minutes": 240}

            def getoption(self, k):
                return self.o[k]
        host = bt_plugin.boot_args(C("live-kakariko-host"), 2, "S.exe", "D")
        self.assertIn("--host-in-game", host)
        self.assertNotIn("--server", host)
        self.assertTrue(host[host.index("--boot-a") + 1].endswith("live-kakariko-host.A.txt"))
        plain = bt_plugin.boot_args(C(bt_plugin.DEFAULT_BOOT), 2, "S.exe", "D")
        self.assertEqual(plain[plain.index("--server") + 1], "S.exe")
        self.assertNotIn("--host-in-game", plain)


if __name__ == "__main__":
    unittest.main()
