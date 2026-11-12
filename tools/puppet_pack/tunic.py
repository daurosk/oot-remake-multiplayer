"""Tunic colours for the puppets (docs/puppet.md, "Colour").

Link's tunic and cap (adult `Mt_Angin`, child cloth) take their colour from one tile texture,
`Tex/Angin_Tile_A_Alb.bntx`, which the models name as a string. The game draws its other looks the
same way: a tunic of another colour is another model (Armor_Adult_Goron names its own albedo), and a
model finds its textures in `Tex/` by name. Material colour parameters written at run time never
showed on screen (reports puppet-colors-2), so a colour here is data:

- a copy of the tile, recoloured, under a name of the same length (`Angin_Tile_R_Alb`);
- a copy of the player's model that names that tile instead (same length, so nothing in the
  file moves), saved as `Model/<project>.<Player_Adult|Player_Child>.bfres`;
- a puppet actor whose ModelInfo (and ActorInfo row) names that project.

The tile is BC3 (BC1 is handled too): each 4x4 block has two RGB565 end colours and the texels are
fixed blends of them, so recolouring the two end colours of every block recolours every texel with
no encoder and no loss beyond the 565 rounding. The block order (Tegra block-linear tiling) does
not matter since every block gets the same change.

Nothing from the game is in this file: everything is made from the user's own romfs.
"""
import struct

TILE = "Angin_Tile_A_Alb"

# name -> (letter in the tile's name, sRGB colour). Green is the game's own tile (no copy). The
# names match lichii/hook/coop_colors.cpp and the kit's coop-ini.ps1.
COLORS = {
    "red": ("R", (0xD0, 0x20, 0x20)),
    "orange": ("O", (0xF0, 0x80, 0x10)),
    "yellow": ("Y", (0xE8, 0xD0, 0x20)),
    "cyan": ("C", (0x20, 0xC8, 0xD8)),
    "blue": ("B", (0x20, 0x50, 0xE0)),
    "purple": ("P", (0x80, 0x30, 0xC8)),
    "pink": ("M", (0xF0, 0x70, 0xB0)),
    "white": ("W", (0xE8, 0xE8, 0xE8)),
    "black": ("K", (0x28, 0x28, 0x28)),
    # The game's own tile under another name, not recoloured (the colour is only for the list):
    # the green puppet, for when your own Link wears another colour (SELF_DIR).
    "green": ("G", (0x30, 0xA0, 0x40)),
}

# Your own Link in your colour: the game's tile recoloured under its own name, one per colour,
# kept beside the pack (not in romfs/, so the game does not load it). The kit copies the one of
# your colour over romfs/Tex/<TILE>.bntx.zs at each start (scripts/kit-paths.ps1
# Set-KitSelfTunic); green or none: no copy, the game's own tile.
SELF_DIR = "coop_self"

BC1, BC3 = 0x1A, 0x1C


class TunicError(ValueError):
    pass


def tile_name(color):
    return f"Angin_Tile_{COLORS[color][0]}_Alb"


def project_name(variant, color):
    """The model project of a colour: `Model/<this>.Player_<variant>.bfres`."""
    return f"CoopTunic_{variant}_{color.capitalize()}"


def actor_name(variant, color):
    return f"CoopPuppet_{variant}_{color.capitalize()}"


# --- colour maths (sRGB 0..255 <-> linear 0..1) ---

def _lin(c):
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def _srgb(x):
    x = min(max(x, 0.0), 1.0)
    c = x * 12.92 if x <= 0.0031308 else 1.055 * x ** (1 / 2.4) - 0.055
    return int(round(c * 255))


def _luma(rgb_lin):
    r, g, b = rgb_lin
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def unpack565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def pack565(rgb):
    r, g, b = rgb
    return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)


class Recolor:
    """Maps a texel to `target` scaled by its brightness against `base_luma` (the texture's
    average), so the tile's weave and shading stay and its average becomes the target."""

    def __init__(self, target, base_luma):
        if base_luma <= 0:
            raise TunicError("the texture is black")
        self.target = tuple(_lin(c) for c in target)
        self.base = base_luma
        self._memo = {}

    def __call__(self, v565):
        out = self._memo.get(v565)
        if out is None:
            k = _luma(tuple(_lin(c) for c in unpack565(v565))) / self.base
            out = pack565(tuple(_srgb(t * k) for t in self.target))
            self._memo[v565] = out
        return out


