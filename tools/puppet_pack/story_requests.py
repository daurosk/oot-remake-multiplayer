"""Writes lichii/hook/coop_story_requests.inc: which event settings start a shared story cutscene.

    python -m puppet_pack.story_requests --romfs <romfs> --out lichii/hook/coop_story_requests.inc

The game asks for an event by its event setting (RSDB/EventSetting: a name, the flow and the entry
it plays) and keeps "already played" per setting (`HavePlayedEvent.<setting>`). Most shared story
cutscenes (`kNames` in lichii/hook/coop_shared_scenes.cpp) are a setting of their own name. Many
settings play a story flow under another name: the Great Deku Tree's meeting is the setting
`KokiriForest_Meet_DekuTree_MeetDekuTree_01` of the flow Meet_DekuTree (player report 110,
docs/scenes.md "Shared story cutscenes"), Darunia's dance `Meet_Darunia_PlayOcarina` one of
Get_StoneOfFire, the Fire Temple's freed Gorons `Dm_FireTemple_0014`..`_0022` entries of
Dm_FireTemple_0020.

Every named setting (not a numeric row id: those are talks) whose flow is a story name and whose own
name is not that flow's is classified here from the game's data, first rule that holds:
  boss     its components are BossDie, or `Boss` is in its name or entry: the boss scenes have it
           (lichii/hook/coop_boss_scenes.cpp, docs/dungeons-boss.md);
  gimmick  a dungeon live event (a `HavePlayedEvent.<name>` of lichii/hook/coop_dungeon.cpp or a
           place of lichii/hook/coop_live_places.cpp): the dungeon path shares it;
  own      started inside a talk of the player's own (components NPCTalk*, or an entry Talk*):
           another game has no talk to continue;
  own      played where the player is, on that player's own arrival (base PlayerPos);
  own      it changes what is the player's own (docs/items.md "Per player"), read from the event flow
           its entry runs (subflows, switches, forks, and the settings it goes on to by EventTransition,
           which a game asked for it plays too; puppet_pack/evfl.py): an item taken from the pouch
           (a trade), a consumable added, rupees, a bottle's contents, the player's ocarina input awaited,
           gear equipped from the pouch, a shop, a chest opened. A +1 of an item co-op shares, a get
           window or a shown
           item is not. A movie with no flowchart (a timeline only) is not read;
  room     it turns a room's own state round in the game that plays it (room_turn): a dungeon gimmick
           whose flow toggles the room (the Forest Temple's twisted corridor, row 173), which a joining
           game would turn once more, and under its player;
  story    a `Dm_` cutscene entry of a `Dm_` story flow, or a name that carries its flow's name as
           a whole part (`Get_StoneOfWater_ShowLetter`): shared, sent and joined by its name;
  never    anything else: a room or object event the flow's file also holds (a door, a hole).
The `story` ones go in kStorySettings, the rest in comments with their reason. The same reading of
the event flow is done for the story names themselves: one that changes the player's own things
(Zelda's letter, a reward, a pouch content) goes in kOwnStory with why, and is neither sent nor
joined: another game would be given it too; one that turns a room's own state (room_turn) goes in
kRoomStory with why, likewise neither sent nor joined (the room's state comes by its held flags). It also writes
`kNoSetting`, the story names with no setting: no game can be asked for them, so they are neither
sent nor joined. Every story setting and every other story name must have its played flag in
GameData/GameDataList.Product.*.byml.zs, else the tool stops.
Names only: nothing of the game's data but its own event and scene names.
"""
import argparse
import glob
import os
import re
import sys
from collections import namedtuple

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml, evfl, zs  # noqa: E402

HOOK = os.path.join(os.path.dirname(__file__), "..", "..", "lichii", "hook")

Setting = namedtuple("Setting", "flow entry base components")


def story_names(cpp_text):
    """The quoted names of `kNames[] = { ... };` in coop_shared_scenes.cpp."""
    m = re.search(r"kNames\[\]\s*=\s*\{(.*?)\};", cpp_text, re.S)
    if not m:
        raise ValueError("no kNames table")
    return set(re.findall(r'"([^"]+)"', m.group(1)))


