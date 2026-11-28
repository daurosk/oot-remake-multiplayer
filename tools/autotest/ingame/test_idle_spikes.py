"""Row 322 (player report 11-28, v0.12.0): solo idle in play must not freeze every ~10 s.

Player text (coord reports/player-reports.md row 322, untrusted): "on the normal longshot build my game runs fine but
... on the squadala build my game freezes roughly every ten seconds"; an older build does not. Row 315's 60 s heap
walk (853e67202) is not a 10 s cadence, so this test MEASURES where the freezes fall and what ran next to each one.

| Check | Source |
|---|---|
| game A alone, co-op idle (no Host/Join: the player was not in a session) | the --solo boot (solo.py MARK, prove()) |
| in play as child Link, then no input at all | need_child_in_play (test_rows); the bridge `resume`, nothing pressed |
| WINDOWS full 5 s windows of real time (>= 3 min) | Longshot's 'vi:' windows (test_unfocus WINDOW/BUCKETS/windows, ls31-src host/nn_vi.c:704-705) |
| no freeze: no present over 40 ms in any window after the settle | the windows' 'over 40' bucket (test_host_menu SETTLE drops the partial window and the bridge hold's end) |
| what ran next to each spike | every module/host line between the window before and the spiky one that matches NEXT_TO, kept in <out>/idle-spikes-A.json |

The pass line is the PORT AUTHOR'S DECISION (fps-spike-0121-4), not the player's: the player's signature is one freeze
about every 2nd 5 s window; here NO window after the settle may hold a present over 40 ms, and the mean fps stays
>= 90 % of the 60 cap (test_unfocus MIN_SHARE). The json gives each spiky window's index, so the period (5 s x the
index gaps) is read from it, on v0.12.0 and on the fixed build alike.

Hosted (test_hosted_idle_no_spikes, --boot live-kakariko-host; the player: freezes once he hosts or joins): A hosts by its
own menu and B joins (test_host_menu.prove_hosting: TEST_HOST_ON, LISTENING, ANNOUNCED on A, WELCOMED on B), both idle
WINDOWS windows; the same judge on each game, each spiky window with `memory_line` (a 'coop: memory:' / 'coop: host:
memory:' line ran next to it, MEMORY), in <out>/idle-spikes-hosted-A.json and -B.json. Cause (fps-spike-0121-6): v0.12.0's
memory lines walk every heap (row 271); fix 853e67202 reads counters only.
"""
import json
import os
import re
import time

import pytest

import solo
from test_host_menu import HOST_BOOT, NO_CRASH, SETTLE, VCORES, prove_hosting
from test_rows import need_child_in_play
from test_unfocus import BUCKETS, CAP_FPS, MIN_SHARE, POLL_S, WINDOW, windows

WINDOWS = 36          # full 5 s windows judged: 180 s of solo idle (the brief's >= 3 min; the player's ~10 s period x 18)
OVER = 5              # the BUCKETS index of 'over 40' (ls31-src host/nn_vi.c:705)
KEEP = 30             # lines kept next to one spiky window (the json stays small)
CUT = 240             # characters kept of one line
# What may have run next to a spike: module lines, the vcores stall line, the renderer's slow submit/present, the
# controller-read gap, saves, heaps (row 315's family). Not the renderer's 5 s counter lines (vk: a frame..., mirrors).
NEXT_TO = re.compile(r"coop: |vcores: .*(waited|held it)|slow (submit|present)|hid: the game read no|"
                     r"memory heaps|heap|save|nvn: GPU waits")
# The row 322 cause (fps-spike-0121-6): the game's memory line every 60 s (coop_hookshot.cpp, solo too) and the host's
# every 30 s (coop_host_menu.cpp, 'coop: host: memory: ' + ProcessMemoryText) walked every heap in v0.12.0.
MEMORY = re.compile(r"coop: (host: )?memory: ")


def windows_with_lines(lines):
    """[(window, [lines next to it])] for each full window in `lines`, in order (the same windows as windows())."""
    out, between, fps = [], [], None
    for ln in lines:
        if WINDOW.search(ln):
            fps = ln
            continue
        if BUCKETS.search(ln) and fps:
            w = windows([fps, ln])
            out.append((w[0], between))
            between, fps = [], None
            continue
        if NEXT_TO.search(ln):
            between.append(ln[:CUT])
    return out


