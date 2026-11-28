"""Pilot rc rows in the new form (test-bridge-c127): six rows of different kinds, each a test written once.

| Test | Old row (group file) | Kind |
|---|---|---|
| test_puppets_spawn | hs-puppets (parity-wave1 #202) | log + world state |
| test_masks_on_puppet | hs-masks (parity-wave1 #91) | equip state + log + LOOK (shots on each game) |
| test_hud_blocks | hs-hud (parity-wave1) | LOOK (HUD shots on each game) |
| test_ocarina_notes | hs-ocarina (parity-wave2 #152) | bridge input + sound log |
| test_story_flag_shared | kak-gate / hs-kak-letter flag (#171) | flag share + checkpoint/desync_check |
| test_teleport_to_player | teleport-same (#178) | chat command + positions |

Every log pattern is the old row's own expect= (tools/autotest/groups/*.txt), so old and new judge the same thing;
every key, flag and line is cited where it is used.
"""
import math
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import game_names              # noqa: E402  (the ONE item name table)
import ocarina_notes as ON     # noqa: E402  (the ONE key -> note table)
import playtest_client as pc   # noqa: E402
from kit_move import on_screen  # noqa: E402  (a point on a game's screen from its state `view`)
from pair import READY_FRAMES, SetupFail   # noqa: E402


def need_child_in_play(pair):
    pair.need_boot_place("child", max_frames=READY_FRAMES)   # (in play, stepped; the boot's scene AND child Link)


def place_in_front(pair, x, of, dist=2.5):
    """Puts game x's Link `dist` m in front of game `of`'s Link, facing it ('place X Y Z' / 'face X Z':
    tools/autotest/hookshot.py:357-366; facing 0 = +z, 90 = +x: docs/playtest.md "State")."""
    s = pair.state(of)
    px, py, pz = s["pos"]
    yaw = math.radians(s["facing"])
    tx, tz = px + dist * math.sin(yaw), pz + dist * math.cos(yaw)
    pair.act(x, "place %.2f %.2f %.2f" % (tx, py, tz))
    pair.act(x, "face %.2f %.2f" % (px, pz))
    pair.step_both(10)


# The name tag floats just over the other Link's head: lichii/hook/coop_hook.cpp:196-197 kHeadChild 1.35 / kHeadAdult 1.9
# (m above the feet; HeadHeight by age), where the module's markers put it (tag_head) and the balloon goes.
HEAD_M = {"child": 1.35, "adult": 1.9}
EDGE = 0.9        # |x|, |y| <= 0.9 of the screen (-1..1, kit_move.on_screen): in view, not cut at the edge
CAM_TRIES = 3     # ZL taps per game at most (the game's own camera reset: docs/playtest.md `shoot`, "taps ZL (the camera
                  # goes behind him)"); c272735f8: one tap each left B's camera on A's side (rc-release-0120-5's shots)
CAM_WAIT = 20     # frames after each tap (the camera's swing behind Link; as before)


def other_in_view(pair, x, other, behind=True):
    """Game x's camera behind its own Link, facing the other player, read from x's state (`view` = the last drawn
    frame's camera, kit_move.on_screen): the other player's feet and tag point (HEAD_M over the feet) on x's screen,
    and x's own Link nearer the camera than the other (clip w, the depth: the camera behind x's Link, not on the other's
    side; behind=False: side by side, as stand_apart, only in view). The other player's place is its own game's Link
    (x shows it as the puppet). Returns (ok, why)."""
    st, ot = pair.state(x), pair.state(other)
    view, own, far = st.get("view"), st.get("pos"), ot.get("pos")
    if not view or not own or not far:
        return False, "%s: no view or place (view %s, pos %s, %s's pos %s)" % (x, bool(view), own, other, far)
    head = [far[0], far[1] + HEAD_M.get(ot.get("age"), HEAD_M["child"]), far[2]]
    feet_s, head_s, own_s = on_screen(view, far), on_screen(view, head), on_screen(view, own)
    m = view["clip"]

    def depth(p):
        return sum(m[12 + j] * v for j, v in enumerate(list(p) + [1.0]))
    why = "%s: %s's feet %s, tag point %s on screen; own Link at depth %.2f, %s's at %.2f" % (
        x, other, feet_s, head_s, depth(own), other, depth(far))
    inside = all(s is not None and abs(s[0]) <= EDGE and abs(s[1]) <= EDGE for s in (feet_s, head_s))
    return inside and (not behind or (own_s is not None and depth(own) < depth(far))), why


