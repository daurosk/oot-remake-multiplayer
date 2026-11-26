"""Your own Link's colour changed while the game runs (docs/puppet.md, "Changing your colour in the game").

The game keeps a loaded texture's texels in its memory as they are in the file. Hookshot draws a
texture again when those bytes change, so the co-op module can put another colour's texels over
them: your Link changes at once, with no restart. For that the pack holds, per colour, the texels
of every tunic texture your Link wears, each in the game's own format and size:

- the tile (`puppet_pack.tunic`: hat and upper body) recoloured as for the puppets;
- the dark tile (the child's tunic body) and the other tunic textures with only their green end
  colours changed in place (`puppet_pack.tunic_extra`);
- green: the game's own texels marked (tunic.SELF_MARK), as your own Link's green S copies hold
  them, to find the textures by and to go back to. Never the game's own unmarked: those tunic
  textures are on Mido's and Saria's models too (every Kokiri), and the module finds textures by
  their texels.

One file per colour, `coop_look/<colour>.bin`, beside the pack's `romfs/` (the game does not load
it). Little-endian:

    "COOPLOOK", u32 version (1), u32 count, then per texture:
    its name in the game (64 bytes, NUL-filled), u64 offset of the texels in its BNTX (the data
    block's header is the 16 bytes in front), u64 size, the texels (the image: every mip level,
    BRTI's image size, as the game's texture memory holds it)

Read by hookshot/src/coop_look_tunic.cpp.

The gear (docs/puppet.md, "The Link customizer"): `coop_look/gear.bin`, the same format, holds the
game's own texels of every shield, sword, boots, gauntlets and Goron / Zora tunic texture in GEAR,
and of the metalness textures of the metal items (look_metal; `coop_look/metal.bin` says which of
their blocks lie under which part).
The module paints their parts in the colours the player picks (any colour: hookshot/src/
coop_look_parts.cpp, whose table names the same textures) and writes them over the game's.

Nothing from the game is in this file: everything is made from the user's own romfs.
"""
import struct

from .look_metal import TEXTURES as METAL_TEXTURES
from .tunic import TunicError, _brti

DIR = "coop_look"
MAGIC = b"COOPLOOK"
VERSION = 1
NAME_BYTES = 64


# The gear textures in Tex/ (hookshot/src/coop_look_parts.cpp Gear(): the same names, checked by
# the tests).
GEAR = (
    "Mt_Weapon_Shield_Hylian_Alb",
    "Mt_Weapon_Shield_Mirror_Alb",
    "Mt_Body_Weapon_Shield_Deku_Alb",
    "Mt_Weapon_Sword_Kokiri_Alb",
    "Mt_Weapon_Sword_Master_Alb",
    "Mt_Weapon_Sword_Biggoron_Alb.00",
    "Mt_Weapon_Sword_Biggoron_Alb.01",
    "Mt_Boots_Player_Adult_Alb",
    "Mt_Leather_Player_Child_Alb",   # the child's boots (and his belts: one texture)
    # leather-link: the adult's gloves and upper leather (Clothing Color 3 on Link, link_leather only).
    "Mt_Glove_Player_Adult_Alb",
    "Mt_Leater_Upper_Player_Adult_Alb",
    "Mt_Boots_Armor_Adult_HoverBoot_Alb",
    "Mt_Boots_Armor_Adult_HeavyBoot_Alb",
    "Mt_Silver_Armor_Adult_Gauntlet_Silver_Alb",
    "Mt_Gold_Armor_Adult_Gauntlet_Gold_Alb",
    "Mt_Angin_Armor_Adult_Goron_Alb",
    "Mt_Vest_Armor_Adult_Goron_Alb",
    "Mt_Angin_Armor_Adult_Zora_Alb",
    "Mt_Vest_Armor_Adult_Zora_Alb",
    "Mt_Angin_Acs_Armor_Adult_Zora_Alb",
    # The rest of the adult's Goron and Zora tunic cloth (their models name them; the dump's end colours:
    # the Goron inner 82% crimson, the Zora parts 45% blue, the Zora feather 49% blue / cyan).
    "Mt_Inner_Armor_Adult_Goron_Alb",
    "Mt_Parts_Armor_Adult_Zora_Alb",
    "Mt_Feather_Armor_Adult_Zora_Alb",
    # The metalness textures of the metal parts (#202, look_metal: a painted metal part's metal is
    # painted down with it). In gear.bin for the game's own texels; the item copies name their own.
) + METAL_TEXTURES
GEAR_FILE = "gear.bin"


