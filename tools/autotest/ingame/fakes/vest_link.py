"""Fake-game reactions for test_vest_link.py (vest-link's file).

Only after a `menu look link_vest=...` line (the test's own verb): the module's lines for it, a camera (view.clip:
fakes/hood_trim.py's VIEW, as fakes/hair_link.py; only when no other fake gave one), and on each of this row's shots
(named vest-link-*) one vest block per Link in the middle third of that Link's screen box (bandcheck.box: where
test_vest_link.py's huecheck.solid_count looks), in that Link's vest colour (unset: its tunic's slot colour).
- Own Link: `coop: look: your Link's tunic is <painting> now (...)` (hookshot coop_look_tunic.cpp Step, host.what
  "tunic"), the painting as CoopLookParts::TunicPainting (`tunic-RRGGBB[-vest-RRGGBB]`, upper-case hex).
- The other game: `coop: look: the puppets' tunics are <name> now` (coop_look_live.cpp FollowTunics), the name as
  CoopLookParts::PuppetTunicsName (`ptunics|<colour>=RRGGBB[+vestRRGGBB]`).
- Slot colours: A is P1 0x3070FF (blue), B P2 0xFF3030 (red) (coop/protocol messages.hpp slot_color).
Selftest variable: BT_FAKE_VEST=norepaint (the lines are said but no vest is drawn: the texture not repainted);
BT_FAKE_VEST=nounset (the unset line is said but the last colour stays drawn: the row's bug).
"""
import os

SLOT = {"A": (0x3070FF, "blue"), "B": (0xFF3030, "red")}
_VIEW = []


def _view():
    """fakes/hood_trim.py's VIEW (one camera table for the fakes; loaded by path as fakegame does)."""
    if not _VIEW:
        import importlib.util
        spec = importlib.util.spec_from_file_location("fakes_hood_trim_view_vest",
                                                      os.path.join(os.path.dirname(os.path.abspath(__file__)), "hood_trim.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _VIEW.append(mod.VIEW)
    return _VIEW[0]


def setup(game):
    game.vest_link, game.vest_link_seen, game.vest_link_drawn = None, False, None


def act(game, w):
    if not (len(w) == 3 and w[0] == "menu" and w[1] == "look" and w[2].lower().startswith("link_vest=")):
        return False
    v = w[2].split("=", 1)[1]
    vest = None if v.lower() == "none" else int(v.lstrip("#"), 16)
    game.vest_link_seen = game.other.vest_link_seen = True
    if vest == game.vest_link:
        return True
    game.vest_link = vest
    if vest is not None or os.environ.get("BT_FAKE_VEST") != "nounset":
        game.vest_link_drawn = vest
    tunic, colour = SLOT[game.tag]
    own = "tunic-%06X" % tunic + ("" if vest is None else "-vest-%06X" % vest)
    plus = "" if vest is None else "+vest%06X" % vest
    game.later(2, lambda: game.say("coop: look: your Link's tunic is %s now (1 texture(s) drawn so by Longshot's texture "
                                   "overrides; the game's memory is left as it is)" % own))
    o = game.other
    o.later(10, lambda: o.say("coop: look: the puppets' tunics are ptunics|%s=%06X%s now" % (colour, tunic, plus)))
    return True


def state(game, st):
    if game.vest_link_seen and st.get("pos") is not None:
        st.setdefault("view", _view())   # (never over another fake's camera: B #149, a fake acts for its own row only)


def after(game, m, r):
    if m.get("cmd") != "shot" or not r.get("path") or not game.vest_link_seen:
        return
    if not str(m.get("name", "")).startswith("vest-link-"):
        return   # (only this row's own shots: B #149, a fake never draws on other tests' shots)
    import bandcheck
    VIEW = game.state().get("view") or _view()   # (the camera this game's state reports: the test's box uses it)
    from PIL import Image, ImageDraw
    norepaint = os.environ.get("BT_FAKE_VEST") == "norepaint"
    im = Image.open(r["path"]).convert("RGB")
    d = ImageDraw.Draw(im)
    for g in (game, game.other):
        b = bandcheck.box(VIEW, g.pos, im.size[0], im.size[1])
        if not b:
            continue
        c = g.vest_link_drawn if g.vest_link_drawn is not None and not norepaint else SLOT[g.tag][0]
        h = (b[3] - b[1]) // 3
        d.rectangle([b[0] + 1, b[1] + h + 1, b[2] - 2, b[1] + 2 * h], fill=((c >> 16) & 255, (c >> 8) & 255, c & 255))
    im.save(r["path"])