def camera_behind_each(pair):
    """Each game's camera behind its own Link facing the other, by the game's own ZL reset (CAM_TRIES taps each at
    most), proven per game on state (other_in_view); else SetupFail naming the game and its numbers."""
    for x, other in (("A", "B"), ("B", "A")):
        why = ""
        for n in range(CAM_TRIES):
            pair.press(x, ["ZL"], frames=8)
            pair.step_both(CAM_WAIT)
            ok, why = other_in_view(pair, x, other)
            if ok:
                pair.notes.append("camera behind %s after %d ZL: %s" % (x, n + 1, why))
                break
        else:
            raise SetupFail("%s's camera not behind its Link facing %s after %d ZL: %s" % (x, other, CAM_TRIES, why))


def face_each_other(pair):
    """THE set-up for a check that needs each game to see the other's Link (its puppet): A is put in front of B facing
    it, B faces A, then each camera goes behind its own Link (camera_behind_each: the game's own ZL reset, proven on
    each game's state). place/face move a Link, never its camera: before this, A's camera stayed behind B and B's
    puppet was cut off at the bottom of every A shot (nocs4-182cd53b 01:36, gear/hair/leather/vest no-CS)."""
    place_in_front(pair, "A", of="B")
    pair.act("B", "face %.2f %.2f" % tuple(pair.state("A")["pos"][::2]))
    pair.step_both(10)
    camera_behind_each(pair)


@pytest.mark.budget(120)
@pytest.mark.row("#202 puppet shows (hs-puppets)")
def test_puppets_spawn(pair):
    with pair.setup():
        need_child_in_play(pair)
    # the old row's expects, since the session start (the puppets spawn at the connect)
    assert pair.log["B"].find(r"P1 CoopPuppet_(Child|Adult)\S* spawned after", pos=0), "B: no P1 puppet spawned"
    assert pair.log["A"].find(r"P2 CoopPuppet_(Child|Adult)\S* spawned after", pos=0), "A: no P2 puppet spawned"
    # and the puppet is in each game's world list now (Client.world(): the whole list, the bridge's dump)
    for x, other in (("A", 2), ("B", 1)):
        w = pair.c[x].world().get("world") or {}
        names = [a.get("name") or "" for a in w.get("actors", [])]
        assert any(n.startswith("CoopPuppet_") for n in names), "%s: no CoopPuppet_ actor in the world list (%s)" % (
            x, w.get("why") or "%d actors" % len(names))


@pytest.mark.budget(240)
@pytest.mark.look
@pytest.mark.row("#91 masks on the other player's puppet (hs-masks)")
def test_masks_on_puppet(pair):
    skull = game_names.SKULL_MASK   # hs-masks.A.txt's give / wear lines (one table)
    with pair.setup():
        need_child_in_play(pair)
        pair.give("A", "PouchData.%s.StockNum" % skull)
        face_each_other(pair)
    # 'wear' writes PlayerStatus.EquipHeadArmor at once (coop_testinput.cpp WearFlag head); 'dress head' is the
    # game's own equip call (queued: frames run). equips.head is that flag's text (coop_playtest.cpp state).
    pair.act("A", "wear head %s" % skull)
    pair.act("A", "dress head")
    pair.until("A's own equips.head is the Skull Mask", lambda: pair.state("A")["equips"]["head"] == skull, 120)
    pair.expect_log("B", r"P1 wears head 'Armor_Child_Mask_Skull'", max_frames=300)   # the row's expect
    pair.step_both(20)
    for x in ("A", "B"):                     # each game, twice a few frames apart (the puppet's animation)
        pair.check_shot(x, "mask-skull-1", tolerance=0.80)
    pair.step_both(6)
    for x in ("A", "B"):
        pair.check_shot(x, "mask-skull-2", tolerance=0.80)
    pair.act("A", "wear head none")
    pair.act("A", "dress head")
    pair.expect_log("B", r"P1 takes off head", max_frames=300)                         # the row's expect
    pair.never_log("B", r"create CoopPuppet_Child")    # hs-masks.A.txt pass rule: no new puppet for a mask


@pytest.mark.budget(120)
@pytest.mark.look
@pytest.mark.row("HUD blocks (hs-hud)")
def test_hud_blocks(pair):
    with pair.setup():
        need_child_in_play(pair)
    assert pair.log["A"].find(r"coop: drawing over Hookshot.s picture", pos=0), "A: the HUD never drew (row's expect)"
    pair.step_both(30)
    for x in ("A", "B"):
        pair.check_shot(x, "hud-1", tolerance=0.85)
    pair.step_both(10)
    for x in ("A", "B"):
        pair.check_shot(x, "hud-2", tolerance=0.85)


