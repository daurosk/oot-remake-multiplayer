"""Fake-game reactions for test_hair_link.py (hair-link's file).

Only after a `menu look hair=...` line (the test's own verb): the module's lines for it, and on each shot one hair
block per Link in that Link's hair colour (unset: the game's blond).
- Own Link: `coop: look: your Link's gear is <painting> now (...)` (hookshot coop_look_tunic.cpp Step, host.what
  "gear"), the painting as CoopLookParts::Name (`gear`, with the hair `gear.hair-RRGGBB` here: no other gear part set).
  Unset: no own line (the test does not wait for one).
- The other game: `coop: look: the puppets' boots are <name> now` (coop_look_live.cpp FollowBoots), the name as
  CoopLookParts::BootsPaintingName (`boots[.<L>-hair-RRGGBB]`); the fake's letters: A's puppet G, B's puppet R.
- A warp to a scene ending `_Adult` makes the fake Link adult (the bridge's warp sets Sequence_IsChildhood: docs/playtest.md).
Selftest variable: BT_FAKE_HAIR=norepaint (the lines are said but no hair is drawn: the texture not repainted).
"""
import os

BLOND = 0xE0C060      # (the fake's game hair: any unsaturated-enough yellow far from RED/BLUE)
LETTER = {"A": "G", "B": "R"}
BOX = {"A": 100, "B": 220}   # (x of each Link's hair block in the 320x180 fake shot; above the vest fake's blocks)


def setup(game):
    game.hair_link, game.hair_link_seen = None, False


def act(game, w):
    if not (len(w) == 3 and w[0] == "menu" and w[1] == "look" and w[2].lower().startswith("hair=")):
        return False
    v = w[2].split("=", 1)[1]
    hair = None if v.lower() == "none" else int(v.lstrip("#"), 16)
    game.hair_link_seen = game.other.hair_link_seen = True
    if hair == game.hair_link:
        return True
    game.hair_link = hair
    if hair is not None:
        game.later(2, lambda: game.say("coop: look: your Link's gear is gear.hair-%06X now (1 texture(s) drawn so by "
                                       "Longshot's texture overrides; the game's memory is left as it is)" % hair))
    o = game.other
    piece = "" if hair is None else ".%s-hair-%06X" % (LETTER[game.tag], hair)
    o.later(10, lambda: o.say("coop: look: the puppets' boots are boots%s now" % piece))
    return True


def state(game, st):
    if st.get("age") and str(game.scene or "").endswith("_Adult"):
        st["age"] = "adult"


def after(game, m, r):
    if m.get("cmd") != "shot" or not r.get("path") or not game.hair_link_seen:
        return
    from PIL import Image, ImageDraw
    norepaint = os.environ.get("BT_FAKE_HAIR") == "norepaint"
    im = Image.open(r["path"]).convert("RGB")
    d = ImageDraw.Draw(im)
    for g in (game, game.other):
        c = g.hair_link if g.hair_link is not None and not norepaint else BLOND
        x0 = BOX[g.tag]
        d.rectangle([x0, 40, x0 + 30, 70], fill=((c >> 16) & 255, (c >> 8) & 255, c & 255))
    im.save(r["path"])
