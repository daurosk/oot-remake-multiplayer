"""Builds the co-op puppet actor packs from the user's own romfs.

    python -m puppet_pack.build --romfs <extracted romfs> --mod-dir <mod folder>
    python -m puppet_pack.build --romfs <extracted romfs> --user-dir <emulator user folder>

Writes, under <mod folder>/romfs:
  Pack/Actor/CoopPuppet_Child.pack.zs, Pack/Actor/CoopPuppet_Adult.pack.zs
  RSDB/ActorInfo.Product.<v>.rstbl.byml.zs        (game table plus two rows)
  RSDB/GameActorInfo.Product.<v>.rstbl.byml.zs    (game table plus two rows)
  System/Resource/ResourceSizeTable.Product.<v>.<platform>.rsizetable.zs (plus new entries)
and <mod folder>/coop_puppet.json describing what it made.

The puppets keep Link's sound components (docs/puppet.md, Sound); --no-sound leaves them out.

Everything is made from the files in --romfs; nothing from the game is in this tool.
The input is never changed, and the tables are always rebuilt from the game's own
copies, so running the tool again gives the same result.
"""
import argparse
import glob
import hashlib
import json
import os
import shutil
import sys
import tempfile

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml, dumpcheck, nup, rstb, sarc, zs  # noqa: E402

TITLE_ID = "0400486024DEC000"
DEFAULT_MOD_NAME = "coop-puppet"
VARIANTS = ("Child", "Adult")
PACK_DICT = "pack.zsdic"

# Components taken from the player's own ActorParam: what it takes to draw Link, play
# his AS commands and wear his equipment. EquipmentUserRef brings the player's
# EquipmentUserParam chain, whose BlackboardTableRef names the EquipmentUser_* slots
# (as Player_Event has it, without the player's BlackboardRef). Everything else of the
# player (input, camera, physics, life, the `Player` actor name) is left out.
KEEP_COMPONENTS = (
    "ASInfoRef",
    "ASOptimize",
    "ASRef",
    "AnimationRef",
    "BoneBindRef",
    "EquipmentUserRef",
    "LODRef",
    "LocalDepthShadowRef",
    "ModelBindRef",
    "ModelInfoRef",
)

# Sound (issue 131): the player's AS node events (AnimationEvent/AsNode/Player_*.root.baev:
# `AtSound`, `Voice`, `ChangeSeFloorContact`) fire on the actor's SLink user, and the
# footstep component plays the steps from the feet bones. The puppet runs the same AS, so
# with these and the ActorInfo `SLinkUserName` its own animation plays Link's sounds at the
# puppet. `--no-sound` leaves them out (the pack as before).
SOUND_COMPONENTS = ("SLinkRef", "SoundFootStepRef")

# The player's footstep param has `ProcType` Player (murmur3 hash 145979025): the steps are
# worked out by the player's own code, which the puppet does not run, and `IsUseRayCast` casts
# from the leg bones for the floor. Dark Link (Enemy_MiddleBoss_DarkLink), the game's own copy of
# Link, takes the player's footstep param as its $parent and changes exactly these three:
# ProcType None (murmur3 3069107721), no ray cast, no grass. The puppet gets its own file with the
# same changes, $parent the player's variant file (issue 131, puppet-sound-2). `--player-footsteps`
# keeps the player's file (the pack of test7 and test8).
FOOTSTEP_PROC_NONE = 3069107721
FOOTSTEP_OVERRIDES = {"IsEnableGrass": False, "IsUseRayCast": False, "ProcType": byml.UInt(FOOTSTEP_PROC_NONE)}
FOOTSTEP_DIR = "Component/SoundFootStepParam/"
# `--sound-user NAME` (test switch): the puppet's sounds go to another actor's SLink user, through
# its own SLink param and its ActorInfo row, e.g. Enemy_MiddleBoss_DarkLink if the Player_* users
# turn out to play only as the listener's own sounds. NAME must be some row's SLinkUserName.
SLINK_DIR = "Component/SLink/"
SLINK_SUFFIX = ".engine__component__SLinkParam.bgyml"
FOOTSTEP_SUFFIX = ".game__component__SoundFootStepParam.bgyml"

