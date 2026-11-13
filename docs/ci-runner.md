# Self-hosted CI runner (the owner's PC)

The Build workflow's `windows` job (MSVC build, ctest, kit script tests) can run on
the owner's PC instead of GitHub's `windows-latest`, which queues and takes 12 to
22 minutes. Everything else stays on GitHub: linux jobs (no WSL here),
python-tools, content guard, release attest, Fritchii autobuild.

## Who can use it

The repo is public and takes outside pull requests, so the runner must never run
code someone else wrote and sent. The job's `runs-on` picks the PC only when all of
these hold, and `windows-latest` otherwise:

- the event is a `push` (never `pull_request`, `pull_request_target` or a manual run),
- to `daurosk/oot-remake-multiplayer` itself,
- `github.actor` and `github.triggering_actor` are both `daurosk` (a re-run by
  anyone else goes to GitHub),
- the repo variable `COOP_SELF_HOSTED` is `on` (the off switch).

`tools/check_actions.py` (content guard, every push and pull request) fails if any
workflow names `self-hosted` or the `daurosk-pc` label anywhere but that exact
line, or names it in a workflow with `pull_request_target`. So a pull request that
widens the gate fails its own check, and it cannot reach the PC anyway: pull
requests always run on GitHub. Repo setting: fork pull request workflows need
approval for all outside contributors (`approval_policy: all_external_contributors`).

What daurosk pushes still runs here. Merging someone's pull request and pushing it
to `main` or `integrate-*` runs their code on this PC, as it runs it on a player's
PC once released: review it first.

## How it is locked down

- Local standard user `oot-ci` (Users group only, not an admin), random password
  nobody sees. The service runs as that user.
- `C:\actions-runner` has no inherited rights: SYSTEM, Administrators and `oot-ci`
  only. Jobs work in `C:\actions-runner\_work`.
- The owner's profile (`C:\Users\<owner>`, holding the repo, kits, keys, firmware,
  saves) grants only SYSTEM, Administrators and the owner, so `oot-ci` cannot read
  it; `install.ps1` refuses to run if it grants Users or Everyone.
- No secrets: the job has the read-only `GITHUB_TOKEN` only (`permissions: contents: read`).
- One runner = one job at a time. Its first step drops `Runner.Worker` to
  BelowNormal priority, and every later step inherits that, so a game test on this
  PC keeps the CPU.

## Install, switch, remove (owner, elevated PowerShell)

1. Open PowerShell with "Run as administrator", `cd` to the repo checkout.
2. `powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\install.ps1`
   (official actions/runner v2.338.2, SHA-256 checked against GitHub's release notes).
3. Once the `windows` job's `runs-on` change is on the branch you push, switch it on:
   `gh api -X POST repos/daurosk/oot-remake-multiplayer/actions/variables -f name=COOP_SELF_HOSTED -f value=on`
4. Off: `gh api -X PATCH repos/daurosk/oot-remake-multiplayer/actions/variables/COOP_SELF_HOSTED -f value=off`
   (jobs go back to `windows-latest`). Remove everything: `tools\ci-runner\uninstall.ps1`.

The runner updates itself; to move to a newer release by hand, run uninstall and
install with `-Version` and `-Sha256` from that release's notes. Tools it needs and
already finds on this PC: Visual Studio 2022 Build Tools (MSVC, CMake, Ninja),
Git for Windows (bash), Windows PowerShell.
