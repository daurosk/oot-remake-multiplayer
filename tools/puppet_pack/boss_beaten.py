"""Writes lichii/hook/coop_boss_beaten.inc: each dungeon boss's "beaten set", read from the game's own romfs.

D9 (docs/sync-coverage.md) keeps a boss's end state each game's own: every game in the boss's area runs the boss's
own death setting itself, which sets that game's flags, quest step and warp as a solo kill does. A game that did not
(its player was elsewhere, or an old build) still holds the boss alive, while the shared heart container is taken:
a reload brings the fight back (player report #241). After the group's boss death such a game is given the boss's
whole beaten set, one way, at a scene load outside the boss's scene (lichii/hook/coop_boss_beaten.h).

The set is what the boss's death setting writes in the dungeon (not the boss rush, not the dream world):
  - each saved Bool its flow sets (EventTriggerSetGameDataBool), with the value it sets;
  - HavePlayedEvent of the setting itself, of every event it plays (EventTransition: Dm_<Dungeon>_BossOut) and of
    the boss's intro (Dm_<Dungeon>_BossIn, played before any death);
  - the dungeon quest's step it moves to (EventExecuteProgressChallengeStep): the step right after one of the quest's
    boss steps (RSDB Challenge `IsBossBattle`: a dungeon boss's is its quest's last, the earlier ones are mid-bosses:
    Big Octo, the Forest Temple's; Defeat_Ganon has Ganondorf's Step05 and Ganon's Step08/Step09),
    raised to only from a step before it (never from Complete: a save that
    finished the dungeon is never stepped back, docs/dungeons-boss.md "boss_ours").
Left out: flags that are not saved, and the fight's own state that co-op never shares and the stale-flag mend clears
(CoopDungeon::NotShared: the shut boss door, Dodongo's and Volvagia's arena floor).

The boss rooms' own logic reads these (checked 2026-11-16 in Logic/*_Level_Room_*: Gohma_DefeatBoss and
Get_StoneOfForest_IsAfter_GoToWarpPoint_Exp; Dodongo's HavePlayedEvent.Dm_DodongoCave_BossOut and
Enemy_DungeonBoss_Dodongo_BossAppear; the other six their quest's IsAfter_<step>_Exp), so the room loads as a
beaten save's room: no boss, the blue warp there.

    python -m puppet_pack.boss_beaten --romfs <extracted romfs> --out lichii/hook/coop_boss_beaten.inc

Only flag names, their hashes and step names go in the file (as coop_quests.cpp and coop_dungeon.cpp hold).
"""
import argparse
import os
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml, evfl, zs  # noqa: E402

# CoopBossScenes kBosses 0 to 9 (lichii/hook/coop_boss_scenes.cpp): the boss index (the room for 0 to 7), the boss, its death setting
# (outro_asks[0]), its intro event and its death event. tests/test_boss_beaten.py checks this against that file.
BOSSES = [
    (0, "Queen Gohma", "InsideTheDekuTree_BossDie", "Dm_Deku_BossIn", "Dm_Deku_BossOut"),
    (1, "King Dodongo", "Get_StoneOfFire_DefeatBoss", "Dm_DodongoCave_BossIn", "Dm_DodongoCave_BossOut"),
    (2, "Barinade", "Get_StoneOfWater_DestroyBoss", "Dm_InsideJabu_Jabu_BossIn", "Dm_InsideJabu_Jabu_BossOut"),
    (3, "Phantom Ganon", "ForestTemple_SageOfForest_PhantomGanonDefeat", "Dm_ForestTemple_BossIn",
     "Dm_ForestTemple_BossOut"),
    (4, "Volvagia", "SageOfFire_DefeatBoss", "Dm_FireTemple_BossIn", "Dm_FireTemple_BossOut"),
    (5, "Morpha", "Demo_SageOfWater_BossDefeat", "Dm_WaterTemple_BossIn", "Dm_WaterTemple_BossOut"),
    (6, "Bongo Bongo", "ShadowTemple_BossWin", "Dm_ShadowTemple_BossIn", "Dm_ShadowTemple_BossOut"),
    (7, "Twinrova", "SageOfSpirit_BossEnd", "Dm_SpiritTemple_BossIn", "Dm_SpiritTemple_BossOut"),
    # 8 and 9: no room in CoopBoss::Rooms() (coop_boss_scenes.cpp has -1); the index is BOSS_SCENE's boss. Defeat_Ganon is
    # each game's own (coop_quests.cpp): Ganondorf's death moves it to Step06 (after Step05, a boss step), Ganon's to
    # Complete (after Step09). EscapeFromCrumblingGanonCastle between them is read only in CrumblingGanonCastle's own
    # logic (Logic/CrumblingGanonCastle_*), which a game given Step06 or Complete never needs: not in either set.
    (8, "Ganondorf", "InsideGanonCastle_Dungeon_BossDie_1", "Dm_InsideGanonCastle_BossIn",
     "Dm_InsideGanonCastle_BossOut"),
    (9, "Ganon", "InsideGanonCastle_Dungeon_BossDie_2", "Dm_FinalBattle_BossIn", "Dm_FinalBattle_BossOut"),
]

