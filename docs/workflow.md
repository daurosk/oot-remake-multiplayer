# Session workflow

How the AI sessions that build co-op are organised. The live state (who is running, what is next, session ids) is kept outside this repository, in `..\coord-multiplayer\`, so none of it is committed.

## Roles

| Role | How many | What it does | What it never does |
|---|---|---|---|
| Owner (Caleb) | 1 | Decides the open questions, approves merges, starts the first coordinator. | |
| Coordinator | 1 at a time | Reports to the owner. Starts, steers, replaces and stops every worker. Keeps the handoff and the worker table current. Reviews worker results and opens pull requests for the owner. | Writes product code. Reads whole diffs or long logs (it sends a worker instead). Lets two workers touch the same files. |
| Worker | As many as the work needs | One task from one brief, in its own worktree on its own branch, named by its topic in lowercase with hyphens (`host-sampler`). Reports to the coordinator once, when done or when it reaches its context limit. | Starts other sessions or subagents. Merges. Edits `main` or another worker's files. Talks to the owner instead of the coordinator, unless the coordinator is gone. |

Only the coordinator starts and stops sessions. A worker that needs help, a decision or a replacement asks the coordinator.

## The context rule

No session may go past **250,000 context tokens**. A session that gets near it is replaced by a fresh one; it does not rely on compaction to keep going. This holds for the coordinator too.

Every session checks its own context use (the app's usage tool on itself) when it starts, at the start of every round of work, and before any large read.

| Context used | Worker | Coordinator |
|---|---|---|
| under 150k | Work normally. | Work normally. |
| 150k | | Start no new long jobs; prepare the handoff. |
| 175k | | **Hand off now** (below). |
| 200k | **Stop new work.** Finish or back out the current step, commit, push, write the report with what is left, send it. | |
| 225k | Must already have reported. The coordinator stops it. | Must already have handed off. |
| 250k | Never reached. | Never reached. |

The coordinator's line is lower because its handoff costs context and its replacement must be running before it stops.

The coordinator also checks each running worker's context every round. A worker past 210k that has not reported gets a message to report now; a worker past 225k is stopped and replaced from its last pushed commit.

## Replacing a worker

1. The worker commits, pushes its branch and writes its report: what is done, what is proven, what is left, the exact next step.
2. The coordinator starts a replacement with the same brief plus that report, on the same branch, named `<task>-2`, `-3` and so on.
3. The coordinator stops and archives the old worker and updates the worker table.

## Replacing the coordinator

1. The old coordinator writes a fresh `HANDOFF.md` (replace it, do not append), updates `WORKERS.md`, and starts the new coordinator with the coordinator brief and the handoff.
2. The new coordinator reads them, checks every worker in `WORKERS.md` is where the table says, writes its own session id into the `COORDINATOR` file and messages the old one: "taken over".
3. The old coordinator forwards anything that reached it during the swap, then detaches every session it started (each worker and the new coordinator) so their reports stop reaching it. It tells the owner who the new coordinator is, then stops. It does nothing else after "taken over". The new coordinator checks that every worker was detached and detaches any that were not.
4. Workers always read the `COORDINATOR` file at report time, never earlier, so their reports reach whoever is coordinating at that moment.

If the old coordinator is gone before step 2 (crashed, stopped, out of usage), the owner starts a new one from the same brief; the handoff and the worker table are enough to carry on.

## A round of the coordinator

1. Check own context. Read new messages and reports.
2. Check each running worker's context and state.
3. For each finished worker: read its report (not its whole diff), open the pull request and send a review worker to review it (a review worker reviews every pull request, from a worker or from an outside contributor; nothing merges unreviewed), start whatever that unblocks. Archive every session that has finished its work.
4. Update `WORKERS.md`; rewrite `HANDOFF.md` if anything changed.
5. Tell the owner, in a few lines: what finished, what started, what needs their decision.

## Rules every session follows

- Commit as the project's own account. No AI attribution lines or AI tool mentions in commits, pull requests or committed files.
- Run `tools/check_content.py` before every commit.
- One task, one branch, one worktree (`.worktrees/<branch>`). One build at a time on a shared PC (see [CONTRIBUTING.md](../CONTRIBUTING.md)).
- Nothing from the game in git, and nothing from other projects' repositories with their history or identity.
- Plain-English reports: what changed, how it was tested, what is not yet proven.

## Files outside the repository

| File in `..\coord-multiplayer\` | What |
|---|---|
| `COORDINATOR` | The current coordinator's session id, one line. |
| `HANDOFF.md` | The current state and what is next. Replaced at each coordinator handoff. |
| `WORKERS.md` | Every worker ever started: name, session id, branch, state. |
| `START-COORDINATOR.md` | The prompt that starts a coordinator. |
| `briefs/common.md` | The part of every worker's brief that is the same. |
| `briefs/coordinator.md` | The coordinator's brief. |
| `briefs/<task>.md` | One worker task. |
| `reports/<task>.md` | Each worker's final report, also sent by message. |
