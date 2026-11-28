"""Native puppets N1 probe (reports/native-puppets-f-3.md: native-body-6cda648e FAIL, A walked through B's standing
copy): does a placed NPC of this game stop our Link, and does the copy's body, in the same boot and with the same walk?

| Test | Old row | Boot | Kind |
|---|---|---|---|
| test_native_body_probe | (probe for test_native_body, plan N1) | default + --pack native | positions (state) + module log |

Why: the copy's body is cut from Link's own ControllerSet (tools/puppet_pack/build.py puppet_body). The game's files
(romfs Bootup PhiveConfig LayerEntityFilterTable) say a `Player` body and a `Player` body collide (entry 2, as
Player x Character), so the layer is not proven to be the cause. What differs from a placed NPC is the matter body
(Npc_Default: LayerEntity Character, SubLayerEntity NPC, Mass 60, ContactMassScalePushedCharacter /
ContactMassScalePushedObject 1000; Link's Player_Child: LayerEntity Player, Mass 10, ContactMassScale 0.1, no pushed
scale) and the component preset. Before any build.py change this probe shows, in game, which one acts like a wall.

Steps (measures only; it ends NO-REF with the values in its notes, never PASS):
1. NPC: A placed WALK_FROM_M in front of NPC (the gate guard; romfs Banc/KakarikoVillage/
   KakarikoVillage_Child_Static: Npc_Hylia_Soldier_003 has no MoveRadius, so he stands; Rotate y -0.2855 rad = he faces
   FACING), walks at him WALK_FRAMES: A's closest flat distance to the NPC's own place (world list), how far the NPC
   was moved, how far A went.
2. Copy: A placed WALK_FROM_M beside B (as test_native_body step 1) walks at B: A's closest flat distance to B's own
   place and to the copy's body as A's game has it (A's world list: the `player` actor CoopPuppet_*), how far the copy
   was moved in A's game, how far A went; then A's next `body:` summary of P<n> (gave way count, drift).
Reading: the NPC stops A (closest near 0.73 m: Link 0.23 + the guard's cct 0.5, his pack's Npc_Hylia_MS_cct_Standing)
and the copy does not -> the matter body/preset is the cause (next: a copy with the NPC's matter body); neither stops him -> a
character's body never stops Link in this game, and the copy needs another kind of body (rigid body entity).
Set-up proof: A's log has the `P<n> body: ... has a body` line, the NPC is in A's world list near its romfs place,
A lands on the NPC's ground WALK_FROM_M in front of him.
"""
import math
import re

import pytest

from test_native_body import (HAS_BODY, NOT_MOVED, SUMMARY, SUMMARY_FRAMES, SAMPLE, WALK_FROM_M, WALK_FRAMES,
                              flat, others, pos, yaw_to)

NPC = "Npc_Hylia_Soldier_003"           # romfs Banc KakarikoVillage_Child_Static (no MoveRadius: stands)
NPC_ROMFS = (-47.408, 109.689, -11.486)  # its Translate there
NPC_NEAR_M = 2.0                        # ESTIMATED: the world list has him this near his romfs place (else SETUP-FAIL)
FACING = -0.2855                        # its Rotate y (radians): forward (sin, cos) = toward +z, a little -x
GROUND_M = 0.5                          # ESTIMATED: A lands this near the NPC's height (a slope: Rotate x 0.05)


def world(pair, kind, prefix):
    """Actors of `kind` whose name starts with `prefix` in A's whole world list (playtest_client.Client.world, as
    test_enemies.actors; no frame passes)."""
    w = pair.c["A"].world().get("world") or {}
    return [a for a in w.get("actors") or () if a.get("kind") == kind and (a.get("name") or "").startswith(prefix)]


def one(pair, kind, prefix):
    """The one actor (kind, name prefix) in A's world list now, or None (stale list, not there, or more than one)."""
    found = world(pair, kind, prefix)
    return found[0] if len(found) == 1 else None


