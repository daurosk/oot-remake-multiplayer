"""Tests for hs.py's no-window helpers (run_quiet, popen_quiet, no_window) and for the places that use them: a console
program started from a process with no console of its own gets a NEW visible console window, so the lane runs flashed
windows in a chain. No game, no build.
    python tools/hookshot/tests/test_no_window.py -v"""
import os
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(TOOLS, "hookshot"))
sys.path.insert(0, os.path.join(TOOLS, "autotest"))
import hs  # noqa: E402

DETACHED_PROCESS = 0x00000008
NEW_GROUP = 0x00000200


class NoWindowFlags(unittest.TestCase):
    def test_value_is_create_no_window(self):
        self.assertEqual(hs.CREATE_NO_WINDOW, 0x08000000)

    def test_windows_adds_the_flag_and_keeps_the_rest(self):
        with mock.patch.object(hs, "WINDOWS", True):
            kw = hs.no_window({"cwd": "x", "creationflags": NEW_GROUP})
        self.assertEqual(kw["cwd"], "x")
        self.assertEqual(kw["creationflags"], NEW_GROUP | hs.CREATE_NO_WINDOW)

    def test_windows_without_other_flags(self):
        with mock.patch.object(hs, "WINDOWS", True):
            self.assertEqual(hs.no_window(), {"creationflags": hs.CREATE_NO_WINDOW})

    def test_other_systems_change_nothing(self):
        with mock.patch.object(hs, "WINDOWS", False):
            self.assertEqual(hs.no_window({"cwd": "x"}), {"cwd": "x"})
            self.assertEqual(hs.no_window(), {})

    def test_does_not_change_the_callers_dict(self):
        mine = {"cwd": "x"}
        with mock.patch.object(hs, "WINDOWS", True):
            hs.no_window(mine)
        self.assertEqual(mine, {"cwd": "x"})

    def test_run_quiet_passes_the_flag_to_subprocess_run(self):
        seen = {}
        with mock.patch.object(hs, "WINDOWS", True), \
                mock.patch.object(subprocess, "run", lambda cmd, **k: seen.update(cmd=cmd, **k)):
            hs.run_quiet(["tasklist"], capture_output=True)
        self.assertEqual(seen["cmd"], ["tasklist"])
        self.assertTrue(seen["capture_output"])
        self.assertEqual(seen["creationflags"], hs.CREATE_NO_WINDOW)

    def test_popen_quiet_passes_the_flag_to_subprocess_popen(self):
        seen = {}
        with mock.patch.object(hs, "WINDOWS", True), \
                mock.patch.object(subprocess, "Popen", lambda cmd, **k: seen.update(cmd=cmd, **k)):
            hs.popen_quiet(["coop_server"], stdout=None)
        self.assertEqual(seen["cmd"], ["coop_server"])
        self.assertEqual(seen["creationflags"], hs.CREATE_NO_WINDOW)

    def test_run_quiet_really_runs(self):
        r = hs.run_quiet([sys.executable, "-I", "-c", "print(7)"], capture_output=True, text=True)
        self.assertEqual((r.returncode, r.stdout.strip()), (0, "7"))

    def test_the_default_run_of_the_code_mods_checks_is_quiet(self):
        import inspect
        for fn in (hs.code_mods_line, hs.code_mods_mismatch, hs.pairs_code_mods):
            self.assertIs(inspect.signature(fn).parameters["run"].default, hs.run_quiet, fn.__name__)


class WatchdogFlags(unittest.TestCase):
    """The lane watchdog outlives the shell. DETACHED_PROCESS (no console) made every console program it started open a
    new visible window; CREATE_NO_WINDOW gives it a hidden console they share."""

    def test_watchdog_has_no_window_and_is_not_detached(self):
        import live
        seen = {}
        with tempfile.TemporaryDirectory() as d, \
                mock.patch.object(live.subprocess, "Popen", lambda cmd, **k: seen.update(cmd=cmd, **k)):
            live.spawn_watchdog(d)
        if os.name == "nt":
            flags = seen["creationflags"]
            self.assertTrue(flags & hs.CREATE_NO_WINDOW)
            self.assertTrue(flags & NEW_GROUP)
            self.assertFalse(flags & DETACHED_PROCESS)
        else:
            self.assertTrue(seen["start_new_session"])


class NoRawConsoleChildren(unittest.TestCase):
    """Every subprocess call in tools/autotest and tools/hookshot goes through run_quiet or popen_quiet, except the ones
    that start a game (its window is its own) and the watchdog (flags set by hand, tested above)."""
    GAME_OR_FLAGGED = {
        ("autotest/hookshot.py", "game"),
        ("autotest/hs_steps.py", "game"),
        ("autotest/live.py", "watchdog"),
        ("autotest/placement.py", "launch"),
    }

    def test_only_the_known_raw_calls_remain(self):
        raw = re.compile(r"subprocess\.(run|Popen|check_output|check_call|call)\(")
        found = set()
        for sub in ("autotest", "hookshot"):
            base = os.path.join(TOOLS, sub)
            for name in sorted(os.listdir(base)):
                if not name.endswith(".py") or name.startswith("test_") or name.endswith("_test.py"):
                    continue
                rel = "%s/%s" % (sub, name)
                if rel == "hookshot/hs.py":
                    continue   # (the helpers themselves, and warm_start's game launch through its popen parameter)
                with open(os.path.join(base, name), encoding="utf-8") as f:
                    for line in f:
                        if raw.search(line) and not line.lstrip().startswith(("#", '"""')):
                            found.add((rel, line.strip()))
        files = {rel for rel, _ in found}
        self.assertEqual(files, {rel for rel, _ in self.GAME_OR_FLAGGED}, sorted(found))


if __name__ == "__main__":
    unittest.main()
