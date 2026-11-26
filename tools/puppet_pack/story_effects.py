"""Writes lichii/hook/coop_story_effects.inc: what a shared story event sets in the game that plays it.

    python -m puppet_pack.story_effects --romfs <extracted romfs> --out lichii/hook/coop_story_effects.inc

Row 129 (GitHub #173): co-op shares a story event's played flag (`HavePlayedEvent.<setting>`, coop_story.cpp
and coop_dungeon.cpp), so a game behind the group never runs that event: the game counts it as played. Its
flow's own writes then never happen in that game. The owl in Hyrule Field (HyruleField_Goto_HyruleCastle_Kaepora)
is the only setter of HyruleField_ArriveEntrance, and the game's clock stays stopped until that flag is 1
(docs/time.md, "Clock gate"): a player who joined after the host met the owl never had a running clock.

For every shared played flag this reads the setting's event flow (RSDB EventSetting: its flow and entry, then
subflows, forks and the settings it plays by EventTransition, puppet_pack/evfl.py) and keeps each
EventTriggerSetGameDataBool on its unconditional path (not behind a switch) whose flag
  - is a saved Bool (GameDataList ResetTypeValue 8) whose default is the other value,
  - is not shared itself (named in coop_story.cpp, coop_dungeon.cpp, coop_quests.cpp, coop_chests.cpp or
    coop/host/src/game_items.cpp: the sync carries those),
  - is written one way only: every SetGameDataBool of it in every event flow (Event/EventFlow) sets that
    value, and no AI graph (Logic, AI, Sequence .ainb) has a node that sets it otherwise (a node type with
    `Set` naming it, other than TriggerGameDataSetBool<True|False> of the same value; reads are fine),
  - is not asked by a switch of the same walk (its value would change what the flow does if it were set early).
Such a flag is the game's own invariant: once the event has played, it holds that value for good. Each game
sets it through the game's own Bool setter (lichii/hook/coop_hook.cpp MendStory, co-op idle too). Everything
else the walk finds is written into the file as a comment with why it is left out.

Only flag and event names, and their Murmur3 hashes, go in the file.
"""
import argparse
import glob
import os
import re
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml, evfl, zs  # noqa: E402
from .boss_beaten import murmur3  # noqa: E402
from .eye_switches import read_graph  # noqa: E402

SAVED = 8  # GameDataList ResetTypeValue of a saved flag
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
PLAYED_SOURCES = ("lichii/hook/coop_story.cpp", "lichii/hook/coop_dungeon.cpp")
SHARED_SOURCES = PLAYED_SOURCES + ("lichii/hook/coop_quests.cpp", "lichii/hook/coop_chests.cpp",
                                   "coop/host/src/game_items.cpp")
SET_BOOL = "EventTriggerSetGameDataBool"


def quoted_names(text):
    """Every flag name quoted in a source file's tables."""
    return set(re.findall(r'"([A-Za-z0-9_.]+)"', text))


def played_settings(texts):
    """The settings whose played flag is shared: `HavePlayedEvent.<setting>` named in the sources."""
    out = set()
    for t in texts:
        out |= {n.split(".", 1)[1] for n in quoted_names(t) if n.startswith("HavePlayedEvent.")}
    return out


