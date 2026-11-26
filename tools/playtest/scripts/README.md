# Play-test scripts

One file per script, `lowercase-words.jsonl`. A script is a header and then one kit call per line, the same lines
the call log (`calls.jsonl`) holds, so `live.py replay` runs it:

```
# what: One line: what the script does.
# scene: InsideTheDekuTree
# start: Where the game must be before the first call.
# expect: What you see when it worked (state fields, the prompt, a shot you take after it).
# kit: 1
{"server":"oot-p1","tool":"place","args":{"x":5.45,"y":108.9,"z":6.15,"face_x":6.51,"face_z":7.25}}
{"server":"oot-p1","tool":"step","args":{"frames":30}}
```

- All five header lines are needed. `scene` is one map name. `kit` is the kit's call format when you made it
  (`KIT_VERSION` in `tools/autotest/kit_library.py`; today 1): a script from an older kit still runs if its calls
  are still valid, and the check says when one is not.
- A call is `{"server": "oot-p1" (to p4), "oot-pair" (with a `player` in args), "oot-playtest" or "live", "tool": ..., "args": {...}}`. The check reads
  each call against the tool's schema: a wrong tool name, a missing or unknown argument or a value out of range
  fails. Looks (`get_state`, `get_world`, shots) are skipped when a script is replayed.
- **Make one from a run:** play it once, then
  `python -I tools/autotest/kit_library.py save <session>/calls.jsonl --out tools/playtest/scripts/NAME.jsonl --what "..." --scene Map --start "..." --expect "..."`.
  The looks and the failed calls are left out. Read the file, take out wrong turns, and check it.
- Run one: `python -I tools/autotest/live.py replay --session <session folder> tools/playtest/scripts/NAME.jsonl`
  (`--dry-run` lists the calls and runs none). It stops at the first call that fails.
- Positions to use come from [`../data`](../data/README.md).
- No logs, no shots, no game files here. Write what you expect in `# expect`; a shot proves it, and stays on your PC.
