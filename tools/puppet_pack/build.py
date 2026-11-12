"""Builds the co-op puppet actor packs from the user's own romfs.

    python -m puppet_pack.build --romfs <extracted romfs> --mod-dir <mod folder>
    python -m puppet_pack.build --romfs <extracted romfs> --user-dir <emulator user folder>

Writes, under <mod folder>/romfs:
  Pack/Actor/CoopPuppet_Child.pack.zs, Pack/Actor/CoopPuppet_Adult.pack.zs
  RSDB/ActorInfo.Product.<v>.rstbl.byml.zs        (game table plus two rows)
  RSDB/GameActorInfo.Product.<v>.rstbl.byml.zs    (game table plus two rows)
  System/Resource/ResourceSizeTable.Product.<v>.<platform>.rsizetable.zs (plus new entries)
and <mod folder>/coop_puppet.json describing what it made, and <mod folder>/coop_rooms.txt:
where each dungeon room's placements stand (dungeon_rooms.py; the hook guesses a player's room
from it, issue 145).

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

from . import ainb, byml, dumpcheck, dungeon_rooms, hud, nup, rstb, sarc, tunic, tunic_extra, zs  # noqa: E402

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
# keeps the player's file (the older pack).
FOOTSTEP_PROC_NONE = 3069107721
FOOTSTEP_OVERRIDES = {"IsEnableGrass": False, "IsUseRayCast": False, "ProcType": byml.UInt(FOOTSTEP_PROC_NONE)}
FOOTSTEP_DIR = "Component/SoundFootStepParam/"
# `--sound-user NAME` (test switch): the puppet's sounds go to another actor's SLink user, through
# its own SLink param and its ActorInfo row, e.g. Enemy_MiddleBoss_DarkLink if the Player_* users
# turn out to play only as the listener's own sounds. NAME must be some row's SLinkUserName.
SLINK_DIR = "Component/SLink/"
# The player's XLink param (issue 131, puppet-looks): its PropertyTableListRef names the Player,
# FootStep and HitReaction property tables (RSDB XLinkPropertyTableList), the properties Link's SLink
# user reads: which foot (FootStep's foot enum), the weapon kind, the slope and so on. The shared
# ActorBasic param of FIXED_COMPONENTS has only Common, ActorBasic and WettingRate, so the puppet's
# sound user found none of them. With sound the puppet keeps the player's XLinkRef; the property
# values stay at their defaults (the player's own code sets them). `--basic-xlink` (test switch)
# builds the older pack with ActorBasic.
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
# (`IsPrepareVisibleOff`), so in the player logs every seed copy was made and none was seen.
# A stand-in is the shootable's model only, with the shared default components the puppet uses:
# no physics, sensor, attack, AI, effects or sound. It is drawn wherever the hook puts it and can
# hit nothing (the hook's CoopSpawn::StandIn has the same names; CoopShot_Bomb is in HeldStandIn). A romfs without the source pack
# (the tests' made-up one) gets no stand-in.
STAND_INS = (
    ("Shootable_DekuSeed", "CoopShot_Seed"),
    ("NormalArrow", "CoopShot_Arrow"),
    ("Shootable_Hook", "CoopShot_Hook"),
    ("Shootable_HookLong", "CoopShot_HookLong"),
    # Issue 151: another player's cast lure, seen but never bitten by a fish here.
    ("Weapon_Lure", "CoopShot_Lure"),
    ("Weapon_SinkingLure", "CoopShot_SinkingLure"),
    # Issue 169: another player's bomb while it is in their hands. The game's own bomb copy kept
    # its physics body's fall between the hook's places and dropped out of the puppet's hands; the
    # stand-in has no body. It keeps the bomb's animations, so its fuse can blink.
    ("Item_Bomb", "CoopShot_Bomb"),
    # Issue 85: another player's thrown boomerang. The game's own one made here homed on this
    # game's enemies and hit them.
    ("Weapon_Boomerang", "CoopShot_Boomerang"),
    # Issue 199: another player's Epona under their puppet while they ride. The game's own horse
    # made here would run on her own AI; the stand-in is her model and animations only, placed
    # and played by the hook (lichii/hook/coop_ride.h).
    ("Animal_Epona_Adult", "CoopRide_Epona"),
)
STAND_IN_COMPONENTS = ("ModelInfoRef", "LODRef")
# Components some stand-ins keep beyond the model (by stand-in name): the animation set, so the
# hook can play the owner's animation on it (the bomb's Bomb_Blink; the hookshot tip's chain, whose
# length is its animation set's ChainExtend, issue 85).
STAND_IN_EXTRA_COMPONENTS = {
    "CoopShot_Bomb": ("ASRef", "ASInfoRef", "AnimationRef"),
    "CoopShot_Hook": ("ASRef", "ASInfoRef", "AnimationRef"),
    "CoopShot_HookLong": ("ASRef", "ASInfoRef", "AnimationRef"),
    "CoopRide_Epona": ("ASRef", "ASInfoRef", "AnimationRef"),
}
# ActorInfo fields of the shootable that a model-only actor must not take over.
STAND_IN_DROP_INFO = ("ActorName", "ELinkUserName", "SLinkUserName")
# Issue 198: stand-ins that also play one effect of the shootable's ELink user, through an AI root
# like the impact actors' (below), once when the copy is made. A seed copy is a few pixels for the
# ~0.1 s it flies; the shooter's own seed draws its `Tail` (a trail; key from the seed's ELink user
# table, shot-impacts). The copy is made as the seed is nocked, so the trail runs from then on and
# draws where the copy moves. (stand-in name: (ELink user, xlink key))
STAND_IN_EFFECTS = {
    "CoopShot_Seed": ("Shootable_DekuSeed", "Tail"),
}


# Issue 166: the spark and sound where another player's seed or arrow lands. In the shooter's game
# the shootable's own code plays them; a copy (a stand-in) runs none of it. An impact actor is made
# where the copy lands: no model, an ELink and/or SLink user, and an AI root that plays one xlink key
# once (ainb.oneshot_root, from the Business Scrub seed's AI). Users and keys read from the
# romfs's ELink2/SLink2 files (2026-11-11): the seed's ELink user has `Hit_AdditionalAttacker`
# (its SLink user only `Shoot`); the surface sounds are the shared SLink user `ReactionHit`'s
# `Hit_SlingShot_Bullet` and `Hit_Bow_Arrow` (each with lava, sand, water, bog variants by
# material); the arrow's ELink and SLink users both have `Hit_AdditionalAttacker`.
# (name, ELink user, SLink user, xlink key)
IMPACTS = (
    ("CoopHit_SeedSpark", "Shootable_DekuSeed", None, "Hit_AdditionalAttacker"),
    ("CoopHit_SeedSound", None, "ReactionHit", "Hit_SlingShot_Bullet"),
    ("CoopHit_Arrow", "NormalArrow", "NormalArrow", "Hit_AdditionalAttacker"),
    ("CoopHit_ArrowSound", None, "ReactionHit", "Hit_Bow_Arrow"),
)
# Where the AI template comes from, and the row the impacts' rows start from (AI, ELink and SLink).
IMPACT_SOURCE = "Shootable_ScrubBusinessSees"
IMPACT_TEMPLATE = "AI/Shootable_ScrubBusinessSees_PostUpdateMatrix.root.ainb"
ELINK_DIR = "Component/ELink/"
ELINK_SUFFIX = ".engine__component__ELinkParam.bgyml"
# Row fields of the source that are about its model or name: an impact has neither.
IMPACT_DROP_INFO = ("ActorName", "AnimationResources", "DisplayRadius", "FmdbName", "LoadRadius",
                    "ModelAabbMax", "ModelAabbMin", "ModelProjectName", "UsePreActorRenderer",
                    "ELinkUserName", "SLinkUserName")
IMPACT_COMPONENTS = {
    "LODRef": "?Component/LODParam/Default.engine__component__LODParam.bgyml",
}


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


def make_actor_param(player_components, sound=True, basic_xlink=False):
    missing = [k for k in REQUIRED_COMPONENTS if k not in player_components]
    if missing:
        raise BuildError(f"the player's ActorParam has no {', '.join(missing)}")
    keep = KEEP_COMPONENTS + (SOUND_COMPONENTS if sound else ())
    comps = {k: player_components[k] for k in keep if k in player_components}
    comps.update(FIXED_COMPONENTS)
    if sound and not basic_xlink and player_components.get("XLinkRef"):
        comps["XLinkRef"] = player_components["XLinkRef"]
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


def tunic_model_info(actor, variant, project):
    """(member path, bytes) of a colour puppet's ModelInfo: the player's model under its own
    project name, `Model/<project>.Player_<variant>.bfres` (puppet_pack.tunic)."""
    path = f"Component/ModelInfo/{actor}.engine__component__ModelInfo.bgyml"
    return path, byml.dumps({"FmdbName": f"Player_{variant}", "ModelProjectName": project})


def build_pack(members, variant, sound=True, player_footsteps=False, sound_user=None, actor=None,
               project=None, basic_xlink=False):
    """Returns (pack bytes, actor param bytes, member list, equipment keys) for one variant.
    `actor`: the pack's actor (default CoopPuppet_<variant>); `project`: a model project of its own
    (a tunic colour), else the player's."""
    player = f"Player_{variant}"
    actor = actor or puppet_name(variant)
    doc = make_actor_param(flatten_actor_param(members, player), sound, basic_xlink)
    files = collect_members(members, doc)
    if project:
        path, data = tunic_model_info(actor, variant, project)
        files[path] = data
        doc["Components"]["ModelInfoRef"] = "?" + path
    keys = equipment_keys(files, doc)
    fs = puppet_footstep(doc, actor) if sound and not player_footsteps else None
    if fs:
        path, data, _ = fs
        files[path] = data  # its $parent, the player's file and that one's parent, stay in the pack
        doc["Components"]["SoundFootStepRef"] = "?" + path
    if sound and sound_user:
        path, data, _ = puppet_slink(doc, actor, sound_user)
        files[path] = data
        doc["Components"]["SLinkRef"] = "?" + path
    ap = byml.dumps(doc)
    files[actor_param_path(actor)] = ap
    return sarc.write(files), ap, sorted(files), keys