def walk(flows, flow, entry, load, transition):
    """[(actor, action, params, conditional)] every action an entry can run, like evfl.actions_of, with
    `conditional` true when a switch comes before it on the way there (any switch: its cases may skip it)."""
    out, seen = [], set()
    todo = [(flow, entry, False)]
    while todo:
        f, e, cond = todo.pop()
        if (f, e, cond) in seen:
            continue
        seen.add((f, e, cond))
        chart = flows.get(f)
        if chart is None:
            chart = load(f)
            if chart is not None:
                flows[f] = chart
        if chart is not None and not e and chart.entries:
            e = "EntryPoint0" if "EntryPoint0" in chart.entries else next(iter(chart.entries))
        if chart is None or e not in chart.entries:
            continue
        stack, visited = [(chart.entries[e], cond)], set()
        while stack:
            i, c = stack.pop()
            if i == evfl.NONE or (i, c) in visited or i >= len(chart.events):
                continue
            visited.add((i, c))
            ev = chart.events[i]
            if ev.type == 0:
                if ev.actor < len(chart.actors) and ev.action < len(chart.actors[ev.actor][2]):
                    actor, action = chart.actors[ev.actor][0], chart.actors[ev.actor][2][ev.action]
                    out.append((actor, action, ev.params, c))
                    target = ev.params.get("EventName") if action == "EventTransition" else None
                    if target and transition(target):
                        todo.append(transition(target) + (c,))
                stack.append((ev.next, c))
            elif ev.type == 1:
                if ev.actor < len(chart.actors) and ev.actor < len(chart.queries) \
                        and ev.query < len(chart.queries[ev.actor]):
                    out.append((chart.actors[ev.actor][0], chart.queries[ev.actor][ev.query], ev.params, c))
                stack.extend((k, True) for k in ev.cases)
            elif ev.type == 2:
                stack.extend((k, c) for k in ev.forks)
                stack.append((ev.join, c))
            elif ev.type == 3:
                stack.append((ev.next, c))
            elif ev.type == 4:
                todo.append((ev.flowchart or f, ev.entry, c))
                stack.append((ev.next, c))
    return out


def flow_writes(charts):
    """{flag: {value, ...}} every SetGameDataBool of every event flow."""
    out = {}
    for chart in charts:
        for ev in chart.events:
            if ev.type != 0 or ev.actor >= len(chart.actors) or ev.action >= len(chart.actors[ev.actor][2]):
                continue
            if "SetGameDataBool" in chart.actors[ev.actor][2][ev.action]:
                name = ev.params.get("GameDataName")
                if isinstance(name, str):
                    out.setdefault(name, set()).add(ev.params.get("Value"))
    return out


def graph_setters(graphs):
    """{flag: {("<file>: <node type>", value), ...}} the AI graph nodes that set a flag: a node type with `Set`
    that names it in a string input or parameter, with the value it sets (TriggerGameDataSetBoolTrue/False) or
    None when the type does not say. `graphs` is [(file, [node])] (eye_switches.read_graph)."""
    out = {}
    for name, nodes in graphs:
        for node in nodes:
            kind = node["type"]
            if "Set" not in kind:
                continue
            value = True if kind.endswith("SetBoolTrue") else False if kind.endswith("SetBoolFalse") else None
            for flag in list(node["imm"].values()) + list(node["sdef"].values()):
                out.setdefault(flag, set()).add((f"{name}: {kind}", value))
    return out


def classify(setting, actions, bools, shared, writes, setters):
    """([(flag, value)], [(flag, value, why)]) the effects kept and the Bool writes left out, for one setting's
    walked actions (`walk`). `bools`: {hash: (ResetTypeValue, default)}."""
    asked = {p.get("GameDataName") for _a, act, p, _c in actions if act.startswith("EventQuery")}
    kept, out = {}, {}
    for _actor, action, params, cond in actions:
        if action != SET_BOOL:
            continue
        flag, value = params.get("GameDataName"), params.get("Value")
        if not isinstance(flag, str) or not isinstance(value, bool) or flag in kept or flag in out:
            continue
        row = bools.get(murmur3(flag))
        why = ""
        if flag.startswith("HavePlayedEvent."):
            why = "a played flag (coop_story / coop_dungeon decide which are shared)"
        elif flag in shared:
            why = "shared: the sync carries it"
        elif cond:
            why = "behind a switch"
        elif row is None or row[0] != SAVED:
            why = "not a saved flag"
        elif row[1] == value:
            why = "already its default"
        elif writes.get(flag, set()) != {value}:
            why = "written both ways by the event flows"
        elif any(v is not value for _w, v in setters.get(flag, ())):
            why = "set by an AI graph (" + ", ".join(sorted(w for w, v in setters[flag] if v is not value)[:2]) + ")"
        elif flag in asked:
            why = "asked by a switch of the same event"
        if why:
            out[flag] = (flag, value, why)
        else:
            kept[flag] = (flag, value)
    return list(kept.values()), list(out.values())


