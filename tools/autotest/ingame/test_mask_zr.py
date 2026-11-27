"""mask-zr probe (hair-link-6, B #149, issue #91): does the GAME's own put-on draw a mask on your own Link?

| Test | What it proves | Kind |
|---|---|---|
| test_mask_put_on_own_face | which player way puts the Skull Mask on A's own Link, and A's face framebuffer shot after it | probe (always NO-REF) |

Why: the test path ('wear head' flag + 'dress head', test_rows.test_masks_on_puppet / test_hair_link.test_hair_under_mask)
makes the mask actor and shows it in the HUD but draws it on nobody, own Link included (docs/puppet.md:280-282; gate-look
309d7180c hair-link-mask-red-A.png). Whether a PLAYER's put-on draws it was never seen: hs-mask-zr 11-21 was VOID (assign
never put the mask on ZR, reports/mask-draw-0119.md). Run the same test on S 309d7180c and on the v0.11.12 module: a
mask drawn on one and not the other is a regression; drawn on neither: compare with the mod off before calling it ours.

The player ways, tried in this order on A until A's state equips.head names a mask (each cited; none is a guessed
'right' value: this probe is what proves one):
- zr: the mask quest Enum 0xea010310 (GameDataList: NotReady, Ready, UntilRentMaskOfKeaton, ... Complete;
  reports/world-fails-0115.md:25) = Complete, then `assign SkullMask` (coop_testinput.cpp AssignableItem: SkullMask is
  an item-button value) and ZR (the item button: coop_testinput.cpp Assign 'puts ITEM on ZR'; docs/playtest.md input
  buttons).
- dpad-up / dpad-right / dpad-down: masks are head armor in this game (the HeadShortcut flag and shortcut category
  HeadArmor, ootrr-src/data/flags.json; reports/world-fails-0115-2.md:57-59); D-pad Left is the ocarina
  (docs/items.md:320), so the other three directions are tried, one press each.
After each try: note equips.head, the flags HeadShortcut / UI_CurrentShortCutCategory / PlayerStatus.EquipHeadArmor
(flag_read), and a framebuffer shot of A (A's Link faces the camera: test_gear_link.face_each_other, as the hair rows).
The face is judged from the kept shots (mask-zr-*-A.png under new/): this probe ends NO-REF with its findings, never
PASS or FAIL. Shots: before + up to 4 tries + 1 a second later = at most 6 (the suite's limit is 8).
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_gear_link import face_each_other   # noqa: E402  (the hair rows' set-up: A's face in A's shot)
from test_rows import need_child_in_play   # noqa: E402  (the shared set-up helpers)

SKULL = "Armor_Child_Mask_Skull"
MASK_QUEST = "0xea010310"
FLAGS = ("HeadShortcut", "UI_CurrentShortCutCategory", "PlayerStatus.EquipHeadArmor")
SETTLE = 90   # frames after each try before the reads (the put-on animation; ESTIMATED, the probe notes the reads)
TRIES = (
    ("zr", ("setenum %s Complete" % MASK_QUEST, "assign SkullMask"), "ZR"),
    ("dpad-up", (), "UP"),
    ("dpad-right", (), "RIGHT"),
    ("dpad-down", (), "DOWN"),
)


def note(pair, line):
    print(line)
    pair.notes.append(line)


def reads(pair, what):
    head = (pair.state("A").get("equips") or {}).get("head")
    vals = []
    for name in FLAGS:
        try:
            r = pair.flag_read("A", name)
            vals.append("%s=%s" % (name, r.get("value", r)))
        except Exception as e:   # noqa: BLE001  (a flag this game does not have is a finding, not a stop)
            vals.append("%s=? (%s)" % (name, type(e).__name__))
    note(pair, "mask-zr A %s: equips.head=%r %s" % (what, head, " ".join(vals)))
    return head


@pytest.mark.budget(240)   # ESTIMATED: set-up < 30 s, 4 tries x (8 + SETTLE) frames, 6 shots
@pytest.mark.look
@pytest.mark.row("#91 probe: a player's mask put-on on own Link (hair-link-6)")
def test_mask_put_on_own_face(pair):
    with pair.setup():
        need_child_in_play(pair)
        pair.give("A", "PouchData.%s.StockNum" % SKULL)
        face_each_other(pair)
    found = None
    head = reads(pair, "before")
    pair.check_shot("A", "mask-zr-before")
    if head:
        note(pair, "mask-zr: A already wears %r before any try (save state)" % head)
    for name, lines, button in TRIES:
        for line in lines:
            r = pair.act("A", line)
            note(pair, "mask-zr A %s: act %r -> %s" % (name, line, r.get("ok", r)))
        pair.press("A", buttons=[button], frames=8)
        pair.step_both(SETTLE)
        head = reads(pair, name)
        pair.check_shot("A", "mask-zr-%s" % name)
        if head and "Mask" in head:
            found = name
            pair.step_both(30)
            pair.check_shot("A", "mask-zr-%s-2" % name)
            break
    pair.unproven("probe: %s (look at A's face in the mask-zr-*-A shots)" % (
        "the mask went on by %s" % found if found else "no try put a mask on A (equips.head stayed %r)" % head))
