"""Regress vs 0.11.12 (coord offline/regress-01113.ps1, reports/rc-release-0120.md section 4): steps 5 (sword handle part)
and 6 (name tags, pause > System > Title Screen dialog) as bridge tests. Owner: regress-port-a.

| Test | Old step (coord offline/regress-01113-steps/) | Kind |
|---|---|---|
| test_name_tags | s6-ui.ps1: scenarios hs-native-tags + s6-ui marks a-tags / a-tags-2 | log + LOOK (shots on each game) |
| test_title_screen_dialog | s6-ui.ps1: s6-ui marks a-sys-tab, a-title-row, a-title-dialog, a-pause-closed | test state + LOOK |
| test_sword_handle | s5-customize.ps1: "Sword handle changed and seen on the other game" (no scenario there: NOT PROVEN) | log + LOOK |

Not here: step 6's F1 routes (f1-event, C3-1: port-g7 owns the bridge's key input; F1 is a keyboard key, hos_coop_key
vk 0x70, coop_hookshot.cpp:3299), its minimap names (test_map_arrow.py), the tunic and Cap trim parts of step 5
(test_tunic.py, test_hood_trim.py), and step 2 lan251 (needs a server restart and a relaunch: harness, regress-port-a).
"""
import os
import re

import pytest

import game_names
import pair as P
from kit_move import on_screen
from test_rows import need_child_in_play, place_in_front
from test_tunic import prove_apart, stand_apart

# The old step's NOT pattern (regress-01113-steps/s6-ui.ps1 -Not 'exception 0x'), with test_look.py's CRASH forms.
CRASH = r"exception 0x|crash window: .*caught|FATAL|CRASH:"

# -- name tags -------------------------------------------------------------------------------------------------------
# hookshot/src/coop_hookshot.cpp:718 (once, the first frame a balloon of ours shows) and :712 (the game gave no memory:
# drawn tags, not the native ones). The old scenario's own log check (hs-native-tags.A.txt header).
TAGS_ON = r"coop: native tags: name tags on the game's balloons \(\d+ shown, screen 0x[0-9a-f]+\)"
TAGS_NO_MEMORY = r"coop: native tags: the game gave no memory for the names"
TAGS_DIST = 3.0   # m: hs-native-tags.A.txt mark "near" ("B stands 3 m in front"); the old file's mid/far walk is not set-up
# The name tag floats just over the other Link's head: lichii/hook/coop_hook.cpp:196-197 kHeadChild 1.35 / kHeadAdult 1.9
# (m above the feet; HeadHeight by age), where the module's markers put it (tag_head) and the balloon goes.
HEAD_M = {"child": 1.35, "adult": 1.9}
EDGE = 0.9        # |x|, |y| <= 0.9 of the screen (-1..1, kit_move.on_screen): in view, not cut at the edge
CAM_TRIES = 3     # ZL taps per game at most (the game's own camera reset: docs/playtest.md `shoot`, "taps ZL (the camera
                  # goes behind him)"); c272735f8: one tap each left B's camera on A's side (rc-release-0120-5's shots)
CAM_WAIT = 20     # frames after each tap (the camera's swing behind Link; as before)


def seen_once(pair, x, pattern, max_frames):
    """A once-only module line: already in the log since the boot (pos=0), else within max_frames from now."""
    if pair.log[x].find(pattern, pos=0):
        return
    pair.expect_log(x, pattern, max_frames=max_frames)


def seen_not(pair, x, good, bad, max_frames):
    """A once-only line `good` (since the boot, else within max_frames), and never `bad` (its failure form): the wait
    ends at whichever comes first, so a failure names the bad line, not a timeout."""
    def either():
        return pair.log[x].find(bad, pos=0) or pair.log[x].find(good, pos=0)
    pair.until("%s log /%s/ or /%s/" % (x, good, bad), either, max_frames=max_frames)
    line = pair.log[x].find(bad, pos=0)
    assert line is None, "%s log has /%s/: %s" % (x, bad, line)