def render(rows, left_out):
    lines = [
        "// Generated by tools/puppet_pack/story_effects.py from the game's RSDB EventSetting, event flows, AI",
        "// graphs and GameDataList, and the hook's shared lists; do not edit. Row 129 (GitHub #173): each saved",
        "// Bool a shared story event sets one way on its unconditional path, which the game that did not play it",
        "// (its played flag came from another game) sets through the game's own Bool setter (CoopStory::Effects).",
        "// {played flag hash, flag hash, value, setting, flag}",
        "constexpr Effect kEffects[] = {",
    ]
    for setting, flag, value in rows:
        lines.append(f'    {{0x{murmur3("HavePlayedEvent." + setting):08x}u, 0x{murmur3(flag):08x}u, '
                     f'{"true" if value else "false"}, "{setting}", "{flag}"}},')
    lines.append("};")
    lines.append("")
    lines.append("// Left out (setting: flag = value: why):")
    for setting, flag, value, why in left_out:
        lines.append(f"//   {setting}: {flag} = {int(value)}: {why}")
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--romfs", required=True, help="your own extracted game")
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    dicts = zs.Dicts.from_romfs(args.romfs)

    def read(*parts):
        paths = sorted(glob.glob(os.path.join(args.romfs, *parts)))
        if not paths:
            raise SystemExit(f"no {'/'.join(parts)} in the romfs")
        with open(paths[-1], "rb") as fh:
            return dicts.decompress(fh.read()) if paths[-1].endswith(".zs") else fh.read()

    texts = {}
    for src in SHARED_SOURCES:
        with open(os.path.join(ROOT, src), encoding="utf-8") as fh:
            texts[src] = fh.read()
    played = played_settings(texts[s] for s in PLAYED_SOURCES)
    shared = set().union(*(quoted_names(t) for t in texts.values()))
    settings = {r["__RowId"]: r for r in byml.loads(read("RSDB", "EventSetting.Product.*.rstbl.byml.zs"))}
    data = byml.loads(read("GameData", "GameDataList.Product.*.byml.zs"))["Data"]
    bools = {int(getattr(r["Hash"], "v", r["Hash"])): (r.get("ResetTypeValue"), bool(r.get("DefaultValue"))) for r in data["Bool"]}

    flows = {}

    def load(name):
        path = os.path.join(args.romfs, "Event", "EventFlow", name + ".bfevfl.zs")
        if not os.path.exists(path):
            return None
        try:
            return evfl.parse(dicts.decompress(open(path, "rb").read()))
        except Exception:  # a timeline only: not read (none of the shared ones sets a flag)
            return None

    def flow_of(setting):
        row = settings.get(setting)
        if row is None:
            return None
        name = (row.get("FlowchartFileName") or setting).split("/")[-1].rsplit(".", 1)[0]
        return name, row.get("EntryPointName") or ""

    every = [c for c in (load(os.path.basename(p)[:-len(".bfevfl.zs")]) for p in
             sorted(glob.glob(os.path.join(args.romfs, "Event", "EventFlow", "*.bfevfl.zs")))) if c is not None]
    writes = flow_writes(every)
    graphs = []
    for folder in ("Logic", "AI", "Sequence"):
        for path in sorted(glob.glob(os.path.join(args.romfs, folder, "*.ainb*"))):
            raw = open(path, "rb").read()
            raw = dicts.decompress(raw) if path.endswith(".zs") else raw
            try:
                graphs.append((os.path.basename(path).split(".")[0], read_graph(raw)[0]))
            except Exception as ex:
                raise SystemExit(f"{path}: not read ({ex})")
    setters = graph_setters(graphs)

    rows, left_out = [], []
    for setting in sorted(played):
        where = flow_of(setting)
        if where is None:
            continue
        actions = walk(flows, where[0], where[1], load, flow_of)
        kept, out = classify(setting, actions, bools, shared, writes, setters)
        rows += [(setting, f, v) for f, v in kept]
        left_out += [(setting, f, v, why) for f, v, why in out]
    with open(args.out, "w", newline="\n") as fh:
        fh.write(render(rows, left_out))
    print(f"{len(rows)} effects from {len(played)} shared played flags; {len(left_out)} Bool writes left out")


if __name__ == "__main__":
    main()