# CoopDungeon::NotShared() (lichii/hook/coop_dungeon.cpp); the test checks the two lists match.
NOT_SHARED = {
    "InsideTheDekuTree_BossArea_ShutIn", "InsideTheDekuTree_BossArea_ChangeENVForBoss",
    "ShadowTemple_Level_StartBossBattle", "InsideGanonCastle_IsEnterBossRoom",
    "DodongoCavern_BossGroundChange", "FireTemple_BossGroundChange",
}

SAVED = 8  # GameDataList ResetTypeValue of a saved flag (coop_story.cpp)


def murmur3(text, seed=0):
    """The game's flag hash (coop::protocol::flag_hash): Murmur3 x86 32-bit of the name."""
    data = text.encode("utf-8")
    c1, c2, m = 0xcc9e2d51, 0x1b873593, 0xffffffff
    h, n = seed, len(data) & ~3
    for i in range(0, n, 4):
        k = int.from_bytes(data[i:i + 4], "little")
        k = (k * c1) & m
        k = ((k << 15) | (k >> 17)) & m
        h ^= (k * c2) & m
        h = ((h << 13) | (h >> 19)) & m
        h = (h * 5 + 0xe6546b64) & m
    k = 0
    for j, b in enumerate(data[n:]):
        k |= b << (8 * j)
    if len(data) & 3:
        k = (k * c1) & m
        k = ((k << 15) | (k >> 17)) & m
        h ^= (k * c2) & m
    h ^= len(data)
    h ^= h >> 16
    h = (h * 0x85ebca6b) & m
    h ^= h >> 13
    h = (h * 0xc2b2ae35) & m
    return h ^ (h >> 16)


def _num(v):
    return int(getattr(v, "v", v))


def normal_world(_actor, query, params):
    """The dungeon's own path: not the dream world, not the boss rush (0 is true, evfl.actions_of)."""
    if query == "EventQueryIsDreamWorld":
        return 1
    if query == "EventQueryIsGameDataBool" and str(params.get("GameDataName", "")).startswith("DreamWorld_BossRush"):
        return 1
    return None


def beaten_set(boss, actions, game_bools, quests):
    """One boss's set from its death setting's walked actions. `game_bools`: {hash: ResetTypeValue} of the
    GameDataList Bools; `quests`: {quest name: (enum hash, [NotReady, steps..., Complete], [boss step indexes])}."""
    room, name, death, boss_in, boss_out = boss
    bools = {}  # name -> value, the last write wins

    def add(flag, value):
        if flag in NOT_SHARED or game_bools.get(murmur3(flag)) != SAVED:
            return
        bools.pop(flag, None)
        bools[flag] = value

    step = None
    played = [death, boss_in]
    for _actor, action, params in actions:
        if action == "EventTriggerSetGameDataBool" and isinstance(params.get("Value"), bool):
            add(params["GameDataName"], params["Value"])
        elif action == "EventTransition" and params.get("EventName"):
            played.append(params["EventName"])
        elif action == "EventExecuteProgressChallengeStep":
            if step is not None and step != (params["ChallengeName"], params["StepName"]):
                raise ValueError(f"{name}: two quest steps in its death: {step} and {params}")
            step = (params["ChallengeName"], params["StepName"])
    if boss_out not in played:
        raise ValueError(f"{name}: its death setting {death} does not play {boss_out}")
    for event in played:
        add("HavePlayedEvent." + event, True)
    if step is None:
        raise ValueError(f"{name}: its death setting {death} moves no quest")
    quest, step_name = step
    enum, steps, boss_steps = quests[quest]
    at = steps.index(step_name)
    if at - 1 not in boss_steps:
        raise ValueError(f"{name}: {quest} step {step_name} is not the one after a boss step "
                         f"({', '.join(steps[i] for i in boss_steps)})")
    return {
        "room": room, "boss": name, "quest": quest, "quest_hash": enum, "step": step_name,
        "below": steps[:at], "bools": list(bools.items()),
    }


