"""Tests for tools/check_actions.py (run: python tools/check_actions_test.py -v)."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_actions  # noqa: E402

SHA = "11d5960a326750d5838078e36cf38b85af677262"


class CheckActionsTest(unittest.TestCase):
    def lines(self, text):
        return [n for n, _ in check_actions.check_text(text)]

    def test_pinned_with_tag_comment_passes(self):
        self.assertEqual(self.lines(f"      - uses: actions/checkout@{SHA}  # v4.4.0\n"), [])
        self.assertEqual(self.lines(f"        uses: owner/repo/sub/path@{SHA} # v1\n"), [])
        self.assertEqual(self.lines(f"  - uses: 'actions/checkout@{SHA}'  # v4\n"), [])

    def test_tag_or_branch_fails(self):
        self.assertEqual(self.lines("      - uses: actions/checkout@v4\n"), [1])
        self.assertEqual(self.lines("steps:\n  - uses: actions/setup-python@main  # v5\n"), [2])
        self.assertEqual(self.lines("  - uses: actions/checkout\n"), [1])

    def test_short_sha_fails(self):
        self.assertEqual(self.lines("  - uses: actions/checkout@11d5960  # v4\n"), [1])

    def test_pinned_without_comment_fails(self):
        self.assertEqual(self.lines(f"  - uses: actions/checkout@{SHA}\n"), [1])

    def test_local_and_docker_digest_pass(self):
        self.assertEqual(self.lines("  - uses: ./.github/actions/setup\n"), [])
        self.assertEqual(self.lines("  - uses: docker://alpine@sha256:" + "a" * 64 + "\n"), [])
        self.assertEqual(self.lines("  - uses: docker://alpine:3.20\n"), [1])

    def test_other_lines_ignored(self):
        self.assertEqual(self.lines("  # uses: actions/checkout@v4\n  run: echo uses: x@v1\n"), [])

    def runner(self, text):
        return [n for n, _ in check_actions.check_runner(text)]

    def test_gated_runner_line_passes(self):
        text = (f"jobs:\n  windows:\n    {check_actions.RUNNER_LINE}\n"
                "    steps:\n      - if: runner.environment == 'self-hosted'\n"
                "        # self-hosted only\n        run: echo\n")
        self.assertEqual(self.runner(text), [])

    def test_ungated_runner_fails(self):
        self.assertEqual(self.runner("    runs-on: [self-hosted, daurosk-pc]\n"), [1])
        self.assertEqual(self.runner("    runs-on: self-hosted\n"), [1])
        gate_cut = check_actions.RUNNER_LINE.replace(" && github.actor == 'daurosk'", "")
        self.assertEqual(self.runner(f"    {gate_cut}\n"), [1])
        widened = check_actions.RUNNER_LINE.replace("(github.event_name", "(true || github.event_name")
        self.assertEqual(self.runner(f"    {widened}\n"), [1])

    def test_linux_lines_pass_and_cut_ones_fail(self):
        for line in (check_actions.RUNNER_LINE_LINUX, check_actions.RUNNER_LINE_MATRIX):
            self.assertEqual(self.runner(f"    {line}\n"), [])
            self.assertEqual(self.runner("    " + line.replace(" && vars.COOP_SELF_HOSTED_LINUX == 'on'", "") + "\n"), [1])
            self.assertEqual(self.runner("    " + line.replace("github.event_name == 'workflow_dispatch'", "github.event_name == 'pull_request'") + "\n"), [1])
        self.assertEqual(self.runner("    runs-on: [self-hosted, squadala-wsl]\n"), [1])

    def test_gate_never_picks_self_hosted_for_a_pull_request(self):
        # Evaluate the gate the way GitHub does, for every event shape: only daurosk's own push/dispatch passes.
        import re as _re

        def gate_true(line, ctx):
            m = _re.search(r"\$\{\{ (.*) && fromJSON", line)
            expr = m.group(1).replace("matrix.os == 'ubuntu-latest' && ", "")
            expr = _re.sub(r"startsWith\(github\.ref, '([^']*)'\)", lambda g: repr(ctx["ref"].startswith(g.group(1))), expr)
            for k in ("repository", "actor", "triggering_actor", "event_name", "ref"):
                expr = expr.replace(f"github.{k}", repr(ctx[k]))
            expr = _re.sub(r"vars\.\w+", repr("on"), expr).replace("&&", " and ").replace("||", " or ")
            return eval(expr)  # noqa: S307 - our own constant expression, made of literals only

        base = {"repository": "daurosk/oot-remake-multiplayer", "actor": "daurosk", "triggering_actor": "daurosk"}
        cases = [
            ({"event_name": "push", "ref": "refs/heads/integrate-0120"}, True),
            ({"event_name": "push", "ref": "refs/heads/main"}, True),
            ({"event_name": "push", "ref": "refs/tags/v0.12.0"}, True),
            ({"event_name": "workflow_dispatch", "ref": "refs/heads/ci-speed-c143"}, True),
            ({"event_name": "push", "ref": "refs/heads/some-work-branch"}, False),
            ({"event_name": "pull_request", "ref": "refs/pull/9/merge"}, False),
            ({"event_name": "pull_request", "ref": "refs/heads/main"}, False),
            ({"event_name": "pull_request_target", "ref": "refs/heads/main"}, False),
            ({"event_name": "push", "ref": "refs/heads/main", "actor": "someone"}, False),
            ({"event_name": "workflow_dispatch", "ref": "refs/heads/main", "triggering_actor": "someone"}, False),
            ({"event_name": "push", "ref": "refs/heads/main", "repository": "fork/oot-remake-multiplayer"}, False),
        ]
        for line in (check_actions.RUNNER_LINE_LINUX, check_actions.RUNNER_LINE_MATRIX):
            for ctx, want in cases:
                full = dict(base, **ctx)
                self.assertEqual(gate_true(line, full), want, (line[:40], full))
        # The (not installed) Windows runner line is push-only (its hook limits the branches): never for a
        # pull request, a fork or another actor.
        for ctx, want in cases:
            if not want and ctx.get("ref") != "refs/heads/some-work-branch":
                self.assertFalse(gate_true(check_actions.RUNNER_LINE, dict(base, **ctx)), ctx)

    def test_runner_through_matrix_fails(self):
        text = "    strategy:\n      matrix:\n        os: [windows-latest, daurosk-pc]\n    runs-on: ${{ matrix.os }}\n"
        self.assertEqual(self.runner(text), [3])

    def test_runner_with_pull_request_target_fails(self):
        text = f"on:\n  pull_request_target:\njobs:\n  a:\n    {check_actions.RUNNER_LINE}\n"
        self.assertEqual(self.runner(text), [1])

    def test_no_runner_no_problem(self):
        self.assertEqual(self.runner("on:\n  pull_request_target:\njobs:\n  a:\n    runs-on: ubuntu-latest\n"), [])

    def test_repository_workflows_pass(self):
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        for path in check_actions.workflow_files(root):
            with open(path, encoding="utf-8") as f:
                text = f.read()
            self.assertEqual(check_actions.check_text(text), [], path)
            self.assertEqual(check_actions.check_runner(text), [], path)


if __name__ == "__main__":
    unittest.main()
