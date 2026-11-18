"""Custom character models on the puppets ("skins", docs/puppet.md "Custom models").

A character model mod for the game (a `romfs` folder, as made for emulators, for example a
"playable <character>" mod) points the game's own player actors at a model of its own: its
`Pack/Actor/Player_<Adult|Child>.pack` names its own ModelInfo, AnimationParam (Link's animations
retargeted onto its skeleton) and EquipmentUserParam, and it replaces `AS/Player_<age>.root.asb`
and the resource size table in place. Put into `mods` as it is, it changes every puppet too (they
read the same AS file) and its size table hides the puppet pack's entries.

So a skin is the mod repackaged under names of its own, never installed in place:

- a puppet actor `CoopPuppet_<age>_Skin_<id>` made from the mod's player pack (build.build_pack,
  as for the plain puppet), with its ASRef on a copy of the mod's AS root,
  `AS/CoopSkin_<id>_<age>.root.asb`;
- the mod's own new files (models, textures, retargeted animations, cloth): every file whose path
  the game's size table does not know. A file the game has (one the mod replaces in place) is left
  out, so it reaches no one else's Link;
- size-table entries from the mod's own table (it ships one), else from the file.

Nothing from the game or the mod is in this file: everything is read from the user's own copies.
"""
import hashlib
import os
import re

from . import byml, rstb, sarc

VARIANTS = ("Child", "Adult")
ID_RE = re.compile(r"^[A-Za-z0-9]{1,20}$")
# Top folders of a mod that never go into a skin: actor packs (the player's are read, not copied;
# others replace the game's actors), tables, text, sound, scenes and events, and code.
SKIP_TOP = {"Pack", "System", "RSDB", "Mals", "Sound", "SLink2", "Event", "Banc", "UI", "Font",
            "Layout", "Shader", "exefs"}
CODE_DIRS = ("exefs", "cheats")
MARGIN = 0x1000


class SkinError(ValueError):
    pass


def actor_name(variant, skin_id):
    return f"CoopPuppet_{variant}_Skin_{skin_id}"


def as_path(skin_id, variant):
    """The skin's own copy of the mod's AS root (a pack member path, no .zs)."""
    return f"AS/CoopSkin_{skin_id}_{variant}.root.asb"


def strip_zs(path):
    return path[:-3] if path.endswith(".zs") else path


def find_romfs(mod_dir):
    """The mod's romfs folder: <mod>/romfs, or the folder itself when it holds the game's layout."""
    for cand in (os.path.join(mod_dir, "romfs"), mod_dir):
        if os.path.isdir(os.path.join(cand, "Pack", "Actor")):
            return cand
    raise SkinError(f"{mod_dir}: no romfs folder with Pack/Actor in it")


class Skin:
    """One model mod, read: `members[variant]` the members of its player pack, `table` its own size
    table (or None), `files` its romfs files by path (with .zs)."""

    def __init__(self, skin_id, romfs, members, table, files):
        self.id = skin_id
        self.romfs = romfs
        self.members = members
        self.table = table
        self.files = files

    @property
    def variants(self):
        return [v for v in VARIANTS if v in self.members]


def _walk(romfs):
    """The mod's files (romfs paths). Symlinks are skipped, files and folders: a mod is plain files, and a
    link could point anywhere on the computer."""
    out = []
    for root, dirs, names in os.walk(romfs):
        dirs[:] = sorted(d for d in dirs if not os.path.islink(os.path.join(root, d)))
        for n in sorted(names):
            p = os.path.join(root, n)
            if os.path.islink(p):
                continue
            out.append(os.path.relpath(p, romfs).replace(os.sep, "/"))
    return out


def pick_table(tables, game_table=None):
    """Of a mod's size tables (a mod made for several game versions ships one per version), the one
    named like the game's (`game_table`, its romfs path); else the one of the newest version (the
    highest Product number), else None."""
    if game_table in tables:
        return game_table

    def version(path):
        m = re.search(r"\.Product\.(\d+)\.", path)
        return int(m.group(1)) if m else -1
    return max(tables, key=lambda t: (version(t), t)) if tables else None


