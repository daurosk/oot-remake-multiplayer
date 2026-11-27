"""typing-blink-0121 (owner, v0.12.0, free-running play): "When typing the typing indicator blinks in and out."

The typed line ("you: hello_") is on the game's own banner, SimpleInfo_00 (hookshot/src/coop_hookshot.cpp
NativeTyping, CoopNativeUi::Banner). The game's open (slot 5 f_804d6100) with mode 2 plays the banner's "open"
animation from its start EVEN ON A BANNER THAT SHOWS: its update f_802f1380 calls slot 70 (f_80795d20 -> f_802f62e0:
the "open" animation, state +0x1a6 set back to 1) for every request of 2, whatever the state (main 1.1.1, read with
capstone; reports/typing-blink-0121.md). So any open of ours on a banner that is up is the banner going out and coming
in again, and a banner of ours that the game shuts is opened again by the next step (the same look). And v0.12.0 put
the banner's Type animation back at its first frame on EVERY text set (Banner::Set; revive-flicker-0121 found it for the
held revive line, 1d8a1b11e): each key typed did it on a banner that showed. `type_resets` counts them: one for the
whole typed line.

The module's banner state (docs/playtest.md "banner"; CoopNativeUi::Stats): `opens` (our open calls), `reopens` (opens
asked of a banner that is up: kept up, not opened again), `shut_by_game` (ours, then shut without our close),
`taken_by_game`, `type_resets`, `state`, `request`, `holds`, `page`; and its log lines "coop: native ui: banner: ...".

| Check | How |
|---|---|
| the chat box opens and types on each game | bridge `key` T (the chat key, coop_chatbox.h kOpenKey), `key` text (hos_coop_char's steps) |
| the typed line stays up while typing, stepped | every few frames: banner holds "ours", page, state 1/2; ONE open for the whole line; no shut by the game |
| ... and running in real time (the owner's play) | bridge `resume` for REAL_S seconds with keys typed meanwhile, `pause`; still one open, no shut |
| it shows, steady | bridge shots of each game SHOT_GAP frames apart (check_shot typing-banner-1 / -2) |
| it goes away at once when the box closes | `key` Esc: banner shut (holds "shut", no page) within CLOSE_FRAMES |

Not here: a solo boot. The chat box is co-op's (a line to send needs a session); the Banner rule itself (never an open
on a banner that is up) is the same for every page and the held line, solo or not: tests/hookshot/native_ui_test.cpp.
"""
import re
import time

import pytest

OPEN_KEY = "T"            # hookshot/src/coop_chatbox.h: T opens the chat box unless LICHII_COOP_CHAT_KEY says otherwise
# hookshot/src/coop_hookshot.cpp NativeTyping: logged once a run, at the first typed line on the banner
TYPING_ON = r"coop: native ui: typing on the game's banner"
BANNER_LINE = re.compile(r"coop: native ui: banner: (.+)$")   # CoopNativeUi::TakeBannerLines
UP_STATES = (1, 2)        # +0x1a6: 1 opening (f_802f62e0), 2 open (slot 72 f_802eda10)
SHOW_FRAMES = 90          # the first typed line on the banner: its open animation is a few frames (~3 s of game bound)
KEY_GAP = 4               # frames between typed keys (a fast typist at 30 fps)
STEPPED_KEYS = 30         # keys typed stepped: the whole line, the banner read after each
REAL_S = 15               # real-time window (wall seconds), keys typed meanwhile
REAL_KEY_S = 0.5
SHOT_GAP = 6              # frames between the two shots of each game
CLOSE_FRAMES = 30         # the box closed: the banner shut by then (one close: its own out animation)
SHOT_TOL = 0.85           # the banner over a still boot place: still UI


def banner(pair, x):
    st = pair.state(x)
    b = st.get("banner")
    assert isinstance(b, dict), "%s: no 'banner' in the bridge state: a module without typing-blink-0121" % x
    return b


def games(pair):
    return [x for x in ("A", "B") if x in pair.c]


def up_now(b):
    return b.get("holds") == "ours" and b.get("page") and b.get("state") in UP_STATES


def banner_lines(pair, x, pos):
    return [m.group(1) for m in (BANNER_LINE.search(ln) for ln in pair.log[x].since(pos)) if m]