def gimmick_names(dungeon_cpp, live_places_cpp):
    """The dungeon live events: `HavePlayedEvent.<name>` in coop_dungeon.cpp, places' names in coop_live_places.cpp."""
    out = set(re.findall(r'"HavePlayedEvent\.([A-Za-z0-9_]+)"', dungeon_cpp))
    out |= set(re.findall(r'\{"([A-Za-z0-9_]+)",\s*"[A-Za-z0-9_]+",\s*-?[0-9]', live_places_cpp))
    return out


def _stem(path):
    return os.path.basename(path or "").split(".")[0]


def event_settings(rows):
    """{setting name: Setting} from the rows of RSDB/EventSetting (a setting with no flow: flow "")."""
    return {
        r["__RowId"]: Setting(_stem(r.get("FlowchartFileName")), r.get("EntryPointName", "") or "",
                              _stem(r.get("EventBaseSetting")), _stem(r.get("EventSettingComponentList")))
        for r in rows
    }


def carries(name, flow):
    """Whether `name` holds `flow` as whole parts between underscores."""
    return f"_{flow}_" in f"_{name}_"


# What a game asked for a scene would change of that player's own (docs/items.md "Per player": rupees,
# ammo and other consumables, bottle contents, equipped gear). Items co-op shares (key items, stones,
# songs, medallions, the Ocarina, bag/quiver/wallet tiers: max/OR) are not: a +1 of one in the joining
# game gives nothing new, and a get window or a shown item alone changes nothing.
RUPEE_ACTIONS = ("IncreaseRupee", "DecreaseRupee", "SendRupee", "PayItemPrice", "IntToRupee")
# Other actions that change the player's own pouch or gear, with why (every inventory-changing action of the game's
# flows, listed 2026-11-16, G2 review). Not here: the item get's own plumbing every get passes (Dm_SY_Get's
# AddOwnToPouch and RecoverByCureItem, GetPouchContent's PushToEmptyBottle and get windows), heals.
OWN_ACTIONS = {
    "EventTriggerRemoveSwordBigGoronChallengeActor": "Biggoron's sword trade challenge",
    "EventTriggerRequestVanishExchangeItemAfterBancChanged": "a trade item vanishes",
    "EventTriggerRemoveOwnedMask": "a mask given back",
    "EventTriggerRemoveWeaponKeepSheath": "a weapon taken from the player",
    "EventTriggerRequestDeletePlayerEquipmentFromPouch": "gear deleted from the pouch",
    "EventPlayerSetItemForMiniGame": "a minigame's own items",
    "EventTriggertExchangeEnemyMuseumPouchAndState": "the pouch swapped (the enemy museum)",
    "EventTriggertExchangeEnemyMuseumPouchAndStateForContinuous": "the pouch swapped (the enemy museum)",
    "EventTriggertRestorePouchFromEnemyMuseum": "the pouch swapped back (the enemy museum)",
}
# Quests whose steps are each player's own, with why: a scene that moves one would move the joining game's quest
# for a race or trade it never played (row 150, the Epona quest: Dm_LonLon_0040, Ingo beaten, sets Get_Epona to
# AfterWonGambleRace; a friend who joined it in the ranch was left one step short of the end for good, and co-op's
# end-only sharing of Get_Epona counts that step as "riding out", so the group's Complete never came either).
OWN_QUESTS = {
    "Get_Epona": "the race against Ingo: co-op shares only the quest's end, lichii/hook/coop_quests.cpp",
}
CHALLENGE_STEP_ACTIONS = ("EventExecuteProgressChallengeStep", "EventExecuteRollbackChallengeStep")
OCARINA_INPUT = ("EventWaitOcarinaInput", "EventPlayerPlayOcarinaFree")  # waits for the player to play
CONSUMABLES = ("Item_Arrow", "Item_Bomb", "Item_DekuNut", "Item_DekuStick", "Item_Bullet", "Item_MagicBean",
               "Item_Rupee", "Obj_Rupee")  # their counts are per player (not a bag/quiver tier)