# The puppet is no use without these: no AS, no model, nowhere to put a sword.
REQUIRED_COMPONENTS = ("ASRef", "ModelInfoRef", "EquipmentUserRef")
# Blackboard keys of the EquipmentUser: the instance parameters a create request can set.
EQUIP_KEY_PREFIX = "EquipmentUser_"

# Components set to the engine's shared defaults, the ones the simple event actors use.
# These files are not in any actor pack; the engine holds them.
FIXED_COMPONENTS = {
    "ActorLogicNodeRef": "Work/Logic/NodeDefinition/Node/ActorLogicSimpleActor.aidefn",
    "PreActorSettingRef": "?ActorSystem/PreActorSetting/ActorOnly.game__actor__PreActorSetting.bgyml",
    "SystemSetting": "?ActorSystem/ActorSystemSetting/GameActorDefault.engine__actor__ActorSystemSetting.bgyml",
    "WorldInfoRef": "?Component/WorldInfoParam/Default.game__component__WorldInfoParam.bgyml",
    "XLinkRef": "?Component/XLink/ActorBasic.engine__component__XLinkParam.bgyml",
}

# ActorInfo fields of the player row that belong to the player only. SLinkUserName names the
# sound user and is kept unless the pack is built without sound.
DROP_INFO_FIELDS = ("ActorName", "ELinkUserName")
SOUND_INFO_FIELDS = ("SLinkUserName",)
# Always calculated while loaded: the puppet is wherever its player is.
INFO_OVERRIDES = {"CalcRadius": 100000.0, "CoreUsedLevel": byml.UInt(0)}
# The puppet's ActorParam is the player's merged with its $parent, Player_Base, in one file. The
# game parses an ActorParam in a heap of its size-table entry, and the player's entry is sized for
# its own file only: Player_Adult's ActorParam is a 776-byte stub (entry 6,232) and Player_Base's
# has its own entry (13,336). With the player's entry alone the adult puppet's 1,508-byte file ran
# out of heap: the 0x98-byte engine__actor__ActorParam object (factory main+0x9a9300) got a null
# pointer and every create was dropped (issue 111). The puppet's entry is the sum of the entries of
# every file in the $parent chain that flatten_actor_param merges (actor_param_chain).

# Room left in a size-table entry above the bytes we wrote.
PACK_SIZE_MARGIN = 0x1000

# Stand-ins for other players' projectiles (issue 85). The copy of a slingshot seed, an arrow or a
# hookshot tip used to be the game's own shootable, made without a shooter: it starts in its
# `UsedExternally` state, and the seed's ShootableParam hides it until it is shot
# (`IsPrepareVisibleOff`), so in the test4 player logs every seed copy was made and none was seen.
# A stand-in is the shootable's model only, with the shared default components the puppet uses:
# no physics, sensor, attack, AI, effects or sound. It is drawn wherever the hook puts it and can
# hit nothing (the hook's CoopSpawn::StandIn has the same names). A romfs without the source pack
# (the tests' made-up one) gets no stand-in.
STAND_INS = (
    ("Shootable_DekuSeed", "CoopShot_Seed"),
    ("NormalArrow", "CoopShot_Arrow"),
    ("Shootable_Hook", "CoopShot_Hook"),
    ("Shootable_HookLong", "CoopShot_HookLong"),
)
STAND_IN_COMPONENTS = ("ModelInfoRef", "LODRef")
# ActorInfo fields of the shootable that a model-only actor must not take over.
STAND_IN_DROP_INFO = ("ActorName", "ELinkUserName", "SLinkUserName")


class BuildError(RuntimeError):
    pass


def puppet_name(variant):
    return f"CoopPuppet_{variant}"


def actor_param_path(actor):
    return f"Actor/{actor}.engine__actor__ActorParam.bgyml"


def ref_to_member(ref):
    """`?Component/X.bgyml` or `Work/Component/X.gyml` -> the member path inside a pack."""
    if not isinstance(ref, str):
        return None
    p = ref[1:] if ref.startswith("?") else ref
    if p.startswith("Work/"):
        p = p[5:]
        if p.endswith(".gyml"):
            p = p[:-5] + ".bgyml"
    return p


