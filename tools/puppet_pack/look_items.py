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
from .look_live import GEAR, mark_bntx
from .tunic import COLORS, TunicError, rename

SLOTS = 4
COPIES_PER_SLOT = 2
MAX_SLOTS = 4  # copies 1..8: the texture prefix C<k>_ has one digit

# The coloured items (hookshot/src/coop_look_parts.cpp names their textures). The Kokiri boots are
# in the player's own model, not an item: see BODY_BOOTS.
ITEMS = (
    "Weapon_Shield_Hylian",
    "Weapon_Shield_Mirror",
    "Weapon_Shield_Deku",
    "Weapon_Sword_Kokiri",
    "Weapon_Sword_Master",
    "Weapon_Sword_Biggoron",
    # The Giant's Knife and the broken one are their own actors on the Biggoron model project
    # (their ModelInfo names it, the Giant's through `$parent`): copies of their own, or a puppet
    # holding one wears the game's actor, whose blade shows only while your own sword is out.
    "Weapon_Sword_Giant",
    "Weapon_Sword_BrokenGiant",
    "Armor_Adult_HoverBoot",
    "Armor_Adult_HeavyBoot",
    "Armor_Adult_Gauntlet_Silver",
    "Armor_Adult_Gauntlet_Gold",
    "Armor_Adult_Goron",
    "Armor_Adult_Zora",
)

PREFIX = "Mt_"

# Wearer copies (held-piece). The weapon component (an ActorParam's `WeaponRef`; its update
# f_710093e720, read from the game's code) writes shader values into the item's model by name
# hash every frame, from the item's state and its wearer's EquipmentUser (0x673475da: drawn or
# swinging, with the wearer's SwingStrength; 0xe7159148: 0; 0x29efb878 only with the tag
# Equipment_IsHideBladeMaterial, which no actor of this game has). Those values live with the
# model resource, which every actor of one name shares: a puppet in the game's own sword shows
# its blade only while your own sword is out, and flickers when two wearers disagree. The Look
# copies above give a puppet a resource of its own only in a colour slot (4 slots, Hookshot with
# patch 0020). So every player item with a weapon component gets one copy per player number,
# `<item>_Blade<n>` (n = 1..BLADE_COPIES, the protocol's kMaxPlayers), whose models are the
# model project's once per number, `<project>_Blade<n>` (the Giant's Knife and the broken one
# share the Biggoron's): the game's bytes and textures, unchanged. The list is read from the
# romfs (wearer_items): an item pack whose ActorParam, with its $parents, has a WeaponRef and a
# model, and no PerimeterAnalyserRef (the Poes' weapons: enemies hold them), not a `_Base`.
# A puppet without a ready Look copy wears its player number's copy
# (lichii/hook/coop_puppet.cpp DressInBlades), on every runtime.
BLADE_COPIES = 16
WEARER_COMPONENT = "WeaponRef"
ENEMY_COMPONENT = "PerimeterAnalyserRef"


def blade_name(item, number):
    return f"{item}_Blade{number}"


# Row 198 (0.11.9): a wearer copy's model named the game's own gear textures, so the puppet's sword was drawn with
# the very texture your own Link's colour is drawn over (the game loads a texture once, by its name), and its texels
# were the game's (your Link's wardrobe finds its textures by them). Each wearer copy now names gear textures of its
# own, `Mt_` -> `W<a..p>_` (as long; one letter a player number), their texels marked BLADE_MARK_BASE + the number:
# never a look copy's 1..8, a boots colour's letter, or the tunic's marks. Painted by nobody: the game's colours.
BLADE_MARK_BASE = 0x80


def blade_tex_name(name, number):
    """A wearer copy's name of a gear texture: `Mt_` -> `W<letter>_` (as long), a for number 1 to p for 16."""
    if not name.startswith(PREFIX) or not 1 <= number <= BLADE_COPIES:
        raise TunicError(f"{name}: no wearer copy name for number {number}")
    return f"W{chr(ord('a') + number - 1)}_" + name[len(PREFIX):]


