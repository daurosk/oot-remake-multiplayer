# Shared play-test library

What testers (people and agents) write down for the next tester. Two folders:

| Folder | What goes in |
|---|---|
| [`scripts/`](scripts/README.md) | A script: kit calls that set a scene up or run one check. `live.py replay` runs it. |
| [`data/`](data/README.md) | Facts about the game we wrote down: named positions and routes per scene, menu paths, stuck spots. |

How to drive the games first: [docs/playtest-agents.md](../../docs/playtest-agents.md).

## The hard rule

Only information about the game goes in: numbers and names we wrote down, and calls we wrote. **Never a game
file, a key, a save, a log, a screenshot, an extracted value list or a build** ([AGENTS.md](../../AGENTS.md)).

- Only `.md`, `.json` and `.jsonl` files, each under 256 KB. No pictures: a shot stays on your PC (`<log dir>/shots/`).
- No local path, user name, address, IP or password in a file. `tools/check_content.py` refuses a drive path, an IP
  address and a user folder in this folder.
- A text over 300 characters is a pasted log, not a note: the check refuses it.

## Adding a file

A normal pull request: fork, branch, add the file, run `python tools/autotest/kit_library.py check` and
`python tools/check_content.py`. CI runs both and `tools/autotest/tests/test_kit_library.py`, so a script that
calls a tool that does not exist, or a data file with a wrong key, fails there. Say in the pull request which game
version and which kit version the file was made with, and whether you ran it in a game or only checked it.
