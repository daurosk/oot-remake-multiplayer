"""The Goron and Zora tunics in the player's colour (docs/puppet.md, "The Goron and Zora tunics").

These two are not the Kokiri tunic with another tile: each is an actor of its own
(`Armor_Adult_Goron`, `Armor_Adult_Zora`) with its own model and its own textures (ASTC), worn
over Link's body, whose green parts it hides. So a colour is, per tunic:

- a copy of its cloth textures with only the cloth's own colour changed (the Goron tunic's red,
  the Zora tunic's blue; `puppet_pack.tunic_extra`, in place, same size), under a name of the
  same length (`Mt_Angin_Armor_Adult_Goron_Alb` -> `Mt_Angin_CoopR_Adult_Goron_Alb`);
- a copy of the tunic's model that names them, `Model/CoopTunic_Goron_Red.Armor_Adult_Goron.bfres`;
- a copy of the tunic's actor with that model, `CoopArmor_Goron_Red`, which a puppet of that
  colour wears instead of the game's (lichii/hook/coop_puppet.cpp, Planner::ArmorFor).

What stays the game's, so that the tunic still reads as what it is: the Goron tunic's gold trim,
plates, beads and dark undershirt, the Zora tunic's fins, scales' pattern, ornaments and grey
undershirt. Only the cloth takes the colour, as the Kokiri tunic's does.

Green (no colour) is the game's own model under the copy's name, no texture copies: the green
puppet's tunic, for when your own Link's tunic model is replaced by your colour's.

Nothing from the game is in this file: everything is made from the user's own romfs.
"""
from . import tunic_extra
from .tunic import COLORS, TunicError, rename


def _sat(rgb, least):
    hi, lo = max(rgb[:3]), min(rgb[:3])
    return hi >= 6 and hi - lo >= max(3, least * hi)


def is_red(rgb):
    """The Goron tunic's red cloth, its dark folds included (hue 325..22, a third saturated or
    more); not the gold trim (hue 30..60) or the dull brown plates (a quarter saturated)."""
    h = tunic_extra.hue(rgb)
    return h is not None and (h >= 325 or h <= 22) and _sat(rgb, 0.35)


def is_blue(rgb):
    """The Zora tunic's blue cloth (hue 185..265); not grey, white or the brown leather."""
    h = tunic_extra.hue(rgb)
    return h is not None and 185 <= h <= 265 and _sat(rgb, 0.12)


# kind -> the game's actor (its pack, its model project and model name are all this), the test
# for the cloth's colour, and the cloth textures in Tex/. The first texture is the main one: its
# cloth's average brightness becomes the colour, and the others keep their shade against it.
ARMORS = {
    "Goron": {
        "actor": "Armor_Adult_Goron",
        "pick": is_red,
        "textures": ("Mt_Angin_Armor_Adult_Goron_Alb", "Mt_Vest_Armor_Adult_Goron_Alb"),
    },
    "Zora": {
        "actor": "Armor_Adult_Zora",
        "pick": is_blue,
        "textures": ("Mt_Angin_Armor_Adult_Zora_Alb", "Mt_Angin_Acs_Armor_Adult_Zora_Alb",
                     "Mt_Vest_Armor_Adult_Zora_Alb"),
    },
}


def actor_name(kind, color):
    """The tunic's actor in a colour: what a puppet of that colour wears."""
    return f"CoopArmor_{kind}_{color.capitalize()}"


def project_name(kind, color):
    """The model project of a colour: `Model/<this>.<the game's actor>.bfres`."""
    return f"CoopTunic_{kind}_{color.capitalize()}"


def texture_name(name, color):
    """The colour's copy of a texture, as long as the game's name (`_Armor_` -> `_CoopR_`)."""
    if "_Armor_" not in name:
        raise TunicError(f"{name}: no _Armor_ in the name")
    return name.replace("_Armor_", f"_Coop{COLORS[color][0]}_", 1)


def base_luma(main, kind):
    """The linear luma of the main texture's cloth that becomes the colour."""
    base = tunic_extra.picked_luma(main, ARMORS[kind]["pick"])
    if base <= 0:
        raise TunicError(f"the {kind} tunic's main texture has none of its cloth colour")
    return base


def parsed(data, kind):
    """`tunic_extra.astc_parsed` for the tunic's cloth colour (None for a BC texture): pass it to
    `texture_for` for every colour, the texture is then read once."""
    return tunic_extra.astc_parsed(data, ARMORS[kind]["pick"]) if tunic_extra.is_astc(data) else None


def texture_for(data, kind, name, color, base, parsed_blocks=None):
    """The colour's copy of one of the tunic's textures, renamed. Not for green (no copies)."""
    if color == "green":
        raise TunicError("green wears the game's own textures")
    out = tunic_extra.recolor(data, color, base, ARMORS[kind]["pick"], parsed_blocks)
    return rename(out, name, texture_name(name, color), at_least=1)


def model_for(model, kind, color, names):
    """The tunic's model naming the colour's copies of `names` (the textures that were made);
    green: the game's model as it is."""
    if color == "green":
        return model
    for n in names:
        model = rename(model, n, texture_name(n, color), at_least=1, at_most=1)
    return model
