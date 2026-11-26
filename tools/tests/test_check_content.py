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


class UnlistedPythonTests(unittest.TestCase):
    def test_a_test_file_no_workflow_names_is_found(self):
        wf = "run: python tools/autotest/tests/test_a.py -v\n"
        paths = ["tools/autotest/tests/test_a.py", "tools/autotest/tests/test_b.py",
                 "tools/puppet_pack/tests/test_c.py", "tools/autotest/helper.py", "tests/host/x_test.cpp"]
        self.assertEqual(check_content.unlisted_python_tests(paths, wf),
                         ["tools/autotest/tests/test_b.py", "tools/puppet_pack/tests/test_c.py"])

    def test_name_test_py_files_count_and_a_runner_script_counts(self):
        paths = ["tools/a/x_test.py", "tools/a/y_test.py", "tools/hookshot/z_test.py", "tools/a/notatest.py"]
        wf = "run: python tools/a/x_test.py\n"
        runner = "python z_test.py\n"   # tools/hookshot/host_compile_check.* names its files without the folder
        self.assertEqual(check_content.unlisted_python_tests(paths, wf, runner), ["tools/a/y_test.py"])

    def listed(self, files):
        root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, root)
        for path, text in files.items():
            full = os.path.join(root, path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8") as f:
                f.write(text)
        return check_content.listed_cpp_tests(root, list(files))

    def test_cpp_tests_are_resolved_against_the_folder_of_the_cmake_file(self):
        listed = self.listed({
            "tests/CMakeLists.txt": "add_executable(t\n  host/a_test.cpp\n  host/session_test.cpp\n  ${CMAKE_CURRENT_SOURCE_DIR}/net/n_test.cpp)\n",
            "tests/server/CMakeLists.txt": "add_executable(s\n  session_test.cpp)\n",
        })
        self.assertEqual(listed, {"tests/host/a_test.cpp", "tests/host/session_test.cpp", "tests/net/n_test.cpp",
                                  "tests/server/session_test.cpp"})
        paths = ["tests/host/a_test.cpp", "tests/host/c_test.cpp", "tests/host/helper.hpp", "tools/x.cpp"]
        self.assertEqual(check_content.unlisted_cpp_tests(paths, listed), ["tests/host/c_test.cpp"])
        self.assertEqual(check_content.unlisted_cpp_tests(["tests\\host\\d_test.cpp"], listed), ["tests\\host\\d_test.cpp"])

    def test_a_second_file_of_the_same_name_is_found(self):
        listed = self.listed({"tests/CMakeLists.txt": "host/session_test.cpp\n"})
        self.assertEqual(check_content.unlisted_cpp_tests(["tests/host/session_test.cpp", "tests/server/session_test.cpp"], listed),
                         ["tests/server/session_test.cpp"])

    def test_a_name_that_ends_another_is_found(self):
        listed = self.listed({"tests/CMakeLists.txt": "host/game_flags_test.cpp\n"})
        self.assertEqual(check_content.unlisted_cpp_tests(["tests/host/game_flags_test.cpp", "tests/x/flags_test.cpp"], listed),
                         ["tests/x/flags_test.cpp"])

    def test_a_cmake_comment_lists_nothing(self):
        listed = self.listed({"tests/CMakeLists.txt": "# host/a_test.cpp\nhost/b_test.cpp # a_test.cpp\n"})
        self.assertEqual(listed, {"tests/host/b_test.cpp"})

    def test_ingame_rows_count_when_their_selftest_runs(self):
        """tools/autotest/ingame/test_*.py are played whole by tools/autotest/tests/test_ingame.py (row-53-ocarina-5)."""
        paths = ["tools/autotest/ingame/test_row53.py", "tools/autotest/tests/test_ingame.py",
                 "tools/autotest/tests/test_ingame_row53.py"]      # (a port's own selftests: test_ingame.py load_tests)
        self.assertEqual(check_content.unlisted_python_tests(paths, "run: python tools/autotest/tests/test_ingame.py -v"), [])
        self.assertEqual(check_content.unlisted_python_tests(["tools/autotest/tests/test_ingame_row53.py"], "run: x"),
                         ["tools/autotest/tests/test_ingame_row53.py"])
        self.assertEqual(check_content.unlisted_python_tests(paths, "run: python tools/other.py"), paths)

    def test_runner_names_match_whole_words(self):
        paths = ["tools/hookshot/z_test.py", "tools/hookshot/xz_test.py"]
        self.assertEqual(check_content.unlisted_python_tests(paths, "", "python xz_test.py\n"), ["tools/hookshot/z_test.py"])

    def test_the_repository_builds_every_cpp_test(self):
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        files = check_content.tracked_files(root)
        self.assertEqual(check_content.unlisted_cpp_tests(files, check_content.listed_cpp_tests(root, files)), [])

    def test_windows_separators_count(self):
        self.assertEqual(check_content.unlisted_python_tests(["tools\\x\\tests\\test_q.py"], ""), ["tools\\x\\tests\\test_q.py"])

    def test_the_repository_runs_every_python_test_file(self):
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        if not os.path.isdir(os.path.join(root, ".github", "workflows")):
            self.skipTest("no workflows folder (a kit copy)")
        files = check_content.tracked_files(root)
        self.assertEqual(check_content.unlisted_python_tests(files, check_content.workflow_text(root),
                                                             check_content.runner_text(root, files)), [])


if __name__ == "__main__":
    unittest.main()