def touches_own(actions):
    """Why the actions an entry runs (evfl.actions_of) change what is the player's own, or "": an item
    taken from the pouch (AddOrRemovePouchContent with Num < 0: a trade) or a consumable added, rupees,
    a bottle's contents (any Bottle action but PushToEmptyBottle, which every item get passes), the
    player's ocarina input awaited, gear equipped from the player's pouch, a shop, a chest opened
    (Dm_SY_Treasurebox: the item comes over by the shared items anyway)."""
    for actor, action, params in actions:
        if actor in ("<subflow>", QUERY):
            continue  # a call, named "flow:entry"; the actions it runs are listed after it (G2 review: the
            # get window's OpenGetWindowForBottle_SubProcess, which every item get can reach, is only text)
            # a switch's question (QUERY) changes nothing
        if action == "EventTransition" and params.get("EventName") == "Dm_SY_Treasurebox":
            return "goes on to Dm_SY_Treasurebox (a chest the player opens)"
        if action == "EventTriggerAddOrRemovePouchContent":
            num, item = params.get("Num", 0), str(params.get("Actor", ""))
            if isinstance(num, int) and num < 0:
                return f"takes {item or 'an item'} from the pouch"
            if num > 0 and item.startswith(CONSUMABLES) and "Bag" not in item and "Quiver" not in item:
                return f"adds {item}, a consumable"
        elif action in OWN_ACTIONS:
            return f"runs {actor} {action} ({OWN_ACTIONS[action]})"
        elif action in CHALLENGE_STEP_ACTIONS and params.get("ChallengeName") in OWN_QUESTS:
            quest = params["ChallengeName"]
            step = params.get("StepName") or params.get("ChallengeStep") or "?"
            return f"moves {quest} to {step} ({OWN_QUESTS[quest]})"
        elif any(w in action for w in RUPEE_ACTIONS):
            return f"runs {actor} {action} (rupees)"
        elif "Bottle" in action and action != "EventTriggerPushToEmptyBottle" and actor != "UISystemActor":
            return f"runs {actor} {action} (a bottle's contents)"
        elif action.startswith(OCARINA_INPUT):
            return f"runs {actor} {action} (the player's ocarina)"
        elif "EquipDynamicEquipmentFromPouch" in action:
            return f"runs {actor} {action} (gear from the player's pouch)"
        elif actor == "Npc_EventStarter" and "Shop" in action:
            return f"runs {actor} {action} (a shop)"
    return ""


# Actions that turn a room's own shape in the game that runs them, with why (every room-changing action of the shared
# story flows, listed 2026-11-19, row 173: the flows' other warps move only the flow's own cast and the player).
ROOM_ACTIONS = {
    "EventTriggerRelocateTwistCorridor": "the twisted corridor turns round the player",
}
QUERY = "<query>"  # a switch's question in the walk (story_requests.main): (QUERY, query, params)


def room_turn(actions):
    """Why the actions an entry runs turn a room's own state, or "": a room action (ROOM_ACTIONS), or a flag the
    entry asks (a switch's EventQueryIsGameDataBool, given as (QUERY, query, params)) and sets both ways: a
    toggle, which a game joining the event turns once more (Dm_ForestTemple_0005 asks TwistedCorridor_A_02 and
    writes its opposite; row 173). A flag set to one value only (a first-time mark, a water level) is the same in
    every game that plays it."""
    asked, set_to = set(), {}
    for actor, action, params in actions:
        if action in ROOM_ACTIONS:
            return f"runs {actor} {action} ({ROOM_ACTIONS[action]})"
        if actor == QUERY and action == "EventQueryIsGameDataBool" and params.get("GameDataName"):
            asked.add(params["GameDataName"])
        elif actor == "GameDataActor" and action == "EventTriggerSetGameDataBool" and params.get("GameDataName"):
            set_to.setdefault(params["GameDataName"], set()).add(bool(params.get("Value")))
    toggled = sorted(f for f in asked if set_to.get(f) == {True, False})
    return f"toggles {', '.join(toggled)} (the room turns again in a game that joins)" if toggled else ""


