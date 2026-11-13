"""Your own Link's colour changed while the game runs (docs/puppet.md, "Changing your colour in the game").

The game keeps a loaded texture's texels in its memory as they are in the file. Hookshot draws a
texture again when those bytes change, so the co-op module can put another colour's texels over
them: your Link changes at once, with no restart. For that the pack holds, per colour, the texels
of every tunic texture your Link wears, each in the game's own format and size:

- the tile (`puppet_pack.tunic`: hat and upper body) recoloured as for the puppets;
- the dark tile (the child's tunic body) and the other tunic textures with only their green end
  colours changed in place (`puppet_pack.tunic_extra`);
- green: the game's own texels, to find the textures by and to go back to.

One file per colour, `coop_look/<colour>.bin`, beside the pack's `romfs/` (the game does not load
it). Little-endian:

    "COOPLOOK", u32 version (1), u32 count, then per texture:
    its name in the game (64 bytes, NUL-filled), u64 size, the texels (the BNTX's data block)

Read by hookshot/src/coop_look_tunic.cpp.

Nothing from the game is in this file: everything is made from the user's own romfs.
"""
import struct

from .tunic import TunicError, _brti

DIR = "coop_look"
MAGIC = b"COOPLOOK"
VERSION = 1
NAME_BYTES = 64


def texels(bntx):
    """The data block of a one-texture BNTX: every mip level, as the game has it in memory."""
    _, start, end = _brti(bntx)
    return bntx[start:end]


def pack(textures):
    """[(the texture's name in the game, its BNTX file)] -> the colour's file."""
    out = [MAGIC, struct.pack("<II", VERSION, len(textures))]
    for name, bntx in textures:
        n = name.encode("ascii")
        if not 0 < len(n) < NAME_BYTES:
            raise TunicError(f"texture name '{name}' does not fit {NAME_BYTES - 1} bytes")
        data = texels(bntx)
        out += [n.ljust(NAME_BYTES, b"\0"), struct.pack("<Q", len(data)), data]
    return b"".join(out)


def unpack(blob):
    """The colour's file -> [(name, texels)]."""
    if blob[:8] != MAGIC:
        raise TunicError("not a look file")
    version, count = struct.unpack_from("<II", blob, 8)
    if version != VERSION:
        raise TunicError(f"look file version {version}, not {VERSION}")
    out, at = [], 16
    for _ in range(count):
        name = blob[at:at + NAME_BYTES].rstrip(b"\0").decode("ascii")
        size = struct.unpack_from("<Q", blob, at + NAME_BYTES)[0]
        at += NAME_BYTES + 8
        if at + size > len(blob):
            raise TunicError("the look file is cut short")
        out.append((name, blob[at:at + size]))
        at += size
    return out