# --- BNTX ---

def _brti(data):
    if data[:4] != b"BNTX":
        raise TunicError("not a BNTX file")
    i = data.find(b"BRTI")
    j = data.find(b"BRTD")
    if i < 0 or j < 0 or data.find(b"BRTI", i + 4) >= 0:
        raise TunicError("expected one texture in the BNTX")
    fmt = struct.unpack_from("<I", data, i + 0x1C)[0] >> 8 & 0xFF
    size = struct.unpack_from("<Q", data, j + 8)[0]
    return fmt, j + 0x10, j + size


def _blocks(fmt):
    if fmt == BC3:
        return 16, 8
    if fmt == BC1:
        return 8, 0
    raise TunicError(f"texture format 0x{fmt:02x} is not BC1 or BC3")


def average_luma(data):
    """Mean linear brightness of the end colours of every non-empty block."""
    fmt, start, end = _brti(data)
    step, off = _blocks(fmt)
    total, n = 0.0, 0
    memo = {}
    for p in range(start, end - step + 1, step):
        c0, c1, idx = struct.unpack_from("<HHI", data, p + off)
        if c0 == 0 and c1 == 0 and idx == 0:
            continue  # padding
        for c in (c0, c1):
            y = memo.get(c)
            if y is None:
                y = memo[c] = _luma(tuple(_lin(x) for x in unpack565(c)))
            total += y
            n += 1
    if n == 0:
        raise TunicError("the texture has no data")
    return total / n


def recolor_bntx(data, target, old_name, new_name):
    """A copy of a BC1/BC3 BNTX with every block's end colours recoloured and the texture's name
    changed (same length)."""
    if len(old_name) != len(new_name):
        raise TunicError("the new name must be as long as the old one")
    fmt, start, end = _brti(data)
    step, off = _blocks(fmt)
    f = Recolor(target, average_luma(data))
    out = bytearray(data)
    for p in range(start, end - step + 1, step):
        q = p + off
        c0, c1, idx = struct.unpack_from("<HHI", out, q)
        if c0 == 0 and c1 == 0 and idx == 0:
            continue
        n0, n1 = f(c0), f(c1)
        if fmt == BC1:
            n0, n1, idx = _bc1_order(c0 > c1, n0, n1, idx)
        struct.pack_into("<HHI", out, q, n0, n1, idx)
    return rename(bytes(out), old_name, new_name, at_least=1)


def _bc1_order(four, n0, n1, idx):
    """BC1 picks 4-colour mode when c0 > c1: keep the block's mode after the recolour."""
    if four == (n0 > n1):
        return n0, n1, idx
    if n0 == n1:
        if four:  # cannot be 4-colour any more: every texel the one colour
            return n0, n1, 0
        return n0, n1, idx  # 3-colour mode kept (c0 <= c1)
    # swap the ends: index 0 <-> 1; in 4-colour mode 2 <-> 3 too, in 3-colour mode 2 and 3 stay
    x = 0
    for k in range(16):
        i = idx >> (2 * k) & 3
        if four or i < 2:
            i ^= 1
        x |= i << (2 * k)
    return n1, n0, x


def rename(data, old, new, at_least=1, at_most=None):
    """Replaces every NUL-ended `old` string with `new` (same length)."""
    if len(old) != len(new):
        raise TunicError("the new name must be as long as the old one")
    a, b = old.encode() + b"\0", new.encode() + b"\0"
    n = data.count(a)
    if n < at_least or (at_most is not None and n > at_most):
        raise TunicError(f"expected {at_least}..{at_most or 'any'} '{old}' strings, found {n}")
    return data.replace(a, b)


def tile_for(data, color):
    """The tile of a colour: green is the game's own tile renamed, the rest recoloured."""
    if color == "green":
        return rename(data, TILE, tile_name(color), at_least=1)
    return recolor_bntx(data, COLORS[color][1], TILE, tile_name(color))


def self_tile(data, color):
    """The game's tile recoloured under its own name: your own Link in `color` (not green)."""
    return recolor_bntx(data, COLORS[color][1], TILE, TILE)


def model_for(model, color):
    """A copy of a player model (bfres) that names the colour's tile instead of the green one."""
    return rename(model, TILE, tile_name(color), at_least=1, at_most=1)