def classify(name, s, gimmicks, actions=()):
    """(class, reason) of a named setting `s` of a story flow (module docstring); `actions` are those
    its entry runs (evfl.actions_of, through its transitions too: a game asked for it plays those as well)."""
    if s.components == "BossDie" or "Boss" in name or "Boss" in s.entry:
        return "boss", "a boss event: the boss scenes have it"
    if name in gimmicks:
        return "gimmick", "a dungeon live event"
    if s.components.startswith("NPCTalk") or s.entry.startswith("Talk"):
        return "own", "started in the player's own talk"
    if s.base == "PlayerPos":
        return "own", "played on the player's own arrival"
    own = touches_own(actions)
    if own:
        return "own", "changes the player's own things: " + own
    room = room_turn(actions)
    if room:
        return "room", "turns the room's own state: " + room
    if (name.startswith("Dm_") and s.flow.startswith("Dm_")) or carries(name, s.flow):
        return "story", ""
    return "never", "no story name in it: a room or object event"


def table(settings, story, gimmicks, actions_of=lambda flow, entry: ()):
    """[(setting, flow, class, reason)] sorted by flow and name: every named setting of a story flow
    under another name. `actions_of(flow, entry)` gives the actions an entry runs."""
    rows = []
    for name, s in settings.items():
        if s.flow in story and name != s.flow and not name[:1].isdigit():
            rows.append((name, s.flow) + classify(name, s, gimmicks, actions_of(s.flow, s.entry)))
    return sorted(rows, key=lambda r: (r[1], r[0]))


def own_story(story, settings, actions_of):
    """[(story name, why)] the story names that are a setting of their own and change the player's own
    things (touches_own of what their entry runs)."""
    out = []
    for n in sorted(story):
        s = settings.get(n)
        if s and s.flow:
            why = touches_own(actions_of(s.flow, s.entry))
            if why:
                out.append((n, why))
    return out


def room_story(story, settings, actions_of):
    """[(story name, why)] the story names that are a setting of their own, change none of the player's own
    things, and turn a room's own state (room_turn of what their entry runs)."""
    out = []
    for n in sorted(story):
        s = settings.get(n)
        if s and s.flow:
            acts = actions_of(s.flow, s.entry)
            why = "" if touches_own(acts) else room_turn(acts)
            if why:
                out.append((n, why))
    return out


def no_setting(story, settings):
    """The story names the game has no event setting of."""
    return sorted(n for n in story if n not in settings)


def murmur3(text, seed=0):
    """MurmurHash3 x86 32-bit, the game's hash of a flag name (coop::host::flag_hash)."""
    data = text.encode()
    h, c1, c2, m = seed, 0xCC9E2D51, 0x1B873593, 0xFFFFFFFF
    n = len(data) // 4
    for i in range(n):
        k = int.from_bytes(data[4 * i:4 * i + 4], "little")
        k = (k * c1) & m
        k = ((k << 15) | (k >> 17)) & m
        h ^= (k * c2) & m
        h = ((h << 13) | (h >> 19)) & m
        h = (h * 5 + 0xE6546B64) & m
    tail = data[4 * n:]
    if tail:
        k = int.from_bytes(tail, "little")
        k = (k * c1) & m
        k = ((k << 15) | (k >> 17)) & m
        h ^= (k * c2) & m
    h ^= len(data)
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & m
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & m
    h ^= h >> 16
    return h


def played_flag(name):
    """The flag the game sets once the setting `name` has played."""
    return "HavePlayedEvent." + name


def missing_played_flags(names, gamedata):
    """The names whose played flag's hash is not in `gamedata` (the decompressed, big-endian GameDataList)."""
    return [n for n in names if murmur3(played_flag(n)).to_bytes(4, "big") not in gamedata]


