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

--skin ID=FOLDER (any number) also makes a puppet per age from a character model mod
(CoopPuppet_<age>_Skin_<ID>, skin.py); --test-skin ID gives every puppet that model (a test with
a hook that knows no skins).

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
import struct
import sys
import tempfile

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import ainb, asb_carry, byml, dumpcheck, dungeon_rooms, hud, look_cap, look_items, look_live, look_metal, map_captures, nup, pause_menu, rstb, sarc, skin, title_menu, tunic, tunic_extra, ui_pack, zs  # noqa: E402

TITLE_ID = "0400486024DEC000"
DEFAULT_MOD_NAME = "coop-puppet"
VARIANTS = ("Child", "Adult")
PACK_DICT = "pack.zsdic"
CARRY_AS_DICT = "zs.zsdic"  # the AS roots' dictionary (AS/Player_Child.root.asb.zs)

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

# Perception (issues 150 and 178): an enemy picks its target with the game's own perception search
# (AI modules EnemyCommmon.PerceptionTargetSearchAndSet and Common.MinimumPerceptionSetTarget: Gohma's
# wander and hanging battle, her eggs' hatch). It finds actors that emit perception (PerceptionEmitterRef)
# of an affiliation the enemy targets (Gohma's and her eggs' AffiliationParam EnemyCommon: TargetAffiliations
# Player, murmur3 145979025). The player has both; a puppet had neither, so a boss and its eggs in a game
# only ever went for that game's own Link. The puppet keeps the player's AffiliationRef (DefaultAffiliation
# Player) and gets a PerceptionEmitter of its own: the player's file without its AdvancedEmitters (AimTerror,
# worked by the player's own aiming code). Every perception emitter in the game has a Physics component; the
# puppet gets the one of InterestSpotActor, a logic-only emitter whose controller set is empty (no body: the
# puppet still collides with nothing).
# Was OFF for 0.11 (own-look-2, lane1 2026-11-15): with the emitter on, a game crashes ~8 s after the save
# loads when a child puppet wears a gear copy (Weapon_Sword_Kokiri_Look1) and holds the slingshot (guest null
# read 0x7d8, lr 0x82deeef8; the slingshot actor in x19). The same pack built without it plays on. Cause (perception-a,
# from the lane logs: every crashed game had no 'puppet guard' line, every game that played on had one): the
# perception's Physics fills the puppet's component slot 10, so Hookshot patch 0002's guard, which arms the null page
# when slot 10 is missing, never runs; the game's later read of the puppet's missing Player component faults. The
# module now arms the page itself before its first puppet (hookshot/src/coop_null_page.h, no game rebuild) and makes
# the quiet copies everywhere when it cannot. ON by default since the lane proof (perception-on-a, 2026-11-16: module
# with the null page + this pack, 9 lane-1 runs: gear copies and colours, held items, slingshot and tools, a scene
# change, a player leaving/crashing/rejoining, a downed player, a chase, Queen Gohma; no crash, every game armed the
# page; Gohma, her larvae and other enemies turned to the other player when nearer). `--no-perception` turns it off.
# Guards (review-boss-sync must-fix): the stealth guards (AffiliationParam NpcSecurityGuard: the castle's child
# soldiers, the Gerudo) target Player too, and their AI (NpcSecurityGuard.SetIsObserve) takes any perceived
# emitter of affiliation Player as Link: a puppet in view would catch this game's Link. Nothing in the data keeps
# them apart from the enemies: every enemy targets only Player, and the guards' sight filter
# (PerceptionFilterNone, Phive/Config/PhiveConfig.byml mask 0x41803FFF) takes the PerceptionPlayer sub-layer that
# the enemies' (PerceptionFilterPlayerOnly, mask 2) take alone. So each puppet also has a QUIET copy without the
# perception (quiet_name: CoopPuppet_ChildQuiet, CoopPuppet_ChildQuiet_Red), and the manifest lists the scenes
# whose placements have a stealth guard (guard_scenes: "quiet_scenes"); the hook makes the quiet puppets there
# (coop_puppet.cpp Planner::ActorFor). `--no-perception` builds the pack before this (no perception, no copies).
PERCEPTION_SOURCE = "InterestSpotActor"
PERCEPTION_PHYSICS = "Component/Physics/InterestSpotActor.engine__component__PhysicsParam.bgyml"
PERCEPTION_CONTROLLERS = "Phive/ControllerSetParam/InterestSpotActor.phive__ControllerSetParam.bgyml"
EMITTER_DIR = "Component/PerceptionEmitterParam/"
EMITTER_SUFFIX = ".game__component__PerceptionEmitterParam.bgyml"
EMITTER_DROP = ("AdvancedEmitters",)
QUIET = "Quiet"
GUARD_AFFILIATION = "Component/AffiliationParam/NpcSecurityGuard.game__component__AffiliationParam.bgyml"

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
    # Issue 166: the seed and the arrow once more, as stand-ins that also keep the shootable's own
    # effect and sound users (STAND_IN_XLINK). The hook asks for these first and plays the
    # shootable's hit effect on the copy with the game's own xlink search and emit, where the copy
    # lands; no AI and no extra actor. A game that will not make one gets the plain stand-in above.
    ("Shootable_DekuSeed", "CoopShotFx_Seed"),
    ("NormalArrow", "CoopShotFx_Arrow"),
)
STAND_IN_COMPONENTS = ("ModelInfoRef", "LODRef")
# Issue 166: these keep the shootable's ELink, SLink and XLink params (in place of the shared
# ActorBasic XLink param) and its ELinkUserName / SLinkUserName in the ActorInfo row, as the puppet
# keeps the player's for its sounds. Still no physics, sensor, attack or AI: nothing plays by
# itself; the hook's CoopSpawn::EffectStandIn has the same names.
STAND_IN_XLINK = ("CoopShotFx_Seed", "CoopShotFx_Arrow")
STAND_IN_XLINK_COMPONENTS = ("ELinkRef", "SLinkRef", "XLinkRef")
# Components some stand-ins keep beyond the model (by stand-in name): the animation set, so the
# hook can play the owner's animation on it (the bomb's Bomb_Blink; the hookshot tip's chain, whose
# length is its animation set's ChainExtend, issue 85).
STAND_IN_EXTRA_COMPONENTS = {
    "CoopShot_Bomb": ("ASRef", "ASInfoRef", "AnimationRef"),
    "CoopShot_Hook": ("ASRef", "ASInfoRef", "AnimationRef"),
    "CoopShot_HookLong": ("ASRef", "ASInfoRef", "AnimationRef"),
    "CoopRide_Epona": ("ASRef", "ASInfoRef", "AnimationRef"),
    "CoopShotFx_Seed": STAND_IN_XLINK_COMPONENTS,
    "CoopShotFx_Arrow": STAND_IN_XLINK_COMPONENTS,
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


def quiet_name(actor):
    """The puppet's copy no perception sees: CoopPuppet_Child -> CoopPuppet_ChildQuiet, and
    CoopPuppet_Child_Red -> CoopPuppet_ChildQuiet_Red (still a tunic actor to the hook)."""
    for v in VARIANTS:
        plain = puppet_name(v)
        if actor == plain or actor.startswith(plain + "_"):
            return plain + QUIET + actor[len(plain):]
    raise BuildError(f"{actor} is no puppet")


# --- Character Select (github.com/snaildev99/OoT-Character-Select): a puppet per character ---------------
# That mod lets a player be one of over a hundred characters; each is a player model under a file name
# of its own, Model/Player_<variant>_C<file>.Player_<variant>.bfres (file = the stored character number
# less 2). A puppet that shows such a player is the plain puppet with that model project, exactly as a
# tunic colour's puppet is: nothing of the mod's is copied, the game loads the mod's own file. Its
# characters.json (beside its romfs folder) lists the files it has and the heap sizes its models need.
CHARACTERS_FILE = "characters.json"
CHARACTERS_MOD = "OoT Character Select"
CHARACTERS_FORMAT = 1
CHARACTER_HEAPS = ("InstanceHeapSize", "PreActorRenderInfoHeapSize")


def character_actor(variant, file):
    """Still a tunic actor to the hook (CoopPuppet_<variant>_<something>), so its checks apply."""
    return f"CoopPuppet_{variant}_C{file:03d}"


def character_project(variant, file):
    return f"Player_{variant}_C{file:03d}"


def mod_list_off(mods_root, name):
    """The Mods tab's list (mods/coop-mods.list, "coop-mods 1") names `name` (its path under mods with '/') off: the
    first row for it, letters without case (as Longshot reads it). No list or no row: on (C #58: a mod switched off
    loads no models, so its characters get no puppets)."""
    fold = lambda t: "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in t.replace("\\", "/"))
    try:
        with open(os.path.join(mods_root, "coop-mods.list"), "rb") as f:
            lines = f.read().decode("utf-8", "replace").split("\n")
    except OSError:
        return False
    if lines[0].rstrip("\r") != "coop-mods 1":
        return False
    for line in lines[1:]:
        line = line.rstrip("\r")
        for state in ("on ", "off "):
            if line.startswith(state) and fold(line[len(state):]) == fold(name):
                return state == "off "
    return False


def find_characters(mod_dir, log=print):
    """The Character Select mod among the mods the pack is made into: {"mod": its folder, "files":
    {file number: [variants it has a model for]}, "heaps": {variant: {field: bytes}}, "numbers": {file:
    stored number}, "colours": {"offset", "head"} or None}, or None. Looked for beside the pack (an emulator's load/<title id>) and in the mods
    folder of a `contents/<title id>/<pack>` layout. A list that cannot be read is said and left out."""
    here = os.path.abspath(mod_dir)
    roots = [os.path.dirname(here)]
    if os.path.basename(os.path.dirname(os.path.dirname(here))).lower() == "contents":
        roots.append(os.path.dirname(os.path.dirname(os.path.dirname(here))))
    for root in roots:
        try:
            names = sorted(os.listdir(root))
        except OSError:
            continue
        for name in names:
            path = os.path.join(root, name, CHARACTERS_FILE)
            if name.startswith(("-", "_", ".")) or not os.path.isfile(path):
                continue
            try:
                with open(path, encoding="utf-8") as f:
                    doc = json.load(f)
                if doc.get("mod") != CHARACTERS_MOD or doc.get("format") != CHARACTERS_FORMAT:
                    continue
                if len(roots) > 1 and mod_list_off(roots[1], (f"contents/{os.path.basename(root)}/{name}"
                                                             if root == roots[0] else name)):
                    log(f"characters: {name} is switched off in the Mods tab: no character puppets from it")
                    continue
                files, numbers = {}, {}
                for row in doc["characters"]:
                    if "file" not in row:
                        continue
                    n = int(row["file"])
                    have = [v for v in VARIANTS if os.path.isfile(os.path.join(
                        root, name, "romfs", "Model", f"{character_project(v, n)}.Player_{v}.bfres.zs"))]
                    if have:
                        files[n], numbers[n] = have, int(row["number"])
                heaps = {v: {k: int(doc["actor_heaps"][v][k]) for k in CHARACTER_HEAPS} for v in VARIANTS}
                # The mod's colour routine for a puppet (optional): main's offset and its first 16 bytes, which
                # the hook checks before it calls it (docs/puppet.md).
                colours = None
                if isinstance(doc.get("puppet_colours"), dict):
                    c = doc["puppet_colours"]
                    offset, head = int(c["offset"]), str(c["head"]).lower()
                    if not 0 < offset < 1 << 32 or len(head) != 32 or any(ch not in "0123456789abcdef" for ch in head):
                        raise ValueError("puppet_colours: offset and 32 hex digits of head wanted")
                    colours = {"offset": offset, "head": head}
            except (OSError, ValueError, KeyError, TypeError) as e:
                log(f"characters: {path} could not be read ({e}): no character puppets")
                continue
            if files:
                return {"mod": name, "dir": os.path.join(root, name), "files": files, "heaps": heaps,
                        "numbers": numbers, "colours": colours}
    return None


# The pack's RSDB tables and size table are the game's plus the pack's rows, and they replace the mod's own copies
# (the game reads one file per path). So the Character Select mod's own changes to them go into the pack's: its rows
# that differ from the game's (its Player rows' larger heaps: up to 685 bones), its size-table entries (its model
# files, its larger files). Read from the mod's own files, nothing about their contents assumed; a mod without a copy
# of a table changes nothing.
def character_mod_rows(characters, table_rel, dicts, game_rows, log=print):
    """The rows of the mod's own copy of an RSDB table (`table_rel`, the game's path) that are new or differ from the
    game's `game_rows` ({id: row}), in the mod's order."""
    path = os.path.join(characters["dir"], "romfs", table_rel)
    if not os.path.isfile(path):
        return []
    try:
        rows = byml.loads(dicts.decompress(read(path)))
    except (OSError, ValueError, KeyError, IndexError, struct.error) as e:
        log(f"characters: {characters['mod']}'s {table_rel} could not be read ({e}): its rows left out")
        return []
    return [r for r in rows if isinstance(r, dict) and "__RowId" in r and game_rows.get(r["__RowId"]) != r]


def character_player_rows(characters, rows):
    """The Player_<age> rows (`rows`: {id: row}, the mod's if it changed them) with at least the heaps the mod's
    characters.json asks for (actor_heaps): the player's own Link is one of the characters, as with the mod alone."""
    out = []
    for v in VARIANTS:
        row = rows.get(f"Player_{v}")
        if row is None:
            continue
        row = dict(row)
        for key, size in characters["heaps"][v].items():
            old = row.get(key)
            size = max(int(size), int(_num(old or 0)))
            row[key] = type(old)(size) if isinstance(old, byml._Num) else size
        out.append(row)
    return out


def merge_character_table(table, characters, rs_rel, dicts, log=print):
    """The mod's own size table into ours (the larger size of each entry: a file either side makes), when it is for
    the game's version; then each character model file with no entry gets one, sized as its unpacked file plus the
    margin (as a skin's file). The count of entries changed."""
    tables = sorted("System/Resource/" + os.path.basename(t) for t in glob.glob(os.path.join(
        characters["dir"], "romfs", "System", "Resource", "ResourceSizeTable.Product.*.rsizetable.zs")))
    name = skin.pick_table(tables, rs_rel)
    changed = 0
    if name == rs_rel:
        try:
            theirs = rstb.Rstb.loads(dicts.decompress(read(os.path.join(characters["dir"], "romfs", name))))
        except (OSError, ValueError, rstb.RstbError) as e:
            log(f"characters: {characters['mod']}'s {name} could not be read ({e}): its entries left out")
            theirs = None
        for ours, mine in ((table.by_crc, theirs.by_crc if theirs else {}),
                           (table.by_name, theirs.by_name if theirs else {})):
            for k, size in mine.items():
                if ours.get(k, -1) < size:
                    ours[k] = size
                    changed += 1
    elif name is not None:
        log(f"characters: {characters['mod']}'s size table is for another game version ({name}): only its model "
            "files are sized")
    for n in sorted(characters["files"]):
        for v in characters["files"][n]:
            path = f"Model/{character_project(v, n)}.Player_{v}.bfres"
            if table.get(path) is not None:
                continue
            try:
                size = len(dicts.decompress(read(os.path.join(characters["dir"], "romfs", path + ".zs"))))
            except (OSError, ValueError) as e:
                log(f"characters: {path} could not be read ({e}): no size-table entry for it")
                continue
            table.add_new(path, ((size + 0x1F) & ~0x1F) + PACK_SIZE_MARGIN)
            changed += 1
    return changed


def guard_scenes(romfs, dicts):
    """{scene: [guard actor, ...]}: the scenes (Banc/<area>/<scene>_Static/_Dynamic) that place a stealth
    guard, an actor whose ActorParam takes GUARD_AFFILIATION. Only Npc_ actors are opened (every guard
    is one; the other placements are thousands of packs)."""
    placed = {}
    for path in sorted(glob.glob(os.path.join(romfs, "Banc", "*", "*.bcett.byml.zs"))):
        scene = os.path.basename(path)[: -len(".bcett.byml.zs")]
        for suffix in ("_Static", "_Dynamic"):
            if scene.endswith(suffix):
                scene = scene[: -len(suffix)]
        root = byml.loads(dicts.decompress(read(path)))
        for a in root.get("Actors", []) or []:
            name = a.get("Gyaml")
            if isinstance(name, str) and name.startswith("Npc_"):
                placed.setdefault(scene, set()).add(name)
    guard = {}
    for name in sorted(set().union(*placed.values()) if placed else ()):
        src = os.path.join(romfs, "Pack", "Actor", f"{name}.pack.zs")
        if not os.path.isfile(src):
            continue
        members = sarc.read(dicts.decompress(read(src)))
        try:
            guard[name] = ref_to_member(flatten_actor_param(members, name).get("AffiliationRef")) == GUARD_AFFILIATION
        except BuildError:
            guard[name] = False
    return {scene: sorted(n for n in names if guard.get(n)) for scene, names in sorted(placed.items())
            if any(guard.get(n) for n in names)}


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


def emitter_path(actor):
    return f"{EMITTER_DIR}{actor}{EMITTER_SUFFIX}"


def perception_members(source_members):
    """The body-less Physics files of PERCEPTION_SOURCE's pack ({member path: bytes}), or None when
    that pack lacks them (then the puppets get no perception)."""
    paths = (PERCEPTION_PHYSICS, PERCEPTION_CONTROLLERS)
    if not all(p in source_members for p in paths):
        return None
    physics = byml.loads(source_members[PERCEPTION_PHYSICS])
    if physics.get("ControllerSetPath") != "Work/" + PERCEPTION_CONTROLLERS[:-len(".bgyml")] + ".gyml":
        raise BuildError(f"{PERCEPTION_PHYSICS} names another controller set")
    if byml.loads(source_members[PERCEPTION_CONTROLLERS]):
        raise BuildError(f"{PERCEPTION_CONTROLLERS} is not empty: the puppet would get a body")
    return {p: source_members[p] for p in paths}


def puppet_emitter(members, src, actor):
    """The puppet's own emitter param: the player's `src` without EMITTER_DROP. (path, bytes, src)."""
    if src not in members:
        raise BuildError(f"the player's perception emitter {src} is not in the pack")
    own = {k: v for k, v in byml.loads(members[src]).items() if k not in EMITTER_DROP}
    if "$parent" in own:
        raise BuildError(f"the player's perception emitter {src} has a $parent")
    return emitter_path(actor), byml.dumps(own), src


def add_perception(doc, player_components, members, actor, files, perception):
    """Makes the puppet's ActorParam `doc` perceivable (PERCEPTION_SOURCE): the player's AffiliationRef,
    the puppet's own emitter param and the body-less physics. Returns the emitter's (path, bytes, the
    player's member path it stands in for)."""
    aff = player_components.get("AffiliationRef")
    src = ref_to_member(player_components.get("PerceptionEmitterRef"))
    if not aff or not src:
        raise BuildError("the player's ActorParam has no AffiliationRef or PerceptionEmitterRef")
    path, data, _ = puppet_emitter(members, src, actor)
    doc["Components"]["AffiliationRef"] = aff
    doc["Components"]["PerceptionEmitterRef"] = "?" + path
    doc["Components"]["PhysicsRef"] = "?" + PERCEPTION_PHYSICS
    files.update(collect_members(members, {"Components": {"AffiliationRef": aff}}))
    files.update(perception)
    files[path] = data
    return path, data, src


# Native puppets phase 1 (docs/native-puppets.md, `--puppet-body`, off by default): the puppet gets a body
# of its own, cut from the player's own ControllerSet ($parent chain merged): Link's shapes, matter bodies,
# contact collections and character controller, plus his cloth (hs-puppets-alt-b's `cloth` mode ran without
# the slot-10 crash). Left out: the sensors (Tgt/Atk/Chemical: nothing hits or is hit by the copy yet),
# ragdoll, rigid body entities (ride, elastic hands, reflect shield, liftable sweep), controller entities,
# IK and looking. Its character controller is the player's with DefaultMoveState Free, the value the game's
# placed NPCs use (Phive/CharacterControllerParam/Npc_Kokiri_MC_ThroughGround), so the body does not fall
# between the hook's places (the held bomb's fall, reports/held-bomb.md). Not run in a game yet.
BODY_KEYS = ("InitializerName", "ShapeNamePathAry", "MatterRigidBodyNamePathAry", "CColEntityNamePathAry")
CLOTH_KEYS = ("Cloth", "ClothAdvandecOption", "ClothList", "ClothReaction")
BODY_MOVE_STATE = "Free"
PHYSICS_DIR = "Component/Physics/"
PHYSICS_SUFFIX = ".engine__component__PhysicsParam.bgyml"
# The body's character controller runs the game's default component preset, as the placed NPCs' do
# (Npc_Kokiri_MC_ThroughGround names no PresetTypeName). The player's names the "Player" preset
# (Bootup pack, Phive/Config/PhiveConfig.byml, CharacterComponentPresetCollection), whose external
# components PlayerClimb, PlayerClimbLadder and PlayerRagdoll are built for the player's own body. The
# body has no ragdoll, so PlayerRagdoll's setup (vtable 0x84bbb9f8, CharacterExternalPlayerRagdoll, slot 5 at
# 0x80a19ac0) read the actor's ragdoll as null and wrote through it: crash 0xc0000005 at guest 0x70 on
# ModuleSystemWorker right after `create CoopPuppet_Child_Black` (crash-0116-b). So PresetTypeName goes, and
# of the player's UpdateComponentParam and MoveStateParam only the entries the default preset has stay
# (its UpdateComponents and MoveStateComponents, from the same file): InteractWithRagdoll (also a ragdoll
# user), PlayerOnGround and the other Player-only entries go.
BODY_DROP_KEYS = ("PresetTypeName",)
BODY_UPDATE_COMPONENTS = ("FrameSetup", "MoveStateUpdate", "Rotate", "JumpExtension", "FloatWater",
                          "AccumulateImpulse", "FollowSurfaceExtension", "FollowRagdoll", "ApplyVelocity")
BODY_MOVE_STATES = ("InAir", "OnGround", "Free")
CONTROLLER_SET_DIR = "Phive/ControllerSetParam/"
CONTROLLER_SET_SUFFIX = ".phive__ControllerSetParam.bgyml"
CHARACTER_DIR = "Phive/CharacterControllerParam/"
CHARACTER_SUFFIX = ".phive__CharacterControllerParam.bgyml"


def _chain(members, path):
    """A member and its in-pack `$parent` chain: [(member path, document)], the member first."""
    chain, seen = [], set()
    while path and path in members and path not in seen:
        seen.add(path)
        doc = byml.loads(members[path])
        if not isinstance(doc, dict):
            break
        chain.append((path, doc))
        path = ref_to_member(doc.get("$parent"))
    return chain


def _merged_chain(members, path):
    """A member with its in-pack `$parent` chain merged (child wins)."""
    out = {}
    for _, doc in reversed(_chain(members, path)):
        out.update({k: v for k, v in doc.items() if k != "$parent"})
    return out


def chain_entry(table, paths, data):
    """The size-table entry of a file we made from the game's files `paths` (a member and the `$parent`s merged
    into it): the sum of their entries, or the file's bytes and the margin when that is more. The game parses
    a param in a heap of its entry, and each game file's entry is sized for that file only, so a merged file
    needs the room of every file it holds. With the top file's entry alone the child puppet's body (its
    character controller is Player_Child's merged with Player's) ran out of heap at its InteractWithRagdoll
    object (factory main+0x107dd10, game__physics__CharacterUpdateInteractWithRagdollParam): a null pointer
    and a crash in GeneralFormatAsync_ when the puppet loaded (crash-0116-a; the ActorParam's case was
    issue 111)."""
    return max(sum(table.get(p) or 0 for p in paths), len(data) + PACK_SIZE_MARGIN)


def chain_paths(members, path):
    """The member paths chain_entry sizes a merge of `path` by: the member and its in-pack `$parent`s."""
    return [p for p, _ in _chain(members, path)] or [path]


def _work_ref(member):
    """`Phive/X.phive__Y.bgyml` -> `Work/Phive/X.phive__Y.gyml` (how params name each other)."""
    return "Work/" + member[:-len(".bgyml")] + ".gyml"


def body_paths(actor):
    """The puppet's own (PhysicsParam, ControllerSetParam, CharacterControllerParam) member paths."""
    return (f"{PHYSICS_DIR}{actor}{PHYSICS_SUFFIX}", f"{CONTROLLER_SET_DIR}{actor}{CONTROLLER_SET_SUFFIX}",
            f"{CHARACTER_DIR}{actor}{CHARACTER_SUFFIX}")


def body_character_controller(cct):
    """The body's character controller from the player's merged one `cct`: in Free (BODY_MOVE_STATE), on the
    game's default component preset (no PresetTypeName), with only the update and move-state params that
    preset has (BODY_UPDATE_COMPONENTS, BODY_MOVE_STATES). An emptied map is left out."""
    out = {k: v for k, v in cct.items() if k not in BODY_DROP_KEYS}
    for key, keep in (("UpdateComponentParam", BODY_UPDATE_COMPONENTS), ("MoveStateParam", BODY_MOVE_STATES)):
        if key not in out:
            continue
        kept = {k: v for k, v in (out[key] or {}).items() if k in keep} if isinstance(out[key], dict) else {}
        if kept:
            out[key] = kept
        else:
            del out[key]
    out["DefaultMoveState"] = BODY_MOVE_STATE
    return out


def pvp_sensors(cs, cs_src):
    """The player's `Tgt` sensor only, from its merged ControllerSet `cs` (`--puppet-pvp`, docs/native-puppets.md
    5.1): {ControllerSet key: the entries PVP_SENSORS names}. The `Atk` receipt, `ForArea`, the attack and
    chemical bodies are left out."""
    out = {}
    for key, names in PVP_SENSORS.items():
        field = "AliasSensor" if key == "AliasSensorList" else "Name"
        keep = [e for e in cs.get(key) or () if isinstance(e, dict) and e.get(field) in names]
        if not keep:
            raise BuildError(f"the player's ControllerSet {cs_src} has no {'/'.join(names)} in {key}")
        out[key] = keep
    return out


def puppet_body(members, player_components, actor, sensors=False):
    """The puppet's body files (`--puppet-body`): {member path: (bytes, the player's member it is cut from)}.
    `sensors`: the `_Pvp` copy's body, with the player's `Tgt` sensor (pvp_sensors)."""
    src = ref_to_member(player_components.get("PhysicsRef"))
    if not src or src not in members:
        raise BuildError("the player's ActorParam has no PhysicsRef in the pack")
    cs_src = ref_to_member(_merged_chain(members, src).get("ControllerSetPath"))
    if not cs_src or cs_src not in members:
        raise BuildError(f"the player's physics param {src} has no ControllerSet in the pack")
    cs = _merged_chain(members, cs_src)
    missing = [k for k in BODY_KEYS if k not in cs]
    if missing:
        raise BuildError(f"the player's ControllerSet {cs_src} has no {', '.join(missing)}")
    cct_src = ref_to_member(cs.get("PathCharacterController"))
    if not cct_src or cct_src not in members:
        raise BuildError(f"the player's ControllerSet {cs_src} has no character controller in the pack")
    cct = body_character_controller(_merged_chain(members, cct_src))
    phys_path, cs_path, cct_path = body_paths(actor)
    own_cs = {k: cs[k] for k in BODY_KEYS + CLOTH_KEYS if k in cs}
    # Only the contact collection the character controller names (Contact_0_cct); not the ragdoll's.
    user = cct.get("UserContactCollectionName")
    own_cs["CColEntityNamePathAry"] = [e for e in cs["CColEntityNamePathAry"]
                                       if isinstance(e, dict) and e.get("Name") == user]
    if not own_cs["CColEntityNamePathAry"]:
        raise BuildError(f"the player's ControllerSet {cs_src} has no contact collection {user!r}")
    own_cs["PathCharacterController"] = _work_ref(cct_path)
    if sensors:
        own_cs.update(pvp_sensors(cs, cs_src))
    return {phys_path: (byml.dumps({"ControllerSetPath": _work_ref(cs_path)}), src),
            cs_path: (byml.dumps(own_cs), cs_src),
            cct_path: (byml.dumps(cct), cct_src)}


def add_body(doc, player_components, members, actor, files, sensors=False):
    """Gives the puppet's ActorParam `doc` its body (puppet_body), with every member the body names."""
    own = puppet_body(members, player_components, actor, sensors)
    for path, (data, _) in own.items():
        files[path] = data
        files.update(collect_members(members, byml.loads(data)))
    doc["Components"]["PhysicsRef"] = "?" + body_paths(actor)[0]
    return own


# Native puppets phase 3 (docs/native-puppets.md "Lifting another player", `--puppet-liftable`, off by default).
# Owner rule (2026-11-19): only a YOUNG Link can be lifted, and he is carried as Link carries Ruto in Jabu-Jabu.
# So only the child puppets (CoopPuppet_Child*, LIFTABLE_VARIANTS) get a LiftableRef, and it is Ruto's own
# (LIFTABLE_SOURCE, Npc_Zora_Ruto_Child_001: Component/LiftableParam/NpcZoraRutoChild; her LiftableType, lift
# and put offsets, weight 19 and SpecialDeal), its `$parent` chain merged as far as her pack holds it. Link's own
# lifter then lifts the copy as it lifts Ruto: the game picks Link's Ruto set by the LiftableType (Player_Child
# and Player_Adult AS: Lift_Ready_Ruto, Lift_Up_Ruto, Pose_Ruto / _Run / _Caution, Lift_Put_Ruto,
# Lift_Throw_Ruto). An adult puppet has no LiftableRef at all: the game itself can never lift it. The hook
# (coop_lift_player.h, LICHII_COOP_LIFT_PLAYER=1) sees the lift and asks the lifted player's game. Not run in a
# game yet: whether a lifter takes Ruto's LiftGrade on a copy without her AI, and what SpecialDeal,
# IsResetBehaviorModeOnThrown and KeepSensorHitToParent do there, are phase 3's UNKNOWNs.
LIFTABLE_SOURCE = "Npc_Zora_Ruto_Child_001"
LIFTABLE_VARIANTS = ("Child",)


def liftable_param(source_members):
    """Ruto's LiftableRef param: (its member path, the param with its in-pack `$parent` chain merged),
    or None when LIFTABLE_SOURCE's pack has none."""
    try:
        comps = flatten_actor_param(source_members, LIFTABLE_SOURCE)
    except BuildError:
        return None
    src = ref_to_member(comps.get("LiftableRef"))
    if not src or src not in source_members:
        return None
    return src, _merged_chain(source_members, src)


def liftable_path(actor, src):
    """The puppet's own liftable param, beside Ruto's: `<dir>/<actor>.<her file kind>`."""
    head, _, base = src.rpartition("/")
    return f"{head}/{actor}.{base.split('.', 1)[1]}"


def add_liftable(doc, actor, files, liftable):
    """Gives the puppet's ActorParam `doc` a LiftableRef of its own (`liftable`: liftable_param()).
    Returns (member path, bytes, Ruto's member it is cut from)."""
    src, param = liftable
    path = liftable_path(actor, src)
    data = byml.dumps(param)
    files[path] = data
    doc["Components"]["LiftableRef"] = "?" + path
    return path, data, src


# Native puppets phase 4 (docs/native-puppets.md section 5, `--puppet-pvp`, off by default; implies
# `--puppet-body`): beside each puppet a second actor, `<puppet>_Pvp`, that the hook makes only while PvP is on
# between two games that run PvP on real bodies (LICHII_COOP_PVP_NATIVE=1). It has the player's own SensorHit,
# Life, Reaction and Target params; its body has the player's `Tgt` sensor only; it carries the player's
# AttackParam rows (5.3 A, option i); and it has an attention sender of its own whose LockOn and AutoAim clients
# are cut from EnemyCommon's (ATTENTION_SOURCE's pack), so Z-targeting and aim assist find it as they find an
# enemy. The plain puppet stays byte for byte as before. Not run in a game yet (UNKNOWNs U1-U7, docs 5.8).
PVP_SUFFIX = "_Pvp"
PVP_COMPONENTS = ("SensorHitRef", "LifeRef", "ReactionRef", "TargetRef")
PVP_SENSORS = {"CColSensorNamePathAry": ("Tgt",), "ControllerSensorNamePathAry": ("Tgt",),
               "RigidBodySensorNamePathAry": ("Body_Tgt",), "AliasSensorList": ("Tgt",)}
ATTENTION_SOURCE = "Enemy_Amos"
ATTENTION_CLIENTS = ("LockOn", "AutoAim")
ATTENTION_CLIENT_DIR = "Attention/ClientParam/"
ATTENTION_CLIENT_SUFFIX = ".game__attention__ClientParam.bgyml"
ATTENTION_SENDER_DIR = "Component/AttentionSenderParam/"
ATTENTION_SENDER_SUFFIX = ".game__component__AttentionSenderParam.bgyml"
ATTACK_PARAM_DIR = "GameBalance/AttackParam/"
ATTACK_PARAM_SUFFIX = ".game__gamebalance__AttackParam.bgyml"


def pvp_name(actor):
    return actor + PVP_SUFFIX


def _client_src(client):
    return f"{ATTENTION_CLIENT_DIR}EnemyCommon_{client}{ATTENTION_CLIENT_SUFFIX}"


def attention_clients(source_members):
    """EnemyCommon's LockOn and AutoAim client params, each with its in-pack `$parent` chain merged:
    {client: param}, or None when ATTENTION_SOURCE's pack lacks one."""
    if any(_client_src(c) not in source_members for c in ATTENTION_CLIENTS):
        return None
    return {c: _merged_chain(source_members, _client_src(c)) for c in ATTENTION_CLIENTS}


def attention_paths(actor):
    """The `_Pvp` copy's (sender param, {client: client param}) member paths."""
    return (f"{ATTENTION_SENDER_DIR}{actor}{ATTENTION_SENDER_SUFFIX}",
            {c: f"{ATTENTION_CLIENT_DIR}{actor}_{c}{ATTENTION_CLIENT_SUFFIX}" for c in ATTENTION_CLIENTS})


def puppet_pvp(members, player_components, actor, clients):
    """The `_Pvp` copy's own files besides its body: {member path: (bytes, the member it is cut from)}."""
    missing = [k for k in PVP_COMPONENTS if ref_to_member(player_components.get(k)) not in members]
    if missing:
        raise BuildError(f"the player's ActorParam has no {', '.join(missing)} in the pack")
    sender, client_paths = attention_paths(actor)
    own = {path: (byml.dumps(clients[c]), _client_src(c)) for c, path in client_paths.items()}
    own[sender] = (byml.dumps({"ClientList": [_work_ref(client_paths[c]) for c in ATTENTION_CLIENTS]}),
                   f"{ATTENTION_SENDER_DIR}EnemyCommon{ATTENTION_SENDER_SUFFIX}")
    return own


def add_pvp(doc, player_components, members, actor, files, clients):
    """Gives the `_Pvp` copy's ActorParam `doc` the player's PVP_COMPONENTS, the player's AttackParam rows and
    an attention sender of its own (puppet_pvp). Returns puppet_pvp's files."""
    for k in PVP_COMPONENTS:
        doc["Components"][k] = player_components[k]
        files.update(collect_members(members, {k: player_components[k]}))
    for path in sorted(members):
        if path.startswith(ATTACK_PARAM_DIR) and path.endswith(ATTACK_PARAM_SUFFIX):
            files[path] = members[path]
    own = puppet_pvp(members, player_components, actor, clients)
    for path, (data, _) in own.items():
        files[path] = data
    doc["Components"]["AttentionSenderRef"] = "?" + attention_paths(actor)[0]
    return own


def add_carry(doc, actor, files):
    """The young puppet's carried pose (asb_carry): its own AnimationParam (the player's, Ruto's animations
    added, retargeted) and its ASRef on the carry AS root. Returns the AnimationParam's member path, or None
    when the player's AnimationParam is not in the pack (then the puppet is left as it was)."""
    src = ref_to_member(doc["Components"].get("AnimationRef"))
    if not src or src not in files:
        return None
    path = asb_carry.carry_animation_path(actor)
    files[path] = byml.dumps(asb_carry.carry_animation_param(byml.loads(files[src])))
    doc["Components"]["AnimationRef"] = "?" + path
    doc["Components"]["ASRef"] = "?" + asb_carry.CARRY_AS
    return path


def tunic_model_info(actor, variant, project):
    """(member path, bytes) of a colour puppet's ModelInfo: the player's model under its own
    project name, `Model/<project>.Player_<variant>.bfres` (puppet_pack.tunic)."""
    path = f"Component/ModelInfo/{actor}.engine__component__ModelInfo.bgyml"
    return path, byml.dumps({"FmdbName": f"Player_{variant}", "ModelProjectName": project})


def build_pack(members, variant, sound=True, player_footsteps=False, sound_user=None, actor=None,
               project=None, basic_xlink=False, perception=None, as_ref=None, body=False, liftable=None,
               carry=False, pvp=None):
    """Returns (pack bytes, actor param bytes, member list, equipment keys) for one variant.
    `actor`: the pack's actor (default CoopPuppet_<variant>); `project`: a model project of its own
    (a tunic colour), else the player's. `perception`: perception_members() to make the puppet a
    target of the game's perception (PERCEPTION_SOURCE), or None. `as_ref`: an AS root of its own
    (a skin's copy, skin.as_path), else the player's. `body`: the puppet's own body (`--puppet-body`,
    puppet_body). `liftable`: liftable_param() to make the puppet liftable (`--puppet-liftable`), or None;
    only a young variant (LIFTABLE_VARIANTS) takes it: an adult puppet is never liftable. `carry`: with
    `liftable`, the young puppet plays Ruto's carried pose (asb_carry): its ASRef on asb_carry.CARRY_AS (unless
    a skin gives its own AS) and an AnimationParam of its own with Ruto's animations, retargeted.
    `pvp`: attention_clients() to make this actor a `_Pvp` copy (`--puppet-pvp`: a body with the `Tgt` sensor,
    add_pvp), or None."""
    player = f"Player_{variant}"
    actor = actor or puppet_name(variant)
    player_components = flatten_actor_param(members, player)
    doc = make_actor_param(player_components, sound, basic_xlink)
    files = collect_members(members, doc)
    if perception:
        add_perception(doc, player_components, members, actor, files, perception)
    if body or pvp:
        add_body(doc, player_components, members, actor, files, sensors=bool(pvp))
    if pvp:
        add_pvp(doc, player_components, members, actor, files, pvp)
    if liftable and variant in LIFTABLE_VARIANTS:
        add_liftable(doc, actor, files, liftable)
        if carry and variant in asb_carry.CARRY_VARIANTS and not as_ref:
            add_carry(doc, actor, files)
    if project:
        path, data = tunic_model_info(actor, variant, project)
        files[path] = data
        doc["Components"]["ModelInfoRef"] = "?" + path
    if as_ref:
        doc["Components"]["ASRef"] = "?" + as_ref
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
    for k in extra:  # a kept component wins over the shared default (the shootable's own XLinkRef)
        if k in components:
            comps[k] = components[k]
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
    # (issue 166: a STAND_IN_XLINK one keeps the shootable's effect and sound user names)
    drop = ("ActorName",) if name in STAND_IN_XLINK else STAND_IN_DROP_INFO
    row = {k: v for k, v in source_row.items() if k not in drop}
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
    dungeon_rooms.scene_rooms reads from a RomFS folder, and the two map files map_captures reads."""
    out = []
    for scene, (kind, _i) in sorted(n.listdir("Banc").items()):
        if kind == "d":
            out += n.glob(f"Banc/{scene}/*.bcett.byml.zs")
    for pattern in map_captures.FILES:  # the map pictures' places, for coop_maps.txt
        out += n.glob(pattern)[:1]
    return out


def nup_menu_files(n):
    """RomFS paths of what the menus' steps edit (title_menu.py's title rows, pause_menu.py's Multiplayer tab), in an
    open Nup: their UI archives and every language's message archive. A .nup build extracts only what is named, and
    these are not the pack's own sources: without them both steps were left out ("No such file")."""
    out = []
    for pattern in (title_menu.TITLE_ARCHIVE, pause_menu.COMMON_ARCHIVE):
        out += n.glob(pattern)[:1]
    newest = {}   # (a .nup has the base game's and the update's: the game reads the newest, Product.110 over 100)
    for rel in n.glob(pause_menu.MALS_GLOB):
        lang = rel.split("/")[-1].split(".")[0]
        if lang not in newest or _product(rel) > _product(newest[lang]):
            newest[lang] = rel
    return out + [newest[lang] for lang in sorted(newest)]


def _product(rel):
    """The number after .Product. in a RomFS name (100, 110 ...), 0 when there is none."""
    part = rel.split(".Product.")[-1].split(".")[0] if ".Product." in rel else ""
    return int(part) if part.isdigit() else 0


def nup_look_files(n, dicts):
    """RomFS paths of the gear colours' sources in an open Nup: each gear texture (look_live.GEAR,
    for coop_look/gear.bin: your own sword, shields, gauntlets, boots) and each coloured item's
    actor pack and models (look_items.ITEMS, write_look_items: the puppets' copies). `dicts`: the
    game's zstd dictionaries (to read the item packs' ModelInfo). The 0.11.0 kit's packs, made
    from the player's .nup, had none of them: only the boots in gear.bin, and no item copies."""
    out = []
    for g in look_live.GEAR:
        out += n.glob(f"Tex/{g}.bntx.zs")[:1]
    for item in look_items.ITEMS:
        packs = n.glob(f"Pack/Actor/{item}.pack.zs")[:1]
        if not packs:
            continue
        out += packs
        try:
            _, project, _ = look_items.model_info(sarc.read(dicts.decompress(n.read(packs[0]))), item)
        except (look_items.TunicError, ValueError, KeyError):
            continue   # (write_look_items says so)
        out += n.glob(f"Model/{project}.*.bfres.zs")
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


# The variants whose own Link wears the colour's model copy (above). The child too since
# colour-agree: the owner's lane1 run (2026-11-14) showed the child's own Link with the hat in the
# colour but the body and sleeves still the game's green, its replaced Tex/ files in place, while
# the puppet of the same colour (renamed copies) was coloured all over: as the adult in bigrun5.
SELF_MODEL_VARIANTS = ("Adult", "Child")


def write_tunics(romfs, out_romfs, dicts, table, colors, log=print, self_dir=None, self_tiles=None,
                 self_dark_tiles=None, self_files=None, look_dir=None, look_files=None):
    """The recoloured tiles and the model copies that name them (puppet_pack.tunic), with their
    size-table entries (the game's own file's). Returns {colour: [paths]}. With `self_dir`, also
    the game's tile recoloured under its own name per colour (not green) into it, listed in
    `self_tiles` ({colour: file name}): your own Link in your colour; and the dark tile (the child's
    tunic body, tunic.DARK) the same way in `self_dark_tiles`. With `look_dir`, also each colour's
    texels of your own Link's tunic textures (puppet_pack.look_live), green among them, listed in
    `look_files` ({colour: file name}): your colour changed while the game runs."""
    if look_files is None:
        look_files = {}
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
    # The other tunic textures, adult and child (puppet_pack.tunic_extra): only their green parts change.
    extras = {}
    for name in tunic_extra.EXTRA:
        rel_ = f"Tex/{name}.bntx.zs"
        if os.path.isfile(os.path.join(romfs, rel_)):
            raw_ = read(os.path.join(romfs, rel_))
            extras[name] = (rel_, raw_, dicts.decompress(raw_))
        else:
            log(f"tunics: no {rel_} in this romfs, that part of the tunic stays green")
    base = tunic.average_luma(tile)
    models = {}
    for v in VARIANTS:
        m_rel = f"Model/Player_{v}.Player_{v}.bfres.zs"
        m_src = os.path.join(romfs, m_rel)
        models[v] = (m_rel, read(m_src))
    # The boots in the body models (look_items.BODY_BOOTS): each colour's model names its own copy.
    boots = {}
    for name in look_items.BODY_BOOTS:
        b_rel = f"Tex/{name}.bntx.zs"
        if os.path.isfile(os.path.join(romfs, b_rel)):
            boots[name] = (b_rel, read(os.path.join(romfs, b_rel)))
        else:
            log(f"tunics: no {b_rel} in this romfs, those boots stay the game's on puppets")
    own_sizes = {}  # your own copies' Tex/ paths -> size-table entry, added once
    for c in colors:
        paths = []
        name = tunic.tile_name(c)
        # Your own Link's S copies for this colour, green too (npc-colour): the game's tunic
        # textures are shared with Mido's and Saria's models, so your own Link never wears them.
        own_set = bool(self_dir)
        try:
            # The puppets' copy is marked: its texels are never your own Link's (tunic.TUNIC_MARK).
            data = look_live.mark_bntx(tunic.tile_for(tile, c), tunic.TUNIC_MARK)
            # Your own Link's: marked (tunic.SELF_MARK), so never the texels of a texture the game shares
            # with other models (green: the game's own tile).
            own = (look_live.mark_bntx(tile if c == "green" else tunic.self_tile(tile, c), tunic.SELF_MARK)
                   if self_dir or look_dir else None)
        except tunic.TunicError as e:
            raise BuildError(f"{tile_rel}: {e}")
        look = [(tunic.TILE, tile if own is None else own)]
        # (0.11.9, exact puppet tunics: the puppets' copy as the pack writes it, to be found in memory and repainted
        # live in its wearer's exact colour; the same textures, in the same order, as green.bin's)
        pup = [(name, data)]
        if own is not None and self_dir and c != "green":
            # Same size as the game's tile once unpacked: its size-table entry stands. (Packs
            # before colour-agree put this one over the game's tile; the kit now uses self_files.)
            fname = f"{c}.bntx.zs"
            write(os.path.join(self_dir, fname), recompress(dicts, tile_raw, own))
            self_tiles[c] = f"{tunic.SELF_DIR}/{fname}"
        if own_set:
            own_name = tunic.tile_name(c, tunic.SELF_LETTER)
            fname = f"{c}.{own_name}.bntx.zs"
            write(os.path.join(self_dir, fname), recompress(dicts, tile_raw, tunic.rename(own, tunic.TILE, own_name)))
            self_files.setdefault(c, {})[f"Tex/{own_name}.bntx.zs"] = f"{tunic.SELF_DIR}/{fname}"
            own_sizes[strip_zs(f"Tex/{own_name}.bntx.zs")] = tile_size or len(own) + PACK_SIZE_MARGIN
        dst = f"Tex/{name}.bntx.zs"
        write(os.path.join(out_romfs, dst), recompress(dicts, tile_raw, data))
        table.add_new(strip_zs(dst), tile_size or len(data) + PACK_SIZE_MARGIN)
        paths.append(dst)
        if dark is not None:
            try:
                ddata = look_live.mark_bntx(tunic.dark_tile_for(tile, dark, c), tunic.TUNIC_MARK)
                own_dark = (look_live.mark_bntx(dark if c == "green" else tunic.self_dark_tile(tile, dark, c),
                                                tunic.SELF_MARK) if self_dir or c == "green" else None)
            except tunic.TunicError as e:
                raise BuildError(f"{dark_rel}: {e}")
            # Green is the game's dark tile (its size); the rest are BC3 like the main tile.
            size = dark_size if c == "green" else tile_size
            if own_dark is not None and self_dir:
                if c != "green":
                    fname = f"{c}.dark.bntx.zs"
                    write(os.path.join(self_dir, fname), recompress(dicts, tile_raw, own_dark))
                    self_dark_tiles[c] = f"{tunic.SELF_DIR}/{fname}"
                own_name = tunic.dark_name(c, tunic.SELF_LETTER)
                fname = f"{c}.{own_name}.bntx.zs"
                write(os.path.join(self_dir, fname),
                      recompress(dicts, dark_raw if c == "green" else tile_raw,
                                 tunic.rename(own_dark, tunic.DARK, own_name)))
                self_files.setdefault(c, {})[f"Tex/{own_name}.bntx.zs"] = f"{tunic.SELF_DIR}/{fname}"
                # (one entry for every colour's file: the biggest of them)
                own_rel = strip_zs(f"Tex/{own_name}.bntx.zs")
                own_sizes[own_rel] = max(own_sizes.get(own_rel, 0), (size or len(own_dark) + PACK_SIZE_MARGIN)
                                         if c == "green" else tile_size or len(own_dark) + PACK_SIZE_MARGIN)
            pup.append((tunic.dark_name(c), ddata))
            if look_dir:
                # In memory: the game's ASTC dark tile (green), else the BC3 copy your own Link
                # wears for that colour; without those, the ASTC one recoloured in place.
                try:
                    look.append((tunic.DARK, own_dark if own_dark is not None
                                 else tunic_extra.recolor(dark, c, base)))
                except tunic.TunicError as e:
                    raise BuildError(f"{dark_rel}: {e}")
            ddst = f"Tex/{tunic.dark_name(c)}.bntx.zs"
            write(os.path.join(out_romfs, ddst),
                  recompress(dicts, dark_raw if c == "green" else tile_raw, ddata))
            table.add_new(strip_zs(ddst), size or len(ddata) + PACK_SIZE_MARGIN)
            paths.append(ddst)
        for name, (e_rel, e_raw, e_data) in extras.items():
            try:
                e_own = None if c == "green" else tunic_extra.recolor(e_data, c, base)
                e_pup = look_live.mark_bntx(tunic_extra.for_puppet(e_data, e_own, name, c), tunic.TUNIC_MARK)
                # your own Link's: marked (an extra with no green part is the game's own texels)
                e_own = look_live.mark_bntx(e_data if e_own is None else e_own, tunic.SELF_MARK)
            except tunic.TunicError as e:
                raise BuildError(f"{e_rel}: {e}")
            look.append((name, e_own))
            pup.append((tunic_extra.extra_name(name, c), e_pup))
            if own_set:
                # Same size and layout as the game's file.
                own_name = tunic_extra.extra_name(name, c, tunic.SELF_LETTER)
                fname = f"{c}.{own_name}.bntx.zs"
                write(os.path.join(self_dir, fname), recompress(dicts, e_raw, tunic.rename(e_own, name, own_name)))
                self_files.setdefault(c, {})[f"Tex/{own_name}.bntx.zs"] = f"{tunic.SELF_DIR}/{fname}"
                own_sizes[strip_zs(f"Tex/{own_name}.bntx.zs")] = (table.get(strip_zs(e_rel))
                                                                  or len(e_own) + PACK_SIZE_MARGIN)
            edst = f"Tex/{tunic_extra.extra_name(name, c)}.bntx.zs"
            write(os.path.join(out_romfs, edst), recompress(dicts, e_raw, e_pup))
            table.add_new(strip_zs(edst), table.get(strip_zs(e_rel)) or len(e_pup) + PACK_SIZE_MARGIN)
            paths.append(edst)
        for v, (m_rel, m_raw) in models.items():
            names = [n for n in extras if v in tunic_extra.EXTRA[n]]
            try:
                m_data = dicts.decompress(m_raw)
                data = tunic.model_for(m_data, c, dark=dark is not None)
                data = tunic_extra.model_names(data, c, names)
                data = look_items.body_model_for(data, c) if boots else data   # (the puppets' only)
                own_model = None
                if own_set and v in SELF_MODEL_VARIANTS:
                    own_model = tunic.model_for(m_data, c, dark=dark is not None, letter=tunic.SELF_LETTER)
                    own_model = tunic_extra.model_names(own_model, c, names, letter=tunic.SELF_LETTER)
            except tunic.TunicError as e:
                raise BuildError(f"{m_rel}: {e}")
            dst = f"Model/{tunic.project_name(v, c)}.Player_{v}.bfres.zs"
            write(os.path.join(out_romfs, dst), recompress(dicts, m_raw, data, level=3))
            table.add_new(strip_zs(dst), table.get(strip_zs(m_rel)) or len(data) + PACK_SIZE_MARGIN)
            paths.append(dst)
            if own_model is not None:
                # Your own Link wears its own model copy (same size: the game's entry stands),
                # naming your own S textures: bigrun5 (adult) and the lane1 run of 2026-11-14
                # (child) showed the game's own names replaced in Tex/ did not reach the whole
                # tunic, while renamed ones did; and not the puppets' copies (colour-agree).
                fname = f"{c}.Player_{v}.bfres.zs"
                write(os.path.join(self_dir, fname), recompress(dicts, m_raw, own_model, level=3))
                self_files.setdefault(c, {})[m_rel] = f"{tunic.SELF_DIR}/{fname}"
        for name, (b_rel, b_raw) in boots.items():
            try:
                b_data = look_items.boots_texture_for(dicts.decompress(b_raw), name, c)
            except tunic.TunicError as e:
                raise BuildError(f"{b_rel}: {e}")
            b_dst = f"Tex/{look_items.boots_name(name, c)}.bntx.zs"
            write(os.path.join(out_romfs, b_dst), recompress(dicts, b_raw, b_data))
            table.add_new(strip_zs(b_dst), table.get(strip_zs(b_rel)) or len(b_data) + PACK_SIZE_MARGIN)
            paths.append(b_dst)
        if look_dir:
            write(os.path.join(look_dir, f"{c}.bin"), look_live.pack(look))
            look_files[c] = f"{look_live.DIR}/{c}.bin"
            if c != "green":   # (the plain puppet's tunic is the game's own: never repainted)
                write(os.path.join(look_dir, f"puppet-{c}.bin"), look_live.pack(pup))
                look_files[f"puppet-{c}"] = f"{look_live.DIR}/puppet-{c}.bin"
        out[c] = paths
        log(f"tunic {c}: {tunic.tile_name(c)} and {len(models)} models")
    if look_dir and out and "green" not in out:
        # Green to go back to: the game's own texels, marked as green's S copies would be
        # (tunic.SELF_MARK), never the game's shared ones: with no green S copies in the pack your
        # own Link is not found (keeps its colour) rather than every Kokiri repainted (npc-colour).
        mark = lambda b: look_live.mark_bntx(b, tunic.SELF_MARK)  # noqa: E731
        look = [(tunic.TILE, mark(tile))] + ([(tunic.DARK, mark(dark))] if dark is not None else [])
        look += [(n, mark(e_data)) for n, (_, _, e_data) in extras.items()]
        write(os.path.join(look_dir, "green.bin"), look_live.pack(look))
        look_files["green"] = f"{look_live.DIR}/green.bin"
    for path, size in own_sizes.items():
        table.add_new(path, size)
    if self_dark_tiles and dark is not None:
        # The kit puts a BC3 copy (the main tile's size) over the game's ASTC dark tile: its
        # entry must hold the bigger of the two. A bigger entry is fine for the game's own file.
        need = tile_size or len(tile) + PACK_SIZE_MARGIN
        if dark_size is None or dark_size < need:
            table.set(strip_zs(dark_rel), need)
    return out


def write_look_items(romfs, out_romfs, dicts, table, ai_rows, gai_rows, slots, log=print):
    """The puppets' own copies of the coloured items (puppet_pack.look_items): per slot, each
    item's actor, its models naming the slot's textures, and those textures. Returns (the
    manifest's look_items, new ActorInfo rows, new GameActorInfo rows); an item this romfs lacks
    is left out and said."""
    made, new_ai, new_gai, files = [], [], [], []
    textures = {}  # gear name -> (Tex/ path, raw), read once
    for item in look_items.ITEMS:
        src = os.path.join(romfs, "Pack", "Actor", f"{item}.pack.zs")
        if not os.path.isfile(src) or item not in ai_rows:
            log(f"look items: no {item} in this romfs, its colours stay the game's on puppets")
            continue
        try:
            members = sarc.read(dicts.decompress(read(src)))
            _, project, _ = look_items.model_info(members, item)
            info_chain = look_items.model_info_chain(members, item)
        except (look_items.TunicError, ValueError, KeyError) as e:
            log(f"look items: {item} left out ({e})")
            continue
        models = sorted(f for f in os.listdir(os.path.join(romfs, "Model"))
                        if f.startswith(project + ".") and f.endswith(".bfres.zs"))
        if not models:
            log(f"look items: no Model/{project}.* in this romfs, {item} left out")
            continue
        # (each slot's two copies, then one more, the neutral copy (row 198): the game's colours, marked, never
        # painted, worn by a puppet with no slot of its own, so it never wears the game's item your colour is drawn on)
        for slot in range(1, slots * look_items.COPIES_PER_SLOT + 2):
            actor = look_items.actor_name(item, slot)
            pack, ap, info = look_items.copy_pack(members, item, slot)
            dst = f"Pack/Actor/{actor}.pack.zs"
            write(os.path.join(out_romfs, dst), dicts.compress(pack, PACK_DICT))
            table.add_new(strip_zs(dst), ((len(pack) + 0x1F) & ~0x1F) + PACK_SIZE_MARGIN)
            ap_size = table.get(look_items.actor_param_path(item)) or 0
            table.add_new(look_items.actor_param_path(actor), max(ap_size, len(ap) + PACK_SIZE_MARGIN))
            table.add_new(look_items.model_info_path(actor), chain_entry(table, info_chain, info))
            # Row 184: the copy's own AnimationParam (retargeted clips), with room for each IsRetarget it adds
            # (row 198: sized as the item's, the game refused to make the shield copies).
            anim = look_items.copy_animation_param(members, item, actor)
            if anim is not None:
                anim_size = table.get(look_items.item_animation_param(members, item)[0]) or 0
                table.add_new(anim[0], look_items.copy_animation_param_entry(members, item, anim_size, anim[1],
                                                                             PACK_SIZE_MARGIN))
            new_ai.append(look_items.info_row(ai_rows[item], item, slot))
            new_gai.append(dict(gai_rows.get(item, {}), __RowId=actor))
            paths = [dst]
            named = set()
            for m in models:
                m_rel = f"Model/{m}"
                m_raw = read(os.path.join(romfs, "Model", m))
                data, names = look_items.model_for(dicts.decompress(m_raw), slot)
                named.update(names)
                m_dst = f"Model/{actor}{m[len(project):]}"
                write(os.path.join(out_romfs, m_dst), recompress(dicts, m_raw, data, level=3))
                table.add_new(strip_zs(m_dst), table.get(strip_zs(m_rel)) or len(data) + PACK_SIZE_MARGIN)
                paths.append(m_dst)
            for g in sorted(named):
                if g not in textures:
                    t_rel = f"Tex/{g}.bntx.zs"
                    p = os.path.join(romfs, t_rel)
                    textures[g] = (t_rel, read(p)) if os.path.isfile(p) else None
                if textures[g] is None:
                    continue
                t_rel, t_raw = textures[g]
                data = look_items.texture_for(dicts.decompress(t_raw), g, slot)
                t_dst = f"Tex/{look_items.tex_name(g, slot)}.bntx.zs"
                write(os.path.join(out_romfs, t_dst), recompress(dicts, t_raw, data))
                table.add_new(strip_zs(t_dst), table.get(strip_zs(t_rel)) or len(data) + PACK_SIZE_MARGIN)
                paths.append(t_dst)
            files += paths
        made.append(item)
    if made:
        log(f"look items: {len(made)} items x {slots} slots x {look_items.COPIES_PER_SLOT} copies ({len(files)} files): "
            "puppets in their own gear colours")
    copies = slots * look_items.COPIES_PER_SLOT
    return ({"slots": slots, "copies": copies, "neutral": copies + 1, "actors": made, "files": files} if made else None,
            new_ai, new_gai)


def write_blade_items(romfs, out_romfs, dicts, table, ai_rows, gai_rows, copies, log=print):
    """Wearer copies (look_items, "Wearer copies"): every player item whose weapon component
    sets its model's values from its wearer, read from the romfs (look_items.is_wearer_item), once
    per player number; its models once per model project and number, the game's bytes unchanged.
    Returns (the manifest's blade_items, new ActorInfo rows, new GameActorInfo rows)."""
    made, new_ai, new_gai, files = [], [], [], []
    pdir = os.path.join(romfs, "Pack", "Actor")
    mdir = os.path.join(romfs, "Model")
    model_files = sorted(f for f in os.listdir(mdir) if f.endswith(".bfres.zs")) if os.path.isdir(mdir) else []
    projects_done = set()
    for f in sorted(os.listdir(pdir)) if os.path.isdir(pdir) else []:
        if not f.startswith("Weapon_") or not f.endswith(".pack.zs"):
            continue
        item = f[:-len(".pack.zs")]
        try:
            members = sarc.read(dicts.decompress(read(os.path.join(pdir, f))))
            if not look_items.is_wearer_item(members, item):
                continue
            _, project, _ = look_items.model_info(members, item)
            info_chain = look_items.model_info_chain(members, item)
        except (look_items.TunicError, ValueError, KeyError) as e:
            log(f"wearer copies: {item} left out ({e})")
            continue
        if item not in ai_rows:
            log(f"wearer copies: {item} has no ActorInfo row, left out")
            continue
        models = [m for m in model_files if m.startswith(project + ".")]
        if not models:
            log(f"wearer copies: no Model/{project}.* in this romfs, {item} left out")
            continue
        for n in range(1, copies + 1):
            actor = look_items.blade_name(item, n)
            copy_project = look_items.blade_name(project, n)
            pack, ap, info = look_items.copy_pack_as(members, item, actor, copy_project)
            dst = f"Pack/Actor/{actor}.pack.zs"
            write(os.path.join(out_romfs, dst), dicts.compress(pack, PACK_DICT))
            table.add_new(strip_zs(dst), ((len(pack) + 0x1F) & ~0x1F) + PACK_SIZE_MARGIN)
            ap_size = table.get(look_items.actor_param_path(item)) or 0
            table.add_new(look_items.actor_param_path(actor), max(ap_size, len(ap) + PACK_SIZE_MARGIN))
            table.add_new(look_items.model_info_path(actor), chain_entry(table, info_chain, info))
            # Row 184: the copy's own AnimationParam (retargeted clips), with room for each IsRetarget it adds
            # (row 198: sized as the item's, the game refused to make the shield copies).
            anim = look_items.copy_animation_param(members, item, actor)
            if anim is not None:
                anim_size = table.get(look_items.item_animation_param(members, item)[0]) or 0
                table.add_new(anim[0], look_items.copy_animation_param_entry(members, item, anim_size, anim[1],
                                                                             PACK_SIZE_MARGIN))
            new_ai.append(look_items.info_row_as(ai_rows[item], actor, copy_project))
            new_gai.append(dict(gai_rows.get(item, {}), __RowId=actor))
            files.append(dst)
            if (project, n) in projects_done:
                continue   # (the Giant's Knife on the Biggoron's: its models are there already)
            projects_done.add((project, n))
            named = set()
            for m in models:
                raw = read(os.path.join(mdir, m))
                m_dst = f"Model/{copy_project}{m[len(project):]}"
                # (row 198: its own gear textures, marked, never the game's that your Link's colour is drawn over)
                data, names = look_items.blade_model_for(dicts.decompress(raw), n)
                named.update(names)
                write(os.path.join(out_romfs, m_dst), recompress(dicts, raw, data, level=3) if names else raw)
                table.add_new(strip_zs(m_dst), table.get(strip_zs(f"Model/{m}")) or len(data))
                files.append(m_dst)
            for g in sorted(named):
                t_rel = f"Tex/{g}.bntx.zs"
                t_path = os.path.join(romfs, t_rel)
                if not os.path.isfile(t_path):
                    continue
                t_raw = read(t_path)
                t_data = look_items.blade_texture_for(dicts.decompress(t_raw), g, n)
                t_dst = f"Tex/{look_items.blade_tex_name(g, n)}.bntx.zs"
                write(os.path.join(out_romfs, t_dst), recompress(dicts, t_raw, t_data))
                table.add_new(strip_zs(t_dst), table.get(strip_zs(t_rel)) or len(t_data) + PACK_SIZE_MARGIN)
                files.append(t_dst)
        made.append(item)
    if made:
        log(f"wearer copies: {len(made)} items ({', '.join(made)}) x {copies} player numbers, "
            f"{len({p for p, _ in projects_done})} model projects ({len(files)} files): every puppet's own")
    return ({"copies": copies, "actors": made, "files": files} if made else None), new_ai, new_gai


class _StagedTable:
    """A size table's changes held back (write_skins: a skin's entries go in only when it is built whole)."""

    def __init__(self, table):
        self.table = table
        self.changes = {}  # path -> (how, size)

    def get(self, path):
        if path in self.changes:
            return self.changes[path][1]
        return self.table.get(path)

    def add_new(self, path, size):
        self.changes[path] = ("add_new", size)

    def set(self, path, size):
        self.changes[path] = ("set", size)

    def commit(self):
        for path, (how, size) in self.changes.items():
            getattr(self.table, how)(path, size)


def write_skins(romfs, out_romfs, dicts, table, skins, ai_rows=None, new_ai=None, new_gai=None, log=print,
                self_dir=None, game_has=None, game_read=None, effects=False):
    """Reads each character model mod (skin.load) and writes its own new files, its AS root copy and
    its copies of the gear it changes (skin.gear_items, `<item>_Skin_<id>`, rows into `new_ai` and
    `new_gai`) into the pack, with their size-table entries. `skins`: [(id, mod folder)]. With
    `self_dir`, also your own Link's player packs per age (skin.self_pack) into `self_dir/<id>/`,
    listed in the entry's `self_files` ({romfs path: file beside the pack}) for the kit to put in place
    when you wear that skin. With `effects`, also each mod's own copies of the game's effect files
    (skin.effect_files) as they are. Returns [(skin.Skin, {variant: AS root copy or None}, manifest entry)].
    A skin that cannot be built (a broken mod, a file another skin has with other contents) is left out
    with a line in the log; the others and the rest of the pack are built. `game_has(path)`: whether the
    game's romfs has that file (default: a file under `romfs`); `game_read(path)`: its bytes (default: read
    under `romfs`). A .nup build gives both through its reader: `romfs` there holds only some files."""
    ai_rows = ai_rows or {}
    new_ai = new_ai if new_ai is not None else []
    new_gai = new_gai if new_gai is not None else []
    game_members = {}
    for v in VARIANTS:
        p = os.path.join(romfs, "Pack", "Actor", f"Player_{v}.pack.zs")
        if os.path.isfile(p):
            game_members[v] = sarc.read(dicts.decompress(read(p)))
    out = []
    seen = set()
    written = {}  # path -> (skin id, sha256): files the skins wrote, shared when the same
    if game_has is None:
        game_has = lambda path: os.path.isfile(os.path.join(romfs, path))  # noqa: E731
    if game_read is None:
        game_read = lambda path: read(os.path.join(romfs, path))  # noqa: E731
    game_tables = glob.glob(os.path.join(romfs, "System", "Resource", "ResourceSizeTable.Product.*.rsizetable.zs"))
    game_table = ("System/Resource/" + os.path.basename(game_tables[0])) if len(game_tables) == 1 else None

    def one(skin_id, mod_dir, w, t, ai, gai, mine):
        """One skin, into w (writes), t (size table), ai/gai (rows) and mine (its files): kept only when
        it is whole, so a broken mod costs only its own skin."""
        sk = skin.load(skin_id, mod_dir, dicts, read, game_table)
        variants = skin.check_changes_model(sk, game_members, flatten_actor_param)
        items = []  # (path, bytes) for the skin's hash
        files = []
        taken = skin.new_files(sk, t, written, game_has)
        # A model project another skin has with other contents (an outfit variant of the same mod) is
        # moved to a name of its own, <project>_<id>, with every ModelProjectName naming it (skin.py).
        # Only the skin's own model projects (its player models and its gear's) are renamed: a project
        # such as the game's Player_Adult, under which a mod adds a file, is named everywhere in its packs
        # for the game's own files too. A file of other contents there keeps the first skin's copy.
        own = {skin.model_info(sk.members[v], flatten_actor_param(sk.members[v], f"Player_{v}"))[1]
               for v in variants}
        own |= {g[3] for g in skin.gear_items(sk, dicts, read, game_has, game_read)}
        own.discard(None)
        renames = {}
        kept = []
        for f in taken:
            proj = skin.project_of(f)
            if proj and proj not in renames and f in written and \
                    written[f][1] != hashlib.sha256(read(os.path.join(sk.romfs, f))).hexdigest():
                if proj in own:
                    renames[proj] = skin.unique_project(proj, skin_id)
                else:
                    kept.append(f)
        if kept:
            log(f"skin {skin_id}: " + ", ".join(kept) + " differ from another skin's; that skin's copy is used "
                "(not this skin's model)")
            taken = [f for f in taken if f not in kept]
        if renames:
            for v in list(sk.members):
                sk.members[v] = skin.rename_projects(sk.members[v], renames)
            log(f"skin {skin_id}: model project " + ", ".join(f"{a} -> {b}" for a, b in renames.items())
                + " (another skin has it with other files)")
        sk.renames = renames
        for src in taken:
            f = skin.renamed_path(src, renames)
            raw = read(os.path.join(sk.romfs, src))
            digest = hashlib.sha256(raw).hexdigest()
            if f in written:
                other, other_digest = written[f]
                if other_digest != digest:
                    # Two versions of one mod (an outfit variant) name their models the same: only one
                    # of them can be in the pack until skins get files of their own names.
                    raise BuildError(f"{f} is also in skin {other} with other contents "
                                     f"(two versions of one mod, or two mods with a file of one name); only one of them can be in the pack")
                items.append((f, raw))
                files.append(f)
                continue  # the same file: shared
            mine[f] = (skin_id, digest)
            w(os.path.join(out_romfs, f), raw)
            size = sk.table.get(strip_zs(src)) if sk.table is not None else None
            if size is None:
                data = dicts.decompress(raw) if f.endswith(".zs") else raw
                size = len(data) + PACK_SIZE_MARGIN
            t.add_new(strip_zs(f), size)
            items.append((f, raw))
            files.append(f)
        # --skin-effects: the mod's own copies of the game's effect files (Effect/, ELink2/), such as a recoloured
        # sword glow. The game finds effects by its own names, so these go in as they are: every sword with that
        # effect shows it in this game, not only the skin's. Only a path the game has, exactly (another version's
        # link table is skipped, with a line); a mod's new effect resource is new_files' (once). Each skin's own
        # copy counts in its hash, whichever skin's copy the pack holds.
        fx, fx_skipped = skin.effect_files(sk, game_has) if effects else ((), ())
        for f in fx_skipped:
            log(f"skin {skin_id}: {f} left out: the game has no such effect file (another version?)")
        for f in fx:
            raw = read(os.path.join(sk.romfs, f))
            digest = hashlib.sha256(raw).hexdigest()
            items.append((f, raw))   # (the skin's own copy in its hash, whatever another skin brings)
            files.append(f)
            if f in written:
                if written[f][1] != digest:
                    log(f"skin {skin_id}: {f} differs from skin {written[f][0]}'s; that skin's is used")
                continue
            mine[f] = (skin_id, digest)
            w(os.path.join(out_romfs, f), raw)
            data = dicts.decompress(raw) if f.endswith(".zs") else raw
            t.set(strip_zs(f), max(t.get(strip_zs(f)) or 0,
                                   (sk.table.get(strip_zs(f)) if sk.table is not None else None) or 0,
                                   (len(data) + 0x1F) & ~0x1F))
        as_refs = {}
        for v in variants:
            items.append((f"Pack/Actor/Player_{v}.pack", sarc.write(sk.members[v])))
            src = f"AS/Player_{v}.root.asb.zs"
            if os.path.isfile(os.path.join(sk.romfs, src)):
                raw = read(os.path.join(sk.romfs, src))
                dst = skin.as_path(skin_id, v)
                w(os.path.join(out_romfs, dst + ".zs"), raw)
                game_size = t.get(strip_zs(src))
                mod_size = sk.table.get(strip_zs(src)) if sk.table is not None else None
                size = max(game_size or 0, mod_size or 0) or len(dicts.decompress(raw)) + PACK_SIZE_MARGIN
                t.add_new(dst, size)
                items.append((dst, raw))
                files.append(dst + ".zs")
                as_refs[v] = dst
            else:
                as_refs[v] = None  # the mod keeps the game's AS: so does its puppet
        self_files = {}
        for v in variants if self_dir else ():
            # Your own Link: the mod's player pack with its ASRef on the skin's own AS copy, so the puppets
            # keep the game's AS root. Put over Pack/Actor/Player_<age> only for the player who wears it.
            pack = skin.self_pack(sk.members[v], v, as_refs[v])
            rel_ = f"Pack/Actor/Player_{v}.pack.zs"
            fname = f"{skin_id}/Player_{v}.pack.zs"
            w(os.path.join(self_dir, fname), dicts.compress(pack, PACK_DICT))
            self_files[rel_] = f"{skin.SELF_DIR}/{fname}"
            # Its entry holds the bigger of the game's pack and this one (a bigger entry is fine for the game's).
            need = max(t.get(strip_zs(rel_)) or 0,
                       (sk.table.get(strip_zs(rel_)) if sk.table is not None else None) or 0,
                       ((len(pack) + 0x1F) & ~0x1F) + PACK_SIZE_MARGIN)
            t.set(strip_zs(rel_), need)
            for path, _ in actor_param_chain(sk.members[v], f"Player_{v}"):
                mod_size = sk.table.get(path) if sk.table is not None else None
                if mod_size and mod_size > (t.get(path) or 0):
                    t.set(path, mod_size)
        gear = {}
        for item, g_members, fmdb, project, info_src in skin.gear_items(sk, dicts, read, game_has, game_read, log):
            if item not in ai_rows:
                log(f"skin {skin_id}: {item} is no actor of this game, left out")
                continue
            actor = skin.gear_name(item, skin_id)
            project = renames.get(project, project)
            pack, ap, info = skin.gear_pack(skin.rename_projects(g_members, renames), item, skin_id, fmdb, project)
            dst = f"Pack/Actor/{actor}.pack.zs"
            w(os.path.join(out_romfs, dst), dicts.compress(pack, PACK_DICT))
            t.add_new(strip_zs(dst), ((len(pack) + 0x1F) & ~0x1F) + PACK_SIZE_MARGIN)
            ap_size = max(t.get(actor_param_path(item)) or 0,
                          (sk.table.get(actor_param_path(item)) if sk.table is not None else None) or 0)
            t.add_new(actor_param_path(actor), max(ap_size, len(ap) + PACK_SIZE_MARGIN))
            t.add_new(f"Component/ModelInfo/{actor}.engine__component__ModelInfo.bgyml",
                          max(t.get(info_src) or 0, len(info) + PACK_SIZE_MARGIN))
            row = dict(ai_rows[item], __RowId=actor, ModelProjectName=project, FmdbName=fmdb)
            if "ActorName" in row:
                row["ActorName"] = actor
            ai.append(row)
            gai.append({"__RowId": actor})
            items.append((dst, pack))
            files.append(dst)
            gear[item] = actor
        models = {v: skin.model_info(sk.members[v], flatten_actor_param(sk.members[v], f"Player_{v}"))[1]
                  for v in variants}
        entry = {"id": skin_id, "variants": variants, "models": models,
                 "actors": {v: skin.actor_name(v, skin_id) for v in variants},
                 "as": as_refs, "gear": gear, "self_files": self_files, "files": files,
                 "sha256": skin.content_hash(items), "renamed": renames,
                 "table": sk.table is not None}
        skipped = [v for v in sk.variants if v not in variants]
        log(f"skin {skin_id}: {', '.join(variants)} ({', '.join(models[v] for v in variants)}), "
            f"{len(files)} files" + (f", gear {', '.join(gear)}" if gear else "")
            + (f"; {', '.join(skipped)} unchanged, left out" if skipped else ""))
        return sk, as_refs, entry

    for skin_id, mod_dir in skins:
        if skin_id.lower() in seen:
            log(f"skin {skin_id}: left out: the id is given twice")
            continue
        seen.add(skin_id.lower())
        writes, ai, gai, mine = [], [], [], {}
        t = _StagedTable(table)
        try:
            sk, as_refs, entry = one(skin_id, mod_dir, lambda path, data: writes.append((path, data)), t, ai, gai,
                                     mine)
        except Exception as e:  # noqa: BLE001 - any broken mod: its skin is left out, the pack is built
            reason = str(e) if isinstance(e, (skin.SkinError, BuildError)) else f"{type(e).__name__}: {e}"
            log(f"skin {skin_id}: left out: {reason}")
            continue
        for path, data in writes:
            write(path, data)
        t.commit()
        new_ai.extend(ai)
        new_gai.extend(gai)
        written.update(mine)
        out.append((sk, as_refs, entry))
    return out


def skin_row(row, sk, variant):
    """A skin puppet's ActorInfo row: the puppet's, with the mod's model and its animation projects
    added (the retargeted ones the mod's AnimationParam lists)."""
    comps = flatten_actor_param(sk.members[variant], f"Player_{variant}")
    fmdb, project = skin.model_info(sk.members[variant], comps)
    if project:
        row["ModelProjectName"] = project
    if fmdb:
        row["FmdbName"] = fmdb
    anims = list(row.get("AnimationResources") or [])
    have = {a.get("ModelProjectName") for a in anims if isinstance(a, dict)}
    for name in skin.animation_projects(sk.members[variant], comps):
        if name not in have:
            anims.append({"ModelProjectName": name})
    if anims:
        row["AnimationResources"] = anims
    return row


def build(romfs, mod_dir, log=print, heaps=None, sound=True, player_footsteps=False, sound_user=None,
          tunics=(), test_tunic=None, stand_in_effects=False, basic_xlink=False, title_rows=False, pause_tab=False,
          look_slots=look_items.SLOTS, perception=True, blade_copies=look_items.BLADE_COPIES,
          skins=(), test_skin=None, game_has=None, game_read=None, skin_effects=False, body=False, liftable=False,
          pvp=False, characters=None):
    """`characters`: find_characters()'s answer, for a puppet per character of the Character Select mod
    (character_actor; each with its quiet copy, as the colours). `tunics`: the tunic colours to make puppets of (puppet_pack.tunic.COLORS; each is a model,
    a tile and an actor per age, CoopPuppet_<age>_<Colour>). `test_tunic`: give the plain puppets
    that colour too (a test of the colours with a hook that knows none).
    `heaps`: {actor name: InstanceHeapSize} set outright, for tests of the heap (issue 111).
    `sound`: False builds the puppets without Link's sounds (issue 131). `player_footsteps`: keep
    the player's footstep param instead of the puppet's own (FOOTSTEP_OVERRIDES). `sound_user`: another
    SLink user for the puppets' sounds (SLINK_DIR). `basic_xlink`: the shared ActorBasic XLink param
    instead of the player's (test switch, the pack before puppet-looks). `look_slots`: how many
    players' puppets can wear gear colours of their own (puppet_pack.look_items; 0: none).
    `perception`: True makes the puppets a target of the game's perception (PERCEPTION_SOURCE), each with a
    quiet copy (quiet_name) for the scenes with stealth guards (guard_scenes); on by default (PERCEPTION_SOURCE).
    `skins`: [(id, mod folder)] character model mods to make puppets of (skin.py; each a puppet per
    age it changes, CoopPuppet_<age>_Skin_<id>). `test_skin`: one of those ids; every other puppet
    (plain and colours) wears that model too, a test with a hook that knows no skins. `skin_effects`: the
    skins' own copies of the game's effect files go into the pack too (skin.effect_files; every sword with
    that effect shows it, for everyone whose pack has the skin).
    `body`: every puppet gets a body of its own (`--puppet-body`, puppet_body; off by default).
    `liftable`: every young (child) puppet gets Ruto's LiftableRef (`--puppet-liftable`, liftable_param; off by default).
    `pvp`: every puppet gets a `_Pvp` copy beside it (`--puppet-pvp`, add_pvp; off by default); the manifest
    then says "pvp": true."""
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
    seen_by = None
    lift_param = None
    lift_chain = None
    carry_data = None
    if liftable:
        src = os.path.join(romfs, "Pack", "Actor", f"{LIFTABLE_SOURCE}.pack.zs")
        if not os.path.isfile(src):
            raise BuildError(f"--puppet-liftable: missing {rel(romfs, src)}")
        ruto = sarc.read(dicts.decompress(read(src)))
        lift_param = liftable_param(ruto)
        if lift_param is None:
            raise BuildError(f"--puppet-liftable: {LIFTABLE_SOURCE} has no LiftableRef in its pack")
        lift_chain = chain_paths(ruto, lift_param[0])
        # Ruto's carried pose on the young copies (asb_carry): the player's AS root with the carry clips, or the
        # game's own Eaten when it cannot be made (the fallback, logged).
        carry_src = f"AS/Player_{asb_carry.CARRY_VARIANTS[0]}.root.asb"
        carry_path = os.path.join(romfs, carry_src + ".zs")
        carry_data = asb_carry.carry_as(dicts.decompress(read(carry_path))) if os.path.isfile(carry_path) else None
        if carry_data is None:
            log(f"note: --puppet-liftable: no carry AS from {carry_src}: a carried copy plays Link's own Eaten")
        else:
            write(os.path.join(out_romfs, asb_carry.CARRY_AS + ".zs"), dicts.compress(carry_data, CARRY_AS_DICT))
            table.add_new(asb_carry.CARRY_AS, max(table.get(carry_src) or 0, len(carry_data)) + PACK_SIZE_MARGIN)
            log(f"{asb_carry.CARRY_AS}: {carry_src} with Eaten's clips on Ruto's {asb_carry.RUTO_CLIP}")
    clients = None
    client_chains = {}
    if pvp:
        src = os.path.join(romfs, "Pack", "Actor", f"{ATTENTION_SOURCE}.pack.zs")
        if not os.path.isfile(src):
            raise BuildError(f"--puppet-pvp: missing {rel(romfs, src)}")
        amos = sarc.read(dicts.decompress(read(src)))
        clients = attention_clients(amos)
        client_chains = {_client_src(c): chain_paths(amos, _client_src(c)) for c in ATTENTION_CLIENTS}
        if clients is None:
            raise BuildError(f"--puppet-pvp: {ATTENTION_SOURCE} has no EnemyCommon LockOn/AutoAim clients")
    if perception:
        src = os.path.join(romfs, "Pack", "Actor", f"{PERCEPTION_SOURCE}.pack.zs")
        seen_by = perception_members(sarc.read(dicts.decompress(read(src)))) if os.path.isfile(src) else None
        if seen_by is None:
            log(f"note: no {PERCEPTION_SOURCE} physics in this romfs: the puppets are no target of the game's perception")
        else:
            for path in seen_by:
                if table.get(path) is None:
                    raise BuildError(f"the size table has no {path}")
    for c in list(tunics) + ([test_tunic] if test_tunic else []):
        if c not in tunic.COLORS:
            raise BuildError(f"no tunic colour {c} (have {', '.join(tunic.COLORS)})")
    tunics = list(dict.fromkeys(list(tunics) + ([test_tunic] if test_tunic else [])))
    self_tiles = {}
    self_dark_tiles = {}
    self_files = {}
    look_files = {}
    tunics_off = not tunics  # --tunics none: on purpose, the kit says nothing (kit-paths.ps1)
    tunic_files = write_tunics(romfs, out_romfs, dicts, table, tunics, log,
                               self_dir=os.path.join(mod_dir, tunic.SELF_DIR), self_tiles=self_tiles,
                               self_dark_tiles=self_dark_tiles, self_files=self_files,
                               look_dir=os.path.join(mod_dir, look_live.DIR), look_files=look_files) if tunics else {}
    tunics = [c for c in tunics if c in tunic_files]
    test_tunic = test_tunic if test_tunic in tunic_files else None
    if tunics:
        # The gear's own texels, for the Link customizer (look_live.GEAR).
        def read_tex(name):
            p = os.path.join(romfs, "Tex", f"{name}.bntx.zs")
            return dicts.decompress(read(p)) if os.path.isfile(p) else None
        gear_blob = look_live.gear(read_tex, log)
        if gear_blob is not None:
            write(os.path.join(mod_dir, look_live.DIR, look_live.GEAR_FILE), gear_blob)
            look_files["gear"] = f"{look_live.DIR}/{look_live.GEAR_FILE}"
            log(f"look: {look_live.DIR}/{look_live.GEAR_FILE} ({len(gear_blob) >> 10} KB of gear textures)")
            # Which metalness blocks lie under which metal part (#202, look_metal).
            metal_blob = look_metal.metal(read_tex, log)
            if metal_blob is not None:
                write(os.path.join(mod_dir, look_live.DIR, look_metal.FILE), metal_blob)
                look_files["metal"] = f"{look_live.DIR}/{look_metal.FILE}"
        # The blocks of the tunic's textures that only the cap uses (the Customize page's Cap trim,
        # look_cap), beside green.bin, which holds their texels.
        if "green" in look_files:
            def read_model(variant):
                p = os.path.join(romfs, *look_cap.model_path(variant).split("/"))
                return dicts.decompress(read(p)) if os.path.isfile(p) else None
            cap_blob = look_cap.cap(read_tex, read_model, log)
            if cap_blob is not None:
                write(os.path.join(mod_dir, look_live.DIR, look_cap.FILE), cap_blob)
                look_files["cap"] = f"{look_live.DIR}/{look_cap.FILE}"
    look_item_files = None
    look_slots = max(0, min(look_slots, look_items.MAX_SLOTS))
    if tunics and look_slots > 0:
        gai_rows = {r["__RowId"]: r for r in gai}
        look_item_files, rows, grows = write_look_items(romfs, out_romfs, dicts, table, ai_rows, gai_rows,
                                                        look_slots, log)
        new_ai += rows
        new_gai += grows
    blade_files = None
    if blade_copies > 0:
        gai_rows = {r["__RowId"]: r for r in gai}
        blade_files, rows, grows = write_blade_items(romfs, out_romfs, dicts, table, ai_rows, gai_rows,
                                                     blade_copies, log)
        new_ai += rows
        new_gai += grows
    skin_list = write_skins(romfs, out_romfs, dicts, table, skins, ai_rows, new_ai, new_gai, log,
                            self_dir=os.path.join(mod_dir, skin.SELF_DIR), game_has=game_has,
                            game_read=game_read, effects=skin_effects) if skins else []
    by_id = {sk.id.lower(): (sk, refs) for sk, refs, _ in skin_list}
    if test_skin and test_skin.lower() not in by_id:
        if any(sid.lower() == test_skin.lower() for sid, _ in skins):
            raise BuildError(f"--test-skin {test_skin}: that skin was left out (see its line above)")
        raise BuildError(f"--test-skin {test_skin}: no --skin with that id")
    worn = by_id.get(test_skin.lower()) if test_skin else None
    jobs = [(v, None) for v in VARIANTS] + [(v, c) for v in VARIANTS for c in tunics]
    jobs = [(v, c, False, None) for v, c in jobs] + ([(v, c, True, None) for v, c in jobs] if seen_by else [])
    # Each skin's puppet per age, with its quiet copy for the guard scenes when puppets are seen.
    jobs += [(v, None, q, (sk, refs)) for sk, refs, _ in skin_list for v in refs
             for q in ((False, True) if seen_by else (False,))]
    # Each character of the Character Select mod per age it has, with its quiet copy as the skins'.
    jobs = [(*j, None) for j in jobs]
    if characters:
        jobs += [(v, None, q, None, n) for n in sorted(characters["files"]) for v in VARIANTS
                 if v in characters["files"][n] for q in ((False, True) if seen_by else (False,))]
    # Each puppet's `_Pvp` copy right after it (--puppet-pvp; off: the jobs as before).
    jobs = [(*j, False) for j in jobs]
    if clients:
        jobs = [j for job in jobs for j in (job, (*job[:5], True))]
    for v, color, quiet, own_skin, char, pvp_copy in jobs:
        player = f"Player_{v}"
        # A skin job, or (--test-skin) any puppet of an age the tested skin changes.
        wears = own_skin or (worn if worn and v in worn[1] and char is None else None)
        if own_skin:
            actor = skin.actor_name(v, own_skin[0].id)
        elif char is not None:
            actor = character_actor(v, char)
        else:
            actor = tunic.actor_name(v, color) if color else puppet_name(v)
        actor = quiet_name(actor) if quiet else actor
        actor = pvp_name(actor) if pvp_copy else actor
        shade = None if wears or char is not None else color or test_tunic
        project = character_project(v, char) if char is not None else (tunic.project_name(v, shade) if shade else None)
        if wears:
            members = wears[0].members[v]
        else:
            src = os.path.join(romfs, "Pack", "Actor", f"{player}.pack.zs")
            if not os.path.isfile(src):
                raise BuildError(f"missing {rel(romfs, src)}")
            members = sarc.read(dicts.decompress(read(src)))
        pack, ap, names, keys = build_pack(members, v, sound, player_footsteps, sound_user, actor, project,
                                           basic_xlink=basic_xlink, perception=None if quiet else seen_by,
                                           as_ref=wears[1][v] if wears else None, body=body,
                                           liftable=lift_param, carry=carry_data is not None,
                                           pvp=clients if pvp_copy else None)
        dst_rel = f"Pack/Actor/{actor}.pack.zs"
        write(os.path.join(out_romfs, dst_rel), dicts.compress(pack, PACK_DICT))
        if player not in ai_rows:
            raise BuildError(f"ActorInfo has no row {player}")
        new_ai.append(info_row(ai_rows[player], actor, heaps.get(actor), sound, sound_user))
        if project:
            new_ai[-1]["ModelProjectName"] = project
        if wears:
            skin_row(new_ai[-1], wears[0], v)
        if char is not None:
            for key, size in characters["heaps"][v].items():
                old = new_ai[-1].get(key)
                size = max(int(size), int(_num(old or 0)))
                new_ai[-1][key] = type(old)(size) if isinstance(old, byml._Num) else size
        new_gai.append({"__RowId": actor})

        pack_size = ((len(pack) + 0x1F) & ~0x1F) + PACK_SIZE_MARGIN
        table.add_new(strip_zs(dst_rel), pack_size)
        chain_ap = 0
        for path, _ in actor_param_chain(members, player):
            size = table.get(path)
            if wears and wears[0].table is not None:
                # A mod's own file in the chain can be bigger than the game's (its table says so).
                size = max(size or 0, wears[0].table.get(path) or 0) or None
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
        emitter = None
        if emitter_path(actor) in names:
            src = ref_to_member(flatten_actor_param(members, player).get("PerceptionEmitterRef"))
            path, data, src = puppet_emitter(members, src, actor)
            # Sized as the player's emitter param it stands in for, as the footstep param.
            em_size = max((table.get(src) or 0) + max(0, len(data) - len(members[src])), len(data) + PACK_SIZE_MARGIN)
            table.add_new(path, em_size)
            emitter = {"param": path, "size": em_size}
        body_files = None
        if body or pvp_copy:
            body_files = {}
            for path, (data, src) in puppet_body(members, flatten_actor_param(members, player), actor,
                                                 sensors=pvp_copy).items():
                # Sized as the player's file it is cut from and every `$parent` merged into it (chain_entry).
                size = chain_entry(table, chain_paths(members, src), data)
                table.add_new(path, size)
                body_files[path] = size
        lift_file = None
        if lift_param and v in LIFTABLE_VARIANTS:
            path = liftable_path(actor, lift_param[0])
            # Sized as Ruto's file it is cut from and every `$parent` merged into it (chain_entry).
            size = chain_entry(table, lift_chain, byml.dumps(lift_param[1]))
            table.add_new(path, size)
            lift_file = {"param": path, "size": size}
            carry_anim = asb_carry.carry_animation_path(actor)
            if carry_anim in names:
                src = ref_to_member(flatten_actor_param(members, player).get("AnimationRef"))
                # Sized as the player's AnimationParam it is cut from, plus the margin.
                table.add_new(carry_anim, max(table.get(src) or 0, len(members.get(src, b""))) + PACK_SIZE_MARGIN)
                lift_file["carry"] = {"as": asb_carry.CARRY_AS, "animation": carry_anim}
        made.append({"actor": actor, "from": player, "perception": emitter, "quiet": quiet, "tunic": shade, "character": char, "model": project,
                     "skin": wears[0].id if wears else None, "pack": dst_rel, "pack_bytes": len(pack),
                     "instance_heap": _num(new_ai[-1].get("InstanceHeapSize", 0)), "param_size": ap_size,
                     "members": names, "equipment_keys": keys, "footstep": footstep, "body": body_files})
        if lift_file:
            made[-1]["liftable"] = lift_file  # (off: the manifest as before)
        if pvp_copy:
            pvp_files = {}
            for path, (data, src) in puppet_pvp(members, flatten_actor_param(members, player), actor,
                                                clients).items():
                # Sized as the file it is cut from and every `$parent` merged into it (chain_entry; the
                # sender is a file of our own beside EnemyCommon's).
                size = chain_entry(table, client_chains.get(src, [src]), data)
                table.add_new(path, size)
                pvp_files[path] = size
            made[-1]["pvp"] = pvp_files  # (off: the manifest as before)
        if char is None:
            log(f"{actor}: {len(names)} files, {len(pack)} bytes, {len(keys)} equipment keys -> {dst_rel}")
    if characters:
        log(f"characters: {sum(1 for m in made if m['character'] is not None)} puppets for {len(characters['files'])} "
            f"characters of the mod {characters['mod']} (CoopPuppet_<age>_C<file>, the mod's own model files)")

    src = os.path.join(romfs, "Pack", "Actor", f"{IMPACT_SOURCE}.pack.zs")
    template = None
    if os.path.isfile(src) and IMPACT_SOURCE in ai_rows:
        template = sarc.read(dicts.decompress(read(src))).get(IMPACT_TEMPLATE)
    if template is None:
        log(f"impacts: no {IMPACT_TEMPLATE} in this romfs, left out")

    shots = []
    # Stand-ins are model-only unless stand_in_effects (--seed-trail). With the seed's Tail effect
    # (an AI root and AIInfo from the impacts' template) the game dropped every create of
    # CoopShot_Seed ("request dropped", issue 198, every run from 2026-11-12 on; the model-only one
    # was made in every run before), as it drops the impact actors built the same way. Off until an
    # effect actor is seen made in a game.
    fx_template = template if stand_in_effects else None
    if template is not None and not stand_in_effects:
        log("stand-in effects (seed trail): left out (--seed-trail to try them)")
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

    # The title screen's Host, Join and Co-op Settings rows (title_menu.py): left out on any trouble,
    # the puppets do not need them.
    # Only for Hookshot's game (--title-rows): the emulator has no title menu, its rows would do nothing.
    # All or nothing (title_menu.build writes only when every file was made).
    # One view of the game's files for every UI builder (ui_pack.PackRun): what one made is what the next reads (the
    # language archives get both steps' labels), and each size-table entry grows once from the game's original.
    ui_run = ui_pack.PackRun(romfs, table)
    title_files = []
    if title_rows:
        try:
            title_files = title_menu.build(romfs, out_romfs, dicts, table,
                                           lambda root, r, data: write(os.path.join(root, r), data), recompress, log,
                                           run=ui_run)
        except (title_menu.TitleMenuError, OSError, ValueError, KeyError, IndexError, struct.error) as e:
            log(f"title menu: rows left out ({e}); the title keeps its own three, co-op uses the Minus prompt")
    # The pause menu's Multiplayer tab and page (pause_menu.py, issue 264): Hookshot's game only (--pause-tab), with
    # the co-op module that shows them (patch 0030); both stored hidden, so without the module the menu is the game's.
    # After the title rows, through the same view (ui_run): its labels join theirs in each language archive. All or
    # nothing.
    pause_files = []
    if pause_tab:
        try:
            pause_files = pause_menu.build(romfs, out_romfs, dicts, table,
                                           lambda root, r, data: write(os.path.join(root, r), data), recompress, log,
                                           run=ui_run)
        except (pause_menu.PauseMenuError, OSError, ValueError, KeyError, IndexError, struct.error) as e:
            log(f"pause menu: the Multiplayer tab left out ({e}); the pause menu keeps its four")

    if characters:
        # The mod's own rows first, the pack's own over them, then the player's own Link's heaps (as the mod alone).
        mod_ai = character_mod_rows(characters, rel(romfs, ai_path), dicts, ai_rows, log)
        mod_gai = character_mod_rows(characters, rel(romfs, gai_path), dicts, {r["__RowId"]: r for r in gai}, log)
        players = character_player_rows(characters, {**ai_rows, **{r["__RowId"]: r for r in mod_ai}})
        new_ai = mod_ai + new_ai + players
        new_gai = mod_gai + new_gai
        log(f"characters: {len(mod_ai)} ActorInfo and {len(mod_gai)} GameActorInfo rows of {characters['mod']}'s own "
            f"kept; the player's own Link: " + ", ".join(
                f"{r['__RowId']} {_num(r.get('InstanceHeapSize', 0))}/{_num(r.get('PreActorRenderInfoHeapSize', 0))}"
                for r in players))
    ai_new = byml.dumps(upsert_rows(ai, new_ai))
    gai_new = byml.dumps(upsert_rows(gai, new_gai))
    for path, old, new in ((ai_path, ai_old, ai_new), (gai_path, gai_old, gai_new)):
        key = strip_zs(rel(romfs, path))
        size = table.get(key)
        if size is None:
            raise BuildError(f"the size table has no {key}")
        table.set(key, size + max(0, len(new) - len(old)) + PACK_SIZE_MARGIN)
    if characters:
        n = merge_character_table(table, characters, rel(romfs, rs_path), dicts, log)
        log(f"characters: {n} size-table entries from {characters['mod']} (its own table and model files)")
    for path, new in ((ai_path, ai_new), (gai_path, gai_new), (rs_path, table.dumps())):
        r = rel(romfs, path)
        write(os.path.join(out_romfs, r), recompress(dicts, read(path), new))
        log(f"wrote {r}")

    guards = guard_scenes(romfs, dicts) if seen_by else {}
    quiet_scenes = sorted(guards)
    if seen_by:
        log(f"quiet puppets (no perception) in {len(quiet_scenes)} scenes with stealth guards: {', '.join(quiet_scenes)}")
    manifest = {
        "tool": "tools/puppet_pack",
        "title_id": TITLE_ID,
        "actors": made,
        "shots": shots,
        "impacts": impacts,
        "tunics": tunic_files,
        "skins": [entry for _, _, entry in skin_list],
        "test_skin": worn[0].id if worn else None,
        "self_tiles": self_tiles,
        "self_dark_tiles": self_dark_tiles,
        "self_files": self_files,
        "look_files": look_files,
        "look_items": look_item_files,
        "blade_items": blade_files,
        "tunics_off": tunics_off,
        "characters": ({"mod": characters["mod"], "numbers": {str(n): characters["numbers"][n] for n in sorted(characters["files"])},
                        "variants": {str(n): characters["files"][n] for n in sorted(characters["files"])},
                        "colours": characters.get("colours")} if characters else None),
        "test_tunic": test_tunic,
        "sound": sound,
        "footsteps": "player" if player_footsteps or not sound else "puppet",
        "perception": bool(seen_by),
        "body": bool(body),
        "quiet_scenes": quiet_scenes,
        "guards": guards,
        "sound_user": sound_user if sound and sound_user else None,
        "xlink": "player" if sound and not basic_xlink else "basic",
        "tables": [rel(romfs, p) for p in (ai_path, gai_path, rs_path)],
        "title_rows": bool(title_rows),  # asked for (--title-rows); title_menu: what was made
        "title_menu": title_files,
        "pause_menu": pause_files,
        "source_sha256": {rel(romfs, p): hashlib.sha256(read(p)).hexdigest()
                          for p in (ai_path, gai_path, rs_path)},
    }
    if lift_param:
        manifest["liftable"] = True  # --puppet-liftable (off: the manifest as before)
    if clients:
        manifest["pvp"] = True  # --puppet-pvp (off: the manifest as before)
    scenes, rooms, points = dungeon_rooms.write_table(romfs, os.path.join(mod_dir, "coop_rooms.txt"), dicts)
    log(f"coop_rooms.txt: {points} placements in {rooms} rooms of {scenes} dungeons")
    manifest["rooms"] = {"scenes": scenes, "rooms": rooms, "placements": points}
    maps = map_captures.write_table(romfs, os.path.join(mod_dir, "coop_maps.txt"))
    log(f"coop_maps.txt: {maps} maps (players on the minimap and the pause map)")
    manifest["maps"] = maps
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
    ap.add_argument("--perception", action="store_true",
                    help="the default now: puppets the game's perception sees (enemies go for every player), with "
                         "quiet copies for the stealth guards' scenes (issue 150); kept so older commands still work")
    ap.add_argument("--no-perception", action="store_true",
                    help="puppets no perception sees (enemies go only for each game's own Link; the pack before issue 150)")
    ap.add_argument("--puppet-body", action="store_true",
                    help="native puppets phase 1 test (docs/native-puppets.md): every puppet gets a body of its own "
                         "(Link's shapes and character controller, no sensors), off by default")
    ap.add_argument("--puppet-liftable", action="store_true",
                    help="native puppets phase 3 test (docs/native-puppets.md): every young (child) puppet gets Ruto's "
                         "LiftableRef, so Link can lift and carry it as Ruto (with LICHII_COOP_LIFT_PLAYER=1); "
                         "adult puppets never; off by default")
    ap.add_argument("--puppet-pvp", action="store_true",
                    help="native puppets phase 4 test (docs/native-puppets.md section 5): every puppet gets a _Pvp "
                         "copy (a body with Link's Tgt sensor, his SensorHit/Life/Reaction/Target params, a LockOn "
                         "and AutoAim attention sender), used with LICHII_COOP_PVP_NATIVE=1; off by default")
    ap.add_argument("--no-sound", action="store_true",
                    help="build the puppets without Link's sounds (the pack before issue 131)")
    ap.add_argument("--seed-trail", action="store_true",
                    help="test switch: the seed stand-in also plays the seed's Tail trail (issue 198; the "
                         "game dropped every such create so far, so off by default)")
    ap.add_argument("--no-seed-trail", action="store_true",
                    help="the default now (stand-ins model-only); kept so older commands still work")
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
    ap.add_argument("--title-rows", action="store_true",
                    help="Hookshot's game: Host, Join and Co-op Settings as rows of the title (title_menu.py)")
    ap.add_argument("--pause-tab", action="store_true",
                    help="Hookshot's game: the pause menu's Multiplayer tab and page (pause_menu.py; patch 0030's module)")
    ap.add_argument("--skin", action="append", default=[], metavar="ID=FOLDER",
                    help="a character model mod (its folder, holding romfs) to make puppets of, "
                         "CoopPuppet_<age>_Skin_<ID> (skin.py); may be given more than once")
    ap.add_argument("--skin-effects", action="store_true",
                    help="the --skin mods' own copies of the game's effect files (Effect/, ELink2/: a recoloured "
                         "sword glow) go into the pack as they are. This changes every Master Sword in your game "
                         "(and every sword with that effect), worn or not, solo too")
    ap.add_argument("--test-skin", metavar="ID",
                    help="test switch: every puppet wears the --skin of this id (a hook that knows no skins)")
    ap.add_argument("--look-slots", type=int, default=look_items.SLOTS, metavar="N",
                    help="how many players' puppets wear gear colours of their own (look_items.py; 0 to 4, 0: none)")
    ap.add_argument("--blade-copies", type=int, default=look_items.BLADE_COPIES, metavar="N",
                    help="copies of each sword per player number, so every puppet's blade is its own "
                         "(look_items.is_wearer_item; 0: none, puppets wear the game's items)")
    ap.add_argument("--no-characters", action="store_true",
                    help="no puppets for the characters of an installed Character Select mod (find_characters)")
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
                menus = nup_menu_files(n) if (a.title_rows or a.pause_tab) else []
                for rel in list(dumpcheck.nup_files(n).values()) + nup_scene_files(n) + menus:
                    nup.write_under(tmp, rel, n.read(rel))
                # The gear colours' sources (with the dictionaries just taken out, to read the items' packs).
                for rel in nup_look_files(n, zs.Dicts.from_romfs(tmp)):
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


def _reader_has(reader, path):
    """Whether a hud reader's RomFS has exactly this file (no pattern: a name with `[` or `*` is a name)."""
    d, _, name = path.rpartition("/")
    return name in reader.files(d)


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
    skins = []
    for sk in a.skin:
        sid, sep, folder = sk.partition("=")
        if not sep or not sid or not folder:
            print(f"error: --skin {sk}: want ID=FOLDER", file=sys.stderr)
            return 2
        skins.append((sid, folder))
    try:
        tunics = [] if a.tunics.strip().lower() == "none" else [c.strip() for c in a.tunics.split(",") if c.strip()]
        build(romfs, mod_dir, heaps=heaps, sound=not a.no_sound, player_footsteps=a.player_footsteps,
              sound_user=a.sound_user, tunics=tunics, test_tunic=a.test_tunic,
              stand_in_effects=a.seed_trail and not a.no_seed_trail, basic_xlink=a.basic_xlink,
              title_rows=a.title_rows, pause_tab=a.pause_tab, look_slots=max(0, min(a.look_slots, look_items.MAX_SLOTS)),
              perception=not a.no_perception,
              blade_copies=max(0, min(a.blade_copies, look_items.BLADE_COPIES)), skins=skins, test_skin=a.test_skin,
              skin_effects=a.skin_effects,
              game_has=(lambda path: _reader_has(hud_reader, path)) if hud_reader is not None else None,
              game_read=hud_reader.read if hud_reader is not None else None,
              body=a.puppet_body, liftable=a.puppet_liftable, pvp=a.puppet_pvp,
              characters=None if a.no_characters else find_characters(mod_dir))
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
