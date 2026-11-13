# Session workflow

How the AI sessions that build co-op are organised. The live state (who is running, what is next, session ids, briefs and reports) is kept outside this repository, in `..\coord-multiplayer\`, so none of it is committed.

## Roles

| Role | How many | What it does | What it never does |
|---|---|---|---|
| Owner (Alex) | 1 | Decides the open questions and starts the first coordinator. | |
| Coordinator | 1 or 2 (each on its own account; who runs what is in `..\coord-multiplayer\`) | Reports to the owner. Plans each chunk of work, writes the briefs, starts, steers, replaces and stops workers, reviewers and the integrator. Opens pull requests and merges. Keeps the handoff and the worker table current. | Writes product code. Reads whole diffs or long logs (it sends a worker instead). Lets two workers touch the same files. |
| Worker | As many as the work needs | One task from one brief, in its own worktree on its own branch, named by its topic in lowercase with hyphens (`host-sampler`). Writes code, unit tests and docs, pushes, lets CI build and test it, and writes its report with a "check in game" list. | Builds the emulator, runs the game or takes the build lock. Starts other sessions or subagents. Merges. Edits `main` or another worker's files. |
| Reviewer | One per branch or pull request | Reviews one branch it did not write (below) and writes what must change. | Reviews its own work. |
| Integrator | 1 per chunk | Merges the chunk's reviewed branches into one integration branch, builds once (unit tests, emulator, kits), fixes merge and compile errors, then runs one big in-game test with the autotest. | Starts features of its own. |

Only coordinators start and stop sessions. A worker that needs help, a decision or a replacement asks its coordinator.

## How a chunk of work goes

1. The coordinator plans a chunk: a set of branches that ship together, each with a brief.
2. Each worker writes its change with unit tests and pushes its branch. There are **no per-branch builds and no per-branch game runs**: CI building and running the unit tests is the proof at this step.
3. A reviewer reviews each branch. Fixes go back to the branch's author.
4. The integrator merges the reviewed branches into `integrate-pN`, builds everything once and runs **one real in-game test turn** (`tools/autotest`, two kits) that covers every branch's "check in game" list, and reports a result per check ([testing.md](testing.md)).
5. A failed check goes back to its branch's author; the fix joins the next chunk unless it blocks the release.
6. The release pass merges the integration branch into `main`, publishes the release, updates the README and tidies the issues (below).

Only one session builds on a PC at a time (`build.lock` in the repo root) and only one runs the games at a time (`..\coord-multiplayer\TEST-TURN`).

## Context

Sessions stay lean: they search instead of reading whole files, keep tool output short, commit and push often, and keep their report file current, so a session that stops loses nothing. A session that runs low on context is replaced by a fresh one from its last pushed commit and its report; it does not rely on compaction.

## Replacing a worker

1. The worker commits, pushes its branch and writes its report: what is done, what is proven, what is left, the exact next step.
2. The coordinator starts a replacement with the same brief plus that report, on the same branch.
3. The coordinator stops and archives the old worker and updates the worker table.

## Replacing a coordinator

1. The old coordinator writes a fresh `HANDOFF.md` (replace it, do not append), updates `WORKERS.md`, and starts the new coordinator with the coordinator brief and the handoff.
2. The new coordinator reads them, checks every worker in `WORKERS.md` is where the table says, writes its own session id into the `COORDINATOR` file and messages the old one: "taken over".
3. The old coordinator forwards anything that reached it during the swap, then detaches every session it started so their reports stop reaching it. It tells the owner who the new coordinator is, then stops.
4. Workers read the `COORDINATOR` file at report time, never earlier, so their reports reach whoever is coordinating at that moment.

If the old coordinator is gone before step 2 (crashed, stopped, out of usage), the owner starts a new one from the same brief; the handoff and the worker table are enough to carry on.

## Rules every session follows

- Commit as the project's own account. No AI attribution lines or AI tool mentions in commits, pull requests or committed files.
- Run `tools/check_content.py` before every commit.
- One task, one branch, one worktree (`.worktrees/<branch>`).
- Nothing from the game in git, and nothing from other projects' repositories with their history or identity.
- Plain-English reports: what changed, how it was tested, what is not yet proven.
- A branch that resolves an issue says so: `Fixes #N` for what is fully resolved and proven (in the game, when the issue is about the game), `Part of #N` otherwise. The coordinator puts the `Fixes #N` lines in the pull request body and the merge commit, so the issue closes when `main` is pushed.
- Every release updates README.md in the same pass: "Play the test release" (it links the releases page, latest, by no name), what works and the known issues, and the status rows. A release is not done until the README on main says what it brings. No file in the repo names a release (old or new): `tools/check_content.py` fails on one; the name goes only into the GitHub release and its notes.
- Every release tidies the issues in the same pass: issues that are done (with proof) or obsolete are closed, the rest are labelled, and [known-issues.md](known-issues.md) is rewritten for that release.
- Release notes start with a short "New here?" part for new players: which file to download, the [setup guide](setup.md), and that each player needs their own copy of the game.
- Release kits that leave the PC are built from a neutral folder (`.worktrees/<topic>`) on a branch named by its topic.

## Files outside the repository

| File in `..\coord-multiplayer\` | What |
|---|---|
| `COORDINATOR` | The current coordinator's session id, one line. |
| `HANDOFF.md` | The current state and what is next. Replaced at each coordinator handoff. |
| `WORKERS.md` | Every worker ever started: name, session id, branch, state. |
| `TEST-TURN` | Which session may run the games now, and the queue. |
| `COORD-LOG.md` | One line per event (pushed, reviewed, merged). |
| `START-COORDINATOR.md` | The prompt that starts a coordinator. |
| `briefs/common.md` | The part of every worker's brief that is the same. |
| `briefs/coordinator.md` | The coordinator's brief. |
| `briefs/<task>.md` | One worker task. |
| `reports/<task>.md` | Each worker's and reviewer's report. |
