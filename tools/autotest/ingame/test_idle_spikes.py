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
"""
import json
import os
import re
import time

import pytest

import solo
from test_host_menu import SETTLE, VCORES
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


def judge_idle(got):
    """(bad, note, facts) for the settled windows `got` ([(window, lines)])."""
    spiky = [i for i, ((_, _, b), _) in enumerate(got) if b[OVER] > 0]
    mean = sum(f for (f, _, _), _ in got) / max(1, len(got))
    gaps = [(b - a) * 5 for a, b in zip(spiky, spiky[1:])]
    waits = [float(m.group(1)) for _, ls in got for m in map(VCORES.search, ls) if m]
    note = "A solo idle: %d windows, fps mean %.1f, windows with an over-40 present %s (gaps %s s), vcores waits max %s ms" % (
        len(got), mean, spiky or "none", gaps or "-", "%.1f" % max(waits) if waits else "none")
    facts = {"windows": [{"i": i, "fps": f, "s": s, "buckets": b} for i, ((f, s, b), _) in enumerate(got)],
             "spiky": [{"i": i, "over40": got[i][0][2][OVER], "next_to": got[i][1][-KEEP:]} for i in spiky],
             "gaps_s": gaps, "mean_fps": mean}
    bad = []
    if spiky:
        bad.append("%s: a freeze (present over 40 ms) in solo idle" % note)
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
    if pair.out_dir:
        with open(os.path.join(pair.out_dir, "idle-spikes-A.json"), "w", encoding="utf-8") as f:
            json.dump(facts, f, indent=1)
    assert not bad, "; ".join(bad)
