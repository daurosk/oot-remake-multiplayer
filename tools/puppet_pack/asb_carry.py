"""The carried pose for the young puppets: Ruto's own carry on Link's skeleton (docs/native-puppets.md,
"The carried Link's pose"; `--puppet-liftable`, off by default).

Owner rule (2026-11-19): a lifted young Link is carried as Link carries Ruto, with Ruto's own animation. While
lifted, the lifted player's game sends the AS command `Eaten` as its Link's animation (coop_lift_player.h,
CarriedAnimation), so every copy of it plays that command. A child copy built with `--puppet-liftable` gets:

- an AS root of its own (CARRY_AS), a copy of the player's `AS/Player_Child.root.asb` in which the two clips of
  its `Eaten` command (EATEN_CLIPS, Link's hold by a Like Like) name Ruto's carried loop instead (RUTO_CLIP,
  `Act_LiftRest_Wait`, the clip her own `Act_LiftRest` command loops while Link carries her). Only that copy's
  AS changes: the game's own Link and the plain puppets keep the game's file;
- the player's AnimationParam with Ruto's animation project added as a retargeted resource (RUTO_ANIMATION,
  `IsRetarget` true, as the game's own child Epona plays the adult Epona's clips), so the clip plays on Link's
  skeleton by bone name (Arm_1/2, Leg_1/2, Spine_1/2 are named alike in both).

The AS binary (`ASB `, version 0x41b) as far as this needs it, read from the game's files: the string pool starts
at header word 14 (file offset 0x3c) and is the file's last section, its size is header word 23 (0x60), and a
node's clip name is a u32 offset into that pool. The new name goes at the pool's end (no other offset moves) and
the two clip fields point at it. Anything not as expected (another version, a pool that is not last, a clip name
missing or not named exactly once): no carry AS, and the copy plays the game's own `Eaten` (the fallback).
"""
import struct

MAGIC = b"ASB "
VERSION = 0x41B
POOL_AT = 0x3C     # header word 14: the string pool's file offset
POOL_SIZE_AT = 0x60  # header word 23: its size
EATEN_CLIPS = ("Eaten_F", "Eaten_B")
RUTO_CLIP = "Act_LiftRest_Wait"
RUTO_ANIMATION = "Npc_Zora_ZU_Ruto_Child_Animation"
CARRY_AS = "AS/CoopPuppet_Carry_Child.root.asb"
CARRY_VARIANTS = ("Child",)


def _pool(raw):
    if len(raw) < POOL_SIZE_AT + 4 or raw[:4] != MAGIC or struct.unpack_from("<I", raw, 4)[0] != VERSION:
        return None
    at, size = struct.unpack_from("<I", raw, POOL_AT)[0], struct.unpack_from("<I", raw, POOL_SIZE_AT)[0]
    if at < POOL_SIZE_AT + 4 or at + size != len(raw):
        return None
    return at, size


def string_offset(raw, name):
    """`name`'s offset in the pool (a whole, NUL-ended string), or None."""
    p = _pool(raw)
    if p is None:
        return None
    at, _ = p
    want = name.encode() + b"\0"
    i = raw.find(want, at)
    while i >= 0:
        if i == at or raw[i - 1] == 0:
            return i - at
        i = raw.find(want, i + 1)
    return None


def references(raw, offset):
    """The 4-aligned file offsets before the pool whose u32 is `offset`."""
    at, _ = _pool(raw)
    return [i for i in range(0, at - 3, 4) if struct.unpack_from("<I", raw, i)[0] == offset]


def carry_as(raw, clips=EATEN_CLIPS, to=RUTO_CLIP):
    """The player's AS root `raw` with `clips` naming `to`, or None (then the copy keeps the game's AS)."""
    p = _pool(raw)
    if p is None:
        return None
    at, size = p
    fields = []
    for clip in clips:
        off = string_offset(raw, clip)
        if off is None:
            return None
        refs = references(raw, off)
        if len(refs) != 1:
            return None
        fields.append(refs[0])
    out = bytearray(raw)
    new = string_offset(raw, to)
    if new is None:
        new = size
        out += to.encode() + b"\0"
        struct.pack_into("<I", out, POOL_SIZE_AT, size + len(to) + 1)
    for f in fields:
        struct.pack_into("<I", out, f, new)
    return bytes(out)


def carry_animation_param(doc):
    """The player's AnimationParam `doc` with Ruto's animations added, retargeted (a new dict)."""
    res = [dict(r) for r in doc.get("AnimationResources", [])]
    if not any(r.get("ModelProjectName") == RUTO_ANIMATION for r in res):
        res.append({"IsRetarget": True, "ModelProjectName": RUTO_ANIMATION})
    out = dict(doc)
    out["AnimationResources"] = res
    return out


def carry_animation_path(actor):
    return f"Component/AnimationParam/{actor}.engine__component__AnimationParam.bgyml"