def judge_idle(got, who="A solo idle"):
    """(bad, note, facts) for the settled windows `got` ([(window, lines)]); `who` names the game in the note."""
    spiky = [i for i, ((_, _, b), _) in enumerate(got) if b[OVER] > 0]
    mean = sum(f for (f, _, _), _ in got) / max(1, len(got))
    gaps = [(b - a) * 5 for a, b in zip(spiky, spiky[1:])]
    waits = [float(m.group(1)) for _, ls in got for m in map(VCORES.search, ls) if m]
    note = "%s: %d windows, fps mean %.1f, windows with an over-40 present %s (gaps %s s), vcores waits max %s ms" % (
        who, len(got), mean, spiky or "none", gaps or "-", "%.1f" % max(waits) if waits else "none")
    mem = [i for i, (_, ls) in enumerate(got) if any(MEMORY.search(ln) for ln in ls)]
    facts = {"windows": [{"i": i, "fps": f, "s": s, "buckets": b} for i, ((f, s, b), _) in enumerate(got)],
             "spiky": [{"i": i, "over40": got[i][0][2][OVER], "memory_line": i in mem, "next_to": got[i][1][-KEEP:]}
                       for i in spiky],
             "memory_windows": mem, "spiky_on_memory": len(set(spiky) & set(mem)), "gaps_s": gaps, "mean_fps": mean}
    bad = []
    if spiky:
        bad.append("%s: a freeze (present over 40 ms) while idle (%d of them next to a memory line)" % (
            note, facts["spiky_on_memory"]))
    if mean < MIN_SHARE * CAP_FPS:
        bad.append("%s: mean < %.0f fps" % (note, MIN_SHARE * CAP_FPS))
    return bad, note, facts


@solo.MARK
@pytest.mark.budget(300)
@pytest.mark.row("#322: no freeze every ~10 s in solo idle (v0.12.0 player report)")
def test_solo_idle_no_spikes(pair):
    with pair.setup():
        solo.prove(pair)
        need_child_in_play(pair)
        mark = pair.log["A"].mark()
    pair.call("A", "resume")
    got = []
    try:
        while True:
            got = windows_with_lines(pair.log["A"].since(mark))[SETTLE:]
            if len(got) >= WINDOWS:
                break
            pair.deadline.check("%d full vi windows of A (have %d)" % (WINDOWS, len(got)))
            time.sleep(POLL_S)
    finally:
        pair.call("A", "pause")
    bad, note, facts = judge_idle(got[:WINDOWS])
    pair.notes.append(note)
    keep(pair, "idle-spikes-A.json", facts)
    assert not bad, "; ".join(bad)


def keep(pair, name, facts):
    if pair.out_dir:
        with open(os.path.join(pair.out_dir, name), "w", encoding="utf-8") as f:
            json.dump(facts, f, indent=1)


@pytest.mark.needs_boot(HOST_BOOT)
@pytest.mark.budget(360)
@pytest.mark.row("#322 hosted: no freeze every 30/60 s while hosting idle (the host's and the game's memory lines)")
def test_hosted_idle_no_spikes(pair):
    """A hosts by its own menu, B joins (test_host_menu's boot and set-up proof), both idle in play with no input
    for WINDOWS full windows each; the same judge as the solo test, on each game; every spiky window says whether a
    memory line ran next to it (the player: 'fine with co-op off, freezes once I host or join')."""
    with pair.setup():
        need_child_in_play(pair)
        prove_hosting(pair, "322")
        marks = {x: pair.log[x].mark() for x in ("A", "B")}
    for x in ("A", "B"):
        pair.call(x, "resume")
    got = {}
    try:
        while True:
            got = {x: windows_with_lines(pair.log[x].since(marks[x]))[SETTLE:] for x in ("A", "B")}
            if all(len(g) >= WINDOWS for g in got.values()):
                break
            pair.deadline.check("%d full vi windows of A and B (have %d, %d)" % (WINDOWS, len(got["A"]), len(got["B"])))
            time.sleep(POLL_S)
    finally:
        for x in ("A", "B"):
            pair.call(x, "pause")
    bad = []
    for x, who in (("A", "A hosting idle"), ("B", "B joined idle")):
        b, note, facts = judge_idle(got[x][:WINDOWS], who)
        pair.notes.append(note)
        keep(pair, "idle-spikes-hosted-%s.json" % x, facts)
        bad += b
    for x in ("A", "B"):
        pair.never_log(x, NO_CRASH)
    assert not bad, "; ".join(bad)