def render(rows, nosetting, ownstory=(), roomstory=()):
    lines = [
        "// Generated by tools/puppet_pack/story_requests.py from the game's RSDB EventSetting and GameDataList",
        "// and the hook's own lists; do not edit. Every named event setting that plays a shared story flow under",
        "// another name: the shared ones (sent and joined by that name, coop_shared_scenes.h StorySetting), then",
        "// the others with why, then the story names the game has no setting of.",
        "constexpr StorySetting kStorySettings[] = {",
    ]
    lines += [f'    {{"{name}", "{flow}"}},' for name, flow, cls, _ in rows if cls == "story"]
    lines.append("};")
    lines.append("// Not shared (a request of one is another event: the story search's find is refused):")
    lines += [f"//   {cls:7} {name} ({flow}): {why}" for name, flow, cls, why in rows if cls != "story"]
    lines.append("// Story names that change the player's own things (neither sent nor joined), and why:")
    lines.append("constexpr StoryOwn kOwnStory[] = {")
    lines += [f'    {{"{n}", "{why}"}},' for n, why in ownstory]
    lines.append("};")
    lines.append("// Story names that turn a room's own state (neither sent nor joined: the held flags bring it), and why:")
    lines.append("constexpr StoryOwn kRoomStory[] = {")
    lines += [f'    {{"{n}", "{why}"}},' for n, why in roomstory]
    lines.append("};")
    lines.append("constexpr std::string_view kNoSetting[] = {")
    lines += [f'    "{n}",' for n in nosetting]
    lines.append("};")
    return "\n".join(lines) + "\n"


def _text(name):
    with open(os.path.join(HOOK, name), encoding="utf-8") as fh:
        return fh.read()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--romfs", required=True, help="your own extracted game (holds RSDB and GameData)")
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    story = story_names(_text("coop_shared_scenes.cpp"))
    gimmicks = gimmick_names(_text("coop_dungeon.cpp"), _text("coop_live_places.cpp"))
    dicts = zs.Dicts.from_romfs(args.romfs)

    def read(*parts):
        paths = sorted(glob.glob(os.path.join(args.romfs, *parts)))
        if not paths:
            raise SystemExit(f"no {'/'.join(parts)} in the romfs")
        with open(paths[-1], "rb") as fh:
            return zs.maybe_decompress(dicts, fh.read())

    settings = event_settings(byml.loads(read("RSDB", "EventSetting.Product.*.rstbl.byml.zs")))
    flows = {}

    def load(flow):
        path = os.path.join(args.romfs, "Event", "EventFlow", flow + ".bfevfl.zs")
        if not os.path.exists(path):
            return None
        with open(path, "rb") as fh:
            return evfl.parse(zs.maybe_decompress(dicts, fh.read()))

    def transition(event):
        target = settings.get(event)
        return (target.flow, target.entry) if target and target.flow else None

    def actions(flow, entry):
        asked = []  # the switches' questions, for room_turn (every case is still followed)

        def answer(actor, query, params):
            asked.append((QUERY, query, params))

        return evfl.actions_of(flows, flow, entry, load, transition, answer) + asked

    rows = table(settings, story, gimmicks, actions)
    ownstory = own_story(story, settings, actions)
    roomstory = room_story(story, settings, actions)
    nosetting = no_setting(story, settings)
    gamedata = read("GameData", "GameDataList.Product.*.byml.zs")
    asked = [r[0] for r in rows if r[2] == "story"] + sorted(story - set(nosetting))
    missing = missing_played_flags(asked, gamedata)
    if missing:
        raise SystemExit("no played flag in the game for: " + ", ".join(played_flag(n) for n in missing))
    with open(args.out, "w", newline="\n") as fh:
        fh.write(render(rows, nosetting, ownstory, roomstory))
    shared = sum(1 for r in rows if r[2] == "story")
    print(f"{shared} story settings, {len(rows) - shared} not shared, {len(ownstory)} story names changing the "
          f"player's own things, {len(roomstory)} turning a room's own state, {len(nosetting)} story names with no setting")


if __name__ == "__main__":
    main()