def _strings(v):
    if isinstance(v, dict):
        for x in v.values():
            yield from _strings(x)
    elif isinstance(v, list):
        for x in v:
            yield from _strings(x)
    elif isinstance(v, str):
        yield v


def actor_param_chain(members, actor):
    """The actor's ActorParam and its `$parent` chain: [(member path, document)], the actor first."""
    chain = []
    path = actor_param_path(actor)
    seen = set()
    while path:
        if path in seen:
            raise BuildError(f"$parent loop at {path}")
        seen.add(path)
        if path not in members:
            raise BuildError(f"{path} is not in the pack")
        doc = byml.loads(members[path])
        chain.append((path, doc))
        path = ref_to_member(doc.get("$parent")) if isinstance(doc, dict) else None
    return chain


def flatten_actor_param(members, actor):
    """The actor's ActorParam with its `$parent` chain merged in (child wins)."""
    chain = [doc for _, doc in actor_param_chain(members, actor)]
    comps = {}
    for doc in reversed(chain):
        comps.update(doc.get("Components", {}))
    return comps


def make_actor_param(player_components, sound=True):
    missing = [k for k in REQUIRED_COMPONENTS if k not in player_components]
    if missing:
        raise BuildError(f"the player's ActorParam has no {', '.join(missing)}")
    keep = KEEP_COMPONENTS + (SOUND_COMPONENTS if sound else ())
    comps = {k: player_components[k] for k in keep if k in player_components}
    comps.update(FIXED_COMPONENTS)
    return {"Components": comps}


def collect_members(members, root_doc):
    """Every pack member that root_doc refers to, directly or through other members."""
    out = {}
    todo = [s for s in _strings(root_doc)]
    while todo:
        p = ref_to_member(todo.pop())
        if p in out or p not in members:
            continue
        out[p] = members[p]
        if p.endswith(".bgyml"):
            try:
                todo.extend(_strings(byml.loads(members[p])))
            except byml.BymlError:
                pass
    return out


def blackboard_table(files, path):
    """A BlackboardParamTable with its $parent chain merged: {array name: {key: entry}}."""
    chain, seen = [], set()
    while path and path in files and path not in seen:
        seen.add(path)
        doc = byml.loads(files[path])
        chain.append(doc)
        path = ref_to_member(doc.get("$parent"))
    out = {}
    for doc in reversed(chain):
        for name, arr in doc.items():
            if isinstance(arr, list):
                for e in arr:
                    if isinstance(e, dict) and "BBKey" in e:
                        out.setdefault(name, {})[e["BBKey"]] = e
    return out


def equipment_keys(files, actor_param):
    """The EquipmentUser_* instance keys the puppet's EquipmentUser blackboard table has.

    Follows EquipmentUserRef and its $parent chain to the first BlackboardTableRef.
    Raises BuildError when there is none: the puppet could not wear anything.
    """
    path = ref_to_member(actor_param["Components"].get("EquipmentUserRef"))
    seen = set()
    table = None
    while path and path not in seen:
        seen.add(path)
        if path not in files:
            raise BuildError(f"{path} is not in the pack")
        doc = byml.loads(files[path])
        table = ref_to_member(doc.get("BlackboardTableRef"))
        if table:
            break
        path = ref_to_member(doc.get("$parent"))
    if not table or table not in files:
        raise BuildError("the EquipmentUserParam names no blackboard table in the pack")
    keys = []
    for arr in blackboard_table(files, table).values():
        keys += [k for k, e in arr.items() if k.startswith(EQUIP_KEY_PREFIX) and e.get("IsInstanceParam")]
    if not keys:
        raise BuildError(f"{table} has no {EQUIP_KEY_PREFIX}* instance keys")
    return sorted(keys)


def footstep_path(actor):
    return f"{FOOTSTEP_DIR}{actor}{FOOTSTEP_SUFFIX}"


def puppet_footstep(doc, actor):
    """The puppet's own footstep param, Dark Link's changes over the player's file (see
    FOOTSTEP_OVERRIDES): (member path, bytes, the player's member path it replaces), or None when
    the ActorParam has no footstep component."""
    src = ref_to_member(doc["Components"].get("SoundFootStepRef"))
    if not src:
        return None
    if not src.startswith(FOOTSTEP_DIR) or not src.endswith(FOOTSTEP_SUFFIX):
        raise BuildError(f"unexpected footstep param {src}")
    own = {"$parent": "Work/" + src[:-len(".bgyml")] + ".gyml"}
    own.update(FOOTSTEP_OVERRIDES)
    return footstep_path(actor), byml.dumps(own), src


