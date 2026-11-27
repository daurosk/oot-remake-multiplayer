"""Fake-game reactions for test_chartab.py (chartab-0121's file): Character Select's Customize screen and its keys.

- press: MINUS opens / shuts the - menu (on Map); L three times on it reaches the Characters page (test_charpage.py's
  path, coop_pause_tab.h GameMinusCategory: Map, Threads of Time, Key Items, Characters); X there opens his Customize
  screen (docs/hookshot.md "Customize"), driven, on Link's page 1 of 5, and says SnailKeysLookUp's line
  (coop_pause_tab_module.cpp) with each of the four panes set once. A opens the mixer (no key then: Driver::KeysNow),
  B leaves the mixer, else the screen (back to his page); Y turns the page.
- state: menu.customize_screen as coop_menu_state.cpp ScreenJson (open, driven, title, page_no, pages, mixer, keys).
- shot: while the screen shows its keys, two key blocks on the footer's guide line (the 320 x 180 fake shot's bottom
  rows, inside test_chartab.FOOTER), none in the mixer.
Selftest variable BT_FAKE_CHARTAB: `words` (the 0.12.0-like fall-back: the texts not set, so the title keeps its
words and the keys are not the game's guide keys), `blank` (the 0.12.0 bug itself: state and line say shown, but
nothing is drawn on the footer).
"""
import os

PAGES = 5   # (Link: Clothes, Shields, Swords, Boots and gauntlets, ... : snail_custom_test "Link: five pages")
LINE = ("coop: characters: Character Select's Customize screen keys: Y part 0x1000, A part 0x2000, texts set "
        "[Pa_CoopKeyY_00/T_Icon_00 1, Pa_CoopKeyY_00/T_Guide_00 1, Pa_CoopKeyA_00/T_Icon_00 1, "
        "Pa_CoopKeyA_00/T_Guide_00 1]: shown as the game's guide keys")
WORDS = ("coop: characters: Character Select's Customize screen keys: Y part 0x1000, A part 0x2000, texts set "
         "[Pa_CoopKeyY_00/T_Icon_00 0]: the title keeps its worded hints")


def setup(game):
    game.ct = {"minus": False, "left": 0, "open": False, "mixer": False, "page": 1}


def press(game, buttons):
    ct = game.ct
    how = os.environ.get("BT_FAKE_CHARTAB", "")
    if ct["open"]:
        if "A" in buttons and not ct["mixer"]:
            ct["mixer"] = True
        elif "B" in buttons:
            if ct["mixer"]:
                ct["mixer"] = False
            else:
                ct["open"] = False
        elif "Y" in buttons and not ct["mixer"]:
            ct["page"] = ct["page"] % PAGES + 1
        return False
    if "MINUS" in buttons:
        ct["minus"], ct["left"] = not ct["minus"], 0
    elif "L" in buttons and ct["minus"]:
        ct["left"] += 1
    elif "R" in buttons and ct["minus"]:
        ct["left"] = max(0, ct["left"] - 1)
    elif "X" in buttons and ct["minus"] and ct["left"] == 3:
        ct.update(open=True, mixer=False, page=1)
        game.later(2, lambda: game.say(WORDS if how == "words" else LINE))
    return False


def _guide(game):
    return os.environ.get("BT_FAKE_CHARTAB", "") != "words"


def state(game, st):
    ct = getattr(game, "ct", None)
    if not ct or not ct["open"]:
        return
    guide = _guide(game)
    title = "Link (page %d of %d)" % (ct["page"], PAGES) + ("" if guide or ct["mixer"] else "  Y: page  A: Fine-tune")
    st.setdefault("menu", {"coop": {"open": False}})["customize_screen"] = {
        "open": True, "driven": True, "title": title, "options": [], "focused": 0, "focused_label": None,
        "mixer": ct["mixer"], "page_no": ct["page"], "pages": PAGES,
        "keys": {"guide": guide, "page": not ct["mixer"], "mix": not ct["mixer"]}}


def after(game, m, r):
    ct = getattr(game, "ct", None)
    if m.get("cmd") != "shot" or not r.get("path") or not ct or not ct["open"] or ct["mixer"]:
        return
    if not _guide(game) or os.environ.get("BT_FAKE_CHARTAB", "") == "blank":
        return
    from PIL import Image, ImageDraw
    im = Image.open(r["path"]).convert("RGB")
    d = ImageDraw.Draw(im)
    for x0 in (200, 262):            # (the A key, then the Y key: 1050 and 1510 of 3840, words left of each)
        d.rectangle([x0, 168, x0 + 40, 178], fill=(240, 240, 240))
    im.save(r["path"])