def open_box(pair, x):
    r = pair.key(x, OPEN_KEY)
    assert r.get("taken") is True, "%s: the chat key was not co-op's (%s)" % (x, r)


def type_text(pair, x, text):
    r = pair.call(x, "key", text=text)
    assert r.get("taken") == len(text), "%s: typed %r, co-op took %s" % (x, text, r.get("taken"))


def run(pair):
    xs = games(pair)
    with pair.setup():
        pair.need_boot_place()
        for x in xs:
            b = banner(pair, x)
            assert b.get("page") is False and b.get("held") is False, "%s: a banner of ours up before typing: %s" % (x, b)
        marks = {x: pair.log[x].mark() for x in xs}
    start = {x: banner(pair, x) for x in xs}
    try:
        # 1. Stepped: open the box, type a line, the banner read between keys.
        for x in xs:
            open_box(pair, x)
            type_text(pair, x, "h")
        pair.until("the typed line on each game's banner",
                   lambda: all(up_now(banner(pair, x)) for x in xs), max_frames=SHOW_FRAMES)
        for x in xs:
            pair.expect_log(x, TYPING_ON, max_frames=SHOW_FRAMES)
        bad = []
        for i in range(STEPPED_KEYS):
            for x in xs:
                type_text(pair, x, "ey there "[i % 9])
            pair.step_both(KEY_GAP)
            for x in xs:
                b = banner(pair, x)
                if not up_now(b):
                    bad.append("%s key %d: %s" % (x, i, b))
        for x in xs:
            pair.check_shot(x, "typing-banner-1", tolerance=SHOT_TOL)
        pair.step_both(SHOT_GAP)
        for x in xs:
            pair.check_shot(x, "typing-banner-2", tolerance=SHOT_TOL)
        stepped = {x: banner(pair, x) for x in xs}
        # 2. Real time, as the owner played: keys typed while the games run free.
        for x in xs:
            pair.call(x, "resume")
        try:
            end = time.monotonic() + REAL_S
            while time.monotonic() < end:
                pair.deadline.check("the real-time typing window")
                for x in xs:
                    type_text(pair, x, "a")
                time.sleep(REAL_KEY_S)
        finally:
            for x in xs:
                pair.call(x, "pause")
        real = {x: banner(pair, x) for x in xs}
        for x in xs:
            if not up_now(real[x]):
                bad.append("%s after the real-time window: %s" % (x, real[x]))
        # 3. Esc: the line goes at once.
        for x in xs:
            r = pair.key(x, "Esc")
            assert r.get("taken") is True, "%s: Esc not co-op's with the box open (%s)" % (x, r)
        pair.until("each game's banner shut after Esc",
                   lambda: all(banner(pair, x).get("holds") == "shut" and not banner(pair, x).get("page") for x in xs),
                   max_frames=CLOSE_FRAMES)
        for x in xs:
            lines = banner_lines(pair, x, marks[x])
            pair.notes.append("%s banner: start %s, stepped %s, real %s; lines: %s" % (
                x, start[x], stepped[x], real[x], " | ".join(lines[:40])))
            for when, b in (("stepped", stepped[x]), ("real time", real[x])):
                opens = b["opens"] - start[x]["opens"]
                shut = b["shut_by_game"] - start[x]["shut_by_game"]
                taken = b["taken_by_game"] - start[x]["taken_by_game"]
                resets = b["type_resets"] - start[x]["type_resets"]
                if opens != 1 or resets != 1 or shut or taken:
                    bad.append("%s %s: %d opens and %d Type resets (1 each wanted: each more one is the banner going "
                               "out and in), %d shut by the game, %d taken by the game while typing" % (
                                   x, when, opens, resets, shut, taken))
        assert not bad, "the typed line blinked: " + "; ".join(bad)
    finally:
        for x in xs:   # (the boot is shared: never leave a box open)
            try:
                if banner(pair, x).get("page"):
                    pair.key(x, "Esc")
            except Exception as e:   # noqa: BLE001
                pair.notes.append("%s: Esc at the end: %s: %s" % (x, type(e).__name__, e))


@pytest.mark.look
@pytest.mark.budget(240)
@pytest.mark.row("typing-blink-0121: the typed chat line stays on the banner while typing, goes at once on Esc")
def test_typing_line_steady(pair):
    run(pair)