def load(skin_id, mod_dir, dicts, read, game_table=None):
    """Reads a model mod. `read(path) -> bytes`; `game_table`: the romfs path of the game's own size
    table (pick_table). Raises SkinError for anything that is not one."""
    if not ID_RE.match(skin_id or ""):
        raise SkinError(f"skin id {skin_id!r}: 1 to 20 letters or digits")
    for d in CODE_DIRS:
        if os.path.isdir(os.path.join(mod_dir, d)):
            raise SkinError(f"{mod_dir} has a {d} folder: code and cheat mods can't be skins")
    romfs = find_romfs(mod_dir)
    members = {}
    for v in VARIANTS:
        p = os.path.join(romfs, "Pack", "Actor", f"Player_{v}.pack.zs")
        if os.path.isfile(p):
            try:
                members[v] = sarc.read(dicts.decompress(read(p)))
            except Exception as e:  # noqa: BLE001 - any broken file is the mod's, say which
                raise SkinError(f"{p}: not a pack the game reads ({e})")
    if not members:
        raise SkinError(f"{mod_dir} does not change the player (no Pack/Actor/Player_Adult or Player_Child)")
    table = None
    files = _walk(romfs)
    name = pick_table([f for f in files if f.startswith("System/Resource/") and ".rsizetable" in f], game_table)
    if name:
        try:
            table = rstb.Rstb.loads(dicts.decompress(read(os.path.join(romfs, name))))
        except Exception as e:  # noqa: BLE001 - any broken file is the mod's, say which
            raise SkinError(f"{name}: not a size table the game reads ({e})")
    return Skin(skin_id, romfs, members, table, files)


def model_info(members, components):
    """(FmdbName, ModelProjectName) the components' ModelInfoRef names, or (None, None)."""
    from .build import ref_to_member  # (build imports this module)
    path = ref_to_member(components.get("ModelInfoRef"))
    if path not in members:
        return None, None
    doc = byml.loads(members[path])
    return doc.get("FmdbName"), doc.get("ModelProjectName")


def animation_projects(members, components):
    """The animation model projects the components' AnimationRef lists (retargeted ones too)."""
    from .build import ref_to_member
    path = ref_to_member(components.get("AnimationRef"))
    if path not in members:
        return []
    out = []
    for r in byml.loads(members[path]).get("AnimationResources", []) or []:
        name = r.get("ModelProjectName") if isinstance(r, dict) else None
        if isinstance(name, str) and name not in out:
            out.append(name)
    return out


def check_changes_model(skin, game_members, flatten):
    """The variants whose model the mod changes; SkinError when it changes none."""
    changed = []
    for v in skin.variants:
        mine = model_info(skin.members[v], flatten(skin.members[v], f"Player_{v}"))
        game = model_info(game_members[v], flatten(game_members[v], f"Player_{v}")) if v in game_members else (None, None)
        if mine[1] and mine != game:
            changed.append(v)
    if not changed:
        raise SkinError(f"the mod {skin.id} does not change the player's model")
    return changed


def new_files(skin, game_table, ours=(), game_has=None):
    """The mod's files a skin takes: every file the game neither lists in its size table nor has in
    its romfs (`game_has(path)`; the mod's own new files), outside SKIP_TOP. Files the game has are
    the mod's in-place replacements: left out, so they reach no one else's Link. `ours`: paths an
    earlier skin of this pack wrote (in the table now, but not the game's)."""
    out = []
    for f in skin.files:
        top = f.split("/", 1)[0]
        if top in SKIP_TOP or "/" not in f:
            continue
        if f not in ours and (game_table.get(strip_zs(f)) is not None or (game_has is not None and game_has(f))):
            continue
        out.append(f)
    return out


def entry_size(skin, path, data_len, game_table=None):
    """Size-table entry for a file of the skin: the mod's own entry, else the game's for the same
    path, else the file's (unpacked) length plus a margin."""
    for t in (skin.table, game_table):
        if t is not None:
            s = t.get(strip_zs(path))
            if s is not None:
                return s
    return data_len + MARGIN


def content_hash(items):
    """SHA-256 over (path, bytes) pairs, paths in order: the skin's identity on the wire."""
    h = hashlib.sha256()
    for path, data in sorted(items, key=lambda x: x[0].encode("utf-8")):
        h.update(path.encode("utf-8") + b"\0")
        h.update(hashlib.sha256(data).digest())
    return h.hexdigest()


# Unique names: two versions of one mod (an outfit variant) name their model project the same and differ
# in its files. The later skin then gets its project under a name of its own, `<project>_<id>`, and every
# ModelProjectName naming the old one in its packs is rewritten, so both skins fit in one pack.
def project_of(path):
    """The model project of a `Model/<project>.<rest>` file, else None."""
    if not path.startswith("Model/"):
        return None
    name = path[len("Model/"):]
    return name.split(".", 1)[0] if "." in name else None


def unique_project(project, skin_id):
    return f"{project}_{skin_id}"


def renamed_path(path, renames):
    """`path` with its model project renamed (`renames`: {old project: new}), else as it is."""
    proj = project_of(path)
    if proj in renames:
        return "Model/" + renames[proj] + path[len("Model/") + len(proj):]
    return path


