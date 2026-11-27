"""chartab-0121 (owner, v0.12.0: "In the characters tab in link customization there's no more indicator to pick the
exact hue or change page"): Character Select's Customize screen, opened from his Characters page, shows its Y (next
page) and A (fine-tune: the exact hue) keys as the game's guide keys again, on each game.

Root cause (reports/chartab-0121.md): since menu-customize-01113 (79ecaf26a, 0.12.0) the kit puts two copies of the
game's PaGuideKey_00 in his CharacterCustomize_00 (tools/puppet_pack/merge_tables.py add_snail_keys) and the title
drops its worded hints once both parts are found; the game does not give a copied part's panes their "<part>-<pane>"
labels, so both keys drew nothing. The module now sets the keys' texts (coop_pause_tab_module.cpp SnailKeysLookUp)
and counts them as shown only when all four panes took their text (CoopSnailCustom::KeysShownFrom).

| Test | What it proves | Kind |
|---|---|---|
| test_customize_keys_show | on A and on B: his screen opens driven, its keys are the game's guide keys (state keys.guide, page, mix; the module's 'shown as the game's guide keys' line with 4 texts set), the footer differs from the mixer's (where neither key shows), Y turns the page | state + log + LOOK (shots on each game) |

Set-up and judging, each cited where used:
- The way there: MINUS (the - menu opens on Map), L (Threads of Time), L (Key Items), L (the Characters page):
  test_charpage.py to_key_items_then_left (charpage-288-hold.A.txt's path, coop_pause_tab.h GameMinusCategory), the
  same 150-frame waits. X on the Characters page opens his Customize screen (docs/hookshot.md "Customize").
- State: `menu.customize_screen` (docs/playtest.md; hookshot/src/coop_menu_state.cpp ScreenJson): open, driven,
  title, page_no, pages, mixer and `keys` {guide, page, mix} (chartab-0121, Driver::KeysShown / KeysNow).
- Log: SnailKeysLookUp's 'coop: characters: Character Select's Customize screen keys: ... texts set [...]: shown as
  the game's guide keys' line, each of the four panes with its count (the game's pane-text call's own; 0 or -1: not
  set, and then the title keeps its words: FAIL here, the keys must show).
- Keys: Y turns the page (Driver::Pad kY, more than one page: Link has five, snail_custom_test), A opens the mixer
  (Driver::Pad kA; KeysNow: no key in the mixer), B leaves it (Driver::Pad kB).
- Footer: the keys sit on the footer's guide line (merge_tables.py SNAIL_KEY_Y -1018 of the 3840 x 2160 canvas, y up
  from the middle: 97% down the picture; icons at x 1050 and 1510, words left of each: 64% to 92% across). The footer
  box below is wider than that on every side; with the mixer open neither key shows (KeysNow), so the box must change
  between the two shots by more than noise (the game's own Back key stays in both).
- A shot of each game's screen with the keys, compared to refs/chartab-keys-<x>.png (NO-REF until the owner approves
  one: never PASS by itself), and the mixer's shot kept beside it.
"""
import pytest

WAIT = 150                     # (test_charpage.py WAIT: scenarios/charpage-288-hold.A.txt 'wait 150' after every key)
KEYS_LINE = (r"coop: characters: Character Select's Customize screen keys: Y part 0x[0-9a-f]+, A part 0x[0-9a-f]+, "
             r"texts set \[Pa_CoopKeyY_00/T_Icon_00 [1-9]\d*, Pa_CoopKeyY_00/T_Guide_00 [1-9]\d*, "
             r"Pa_CoopKeyA_00/T_Icon_00 [1-9]\d*, Pa_CoopKeyA_00/T_Guide_00 [1-9]\d*\]: shown as the game's guide keys")
WORDS_LINE = r"coop: characters: Character Select's Customize screen keys: .*: the title keeps its worded hints"
FATAL = r"FATAL: the game aborted"
FOOTER = (0.55, 0.92, 0.97, 1.0)   # (x0, y0, x1, y1) fractions of the picture: the guide line and room around it
FOOTER_MOVED = 0.02                # mean absolute change (0..1) the box must show with the keys gone


def screen(pair, x):
    return ((pair.state(x).get("menu") or {}).get("customize_screen") or {})


