# Self-hosted CI runners (the owner's PC)

GitHub's free queue holds our Build runs for hours, so our own runs use the owner's PC:
3 Linux runners `squadala-wsl-1..3` (label `squadala-wsl`) in a WSL distro of their own,
`squadala-ci` (Ubuntu 24.04; WSL capped at 20 GB, 24 threads). They run `scope`, `python-tools`
(ubuntu row) and `linux` (gcc, clang, asan, tsan) while the repo variable `COOP_SELF_HOSTED_LINUX`
is `on`.

They take only daurosk's own push to `main`, `integrate-*`, `release-*` or a tag, and daurosk's
manual runs (workflow_dispatch: only people with write access can start one). Pull requests,
forks included, always run on GitHub. Stays on GitHub: `windows` and `python-tools` on Windows,
`linux-longshot` (built on Ubuntu 22.04 for its glibc 2.35 baseline; the WSL distro is 24.04),
`windows-hook` (fork pull requests only), Content guard, release attest. The job matrix is the
same on both: a run on our runners is the full CI. The runners never carry the PC's name (it is
the owner's name, and public job logs print the machine name): the distro's hostname is
`squadala-ci` (wsl.conf `[network]`, checked by install-wsl.ps1).

The Windows runner described further down (oot-ci, `daurosk-pc`) is NOT installed: the owner
dropped it on 2026-11-26 (it would show the PC's name in job logs). `COOP_SELF_HOSTED` stays unset.

**Off switch / runner offline.** A job sent to a runner that is offline waits in the queue (up to
GitHub's 24 h). Set the variable to `off` and new runs use GitHub's runners again:
`gh api -X PATCH repos/daurosk/oot-remake-multiplayer/actions/variables/COOP_SELF_HOSTED_LINUX -f value=off`.
Runs already queued keep their runner: cancel and re-run them. Keep it `off` whenever WSL is held down.

**Memory and disk guard.** The games always win, in two halves:
- Before a job starts, its hook waits while a test-lane game or offline-queue job runs (a lane lock
  `oot-remake-multiplayer\laneN.lock`, or a first `LANE1:`/`LANE2:` line of `coord-multiplayer\TEST-TURN` that
  is not free/none: queue jobs, manual holders and owner-playing lines alike) or Windows' free commit is below
  75 GB, at most 5 h, then fails the job ("memory guard: waited ..."). It refuses at once when C: has less than
  60 GB free ("disk guard"). After every job a second hook empties the job's workspace.
  Do not drop the lane wait: on 2026-11-27 jobs run while a lane game ran (VM at 12 of 32 CPUs, 20 GB) dropped
  both games to 0.1-0.4 fps within 10 s (downed-move-53d40ab3-2 SETUP-FAIL, Build 46083381220).
- The offline queue's start gate (`coord-multiplayer\offline\lib\wsl-reserve.ps1`, Get-WslReserveGB) counts a
  live WSL VM as its whole `.wslconfig` memory cap already taken, so a game pair never starts into memory a
  running CI job can still grow into.
The hook reads the Windows side from `/run/squadala-ci/status` ("time, free commit GB, C: free GB, lanes busy
0/1"), fed every 15 s by `status-feed.ps1` (below; tests: `tools/ci-runner/wsl/status_feed_test.ps1`, windows
job). A stale, missing or old-format file counts as "not enough" (fail closed). Only the 0/1 enters the distro:
TEST-TURN text never reaches a public job log.

## Linux runners in WSL

`tools/ci-runner/wsl/`. Same gate as the Windows runner, in `hooks/job-started.sh` (root-owned,
no switches). The runners have a WSL distro of their own, `squadala-ci` (in
`%LOCALAPPDATA%\squadala-ci\wsl`); the owner's `Ubuntu-24.04` (local CI, user `lci`, needs `/mnt/c`)
is not changed. How `squadala-ci` is locked down:

- `%UserProfile%\.wslconfig` (one VM for every distro, so local CI shares the cap): `memory=20GB` (3 jobs at
  make -j4), `processors=12` (of 32), `swap=8GB` (a file on the VM's side, not Windows commit), `autoMemoryReclaim=dropCache`.
  Never raise them: the test lanes need the rest of the commit.
- `/etc/wsl.conf`: no Windows interop (a job cannot start Windows programs as the owner) and no
  Windows drives (`/mnt/c` is gone). install-wsl.ps1 checks both after the restart.
- Jobs run as `squadala-ci`: no password, no sudo (root in WSL could mount C:), systemd units with
  `NoNewPrivileges`. `/opt/squadala-ci/bin/sudo` answers the workflow's sudo lines: `apt-get update`
  is skipped, `apt-get install` passes only for preinstalled packages (`setup.sh` PACKAGES),
  `sysctl -w vm.mmap_rnd_bits=28` only when it already is 28 (set at boot).
- Each runner folder `/opt/squadala-ci/runner-N` is root-owned; `squadala-ci` writes only `_work` and
  `_diag`. Shared: ccache `/var/cache/squadala-ccache` (20 GB cap) and the tool cache.
- Left as is: the distro reaches the network (it must, for GitHub) and the Windows host's NAT address,
  so only services bound to 127.0.0.1 on Windows are out of its reach (our listeners bind 127.0.0.1).

Install (owner, normal PowerShell, not elevated; free commit >= 40 GB):
`powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\wsl\install-wsl.ps1`. It copies the
scripts in, runs `setup.sh` as root, restarts the distro, registers `squadala-wsl-1..3` (token piped on
stdin only), and registers the logon task `squadala-ci status feed` (`status-feed.ps1`, hidden), whose
`wsl.exe` also keeps the distro and its runners up. Then switch on:
`gh api -X POST repos/daurosk/oot-remake-multiplayer/actions/variables -f name=COOP_SELF_HOSTED_LINUX -f value=on`.
The runners start at logon (the task); the PC must be logged in. Remove: switch off, then for each runner
`wsl -d squadala-ci -u root -- systemctl disable --now squadala-ci-runner@N` and delete it in
Settings > Actions > Runners; `Unregister-ScheduledTask 'squadala-ci status feed'`.

## Windows runner (NOT installed, dropped 2026-11-26): who could use it

The repo is public and takes outside pull requests, so the runner must never run
code someone else wrote and sent. Two layers:

1. **The runner's own hook (the real gate).** Before any step of any job, the
   runner runs `C:\ProgramData\oot-ci\job-started.ps1` (from
   `tools/ci-runner/job-started.ps1`). It fails the job unless it is a `push` to
   `daurosk/oot-remake-multiplayer` by `daurosk` (`GITHUB_ACTOR` and
   `GITHUB_TRIGGERING_ACTOR`), on `main`, an `integrate-*` branch or a tag. It
   also refuses every job while any guarded folder (below) is open to the runner
   user. It has no switches: no environment variable changes what it checks
   (the checks live in `job-check.ps1`, which the tests load on their own). The
   hook, `job-check.ps1` and its setting (`ACTIONS_RUNNER_HOOK_JOB_STARTED` in the runner's `.env`
   and the machine environment) are owned by Administrators and only readable by
   the runner user, so neither a pull request nor a job can change them. Tests:
   `tools/ci-runner/job_started_test.ps1` (windows job).
2. **The workflow's `runs-on`** sends the job to the PC only for that same push
   and only while the repo variable `COOP_SELF_HOSTED` is `on`; otherwise
   `windows-latest`. This keeps normal runs off the PC, but it is not protection:
   GitHub runs a pull request with the pull request's own copy of the workflow,
   which can name the runner however it likes. `tools/check_actions.py` flags such
   an edit in Content guard so a reviewer sees it, but cannot stop that run.

Repo setting: workflows from fork pull requests need approval for every outside
contributor (`approval_policy: all_external_contributors`). **Before clicking
"Approve and run" on an outside pull request whose diff touches `.github/`, read
that diff.** The hook refuses such a job anyway, but a refused job is the signal
that someone tried.

What daurosk pushes still runs here: merging someone's pull request into `main` or
`integrate-*` runs their CMake files, scripts and tests on this PC at build time.
Read outside pull requests for hostile build-time code before merging them.

## How it is locked down

- Local standard user `oot-ci` (Users group only; not in Administrators or Remote
  Desktop Users), random password nobody sees, denied interactive, RDP and
  network logon. The service runs as that user.
- `C:\actions-runner` is owned by Administrators; `oot-ci` may read and run it
  but writes only `_work` (jobs) and `_diag` (logs). It cannot change the
  runner, its `.env`, `.runner` or `.credentials`. The runner is registered with
  `--disableupdate` for that reason (see Updating).
- Deny ACEs for `oot-ci` (full, inherited) on every user profile (the owner's
  holds the repo, keys, firmware, saves) and `C:\Users\Public`, and on every
  top-level folder of every fixed drive except Windows, Program Files,
  ProgramData and Users (so the kits `C:\oot*`, `C:\OoT-Coop-*`, `C:\Games`,
  `C:\msys64`, server folders...). Drive roots and ProgramData get a Deny on
  creating files or folders in them (`tools/ci-runner/deny.ps1`). The list is
  saved in `C:\ProgramData\oot-ci\denied.txt`; uninstall removes exactly those.
- **New folders fail closed.** C:\ gives Authenticated Users Modify on every new
  top-level folder, so a kit made after the install (`C:\OoT-Coop-6`) starts
  open to `oot-ci`. The hook, running as `oot-ci`, tries to list every guarded
  folder at the start of each job and refuses the job if it can open any of
  them: "open to the runner user: <path>. Run tools\ci-runner\deny.ps1
  (elevated)". Run `deny.ps1` in an elevated PowerShell and jobs run again. Not
  covered: a subfolder inside a denied folder that does not inherit its
  parent's rights and grants Users itself (none on this PC at install time).
- The Deny ACEs name `oot-ci` only: the owner, and everything the owner runs
  (the game in `C:\oot-coop`, the kits, the repo, `.worktrees`, `coop-kit`
  lanes, `repo-backup`, all under the owner's profile), keeps the access it had.
  The first run walks every file under the denied folders to pass the Deny
  down, which can take several minutes on the profile and `C:\msys64`.
- No secrets: the job has the read-only `GITHUB_TOKEN` only (`permissions: contents: read`).
- One runner = one job at a time. Its first step drops `Runner.Worker` to
  BelowNormal priority, and every later step inherits that, so a game test on this
  PC keeps the CPU.
- Left as is: a job can read the runner's own credentials
  (`.credentials_rsaparams`; it has to, to run), so a hostile job that daurosk
  pushed could copy the key and act as this runner, picking up jobs with
  read-only tokens. That is why only daurosk's reviewed pushes get here at all.
  If it ever happens, run uninstall and install again: that registers a new key.

## Windows runner install, switch, remove (owner, elevated PowerShell; not done)

Use a fresh elevated PowerShell with no transcript on: the scripts pass the
registration token and the generated password to `config.cmd` on its command line.

1. Open PowerShell with "Run as administrator", `cd` to the repo checkout.
2. `powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci-runner\install.ps1`
   (official actions/runner v2.338.2, SHA-256 checked against GitHub's release
   notes). It prints every folder it denied.
3. Once the `windows` job's `runs-on` change is on the branch you push, switch it on:
   `gh api -X POST repos/daurosk/oot-remake-multiplayer/actions/variables -f name=COOP_SELF_HOSTED -f value=on`
4. Off: `gh api -X PATCH repos/daurosk/oot-remake-multiplayer/actions/variables/COOP_SELF_HOSTED -f value=off`
   (jobs go back to `windows-latest`). Set it off before the PC is off for long:
   with it on and the runner offline, windows jobs wait in the queue.
5. Remove everything (runner, service, hook, Deny ACEs, logon rights, profile,
   user): `tools\ci-runner\uninstall.ps1`. Safe to run twice. Every step runs
   even if one fails; the failures are listed at the end, and a second run does
   what is left. If the profile is still loaded it says so (the user and
   `C:\ProgramData\oot-ci` stay until then), and a run after a reboot finishes.
   install.ps1 stopped part way: run it again, it picks up where it stopped;
   once a runner is registered it says to run uninstall first.
6. A job failed with "open to the runner user": run
   `tools\ci-runner\deny.ps1` elevated, then re-run the job.

## Updating

The runner cannot update itself (its folder is read-only to it). GitHub stops
sending jobs to a runner about 30 days behind the newest release. To update: run
uninstall, then install with `-Version` and `-Sha256` from the new release's
notes (`gh api repos/actions/runner/releases/latest --jq .body`, the `win-x64`
line), and change the defaults in `install.ps1` in a pull request.

Tools it needs and already finds on this PC: Visual Studio 2022 Build Tools
(MSVC, CMake, Ninja: `ilammy/msvc-dev-cmd` puts the Build Tools' own Ninja on
the job's PATH; the owner's WinGet Ninja is in the denied profile), Git for
Windows (bash: the machine PATH has only `Git\cmd`, so the job's first step adds
`Git\bin`), Windows PowerShell. The runner's
`.env` sets `PSExecutionPolicyPreference=RemoteSigned` for its own processes, as
the user's default policy would refuse to run the hook and `powershell` steps.