def other_in_view(pair, x, other, behind=True):
    """Game x's camera behind its own Link, facing the other player, read from x's state (`view` = the last drawn
    frame's camera, kit_move.on_screen): the other player's feet and tag point (HEAD_M over the feet) on x's screen,
    and x's own Link nearer the camera than the other (clip w, the depth: the camera behind x's Link, not on the other's
    side; behind=False: side by side, as stand_apart, only in view). The other player's place is its own game's Link
    (x shows it as the puppet). Returns (ok, why)."""
    st, ot = pair.state(x), pair.state(other)
    view, own, far = st.get("view"), st.get("pos"), ot.get("pos")
    if not view or not own or not far:
        return False, "%s: no view or place (view %s, pos %s, %s's pos %s)" % (x, bool(view), own, other, far)
    head = [far[0], far[1] + HEAD_M.get(ot.get("age"), HEAD_M["child"]), far[2]]
    feet_s, head_s, own_s = on_screen(view, far), on_screen(view, head), on_screen(view, own)
    m = view["clip"]

    def depth(p):
        return sum(m[12 + j] * v for j, v in enumerate(list(p) + [1.0]))
    why = "%s: %s's feet %s, tag point %s on screen; own Link at depth %.2f, %s's at %.2f" % (
        x, other, feet_s, head_s, depth(own), other, depth(far))
    inside = all(s is not None and abs(s[0]) <= EDGE and abs(s[1]) <= EDGE for s in (feet_s, head_s))
    return inside and (not behind or (own_s is not None and depth(own) < depth(far))), why


def camera_behind_each(pair):
    """Each game's camera behind its own Link facing the other, by the game's own ZL reset (CAM_TRIES taps each at
    most), proven per game on state (other_in_view); else SetupFail naming the game and its numbers."""
    for x, other in (("A", "B"), ("B", "A")):
        why = ""
        for n in range(CAM_TRIES):
            pair.press(x, ["ZL"], frames=8)
            pair.step_both(CAM_WAIT)
            ok, why = other_in_view(pair, x, other)
            if ok:
                pair.notes.append("camera behind %s after %d ZL: %s" % (x, n + 1, why))
                break
        else:
            raise P.SetupFail("%s's camera not behind its Link facing %s after %d ZL: %s" % (x, other, CAM_TRIES, why))


@pytest.mark.budget(150)
@pytest.mark.look
@pytest.mark.row("regress 0.11.12 step 6: name tags on the game's balloons (s6-ui a-tags, hs-native-tags near)")
def test_name_tags(pair):
    with pair.setup():
        need_child_in_play(pair)
        place_in_front(pair, "B", of="A", dist=TAGS_DIST)       # B in front of A, facing A: each sees the other
        camera_behind_each(pair)                                 # (each game's own view, proven on its state)
    for x in ("A", "B"):
        seen_not(pair, x, TAGS_ON, TAGS_NO_MEMORY, max_frames=120)
    for x, other in (("A", "B"), ("B", "A")):                   # (still in view at the shot: the tag on EACH game)
        ok, why = other_in_view(pair, x, other)
        assert ok, "%s: the other player's puppet and tag are not in view at the shot: %s" % (x, why)
    # the old marks a-tags and a-tags-2 (200 frames apart): the tag stays over the other Link
    for x in ("A", "B"):
        pair.check_shot(x, "regress-tags-1", tolerance=0.80)    # 0.80: Kakariko's people and birds move
    pair.step_both(200)
    for x, other in (("A", "B"), ("B", "A")):
        ok, why = other_in_view(pair, x, other)
        assert ok, "%s: the other player's puppet and tag left the view by the second shot: %s" % (x, why)
        pair.check_shot(x, "regress-tags-2", tolerance=0.80)
    for x in ("A", "B"):
        pair.never_log(x, CRASH)