def make_stand_in_param(components, extra=()):
    """A stand-in's ActorParam: the shootable's model (and LOD, and the `extra` components it
    has) with the shared defaults."""
    if "ModelInfoRef" not in components:
        raise BuildError("the shootable's ActorParam has no ModelInfoRef")
    comps = {k: components[k] for k in STAND_IN_COMPONENTS + tuple(extra) if k in components}
    comps.update(FIXED_COMPONENTS)
    return {"Components": comps}


def build_stand_in(members, source, name, template=None):
    """Returns (pack bytes, actor param bytes, member list) for one stand-in. With the impacts' AI
    `template`, a stand-in in STAND_IN_EFFECTS also gets its ELink user and an AI root that plays
    its effect key once."""
    doc = make_stand_in_param(flatten_actor_param(members, source),
                              STAND_IN_EXTRA_COMPONENTS.get(name, ()))
    files = collect_members(members, doc)
    effect = STAND_IN_EFFECTS.get(name) if template is not None else None
    if effect:
        extra, comps = effect_files(template, name, effect[0], None, effect[1])
        files.update(extra)
        doc["Components"].update(comps)
    ap = byml.dumps(doc)
    files[actor_param_path(name)] = ap
    return sarc.write(files), ap, sorted(files)


def impact_files(template, name, elink_user, slink_user, key):
    """{member path: bytes} of one impact actor's pack (its ActorParam last)."""
    comps = dict(FIXED_COMPONENTS)
    comps.update(IMPACT_COMPONENTS)
    files, effect_comps = effect_files(template, name, elink_user, slink_user, key)
    comps.update(effect_comps)
    files[actor_param_path(name)] = byml.dumps({"Components": comps})
    return files