@pytest.mark.budget(240)
@pytest.mark.needs_env("LICHII_COOP_OCARINA_PROBE=1")   # Pair.ocarina_out waits for the probe's line (pair.OCARINA_PROBE)
@pytest.mark.row("#152 own ocarina notes sent and played (hs-ocarina)")
def test_ocarina_notes(pair):
    with pair.setup():
        need_child_in_play(pair)
        # hs-ocarina.A.txt's own steps, in frames (scenarios/hs-ocarina.A.txt:23-30): 'press DLEFT 12' + 'wait 240'
        # takes the ocarina out (the kit save has it on d-left; DLEFT is the bridge's LEFT, hookshot.py KEYS), then
        # 'press A 12' + 'wait 120' closes the first-use "Ocarina Audio Input" note that eats L R Y X ('mark ready').
        # Not taken over: its walk ahead + turn round (:17-22, 'hold LSTICK_UP 40' / 'hold LSTICK_DOWN 4') only make
        # B's shots show the puppet's front; this row judges the sound log, takes no shot, and a walk is not set-up.
        # The ONE shared way (Pair.ocarina_out): the first-use box is closed with B, not A, which the probe-ocarina-
        # 2f14b333 run proved (the scenario's 'press A 12' was never checked by a log line), and no note is sent by it.
        pair.ocarina_out("A")
    for key, note in ON.KEY_NOTE.items():
        pair.log["A"].mark()
        pair.log["B"].mark()
        pair.press("A", [key], frames=30)
        pair.expect_log("A", r"coop: ocarina note \d+ \(Ocarina_%s\) sent" % note, max_frames=90)
        pair.expect_log("B", r"plays sound 'Ocarina_%s'" % note, max_frames=90)
    pair.press("A", ["B"], frames=8)          # put it away (B leaves the ocarina, as a player does)
    pair.step_both(60)


GATE_OPEN = "Npc_Hylia_Soldier_003_GateOpen"   # lichii/hook/coop_story.cpp:97 + :444 (0x19898d14, world, shared 'up')


@pytest.mark.budget(180)
@pytest.mark.row("#171 a story flag set in one game is set in the other (+ checkpoint/desync_check)")
def test_story_flag_shared(pair):
    logs = os.path.join(pair.out_dir or ".", "checkpoints")
    with pair.setup():
        need_child_in_play(pair)
        for x in ("A", "B"):                 # back to 0 in each game (shared only 0 -> 1: coop_story.cpp Way::up)
            pair.flag(x, GATE_OPEN, 0)
        pair.step_both(30)
        for x in ("A", "B"):
            assert pair.flag_read(x, GATE_OPEN).get("value") in (0, False), "%s: %s did not go back to 0" % (x, GATE_OPEN)
    pair.flag("A", GATE_OPEN, 1)
    pair.until("B has %s = 1" % GATE_OPEN, lambda: pair.flag_read("B", GATE_OPEN).get("value") in (1, True), 300)
    pair.expect_log("B", r"bool 0x19898d14.*0 -> 1", max_frames=60)          # the old row's expect
    pair.step_both(30)
    tag = "flag-%d" % (pair.c["A"].last_frame or 0)
    for x, p in (("A", "p1"), ("B", "p2")):
        pc.checkpoint(pair.c[x], logs, p, tag)
    r = subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(HERE), "desync_check.py"), logs, "--clients",
                        "--flags-only"], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, "desync_check exit %d: %s" % (r.returncode, (r.stdout + r.stderr)[-400:])


@pytest.mark.budget(180)
@pytest.mark.row("#178 /teleport to the other player (teleport-same)")
def test_teleport_to_player(pair):
    with pair.setup():
        need_child_in_play(pair)
        a = pair.state("A")["pos"]
        pair.act("B", "place %.2f %.2f %.2f" % (a[0] + 8.0, a[1], a[2]))   # 8 m away, same scene
        pair.step_both(10)
        b = pair.state("B")["pos"]
        assert math.dist(a[::2], b[::2]) > 5, "B was not placed away from A"
    pair.act("A", "chat /teleport B")              # teleport-same.A.txt
    def next_to_b():
        sa, sb = pair.state("A"), pair.state("B")
        if not (pair.in_play(sa) and sa.get("scene") == sb.get("scene")):
            return None
        return math.dist(sa["pos"][::2], sb["pos"][::2]) < 3.0   # tolerance of this test: 'next to B'
    pair.until("A next to B (< 3 m, same scene)", next_to_b, max_frames=450)