# -- Title Screen dialog ---------------------------------------------------------------------------------------------
# The cursor and page proof: LICHII_COOP_TEST_STATE=1 makes the module write <game>/coop-test-state.txt (one line of
# key=value words, at most every 200 ms, from the frame hook: coop_hookshot.cpp:2025-2045 TestState, :2076).
# Its pause words: coop_pause_tab_module.cpp:5395-5424 CoopPauseTab::TestState: menu (the pages seen < 400 ms ago),
# pages_state (2 = open: ToTitleStep's `open`, :4167), pages_page (kSystemPage = 5, coop_pause_tab.h:69), focused
# (FocusedShownName: System's Title Screen is Pa_Button_04, :4176 + docs/game-ui.md:532), to_title (g_to_title, 0 none).
TEST_STATE = "LICHII_COOP_TEST_STATE=1"
STATE_FILE = "coop-test-state.txt"
SYSTEM_PAGE = "5"
TITLE_ROW = "Pa_Button_04"
# The way there is the module's own (ToTitleStep, coop_pause_tab_module.cpp:4152-4200): + (PLUS) opens the pause menu, R
# until the System page, then Down until the cursor is on Pa_Button_04 (at most 12 Downs, :4195), A on it. Not the old
# scenario's single DUP (s6-ui.A.txt: "a wrap would land on Title Screen" was inferred, s6-ui.ps1 -NotProven).
MAX_R, MAX_DOWNS = 6, 12
TAP = 4            # frames a tap is held (ToTitleStep holds 80 ms = ~5 frames; 4 as test_smoke's PLUS)
TAP_GAP = 30       # frames after a tap (ToTitleStep's gaps: R 500 ms, Down 250 ms; the old scenario waited 90)
# The question (docs/game-ui.md:434 + :660-666, docs/addresses.md:393 kSystemWindowVtable "Return to the title
# screen?"): UISystemWindowScreen, in play the layout SystemWindow_00, its text pane T_Text_00 (label %04d, game-ui.md:385).
# Read with the bridge's `text` (docs/playtest.md "Text boxes"). NOT MEASURED: that the question's text comes through
# patch 0041/0042's setters. Until DIALOG_MEASURED cites a lane run's 'dialog text' note, a run that does not see it ends
# NO-REF (the shot regress-title-dialog is then the proof), never PASS on the text.
DIALOG_LAYOUT, DIALOG_PANE = "SystemWindow_00", "T_Text_00"
DIALOG_MEASURED = None   # e.g. "run <name>: A text 'Return to the title screen?' (SystemWindow_00/T_Text_00 wrap 25)"


def read_test_state(pair, x):
    """Game x's coop-test-state.txt as {word: value}, {} when there is none."""
    p = os.path.join(pair.games[x]["folder"], STATE_FILE)
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            line = f.read()
    except OSError:
        return {}
    return dict(m.groups() for m in re.finditer(r"(\w+)=(\S+)", line))


def fresh_state(pair, x, after_t, what, ok, max_frames=240):
    """Steps both games until game x's test state was written after `after_t` (its t= in ms) and ok(state) holds."""
    def pred():
        s = read_test_state(pair, x)
        return s if s and int(s.get("t", "0")) > after_t and ok(s) else None
    return pair.until("%s: %s (%s)" % (x, what, STATE_FILE), pred, max_frames=max_frames)


def tap(pair, x, button):
    t = int(read_test_state(pair, x).get("t", "0"))
    pair.press(x, [button], frames=TAP)
    pair.step_both(TAP_GAP)
    return t


@pytest.mark.budget(240)
@pytest.mark.look
@pytest.mark.needs_env(TEST_STATE)
@pytest.mark.row("regress 0.11.12 step 6: pause > System > Title Screen dialog, B keeps the game (s6-ui a-sys-tab, "
                 "a-title-row, a-title-dialog, a-pause-closed)")
