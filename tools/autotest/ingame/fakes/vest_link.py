"""Fake-game reactions for test_vest_link.py (vest-link's file).

Only after a `menu look link_vest=...` line (the test's own verb): the module's lines for it, and on each shot one
vest block per Link in that Link's vest colour (unset: its tunic's slot colour).
- Own Link: `coop: look: your Link's tunic is <painting> now (...)` (hookshot coop_look_tunic.cpp Step, host.what
  "tunic"), the painting as CoopLookParts::TunicPainting (`tunic-RRGGBB[-vest-RRGGBB]`, upper-case hex).
- The other game: `coop: look: the puppets' tunics are <name> now` (coop_look_live.cpp FollowTunics), the name as
  CoopLookParts::PuppetTunicsName (`ptunics|<colour>=RRGGBB[+vestRRGGBB]`).
- Slot colours: A is P1 0x3070FF (blue), B P2 0xFF3030 (red) (coop/protocol messages.hpp slot_color).
Selftest variable: BT_FAKE_VEST=norepaint (the lines are said but no vest is drawn: the texture not repainted).
"""
import os

SLOT = {"A": (0x3070FF, "blue"), "B": (0xFF3030, "red")}
BOX = {"A": 100, "B": 220}   # (x of each Link's vest block in the 320x180 fake shot)


def setup(game):
    game.vest_link, game.vest_link_seen = None, False


def act(game, w):
    if not (len(w) == 3 and w[0] == "menu" and w[1] == "look" and w[2].lower().startswith("link_vest=")):
        return False
    v = w[2].split("=", 1)[1]
    vest = None if v.lower() == "none" else int(v.lstrip("#"), 16)
    game.vest_link_seen = game.other.vest_link_seen = True
    if vest == game.vest_link:
        return True
    game.vest_link = vest
    tunic, colour = SLOT[game.tag]
    own = "tunic-%06X" % tunic + ("" if vest is None else "-vest-%06X" % vest)
    plus = "" if vest is None else "+vest%06X" % vest
    game.later(2, lambda: game.say("coop: look: your Link's tunic is %s now (1 texture(s) drawn so by Longshot's texture "
                                   "overrides; the game's memory is left as it is)" % own))
    o = game.other
    o.later(10, lambda: o.say("coop: look: the puppets' tunics are ptunics|%s=%06X%s now" % (colour, tunic, plus)))
    return True


def after(game, m, r):
    if m.get("cmd") != "shot" or not r.get("path") or not game.vest_link_seen:
        return
    from PIL import Image, ImageDraw
    norepaint = os.environ.get("BT_FAKE_VEST") == "norepaint"
    im = Image.open(r["path"]).convert("RGB")
    d = ImageDraw.Draw(im)
    for g in (game, game.other):
        c = g.vest_link if g.vest_link is not None and not norepaint else SLOT[g.tag][0]
        x0 = BOX[g.tag]
        d.rectangle([x0, 120, x0 + 30, 160], fill=((c >> 16) & 255, (c >> 8) & 255, c & 255))
    im.save(r["path"])