def slink_path(actor):
    return f"{SLINK_DIR}{actor}{SLINK_SUFFIX}"


def puppet_slink(doc, actor, user):
    """The puppet's own SLink param naming `user`: (member path, bytes, the player's member path)."""
    src = ref_to_member(doc["Components"].get("SLinkRef"))
    if not src:
        raise BuildError("the player's ActorParam has no SLinkRef")
    return slink_path(actor), byml.dumps({"UserName": user}), src


def build_pack(members, variant, sound=True, player_footsteps=False, sound_user=None):
    """Returns (pack bytes, actor param bytes, member list, equipment keys) for one variant."""
    player = f"Player_{variant}"
    doc = make_actor_param(flatten_actor_param(members, player), sound)
    files = collect_members(members, doc)
    keys = equipment_keys(files, doc)
    fs = puppet_footstep(doc, puppet_name(variant)) if sound and not player_footsteps else None
    if fs:
        path, data, _ = fs
        files[path] = data  # its $parent, the player's file and that one's parent, stay in the pack
        doc["Components"]["SoundFootStepRef"] = "?" + path
    if sound and sound_user:
        path, data, _ = puppet_slink(doc, puppet_name(variant), sound_user)
        files[path] = data
        doc["Components"]["SLinkRef"] = "?" + path
    ap = byml.dumps(doc)
    files[actor_param_path(puppet_name(variant))] = ap
    return sarc.write(files), ap, sorted(files), keys


def make_stand_in_param(components):
    """A stand-in's ActorParam: the shootable's model (and LOD) with the shared defaults."""
    if "ModelInfoRef" not in components:
        raise BuildError("the shootable's ActorParam has no ModelInfoRef")
    comps = {k: components[k] for k in STAND_IN_COMPONENTS if k in components}
    comps.update(FIXED_COMPONENTS)
    return {"Components": comps}


def build_stand_in(members, source, name):
    """Returns (pack bytes, actor param bytes, member list) for one stand-in."""
    doc = make_stand_in_param(flatten_actor_param(members, source))
    files = collect_members(members, doc)
    ap = byml.dumps(doc)
    files[actor_param_path(name)] = ap
    return sarc.write(files), ap, sorted(files)


def stand_in_row(source_row, name):
    """The stand-in's ActorInfo row: the shootable's, less its effect, sound and name fields, with
    the puppet's INFO_OVERRIDES."""
    row = {k: v for k, v in source_row.items() if k not in STAND_IN_DROP_INFO}
    row.update(INFO_OVERRIDES)  # calculated wherever it is, as the puppet (a far shot)
    row["__RowId"] = name
    return row


def _num(v):
    return v.v if isinstance(v, byml._Num) else v


def info_row(player_row, actor, heap=None, sound=True, sound_user=None):
    """The puppet's ActorInfo row: the player's, less its player-only fields. `heap`, when given,
    sets InstanceHeapSize (a test switch, `--heap`), keeping the field's BYML type."""
    drop = DROP_INFO_FIELDS + (() if sound else SOUND_INFO_FIELDS)
    row = {k: v for k, v in player_row.items() if k not in drop}
    row.update(INFO_OVERRIDES)
    if sound and sound_user:
        row["SLinkUserName"] = sound_user
    if heap is not None:
        old = row.get("InstanceHeapSize")
        row["InstanceHeapSize"] = type(old)(int(heap)) if isinstance(old, byml._Num) else int(heap)
    row["__RowId"] = actor
    return row


def upsert_rows(rows, new_rows):
    """Puts rows into a table sorted by __RowId (bytes), replacing rows with the same id."""
    by_id = {r["__RowId"]: r for r in rows}
    for r in new_rows:
        by_id[r["__RowId"]] = r
    return [by_id[k] for k in sorted(by_id, key=lambda s: s.encode("utf-8"))]


