"""Fake-game reaction for the unset-row selftests (hair-link-6, B #148): a scene's animated torch flame.

Gate-look 309d7180c (lane 1) FAILed test_hair_unset_is_game / test_vest_unset_is_tunic on a working unset: the two
Kakariko torch flames flicker orange (hue ~10 = #DD4527's) and yellow (hue ~49 = #FFD000's) between shots, and the old
whole-frame count took them as colour left on Link. This fake draws such a flame so the selftests prove the unset check
judges only the wear's own pixels.
Selftest variable: BT_FAKE_FLAME=RRGGBB: on every shot a small flame of that colour at the left edge (away from the
hair/vest blocks of fakes/hair_link.py and vest_link.py), LIT on every shot except a test's first unset shot
(`*-unset-<game>`): the worst case for an unset check (dark before, lit while coloured AND after the unset).
"""
import os


def after(game, m, r):
    v = os.environ.get("BT_FAKE_FLAME")
    if not v or m.get("cmd") != "shot" or not r.get("path"):
        return
    if str(m.get("name", "")).endswith("-unset-%s" % game.tag):
        return
    from PIL import Image, ImageDraw
    c = int(v, 16)
    im = Image.open(r["path"]).convert("RGB")
    ImageDraw.Draw(im).rectangle([10, 40, 15, 49], fill=((c >> 16) & 255, (c >> 8) & 255, c & 255))
    im.save(r["path"])
