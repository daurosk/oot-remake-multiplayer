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

    def test_repository_workflows_pass(self):
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        for path in check_actions.workflow_files(root):
            with open(path, encoding="utf-8") as f:
                self.assertEqual(check_actions.check_text(f.read()), [], path)


if __name__ == "__main__":
    unittest.main()
