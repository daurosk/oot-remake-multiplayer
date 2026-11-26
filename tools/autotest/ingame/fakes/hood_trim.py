"""Fake-game reactions for ingame/test_hood_trim.py (row 278 C4-4; owner port-sweep-c131-2). Only with A's switch
LICHII_COOP_LOOK=cap_trim:yellow[,cap_trim_hoods:on] and no LICHII_COOP_COLOR (fakes/kokiri_band.py and fakes/tunic.py
draw those boots).

- A's look lines (hookshot/src/coop_look_live.cpp:768, the cap.bin line as test_look.py CAP_LINE, lichii/hook/
  coop_look_share.cpp:355) and B's (coop_look_share.cpp:334).
- a camera (view.clip) and on each shot both Links (bandcheck.box): A in its armour's colour (fakes/gear.py's
  PlayerStatus.EquipArmor flag: Kokiri green, Goron red, Zora blue) with the hood band yellow on the Kokiri tunic, and on
  the Goron/Zora hoods only with cap_trim_hoods:on; B green, no band.
Selftest variable BT_FAKE_HOOD_BUG: 'lost' (pass ON: the hoods keep their tunic's colour), 'always' (pass OFF: the
hoods take the trim, the C4-4 fault), 'puppet' (B draws A's puppet with no band at all).
"""
import os

OFF = "LICHII_COOP_LOOK=cap_trim:yellow"
ON = "LICHII_COOP_LOOK=cap_trim:yellow,cap_trim_hoods:on"
# docs/playtest.md "State" view.clip, row-major; 320x180 shots: as fakes/tunic.py (B near x 112 px, A near x 196 px)
VIEW = {"yaw": 0.0, "pitch": 0.0, "y_down": False,
        "clip": [0.4, 0, 0, -1.9, 0, 0.2, 0, -0.8, 0, 0, 0, 0, 0, 0, 0.1, 1]}
EARTH, YELLOW = (120, 100, 80), (0xFF, 0xC0, 0x20)        # coop_colors.cpp:20 yellow FFC020
CLOTH = {"Armor_Adult_Goron": (0xC0, 0x20, 0x20), "Armor_Adult_Zora": (0x20, 0x40, 0xC0)}
KOKIRI = (0x30, 0xA0, 0x40)


def _mode(game):
    """'off' / 'on' for this lane's boot (A's switch), else None."""
    a = game if game.tag == "A" else game.other
    if a is None or a.env_on("LICHII_COOP_COLOR=blue"):
        return None
    return "on" if a.env_on(ON) else "off" if a.env_on(OFF) else None


def setup(game):
    if game.tag != "A":
        return
    mode = "on" if game.env_on(ON) else "off" if game.env_on(OFF) else None
    if not mode or game.env_on("LICHII_COOP_COLOR=blue"):
        return
    parts = "cap_trim:yellow" + (",cap_trim_hoods:on" if mode == "on" else "")
    n = 2 if mode == "on" else 1
    game.say("coop: look: the parts are LICHII_COOP_LOOK's (%s), not the settings': %d part(s) set" % (parts, n))
    game.say("coop: look: the Cap trim paints 43444 blocks of 6 tunic textures (3 of the Goron and Zora tunics)")
    game.say("coop: look: %d part colour(s) sent (PLAYER_LOOK)" % n)


def lane(fl):
    a, b = fl.g["A"], fl.g["B"]
    if _mode(a):
        n = 2 if _mode(a) == "on" else 1
        b.say("coop: look: P1 wears %d part colour(s), character 0 (PLAYER_LOOK)" % n)


def state(game, st):
    if game.other is not None and _mode(game) and st.get("pos") is not None:
        st["view"] = VIEW


def after(game, m, r):
    if m.get("cmd") != "shot" or game.other is None or not _mode(game) or not r.get("path"):
        return
    import bandcheck
    from PIL import Image, ImageDraw
    bug = os.environ.get("BT_FAKE_HOOD_BUG", "")
    a = game if game.tag == "A" else game.other
    armour = a.flags.get("PlayerStatus.EquipArmor", "")
    hood = armour in CLOTH
    band = (not hood) or (_mode(game) == "on" and bug != "lost") or (_mode(game) == "off" and bug == "always")
    if game.tag == "B" and bug == "puppet":
        band = False
    im = Image.open(r["path"]).convert("RGB")
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, im.size[0], im.size[1]], fill=EARTH)
    for who in (game, game.other):
        b = bandcheck.box(VIEW, who.pos, im.size[0], im.size[1])
        if not b:
            continue
        d.rectangle([b[0] + 3, b[1] + 3, b[2] - 4, b[3] - 4], fill=CLOTH.get(armour, KOKIRI) if who is a else KOKIRI)
        if who is a and band:
            d.rectangle([b[0] + 3, b[1] + 3, b[2] - 4, b[1] + 6], fill=YELLOW)
    im.save(r["path"])
