"""Selftests of the page step (B #138; owner port-g7), no game. Run by test_ingame.py's load_tests (CI: python
tools/autotest/tests/test_ingame.py).
- MenuPages: ingame/test_menu_pages.py on fake bridges (ingame/fakes/menu_pages.py): PASS; FAIL when a page step opens
  nothing (the B #138 gap) or the wrong page.
- MenuWords: the `menu` words have ONE source, lichii/hook/coop_testinput.h kMenuWords / kMenuPages: the runner's copies
  (tools/autotest/hookshot.py MENU_WORDS / MENU_PAGES) and the bridge test's PAGES are the same lists, kMenuPages is
  every page CoopPauseMenu::PageWord names, and CoopPause::TestMenu (hookshot/src/coop_pause.cpp) handles each.
"""
import os
import re
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
AUTOTEST = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(AUTOTEST))
sys.path.insert(0, HERE)
sys.path.insert(0, AUTOTEST)
sys.path.insert(0, os.path.join(AUTOTEST, "ingame"))
import test_ingame as T   # noqa: E402
import hookshot   # noqa: E402

pytest_run, summary, results = T.pytest_run, T.summary, T.results
TEST = "test_every_page_opens_on_its_own"


def read(rel):
    with open(os.path.join(REPO, rel), encoding="utf-8") as f:
        return f.read()


def header_list(name):
    m = re.search(r"%s\{(.*?)\};" % name, read("lichii/hook/coop_testinput.h"), re.S)
    assert m, "no %s{...} in coop_testinput.h" % name
    return re.findall(r'"([^"]+)"', m.group(1))


class MenuPages(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_it(self, tag, env=None):
        out = os.path.join(self.tmp, tag)
        p = pytest_run(["test_menu_pages.py", "--fake", "--no-rerun", "--out", out, "-q", "-p", "no:cacheprovider"],
                       env=env)
        return p, summary(out)

    def test_pass(self):
        p, s = self.run_it("ok")
        self.assertEqual(results(s), {TEST: "PASS"}, p.stdout[-2000:])

    def test_fail_when_no_page_comes(self):
        p, s = self.run_it("nopage", env={"BT_FAKE_MENU": "nopage"})
        self.assertEqual(results(s), {TEST: "FAIL"}, p.stdout[-2000:])
        self.assertIn("on its main page", p.stdout)

    def test_fail_on_the_wrong_page(self):
        p, s = self.run_it("wrong", env={"BT_FAKE_MENU": "wrong"})
        self.assertEqual(results(s), {TEST: "FAIL"}, p.stdout[-2000:])
        self.assertIn("on its host page", p.stdout)


class MenuWords(unittest.TestCase):
    def test_runner_copies_are_the_header_lists(self):
        self.assertEqual(list(hookshot.MENU_WORDS), header_list("kMenuWords"))
        self.assertEqual(list(hookshot.MENU_PAGES), header_list("kMenuPages"))
        import test_menu_pages   # (the bridge test takes the runner's list itself, no copy: review-rows-c136)
        self.assertIs(test_menu_pages.PAGES, hookshot.MENU_PAGES)

    def test_every_page_of_the_tab_has_an_open_step(self):
        pages = re.findall(r'case Page::k\w+: return "(\w+)";', read("hookshot/src/coop_pause_menu.cpp"))
        enum = re.search(r"enum class Page \{([^}]*)\}", read("hookshot/src/coop_pause_menu.h")).group(1)
        self.assertEqual(len(pages), len([e for e in enum.split(",") if e.strip()]))   # (PageWord names every page)
        self.assertEqual(sorted(pages), sorted(header_list("kMenuPages")))

    def test_the_module_opens_every_page(self):
        src = read("hookshot/src/coop_pause.cpp")
        start = src.index("bool TestMenu(const std::string& page) {")
        body = src[start:src.index("\n}\n", start)]
        for w in header_list("kMenuWords"):
            self.assertIn('"%s"' % w, body, "CoopPause::TestMenu has no %r" % w)
        table = body[body.index('page.rfind("open ", 0) == 0'):]
        for p in header_list("kMenuPages"):
            self.assertIn('{"%s", CoopPauseMenu::Page::k' % p, table, "TestMenu's open table has no %r" % p)

    def test_the_runner_parses_open(self):
        for w in ("main", "host", "find", "Settings"):
            steps = hookshot.parse("menu open %s\n" % w)
            self.assertEqual((steps[-1].kind, steps[-1].name), ("menu", "open " + w.lower()))
        self.assertEqual(hookshot.parse("menu main\n")[-1].name, "main")
        for bad in ("menu open\n", "menu open map\n", "menu join\n", "menu open main now\n"):
            with self.assertRaises(hookshot.ScriptError):
                hookshot.parse(bad)


if __name__ == "__main__":
    unittest.main()
