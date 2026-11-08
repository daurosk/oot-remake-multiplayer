# Contributing

## How work lands

- `main` is the only long-lived branch. It must always build and pass the content guard.
- Every change, by a person or an AI agent, goes on its own short branch and arrives through a pull request.
- A pull request merges only when its checks pass and the owner approves it.

## Several agents at once

AI agents can work on this repository at the same time when each one follows these rules:

1. **One task, one branch, one worktree.** Each agent works in its own git worktree on its own branch (`agent/<short-task-name>`), never directly in another agent's checkout or on `main`.
2. **One owner per area.** Before starting, an agent claims its area in its pull request title or draft pull request (for example "transport", "server", "presence overlay"). Two agents never edit the same files at the same time.
3. **Small pull requests, merged often.** Rebase on `main` before asking for review, so conflicts stay small.
4. **One build at a time on a shared PC.** Builds take a lock (`mkdir build.lock`, removed when done) so two large builds never run at once.
5. **Only the owner runs the game** for tests, and every run is written down with what it was meant to show.
6. **Plain-English reports.** Each pull request says what changed, how it was tested, and what is not yet proven.

## Before every commit

```bash
python tools/check_content.py
```

It fails if game files, keys, saves, shader caches or other large binaries are staged.