def test_title_screen_dialog(pair):
    games = ("A", "B")
    with pair.setup():
        need_child_in_play(pair)
        pair.step_both(30)
        for x in games:
            s = read_test_state(pair, x)
            assert s.get("menu") == "0", "%s: no %s with menu=0 in play (LICHII_COOP_TEST_STATE on? %s)" % (x, STATE_FILE, s)
        scene = {x: pair.state(x).get("scene") for x in games}
    t = {x: tap(pair, x, "PLUS") for x in games}
    for x in games:
        fresh_state(pair, x, t[x], "the pause menu open", lambda s: s.get("pages_state") == "2", max_frames=300)
    for x in games:
        for _ in range(MAX_R):
            if read_test_state(pair, x).get("pages_page") == SYSTEM_PAGE:
                break
            t0 = tap(pair, x, "R")
            fresh_state(pair, x, t0, "the page after R", lambda s: s.get("pages_state") == "2")
        assert read_test_state(pair, x).get("pages_page") == SYSTEM_PAGE, "%s: not on System after %d R (%s)" % (
            x, MAX_R, read_test_state(pair, x))
    pair.step_both(30)          # (ToTitleStep's 450 ms settle before the cursor is read, kToTitleSettleMs)
    for x in games:
        pair.check_shot(x, "regress-sys-tab", tolerance=0.85)      # still UI: 0.85-0.90 (ingame-tests.md step 6)
    for x in games:
        for _ in range(MAX_DOWNS):
            if read_test_state(pair, x).get("focused") == TITLE_ROW:
                break
            t0 = tap(pair, x, "DOWN")
            fresh_state(pair, x, t0, "the cursor after Down", lambda s: s.get("focused") not in (None, "-"))
        s = read_test_state(pair, x)
        assert s.get("focused") == TITLE_ROW, "%s: the cursor never reached Title Screen (%s) after %d Downs: on %s" % (
            x, TITLE_ROW, MAX_DOWNS, s.get("focused"))
        pair.notes.append("%s: on System's Title Screen: %s" % (x, " ".join("%s=%s" % kv for kv in sorted(s.items())
                                                                            if kv[0] in ("pages_page", "focused"))))
    for x in games:
        pair.check_shot(x, "regress-title-row", tolerance=0.85)
    since = {x: int(pair.call(x, "text", since=0).get("last") or 0) for x in games}
    for x in games:
        tap(pair, x, "A")
    pair.step_both(60)          # (the old scenario's 'wait 180' after A; the question's open animation)
    texts = {}
    for x in games:
        r = pair.call(x, "text", since=since[x], layout=DIALOG_LAYOUT)
        texts[x] = [tx for tx in (r.get("texts") or []) if tx.get("pane") == DIALOG_PANE and tx.get("text")]
        pair.notes.append("%s: dialog text hooked=%s %s" % (x, r.get("hooked"), [tx.get("text") for tx in texts[x]]))
        pair.check_shot(x, "regress-title-dialog", tolerance=0.85)
    # B: the question's No / back (s6-ui.A.txt 'press B'); the game stays: no title, still System, then + closes it
    for x in games:
        t0 = tap(pair, x, "B")
        fresh_state(pair, x, t0, "System again after B", lambda s: s.get("pages_state") == "2" and
                    s.get("pages_page") == SYSTEM_PAGE and s.get("to_title") == "0")
    for x in games:
        tap(pair, x, "PLUS")
    pair.until("both games back in play after +", lambda: all(pair.in_play(pair.state(x)) for x in games), max_frames=300)
    for x in games:
        st = pair.state(x)
        assert st.get("scene") == scene[x], "%s: in %s after the dialog's B, not %s" % (x, st.get("scene"), scene[x])
        pair.never_log(x, r"coop: pause tab: back to the title: A on System's Title Screen")   # (only the module's way)
        pair.never_log(x, CRASH)
    missing = [x for x in games if not texts[x]]
    if missing:
        pair.unproven("no %s/%s text after A on Title Screen in %s (DIALOG_MEASURED: the setter is not proven); the shot "
                      "regress-title-dialog shows it" % (DIALOG_LAYOUT, DIALOG_PANE, ", ".join(missing)))
    if DIALOG_MEASURED is None:
        pair.unproven("dialog text seen (%s) but DIALOG_MEASURED is not set yet: cite this run there" % (
            {x: texts[x][0].get("text") for x in games}))