def tool_hash(folder=None):
    """One SHA-256 for the tool's own *.py files (this folder, not tests/), so the kit can tell a
    pack made by another version of the tool (play.ps1, Get-PuppetPackHint, computes the same):
    sha256 over "<name>:<sha256 of the file's bytes>\\n" for each file, names in ordinal order."""
    folder = folder or os.path.dirname(os.path.abspath(__file__))
    names = sorted((n for n in os.listdir(folder) if n.lower().endswith(".py")
                    and os.path.isfile(os.path.join(folder, n))), key=lambda n: n.encode("utf-8"))
    text = "".join(f"{n}:{hashlib.sha256(read(os.path.join(folder, n))).hexdigest()}\n" for n in names)
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def find_one(romfs, pattern):
    hits = sorted(glob.glob(os.path.join(romfs, pattern)))
    if len(hits) != 1:
        raise BuildError(f"expected one file for {pattern} in {romfs}, found {len(hits)}")
    return hits[0]


def rel(romfs, path):
    return os.path.relpath(path, romfs).replace("\\", "/")


def strip_zs(path):
    return path[:-3] if path.endswith(".zs") else path


def read(path):
    with open(path, "rb") as f:
        return f.read()


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def recompress(dicts, original_raw, data):
    """Compresses like the game's own copy of the file: same dictionary, or none."""
    did = zs.frame_dict_id(original_raw)
    name = None
    if did:
        name = next((n for n, d in dicts.by_name.items() if d.dict_id() == did), None)
        if name is None:
            raise BuildError(f"no dictionary with id {did}")
    return dicts.compress(data, name)