def walk_into(pair, target, kind, prefix):
    """A walks at `target` for WALK_FRAMES; each SAMPLE frames A's place and the (kind, prefix) actor's place in A's
    world list. Returns (A's places, the actor's places (None where the list did not have it))."""
    a_seen, t_seen = [pos(pair, "A")], []
    t = one(pair, kind, prefix)
    t_seen.append(t and t.get("pos"))
    yaw = yaw_to(a_seen[0], target)
    for _ in range(WALK_FRAMES // SAMPLE):
        pair.call("A", "move", yaw=yaw, strength=1.0, frames=SAMPLE)
        pair.step_other("B", SAMPLE)
        a_seen.append(pos(pair, "A"))
        t = one(pair, kind, prefix)
        t_seen.append(t and t.get("pos"))
    return a_seen, t_seen


def measure(name, a_seen, t_seen, ref):
    """One note: A's closest flat distance to `ref` and to the actor's own places, the actor's move, A's move."""
    live = [(a, t) for a, t in zip(a_seen, t_seen) if a and t]
    to_ref = min(flat(a, ref) for a in a_seen)
    to_body = min((flat(a, t) for a, t in live), default=float("nan"))
    ts = [t for t in t_seen if t]
    body_moved = max((flat(ts[0], t) for t in ts), default=float("nan"))
    a_moved = flat(a_seen[0], a_seen[-1])
    way = (ref[0] - a_seen[0][0], ref[2] - a_seen[0][2])     # A's walk, start -> ref
    passed = (a_seen[-1][0] - ref[0]) * way[0] + (a_seen[-1][2] - ref[2]) * way[1] > 0   # A's end beyond ref
    return ("%s: A moved %.2f m, closest %.2f m to its place, %.2f m to its body (%d/%d samples listed), body moved "
            "%.2f m, A %s" % (name, a_moved, to_ref, to_body, len(live), len(a_seen), body_moved,
                              "went past" if passed else "stopped short")), to_ref


@pytest.mark.budget(360)   # ESTIMATED: 2 walks of 120 frames in 5-frame steps + one body summary (~360 frames)
@pytest.mark.needs_pack("native")
@pytest.mark.row("Native puppets N1 probe: does a placed NPC stop our Link, and does the copy's body (same walk)")
def test_native_body_probe(pair):
    with pair.setup():
        pair.need_boot_place(age="child")
        b_nums = others(pair.state("A"))
        assert len(b_nums) == 1, "A sees players %s, not one other" % b_nums
        n = b_nums[0]
        assert pair.log["A"].find(HAS_BODY % n, pos=0), "A's copy of P%d has no body line (/%s/)" % (n, HAS_BODY % n)
        pair.never_log("A", NOT_MOVED % n)
        front = (NPC_ROMFS[0] + WALK_FROM_M * math.sin(FACING), NPC_ROMFS[1] + 0.3,
                 NPC_ROMFS[2] + WALK_FROM_M * math.cos(FACING))
        pair.act("A", "place %.2f %.2f %.2f" % front)
        pair.act("A", "face %.2f %.2f" % (NPC_ROMFS[0], NPC_ROMFS[2]))
        pair.step_both(30)
        guard = one(pair, "npc", NPC)
        assert guard and guard.get("pos"), "%s is not (once) in A's world list near %s: %s" % (
            NPC, front, [a.get("name") for a in world(pair, "npc", "")][:20])
        g0 = guard["pos"]
        assert math.dist(g0, NPC_ROMFS) < NPC_NEAR_M, "%s at %s, not near its romfs place %s" % (NPC, g0, NPC_ROMFS)
        a0 = pos(pair, "A")
        assert abs(a0[1] - g0[1]) < GROUND_M and abs(flat(a0, g0) - WALK_FROM_M) < 0.5, (
            "A not on %s's ground %.1f m in front of him (A %s, NPC %s)" % (NPC, WALK_FROM_M, a0, g0))

    # 1. A walks at the standing NPC
    a_seen, t_seen = walk_into(pair, g0, "npc", NPC)
    note, npc_closest = measure("npc %s" % NPC, a_seen, t_seen, g0)
    pair.notes.append(note)

    # 2. A walks at B's standing copy (test_native_body step 1's walk)
    b0 = pos(pair, "B")
    pair.act("A", "place %.2f %.2f %.2f" % (b0[0] - WALK_FROM_M, b0[1], b0[2]))
    pair.act("A", "face %.2f %.2f" % (b0[0], b0[2]))
    pair.step_both(30)
    mark = pair.log["A"].mark()
    a_seen, t_seen = walk_into(pair, b0, "player", "CoopPuppet_")
    note, copy_closest = measure("copy P%d" % n, a_seen, t_seen, b0)
    pair.notes.append(note)
    line = pair.until("A's next body summary of P%d" % n,
                      lambda: pair.log["A"].find(SUMMARY % n, pos=mark), max_frames=SUMMARY_FRAMES)
    m = re.search(SUMMARY % n, line)
    pair.notes.append("summary: rest %s, moved %s, calls %s, waited %s, gave way %s, drift %s m" % m.groups())
    pair.never_log("A", NOT_MOVED % n)
    pair.unproven("probe: %s" % "; ".join(pair.notes[-3:]))
