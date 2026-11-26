"""Tests for tools/check_content.py: the test-name guard (a ';' in a doctest name)."""

import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import check_content  # noqa: E402


class TestNames(unittest.TestCase):
    def test_semicolon_names_are_found(self):
        bad = check_content.bad_test_names
        self.assertEqual(bad('TEST_CASE("dbno body: a remade puppet gets the key again; a gone one") {'),
                         ["dbno body: a remade puppet gets the key again; a gone one"])
        self.assertEqual(bad('    SUBCASE("up; down") {'), ["up; down"])
        self.assertEqual(bad('SCENARIO("a;b")'), ["a;b"])
        self.assertEqual(bad('TEST_CASE_TEMPLATE("t;", T, int, long)'), ["t;"])
        # The fixture's name is the second argument (tests/CMakeLists.txt's regex missed it before).
        self.assertEqual(bad('TEST_CASE_FIXTURE(Rig, "with a rig; twice")'), ["with a rig; twice"])
        self.assertEqual(bad('TEST_CASE_FIXTURE(ns::Rig<int>, "x;y")'), ["x;y"])

    def test_good_names_pass(self):
        bad = check_content.bad_test_names
        self.assertEqual(bad('TEST_CASE("a remade puppet gets the key again, a gone one: never called") {'), [])
        self.assertEqual(bad('TEST_CASE("x") { int a = 1; int b = 2; }'), [])  # ';' after the name
        self.assertEqual(bad('SUBCASE("quote \\" inside, no semicolon") {'), [])
        self.assertEqual(bad('CHECK(s == "a;b");'), [])  # not a test name
        self.assertEqual(bad('MY_TEST_CASE("a;b")'), [])  # another macro

    def test_escaped_quote_does_not_end_the_name(self):
        self.assertEqual(check_content.bad_test_names('TEST_CASE("say \\"hi\\"; then go")'), ['say \\"hi\\"; then go'])


class TestNameProblems(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.root)

    def write(self, path, text):
        full = os.path.join(self.root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as f:
            f.write(text)

    def test_only_cpp_sources_under_tests(self):
        line = 'TEST_CASE("one; two") {}\n'
        self.write("tests/lichii/a_test.cpp", "// x\n" + line)
        self.write("tests/lichii/b.h", line)
        self.write("tools/x.cpp", line)          # not under tests/
        self.write("tests/notes.md", line)       # not a source
        p = check_content.test_name_problems
        self.assertEqual(p(self.root, "tests/lichii/a_test.cpp"), ['line 2: "one; two"'])
        self.assertEqual(p(self.root, "tests/lichii/b.h"), ['line 1: "one; two"'])
        self.assertEqual(p(self.root, "tools/x.cpp"), [])
        self.assertEqual(p(self.root, "tests/notes.md"), [])

    def test_the_repository_has_none(self):
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        found = [f"{path}: {w}" for path in check_content.tracked_files(root)
                 for w in check_content.test_name_problems(root, path)]
        self.assertEqual(found, [])


class TestPlaytestLibrary(unittest.TestCase):
    """tools/playtest holds text about the game only: no pictures, logs, big files, local paths or IP addresses."""

    def setUp(self):
        self.root = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.root)

    def write(self, path, text="x\n"):
        full = os.path.join(self.root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        data = text if isinstance(text, bytes) else text.encode("utf-8")
        with open(full, "wb") as f:
            f.write(data)

    def problems(self, path):
        return check_content.playtest_problems(self.root, path)

    def test_text_files_pass(self):
        self.write("tools/playtest/scripts/a.jsonl", '# what: x\n{"server":"oot-p1","tool":"step","args":{"frames":1}}\n')
        self.write("tools/playtest/data/a.json", '{"scene": "InsideTheDekuTree", "x": 6.51}\n')
        self.write("tools/playtest/README.md", "Loopback 127.0.0.1 is fine, and so is a version 0.11.1.\n")
        for p in ("tools/playtest/scripts/a.jsonl", "tools/playtest/data/a.json", "tools/playtest/README.md"):
            self.assertEqual(self.problems(p), [], p)

    def test_other_folders_are_not_checked(self):
        self.write("tools/autotest/x.png", b"\x89PNG")
        self.write("docs/a.md", "C:\\Users\\someone\\game 10.0.0.5\n")
        self.assertEqual(self.problems("tools/autotest/x.png"), [])
        self.assertEqual(self.problems("docs/a.md"), [])

    def test_only_text_extensions(self):
        for name in ("shot.png", "run.log", "save.bin", "notes.txt", "a.json.zip", "SHOT.PNG"):
            self.write("tools/playtest/data/" + name, b"x")
            self.assertEqual(len(self.problems("tools/playtest/data/" + name)), 1, name)

    def test_big_file_is_refused(self):
        self.write("tools/playtest/data/big.json", "x" * (check_content.PLAYTEST_MAX_BYTES + 1))
        self.assertIn("is larger than 256 KB", self.problems("tools/playtest/data/big.json"))
        self.write("tools/playtest/data/ok.json", "x" * check_content.PLAYTEST_MAX_BYTES)
        self.assertEqual(self.problems("tools/playtest/data/ok.json"), [])

    def test_local_paths_and_addresses_are_refused(self):
        bad = ["C:\\Users\\someone\\kit", "see d:/games/oot", "/home/someone/oot", "x /Users/someone/oot",
               "host 192.168.1.20 port 7777", "server 8.8.8.8", "10.0.0.5:5001"]
        for line in bad:
            self.write("tools/playtest/data/a.json", line + "\n")
            self.assertEqual(len(self.problems("tools/playtest/data/a.json")), 1, line)
        ok = ["connects to 127.0.0.1:5011", "pos 5.45 108.9 6.15", "version 1.2.3.4.5",
              "about 10 m: https://example.com/a/Users"]
        for line in ok:
            self.write("tools/playtest/data/a.json", line + "\n")
            self.assertEqual(self.problems("tools/playtest/data/a.json"), [], line)

    def test_a_runs_own_files_are_refused_anywhere(self):
        for path in ("calls.jsonl", "tools/autotest/calls.jsonl", "sessions/run1/session.json", "x/state-history.json",
                     ".mcp.json", "playtest-logs/a.log", "playtest-logs/shots/p1-1.png", "run/shots/a.PNG", "docs/sessions/a.md"):
            self.assertEqual(len(self.problems(path)) >= 1, True, path)
        for path in ("tools/playtest/scripts/calls-example.jsonl", "tools/autotest/mcp.json.template", "docs/shots.md",
                     "tools/release/kit/assets/squadala-icon.png", "coop/session.cpp"):
            self.assertEqual(self.problems(path), [], path)

    def test_the_repository_has_none(self):
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        found = [f"{path}: {w}" for path in check_content.tracked_files(root)
                 for w in check_content.playtest_problems(root, path)]
        self.assertEqual(found, [])


if __name__ == "__main__":
    unittest.main()
