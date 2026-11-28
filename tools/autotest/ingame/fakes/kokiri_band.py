"""Fake-game reactions for test_look.py::test_kokiri_hood_band (kokiri-band-c124's port; kokiri-band-bridge's file).

Only with the test's switch LICHII_COOP_LOOK=cap_trim:yellow (+ LICHII_COOP_COLOR=blue): the module's look lines, an
adult Kakariko, a camera (view.clip) and on each shot both Links in blue with the hood band yellow.
Selftest variables: BT_FAKE_CAP_BLOCKS (43600 = look_cap_old's pack before the fix, 43228 an older one), BT_FAKE_BAND=blue (the row's bug: the
puppet's band in the tunic's blue), BT_FAKE_KOKIRI_ON_LINE=1 (a camera with A on B's line of sight: boxes overlap), BT_FAKE_KOKIRI_HIDDEN=1 (the
puppet never drawn on B's shot: behind a wall, as run kokiri-look-182cd53b's Kakariko gate; sight.prove must SETUP-FAIL), BT_FAKE_KOKIRI_STUCK=1 (a
place to a spot off the camera's shot refused: sight.prove's away place leaves A in view = SETUP-FAIL 'did not leave').
"""
import os

# A fake camera (docs/playtest.md "State" view.clip, row-major), behind B (at z 0) looking along +z: x moves a Link
# right, z away (smaller, up the picture); 320x180 shots
FAKE_VIEW = {"yaw": 0.0, "pitch": 0.0, "y_down": False,
             "clip": [0.12, 0, 0.02, -0.5, 0, 0.2, 0.01, -0.8, 0, 0, 0, 0, 0, 0, 0.1, 1]}
# BT_FAKE_KOKIRI_ON_LINE: a camera that draws every Link on the same spot (A on B's line of sight)
ON_LINE_VIEW = {"yaw": 0.0, "pitch": 0.0, "y_down": False,
                "clip": [0, 0, 0, -0.02, 0, 0.2, 0, -0.8, 0, 0, 0, 0, 0, 0, 0, 1]}
BLUE, YELLOW = (48, 112, 255), (255, 192, 32)


def _view():
    return ON_LINE_VIEW if os.environ.get("BT_FAKE_KOKIRI_ON_LINE") else FAKE_VIEW


def _on(game):
    return game.env_on("LICHII_COOP_LOOK=cap_trim:yellow") and game.env_on("LICHII_COOP_COLOR=blue")


def setup(game):
    if not _on(game):
        return
    them = 2 if game.tag == "A" else 1
    # the module's lines (hookshot/src/coop_look_live.cpp StartModule, lichii/hook/coop_look_share.cpp PLAYER_LOOK)
    game.say("coop: look: the parts are LICHII_COOP_LOOK's (cap_trim:yellow), not the settings': 1 part(s) set")
    blocks = os.environ.get("BT_FAKE_CAP_BLOCKS", "43444")
    game.say("coop: look: the Cap trim paints %s blocks of 6 tunic textures (3 of the Goron and Zora tunics)" % blocks)
    game.say("coop: look: 2 part colour(s) sent (PLAYER_LOOK)")
    game.say("coop: look: P%d wears 2 part colour(s), character 0 (PLAYER_LOOK)" % them)
    game.say("coop: look: your Link's tunic is tunic-3070FF-cap-FFC020 now (1 texture(s) drawn so)")


def state(game, st):
    if not _on(game) or st.get("pos") is None:
        return
    # (the age comes from FakeGame.age(): its warp sets Sequence_IsChildhood for an _Adult/_Child name, as the real verb)
    st["view"] = _view()
    if (game.other.scene or "").endswith("_Adult"):
        for a in st["world"]["actors"]:
            if a.get("name") == "CoopPuppet_Child_Green":
                a["name"] = "CoopPuppet_Adult_Green"


def act(game, w):
    """BT_FAKE_KOKIRI_STUCK: a place to a spot the fake camera does not show is refused (the game keeps Link)."""
    if not (_on(game) and os.environ.get("BT_FAKE_KOKIRI_STUCK") and w[:1] == ["place"] and len(w) >= 4):
        return False
    import bandcheck
    return bandcheck.box(_view(), [float(v) for v in w[1:4]], 320, 180) is None


def after(game, m, r):
    if m.get("cmd") != "shot" or not _on(game) or not r.get("path"):
        return
    import bandcheck
    from PIL import Image, ImageDraw
    im = Image.open(r["path"]).convert("RGB")
    d = ImageDraw.Draw(im)
    for who, p in (("own", game.pos), ("puppet", game.other.pos)):
        if who == "puppet" and os.environ.get("BT_FAKE_KOKIRI_HIDDEN"):
            continue
        b = bandcheck.box(_view(), p, im.size[0], im.size[1])
        if b:
            d.rectangle([b[0] + 3, b[1] + 3, b[2] - 4, b[3] - 4], fill=BLUE)
            band = BLUE if who == "puppet" and os.environ.get("BT_FAKE_BAND") == "blue" else YELLOW
            d.rectangle([b[0] + 3, b[1] + 3, b[2] - 4, b[1] + 6], fill=band)
    im.save(r["path"])