def effect_files(template, name, elink_user, slink_user, key):
    """({member path: bytes}, {component: ref}) that make actor `name` play xlink `key` once on
    its own ELink and/or SLink user: the user params, the AI root and the AIInfo."""
    files = {}
    comps = {}
    if elink_user:
        path = f"{ELINK_DIR}{name}{ELINK_SUFFIX}"
        files[path] = byml.dumps({"UserName": elink_user})
        comps["ELinkRef"] = "?" + path
    if slink_user:
        path = slink_path(name)
        files[path] = byml.dumps({"UserName": slink_user})
        comps["SLinkRef"] = "?" + path
    files[f"AI/{name}.root.ainb"] = ainb.oneshot_root(template, f"{name}.root", key,
                                                      elink=bool(elink_user), slink=bool(slink_user))
    info = f"AI/AIInfo/{name}.engine__actor__AIInfo.bgyml"
    files[info] = byml.dumps({"RootAIRef": f"Work/AI/Root/{name}.root.ain"})
    comps["AIInfoRef"] = "?" + info
    return files, comps


def impact_row(source_row, name, elink_user, slink_user):
    """An impact's ActorInfo row: the source's (heap, radii), less its model and name, with its
    own effect and sound users and the puppet's INFO_OVERRIDES."""
    row = {k: v for k, v in source_row.items() if k not in IMPACT_DROP_INFO}
    if isinstance(row.get("PreActorRenderInfoHeapSize"), int):
        row["PreActorRenderInfoHeapSize"] = 0  # nothing drawn (as the game's effect locators)
    if elink_user:
        row["ELinkUserName"] = elink_user
    if slink_user:
        row["SLinkUserName"] = slink_user
    row.update(INFO_OVERRIDES)
    row["__RowId"] = name
    return row


