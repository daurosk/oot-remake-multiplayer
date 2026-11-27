"""0.12.1 owner issue (docs/known-issues.md "The revive message flickers", revive-flicker-0121): while a player is
being helped up, their message (the game's banner SimpleInfo_00 holding the down-but-not-out line, coop_hookshot.cpp
DrawScene -> CoopNativeUi::HoldBanner -> Banner::Step) blinks rapidly.

| Test | Kind |
|---|---|
| test_revive_banner_steady | B's banner shot frame by frame while A holds A (no blank frame), the banner's text sets counted on each game (the bridge's `text`, patch 0041), LOOK on each game |

B is downed (test_dbno_crawl's helpers: same place, rules, no fairy, hits), A stands beside it and holds A. Only B is
shot during the hold: a shot runs its game a frame with no input, and A letting go of A for a frame lets the revive go
(coop_dbno.cpp Reviver::Frame: holding_ && !button -> a release), so A's look is shot beside B before the hold.
"""
import contextlib
import json
import os
import shutil
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_dbno_crawl as dc   # noqa: E402  (the row-44 helpers: place, rules, no fairy, down, restore)

LAYOUT, PANE = "SimpleInfo_00", "T_Text_00"   # the banner and its text pane: docs/game-ui.md "The banner";
#                                                coop_native_ui.cpp Banner::Set (kNameT_Text_00)
# The banner's words in a shot, as fractions of the picture (x0, y0, x1, y1): 'You are down  0:05' at x 552-728,
# y 160-182 of 1280 x 720 in hs-dbno-child 20261125-083408 hs-dbno-child-B-lying-B.png; the band is wider than a long
# line ("P1 is reviving you  0:45  99%") and a little taller.
BAND = (0.30, 0.20, 0.70, 0.27)
WHITE = 200            # the banner's words: the game's white (each channel at least this)
SEEN = 0.4             # a frame shows the line when it has at least this share of the hold's brightest frame's words
MIN_WORDS = 0.002      # ... and at least this share of the band is words (a band with no line at all: none)
DEFAULT_HOLD_TENTHS = 50   # coop_dbno.h Rules revive_tenths (row 174): what the other rows play with
HOLD_TENTHS = 100      # '/dbno hold 100' (coop_dbno.cpp Parse 'hold', messages.hpp kDbnoMaxReviveTenths): 10 s
STEP = 3               # frames A holds A between B's shots (B runs them too: Pair.press)
SAMPLES = 24           # B's shots during the hold (each its own frame)
MIN_REVIVING = 8       # of them while B's line says it is being revived
CHAT = "revive-banner"      # A's chat line to B before the hold (a page that comes while B's held line is up)
REVIVING = "is reviving you"   # coop_dbno_draw.cpp Caption (downed with a reviver)


def words(path):
    """The share of the banner band that is the line's white words."""
    import numpy as np
    from PIL import Image
    with Image.open(path) as im:
        im = im.convert("RGB")
        w, h = im.size
        band = np.asarray(im.crop((int(w * BAND[0]), int(h * BAND[1]), int(w * BAND[2]), int(h * BAND[3]))))
    return float((band.min(axis=2) >= WHITE).mean()) if band.size else 0.0


def banner_sets(pair, x, since):
    """The banner's text sets on game x after `since` (the bridge's `text`, layout SimpleInfo_00, pane T_Text_00):
    (texts, last, hooked)."""
    r = pair.call(x, "text", since=since, layout=LAYOUT)
    texts = [t for t in (r.get("texts") or []) if t.get("pane") == PANE]
    return texts, int(r.get("last") or since), bool(r.get("hooked"))


def banner_deltas(before, after):
    """The module's banner counts (state['banner'], CoopNativeUi::Stats, typing-blink-0121) over the hold; None on a
    build without them (v0.12.0)."""
    if not isinstance(before, dict) or not isinstance(after, dict):
        return None
    return {k: int(after.get(k) or 0) - int(before.get(k) or 0)
            for k in ("opens", "reopens", "type_resets", "shut_by_game", "taken_by_game", "sets")}


def keep(pair, path, name):
    """The frame's shot kept with the result for the owner (<out>/revive-banner/)."""
    if not pair.out_dir or not path or not os.path.exists(path):
        return
    d = os.path.join(pair.out_dir, "revive-banner")
    os.makedirs(d, exist_ok=True)
    shutil.copyfile(path, os.path.join(d, name + ".png"))


@contextlib.contextmanager
def held_back(pair):
    """The hold back at the rows' 5 s (dc._restore puts back the bleed-out only), whatever happened."""
    try:
        yield
    finally:
        try:
            pair.act("A", "chat /dbno hold %d" % DEFAULT_HOLD_TENTHS)
            pair.step_both(30)
        except Exception as e:                 # noqa: BLE001  (the test's own error stays the result)
            pair.notes.append("hold not put back: %s: %s" % (type(e).__name__, e))