def build(romfs, mod_dir, log=print, heaps=None, sound=True, player_footsteps=False, sound_user=None):
    """`heaps`: {actor name: InstanceHeapSize} set outright, for tests of the heap (issue 111).
    `sound`: False builds the puppets without Link's sounds (issue 131). `player_footsteps`: keep
    the player's footstep param instead of the puppet's own (FOOTSTEP_OVERRIDES). `sound_user`: another
    SLink user for the puppets' sounds (SLINK_DIR)."""
    heaps = heaps or {}
    if not os.path.isfile(os.path.join(romfs, "Pack", "ZsDic.pack.zs")):
        raise BuildError(f"{romfs} does not look like the game's romfs (no Pack/ZsDic.pack.zs)")
    dicts = zs.Dicts.from_romfs(romfs)
    if PACK_DICT not in dicts.by_name:
        raise BuildError(f"Pack/ZsDic.pack.zs has no {PACK_DICT}")
    out_romfs = os.path.join(mod_dir, "romfs")

    ai_path = find_one(romfs, "RSDB/ActorInfo.Product.*.rstbl.byml.zs")
    gai_path = find_one(romfs, "RSDB/GameActorInfo.Product.*.rstbl.byml.zs")
    rs_path = find_one(romfs, "System/Resource/ResourceSizeTable.Product.*.rsizetable.zs")
    ai_raw, gai_raw, rs_raw = read(ai_path), read(gai_path), read(rs_path)
    ai_old = dicts.decompress(ai_raw)
    gai_old = dicts.decompress(gai_raw)
    ai = byml.loads(ai_old)
    gai = byml.loads(gai_old)
    table = rstb.Rstb.loads(dicts.decompress(rs_raw))
    ai_rows = {r["__RowId"]: r for r in ai}
    if sound and sound_user and not any(r.get("SLinkUserName") == sound_user for r in ai):
        raise BuildError(f"no actor has the SLink user {sound_user}")
    made = []
    new_ai, new_gai = [], []
    for v in VARIANTS:
        player = f"Player_{v}"
        actor = puppet_name(v)
        src = os.path.join(romfs, "Pack", "Actor", f"{player}.pack.zs")
        if not os.path.isfile(src):
            raise BuildError(f"missing {rel(romfs, src)}")
        members = sarc.read(dicts.decompress(read(src)))
        pack, ap, names, keys = build_pack(members, v, sound, player_footsteps, sound_user)
        dst_rel = f"Pack/Actor/{actor}.pack.zs"
        write(os.path.join(out_romfs, dst_rel), dicts.compress(pack, PACK_DICT))
        if player not in ai_rows:
            raise BuildError(f"ActorInfo has no row {player}")
        new_ai.append(info_row(ai_rows[player], actor, heaps.get(actor), sound, sound_user))
        new_gai.append({"__RowId": actor})

        pack_size = ((len(pack) + 0x1F) & ~0x1F) + PACK_SIZE_MARGIN
        table.add_new(strip_zs(dst_rel), pack_size)
        chain_ap = 0
        for path, _ in actor_param_chain(members, player):
            size = table.get(path)
            if size is None:
                raise BuildError(f"the size table has no {path}")
            chain_ap += size
        ap_size = max(chain_ap, len(ap) + PACK_SIZE_MARGIN)
        table.add_new(actor_param_path(actor), ap_size)
        footstep = None
        if footstep_path(actor) in names:
            path, data, src = puppet_footstep(make_actor_param(flatten_actor_param(members, player)), actor)
            # Sized as the player's file it stands in for (the game's entries run about 5.3 KB over
            # the file), plus any bytes it is longer.
            src_size = table.get(src)
            fs_size = max((src_size or 0) + max(0, len(data) - len(members[src])), len(data) + PACK_SIZE_MARGIN)
            table.add_new(path, fs_size)
            footstep = {"param": path, "size": fs_size}
        if slink_path(actor) in names:
            path, data, src = puppet_slink(make_actor_param(flatten_actor_param(members, player)), actor, sound_user)
            table.add_new(path, max((table.get(src) or 0) + max(0, len(data) - len(members[src])),
                                    len(data) + PACK_SIZE_MARGIN))
        made.append({"actor": actor, "from": player, "pack": dst_rel, "pack_bytes": len(pack),
                     "instance_heap": _num(new_ai[-1].get("InstanceHeapSize", 0)), "param_size": ap_size,
                     "members": names, "equipment_keys": keys, "footstep": footstep})
        log(f"{actor}: {len(names)} files, {len(pack)} bytes, {len(keys)} equipment keys -> {dst_rel}")

    shots = []
    for source, name in STAND_INS:
        src = os.path.join(romfs, "Pack", "Actor", f"{source}.pack.zs")
        if not os.path.isfile(src) or source not in ai_rows:
            log(f"{name}: no {source} in this romfs, left out")
            continue
        members = sarc.read(dicts.decompress(read(src)))
        pack, ap, names = build_stand_in(members, source, name)
        dst_rel = f"Pack/Actor/{name}.pack.zs"
        write(os.path.join(out_romfs, dst_rel), dicts.compress(pack, PACK_DICT))
        new_ai.append(stand_in_row(ai_rows[source], name))
        new_gai.append({"__RowId": name})
        table.add_new(strip_zs(dst_rel), ((len(pack) + 0x1F) & ~0x1F) + PACK_SIZE_MARGIN)
        chain_ap = sum(table.get(path) or 0 for path, _ in actor_param_chain(members, source))
        ap_size = max(chain_ap, len(ap) + PACK_SIZE_MARGIN)
        table.add_new(actor_param_path(name), ap_size)
        shots.append({"actor": name, "from": source, "pack": dst_rel, "pack_bytes": len(pack),
                      "param_size": ap_size, "members": names})
        log(f"{name}: {len(names)} files, {len(pack)} bytes -> {dst_rel}")

    ai_new = byml.dumps(upsert_rows(ai, new_ai))
    gai_new = byml.dumps(upsert_rows(gai, new_gai))
    for path, old, new in ((ai_path, ai_old, ai_new), (gai_path, gai_old, gai_new)):
        key = strip_zs(rel(romfs, path))
        size = table.get(key)
        if size is None:
            raise BuildError(f"the size table has no {key}")
        table.set(key, size + max(0, len(new) - len(old)) + PACK_SIZE_MARGIN)
    for path, new in ((ai_path, ai_new), (gai_path, gai_new), (rs_path, table.dumps())):
        r = rel(romfs, path)
        write(os.path.join(out_romfs, r), recompress(dicts, read(path), new))
        log(f"wrote {r}")

    manifest = {
        "tool": "tools/puppet_pack",
        "title_id": TITLE_ID,
        "actors": made,
        "shots": shots,
        "sound": sound,
        "footsteps": "player" if player_footsteps or not sound else "puppet",
        "sound_user": sound_user if sound and sound_user else None,
        "tables": [rel(romfs, p) for p in (ai_path, gai_path, rs_path)],
        "source_sha256": {rel(romfs, p): hashlib.sha256(read(p)).hexdigest()
                          for p in (ai_path, gai_path, rs_path)},
    }
    manifest["tool_sha256"] = tool_hash()  # the kit compares it with its own tool (Get-PuppetPackHint)
    write(os.path.join(mod_dir, "coop_puppet.json"), json.dumps(manifest, indent=1).encode())
    return manifest