def blade_mark(number):
    """The mark of a wearer copy's gear textures (look_live.mark): BLADE_MARK_BASE + its number."""
    if not 1 <= number <= BLADE_COPIES:
        raise TunicError(f"no wearer copy mark for number {number}")
    return BLADE_MARK_BASE + number


def blade_model_for(model, number):
    """A wearer copy's model naming its own gear textures; (bytes, the gear names it names)."""
    named = [g for g in GEAR if (g.encode() + b"\0") in model]
    for g in named:
        model = rename(model, g, blade_tex_name(g, number), at_least=1)
    return model, named


def blade_texture_for(bntx, name, number):
    """The game's gear texture under a wearer copy's name, its texels marked with the copy's mark."""
    return mark_bntx(rename(bntx, name, blade_tex_name(name, number), at_least=1), blade_mark(number))


def actor_components(members, item):
    """The item's ActorParam components with its $parents (from its pack) merged in; None when
    the pack lacks its ActorParam or a parent."""
    path = actor_param_path(item)
    comps, seen = {}, set()
    while path is not None:
        if path in seen or path not in members:
            return None
        seen.add(path)
        doc = byml.loads(members[path])
        for k, v in (doc.get("Components") or {}).items():
            comps.setdefault(k, v)
        parent = doc.get("$parent")
        if parent is None:
            break
        path = parent[len("Work/"):] if parent.startswith("Work/") else parent
        path = path[:-len(".gyml")] + ".bgyml" if path.endswith(".gyml") else path
    return comps


def is_wearer_item(members, item):
    """Whether the game's weapon component sets this item's model values from its wearer."""
    if item.endswith("_Base"):
        return False
    comps = actor_components(members, item)
    return (comps is not None and WEARER_COMPONENT in comps and ENEMY_COMPONENT not in comps
            and "ModelInfoRef" in comps)


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
    """The game's texture under the slot's name, its texels marked with the slot's number
    (look_live.mark: the module tells your own Link's textures from the copies' by it)."""
    return mark_bntx(rename(bntx, name, tex_name(name, slot), at_least=1), slot)


def actor_param_path(actor):
    return f"Actor/{actor}.engine__actor__ActorParam.bgyml"


def model_info_path(actor):
    return f"Component/ModelInfo/{actor}.engine__component__ModelInfo.bgyml"


def _member(ref):
    if not isinstance(ref, str) or not ref.startswith("?"):
        return None
    return ref[1:]