def footer_change(a_path, b_path):
    """Mean absolute RGB change (0..1) inside FOOTER between two shots (imagecheck.load_rgb, 320 wide)."""
    import imagecheck
    a, b = imagecheck.load_rgb(a_path), imagecheck.load_rgb(b_path)
    h, w = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
    x0, y0, x1, y1 = (int(FOOTER[0] * w), int(FOOTER[1] * h), int(FOOTER[2] * w), int(FOOTER[3] * h))
    return float(abs(a[y0:y1, x0:x1] - b[y0:y1, x0:x1]).mean())


def open_customize(pair, x):
    """The - menu's Characters page, then X: his Customize screen, driven by co-op."""
    pair.press(x, ["MINUS"])
    pair.step_both(WAIT)
    for _ in range(3):
        pair.press(x, ["L"])
        pair.step_both(WAIT)
    pair.press(x, ["X"])
    pair.step_both(WAIT)
    return pair.until("%s: Character Select's Customize screen open and driven" % x,
                      lambda: screen(pair, x).get("driven") and screen(pair, x), max_frames=300)


def leave_menu(pair, x):
    pair.press(x, ["B"])            # (B: his screen back to his page, Driver::Pad: B is his outside the mixer)
    pair.step_both(WAIT)
    pair.until("%s: his Customize screen closed" % x, lambda: not screen(pair, x).get("open"), max_frames=300)
    pair.press(x, ["MINUS"])
    pair.step_both(180)
    pair.until("%s back in play after the - menu" % x, lambda: pair.in_play(pair.state(x)), max_frames=600)


@pytest.mark.budget(420)
@pytest.mark.look
@pytest.mark.row("chartab-0121 Characters tab: the Customize screen's page and exact-hue keys show (owner, v0.12.0)")
def test_customize_keys_show(pair):
    with pair.setup():
        # (a fresh in-play sample first: preflight-first-state-read, gifts-237 SETUP-FAIL 2026-11-26)
        pair.until_in_play("both games in play")
        for x in ("A", "B"):
            assert not screen(pair, x).get("open"), "%s: his Customize screen reads open before the test" % x
    for x in ("A", "B"):
        s = open_customize(pair, x)
        assert pair.log[x].find(KEYS_LINE, pos=0), (
            "%s: no 'shown as the game's guide keys' line with all four texts set (%s)" % (
                x, "the title kept its words" if pair.log[x].find(WORDS_LINE, pos=0) else "no keys line"))
        assert s.get("keys") == {"guide": True, "page": True, "mix": True}, "%s: keys %r (Link: five pages)" % (
            x, s.get("keys"))
        assert "Y:" not in s.get("title", "") and "A:" not in s.get("title", ""), "%s: title %r" % (x, s.get("title"))
        assert s.get("page_no") == 1 and (s.get("pages") or 0) > 1, "%s: page %r of %r" % (
            x, s.get("page_no"), s.get("pages"))
        keys_shot = pair.check_shot(x, "chartab-keys", tolerance=0.85)   # (his screen + Link's turning preview: 0.85)
        # A: the mixer, where neither key shows (KeysNow): the footer must change by the keys alone
        pair.press(x, ["A"])
        pair.step_both(WAIT)
        m = screen(pair, x)
        assert m.get("mixer") and m.get("keys") == {"guide": True, "page": False, "mix": False}, (
            "%s: after A: mixer %r keys %r" % (x, m.get("mixer"), m.get("keys")))
        mixer_shot = pair.check_shot(x, "chartab-mixer", tolerance=0.85)
        moved = footer_change(keys_shot["path"], mixer_shot["path"])
        assert moved >= FOOTER_MOVED, "%s: the footer changed %.4f (< %.2f) when the keys went: nothing drawn" % (
            x, moved, FOOTER_MOVED)
        pair.press(x, ["B"])        # (out of the mixer: both keys again)
        pair.step_both(WAIT)
        assert screen(pair, x).get("keys") == {"guide": True, "page": True, "mix": True}, "%s: after B: %r" % (
            x, screen(pair, x).get("keys"))
        pair.press(x, ["Y"])        # (the page key does what it shows)
        pair.step_both(WAIT)
        assert screen(pair, x).get("page_no") == 2, "%s: Y left the page at %r" % (x, screen(pair, x).get("page_no"))
        pair.check_shot(x, "chartab-keys-page2", tolerance=0.85)
        leave_menu(pair, x)
        pair.never_log(x, FATAL)