def gear(read_tex, log=print):
    """The gear file: the game's own texels of each GEAR texture `read_tex(name)` gives (its BNTX,
    or None when the romfs has none: left out and said). None when there is none at all."""
    found, missing = [], []
    for name in GEAR:
        bntx = read_tex(name)
        if bntx is None:
            missing.append(name)
            continue
        found.append((name, bntx))
    if missing:
        log(f"look: no {', '.join(missing)} in the romfs: those parts keep the game's colours")
    if not found:
        return None
    return pack(found)


def _image(bntx):
    """(start, size) of a one-texture BNTX's image in the file: every mip level, the image's own
    size (BRTI +0x50), without the data block's padding to 4096 bytes."""
    _, start, end = _brti(bntx)
    i = bntx.find(b"BRTI")
    size = struct.unpack_from("<I", bntx, i + 0x50)[0]
    if not 0 < size <= end - start:
        raise TunicError(f"image size {size} does not fit the data block ({end - start} bytes)")
    return start, size


def texels(bntx):
    """The image of a one-texture BNTX: every mip level, as the game copies it into its texture
    memory (the game's storage is this size)."""
    start, size = _image(bntx)
    return bntx[start:start + size]


def mark(data, slot):
    """The texels of a puppet's copy: the image's last byte XOR the copy's number. The game copies every texture's texels into its texture
    memory, with no file or name in front, so a copy holding the game's own texels could not be told
    from your own Link's; the mark tells them apart (the module marks its copies the same way:
    hookshot/src/coop_look_parts.cpp Mark). That byte is in the last mip level (a few pixels: the
    item seen from very far) or the image's padding past it: the 2-bit colour indices of the last
    pixel row of a BC1/BC3 block (in block-linear layout almost always the GOB padding of the last
    mip); for ASTC the weights of one texel of the last block. Not to be seen."""
    if not 1 <= slot <= 255 or not data:
        raise TunicError(f"no mark for copy {slot}")
    out = bytearray(data)
    out[-1] ^= slot
    return bytes(out)


def mark_bntx(bntx, slot):
    """A one-texture BNTX with its image marked as `mark` marks texels."""
    start, size = _image(bntx)
    return bntx[:start] + mark(bntx[start:start + size], slot) + bntx[start + size:]


def offset(bntx):
    """Where texels(bntx) starts in the file."""
    return _brti(bntx)[1]


def pack(textures):
    """[(the texture's name in the game, its BNTX file)] -> the colour's file."""
    out = [MAGIC, struct.pack("<II", VERSION, len(textures))]
    for name, bntx in textures:
        n = name.encode("ascii")
        if not 0 < len(n) < NAME_BYTES:
            raise TunicError(f"texture name '{name}' does not fit {NAME_BYTES - 1} bytes")
        data = texels(bntx)
        out += [n.ljust(NAME_BYTES, b"\0"), struct.pack("<QQ", offset(bntx), len(data)), data]
    return b"".join(out)


def unpack(blob):
    """The colour's file -> [(name, texels)] (the offsets: unpack_offsets)."""
    return [(name, data) for name, _, data in unpack_offsets(blob)]


def unpack_offsets(blob):
    """The colour's file -> [(name, offset, texels)]."""
    if blob[:8] != MAGIC:
        raise TunicError("not a look file")
    version, count = struct.unpack_from("<II", blob, 8)
    if version != VERSION:
        raise TunicError(f"look file version {version}, not {VERSION}")
    out, at = [], 16
    for _ in range(count):
        if at + NAME_BYTES + 16 > len(blob):
            raise TunicError("the look file is cut short")
        name = blob[at:at + NAME_BYTES].rstrip(b"\0").decode("ascii")
        off, size = struct.unpack_from("<QQ", blob, at + NAME_BYTES)
        at += NAME_BYTES + 16
        if at + size > len(blob):
            raise TunicError("the look file is cut short")
        out.append((name, off, blob[at:at + size]))
        at += size
    return out
