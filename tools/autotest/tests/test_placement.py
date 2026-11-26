"""Tests for tools/autotest/placement.py: the monitor choice and the window rects (no game, no real monitor needed)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import placement as pl  # noqa: E402

# A layout like a real desk: a primary, one to its right at an offset, one to the left.
MONS = [
    {"name": "\\\\.\\DISPLAY1", "work": (0, 0, 3072, 1680), "primary": True},
    {"name": "\\\\.\\DISPLAY2", "work": (3840, 463, 5760, 1495), "primary": False},
    {"name": "\\\\.\\DISPLAY3", "work": (-1920, 336, 0, 1368), "primary": False},
]


class DisplayName(unittest.TestCase):
    def test_default(self):
        self.assertEqual(pl.display_name(None, {}), "\\\\.\\DISPLAY2")

    def test_env_beats_default(self):
        self.assertEqual(pl.display_name(None, {pl.ENV: "\\\\.\\DISPLAY3"}), "\\\\.\\DISPLAY3")

    def test_arg_beats_env(self):
        self.assertEqual(pl.display_name("\\\\.\\DISPLAY1", {pl.ENV: "\\\\.\\DISPLAY3"}), "\\\\.\\DISPLAY1")

    def test_blank_env_is_the_default(self):
        self.assertEqual(pl.display_name("", {pl.ENV: "  "}), "\\\\.\\DISPLAY2")


class FindMonitor(unittest.TestCase):
    def test_by_device_name(self):
        self.assertEqual(pl.find_monitor(MONS, "\\\\.\\DISPLAY2")["work"], (3840, 463, 5760, 1495))

    def test_short_and_lowercase_names_match(self):
        self.assertEqual(pl.find_monitor(MONS, "display3")["name"], "\\\\.\\DISPLAY3")
        self.assertEqual(pl.find_monitor(MONS, " DISPLAY1 ")["name"], "\\\\.\\DISPLAY1")

    def test_display1_is_not_display10(self):
        mons = MONS + [{"name": "\\\\.\\DISPLAY10", "work": (0, 0, 10, 10)}]
        self.assertEqual(pl.find_monitor(mons, "DISPLAY1")["name"], "\\\\.\\DISPLAY1")

    def test_missing_monitor_is_an_error_that_lists_the_others(self):
        with self.assertRaises(pl.PlacementError) as c:
            pl.find_monitor(MONS, "\\\\.\\DISPLAY9")
        msg = str(c.exception)
        self.assertIn("DISPLAY9", msg)
        for m in MONS:
            self.assertIn(m["name"], msg)
        self.assertIn("primary", msg)
        self.assertIn(pl.ENV, msg)

    def test_primary_names_the_primary_monitor(self):
        self.assertEqual(pl.find_monitor(MONS, "primary")["name"], "\\\\.\\DISPLAY1")
        self.assertEqual(pl.find_monitor(MONS, "PRIMARY")["name"], "\\\\.\\DISPLAY1")

    def test_the_default_gone_is_the_primary_monitor(self):
        one = [MONS[0]]   # (a laptop alone: no DISPLAY2)
        self.assertEqual(pl.find_monitor(one, pl.DEFAULT_DISPLAY, primary_if_gone=True)["name"], "\\\\.\\DISPLAY1")
        with self.assertRaises(pl.PlacementError):   # (a name asked for stays an error)
            pl.find_monitor(one, pl.DEFAULT_DISPLAY)

    def test_check_display_keeps_the_monitor_when_it_exists(self):
        real, env = pl.monitors, os.environ.pop(pl.ENV, None)
        pl.monitors = lambda: MONS
        try:
            mon = pl.check_display()
            self.assertEqual(mon["name"], "\\\\.\\DISPLAY2")
            self.assertNotIn("note", mon)
            self.assertEqual(pl.check_display("DISPLAY3")["name"], "\\\\.\\DISPLAY3")
        finally:
            pl.monitors = real
            if env is not None:
                os.environ[pl.ENV] = env

    def test_check_display_falls_back_to_primary_when_the_monitor_is_missing(self):
        one = [MONS[0]]
        real, env = pl.monitors, os.environ.pop(pl.ENV, None)
        pl.monitors = lambda: one
        try:
            mon = pl.check_display()   # the default DISPLAY2 is off
            self.assertEqual(mon["name"], "\\\\.\\DISPLAY1")
            self.assertIn("DISPLAY2", mon["note"])
            self.assertIn("primary", mon["note"])
            self.assertEqual(pl.check_display("DISPLAY2")["name"], "\\\\.\\DISPLAY1")   # --display
            os.environ[pl.ENV] = "DISPLAY2"   # a stale OOT_TEST_DISPLAY
            self.assertEqual(pl.check_display()["name"], "\\\\.\\DISPLAY1")
            self.assertNotIn("note", pl.check_display("primary"))
            self.assertNotIn("note", pl.check_display("DISPLAY1"))
        finally:
            pl.monitors = real
            os.environ.pop(pl.ENV, None)
            if env is not None:
                os.environ[pl.ENV] = env

    def test_no_monitors_at_all(self):
        with self.assertRaises(pl.PlacementError):
            pl.find_monitor([], "DISPLAY2")


class Layout(unittest.TestCase):
    def test_two_windows_side_by_side_on_a_monitor_at_the_origin(self):
        self.assertEqual(pl.layout((0, 0, 1920, 1032), 2), [(0, 0, 960, 600), (960, 0, 960, 600)])

    def test_follows_the_monitors_work_area_not_zero_zero(self):
        mon = pl.find_monitor(MONS, "DISPLAY2")
        self.assertEqual(pl.layout(mon["work"], 2), [(3840, 463, 960, 600), (4800, 463, 960, 600)])

    def test_monitor_left_of_the_primary_has_negative_x(self):
        mon = pl.find_monitor(MONS, "DISPLAY3")
        self.assertEqual(pl.layout(mon["work"], 2), [(-1920, 336, 960, 600), (-960, 336, 960, 600)])

    def test_every_window_is_inside_the_work_area(self):
        for work in [(0, 0, 1920, 1032), (-1920, 336, 0, 1368), (100, 50, 1380, 770), (0, 0, 800, 400)]:
            for n in (1, 2, 3):
                for x, y, w, h in pl.layout(work, n):
                    self.assertGreaterEqual(x, work[0])
                    self.assertGreaterEqual(y, work[1])
                    self.assertLessEqual(x + w, work[2])
                    self.assertLessEqual(y + h, work[3])

    def test_windows_do_not_overlap(self):
        r = pl.layout((0, 0, 1000, 700), 2)
        self.assertLessEqual(r[0][0] + r[0][2], r[1][0])

    def test_small_area_shrinks_the_windows(self):
        self.assertEqual(pl.layout((0, 0, 1280, 720), 2), [(0, 0, 640, 600), (640, 0, 640, 600)])
        self.assertEqual(pl.layout((0, 0, 1920, 400), 2)[0][3], 400)

    def test_empty_area_is_an_error(self):
        with self.assertRaises(pl.PlacementError):
            pl.layout((10, 10, 10, 500), 2)
        with self.assertRaises(pl.PlacementError):
            pl.layout((0, 0, 1920, 1080), 0)


@unittest.skipUnless(os.name == "nt", "needs Windows")
class RealMonitors(unittest.TestCase):
    def test_a_primary_monitor_is_listed_with_a_work_area(self):
        mons = pl.monitors()
        self.assertTrue(mons)
        self.assertEqual(sum(1 for m in mons if m["primary"]), 1)
        for m in mons:
            self.assertTrue(m["name"].upper().startswith("\\\\.\\DISPLAY"), m["name"])
            self.assertGreater(m["work"][2] - m["work"][0], 0)
            self.assertGreater(m["work"][3] - m["work"][1], 0)

    def test_the_cli_rects_and_a_missing_display(self):
        import contextlib
        import io
        import json
        prim = [m for m in pl.monitors() if m["primary"]][0]["name"]
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(pl.main(["rects", "--display", prim, "--count", "2"]), 0)
        self.assertEqual(len(json.loads(buf.getvalue())["rects"]), 2)
        buf = io.StringIO()   # a missing monitor: the primary one, with the note
        with contextlib.redirect_stdout(buf):
            self.assertEqual(pl.main(["rects", "--display", "\\\\.\\DISPLAY99"]), 0)
        out = json.loads(buf.getvalue())
        self.assertEqual(out["display"], prim)
        self.assertIn("DISPLAY99", out["note"])


if __name__ == "__main__":
    unittest.main()