def _rename_doc(doc, renames):
    changed = False
    if isinstance(doc, dict):
        for k, v in list(doc.items()):
            if k == "ModelProjectName" and isinstance(v, str) and v in renames:
                doc[k] = renames[v]
                changed = True
            elif isinstance(v, (dict, list)):
                changed |= _rename_doc(v, renames)
    elif isinstance(doc, list):
        for v in doc:
            changed |= _rename_doc(v, renames)
    return changed


def rename_projects(members, renames):
    """Pack members with every ModelProjectName in `renames` rewritten (only the byml members that
    name one); the others are left byte for byte."""
    if not renames:
        return members
    out = dict(members)
    keys = [o.encode("utf-8") for o in renames]
    for path, data in members.items():
        if not path.endswith((".bgyml", ".byml")) or not any(k in data for k in keys):
            continue
        try:
            doc = byml.loads(data)
        except Exception:  # noqa: BLE001 - not a byml we read: left as it is
            continue
        if _rename_doc(doc, renames):
            out[path] = byml.dumps(doc)
    return out


# Gear the mod changes (docs/puppet.md "Custom models"): an item whose pack the mod ships with a
# ModelInfo naming a model the mod itself brings (for example a Goron tunic made for its body). The
# skin gets its own copy `<item>_Skin_<id>`, as the gear colours get `<item>_Look<k>`, so the game's
# item, worn by every other Link, stays the game's.
GEAR_PREFIXES = ("Weapon_", "Armor_", "Item_")


def gear_name(item, skin_id):
    return f"{item}_Skin_{skin_id}"


def gear_items(skin, dicts, read):
    """[(item, members, FmdbName, ModelProjectName, ModelInfo member path)] for each gear item the
    mod gives a model of its own."""
    from .build import actor_param_path, ref_to_member
    out = []
    folder = os.path.join(skin.romfs, "Pack", "Actor")
    have = set(skin.files)
    for name in sorted(os.listdir(folder)):
        if not name.endswith(".pack.zs") or not name.startswith(GEAR_PREFIXES):
            continue
        item = name[:-len(".pack.zs")]
        try:
            members = sarc.read(dicts.decompress(read(os.path.join(folder, name))))
            doc = byml.loads(members[actor_param_path(item)])
        except Exception:  # noqa: BLE001 - not an actor pack we can read: not gear of this skin
            continue
        info_path = ref_to_member(doc.get("Components", {}).get("ModelInfoRef"))
        if info_path not in members:
            continue
        info = byml.loads(members[info_path])
        fmdb, project = info.get("FmdbName"), info.get("ModelProjectName")
        if not isinstance(fmdb, str) or not isinstance(project, str):
            continue
        if f"Model/{project}.{fmdb}.bfres.zs" in have:
            out.append((item, members, fmdb, project, info_path))
    return out


def gear_pack(members, item, skin_id, fmdb, project):
    """(pack bytes, actor param bytes, model info bytes) of the skin's copy of a gear item: the mod's
    pack, its ActorParam under the copy's name naming a ModelInfo of the copy's own path."""
    from .build import actor_param_path
    actor = gear_name(item, skin_id)
    info_path = f"Component/ModelInfo/{actor}.engine__component__ModelInfo.bgyml"
    doc = byml.loads(members[actor_param_path(item)])
    doc["Components"]["ModelInfoRef"] = "?" + info_path
    info = byml.dumps({"FmdbName": fmdb, "ModelProjectName": project})
    ap = byml.dumps(doc)
    files = {k: v for k, v in members.items() if k != actor_param_path(item)}
    files[actor_param_path(actor)] = ap
    files[info_path] = info
    return sarc.write(files), ap, info


# Your own Link in a skin: the kit puts these files over the game's when you wear it (as coop_self for the
# tunic colour), beside the pack, not in romfs/, so the game does not load them for everyone.
SELF_DIR = "coop_skin_self"


def self_pack(members, variant, as_ref):
    """Your own Link's `Player_<variant>` pack in the skin: the mod's pack, with the ASRef of each
    ActorParam in its chain that has one moved to the skin's own AS copy (`as_ref`, skin.as_path), so
    the game's AS root, which every puppet reads, is never replaced. Without `as_ref` (the mod keeps
    the game's AS) the mod's pack as it is."""
    from .build import actor_param_chain
    files = dict(members)
    if as_ref:
        for path, doc in actor_param_chain(members, f"Player_{variant}"):
            comps = doc.get("Components") if isinstance(doc, dict) else None
            if isinstance(comps, dict) and "ASRef" in comps:
                comps["ASRef"] = "?" + as_ref
                files[path] = byml.dumps(doc)
    return sarc.write(files)