def stand_in_row(source_row, name, with_effect=False):
    """The stand-in's ActorInfo row: the shootable's, less its effect, sound and name fields, with
    the puppet's INFO_OVERRIDES (and its STAND_IN_EFFECTS ELink user when `with_effect`)."""
    row = {k: v for k, v in source_row.items() if k not in STAND_IN_DROP_INFO}
    if with_effect and name in STAND_IN_EFFECTS:
        row["ELinkUserName"] = STAND_IN_EFFECTS[name][0]
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


def nup_scene_files(n):
    """RomFS paths of the scene files (Banc/<scene>/*.bcett.byml.zs) in an open Nup, the ones
    dungeon_rooms.scene_rooms reads from a RomFS folder."""
    out = []
    for scene, (kind, _i) in sorted(n.listdir("Banc").items()):
        if kind == "d":
            out += n.glob(f"Banc/{scene}/*.bcett.byml.zs")
    return out


def read(path):
    with open(path, "rb") as f:
        return f.read()


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def recompress(dicts, original_raw, data, level=16):
    """Compresses like the game's own copy of the file: same dictionary, or none."""
    did = zs.frame_dict_id(original_raw)
    name = None
    if did:
        name = next((n for n, d in dicts.by_name.items() if d.dict_id() == did), None)
        if name is None:
            raise BuildError(f"no dictionary with id {did}")
    return dicts.compress(data, name, level=level)


# The variants whose own Link wears the colour's model copy (above); the child's own Link took
# the colour from its replaced tiles (bigrun5 PASS), so it keeps the game's model.
SELF_MODEL_VARIANTS = ("Adult",)