def _merged(base, over):
    """`over` on top of `base`, as a `$parent` is: maps merge key by key, anything else is replaced."""
    out = dict(base)
    for k, v in over.items():
        out[k] = _merged(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else v
    return out


def _info_chain(members, item, path):
    """The ModelInfo at `path` and its `$parent`s (from the same pack): their member paths, `path` first."""
    chain = []
    while path is not None:
        if path in chain:
            raise TunicError(f"{item}: its ModelInfo's $parent loops at {path}")
        if path not in members:
            raise TunicError(f"{item}: its ModelInfo {path} is not in the pack")
        chain.append(path)
        parent = byml.loads(members[path]).get("$parent")
        if parent is None:
            break
        if not isinstance(parent, str):
            raise TunicError(f"{item}: its ModelInfo has a $parent that is no path")
        # `Work/<path>.gyml` names the pack's `<path>.bgyml`.
        path = parent[len("Work/"):] if parent.startswith("Work/") else parent
        path = path[:-len(".gyml")] + ".bgyml" if path.endswith(".gyml") else path
    return chain


def _resolved(members, item, path):
    """The ModelInfo at `path` with its `$parent`s (from the same pack) merged in, `$parent` dropped."""
    doc = {}
    for p in reversed(_info_chain(members, item, path)):
        doc = _merged(doc, byml.loads(members[p]))
    doc.pop("$parent", None)
    return doc


def model_info(members, item):
    """(FmdbName, ModelProjectName, member path) of the item's own model, through its `$parent`s."""
    ap = members.get(actor_param_path(item))
    if ap is None:
        raise TunicError(f"{item}: no {actor_param_path(item)} in its pack")
    comps = byml.loads(ap).get("Components", {})
    path = _member(comps.get("ModelInfoRef"))
    if path is None or path not in members:
        raise TunicError(f"{item}: its ActorParam names no ModelInfo in the pack")
    info = _resolved(members, item, path)
    fmdb, project = info.get("FmdbName"), info.get("ModelProjectName")
    if not isinstance(fmdb, str) or not isinstance(project, str):
        raise TunicError(f"{item}: its ModelInfo has no FmdbName and ModelProjectName")
    return fmdb, project, path


def model_info_chain(members, item):
    """The member paths of the item's ModelInfo and its `$parent`s, its own first: the game files a copy's
    ModelInfo (copy_model_info, every `$parent` merged in) is made from, for its size-table entry
    (build.chain_entry: the Giant's Knife's merges two files)."""
    return _info_chain(members, item, model_info(members, item)[2])


def copy_model_info(members, item, actor, project_name=None):
    """The copy's ModelInfo: the item's whole ModelInfo (its `$parent`s merged in), every model
    project the item's own (its main model and any SubModels) renamed to the copy's. Only the
    names changed: a ModelInfo of the names alone lost the rest (the Biggoron's colour
    ModelVariationAnims, the Giant's Knife's broken SubModel)."""
    _, project, path = model_info(members, item)
    info = _resolved(members, item, path)
    new = project_name or actor
    info["ModelProjectName"] = new
    subs = info.get("SubModels")
    if isinstance(subs, list):
        info["SubModels"] = [dict(m, ModelProjectName=new)
                             if isinstance(m, dict) and m.get("ModelProjectName") == project else m
                             for m in subs]
    return info


def copy_pack(members, item, slot):
    """(pack bytes, actor param bytes, model info bytes) of the slot's copy of `item`: the game's
    pack, its ActorParam under the copy's name naming a ModelInfo of the copy's own project."""
    return copy_pack_as(members, item, actor_name(item, slot))


def copy_pack_as(members, item, actor, project_name=None):
    """copy_pack for any copy name (a Look slot's or a blade copy's); `project_name`: the model
    project it names, if not one of its own name (blade copies share one per model project)."""
    doc = byml.loads(members[actor_param_path(item)])
    doc["Components"]["ModelInfoRef"] = "?" + model_info_path(actor)
    anim = copy_animation_param(members, item, actor)
    if anim is not None:
        doc["Components"]["AnimationRef"] = "?" + anim[0]
    info = byml.dumps(copy_model_info(members, item, actor, project_name))
    ap = byml.dumps(doc)
    files = {k: v for k, v in members.items() if k != actor_param_path(item)}
    files[actor_param_path(actor)] = ap
    files[model_info_path(actor)] = info
    if anim is not None:
        files[anim[0]] = anim[1]
    return sarc.write(files), ap, info


def animation_param_path(actor):
    return f"Component/AnimationParam/{actor}.engine__component__AnimationParam.bgyml"


def item_animation_param(members, item):
    """(member path, doc) of the item's AnimationParam (its ActorParam's AnimationRef, $parents of the
    ActorParam merged in), or None when it names none or the pack lacks it."""
    comps = actor_components(members, item) or {}
    ref = comps.get("AnimationRef")
    path = _member(ref) if isinstance(ref, str) else None
    if path is None or path not in members:
        return None
    return path, byml.loads(members[path])


def copy_animation_param(members, item, actor):
    """Row 184: (member path, bytes) of a copy's own AnimationParam, or None when it needs none.
    A copy names a model project of its own (`<item>_Look<k>`, a blade copy's), and the game binds an
    animation project's clips to the model project they were made for: under another project they
    play only when the resource is retargeted (IsRetarget, by bone name: the game's own child Epona
    plays the adult's clips so). Without it the copy's own clips never move it (the child's Hylian
    Shield hung turned on a puppet's back, 0.11.7). So the copy's AnimationParam is the item's with
    IsRetarget true on every resource; the item's own file is not touched. An AnimationParam with no
    resources (the swords') or with a `$parent` (not seen in the coloured items) gives None."""
    found = item_animation_param(members, item)
    if found is None:
        return None
    _, doc = found
    res = doc.get("AnimationResources")
    if "$parent" in doc or not isinstance(res, list) or not res:
        return None
    out = dict(doc)
    out["AnimationResources"] = [dict(r, IsRetarget=True) if isinstance(r, dict) else r for r in res]
    return animation_param_path(actor), byml.dumps(out)


# Row 198 (0.11.9): the game parses a param in a heap of its size-table entry, and an AnimationParam's entry grows with
# its fields: the game's own (rsizetable 111) take 5344 bytes for one resource with no IsRetarget and 5376 with it,
# 5464 / 5528 for two resources (0x20 a resource that has the field). Row 184's copies were sized as the item's
# (5344), 0x20 short: the game refused to make the copy (its equip call still returned 1) and the puppet kept the
# game's own shield, whose textures your own colours are drawn over (your Customize colours on the other players'
# puppets). Each IsRetarget a copy adds gets this much room, twice the game's.
RETARGET_ROOM = 0x40


def copy_animation_param_entry(members, item, item_entry, data, margin):
    """The size-table entry of a copy's AnimationParam (copy_animation_param's `data`): the item's entry
    `item_entry` with RETARGET_ROOM for each resource the copy gave IsRetarget, or the file's bytes and `margin`
    when that is more."""
    found = item_animation_param(members, item)
    res = found[1].get("AnimationResources") if found else None
    added = sum(1 for r in res if isinstance(r, dict) and "IsRetarget" not in r) if isinstance(res, list) else 0
    return max(item_entry + RETARGET_ROOM * added, len(data) + margin)


def info_row(row, item, slot):
    """The copy's ActorInfo row: the item's, under the copy's name and model project."""
    return info_row_as(row, actor_name(item, slot))


def info_row_as(row, actor, project_name=None):
    """info_row for any copy name."""
    out = dict(row)
    out["__RowId"] = actor
    if "ActorName" in out:
        out["ActorName"] = actor
    out["ModelProjectName"] = project_name or actor
    return out


# The boots in the player's own model (the Kokiri boots; the child's leather texture holds his belts
# too). Every Link draws one texture of a name, so the puppets' body models (puppet_pack.tunic
# model_for, one per tunic colour) name a copy of their own per colour: `Mt_...` -> `P<L>_...`
# (L: the colour's letter, tunic.COLORS; as long). The module paints each colour's copy in the boots
# colour of the player who wears that colour (hookshot/src/coop_look_boots.cpp, same names and
# marks). Your own Link's model copy (coop_self) keeps the game's names: the module paints those.
BODY_BOOTS = (
    "Mt_Boots_Player_Adult_Alb",
    "Mt_Leather_Player_Child_Alb",
)


def boots_name(name, color):
    """A colour's name of a body boots texture: `Mt_` -> `P<L>_` (as long)."""
    if not name.startswith(PREFIX) or color not in COLORS:
        raise TunicError(f"{name}: no boots name for colour {color}")
    return f"P{COLORS[color][0]}_" + name[len(PREFIX):]


def boots_mark(color):
    """The mark of a colour's boots copy (look_live.mark): its letter's code, never a copy's 1..8."""
    return ord(COLORS[color][0])


def body_model_for(model, color):
    """A body model naming the colour's boots copies instead of the game's boots (those it names)."""
    for name in BODY_BOOTS:
        model = rename(model, name, boots_name(name, color), at_least=0)
    return model


def boots_texture_for(bntx, name, color):
    """The game's boots texture under the colour's name, its texels marked with the colour's mark."""
    return mark_bntx(rename(bntx, name, boots_name(name, color), at_least=1), boots_mark(color))
