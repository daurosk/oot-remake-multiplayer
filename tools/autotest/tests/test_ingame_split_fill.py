"""Selftests of row 301's bridge test (tools/autotest/ingame/test_split_fill.py): its black-bar check on made-up
pictures and, on Windows, its layout helper (split_layout.ps1: the kit's own local-coop.ps1) on a made-up work area.
Run by test_ingame.py's load_tests (CI) or alone."""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ingame"))
import test_split_fill as S   # noqa: E402


class SplitFillBars(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def shot(self, w, h, picture):
        """A w x h shot, black, with `picture` (l, t, r, b) filled with a sky-like colour."""
        from PIL import Image
        im = Image.new("RGB", (w, h), (0, 0, 0))
        im.paste((90, 130, 200), picture)
        path = os.path.join(self.tmp, "s.png")
        im.save(path)
        return path

    def test_a_full_picture_has_no_bar(self):
        self.assertEqual(S.bars(self.shot(944, 992, (0, 0, 944, 992))), [])

    def test_a_16_9_band_in_a_tall_window_is_bars_above_and_below(self):
        # (the owner's report before the fix: a 16:9 picture in the middle of a half)
        h = int(944 * 9 / 16)
        top = (992 - h) // 2
        self.assertEqual(S.bars(self.shot(944, 992, (0, top, 944, top + h))), ["top", "bottom"])

    def test_a_narrow_picture_in_a_wide_one_is_bars_at_the_sides(self):
        self.assertEqual(S.bars(self.shot(1600, 900, (400, 0, 1200, 900))), ["left", "right"])

    def test_a_dark_corner_is_not_a_bar(self):
        # (a HUD's black outline or a dark corner leaves picture in the strip)
        path = self.shot(944, 992, (0, 0, 944, 992))
        from PIL import Image
        im = Image.open(path)
        im.paste((0, 0, 0), (0, 0, 15, 900))
        im.save(path)
        self.assertEqual(S.bars(path), [])


@unittest.skipUnless(sys.platform == "win32", "split_layout.ps1 runs the kit's Win32 code")
class SplitFillLayoutHelper(unittest.TestCase):
    def test_two_halves_of_a_made_up_work_area_with_their_switches(self):
        lay = S.helper("-Mode", "layout", "-Work", "0,0,1920,1032", "-Count", "2")
        self.assertEqual(lay["rects"], [[0, 0, 960, 1032], [960, 0, 960, 1032]])
        f = lay["frame"]
        cw, ch = 960 - (f[2] - f[0]), 1032 - (f[3] - f[1])
        self.assertEqual(lay["clients"], [[cw, ch], [cw, ch]])
        s = lay["shapes"][0]
        self.assertEqual(s["HOS_ASPECT"], "%d:%d" % (cw, ch))
        self.assertEqual(s["HOS_OUTPUT_RES"], "%dx%d" % (cw // 8 * 8, ch // 8 * 8))

    def test_relaunch_asks_shots_at_the_output_size(self):
        env = S.relaunch_env({"HOS_ASPECT": "944:1041", "HOS_OUTPUT_RES": "944x1040", "HOS_WINDOW_SIZE": "944x1041"})
        self.assertEqual(env, ["HOS_ASPECT=944:1041", "HOS_OUTPUT_RES=944x1040", "HOS_WINDOW_SIZE=944x1041",
                               "RX_VK_SHOT_W=944", "RX_VK_SHOT_H=1040"])

    def test_place_without_a_game_says_why(self):
        with self.assertRaises(AssertionError) as e:
            S.helper("-Mode", "place", "-GamePid", "1", "-Rect", "0,0,10,10")
        self.assertIn("has no window", str(e.exception))


if __name__ == "__main__":
    unittest.main()
