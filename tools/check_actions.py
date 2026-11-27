"""Fail if a GitHub workflow uses an action that is not pinned to a full commit SHA.

A tag such as actions/checkout@v4 can be moved to other code by whoever controls
that repository; a 40-hex commit SHA cannot (issue 162). Every `uses:` in
.github/workflows/*.yml must therefore be one of:

    owner/repo[/path]@<40 hex>  # <tag it was taken from>
    ./local/action               (an action in this repository)
    docker://image@sha256:<64 hex>

The tag comment says which release the SHA is, so an update can be checked
against the action's own tag (gh api repos/<owner>/<repo>/git/ref/tags/<tag>).

It also guards the self-hosted runners on the owner's PC (docs/ci-runner.md). Any
non-comment line naming `self-hosted` or a runner label (squadala-wsl, daurosk-pc)
must be exactly one of RUNNER_LINES (Linux: daurosk's own push to main, integrate-*,
release-* or a tag, or daurosk's manual run, in daurosk's repo, with the repo variable
COOP_SELF_HOSTED_LINUX on; Windows (not installed): daurosk's push with COOP_SELF_HOSTED
on; everything else gets the GitHub-hosted image), apart from the step tests `runner.environment == 'self-hosted'` and `!=`. A workflow that names the runner must
not use pull_request_target.
Exit code 0 means every check passes, 1 lists the lines to fix.
"""

import os
import re
import sys

USES = re.compile(r"""^\s*(?:-\s*)?uses:\s*['"]?([^'"\s#]+)['"]?\s*(#.*)?$""")
PINNED = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/[^@\s]+)?@[0-9a-f]{40}$")
DOCKER = re.compile(r"^docker://[^@\s]+@sha256:[0-9a-f]{64}$")
TAG_COMMENT = re.compile(r"^#\s*\S+")


def check_text(text):
    """The problems in one workflow file, as (line number, message)."""
    problems = []
    for number, line in enumerate(text.splitlines(), 1):
        m = USES.match(line)
        if not m:
            continue
        ref, comment = m.group(1), (m.group(2) or "").strip()
        if ref.startswith("./") or DOCKER.match(ref):
            continue
        if not PINNED.match(ref):
            problems.append((number, f"not pinned to a full commit SHA: {ref}"))
        elif not TAG_COMMENT.match(comment):
            problems.append((number, f"pinned but no '# <tag>' comment: {ref}"))
    return problems


RUNNER_WORDS = re.compile(r"self-hosted|daurosk-pc|squadala-wsl")
RUNNER_TEST = "runner.environment == 'self-hosted'"
RUNNER_TEST_NOT = "runner.environment != 'self-hosted'"
# The one gate (docs/ci-runner.md): daurosk's own push to main, integrate-*, release-* or a tag, or daurosk's
# manual run, in daurosk's repo, while the repo variable is on. pull_request (forks included) never matches.
def _gate(var):
    return (
        "(github.repository == 'daurosk/oot-remake-multiplayer'"
        " && github.actor == 'daurosk' && github.triggering_actor == 'daurosk'"
        f" && vars.{var} == 'on'"
        " && (github.event_name == 'workflow_dispatch' || (github.event_name == 'push'"
        " && (github.ref == 'refs/heads/main' || startsWith(github.ref, 'refs/heads/integrate-')"
        " || startsWith(github.ref, 'refs/heads/release-') || startsWith(github.ref, 'refs/tags/')))))"
    )


WSL = "fromJSON('[\"self-hosted\", \"linux\", \"x64\", \"squadala-wsl\"]')"
# windows job -> the Windows runner design (not installed: dropped 2026-11-26), else windows-latest.
RUNNER_LINE = (
    "runs-on: ${{ (github.event_name == 'push'"
    " && github.repository == 'daurosk/oot-remake-multiplayer'"
    " && github.actor == 'daurosk' && github.triggering_actor == 'daurosk'"
    " && vars.COOP_SELF_HOSTED == 'on')"
    " && fromJSON('[\"self-hosted\", \"windows\", \"x64\", \"daurosk-pc\"]')"
    " || 'windows-latest' }}"
)
# Linux jobs -> the WSL runners (squadala-wsl), else ubuntu-latest; python-tools picks it for its ubuntu row only.
RUNNER_LINE_LINUX = "runs-on: ${{ " + _gate("COOP_SELF_HOSTED_LINUX") + " && " + WSL + " || 'ubuntu-latest' }}"
RUNNER_LINE_MATRIX = (
    "runs-on: ${{ matrix.os == 'ubuntu-latest' && " + _gate("COOP_SELF_HOSTED_LINUX") + " && " + WSL + " || matrix.os }}"
)
RUNNER_LINES = (RUNNER_LINE, RUNNER_LINE_LINUX, RUNNER_LINE_MATRIX)


def check_runner(text):
    """Self-hosted runner misuse in one workflow file, as (line number, message)."""
    problems = []
    names_runner = False
    for number, line in enumerate(text.splitlines(), 1):
        code = line.strip()
        if code.startswith("#") or not RUNNER_WORDS.search(code.replace(RUNNER_TEST, "").replace(RUNNER_TEST_NOT, "")):
            continue
        names_runner = True
        if code not in RUNNER_LINES:
            problems.append((number, "self-hosted runner named outside the gated runs-on line"))
    if names_runner and re.search(r"^\s*pull_request_target\s*:", text, re.M):
        problems.append((1, "workflow names the self-hosted runner and uses pull_request_target"))
    return problems


def workflow_files(root):
    folder = os.path.join(root, ".github", "workflows")
    if not os.path.isdir(folder):
        return []
    return sorted(
        os.path.join(folder, name) for name in os.listdir(folder)
        if name.endswith((".yml", ".yaml"))
    )


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    failed = False
    for path in workflow_files(root):
        with open(path, encoding="utf-8") as f:
            text = f.read()
        problems = check_text(text) + check_runner(text)
        for number, message in problems:
            failed = True
            print(f"{os.path.relpath(path, root)}:{number}: {message}")
    if failed:
        print("Self-hosted runner: use the exact gated runs-on line (docs/ci-runner.md).")
        print("Pin each action to the commit of its tag, with the tag in a comment, e.g.")
        print("  uses: actions/checkout@<40 hex>  # v4.4.0")
        return 1
    print("All workflow actions are pinned to a commit SHA.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