def load_root(user_dir):
    """The mod load folder the emulator really reads for this user folder.

    The emulator keeps `load_directory` in user/config/qt-config.ini as an
    absolute path. A copied or moved emulator folder keeps the old path, so
    mods written to <user>/load would be ignored. Use the configured folder
    when it is set and exists; otherwise <user>/load.
    """
    default = os.path.join(user_dir, "load")
    ini = os.path.join(user_dir, "config", "qt-config.ini")
    try:
        with open(ini, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return default
    for line in lines:
        key, sep, value = line.partition("=")
        if sep and key.strip() == "load_directory" and value.strip():
            path = os.path.normpath(value.strip())
            if os.path.isdir(path):
                return path
    return default


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--romfs", required=True, help="the game's .nup file, or its extracted romfs folder")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--mod-dir", help="the mod folder to write (its romfs/ is made inside)")
    g.add_argument("--user-dir", help="the emulator's user folder; writes load/<title id>/<mod name>")
    ap.add_argument("--mod-name", default=DEFAULT_MOD_NAME)
    ap.add_argument("--any-version", action="store_true",
                    help=f"accept a dump that is not game data version {dumpcheck.EXPECTED_VERSION}")
    ap.add_argument("--heap", action="append", default=[], metavar="ACTOR=BYTES",
                    help="test switch: set a puppet's InstanceHeapSize, e.g. CoopPuppet_Adult=20000000")
    ap.add_argument("--no-sound", action="store_true",
                    help="build the puppets without Link's sounds (the pack before issue 131)")
    ap.add_argument("--player-footsteps", action="store_true",
                    help="test switch: the puppets keep the player's footstep param (the pack of test7 and test8)")
    ap.add_argument("--sound-user", metavar="NAME",
                    help="test switch: another actor's SLink user for the puppets' sounds, e.g. Enemy_MiddleBoss_DarkLink")
    a = ap.parse_args(argv)
    from_nup = os.path.isfile(a.romfs) and nup.is_nup(a.romfs)
    if from_nup:
        problems = dumpcheck.check_nup(a.romfs, a.any_version)
    else:
        a.romfs = dumpcheck.resolve(a.romfs)
        problems = dumpcheck.check_romfs(a.romfs, a.any_version)
    if problems:
        for p in problems:
            print(f"error: {p}", file=sys.stderr)
        return 1
    if from_nup:
        # Only the files the build reads, into a temporary RomFS folder.
        tmp = tempfile.mkdtemp(prefix="coop-puppet-")
        try:
            with nup.Nup(a.romfs) as n:
                for rel in dumpcheck.nup_files(n).values():
                    nup.write_under(tmp, rel, n.read(rel))
            print(f"read the game files from {a.romfs}")
            return _main_build(a, tmp)
        except nup.NupError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    return _main_build(a, a.romfs)


def _main_build(a, romfs):
    if a.mod_dir:
        mod_dir = a.mod_dir
    else:
        root = load_root(a.user_dir)
        if os.path.normcase(root) != os.path.normcase(os.path.join(a.user_dir, "load")):
            print(f"note: the emulator's settings load mods from {root}")
        mod_dir = os.path.join(root, TITLE_ID, a.mod_name)
    heaps = {}
    for h in a.heap:
        name, sep, size = h.partition("=")
        if not sep or name not in (puppet_name(v) for v in VARIANTS) or not size.isdigit():
            print(f"error: --heap {h}: want CoopPuppet_Child=BYTES or CoopPuppet_Adult=BYTES", file=sys.stderr)
            return 2
        heaps[name] = int(size)
    try:
        build(romfs, mod_dir, heaps=heaps, sound=not a.no_sound, player_footsteps=a.player_footsteps,
              sound_user=a.sound_user)
    except BuildError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(f"done: {mod_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