def read_romfs(romfs):
    dicts = zs.Dicts.from_romfs(romfs)

    def table(path):
        with open(os.path.join(romfs, path), "rb") as f:
            return byml.loads(dicts.decompress(f.read()))

    settings = {r["__RowId"]: r for r in table("RSDB/EventSetting.Product.111.rstbl.byml.zs")}
    data = table("GameData/GameDataList.Product.110.byml.zs")["Data"]
    game_bools = {_num(r["Hash"]): r.get("ResetTypeValue") for r in data["Bool"]}
    quests = {}
    for row in table("RSDB/Challenge.Product.111.rstbl.byml.zs"):
        qname = row["__RowId"].split("/")[-1].split(".")[0]
        steps = ["NotReady"] + [s["Name"] for s in row["Steps"]] + ["Complete"]
        bosses = [i + 1 for i, s in enumerate(row["Steps"]) if s.get("IsBossBattle")]
        enums = [e for e in data["Enum"] if list(e.get("RawValues", [])) == steps]
        if bosses and len(enums) == 1:
            quests[qname] = (_num(enums[0]["Hash"]), steps, bosses)
    flows = {}

    def load(fname):
        path = os.path.join(romfs, "Event", "EventFlow", fname + ".bfevfl.zs")
        if not os.path.exists(path):
            return None
        with open(path, "rb") as f:
            return evfl.parse(dicts.decompress(f.read()))

    out = []
    for boss in BOSSES:
        row = settings[boss[2]]
        fname = row["FlowchartFileName"].split("/")[-1].rsplit(".", 1)[0]
        actions = evfl.actions_of(flows, fname, row["EntryPointName"], load=load, answer=normal_world)
        out.append(beaten_set(boss, actions, game_bools, quests))
    return out


def render(sets):
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Generated by tools/puppet_pack/boss_beaten.py from the game's romfs (its death settings' event flows,",
        "// RSDB Challenge and GameDataList). Do not edit: run the tool again. Read by coop_boss_beaten.cpp.",
        "",
    ]
    for s in sets:
        if not s["bools"] or not s["below"]:
            raise ValueError(f"{s['boss']}: an empty set (a C array cannot be empty)")
        r = s["room"]
        below = ", ".join(f"0x{murmur3(n):08x}u" for n in s["below"])
        lines.append(f"// {s['boss']}: {s['quest']} below {s['step']}: " + ", ".join(s["below"]))
        lines.append(f"constexpr std::uint32_t kBelow{r}[] = {{{below}}};")
        lines.append(f"constexpr BeatenBool kBools{r}[] = {{")
        for flag, value in s["bools"]:
            lines.append(f"    {{0x{murmur3(flag):08x}u, {'true' if value else 'false'}, \"{flag}\"}},")
        lines.append("};")
    lines.append("constexpr BeatenSet kSets[] = {")
    for s in sets:
        r = s["room"]
        lines.append(f"    {{{r}, \"{s['boss']}\", 0x{s['quest_hash']:08x}u, \"{s['quest']}\", "
                     f"0x{murmur3(s['step']):08x}u, \"{s['step']}\", kBelow{r}, kBools{r}}},")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    sets = read_romfs(a.romfs)
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(render(sets))
    for s in sets:
        print(f"{s['boss']}: {s['quest']} -> {s['step']}; " + ", ".join(f"{n}={int(v)}" for n, v in s["bools"]))


if __name__ == "__main__":
    main()
