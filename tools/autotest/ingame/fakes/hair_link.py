"""Fake-game reactions for test_hair_link.py (hair-link's file).

Only after a `menu look hair=...` line (the test's own verb): the module's lines for it, and on each shot one hair
block per Link in that Link's hair colour (unset: the game's blond).
- Own Link: `coop: look: your Link's gear is <painting> now (...)` (hookshot coop_look_tunic.cpp Step, host.what
  "gear"), the painting as CoopLookParts::Name (`gear`, with the hair `gear.hair-RRGGBB` here: no other gear part set).
  Unset: no own line (the test does not wait for one).
- The other game: `coop: look: the puppets' boots are <name> now` (coop_look_live.cpp FollowBoots), the name as
  CoopLookParts::BootsPaintingName (`boots[.<L>-hair-RRGGBB]`); the fake's letters: A's puppet G, B's puppet R.
- A warp to a scene ending `_Adult` makes the fake Link adult (the bridge's warp sets Sequence_IsChildhood: docs/playtest.md).
- Once the hair verb was used: a camera (view.clip: fakes/hood_trim.py's VIEW, loaded by path; only when no other fake
  gave one) and on each of this row's shots (named hair-link-*) each Link's hair as a block in the top third of that
  Link's screen box (bandcheck.box), as test_hair_link.py's check finds it.
Selftest variable: BT_FAKE_HAIR=norepaint (the lines are said but no hair is drawn: the texture not repainted);
BT_FAKE_HAIR=nounset (the unset line is said but the last colour stays drawn: the row's bug);
BT_FAKE_HAIR=speckle (the game's hair has specks of #DD4527's hue, every other pixel: hair-link-309d7180's brown hair
and skin under warm light, which the old +-30 degree band took as red);
BT_FAKE_HAIR=blink (a colour is drawn on a test's first shot of a step but not on its '-2' shot 6 frames later: a check
that cannot see a colour stay, so the still-red control must keep the unset row from passing);
BT_FAKE_HAIR=othertunic (B's red tunic over B's whole box, which overlaps A's, on the shots before a colour only:
hair-measure-0defc551, the other Link counted in the measured box; the test leaves the other Link's box out);
BT_FAKE_WINDMILL=RRGGBB (game B only: a 24x24 block of that colour that moves 40 px between shots, away from the
Links: hair-link-309d7180's turning Kakariko windmill, a false 'red left' on B).
"""
import os

BLOND = 0xE0C060      # (the fake's game hair: any unsaturated-enough yellow far from RED/BLUE)
LETTER = {"A": "G", "B": "R"}


_VIEW = []


def _view():
    """fakes/hood_trim.py's VIEW (one camera table for the fakes; that file is loaded by path as fakegame does)."""
    if not _VIEW:
        import importlib.util
        spec = importlib.util.spec_from_file_location("fakes_hood_trim_view",
                                                      os.path.join(os.path.dirname(os.path.abspath(__file__)), "hood_trim.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _VIEW.append(mod.VIEW)
    return _VIEW[0]


def setup(game):
    game.hair_link, game.hair_link_seen, game.hair_link_drawn = None, False, None


def act(game, w):
    if not (len(w) == 3 and w[0] == "menu" and w[1] == "look" and w[2].lower().startswith("hair=")):
        return False
    v = w[2].split("=", 1)[1]
    hair = None if v.lower() == "none" else int(v.lstrip("#"), 16)
    game.hair_link_seen = game.other.hair_link_seen = True
    if hair == game.hair_link:
        return True
    game.hair_link = hair
    if hair is not None or os.environ.get("BT_FAKE_HAIR") != "nounset":
        game.hair_link_drawn = hair
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
    # (never over another fake's camera: B #149, a fake acts for its own row only). After a ZL the game's camera is
    # behind its Link (fakes/regress_ui.py's camera, `rg_cam`, loaded after this file): ours, a fixed one, would win
    # the setdefault and leave A looking at B's side (test_rows.face_each_other's camera_behind_each SETUP-FAIL,
    # face-link-0121 CI 46094393365), so it is given only to a game whose camera no ZL has moved.
    if game.hair_link_seen and st.get("pos") is not None and getattr(game, "rg_cam", None) is None:
        st.setdefault("view", _view())


def after(game, m, r):
    if m.get("cmd") != "shot" or not r.get("path") or not game.hair_link_seen:
        return
    if not str(m.get("name", "")).startswith("hair-link-"):
        return   # (only this row's own shots: B #149, a fake never draws on other tests' shots)
    import bandcheck
    # the camera this game's state reports at this shot (the one the test's view.clip box uses): another row's fake may
    # have given its own camera (st.setdefault above), so never draw with ours unseen (CI 46063218673: whole-suite FAIL)
    VIEW = game.state().get("view") or _view()
    from PIL import Image, ImageDraw
    mode = os.environ.get("BT_FAKE_HAIR", "")
    blinked = mode == "blink" and str(m.get("name", "")).endswith("-2-%s" % game.tag)
    im = Image.open(r["path"]).convert("RGB")
    d = ImageDraw.Draw(im)
    name = str(m.get("name", ""))
    if mode == "othertunic" and "-red" not in name and "-blue" not in name:
        # B's red tunic + cap over B's whole box (which overlaps A's), on the shots before a colour only: the other
        # Link's red counted in the measured box and moving between shots (hair-measure-0defc551's 9 false FAILs)
        bt = game if game.tag == "B" else game.other
        b = bandcheck.box(VIEW, bt.pos, im.size[0], im.size[1])
        if b:
            d.rectangle([b[0], b[1], b[2] - 1, b[3] - 1], fill=_rgb(0xC83A22))
    for g in (game, game.other):
        b = bandcheck.box(VIEW, g.pos, im.size[0], im.size[1])
        if not b:
            continue
        c = g.hair_link_drawn if g.hair_link_drawn is not None and mode != "norepaint" and not blinked else BLOND
        x0, y0, x1, y1 = b[0] + 1, b[1] + 1, b[2] - 2, b[1] + (b[3] - b[1]) // 3
        d.rectangle([x0, y0, x1, y1], fill=_rgb(c))
        if mode == "speckle" and c == BLOND:
            for y in range(y0, y1 + 1):
                for x in range(x0 + (y % 2), x1 + 1, 2):
                    im.putpixel((x, y), _rgb(0xC03018))   # (hue 7: in the red class, but specks, not a patch)
    mill = os.environ.get("BT_FAKE_WINDMILL")
    if mill and game.tag == "B":
        game.hair_link_mill = (getattr(game, "hair_link_mill", 0) + 1) % 4
        x = 180 + 40 * game.hair_link_mill
        d.rectangle([x, 5, x + 23, 28], fill=_rgb(int(mill, 16)))
    im.save(r["path"])


def _rgb(c):
    return ((c >> 16) & 255, (c >> 8) & 255, c & 255)
