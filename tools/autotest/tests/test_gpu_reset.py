"""Selftests of the VOID gpu-reset label (tools/autotest/ingame/gpu_reset.py + bt_plugin, B #161 2026-11-28), no game.

Both sides of B #161's guard: a driver reset event in the session window followed by the game's DEVICE_LOST -> VOID;
DEVICE_LOST with no event (or the event outside the window, or no DEVICE_LOST) -> the FAIL / SETUP-FAIL stays.
The driver events come from the fake (gpu_reset.FAKE_ENV, a JSON file), never from this PC's event log.
"""
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
sys.path.insert(0, INGAME)
sys.path.insert(0, os.path.dirname(HERE))
import gpu_reset   # noqa: E402
import bt_plugin   # noqa: E402
import rc_tests    # noqa: E402

# the real line from banner-steady-53d40ab3-2 lane1 session/game-out-A.txt:8641 (2026-11-28 00:14 UTC)
FATAL = ("[hos host] vk: FATAL: the GPU device was lost (VK_ERROR_DEVICE_LOST): the driver stopped or reset the GPU, "
         "and nothing sent to it runs any more")
T0 = 1795824581.0        # session.json 'started' of that run


class Session:
    """A live.py session folder: session.json + game-out-A/B.txt with set mtimes, and the fake event file."""

    def __init__(self, lost=("A",), log_at=T0 + 340, events=()):
        self.dir = tempfile.mkdtemp(prefix="gpu-reset-")
        games = {}
        for x in ("A", "B"):
            log = os.path.join(self.dir, "game-out-%s.txt" % x)
            with open(log, "w", encoding="utf-8") as f:
                f.write("[hos host] vk: submit 51532 failed (-4)\n%s\n" % FATAL if x in lost else "coop: hud ok\n")
            os.utime(log, (log_at, log_at))
            games[x] = {"log": log, "folder": os.path.join(self.dir, x)}
        with open(os.path.join(self.dir, "session.json"), "w", encoding="utf-8") as f:
            json.dump({"started": T0, "games": games}, f)
        self.events = os.path.join(self.dir, "events.json")
        with open(self.events, "w", encoding="utf-8") as f:
            json.dump([{"t": t, "id": i} for t, i in events], f)

    def __enter__(self):
        self._old = os.environ.get(gpu_reset.FAKE_ENV)
        os.environ[gpu_reset.FAKE_ENV] = self.events      # (scoped: only inside this with-block)
        return self

    def __exit__(self, *a):
        if self._old is None:
            os.environ.pop(gpu_reset.FAKE_ENV, None)
        else:
            os.environ[gpu_reset.FAKE_ENV] = self._old
        shutil.rmtree(self.dir, ignore_errors=True)


class Cfg:
    def __init__(self, session):
        self._bt_session = session


NOW = T0 + 400


class JudgeTest(unittest.TestCase):
    def test_reset_then_device_lost_is_void(self):
        with Session(events=[(T0 + 332, 14), (T0 + 332, 153)]) as s:
            why = gpu_reset.judge(s.dir, NOW)
        self.assertIsNotNone(why)
        self.assertTrue(why.startswith("VOID gpu-reset: System log nvlddmkm event 14"), why)
        self.assertIn("game-out-A.txt has VK_ERROR_DEVICE_LOST", why)

    def test_device_lost_without_event_stays(self):
        with Session(events=[]) as s:
            self.assertIsNone(gpu_reset.judge(s.dir, NOW))

    def test_event_without_device_lost_stays(self):
        with Session(lost=(), events=[(T0 + 332, 153)]) as s:
            self.assertIsNone(gpu_reset.judge(s.dir, NOW))

    def test_event_before_the_session_stays(self):
        with Session(events=[(T0 - 60, 153)]) as s:
            self.assertIsNone(gpu_reset.judge(s.dir, NOW))

    def test_event_after_the_device_lost_stays(self):
        # (the game lost its device first, the driver event came later: not proven to be the cause)
        with Session(log_at=T0 + 300, events=[(T0 + 360, 153)]) as s:
            self.assertIsNone(gpu_reset.judge(s.dir, NOW))

    def test_other_event_id_stays(self):
        with Session(events=[(T0 + 332, 13)]) as s:
            self.assertIsNone(gpu_reset.judge(s.dir, NOW))

    def test_no_session_stays(self):
        self.assertIsNone(gpu_reset.judge(None, NOW))
        self.assertIsNone(gpu_reset.judge(os.path.join(tempfile.gettempdir(), "no-such-session-gpu"), NOW))


class PluginTest(unittest.TestCase):
    def test_only_fail_and_setup_fail_become_void(self):
        with Session(events=[(T0 + 332, 153)]) as s:
            cfg = Cfg(s.dir)
            for kind in ("PASS", "NO-REF", "FLAKY", "SKIP", "NOT-RUN"):
                self.assertIsNone(bt_plugin._void_why(cfg, kind), kind)
            for kind in ("FAIL", "SETUP-FAIL"):
                self.assertTrue((bt_plugin._void_why(cfg, kind) or "").startswith("VOID gpu-reset"), kind)

    def test_fail_without_event_stays_fail(self):
        with Session(events=[]) as s:
            self.assertIsNone(bt_plugin._void_why(Cfg(s.dir), "FAIL"))
            self.assertIsNone(bt_plugin._void_why(Cfg(s.dir), "SETUP-FAIL"))

    def test_void_is_never_pass(self):
        rows = [{"test": "t::a", "result": "PASS", "why": ""},
                {"test": "t::b", "result": "VOID", "why": "VOID gpu-reset: x | was SETUP-FAIL: bridge refused"}]
        counts = {k: sum(1 for r in rows if r["result"] == k) for k in bt_plugin.RESULTS}
        line = bt_plugin.result_line({"tests": rows, "counts": counts, "minutes": 1.0})
        self.assertTrue(line.startswith("RESULT: VOID (PASS 1, VOID 1); first: t::b VOID gpu-reset"), line)
        self.assertIn("rerun the job once", line)
        rows.append({"test": "t::c", "result": "FAIL", "why": "assert"})
        counts = {k: sum(1 for r in rows if r["result"] == k) for k in bt_plugin.RESULTS}
        self.assertTrue(bt_plugin.result_line({"tests": rows, "counts": counts, "minutes": 1.0})
                        .startswith("RESULT: NOT PASS"))
        self.assertIn("VOID", rc_tests.NOT_PASS)
        self.assertIn("VOID", rc_tests.RESULTS)


if __name__ == "__main__":
    unittest.main()
