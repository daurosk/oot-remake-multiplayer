"""PROBE for revive-flicker-0121 (banner-steady-0121): does each set of the banner's text blank the line, and for how long?

banner-steady-53d40ab3-2 (00:15 UTC 11-27) FAILed test_revive_banner_steady 21 of 24 frames: the banner bar stays up,
only its TEXT is missing after a set (Banner::Step -> TextRing::PutFixed(slot 0) + Banner::Set -> SetPane =
f_8045b970(layout, "T_Text_00", ...), coop_native_ui.cpp). This probe measures that with no reviver: B lies downed,
A stands away and holds nothing, so B's line ('You are down  m:ss') changes only with its clock, once a second.

| Test | Kind |
|---|---|
| test_probe_banner_set | B shot on every frame for PROBE_FRAMES frames (A steps 1 each); per frame: the words in the banner band (test_revive_banner.words) and the banner's text sets (the bridge's `text`); NO-REF always (a probe, never PASS) |

Its result (notes + <out>/probe-banner-set/frames.json): for each set the frames until the line is seen again
('blank after set'), and the dark frames with no set before them. Blank runs after each set = the set itself blanks the
text (candidate (a) re-layout, or (b) the in-place rewrite under the box's kept pointer: probe 2 tells them apart).
No blank after a set = the blink comes from the set RATE while revived, not from one set.

Runs only with --env BT_PROBE_BANNER_SET=1 (needs_env, as test_probe_ocarina.py: every other run deselects it, so no rc
pass and no default fake suite carries an always-NO-REF test; the game reads no such switch).
"""
import json
import os

import pytest

import test_dbno_crawl as dc        # noqa: E402  (the row-44 helpers: place, rules, no fairy, down, restore)
import test_revive_banner as rb     # noqa: E402  (words, banner_sets, keep, BAND, SEEN, MIN_WORDS)

PROBE_FRAMES = 180     # 3 s at 60 fps: at least two clock changes of B's line (one a second)
KEEP_EVERY = 20        # a bright frame's shot kept this often (every dark frame's shot is kept)
AWAY_M = 6.0           # A this far from B: no 'Hold A to revive' for A, no reviver for B (dc.REACH_M is far less)
OUT = "probe-banner-set"
PROBE_ENV = "BT_PROBE_BANNER_SET=1"   # (the opt-in switch: see the docstring)


def runs(samples, top, set_before=False):
    """(after_set, stray): for each set, the frames from it until the line is seen again; dark frames whose dark run
    has no set in it or on the frame just before it (a set is read after its frame's shot: its blank starts at the
    next shot). `set_before`: the read just before the first shot had a set."""
    seen = lambda s: s["words"] >= max(rb.MIN_WORDS, rb.SEEN * top)   # noqa: E731
    after_set, stray = [], []
    for k, s in enumerate(samples):
        if s["sets"]:
            n = 0
            for t in samples[k + 1:]:
                if seen(t):
                    break
                n += 1
            after_set.append({"i": s["i"], "frame": s["frame"], "dark_here": not seen(s), "blank_after": n,
                              "text": (s["texts"] or [""])[-1]})
    dark_run_has_set, prev_set = False, set_before
    for s in samples:
        if seen(s):
            dark_run_has_set, prev_set = False, bool(s["sets"])
            continue
        dark_run_has_set = dark_run_has_set or prev_set or bool(s["sets"])
        prev_set = False
        if not dark_run_has_set:
            stray.append(s["i"])
    return after_set, stray


@pytest.mark.budget(600)   # (no look mark: no check_shot, its kept shots are the owner's copies only)
@pytest.mark.needs_env(PROBE_ENV)
@pytest.mark.row("PROBE revive-flicker-0121: does one set of the banner's text blank its line, and for how many frames (B downed, no reviver)")
def test_probe_banner_set(pair):
    with dc.restored(pair, {}):
        with pair.setup():
            pair.need_boot_place("child")
            dc.to_place(pair, "child", "KokiriForest_Child", "FrontDekuTree")   # test_dbno_crawl's open ground
            dc.rules(pair, 240)          # (kDbnoMaxBleedS: no bleed-out during the probe)
            dc.no_fairy(pair, "B")
        dc.down(pair)
        pair.expect_log("B", dc.LYING_CHILD, max_frames=600)
        pair.step_both(30)
        b = pair.state("B")["pos"]
        pair.act("A", "place %.2f %.2f %.2f" % (b[0] + AWAY_M, b[1], b[2]))
        pair.step_both(10)
        apart = dc.dist(pair.state("A")["pos"], pair.state("B")["pos"])
        assert apart > dc.REACH_M, "A not away from B (%.2f m)" % apart
        since = rb.banner_sets(pair, "B", 0)[1]
        pair.step_both(10)         # (a set just read has had its frames before the first shot)
        pre, since, _ = rb.banner_sets(pair, "B", since)
        before = pair.state("B").get("banner")
        samples, paths, hooked = [], {}, False
        for i in range(PROBE_FRAMES):
            r = pair.call("B", "shot", name="probe-banner-set-f%03d-B" % i)   # (B runs one frame, no input)
            pair.call("A", "step", frames=1)
            texts, since, hooked = rb.banner_sets(pair, "B", since)
            paths[i] = r.get("path")
            samples.append({"i": i, "frame": r.get("frame"), "words": round(rb.words(r["path"]), 4),
                            "sets": len(texts), "texts": [t.get("text") for t in texts][-3:],
                            "set_frames": [t.get("frame") for t in texts][-3:]})
        after = pair.state("B").get("banner")
        top = max(s["words"] for s in samples)
        after_set, stray = runs(samples, top, set_before=bool(pre))
        if pair.out_dir:
            os.makedirs(os.path.join(pair.out_dir, "revive-banner", OUT), exist_ok=True)
        for s in samples:          # every dark frame's shot, and a bright one every KEEP_EVERY, kept for the owner
            if s["words"] < max(rb.MIN_WORDS, rb.SEEN * top) or s["i"] % KEEP_EVERY == 0 or s["sets"]:
                rb.keep(pair, paths[s["i"]], "%s/f%03d-B" % (OUT, s["i"]))
        report = {"samples": samples, "top": top, "hooked": hooked, "after_set": after_set, "stray_dark": stray,
                  "banner": {"before": before, "after": after, "deltas": rb.banner_deltas(before, after)}}
        if pair.out_dir:
            with open(os.path.join(pair.out_dir, "revive-banner", OUT, "frames.json"), "w", encoding="utf-8") as f:
                json.dump(report, f, indent=1)
        pair.notes.append("B sets (i:frame:dark_here:blank_after:text): %s" % " ".join(
            "%d:%s:%s:%d:%r" % (a["i"], a["frame"], int(a["dark_here"]), a["blank_after"], a["text"]) for a in after_set))
        pair.notes.append("B dark frames with no set before them: %d %s" % (len(stray), stray[:40]))
        pair.notes.append("B words per frame (best %.4f): %s" % (top, " ".join("%.3f" % s["words"] for s in samples)))
        pair.notes.append("banner state deltas: %s (hooked %s)" % (report["banner"]["deltas"], hooked))
        assert top >= rb.MIN_WORDS, "no banner line in any of B's %d frames (best %.4f of the band)" % (len(samples), top)
        blanks = [a["blank_after"] for a in after_set]
        pair.unproven("probe: %d sets in %d frames, blank frames after each set %s, %d dark frames with no set" % (
            len(after_set), len(samples), blanks[:20], len(stray)))
