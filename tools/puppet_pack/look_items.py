"""Gear colours on the puppets (docs/puppet.md, "Gear colours on the puppets").

A puppet's shield, sword, boots and gauntlets are the game's own item actors, and every Link on
screen draws the same item with the same textures. So colouring one player's shield would colour
everyone's. The pack therefore makes SLOTS copies of each coloured item, each with textures of its
own:

- the actor `<item>_Look<k>` (k = 1..SLOTS): the game's pack of the item with its ActorParam under
  the new name and a ModelInfo naming the model project `<item>_Look<k>`; its ActorInfo and
  GameActorInfo rows are the item's, renamed;
- that project's models: the item's model files with every gear texture name (look_live.GEAR)
  changed to the slot's (`Mt_...` -> `C<k>_...`, as long, so nothing in the file moves);
- the slot's textures `Tex/C<k>_....bntx`: the game's texture under the slot's name, the game's own
  texels.

Each slot has two copies (COPIES_PER_SLOT; copy k = 2 * slot - 1 or 2 * slot). A player who colours
a part gets a slot (lichii/hook/coop_look_share.h); the co-op module paints the slot's copy that is
not worn in their look and Longshot's texture overrides (patch 0020) draw that copy's textures in
it (hookshot/src/coop_look_live.cpp, nothing of the game is written); then their puppet wears
that copy (lichii/hook/coop_puppet.cpp).
Nothing from the game is kept in the tool: everything is made from the user's own romfs.
"""
from . import byml, sarc
from .look_live import GEAR
from .tunic import TunicError, rename

SLOTS = 4
COPIES_PER_SLOT = 2
MAX_SLOTS = 4  # copies 1..8: the texture prefix C<k>_ has one digit

# The coloured items (hookshot/src/coop_look_parts.cpp names their textures). The Kokiri boots are
# in the player's own model: they are not an item and stay the game's on puppets.
ITEMS = (
    "Weapon_Shield_Hylian",
    "Weapon_Shield_Mirror",
    "Weapon_Shield_Deku",
    "Weapon_Sword_Kokiri",
    "Weapon_Sword_Master",
    "Weapon_Sword_Biggoron",
    "Armor_Adult_HoverBoot",
    "Armor_Adult_HeavyBoot",
    "Armor_Adult_Gauntlet_Silver",
    "Armor_Adult_Gauntlet_Gold",
    "Armor_Adult_Goron",
    "Armor_Adult_Zora",
)

PREFIX = "Mt_"


def actor_name(item, slot):
    return f"{item}_Look{slot}"


def tex_name(name, slot):
    """The slot's name of a gear texture: `Mt_` -> `C<k>_` (as long)."""
    if not name.startswith(PREFIX) or not 1 <= slot <= 9:
        raise TunicError(f"{name}: no slot name for slot {slot}")
    return f"C{slot}_" + name[len(PREFIX):]


def model_for(model, slot):
    """A model copy naming the slot's gear textures; (bytes, the gear names it names)."""
    named = [g for g in GEAR if (g.encode() + b"\0") in model]
    for g in named:
        model = rename(model, g, tex_name(g, slot), at_least=1)
    return model, named


def texture_for(bntx, name, slot):
    """The game's texture under the slot's name."""
    return rename(bntx, name, tex_name(name, slot), at_least=1)


def actor_param_path(actor):
    return f"Actor/{actor}.engine__actor__ActorParam.bgyml"


def model_info_path(actor):
    return f"Component/ModelInfo/{actor}.engine__component__ModelInfo.bgyml"


def _member(ref):
    if not isinstance(ref, str) or not ref.startswith("?"):
        return None
    return ref[1:]


def model_info(members, item):
    """(FmdbName, ModelProjectName, member path) of the item's own model."""
    ap = members.get(actor_param_path(item))
    if ap is None:
        raise TunicError(f"{item}: no {actor_param_path(item)} in its pack")
    comps = byml.loads(ap).get("Components", {})
    path = _member(comps.get("ModelInfoRef"))
    if path is None or path not in members:
        raise TunicError(f"{item}: its ActorParam names no ModelInfo in the pack")
    info = byml.loads(members[path])
    fmdb, project = info.get("FmdbName"), info.get("ModelProjectName")
    if not isinstance(fmdb, str) or not isinstance(project, str):
        raise TunicError(f"{item}: its ModelInfo has no FmdbName and ModelProjectName")
    return fmdb, project, path


def copy_pack(members, item, slot):
    """(pack bytes, actor param bytes, model info bytes) of the slot's copy of `item`: the game's
    pack, its ActorParam under the copy's name naming a ModelInfo of the copy's own project."""
    fmdb, _, _ = model_info(members, item)
    actor = actor_name(item, slot)
    doc = byml.loads(members[actor_param_path(item)])
    doc["Components"]["ModelInfoRef"] = "?" + model_info_path(actor)
    info = byml.dumps({"FmdbName": fmdb, "ModelProjectName": actor})
    ap = byml.dumps(doc)
    files = {k: v for k, v in members.items() if k != actor_param_path(item)}
    files[actor_param_path(actor)] = ap
    files[model_info_path(actor)] = info
    return sarc.write(files), ap, info


def info_row(row, item, slot):
    """The copy's ActorInfo row: the item's, under the copy's name and model project."""
    actor = actor_name(item, slot)
    out = dict(row)
    out["__RowId"] = actor
    if "ActorName" in out:
        out["ActorName"] = actor
    out["ModelProjectName"] = actor
    return out
