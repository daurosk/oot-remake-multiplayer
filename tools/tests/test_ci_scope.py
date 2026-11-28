"""tools/ci_scope.py: which Build runs may skip the C++ jobs (B #131 2026-11-26)."""
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import ci_scope as cs  # noqa: E402

WORK = "refs/heads/test-speed-boot-key"
D = "workflow_dispatch"


class Decide(unittest.TestCase):
    def test_python_only_diff_is_scoped(self):
        full, why = cs.decide(D, WORK, ["tools/autotest/rc_tests.py", "tools/autotest/tests/test_rc_tests.py", "docs/plan.md"])
        self.assertFalse(full)
        self.assertTrue(why.startswith("python-only (files: tools/autotest/rc_tests.py"), why)

    def test_one_cpp_file_is_full(self):
        full, why = cs.decide(D, WORK, ["tools/autotest/rc_tests.py", "hookshot/src/coop_menu.cpp"])
        self.assertTrue(full)
        self.assertIn("hookshot/src/coop_menu.cpp", why)

    def test_workflow_change_is_full(self):
        self.assertTrue(cs.decide(D, WORK, ["tools/autotest/rc_tests.py", ".github/workflows/build.yml"])[0])

    def test_kit_and_cpp_side_tool_folders_are_full(self):
        for f in ("tools/hookshot/hs.py", "tools/puppet_pack/pack.py", "tools/release/kit_pack.ps1", "tools/nucleus/handler.js",
                  "tools/ci-runner/x.ps1", "tools/coop_test/main.cpp", "tools/playtest/scripts/a.txt", "lichii/PIN",
                  "hookshot/patches/0001.patch", "CMakeLists.txt", "server/main.cpp", "tools/dev-check.ps1"):
            self.assertTrue(cs.decide(D, WORK, [f])[0], f)

    def test_md_anywhere_is_scoped(self):
        self.assertFalse(cs.decide(D, WORK, ["hookshot/README.md", "docs/plan.md"])[0])

    def test_kit_read_docs_are_full_and_run_longshot(self):
        # review-mod-153-2 00:26: the kits read these; a change to one must build both kits
        for f in ("THIRD_PARTY_NOTICES.md", "docs/known-issues.md", "docs/setup-linux.md", "docs/problem-send-address.txt",
                  "docs/setup.md"):
            self.assertTrue(cs.kit_read(f), f)
            self.assertTrue(cs.decide(D, WORK, ["tools/autotest/a.py", f])[0], f)
            self.assertTrue(cs.longshot_needed(["docs/plan.md", f]), f)
        # a deleted or renamed kit doc: gone from the tree, still named by make_kit_linux.sh (review-mod-153-2 00:28)
        self.assertTrue(cs.kit_named("docs/known-issues.md", cs.kit_scan_text()))
        self.assertFalse(cs.kit_named("docs/no-such-doc-xyz.md", cs.kit_scan_text()))
        self.assertFalse(cs.kit_read("docs/plan.md"))
        self.assertFalse(cs.kit_read("tools/autotest/README.md"))

    def test_kit_read_list_follows_the_kit_scripts(self):
        # drift check: every repo doc a kit script names by "$repo/<path>" (make_kit_linux.sh, the kit tests) or in
        # make_kit_hookshot.ps1's $KitDocs is in the derived list
        import re
        rel = os.path.join(cs.REPO, "tools", "release")
        named = set()
        for name in ("make_kit_linux.sh", os.path.join("tests", "linux_longshot_kit_test.sh")):
            with open(os.path.join(rel, name), encoding="utf-8") as fh:
                for m in re.finditer(r'\$repo/((?:docs/)?[A-Za-z0-9_.-]+\.(?:md|txt))', fh.read()):
                    named.add(m.group(1))
        with open(os.path.join(rel, "make_kit_hookshot.ps1"), encoding="utf-8") as fh:
            kd = re.search(r"^\$KitDocs = (.*)$", fh.read(), re.M).group(1)
        named.update("docs/" + n for n in re.findall(r"@\('([^']+)'", kd))
        self.assertIn("docs/known-issues.md", named)
        self.assertIn("THIRD_PARTY_NOTICES.md", named)
        missing = sorted(n for n in named if os.path.exists(os.path.join(cs.REPO, n)) and n not in cs.kit_read_paths())
        self.assertEqual(missing, [])

    def test_main_integrate_release_tags_always_full(self):
        for ref in ("refs/heads/main", "refs/heads/integrate-0120", "refs/heads/release-01112", "refs/tags/v0.12.0"):
            self.assertTrue(cs.decide(D, ref, ["docs/a.md"])[0], ref)

    def test_other_events_always_full(self):
        for ev in ("push", "pull_request", "", "schedule"):
            self.assertTrue(cs.decide(ev, WORK, ["docs/a.md"])[0], ev)

    def test_unknown_or_empty_diff_is_full(self):
        self.assertTrue(cs.decide(D, WORK, None)[0])
        self.assertTrue(cs.decide(D, WORK, [])[0])

    def test_prefix_is_a_folder_not_a_name_start(self):
        self.assertTrue(cs.decide(D, WORK, ["tools/autotest_extra.cpp"])[0])
        self.assertTrue(cs.decide(D, WORK, ["tools/rando.cpp"])[0])

    def test_bad_base_name_gives_no_diff(self):
        self.assertIsNone(cs.changed_files("x; rm -rf /"))


