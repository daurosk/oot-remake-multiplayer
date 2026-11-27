"""Fake-game reactions for ingame/test_tunic.py (rows #202 + 198; owner port-sweep-c131(-2)). Only with A's colour switch
(and not test_look.py's LICHII_COOP_LOOK=cap_trim:yellow, which fakes/kokiri_band.py draws).

- the boot's colour lines (A): lichii/hook/coop_puppet_config.cpp:55-59 ('your Link wears ... at the start; green
  players get CoopPuppet_<Age>_Green') and hookshot/src/coop_look_tunic.cpp:505 ('your Link's tunic is tunic-3070FF
  now (N ...').
- a camera (view.clip) and on each shot both Links: A in blue 3070FF, B in green, each in its bandcheck.box.
Selftest switches (tests/test_ingame_tunic.py): BT_FAKE_TUNIC_BUG=cross -> B logs the row 198 cross guard
(hookshot/src/coop_look_live.cpp:591) 200 frames in; BT_FAKE_TUNIC_BUG=green -> B draws A's puppet green (the colour
fault a grey compare cannot see); BT_FAKE_TUNIC_ON_LINE=1 -> a camera that draws both Links on one spot (SETUP-FAIL);
BT_FAKE_TUNIC_BUG=leftover -> the module does not take LICHII_COOP_LOOK and A wears the lane file's Cap trim
(gate-tunic-309d7180: 'tunic-3070FF-cap-3070FF').
- with the test's LICHII_COOP_LOOK=cap_trim: (no parts): its line on each game (hookshot/src/coop_look_live.cpp
  StartModule, 'the parts are LICHII_COOP_LOOK's (cap_trim:), not the settings': 0 part(s) set').
"""
import os

# docs/playtest.md "State" view.clip, row-major; 320x180 shots. B starts at x 4 (fakegame.py), A 2.5 m off at 35 degrees:
# B's box near x 112 px, A's near x 196 px, both boxes and their side boxes apart.
FAKE_VIEW = {"yaw": 0.0, "pitch": 0.0, "y_down": False,
             "clip": [0.4, 0, 0, -1.9, 0, 0.2, 0, -0.8, 0, 0, 0, 0, 0, 0, 0.1, 1]}
ON_LINE_VIEW = {"yaw": 0.0, "pitch": 0.0, "y_down": False,
                "clip": [0, 0, 0, -0.02, 0, 0.2, 0, -0.8, 0, 0, 0, 0, 0, 0, 0, 1]}
BLUE, GREEN = (0x30, 0x70, 0xFF), (0x30, 0xA0, 0x40)   # coop_colors.cpp:21; tools/puppet_pack/tunic.py COLORS green
# Kakariko's ground under the Links, not fakegame.py's green field (which is tunic-green: the side box would take a
# green Link off as background; on a lane the side box is what MEASURED must prove for the real scene)
EARTH = (120, 100, 80)


def _on(game):
    a = game if game.tag == "A" else game.other
    return (a is not None and a.env_on("LICHII_COOP_COLOR=blue")
            and not game.env_on("LICHII_COOP_LOOK=cap_trim:yellow"))


def _view():
    return ON_LINE_VIEW if os.environ.get("BT_FAKE_TUNIC_ON_LINE") else FAKE_VIEW


NO_PARTS = "LICHII_COOP_LOOK=cap_trim:"


def setup(game):
    leftover = os.environ.get("BT_FAKE_TUNIC_BUG") == "leftover"
    if game.env_on(NO_PARTS) and not leftover:
        game.say("coop: look: the parts are LICHII_COOP_LOOK's (cap_trim:), not the settings': 0 part(s) set")
    if game.tag != "A" or not game.env_on("LICHII_COOP_COLOR=blue"):
        return
    game.say("coop: your Link wears green at the start; green players get CoopPuppet_<Age>_Green")
    own = "tunic-3070FF-cap-3070FF" if leftover else "tunic-3070FF"
    game.later(30, lambda: game.say("coop: look: your Link's tunic is %s now (4 drawn)" % own))


def lane(fl):
    if os.environ.get("BT_FAKE_TUNIC_BUG") == "cross" and fl.g["A"].env_on("LICHII_COOP_COLOR=blue"):
        b = fl.g["B"]
        b.later(200, lambda: b.say("coop: look: row 198 cross guard: your colours and a puppet's lie on one texture: "
                                   "tunic #0 on puppets' tunics #21 @0x1234"))


def state(game, st):
    if game.other is None or not _on(game) or st.get("pos") is None:
        return
    st["view"] = _view()


def after(game, m, r):
    if m.get("cmd") != "shot" or game.other is None or not _on(game) or not r.get("path"):
        return
    import bandcheck
    from PIL import Image, ImageDraw
    im = Image.open(r["path"]).convert("RGB")
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, im.size[0], im.size[1]], fill=EARTH)
    for who in (game, game.other):
        b = bandcheck.box(_view(), who.pos, im.size[0], im.size[1])
        if not b:
            continue
        colour = BLUE if who.tag == "A" else GREEN
        if who is not game and who.tag == "A" and os.environ.get("BT_FAKE_TUNIC_BUG") == "green":
            colour = GREEN
        d.rectangle([b[0] + 3, b[1] + 3, b[2] - 4, b[3] - 4], fill=colour)
    im.save(r["path"])
