"""Fake-game reactions for ingame/test_regress_ui.py (regress vs 0.11.12 steps 5 + 6; owner regress-port-a).

- name tags: each game logs hookshot/src/coop_hookshot.cpp:718's line once both games exist (the other Link is near).
- Title Screen dialog (only with LICHII_COOP_TEST_STATE=1): <game>/coop-test-state.txt written after every bridge
  message, the words of coop_pause_tab_module.cpp:5395 TestState (menu, pages_state 2 open, pages_page 0..5 with
  System = 5, focused Pa_Button_00..06, to_title) and t= (ms, from the frame); in the open pause menu R turns the page
  (no wrap past System), DOWN moves the cursor (00..06, wraps), A on System's Pa_Button_04 opens the question
  (SystemWindow_00 / T_Text_00 'Return to the title screen?', docs/addresses.md:393) and B shuts it; `text` answers
  from those texts (docs/playtest.md "Text boxes").
- sword handle (only with A:LICHII_COOP_LOOK=kokiri_hilt:blue): A's lines coop_look_live.cpp:768 +
  coop_look_share.cpp:355, B's coop_look_live.cpp:900 + coop_look_share.cpp:334 + coop_look_live.cpp:320.
Selftest switch (tests/test_ingame_regress_ui.py): BT_FAKE_REGRESS_BUG=tags -> no tag line, the drawn-tags warning
instead (:712); =title -> A on Title Screen opens nothing; =hilt -> B's copy is not painted (:314);
=camera -> B's ZL leaves B's camera in front of B (c272735f8: B's shots of its own Link's face, no A).
=turn -> A's 'face' turns A the other way (towards B: gate-hilt-53d40ab3-2's side-on puppet, its back not to B's
camera).
=fall -> A's 'place' lands 30 m under the place asked (gate-hilt-182cd53b: A not where placed).
"""
import math
import os

STATE_FILE = "coop-test-state.txt"
HILT = "LICHII_COOP_LOOK=kokiri_hilt:blue"
QUESTION = "Return to the title screen?"


def _bug(name):
    return os.environ.get("BT_FAKE_REGRESS_BUG") == name


def _ts(game):
    if not hasattr(game, "rg_page"):
        game.rg_page, game.rg_focus, game.rg_dialog, game.rg_texts = 0, 0, False, []
    return game


def setup(game):
    _ts(game)
    if game.tag == "A" and game.env_on(HILT):
        game.say("coop: look: the parts are LICHII_COOP_LOOK's (kokiri_hilt:blue), not the settings': 1 part(s) set")
        game.later(20, lambda: game.say("coop: look: 1 part colour(s) sent (PLAYER_LOOK)"))


def lane(fl):
    a, b = fl.g["A"], fl.g["B"]
    for g in (a, b):
        if _bug("tags"):
            g.say("coop: native tags: the game gave no memory for the names; drawn name tags instead")
        else:
            g.later(10, lambda g=g: g.say("coop: native tags: name tags on the game's balloons (1 shown, screen 0x1f00)"))
    if a.env_on(HILT):
        b.say("coop: look: other players' gear colours on their puppets on (pack copies, drawn by Longshot's texture "
              "overrides)")
        b.later(40, lambda: b.say("coop: look: P1 wears 1 part colour(s), character 0 (PLAYER_LOOK)"))
        if _bug("hilt"):
            b.later(60, lambda: b.say("coop: look: P1's gear colours not painted (copy 0: the pack's coop_look/gear.bin "
                                      "cannot be read; make the puppet pack again): their puppet keeps the game's items"))
        else:
            b.later(60, lambda: b.say("coop: look: P1's gear colours painted for their puppet (copy 0)"))


def act(game, w):
    """=turn: A's 'face' points A away from the point it was given (back towards B); =fall: A's 'place' lands 30 m
    lower."""
    if _bug("fall") and game.tag == "A" and w[:1] == ["place"]:
        game.pos = [float(w[1]), float(w[2]) - 30.0, float(w[3])]
        return True
    if not (_bug("turn") and game.tag == "A" and w[:1] == ["face"]):
        return False
    dx, dz = float(w[1]) - game.pos[0], float(w[2]) - game.pos[2]
    game.facing = (math.degrees(math.atan2(dx, dz)) + 180.0) % 360
    return True


