"""Fake-game reactions for test_revive_banner.py (revive-flicker-0121's file). They prove the test's logic, not the
game. Built on fakes/dbno_crawl.py's down-but-not-out model (game.dbno, game.dbno_rules; this file loads before it, so
its 'chat /dbno hold N' is answered here and every other /dbno line there).

- 'chat /dbno hold N' (A): the hold for both games, said as the module says its rules (coop_dbno_game.cpp:255).
- `text` (layout SimpleInfo_00): one set of the banner's T_Text_00 per call while a line is held: B downed ('You are
  down' or 'P1 is reviving you  m:ss  NN%'), A beside the downed B ('Hold A to revive P2' / 'Reviving P2').
- `shot`: the banner's white words drawn in test_revive_banner.BAND while a line is held.

BT_FAKE_BANNER_BUG (selftests, tests/test_ingame_revive_banner.py):
  blink   0.12.0: B's line is gone in every other shot while A holds A (the owner's "blinks rapidly")
  replay  B's banner state counts a Type reset per text change while A holds A (1d8a1b11e not in the build)
  setblank  (test_probe_banner_set's selftest) B's line is set only when it changes (its clock ticks once a second of
          frames) and each set blanks B's shots of the next SETBLANK_FRAMES frames: the probe must count them

state['banner'] (typing-blink-0121's CoopNativeUi::Stats): the module has ONE banner, so this file counts on
fakes/chat_typing.py's stats (game.ct_b, the one writer of state['banner']; chat_typing loads first): a held line
coming up opens it once (opens/type_resets + 1, held, request 1, no page), the line going away puts it back
('shut', not held); with 'replay', type_resets grows with each `text` set while B is being revived. (banner-steady-0121:
its own copy of the stats, kept once a line had been held, overwrote the typed banner's in a whole-suite run.)
`text` is answered only for layout SimpleInfo_00 (the banner's; test_revive_banner asks only that), so every other
layout's text (regress_ui's SystemWindow_00) reaches its own fake.
"""
import math
import os

BAND = (0.30, 0.20, 0.70, 0.27)   # (test_revive_banner.BAND)
SETBLANK_FRAMES = 2               # (BT_FAKE_BANNER_BUG=setblank: B's shots blank after each set)


def bug(name):
    return os.environ.get("BT_FAKE_BANNER_BUG") == name


def rows_here(game):
    return getattr(game, "dbno", None) is not None and game.dbno.get("on")


def line(game):
    """The banner's held line now, or None."""
    if not rows_here(game):
        return None
    d, o = game.dbno, game.other
    bleed = game.dbno_rules["bleed"]
    if bug("setblank"):                       # (the clock ticks: a new line once a second of frames)
        bleed = max(0, bleed - game.frame // 60)
    left = "%d:%02d" % divmod(bleed, 60)
    if d["state"] == "down":
        if d["held"] > 0:
            hold = max(1, game.dbno_rules["hold"] * 3)   # (frames: hold tenths x FPS / 10)
            return "P1 is reviving you  %s  %d%%" % (left, min(99, 100 * d["held"] // hold))
        return "You are down  %s" % left
    if o.dbno["state"] == "down" and game.pos and o.pos and math.dist(game.pos[::2], o.pos[::2]) < 2.5 * 1.3:
        return ("Reviving P2  %s" if o.dbno["held"] > 0 else "Hold A to revive P2  %s") % left
    return None


def act(game, words):
    if words[:3] == ["chat", "/dbno", "hold"] and len(words) == 4 and game.tag == "A":
        for g in (game, game.other):
            g.dbno["on"] = True
            g.dbno_rules["hold"] = int(words[3])
            g.say("coop: dbno: on (bleed %d s, hold %d tenths, 12 quarter hearts; set by 1)" % (
                g.dbno_rules["bleed"], g.dbno_rules["hold"]))
        return True
    return False


def state(game, st):
    b = getattr(game, "ct_b", None)          # (fakes/chat_typing.py setup: the module's one banner)
    if b is None:
        return
    held = line(game) is not None
    if held and not b["held"]:
        b["opens"] += 1
        b["type_resets"] += 1
        b.update(state=2, request=1, holds="ours", page=False, held=True)
    elif not held and b["held"]:
        b.update(state=0, request=0, holds="shut", page=False, held=False)
    if "banner" in st:                        # (chat_typing's copy, before this step's change; not with =nostate):
        st["banner"].update(b)                # changed in place, never replaced (fakegame.py guard 1)


def command(game, m):
    if m.get("cmd") != "text" or m.get("layout") != "SimpleInfo_00":
        return None
    seq = getattr(game, "banner_seq", 0)
    texts = []
    shown = line(game)
    if shown is not None and bug("setblank") and game.tag == "B" and shown == getattr(game, "banner_last", None):
        shown = None                          # (setblank: a set only when the line changes)
    elif shown is not None and bug("setblank") and game.tag == "B":
        game.banner_last = shown
        game.banner_blank_to = game.frame + SETBLANK_FRAMES   # (the next shots, by frame: after() sees frame+1)
    if shown is not None:
        seq += 1
        game.banner_seq = seq
        b = getattr(game, "ct_b", None)
        if b is not None and b["held"]:
            b["sets"] += 1
            if bug("replay") and game.tag == "B" and game.dbno["held"] > 0:
                b["type_resets"] += 1
        texts.append({"seq": seq, "frame": game.frame, "layout": "SimpleInfo_00", "pane": "T_Text_00",
                      "text": shown, "length": len(shown), "cut": False, "markup": False})
    since = int(m.get("since") or 0)
    return {"ok": True, "hooked": True, "last": seq, "lost": 0, "texts": [t for t in texts if t["seq"] > since]}


def after(game, m, r):
    if m.get("cmd") != "shot" or not r.get("path"):
        return
    shown = line(game)
    if shown is None:
        return
    if bug("setblank") and game.tag == "B" and game.frame <= getattr(game, "banner_blank_to", -1):
        return
    if bug("blink") and game.tag == "B" and game.dbno["held"] > 0:
        game.banner_blink = not getattr(game, "banner_blink", False)
        if game.banner_blink:
            return
    from PIL import Image, ImageDraw
    with Image.open(r["path"]) as im:
        im = im.convert("RGB")
    w, h = im.size
    d = ImageDraw.Draw(im)
    x0, y0, x1, y1 = int(w * BAND[0]), int(h * BAND[1]), int(w * BAND[2]), int(h * BAND[3])
    d.rectangle([x0, y0, x1, y1], fill=(30, 30, 30))
    n = min(len(shown), 30)
    for k in range(n):                       # (a white block a letter: the line's words)
        cx = x0 + 4 + k * (x1 - x0 - 8) // 30
        d.rectangle([cx, y0 + 3, cx + 2, y1 - 3], fill=(250, 250, 250))
    im.save(r["path"])