def run(cwd, *args):
    subprocess.run(list(args), cwd=cwd, check=True, capture_output=True, text=True)


class RealGit(unittest.TestCase):
    """changed_files + main on a throwaway repo with its own 'origin'."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = self.tmp.name
        self.origin = os.path.join(t, "origin.git")
        self.repo = os.path.join(t, "repo")
        run(t, "git", "init", "-q", "--bare", self.origin)
        run(t, "git", "init", "-q", self.repo)
        for k, v in (("user.name", "t"), ("user.email", "t@example.invalid"), ("commit.gpgsign", "false")):
            run(self.repo, "git", "config", k, v)
        self.write("hookshot/src/a.cpp", "int a;\n")
        run(self.repo, "git", "add", "-A")
        run(self.repo, "git", "commit", "-q", "-m", "base")
        run(self.repo, "git", "remote", "add", "origin", self.origin)
        run(self.repo, "git", "push", "-q", "origin", "HEAD:refs/heads/integrate-0120")
        run(self.repo, "git", "checkout", "-q", "-b", "work")
        self.cwd = os.getcwd()
        os.chdir(self.repo)

    def tearDown(self):
        os.chdir(self.cwd)
        self.tmp.cleanup()

    def write(self, rel, text):
        p = os.path.join(self.repo, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as fh:
            fh.write(text)

    def commit(self, rel):
        self.write(rel, "x\n")
        run(self.repo, "git", "add", "-A")
        run(self.repo, "git", "commit", "-q", "-m", rel)

    def main_out(self, base="integrate-0120"):
        from io import StringIO
        out, err = StringIO(), StringIO()
        old = sys.stdout, sys.stderr
        sys.stdout, sys.stderr = out, err
        os.environ.pop("GITHUB_STEP_SUMMARY", None)
        try:
            cs.main(["--event", D, "--ref", "refs/heads/work", "--base", base])
        finally:
            sys.stdout, sys.stderr = old
        return out.getvalue().strip(), err.getvalue().strip()

    def test_python_only_branch(self):
        self.commit("tools/autotest/x.py")
        self.assertEqual(cs.changed_files("integrate-0120"), ["tools/autotest/x.py"])
        out, err = self.main_out()
        self.assertEqual(out, "full=false\nlongshot=false")
        self.assertEqual(err, "scope: python-only (files: tools/autotest/x.py) (base integrate-0120); "
                              "linux-longshot skipped (every file is one it never reads)")

    def test_cpp_branch(self):
        self.commit("tools/autotest/x.py")
        self.commit("hookshot/src/b.cpp")
        self.assertEqual(self.main_out()[0], "full=true\nlongshot=true")

    def test_windows_only_test_skips_longshot(self):
        self.commit("tools/release/tests/launch_test.ps1")
        self.assertEqual(self.main_out()[0], "full=true\nlongshot=false")

    def test_deleted_kit_doc_is_full_and_runs_longshot(self):
        # review-mod-153-2 00:28: the doc is gone from HEAD, the real kit scripts still name it
        self.commit("docs/known-issues.md")
        run(self.repo, "git", "push", "-q", "origin", "HEAD:refs/heads/integrate-0120")
        run(self.repo, "git", "rm", "-q", "docs/known-issues.md")
        run(self.repo, "git", "commit", "-q", "-m", "rm")
        self.assertEqual(cs.changed_files("integrate-0120"), ["docs/known-issues.md"])
        self.assertEqual(self.main_out()[0], "full=true\nlongshot=true")

    def test_cpp_outside_hookshot_runs_longshot(self):
        self.commit("coop/net/x.cpp")
        self.assertEqual(self.main_out()[0], "full=true\nlongshot=true")

    def test_auto_base_picks_the_newest_integrate(self):
        # integrate-0120 = the base commit; integrate-0121 = one commit later, and work branches off integrate-0121:
        # auto must diff against integrate-0121 (only the work commit), not the stale integrate-0120 (both commits).
        self.commit("hookshot/src/later.cpp")
        run(self.repo, "git", "push", "-q", "origin", "HEAD:refs/heads/integrate-0121")
        self.commit("tools/autotest/x.py")
        self.assertEqual(cs.auto_base(), "integrate-0121")
        out, err = self.main_out("auto")
        self.assertEqual(out, "full=false\nlongshot=false")
        self.assertIn("(base integrate-0121 auto)", err)
        # the named base still overrides auto
        self.assertEqual(self.main_out("integrate-0120")[0], "full=true\nlongshot=true")

    def test_auto_base_none_is_full(self):
        run(self.repo, "git", "push", "-q", "origin", ":refs/heads/integrate-0120")
        self.commit("tools/autotest/x.py")
        self.assertIsNone(cs.auto_base())
        self.assertEqual(self.main_out("auto")[0], "full=true\nlongshot=true")

    def test_base_moved_on_counts_only_own_commits(self):
        self.commit("tools/autotest/x.py")
        run(self.repo, "git", "checkout", "-q", "-b", "other", "HEAD~1")
        self.commit("hookshot/src/c.cpp")
        run(self.repo, "git", "push", "-q", "origin", "HEAD:refs/heads/integrate-0120")
        run(self.repo, "git", "checkout", "-q", "work")
        self.assertEqual(cs.changed_files("integrate-0120"), ["tools/autotest/x.py"])

    def test_move_out_of_cpp_path_is_full(self):
        os.makedirs(os.path.join(self.repo, "tools", "autotest"))
        run(self.repo, "git", "mv", "hookshot/src/a.cpp", "tools/autotest/a.cpp")
        run(self.repo, "git", "commit", "-q", "-m", "move")
        self.assertEqual(sorted(cs.changed_files("integrate-0120")), ["hookshot/src/a.cpp", "tools/autotest/a.cpp"])
        self.assertEqual(self.main_out()[0].splitlines()[0], "full=true")

    def test_longshot_paths(self):
        for f in ("hookshot/src/a.cpp", "lichii/hook/coop_hook.cpp", "patches/x.ips", "hookshot/patches/0012.ips", "x/patches/y"):
            self.assertTrue(cs.longshot_needed([f]), f)
        # every path the job builds or reads (review-mod-153-2 00:23), and anything unknown
        for f in ("coop/host/a.hpp", "coop/net/b.cpp", "server/main.cpp", "cmake/enet.cmake", "CMakeLists.txt",
                  "tools/release/make_kit_linux.sh", "tools/release/x.sh", "tools/release/kit/scripts/kit-paths.ps1",
                  "tools/release/tests/fixtures/linux-longshot-v0.11.2/x.ps1", "tools/release/tests/kit_pack_check.py",
                  "tools/puppet_pack/charsel/data/x", "tools/hookshot/x.py", "tools/hookshot/host_compile_check.sh",
                  ".github/workflows/build.yml", "tests/hookshot/load_module.c", "src/net/x.cpp", "newdir/x"):
            self.assertTrue(cs.longshot_needed([f]), f)
            self.assertTrue(cs.longshot_needed(["docs/a.md", f]), f)
        for f in ("tools/autotest/a.py", "docs/plan.md", "tools/autotest/README.md", "tools/release/tests/launch_test.ps1",
                  ".github/workflows/content-guard.yml"):
            self.assertFalse(cs.longshot_needed([f]), f)
        self.assertTrue(cs.longshot_needed(None))
        self.assertTrue(cs.longshot_needed([]))

    def test_missing_base_is_full(self):
        self.commit("tools/autotest/x.py")
        self.assertIsNone(cs.changed_files("no-such-branch"))
        self.assertTrue(cs.decide(D, "refs/heads/work", None)[0])


if __name__ == "__main__":
    unittest.main()
