"""Developer tool (not run by the kit): the Map page's sequence edit for co-op's own - menu screen, CoopMenu_00.

The owner (2026-11-22): Multiplayer, Lobby and Mods must look and move like the game's own - pages and Snail's
Characters page. Snail's OoT Character Select (github.com/snaildev99/OoT-Character-Select v0.2.1, MIT, Copyright (c)
2026 snaildev99, used with his permission) opens his page as a screen of its own from the - menu's sequence; this runs
HIS sequence helpers (mods/build.py: Graph, vanilla, t_*) as a library to give the Map page's sequence
(Sequence/Map.module.ainb, which no other mod edits) the same kind of page to its right:

  Map, R or ZR (the Map page's checker has them off): the strip's tab sound (mc_TabToRight), the Map slides out to the
      left (its InFromChallenge backwards, from its last frame), its buttons' hit test off, CoopMenu_00 loaded; next
      frame CoopMenu_00 opens with its InFromChallenge (as Snail's page opens; its other In animations held at 0).
  CoopMenu_00's state (the Map page's own pause capture kept): - closes the menu, B too when the menu may close (the
      Map page's own close, CoopMenu_00 closed and unloaded with it); + closes it the Map page's way to the + menu;
      L (only ever seen on co-op's first tab: the co-op module keeps L from the game on the others, and R / ZR on all
      three) plays CoopMenu_00's InChallenge, mc_TabToLeft and the Map's InChallenge backwards (in from the right),
      then, two frames on (so the Map's own checker never reads that same L: it would go on to Threads of Time),
      the Map page's main loop, which closes and unloads CoopMenu_00 once its InChallenge has played (Snail's
      overlapping exit: no frame without a page). ZL / ZR there are the co-op module's (a list's pages).

Needs a Python with his requirements.txt (ainb, mmh3) and the game's romfs:

  python make_data.py --snail <his checkout at v0.2.1> --romfs <romfs> --data <repo>/tools/puppet_pack/coopmenu/data

Writes <data>/map.cdelta (charsel_ui's CDLT1 format: copy-from-the-player's-file and insert operations, none of the
game's bytes) and prints both SHA-1s; coop_menu.py checks them at pack time.
"""
import argparse
import copy
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCREEN = "CoopMenu_00"
MAP = "MapMenu_00"
REL = "Sequence/Map.module.ainb"