def write_tunics(romfs, out_romfs, dicts, table, colors, log=print, self_dir=None, self_tiles=None,
                 self_dark_tiles=None, self_files=None):
    """The recoloured tiles and the model copies that name them (puppet_pack.tunic), with their
    size-table entries (the game's own file's). Returns {colour: [paths]}. With `self_dir`, also
    the game's tile recoloured under its own name per colour (not green) into it, listed in
    `self_tiles` ({colour: file name}): your own Link in your colour; and the dark tile (the child's
    tunic body, tunic.DARK) the same way in `self_dark_tiles`."""
    if self_dark_tiles is None:
        self_dark_tiles = {}
    if self_files is None:
        self_files = {}  # {colour: {romfs path: file beside the pack}}: every file of your own colour
    out = {}
    tile_rel = f"Tex/{tunic.TILE}.bntx.zs"
    tile_src = os.path.join(romfs, tile_rel)
    need = [tile_rel] + [f"Model/Player_{v}.Player_{v}.bfres.zs" for v in VARIANTS]
    missing = [p for p in need if not os.path.isfile(os.path.join(romfs, p))]
    if missing:
        log(f"tunics: no {', '.join(missing)} in this romfs, no colours")
        return out
    tile_raw = read(tile_src)
    tile = dicts.decompress(tile_raw)
    # The child model's second tunic texture (puppet_pack.tunic DARK): without it the child's
    # tunic body stays green. A romfs without it: the dark name stays the game's in the models.
    dark_rel = f"Tex/{tunic.DARK}.bntx.zs"
    dark_raw = dark = None
    if os.path.isfile(os.path.join(romfs, dark_rel)):
        dark_raw = read(os.path.join(romfs, dark_rel))
        dark = dicts.decompress(dark_raw)
    else:
        log(f"tunics: no {dark_rel} in this romfs, the child's tunic body stays green")
    tile_size = table.get(strip_zs(tile_rel))
    dark_size = table.get(strip_zs(dark_rel))
    # The adult's other tunic textures (puppet_pack.tunic_extra): only their green parts change.
    extras = {}
    for name in tunic_extra.EXTRA:
        rel_ = f"Tex/{name}.bntx.zs"
        if os.path.isfile(os.path.join(romfs, rel_)):
            raw_ = read(os.path.join(romfs, rel_))
            extras[name] = (rel_, raw_, dicts.decompress(raw_))
        else:
            log(f"tunics: no {rel_} in this romfs, that part of the adult's tunic stays green")
    base = tunic.average_luma(tile)
    models = {}
    for v in VARIANTS:
        m_rel = f"Model/Player_{v}.Player_{v}.bfres.zs"
        m_src = os.path.join(romfs, m_rel)
        models[v] = (m_rel, read(m_src))
    for c in colors:
        paths = []
        name = tunic.tile_name(c)
        try:
            data = tunic.tile_for(tile, c)
            own = tunic.self_tile(tile, c) if self_dir and c != "green" else None
        except tunic.TunicError as e:
            raise BuildError(f"{tile_rel}: {e}")
        if own is not None:
            # Same size as the game's tile once unpacked: its size-table entry stands.
            fname = f"{c}.bntx.zs"
            write(os.path.join(self_dir, fname), recompress(dicts, tile_raw, own))
            self_tiles[c] = f"{tunic.SELF_DIR}/{fname}"
            self_files.setdefault(c, {})[tile_rel] = self_tiles[c]
        dst = f"Tex/{name}.bntx.zs"
        write(os.path.join(out_romfs, dst), recompress(dicts, tile_raw, data))
        table.add_new(strip_zs(dst), tile_size or len(data) + PACK_SIZE_MARGIN)
        paths.append(dst)
        if dark is not None:
            try:
                ddata = tunic.dark_tile_for(tile, dark, c)
                own_dark = tunic.self_dark_tile(tile, dark, c) if self_dir and c != "green" else None
            except tunic.TunicError as e:
                raise BuildError(f"{dark_rel}: {e}")
            # Green is the game's dark tile (its size); the rest are BC3 like the main tile.
            size = dark_size if c == "green" else tile_size
            if own_dark is not None:
                fname = f"{c}.dark.bntx.zs"
                write(os.path.join(self_dir, fname), recompress(dicts, tile_raw, own_dark))
                self_dark_tiles[c] = f"{tunic.SELF_DIR}/{fname}"
                self_files.setdefault(c, {})[dark_rel] = self_dark_tiles[c]
            ddst = f"Tex/{tunic.dark_name(c)}.bntx.zs"
            write(os.path.join(out_romfs, ddst),
                  recompress(dicts, dark_raw if c == "green" else tile_raw, ddata))
            table.add_new(strip_zs(ddst), size or len(ddata) + PACK_SIZE_MARGIN)
            paths.append(ddst)
        for name, (e_rel, e_raw, e_data) in extras.items():
            try:
                e_own = None if c == "green" else tunic_extra.recolor(e_data, c, base)
                e_pup = tunic_extra.for_puppet(e_data, e_own, name, c)
            except tunic.TunicError as e:
                raise BuildError(f"{e_rel}: {e}")
            if e_own is not None and self_dir:
                # Same size and layout as the game's file: its size-table entry stands.
                fname = f"{c}.{name}.bntx.zs"
                write(os.path.join(self_dir, fname), recompress(dicts, e_raw, e_own))
                self_files.setdefault(c, {})[e_rel] = f"{tunic.SELF_DIR}/{fname}"
            edst = f"Tex/{tunic_extra.extra_name(name, c)}.bntx.zs"
            write(os.path.join(out_romfs, edst), recompress(dicts, e_raw, e_pup))
            table.add_new(strip_zs(edst), table.get(strip_zs(e_rel)) or len(e_pup) + PACK_SIZE_MARGIN)
            paths.append(edst)
        for v, (m_rel, m_raw) in models.items():
            try:
                data = tunic.model_for(dicts.decompress(m_raw), c, dark=dark is not None)
                data = tunic_extra.model_names(data, c, [n for n in extras if v in tunic_extra.EXTRA[n]])
            except tunic.TunicError as e:
                raise BuildError(f"{m_rel}: {e}")
            dst = f"Model/{tunic.project_name(v, c)}.Player_{v}.bfres.zs"
            write(os.path.join(out_romfs, dst), recompress(dicts, m_raw, data, level=3))
            table.add_new(strip_zs(dst), table.get(strip_zs(m_rel)) or len(data) + PACK_SIZE_MARGIN)
            paths.append(dst)
            if v in SELF_MODEL_VARIANTS and self_dir and c != "green":
                # Your own adult Link wears the puppet's model copy (same size: the game's entry
                # stands), which names the colour's textures: bigrun5 showed the game's own names
                # replaced in Tex/ did not reach the adult's lower tunic and hood, while the
                # puppet's renamed ones did. The kit copies it from the pack's romfs, no 2nd copy.
                self_files.setdefault(c, {})[m_rel] = f"romfs/{dst}"
        out[c] = paths
        log(f"tunic {c}: {name} and {len(models)} models")
    if self_dark_tiles and dark is not None:
        # The kit puts a BC3 copy (the main tile's size) over the game's ASTC dark tile: its
        # entry must hold the bigger of the two. A bigger entry is fine for the game's own file.
        need = tile_size or len(tile) + PACK_SIZE_MARGIN
        if dark_size is None or dark_size < need:
            table.set(strip_zs(dark_rel), need)
    return out