# -- sword handle ----------------------------------------------------------------------------------------------------
# A's Kokiri Sword hilt in blue, the test's parts (LICHII_COOP_LOOK: coop_look.h EnvParts, key kokiri_hilt = Part 10,
# coop_look.cpp:30 "Kokiri Sword" / "Hilt"; 'blue' a preset, coop_look_menu.cpp:321 kPresets). The game's own hilt is
# 6E5032 (coop_look_menu.cpp:37). Child Link of the lane save holds the Kokiri Sword (equips.weapon, docs/playtest.md:194).
HILT = "A:LICHII_COOP_LOOK=kokiri_hilt:blue"
# A: coop_look_live.cpp:768 (the parts the switch set; Wire() leaves the tunic out) and coop_look_share.cpp:355 (sent).
A_PARTS = r"coop: look: the parts are LICHII_COOP_LOOK's \(kokiri_hilt:blue\), not the settings': 1 part\(s\) set"
A_SENT = r"coop: look: 1 part colour\(s\) sent \(PLAYER_LOOK\)"
# B: coop_look_share.cpp:334 (A is P1: test_masks_on_puppet's 'P1 wears'), coop_look_live.cpp:900 (gear copies on) and
# :320 (P1's copy painted); :314 the failure.
B_GOT = r"coop: look: P1 wears 1 part colour\(s\), character \d+ \(PLAYER_LOOK\)"
B_SLOTS = r"coop: look: other players' gear colours on their puppets on "
B_PAINTED = r"coop: look: P1's gear colours painted for their puppet \(copy \d+\)"
B_NOT_PAINTED = r"coop: look: P1's gear colours not painted"


@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.needs_env(HILT)
@pytest.mark.row("regress 0.11.12 step 5: the sword handle (Kokiri Sword hilt) customized on A, seen on B's puppet of A")
def test_sword_handle(pair):
    with pair.setup():
        need_child_in_play(pair)
        w = (pair.state("A").get("equips") or {}).get("weapon")
        assert w == game_names.KOKIRI_SWORD, "A holds %s, not the Kokiri Sword: the hilt part would not show" % w
        assert pair.log["A"].find(A_PARTS, pos=0), "A: no '%s' since the boot (the switch did not reach the look)" % A_PARTS
        assert pair.log["B"].find(B_SLOTS, pos=0), "B: other players' gear colours are not on (%s)" % B_SLOTS
        stand_apart(pair)        # A beside B, each camera behind its own Link: A's own hilt on A, the puppet on B
        ok, why = other_in_view(pair, "B", "A", behind=False)   # (B's shot is of A's puppet: proven on B's state)
        if not ok:
            raise P.SetupFail("B: A's puppet not in B's view after stand_apart's ZL: %s" % why)
    prove_apart(pair, ("B",), "regress-hilt")   # (B's shots are of A's puppet: in sight, not only in view)
    seen_once(pair, "A", A_SENT, max_frames=300)
    seen_once(pair, "B", B_GOT, max_frames=300)
    seen_not(pair, "B", B_PAINTED, B_NOT_PAINTED, max_frames=300)
    pair.step_both(20)
    for x in ("A", "B"):
        pair.check_shot(x, "regress-hilt-1", tolerance=0.80)    # 0.80: moving people and the puppet's idle
    pair.step_both(6)
    for x in ("A", "B"):
        pair.check_shot(x, "regress-hilt-2", tolerance=0.80)
    for x in ("A", "B"):
        pair.never_log(x, CRASH)