def edit(B, path):
    """Map.module.ainb's bytes with co-op's page (see the module's notes). B: Snail's mods/build.py."""
    g = B.Graph(path)
    T = B.TRIG
    chk = g.node(4, "SeqQueryControllerTrigMaskChecker")
    outs = chk["Parameters"]["Outputs"]["Bool"]
    for k in ("R", "ZR"):
        assert outs[T[k]]["Name"] == "Trig" + k and not outs[T[k]]["Is Output"], k
        outs[T[k]]["Is Output"] = True
    zl = g.node(24, "", "Element_BoolSelector")
    assert zl["Parameters"]["Inputs"]["Bool"][0]["Output Index"] == T["ZL"]
    assert zl["Plugs"]["Child"][1] == {"Node Index": 23, "Name": "False"}
    g.node(23, "SeqExecuteGenericDummyAction")
    main = g.node(3, "", "Element_Simultaneous")
    assert [c["Node Index"] for c in main["Plugs"]["Child"]] == [63, 72, 7]
    for i, name in ((0, "SeqOneShotSetEnableScreenButtonHitTest"), (15, "SeqOneShotSetEnableScreenButtonHitTest"),
                    (32, "SeqQueryIsClosableMinusMenuByBbutton"), (63, ""), (64, "SeqOneShotStartSound"),
                    (68, "SeqOneShotRequestStopAnimationScreen"), (90, "SeqQueryGetScreenAnimMaxFrame")):
        g.node(i, name)
    for i in (9, 35):
        assert "Is Root Node" in g.node(i)["Flags"], i
    hit_on, hit_off = g.node(0), g.node(15)
    assert [p["Default Value"] for p in hit_on["Parameters"]["Inputs"]["Bool"]] == [True]
    assert [p["Default Value"] for p in hit_off["Parameters"]["Inputs"]["Bool"]] == [False]

    def sound(key):
        return g.add(B.vanilla("Map", 64, "SeqOneShotStartSound", SLinkKey=key))

    def from_last(anim):
        """The Map's `anim` stopped at its last frame (as node 68 does for InFromChallenge), then played backwards."""
        q = copy.deepcopy(g.node(90))
        [p for p in q["Parameters"]["Inputs"]["String"] if p["Name"] == "AnimName"][0].update(
            {"Default Value": anim, "Flags": []})
        qi = g.add(q)
        s = copy.deepcopy(g.node(68))
        [p for p in s["Parameters"]["Inputs"]["String"] if p["Name"] == "AnimationName"][0].update(
            {"Default Value": anim, "Flags": []})
        s["Queries"] = [qi]
        s["Parameters"]["Inputs"]["Float"][0]["Node Index"] = qi
        s["Plugs"]["Generic"][0]["Node Index"] = qi
        return [g.add(s), g.add(B.t_play_anim(MAP, anim, reverse=True))]

    S = SCREEN
    # CoopMenu_00's state; its pad chain is filled in below (its ends need the states it goes to)
    page = g.add(B.t_simultaneous([], root=True))
    # in: the Map out to the left, our screen loaded; next frame, our screen open with its In
    opened = g.add(B.t_simultaneous(
        [g.add(B.t_open_screen(S)), g.add(B.t_stop_anim(S, "InChallenge")), g.add(B.t_stop_anim(S, "DetailIn")),
         g.add(B.t_stop_anim(S, "GuideIn")), g.add(B.t_stop_anim(S, "DemoMessageIn")),
         g.add(B.t_play_anim(S, "InFromChallenge"))], then=page))
    going = g.add(B.t_simultaneous([sound("mc_TabToRight"), g.add(copy.deepcopy(hit_off))] +
                                   from_last("InFromChallenge") + [g.add(B.t_load_screen(S))], then=opened))
    enter = g.add(B.t_end(going))
    zr = g.add(B.t_bool_sel(4, T["ZR"], enter, 23))
    zl["Plugs"]["Child"][1]["Node Index"] = g.add(B.t_bool_sel(4, T["R"], enter, zr))

    # out: B / - (the Map page's close, 9), + (to the + menu, 35), L (back to the Map's main loop, 3)
    close = g.add(B.t_end(g.add(B.t_simultaneous([g.add(B.t_close_unload_screen(S))], then=9))))
    plus = g.add(B.t_end(g.add(B.t_simultaneous([g.add(B.t_close_unload_screen(S))], then=35))))
    # (through a gate of two frames, Snail's picker's: the Map loop's own checker would read the same L in this frame
    # and step on to Threads of Time, LANE1 rerun 2026-11-22 s12)
    gate = g.add(B.t_sequential([g.add(B.t_wait_frames(1)), g.add(B.t_wait_frames(1))], then=3, root=True,
                                same_frame=False))
    back = g.add(B.t_end(g.add(B.t_simultaneous(
        [g.add(B.t_play_anim(S, "InChallenge")), sound("mc_TabToLeft"), g.add(copy.deepcopy(hit_on))] +
        from_last("InChallenge"), then=gate))))
    left = g.add(B.t_bool_sel(4, T["L"], back, 23))
    plus_sel = g.add(B.t_bool_sel(4, T["Plus"], plus, left))
    closable = g.add(B.t_bool_sel(32, 0, close, plus_sel))
    b_sel = g.add(B.t_bool_sel(4, T["B"], closable, plus_sel))
    minus_sel = g.add(B.t_bool_sel(4, T["Minus"], close, b_sel))
    g.nodes[page]["Plugs"] = B._plugs([63, minus_sel])

    # the Map page's main loop: our screen, still open after L (its InChallenge playing), closed once it has played
    shut = g.add(B.t_sequential([g.add(B.t_wait_anim(S, "InChallenge")), g.add(B.t_close_unload_screen(S))],
                                root=False))
    main["Plugs"]["Child"].append({"Node Index": g.add(B.t_screen_state_sel(S, shut, 23)), "Name": ""})
    return g.build()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--snail", required=True, help="Snail's OoT Character Select checkout (v0.2.1)")
    ap.add_argument("--romfs", required=True, help="the game's romfs (read only)")
    ap.add_argument("--data", default=os.path.join(HERE, "data"))
    a = ap.parse_args(argv)
    os.environ["OOT_ROMFS"] = a.romfs.replace(os.sep, "/")
    sys.path[:0] = [os.path.join(a.snail, "mods"), os.path.join(a.snail, "tools")]
    import build as B   # noqa: E402 (his mods/build.py)
    sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
    from puppet_pack import charsel_ui   # noqa: E402
    path = os.path.join(a.romfs, *REL.split("/"))
    with open(path, "rb") as f:
        src = f.read()
    out = edit(B, path)
    delta = charsel_ui.make_delta(src, out)
    assert charsel_ui.apply_delta(src, delta) == out
    os.makedirs(a.data, exist_ok=True)
    with open(os.path.join(a.data, "map.cdelta"), "wb") as f:
        f.write(delta)
    print("map.cdelta: %d bytes; %s %s -> %s %s" % (len(delta), len(src), hashlib.sha1(src).hexdigest(), len(out),
                                                    hashlib.sha1(out).hexdigest()))


if __name__ == "__main__":
    main()