@pytest.mark.budget(600)
@pytest.mark.look
@pytest.mark.row("0.12.1 the revive message does not blink for the player being helped up (revive-flicker-0121)")
def test_revive_banner_steady(pair):
    with dc.restored(pair, {}), held_back(pair):
        with pair.setup():
            pair.need_boot_place("child")
            dc.to_place(pair, "child", "KokiriForest_Child", "FrontDekuTree")   # test_dbno_crawl's open ground
            dc.rules(pair, 240)          # (kDbnoMaxBleedS: no bleed-out during the hold)
            pair.act("A", "chat /dbno hold %d" % HOLD_TENTHS)
            pair.expect_log("B", r"coop: dbno: on \(bleed \d+ s, hold %d tenths" % HOLD_TENTHS, max_frames=300)
            dc.no_fairy(pair, "B")
        dc.down(pair)
        pair.expect_log("B", dc.LYING_CHILD, max_frames=600)
        pair.step_both(30)
        b = pair.state("B")["pos"]
        pair.act("A", "place %.2f %.2f %.2f" % (b[0] + dc.BESIDE_M, b[1], b[2]))
        pair.act("A", "face %.2f %.2f" % (b[0], b[2]))
        pair.step_both(10)
        apart = dc.dist(pair.state("A")["pos"], pair.state("B")["pos"])
        assert apart < dc.REACH_M, "A not beside B (%.2f m)" % apart
        # The helper's side before the hold ('Hold A to revive P2  m:ss'), a few frames apart; B's 'You are down'.
        pair.check_shot("A", "revive-banner-beside")
        pair.check_shot("B", "revive-banner-beside")
        pair.step_both(3)
        pair.check_shot("A", "revive-banner-beside-2")
        # A chat line reaches B just before the hold: a page that comes while B's held line is up (it waits; never
        # an open of the banner that shows: typing-blink-0121 KeptUp).
        pair.act("A", "chat %s" % CHAT)
        pair.expect_log("A", r"coop: test chat '%s': sent" % CHAT, max_frames=120)   # coop_testinput.cpp test chat
        pair.step_both(15)
        since = {x: banner_sets(pair, x, 0)[1] for x in ("A", "B")}
        before = {x: pair.state(x).get("banner") for x in ("A", "B")}
        samples, hooked = [], {}
        for i in range(SAMPLES):
            if pair.log["B"].find(dc.event(dc.EV_REVIVED)):
                break
            pair.press("A", ["A"], frames=STEP)
            r = pair.call("B", "shot", name="revive-banner-f%02d-B" % i)
            keep(pair, r.get("path"), "f%02d-B" % i)
            texts, since["B"], hooked["B"] = banner_sets(pair, "B", since["B"])
            samples.append({"i": i, "frame": r.get("frame"), "words": round(words(r["path"]), 4), "sets": len(texts),
                            "texts": [t.get("text") for t in texts][-3:]})
            if i == 4:                  # the look of B's line while revived (B's own shots: the hold goes on)
                pair.check_shot("B", "revive-banner-held")
            if i == 7:
                pair.check_shot("B", "revive-banner-held-2")
        after = {x: pair.state(x).get("banner") for x in ("A", "B")}
        a_texts, since["A"], hooked["A"] = banner_sets(pair, "A", since["A"])
        a_shown = sorted({t.get("text") or "" for t in a_texts})
        deltas = {x: banner_deltas(before[x], after[x]) for x in ("A", "B")}
        report = {"samples": samples, "hooked": hooked, "A_sets": len(a_texts), "A_texts": a_shown[:12],
                  "banner": {"before": before, "after": after, "deltas": deltas}}
        if pair.out_dir:
            os.makedirs(os.path.join(pair.out_dir, "revive-banner"), exist_ok=True)
            with open(os.path.join(pair.out_dir, "revive-banner", "frames.json"), "w", encoding="utf-8") as f:
                json.dump(report, f, indent=1)
        pair.notes.append("B per frame (i frame words sets): %s" % " ".join(
            "%d:%s:%.4f:%d" % (s["i"], s["frame"], s["words"], s["sets"]) for s in samples))
        pair.notes.append("B banner texts: %s" % sorted({t for s in samples for t in s["texts"]})[:12])
        pair.notes.append("A banner sets during the hold: %d, texts %s (hooked %s)" % (len(a_texts), a_shown[:8], hooked))
        pair.notes.append("banner state deltas over the hold: %s" % deltas)
        reviving = [s for s in samples if any(REVIVING in (t or "") for t in s["texts"])]
        # B finishes being revived (the hold is only paused for the shots, never let go)
        ran = 0
        while not pair.log["B"].find(dc.event(dc.EV_REVIVED)):
            assert ran < 1800, "B not revived after A held A %d more frames" % ran
            pair.press("A", ["A"], frames=10)
            ran += 10
        pair.never_log("B", dc.event(dc.EV_BLED_OUT))
        assert len(samples) >= MIN_REVIVING, "only %d of B's frames shot before the revive ended" % len(samples)
        assert len(reviving) >= MIN_REVIVING or not hooked.get("B"), (
            "B's banner said '%s' in only %d of %d shot frames (texts %s)" % (
                REVIVING, len(reviving), len(samples), sorted({t for s in samples for t in s["texts"]})[:8]))
        for x in ("A", "B"):       # (the module's own count: a build with state 'banner', typing-blink-0121)
            d = deltas[x]
            if d is None:
                continue
            assert d["opens"] <= 1 and d["type_resets"] <= 1 and d["shut_by_game"] == 0, (
                "%s's banner replayed during the hold: %s (want opens <= 1, type_resets <= 1, shut_by_game 0)" % (x, d))
        top = max(s["words"] for s in samples)
        assert top >= MIN_WORDS, "no banner line in any of B's %d frames (best %.4f of the band)" % (len(samples), top)
        dark = [s for s in samples if s["words"] < max(MIN_WORDS, SEEN * top)]
        assert not dark, "B's banner blinked: %d of %d frames without its line (i:words:sets %s; best %.4f)" % (
            len(dark), len(samples), " ".join("%d:%.4f:%d" % (s["i"], s["words"], s["sets"]) for s in dark), top)