def _on(game):
    return game.env_on("LICHII_COOP_TEST_STATE=1")


def press(game, buttons):
    if "ZL" in buttons:   # (the game's own camera reset: behind Link along his facing; =camera: B's stays in front)
        game.rg_cam = not (game.tag == "B" and _bug("camera"))
    if not _on(game) or not game.menu_pause:
        return False
    g = _ts(game)
    if g.rg_dialog:
        if "B" in buttons:
            g.rg_dialog = False
        return True   # (the question takes every button)
    if "R" in buttons:
        g.rg_page = min(5, g.rg_page + 1)
        return True
    if "DOWN" in buttons:
        g.rg_focus = (g.rg_focus + 1) % 7
        return True
    if "A" in buttons:
        if g.rg_page == 5 and g.rg_focus == 4 and not _bug("title"):
            g.rg_dialog = True
            g.rg_texts.append({"seq": len(g.rg_texts) + 1, "frame": g.frame, "layout": "SystemWindow_00",
                               "pane": "T_Text_00", "text": QUESTION, "length": len(QUESTION), "cut": False,
                               "markup": False, "wrap": 25})
        return True
    if "B" in buttons:
        return True
    if "PLUS" in buttons:
        g.rg_page, g.rg_focus = 0, 0   # (closing: built-in toggles menu_pause)
    return False


def command(game, m):
    if m.get("cmd") != "text":
        return None
    g = _ts(game)
    since, lay = int(m.get("since") or 0), m.get("layout")
    texts = [t for t in g.rg_texts if t["seq"] > since and (not lay or lay in t["layout"])]
    return {"ok": True, "hooked": True, "body": True, "last": len(g.rg_texts), "lost": 0, "texts": texts}


def after(game, m, r):
    if not _on(game):
        return
    g = _ts(game)
    menu = game.menu_pause
    line = ("menu=%d pages_state=%d pages_page=%d shown=0 which=0 view=0 typing=-1 cell=0 set_row=0 set_category=0 "
            "focused=%s to_title=0 kb=0 online=1 pouch=0 tab_in=0x0 tab_out=0x0 mp_page=0 t=%d\n") % (
        1 if menu else 0, 2 if menu else -1, g.rg_page if menu else -1,
        ("Pa_Button_%02d" % g.rg_focus) if menu else "-", game.frame * 17)
    with open(os.path.join(game.folder, STATE_FILE), "w", encoding="utf-8") as f:
        f.write(line)


CAM_BACK, CAM_UP, FOCAL = 3.0, 1.2, 1.2   # m behind (or in front of) Link, m up, 1/tan(half the view)


def state(game, st):
    """The state's `view` (docs/playtest.md: clip = m * [x y z 1] row-major, w the depth) once a ZL was pressed: the
    camera CAM_BACK behind Link looking along his facing, or (=camera, B) in front of him looking back at his face, the
    c272735f8 shots. Only set when no other fake set one (setdefault)."""
    cam = getattr(game, "rg_cam", None)
    if cam is None or not st.get("pos") or st.get("facing") is None:
        return
    look = math.radians(st["facing"] + (0 if cam else 180))
    fx, fz = math.sin(look), math.cos(look)          # forward (yaw 0 = +z, 90 = +x)
    rx, rz = fz, -fx                                  # right
    px, py, pz = st["pos"]
    cx, cy, cz = px - CAM_BACK * fx, py + CAM_UP, pz - CAM_BACK * fz
    clip = [FOCAL * rx, 0.0, FOCAL * rz, -FOCAL * (rx * cx + rz * cz),
            0.0, FOCAL, 0.0, -FOCAL * cy,
            0.0, 0.0, 0.0, 0.0,
            fx, 0.0, fz, -(fx * cx + fz * cz)]
    st.setdefault("view", {"yaw": math.degrees(look) % 360, "pitch": 0.0, "clip": clip, "y_down": False})