def build(romfs, mod_dir, log=print, heaps=None, sound=True, player_footsteps=False, sound_user=None,
          tunics=(), test_tunic=None, stand_in_effects=True, basic_xlink=False):
    """`tunics`: the tunic colours to make puppets of (puppet_pack.tunic.COLORS; each is a model,
    a tile and an actor per age, CoopPuppet_<age>_<Colour>). `test_tunic`: give the plain puppets
    that colour too (a test of the colours with a hook that knows none).
    `heaps`: {actor name: InstanceHeapSize} set outright, for tests of the heap (issue 111).
    `sound`: False builds the puppets without Link's sounds (issue 131). `player_footsteps`: keep
    the player's footstep param instead of the puppet's own (FOOTSTEP_OVERRIDES). `sound_user`: another
    SLink user for the puppets' sounds (SLINK_DIR). `basic_xlink`: the shared ActorBasic XLink param
    instead of the player's (test switch, the pack before puppet-looks)."""
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
    for c in list(tunics) + ([test_tunic] if test_tunic else []):
        if c not in tunic.COLORS:
            raise BuildError(f"no tunic colour {c} (have {', '.join(tunic.COLORS)})")
    tunics = list(dict.fromkeys(list(tunics) + ([test_tunic] if test_tunic else [])))
    self_tiles = {}
    self_dark_tiles = {}
    self_files = {}
    tunics_off = not tunics  # --tunics none: on purpose, the kit says nothing (kit-paths.ps1)
    tunic_files = write_tunics(romfs, out_romfs, dicts, table, tunics, log,
                               self_dir=os.path.join(mod_dir, tunic.SELF_DIR), self_tiles=self_tiles,
                               self_dark_tiles=self_dark_tiles, self_files=self_files) if tunics else {}
    tunics = [c for c in tunics if c in tunic_files]
    test_tunic = test_tunic if test_tunic in tunic_files else None
    jobs = [(v, None) for v in VARIANTS] + [(v, c) for v in VARIANTS for c in tunics]
    for v, color in jobs:
        player = f"Player_{v}"
        actor = tunic.actor_name(v, color) if color else puppet_name(v)
        shade = color or test_tunic
        project = tunic.project_name(v, shade) if shade else None
        src = os.path.join(romfs, "Pack", "Actor", f"{player}.pack.zs")
        if not os.path.isfile(src):
            raise BuildError(f"missing {rel(romfs, src)}")
        members = sarc.read(dicts.decompress(read(src)))
        pack, ap, names, keys = build_pack(members, v, sound, player_footsteps, sound_user, actor, project,
                                           basic_xlink=basic_xlink)
        dst_rel = f"Pack/Actor/{actor}.pack.zs"
        write(os.path.join(out_romfs, dst_rel), dicts.compress(pack, PACK_DICT))
        if player not in ai_rows:
            raise BuildError(f"ActorInfo has no row {player}")
        new_ai.append(info_row(ai_rows[player], actor, heaps.get(actor), sound, sound_user))
        if project:
            new_ai[-1]["ModelProjectName"] = project
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
            if src not in members:
                raise BuildError(f"the player's footstep param {src} is not in the pack")
            # Sized as the player's file it stands in for (the game's entries run about 5.3 KB over
            # the file), plus any bytes it is longer.
            src_size = table.get(src)
            fs_size = max((src_size or 0) + max(0, len(data) - len(members[src])), len(data) + PACK_SIZE_MARGIN)
            table.add_new(path, fs_size)
            footstep = {"param": path, "size": fs_size}
        if slink_path(actor) in names:
            path, data, src = puppet_slink(make_actor_param(flatten_actor_param(members, player)), actor, sound_user)
            if src not in members:
                raise BuildError(f"the player's SLink param {src} is not in the pack")
            table.add_new(path, max((table.get(src) or 0) + max(0, len(data) - len(members[src])),
                                    len(data) + PACK_SIZE_MARGIN))
        made.append({"actor": actor, "from": player, "tunic": shade, "model": project, "pack": dst_rel, "pack_bytes": len(pack),
                     "instance_heap": _num(new_ai[-1].get("InstanceHeapSize", 0)), "param_size": ap_size,
                     "members": names, "equipment_keys": keys, "footstep": footstep})
        log(f"{actor}: {len(names)} files, {len(pack)} bytes, {len(keys)} equipment keys -> {dst_rel}")

    src = os.path.join(romfs, "Pack", "Actor", f"{IMPACT_SOURCE}.pack.zs")
    template = None
    if os.path.isfile(src) and IMPACT_SOURCE in ai_rows:
        template = sarc.read(dicts.decompress(read(src))).get(IMPACT_TEMPLATE)
    if template is None:
        log(f"impacts: no {IMPACT_TEMPLATE} in this romfs, left out")

    shots = []
    # stand_in_effects=False (--no-seed-trail) builds the stand-ins model-only, as before issue 198,
    # so a release can back the seed trail out by building the pack again, without a code change.
    fx_template = template if stand_in_effects else None
    if template is not None and not stand_in_effects:
        log("stand-in effects (seed trail): left out (--no-seed-trail)")
    for source, name in STAND_INS:
        src = os.path.join(romfs, "Pack", "Actor", f"{source}.pack.zs")
        if not os.path.isfile(src) or source not in ai_rows:
            log(f"{name}: no {source} in this romfs, left out")
            continue
        members = sarc.read(dicts.decompress(read(src)))
        pack, ap, names = build_stand_in(members, source, name, fx_template)
        effect = name in STAND_IN_EFFECTS and fx_template is not None
        dst_rel = f"Pack/Actor/{name}.pack.zs"
        write(os.path.join(out_romfs, dst_rel), dicts.compress(pack, PACK_DICT))
        new_ai.append(stand_in_row(ai_rows[source], name, effect))
        new_gai.append({"__RowId": name})
        table.add_new(strip_zs(dst_rel), ((len(pack) + 0x1F) & ~0x1F) + PACK_SIZE_MARGIN)
        chain_ap = sum(table.get(path) or 0 for path, _ in actor_param_chain(members, source))
        ap_size = max(chain_ap, len(ap) + PACK_SIZE_MARGIN)
        table.add_new(actor_param_path(name), ap_size)
        if effect:  # its own AI, AIInfo and ELink param (sizes as for the impacts)
            for path, data in effect_files(template, name, STAND_IN_EFFECTS[name][0], None,
                                           STAND_IN_EFFECTS[name][1])[0].items():
                grow = (table.get(IMPACT_TEMPLATE) or 0) + len(data) if path.endswith(".ainb") else 0
                table.add_new(path, max(grow, len(data) + PACK_SIZE_MARGIN))
        shots.append({"actor": name, "from": source, "pack": dst_rel, "pack_bytes": len(pack),
                      "param_size": ap_size, "members": names,
                      "effect": STAND_IN_EFFECTS[name][1] if effect else None})
        log(f"{name}: {len(names)} files, {len(pack)} bytes -> {dst_rel}")

    impacts = []
    for name, elink_user, slink_user, key in IMPACTS if template is not None else ():
        files = impact_files(template, name, elink_user, slink_user, key)
        pack = sarc.write(files)
        dst_rel = f"Pack/Actor/{name}.pack.zs"
        write(os.path.join(out_romfs, dst_rel), dicts.compress(pack, PACK_DICT))
        new_ai.append(impact_row(ai_rows[IMPACT_SOURCE], name, elink_user, slink_user))
        new_gai.append({"__RowId": name})
        table.add_new(strip_zs(dst_rel), ((len(pack) + 0x1F) & ~0x1F) + PACK_SIZE_MARGIN)
        template_size = table.get(IMPACT_TEMPLATE) or 0
        for path, data in files.items():
            grow = template_size + len(data) if path.endswith(".ainb") else 0
            table.add_new(path, max(grow, len(data) + PACK_SIZE_MARGIN))
        impacts.append({"actor": name, "elink": elink_user, "slink": slink_user, "key": key,
                        "pack": dst_rel, "pack_bytes": len(pack), "members": sorted(files)})
        log(f"{name}: {key} on {elink_user or '-'} / {slink_user or '-'}, {len(pack)} bytes -> {dst_rel}")

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
        "impacts": impacts,
        "tunics": tunic_files,
        "self_tiles": self_tiles,
        "self_dark_tiles": self_dark_tiles,
        "self_files": self_files,
        "tunics_off": tunics_off,
        "test_tunic": test_tunic,
        "sound": sound,
        "footsteps": "player" if player_footsteps or not sound else "puppet",
        "sound_user": sound_user if sound and sound_user else None,
        "xlink": "player" if sound and not basic_xlink else "basic",
        "tables": [rel(romfs, p) for p in (ai_path, gai_path, rs_path)],
        "source_sha256": {rel(romfs, p): hashlib.sha256(read(p)).hexdigest()
                          for p in (ai_path, gai_path, rs_path)},
    }
    scenes, rooms, points = dungeon_rooms.write_table(romfs, os.path.join(mod_dir, "coop_rooms.txt"), dicts)
    log(f"coop_rooms.txt: {points} placements in {rooms} rooms of {scenes} dungeons")
    manifest["rooms"] = {"scenes": scenes, "rooms": rooms, "placements": points}
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
    ap.add_argument("--tunics", default=",".join(tunic.COLORS), metavar="LIST",
                    help="the tunic colours to make puppets of, comma-separated, or 'none' "
                         f"(default: all, {','.join(tunic.COLORS)})")
    ap.add_argument("--test-tunic", metavar="COLOUR",
                    help="give the plain puppets this tunic colour too (a test with a hook that knows no colours)")
    ap.add_argument("--no-sound", action="store_true",
                    help="build the puppets without Link's sounds (the pack before issue 131)")
    ap.add_argument("--no-seed-trail", action="store_true",
                    help="build the shot stand-ins model-only, without the seed's Tail trail (issue 198)")
    ap.add_argument("--player-footsteps", action="store_true",
                    help="test switch: the puppets keep the player's footstep param (the older pack)")
    ap.add_argument("--sound-user", metavar="NAME",
                    help="test switch: another actor's SLink user for the puppets' sounds, e.g. Enemy_MiddleBoss_DarkLink")
    ap.add_argument("--no-hud", action="store_true",
                    help="do not make the co-op HUD's look (font, hearts, place names)")
    ap.add_argument("--hud-lang", default=hud.DEFAULT_LANG,
                    help="the language of the HUD's place names, e.g. USen, EUen, EUfr")
    ap.add_argument("--basic-xlink", action="store_true",
                    help="test switch: the puppets get the shared ActorBasic XLink param (the older pack)")
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
        # Only the files the build reads, into a temporary RomFS folder: the pack's sources, and
        # the scene files dungeon_rooms reads for coop_rooms.txt (Banc/<scene>/*.bcett.byml.zs;
        # without them a .nup gave an empty room table).
        tmp = tempfile.mkdtemp(prefix="coop-puppet-")
        try:
            with nup.Nup(a.romfs) as n:
                for rel in list(dumpcheck.nup_files(n).values()) + nup_scene_files(n):
                    nup.write_under(tmp, rel, n.read(rel))
            print(f"read the game files from {a.romfs}")
            with nup.Nup(a.romfs) as n:
                return _main_build(a, tmp, hud.NupReader(n))
        except nup.NupError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    return _main_build(a, a.romfs, hud.DirReader(a.romfs))


def _main_build(a, romfs, hud_reader=None):
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
        tunics = [] if a.tunics.strip().lower() == "none" else [c.strip() for c in a.tunics.split(",") if c.strip()]
        build(romfs, mod_dir, heaps=heaps, sound=not a.no_sound, player_footsteps=a.player_footsteps,
              sound_user=a.sound_user, tunics=tunics, test_tunic=a.test_tunic,
              stand_in_effects=not a.no_seed_trail, basic_xlink=a.basic_xlink)
    except BuildError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if hud_reader is not None and not a.no_hud:
        _build_hud(hud_reader, os.path.join(mod_dir, hud.HUD_DIR), a.hud_lang)
    print(f"done: {mod_dir}")
    return 0


def _build_hud(reader, out_dir, lang):
    """The co-op HUD's font, hearts and place names (hud.py). A failure here does not fail
    the puppets: the co-op window then keeps its plain look."""
    try:
        hud.build(reader, out_dir, lang)
    except (hud.HudError, nup.NupError, OSError, ValueError, KeyError) as e:
        print(f"note: the co-op HUD's game look was not made ({e}); co-op still works, "
              "with plain text.")


if __name__ == "__main__":
    sys.exit(main())
