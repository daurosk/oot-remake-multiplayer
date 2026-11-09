# Contributing

Anyone can contribute: code, testing, desync reports, docs. Fork the repository, make your change on a branch, and open a pull request. Questions and ideas are welcome in [Discussions](https://github.com/daurosk/oot-remake-multiplayer/discussions) or on [Discord](https://discord.gg/GwtRKAqng5).

## The one hard rule

Nothing that came out of the game goes into the repository: no game files, keys, firmware, saves, extracted data, shaders, shader caches, audio, movies or builds that contain them. Not in commits, not in issues, not in pull requests. Only information about the game goes in: addresses, offsets, hashes, and code we wrote.

Two checks enforce this:

- CI runs `tools/check_content.py` on every push and pull request.
- The same check runs before each local commit once you enable the hook:

  ```sh
  git config core.hooksPath .githooks
  ```

A failing check is never bypassed with `--no-verify`. If it flags a file by mistake, change the check in a separate pull request and say why.

Links to game files, keys or firmware in issues or pull requests get the issue closed and the content removed.

## Code from other projects

Code may be copied only from projects whose licence is compatible with GPL-3.0 (MIT, BSD, ISC, zlib, Apache-2.0, or GPL-3.0 itself). Each copy keeps its original header and is credited in the pull request and next to the code.

Closed projects (the TOTK Online mod and its server) may be studied for how they behave, but nothing from them is copied.

## Reporting bugs and desyncs

Use the issue forms. Include the co-op build version, the emulator or recomp version, the game version, how many players, who hosted, and the steps to reproduce. For a desync, say what each player saw; clips from both sides help the most. Logs are welcome; recordings of long play sessions are not needed.

## Pull requests

- One topic per pull request. Small ones are reviewed faster.
- Describe the problem, then what changed.
- Say how you tested it: which build, how many clients, how long. Fill in the pull request template's "Not yet proven" part honestly.
- The build and the unit tests pass (see "Building" in the [README](README.md)), and `tools/check_content.py` passes. Run both before you push.
- If you change a message layout, change [docs/protocol.md](docs/protocol.md) in the same pull request.
- Commit messages are plain sentences, without `feat:` or other prefixes.
- AI-assisted code is welcome. You are responsible for having read and run it.

By opening a pull request you agree that your contribution is licensed under GPL-3.0, the licence of this project.

## Review

Every pull request is reviewed by a review session before it is merged. This holds for everyone: outside contributors, the maintainer, and the project's own workers. Nothing merges unreviewed, however small.

A review session is one of the maintainer's AI sessions, started only to review that pull request ([docs/workflow.md](docs/workflow.md)). It did not write the change. For a pull request from outside it writes what it found as a comment on the pull request; you answer or push fixes to the same branch, and it looks again. Reviews of the project's own workers are written in the project's notes, outside this repository.

A review checks that:

- the change builds, and the unit tests and `tools/check_content.py` pass (CI runs them too);
- it does what the pull request says, and stays on its one topic;
- a changed message layout or meaning comes with a new protocol version and with [docs/protocol.md](docs/protocol.md) changed in the same pull request;
- no game files, keys, firmware, saves or anything else that came out of the game are in it, and no copied code without a compatible licence and credit;
- what was tested and what is not yet proven is said honestly.

## Code and docs

- Code, comments, commit messages and documentation are in English.
- Plain words over jargon in docs and pull requests.

## AI sessions working on this repository

The maintainer's own AI sessions follow [docs/workflow.md](docs/workflow.md): one task, one branch, one worktree; one owner per area; small pull requests, each reviewed by a review worker, merged often.
