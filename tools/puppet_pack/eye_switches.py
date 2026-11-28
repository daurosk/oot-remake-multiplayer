"""Prints the switch table of lichii/hook/coop_eye_switch.cpp (kSwitches).

Run by a developer against their own romfs dump; nothing from the game is in
this tool:

    python -m puppet_pack.eye_switches --romfs <romfs> [--places lichii/hook/coop_live_places.cpp]
                                       [--dungeon lichii/hook/coop_dungeon.cpp] [--classify]

A room's gimmick event is asked by its EventRequestActor when the actor's
input `EventRequestTrigger` rises. That input is wired in the room's logic
graph (an AiGroup in Banc/<scene>/*.bcett: its own `Logic` and the nested
modules in its References), straight or through AND/OR gates and module
inputs, to a switch's output plug: `OnState` (eye, step, hit, hammer and dive
switches), `FirstOn`, `isHit` (the Forest Temple's silver eyes) or
`isLightMax` (sun switches). Another player's copy of the event, asked from
outside, plays its camera only: our asker never saw its trigger (deku-ladder,
tracker rows 90 and 102). So the hook sets that plug on OUR switch instead.

This tool follows the wiring (the graph's input links, its multi-input list,
and module inputs up to the calling graph) from every asker's trigger to the
switch plugs at its leaves, and prints one line per (event, switch): the event,
its scene, the switch's actor name and placement, the plug, and the asker's
placement. With --places only the events that have a live place (shared,
startable) are printed.

AI file layout read here (version 0x408), as far as needed:
  header +0x0c commands, +0x10 nodes, +0x24 string pool, +0x2c immediate
  parameters, +0x34 input/output parameters, +0x38 the multi-input list
  node 0x3c each after the commands (0x18 each): +0x08 type name, +0x14 body
  body: +0x18 the string immediates (index, count); +0x30 + 16*t the inputs
  (index, count) and outputs (index, count) of type t (int, bool, float,
  string, vec3, user)
  input entry: +0x00 name, then source node (s16) and its output (s16) at +4
  (+8 for user types); a source <= -100 is the multi-input list from entry
  (-100 - source), count = the output field; multi entry 8 bytes: node, output
  string immediate 0x0c: +0x00 name, +0x08 value (InstanceName, Name)
  string input: +0x0c its default value (HoldGameDataGetBool's GameDataNameBool: the flag's name)
"""
import argparse
import glob
import os
import re
import struct
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml, zs  # noqa: E402

TYPES = ("int", "bool", "float", "string", "vec3", "udt")
IN_SIZE = {"int": 0x10, "bool": 0x10, "float": 0x10, "string": 0x10, "vec3": 0x18, "udt": 0x14}
OUT_SIZE = {"int": 4, "bool": 4, "float": 4, "string": 4, "vec3": 4, "udt": 8}
# Switch actors whose output starts an event (not the shutters they open).
SWITCH = re.compile(r"^Obj_(?:[A-Za-z]+_)*(?:EyeSwitch|StepSwitch|SwitchHit|HammerSwitch|DiveSwitch|SolarSwitch)(?:_|$)")


def read_graph(data):
    """[node]: {"type", "imm": {name: str}, "in": {type: [(name, src, out)]}, "out": {type: [name]}}, multi(base, n)."""
    u32 = lambda o: struct.unpack_from("<I", data, o)[0]  # noqa: E731
    s16 = lambda o: struct.unpack_from("<h", data, o)[0]  # noqa: E731
    u16 = lambda o: struct.unpack_from("<H", data, o)[0]  # noqa: E731
    pool = u32(0x24)

    def text(o):
        return data[pool + o: data.index(b"\0", pool + o)].decode("utf-8", "replace")

    commands, count = u32(0x0C), u32(0x10)
    io, multi_at, imm = u32(0x34), u32(0x38), u32(0x2C)
    starts = [u32(io + 4 * i) for i in range(12)] + [multi_at]
    ins, outs, defaults, bools = {}, {}, [], []
    for t, name in enumerate(TYPES):
        lst, o = [], starts[2 * t]
        while o < starts[2 * t + 1]:
            at = o + (8 if name == "udt" else 4)
            lst.append((text(u32(o) & 0x3FFFFFFF), s16(at), s16(at + 2)))
            if name == "string":  # its default value (a flag's name) at +0x0c
                defaults.append(text(u32(o + 12)))
            elif name == "bool":  # its default value at +0x0c (an unwired input's constant: a gate's UpdateOnce)
                bools.append(u32(o + 12) & 1)
            o += IN_SIZE[name]
        ins[name] = lst
        lst, o = [], starts[2 * t + 1]
        while o < starts[2 * t + 2]:
            lst.append(text(u32(o) & 0x3FFFFFFF))
            o += OUT_SIZE[name]
        outs[name] = lst
    string_imm = u32(imm + 12)
    nodes = []
    for i in range(count):
        n = 0x74 + commands * 0x18 + i * 0x3C
        body = u32(n + 0x14)
        node = {"type": text(u32(n + 8)), "in": {}, "out": {}, "imm": {}, "sdef": {}, "bdef": {}}
        for t, name in enumerate(TYPES):
            a, b = u32(body + 0x30 + 16 * t), u32(body + 0x34 + 16 * t)
            c, d = u32(body + 0x38 + 16 * t), u32(body + 0x3C + 16 * t)
            node["in"][name] = ins[name][a: a + b]
            node["out"][name] = outs[name][c: c + d]
            if name == "string":
                node["sdef"] = {k[0]: v for k, v in zip(ins[name][a: a + b], defaults[a: a + b])}
            elif name == "bool":
                node["bdef"] = {k[0]: v for k, v in zip(ins[name][a: a + b], bools[a: a + b]) if k[1] == -1}
        first, n_str = u32(body + 0x18), u32(body + 0x1C)
        for k in range(first, first + n_str):
            o = string_imm + 0x0C * k
            node["imm"][text(u32(o))] = text(u32(o + 8))
        nodes.append(node)

    def multi(base, n):
        return [(u16(multi_at + 8 * (base + k)), u16(multi_at + 8 * (base + k) + 2)) for k in range(n)]

    return nodes, multi


def _int(v):
    return int(v.v if hasattr(v, "v") else v)


def _place(actor):
    return tuple(round(float(x), 1) for x in (actor.get("Translate") or [0, 0, 0]))


def _prefix(path):
    return path.rsplit("/", 1)[0] + "/" if "/" in path else ""


# A switch's inputs that put it ON through its own AI (deku-ladder, A #39): a forced, first-on or cleared input, or
# BootUp where the room graph boots the switch from its saved flag.
ON_INPUTS = ("ForceControl", "FirstOn", "isCleared", "IsSkipLight", "Active", "BootUp")
# The asker's outputs a room result can hang on (Dead and Logic_Alive are its own life).
ASKER_OUTPUTS = ("EventBool", "EventBoolEX1", "EventBoolEX2", "EventFinish")
# Results on the asker's outputs that change nothing kept by the room: a save prompt, Navi's balloon, the dungeon
# room step (DungeonRoomStep.*: counted by the map, not drawn: reports/dungeon-sync-b.md).
COSMETIC = ("TriggerRequestAutoSave", "TriggerNaviRequestBallonMessage", "TriggerDungeonRoomStep")
# Actor inputs fed by the asker that only play the event's moment: a chest's appear act (the chest itself is created
# by the shared flag), and the event's own air walls and areas (made for the event; after a reload they are as before
# it, so the game itself keeps nothing of them).
TRANSIENT_INPUTS = ("isSpawnAct", "IsSpawnAct")
TRANSIENT_ACTORS = re.compile(r"AirWall|^Area$")
# A shutter's lock (row 336; rows 62, 72, 160): the inputs that keep a room's shutter shut. A shared flag ORed into
# one does NOT open the shutter in the game that did not play the event: in every player log of an enemy room
# (Dodongo's Cavern's Lizalfos rooms 1 and 2, where the room-clear flag is the OR's only other leaf) the flag landed
# (the room's enemies were deleted and its chest appeared by that same flag) and the shutters stayed shut until a
# reload. The game that played the event has the asker's EventBool on that OR as well, and its shutters open. So a
# lock input the asker drives is never covered by a shared flag: such an asker is class "asker", and our asker's
# outputs are set as the event's own flow sets them (EventTriggerSetEventBool), so both games hold the same inputs.
LOCK_ACTORS = re.compile(r"Shutter")
LOCK_INPUTS = ("LockPlusZ", "LockMinusZ", "Open")


def _leaf(kind, what, plug, neg, under_or):
    """A trigger leaf kept for trigger_guards(): (kind, name, placement hash or 0, plug, negated, under an OR, place,
    the placements its Reference links name (a silver rupee counter's tokens))."""
    if kind != "actor":
        return (kind, what, 0, plug, neg, under_or, (), ())
    links = tuple(sorted(_int(k["Dst"]) for k in what.get("Links", []) or [] if k.get("Name") == "Reference"))
    return ("actor", str(what.get("Gyaml")), _int(what["Hash"]) if "Hash" in what else 0, plug, neg, under_or,
            _place(what), links)


def switch_events(romfs, dicts=None, wiring=None, askers=None, world=None, performers=None):
    """(lines, guards, extras), each sorted.

    lines:  (event, scene, switch name, switch place, plug, asker place)
    guards: (flag, scene, event, why) - a saved flag that keeps the event from starting in our game when it comes
            from another player first: one that deletes the asker (why "deletes the asker"), or one the trigger reads
            negated (why "the trigger reads it negated")
    extras: (event, scene, actor name, actor place, plug) - another actor's plug the trigger ANDs with the switch
    others: (event, scene, what) - any other input on the trigger (a positive flag, a player state, ...), for notes
    wiring: when a dict, filled per line (the lines' tuple) with {"on": [(input, [(kind, what, negated)])] (the
            switch's ON inputs and their sources, [] = unwired), "results": [(kind, what, detail)] (what the asker's
            outputs drive, forward through gates, module outputs and module calls), "delete": [flags that delete the
            asker], "guards": asker_guards() of THIS asker (an event asked by two askers has two lines, each with
            its own)}. classify() and room_objects.py read it.
    askers: when a dict, filled for EVERY asker of an event (with a switch or not; row 98(2), result only), keyed
            (event, scene, asker place): {"actor": its name, "outputs": {output: [results]} (only outputs that
            drive something), "results": all of them, "delete": [flags that delete it]}. asker_class() reads it.
    world:  when a dict, filled per scene with every placed actor: {scene: {hash: (name, DefaultDungeonRoomIndex or
            None, place, the placements all its links name)}}. trigger_guards() reads it (followed world objects).
    performers: when a dict, filled {(scene, event): {(actor name, place)}} with every placed actor whose Logic_Delete
            reads that event's played state (HoldGameDataHavePlayedEvent, or the flag HavePlayedEvent.<event>,
            through gates): the room's "before" actor of the event, deleted only when the room loads with the event
            played (row 165: Dodongo's broken stair). performer_plays() and main --results read it.
    """
    if dicts is None:
        dicts = zs.Dicts.from_romfs(romfs)
    graphs = {}

    def graph(logic):
        path = os.path.join(romfs, "Logic", os.path.basename(str(logic)) + "b")
        if path not in graphs:
            graphs[path] = None
            if os.path.exists(path):
                with open(path, "rb") as f:
                    graphs[path] = read_graph(f.read())
        return graphs[path]

    lines, guards, extras, others = set(), set(), set(), set()
    for folder in sorted(glob.glob(os.path.join(romfs, "Banc", "*"))):
        scene = os.path.basename(folder)
        actors, groups = [], []
        for path in glob.glob(os.path.join(folder, "*.bcett.byml.zs")):
            with open(path, "rb") as f:
                root = byml.loads(dicts.decompress(f.read()))
            actors += root.get("Actors", []) or []
            groups += root.get("AiGroups", []) or []
        by_hash = {_int(a["Hash"]): a for a in actors if "Hash" in a}
        if world is not None:
            world[scene] = {h: (str(a.get("Gyaml", "")), (a.get("Dynamic") or {}).get("DefaultDungeonRoomIndex"),
                                _place(a), tuple(sorted(_int(k["Dst"]) for k in a.get("Links", []) or [])))
                            for h, a in by_hash.items()}
        for g in groups:
            refs = g.get("References", []) or []
            mods = [("", g.get("Logic", ""), None)]
            mods += [(_prefix(r["Path"]), r["Logic"], r.get("InstanceName")) for r in refs if "Logic" in r]
            by_id = {(_prefix(r["Path"]), r["Id"]): by_hash[_int(r["Reference"])]
                     for r in refs if "Reference" in r and _int(r["Reference"]) in by_hash}

            def leaves(pre, g_, src, outp, depth, seen, neg=False, under_or=False, once=False, anded=False):
                """[(kind, actor or flag name, plug, negated, under an OR)], kind "actor", "flag" or "node"; a flag
                behind a gate whose UpdateOnce input is 1 (the game reads it once, when the room loads: the Deku
                Tree ladder's RoomC_Cleared) is kind "flag-once": it carries no live result. "under an OR" is 0 (no
                OR on its way), 1 (an OR and no AND: the leaf alone drives the input) or 2 (an OR and also an AND, a
                gate with two or more wired inputs: the Deku Tree compass lifts' OR(asker, AND(RoomD_StepSwitch_On_01,
                plate)), whose flag alone moves nothing); truthy = in an OR, as before."""
                alone = (2 if anded else 1) if under_or else 0
                nodes, multi = g_
                if src <= -100:
                    found = []
                    for a, b in multi(-100 - src, outp):
                        found += leaves(pre, g_, a, b, depth + 1, seen, neg, under_or, once, anded)
                    return found
                if src < 0 or src >= len(nodes) or depth > 12 or (id(g_), src, outp) in seen:
                    return []
                seen.add((id(g_), src, outp))
                node = nodes[src]
                plugs = node["out"]["bool"]
                plug = plugs[outp] if 0 <= outp < len(plugs) else ""
                inst = node["imm"].get("InstanceName")
                if inst and (pre, inst) in by_id:
                    return [("actor", by_id[(pre, inst)], plug, neg, alone)]
                if not node["type"] and "Name" in node["imm"]:  # a module input: follow it in the calling graph
                    found = []
                    for mpre, logic, minst in mods:
                        if mpre != pre or minst is None:
                            continue
                        mtype = os.path.basename(str(logic))[: -len(".ain")]
                        for ppre, plogic, _ in mods:
                            parent = graph(plogic) if plogic and ppre != pre and pre.startswith(ppre) else None
                            if parent is None:
                                continue
                            for call in parent[0]:
                                if call["type"] == mtype and call["imm"].get("InstanceName") == minst:
                                    for name, a, b in call["in"]["bool"]:
                                        if name == node["imm"]["Name"]:
                                            found += leaves(ppre, parent, a, b, depth + 1, seen, neg, under_or, once, anded)
                    return found
                if node["type"].startswith("HoldGameDataGetBool"):
                    name = node["sdef"].get("GameDataNameBool", "")
                    return [("flag-once" if once else "flag", name, plug, neg, alone)] if name else []
                if not node["in"]["bool"]:
                    return [("node", node["type"], plug, neg, alone)]
                # A gate, or a module call (InvBool, ...): its False output (or an inverter) negates.
                flip = plug == "False" or "InvBool" in node["type"]
                is_or = "OR" in node["type"].upper().replace("OPERATE", "")[:2] or node["type"].startswith("OperateOR")
                once = once or node.get("bdef", {}).get("UpdateOnce") == 1
                wired = sum(b if a <= -100 else int(a >= 0) for name, a, b in node["in"]["bool"] if name != "UpdateOnce")
                anded = anded or (not is_or and wired > 1)
                found = []
                for name, a, b in node["in"]["bool"]:
                    if name != "UpdateOnce":
                        found += leaves(pre, g_, a, b, depth + 1, seen, neg != flip, under_or or is_or, once, anded)
                return found

            def calls(pre, name, inner):
                """[(graph, pre, node index, output index)] where a module port continues: inner False: `pre`'s
                module output `name` -> the calls' outputs in the calling graphs; inner (an input name): the call
                node `name` -> that input's node in the called module."""
                found = []
                if inner:
                    call = name
                    for mpre, logic, minst in mods:
                        sub = graph(logic) if logic and mpre != pre and mpre.startswith(pre) else None
                        if sub is None or minst != call["imm"].get("InstanceName") or \
                                os.path.basename(str(logic))[: -len(".ain")] != call["type"]:
                            continue
                        for k, n in enumerate(sub[0]):
                            if not n["type"] and not n["in"]["bool"] and n["imm"].get("Name") == inner:
                                found.append((sub, mpre, k, 0))
                    return found
                for mpre, logic, minst in mods:
                    if mpre != pre or minst is None:
                        continue
                    mtype = os.path.basename(str(logic))[: -len(".ain")]
                    for ppre, plogic, _ in mods:
                        parent = graph(plogic) if plogic and ppre != pre and pre.startswith(ppre) else None
                        if parent is None:
                            continue
                        for k, call in enumerate(parent[0]):
                            if call["type"] == mtype and call["imm"].get("InstanceName") == minst and \
                                    name in call["out"]["bool"]:
                                found.append((parent, ppre, k, call["out"]["bool"].index(name)))
                return found

            def forward(pre, g_, src, outp, depth, seen):
                """What the plug (src, outp) drives: [("actor", actor, input) | ("flag", name, "set"/"clear") |
                ("node", type, input)], through gates, delays, module outputs and module calls. An actor's detail is
                (input, [(flag or actor name, negated, under an OR)] that input reads)."""
                nodes, multi = g_
                found = []
                if depth > 12:
                    return found
                for j, m in enumerate(nodes):
                    for name, a, b in m["in"]["bool"]:
                        if (src, outp) not in (multi(-100 - a, b) if a <= -100 else [(a, b)]):
                            continue
                        if (id(g_), j, name) in seen:
                            continue
                        seen.add((id(g_), j, name))
                        inst = m["imm"].get("InstanceName")
                        if inst and (pre, inst) in by_id:
                            # what else that input reads (a flag or a switch beside the asker): the flags arriving
                            # from the other game may already drive it
                            also = tuple(sorted({(w if k == "flag" else str(w.get("Gyaml")), ng, uo)
                                                 for k, w, _, ng, uo in leaves(pre, g_, a, b, 0, set())
                                                 if k in ("flag", "actor")}))
                            found.append(("actor", by_id[(pre, inst)], (name, also)))
                        elif m["type"].startswith("TriggerGameDataSetBool"):
                            found.append(("flag", m["sdef"].get("GameDataNameBool", ""),
                                          "set" if m["type"].endswith("True") else "clear"))
                        elif m["type"].endswith(".module") and inst and not calls(pre, m, name):
                            # a module call we cannot enter (not in this group): its input, like an actor's
                            also = tuple(sorted({(w if k == "flag" else str(w.get("Gyaml")), ng, uo)
                                                 for k, w, _, ng, uo in leaves(pre, g_, a, b, 0, set())
                                                 if k in ("flag", "actor")}))
                            found.append(("module", m["type"], (name, also)))
                        elif not m["type"] and "Name" in m["imm"]:  # a module output
                            for g2, p2, k, y in calls(pre, m["imm"]["Name"], False):
                                found += forward(p2, g2, k, y, depth + 1, seen)
                        else:
                            inner = calls(pre, m, name) if inst else []
                            for g2, p2, k, y in inner:
                                found += forward(p2, g2, k, y, depth + 1, seen)
                            if m["type"].startswith("OperateGameDataAlloc") or not m["out"]["bool"]:
                                found.append(("node", m["type"], name))
                            if not inner:
                                for y in range(len(m["out"]["bool"])):
                                    found += forward(pre, g_, j, y, depth + 1, seen)
                return found

            def switch_node(actor):
                """(pre, graph, node) of the switch actor's own logic node in this group."""
                for mpre, logic, _ in mods:
                    g2 = graph(logic) if logic else None
                    for n in (g2[0] if g2 else []):
                        if by_id.get((mpre, n["imm"].get("InstanceName"))) is actor:
                            return mpre, g2, n
                return None

            def played_of(g_, src, outp, depth=0):
                """The events whose played state the plug (src, outp) reads, through gates in this graph."""
                nodes, multi = g_
                if src <= -100:
                    return {e for a, b in multi(-100 - src, outp) for e in played_of(g_, a, b, depth + 1)}
                if src < 0 or src >= len(nodes) or depth > 12:
                    return set()
                node = nodes[src]
                if node["type"] == "HoldGameDataHavePlayedEvent":
                    return {node["sdef"].get("EventName", "")} - {""}
                name = str(node["sdef"].get("GameDataNameBool", ""))
                if node["type"].startswith("HoldGameDataGetBool") and name.startswith("HavePlayedEvent."):
                    return {name[len("HavePlayedEvent."):]}
                if not (node["type"].startswith("Operate") or "InvBool" in node["type"]):
                    return set()
                return {e for n, a, b in node["in"]["bool"] if n != "UpdateOnce" for e in played_of(g_, a, b, depth + 1)}

            for pre, logic, _ in mods:
                g_ = graph(logic) if logic else None
                if g_ is None or performers is None:
                    continue
                for node in g_[0]:
                    actor = by_id.get((pre, node["imm"].get("InstanceName")))
                    if actor is None:
                        continue
                    for name, a, b in node["in"]["bool"]:
                        if name == "Logic_Delete":
                            for event in played_of(g_, a, b):
                                performers.setdefault((scene, event), set()).add((str(actor.get("Gyaml")),
                                                                                  _place(actor)))

            for pre, logic, _ in mods:
                g_ = graph(logic) if logic else None
                if g_ is None:
                    continue
                for index, node in enumerate(g_[0]):
                    if node["type"] != "ActorLogicEventRequestActor":
                        continue
                    asker = by_id.get((pre, node["imm"].get("InstanceName")))
                    event = (asker or {}).get("Dynamic", {}).get("RequestEventName") if asker else None
                    if not event:
                        continue
                    trigger, delete = [], []
                    for name, a, b in node["in"]["bool"]:
                        if name == "EventRequestTrigger":
                            trigger = leaves(pre, g_, a, b, 0, set())
                        elif name == "Logic_Delete":
                            delete = leaves(pre, g_, a, b, 0, set())
                    if askers is not None:
                        per = {}
                        for out in ASKER_OUTPUTS:
                            if out in node["out"]["bool"]:
                                got = forward(pre, g_, index, node["out"]["bool"].index(out), 0, set())
                                got = sorted({(k, str(w.get("Gyaml")) if k == "actor" else w, d) for k, w, d in got
                                              if not (k == "actor" and w is asker)})
                                if got:
                                    per[out] = got
                        askers[(str(event), scene, _place(asker))] = {
                            "actor": str(asker.get("Gyaml")), "outputs": per,
                            "results": sorted({r for v in per.values() for r in v}),
                            "delete": sorted({w for k, w, _, ng, _ in delete if k == "flag" and not ng}),
                            "trigger": sorted({_leaf(k, w, plug, ng, uo) for k, w, plug, ng, uo in trigger})}
                    switches = [t for t in trigger if t[0] == "actor" and SWITCH.match(str(t[1].get("Gyaml", ""))) and t[2]]
                    if not switches:
                        continue
                    event = str(event)
                    for _, actor, plug, _, _ in switches:
                        lines.add((event, scene, str(actor["Gyaml"]), _place(actor), plug, _place(asker)))
                    for kind, what, plug, neg, under_or in trigger:
                        if kind == "actor" and SWITCH.match(str(what.get("Gyaml", ""))):
                            continue
                        if kind == "flag" and neg:
                            guards.add((what, scene, event, "the trigger reads it negated"))
                        elif kind == "actor" and not neg and not under_or and plug:
                            extras.add((event, scene, str(what["Gyaml"]), _place(what), plug))
                        else:
                            label = str(what.get("Gyaml")) if kind == "actor" else what
                            others.add((event, scene, f"{'not ' if neg else ''}{label}.{plug}{' (in an OR)' if under_or else ''}"))
                    for kind, what, plug, neg, under_or in delete:
                        if kind == "flag" and not neg and not what.startswith("HavePlayedEvent."):
                            guards.add((what, scene, event, "deletes the asker"))
                    if wiring is None:
                        continue
                    results = []
                    for out in ASKER_OUTPUTS:
                        if out in node["out"]["bool"]:
                            results += forward(pre, g_, index, node["out"]["bool"].index(out), 0, set())
                    results = [(k, str(w.get("Gyaml")) if k == "actor" else w, d) for k, w, d in results
                               if not (k == "actor" and w is asker)]
                    deletes = sorted({w for k, w, _, ng, _ in delete if k == "flag" and not ng})
                    for _, actor, plug, _, _ in switches:
                        found = switch_node(actor)
                        on = []
                        if found:
                            spre, sg, sn = found
                            for name, a, b in sn["in"]["bool"]:
                                if name in ON_INPUTS:
                                    srcs = [] if a == -1 else leaves(spre, sg, a, b, 0, set())
                                    on.append((name, [(k, str(w.get("Gyaml")) if k == "actor" else w, ng)
                                                      for k, w, _, ng, _ in srcs]))
                        line = (event, scene, str(actor["Gyaml"]), _place(actor), plug, _place(asker))
                        wiring[line] = {"on": on, "results": sorted(set(results)), "delete": deletes,
                                        "guards": asker_guards(trigger, delete)}
    return sorted(lines), sorted(guards), sorted(extras), sorted(others)


LOAD_GUARD = "load:"  # asker_guards(): a flag the room reads only when it loads (kind "flag-once")


def load_guard(guard):
    """(flag, read only at the room's load) of one asker_guards() entry."""
    return (guard[len(LOAD_GUARD):], True) if guard.startswith(LOAD_GUARD) else (guard, False)


def asker_guards(trigger, delete):
    """The saved flags that stop ONE asker (g3-objects): a flag its trigger reads negated outside an OR (on: the
    trigger is false whatever the switch says), or a flag that deletes it (on: no asker). HavePlayedEvent.* is left
    out (coop_dungeon.cpp holds those in the dungeon). `trigger`/`delete`: leaves() of its EventRequestTrigger and
    Logic_Delete, (kind, what, plug, negated, under an OR).

    A negated flag under an OR counts only when it is the trigger's ONE leaf under an OR (Jabu-Jabu room 1:
    OnState AND an OR whose other inputs are unwired, i.e. the flag alone); with two or more leaves under ORs the
    tool cannot tell which OR holds which, so none of them counts (no hold: the safe side).

    A flag behind a gate with UpdateOnce 1 (kind "flag-once") is read only when the room loads: on at the load it
    stops the asker for that visit, but landing live (the other player's press) stops nothing. It is returned as
    a LOAD guard, "load:<flag>" (load_guard() splits it): the Deku Tree room D compass asker, deleted by
    RoomD_StepSwitch_On_01 only at a load (events-next-a-5, Room_D_Prefab_b6c3). The C++ gate arms such a guard
    only when the flag was already on when our room loaded (CoopSwitchShare: the visit's first sight of the
    switch), never by a mid-visit turn-on (our asker still exists then: a hold would replay the event)."""
    in_or = [x for x in trigger if x[4]]
    out = set()
    for kind, what, _, neg, under_or in trigger:
        # under_or == 1: in an OR with no AND on the way (g1-events 9fce0c8f: 2 = also behind an AND, not alone)
        if kind in ("flag", "flag-once") and neg and (not under_or or (under_or == 1 and len(in_or) == 1)):
            out.add(what if kind == "flag" else LOAD_GUARD + what)
    for kind, what, _, neg, _ in delete:
        if kind in ("flag", "flag-once") and not neg and not what.startswith("HavePlayedEvent."):
            out.add(what if kind == "flag" else LOAD_GUARD + what)
    # read live AND once at the load (two paths to the same flag): the live read stops the asker at any time
    return sorted(g for g in out if not (load_guard(g)[1] and load_guard(g)[0] in out))


def two_way_flags(wiring):
    """The flags some asker sets and some asker (the same or another) clears, over all lines (g3-objects)."""
    seen = {}
    for wire in wiring.values():
        for kind, what, detail in wire["results"]:
            if kind == "flag" and detail in ("set", "clear"):
                seen.setdefault(what, set()).add(detail)
    return {f for f, d in seen.items() if d == {"set", "clear"}}


def classify(line, wire, shared, two_way=frozenset(), locks=False):
    """(class, fix, why) of a switch line whose plug does not latch (deku-ladder, A #39).

    class "A": every room result of the event reaches the room through a shared flag: the asker's outputs only set
    shared flags, or the actors they drive also read a shared flag (or the switch, itself on from a shared flag) in
    the same OR, or what they drive is the event's moment only (COSMETIC, TRANSIENT_*: after a reload the game itself
    shows it as before the event). Nothing to drive: the other player's flags (live, coop_dungeon) are the native
    path, and the room ends as the other player's own room does after a reload.
    class "B": a result hangs on our asker only (an actor input, an unshared flag, a dungeon step), so our asker must
    run the event. fix: "input <name>" - the switch's ON input is unwired: set it (the type-0 store) so the
    switch's own AI turns on and keeps its output; "split" - the ON input reads a shared flag that does not delete
    the asker: hold only the asker's delete flags (the ON flag comes, our switch turns on, our asker asks); "none" -
    the ON input's flag also deletes the asker (or no ON input): no native way, reported.
    An actor input is also covered by a shared flag ORed into it that the asker's own results drive the same way
    (the asker clears it and the OR reads it negated, or sets it and the OR reads it as is: g3-objects, Jabu room
    14's upper switch re-arms from `not Updown_MoveWater`, which its event clears).
    A class B fix that runs our asker (input, split) is refused ("none") when the asker writes a shared flag that
    some asker also writes the other way (`two_way`): our late run would be a second writer and could undo the
    other player's newer write for everyone (g3-objects: Jabu room 14's water flags once they are shared).
    `locks` (asker_class, row 336): a shutter's lock input (LOCK_ACTORS, LOCK_INPUTS) is never covered by a shared flag.
    """
    event, scene, switch, at, plug, asker_at = line
    on_flags = {w for _, srcs in wire["on"] for k, w, ng in srcs if k == "flag" and not ng and w in shared}
    writes = {(w, d) for k, w, d in wire["results"] if k == "flag" and d in ("set", "clear")}
    misses = []
    for kind, what, detail in wire["results"]:
        if kind == "flag":
            if detail == "set" and what not in shared and "NaviHint" not in what:
                misses.append(f"unshared flag {what}")
        elif kind == "node":
            if not any(what.startswith(c) for c in COSMETIC) and detail not in TRANSIENT_INPUTS:
                misses.append(f"{what}.{detail}")
        else:  # an actor's or a module call's input
            name, also = detail
            if name in TRANSIENT_INPUTS or (kind == "actor" and TRANSIENT_ACTORS.search(what)):
                continue
            # the asker's own polarity on that input (a False output or an inverter flips it); not found (it is
            # behind a module port): any
            polarity = {ng for w, ng, _ in also if w == "EventRequestActor"}
            covered = any((uo == 1 or not polarity) and (not polarity or ng in polarity) and
                          (w in shared or (w == switch and on_flags)) for w, ng, uo in also)
            covered = covered or any(uo == 1 and w in shared and (w, "clear" if ng else "set") in writes
                                     for w, ng, uo in also)
            if locks and kind == "actor" and LOCK_ACTORS.search(what) and name in LOCK_INPUTS:
                covered = False
            if not covered:
                misses.append(f"{what}.{name}")
    if not misses:
        return "A", "", "results through shared flags"
    misses = sorted(set(misses))
    second = sorted(w for w, _ in writes if w in shared and w in two_way)
    if second:
        return "B", "none", "; ".join(misses) + f"; our asker would write shared two-way {', '.join(second)} again"
    forced =[n for n, srcs in wire["on"] if n != "BootUp" and n != "Active" and not srcs]
    wired = [w for n, srcs in wire["on"] if n in ("BootUp", "ForceControl", "FirstOn", "isCleared")
             for k, w, ng in srcs if k == "flag" and not ng]
    if wired and set(wired) <= shared and not set(wired) & set(wire["delete"]):
        return "B", "split", "; ".join(misses)
    if forced:
        return "B", f"input {forced[0]}", "; ".join(misses)
    return "B", "none", "; ".join(misses)


def event_plays(romfs, events, dicts=None):
    """{event: {(actor, ASName)}}: every EventPlayAS its event flow can run (RSDB EventSetting: the flow and entry;
    through subflows and forks, puppet_pack/evfl.py actions_of). An event without a setting or flow gives nothing."""
    from . import evfl
    if dicts is None:
        dicts = zs.Dicts.from_romfs(romfs)
    found = sorted(glob.glob(os.path.join(romfs, "RSDB", "EventSetting.Product.*.rstbl.byml.zs")))
    if not found:
        return {}
    with open(found[-1], "rb") as f:
        settings = {r["__RowId"]: r for r in byml.loads(dicts.decompress(f.read()))}
    flows = {}

    def load(name):
        path = os.path.join(romfs, "Event", "EventFlow", name + ".bfevfl.zs")
        if not os.path.exists(path):
            return None
        with open(path, "rb") as f:
            return evfl.parse(dicts.decompress(f.read()))

    out = {}
    for event in sorted(set(events)):
        row = settings.get(event)
        if not row or not row.get("FlowchartFileName"):
            continue
        flow = str(row["FlowchartFileName"]).split("/")[-1].split(".")[0]
        acts = evfl.actions_of(flows, flow, row.get("EntryPointName") or "", load=load)
        out[event] = {(a, str(p.get("ASName"))) for a, act, p in acts if act == "EventPlayAS" and p.get("ASName")}
    return out


def performer_plays(event, scene, performers, plays):
    """[(actor, place, ASName)] (row 165): the event flow's EventPlayAS on a room actor whose Logic_Delete reads the
    event's played state. The game deletes that actor (and makes its end actor) only when the room loads with the
    event played; live, the event's own AS is what changes it (Dodongo's stair falls). So the other player's played
    flag changes nothing in our loaded room: our performer plays that AS (result only: our Link is not bound)."""
    by_name = {}
    for actor, asname in plays.get(event, ()):
        by_name.setdefault(actor, set()).add(asname)
    return sorted((actor, place, asname) for actor, place in performers.get((scene, event), ())
                  for asname in by_name.get(actor, ()))


def lock_only(key, wire, shared, plays=()):
    """True when the asker is class asker ONLY for a shutter's lock (row 336): without the LOCK_INPUTS rule it would be
    class live (as in 0.12.0). Such an event's set has a bounded fallback (coop_event_result.cpp kLocks): given up, its
    flags land as a class-live event's did."""
    if plays:
        return False
    event, scene, at = key
    w = {"on": [], "results": wire["results"], "delete": wire["delete"]}
    line = (event, scene, "", at, "", at)
    return classify(line, w, shared)[0] == "A" and classify(line, w, shared, locks=True)[0] == "B"


def asker_class(key, wire, shared, plays=()):
    """(class, why) of one asker for result only (row 98(2), owner 07:40 UTC 2026-11-17: a gimmick event another
    player played is never replayed here). "live": every room result of the asker reaches the room through a shared
    flag the room reads live (classify's class A), so its held flags landing is the whole result; "asker": a result
    hangs on our asker's outputs (an actor input, an unshared flag, a flag the room reads only at its load), so our
    asker's outputs are set (the game's set-bool, what the event flow's EventTriggerSetEventBool does) before its held
    flags land. `plays` (performer_plays(), row 165): a performer's AS is a result no shared flag gives (the room reads
    the played state only at its load), so such an asker is "asker" too."""
    event, scene, at = key
    cls, _, why = classify((event, scene, "", at, "", at), {"on": [], "results": wire["results"],
                                                             "delete": wire["delete"]}, shared, locks=True)
    if plays:
        played = "; ".join(f"{actor} plays {asname} (its room deletes it at load only)" for actor, _, asname in plays)
        return "asker", played if cls == "A" else f"{why}; {played}"
    return ("live", why) if cls == "A" else ("asker", why)


# An actor's trigger plug that a kill rises (the object sync's shared kills and removes, docs/objects.md).
KILL_PLUGS = ("Dead", "IsDeadNotDelay", "IsDeathEventPlay")
# A press switch: what a pushed block (or a statue, a rolling rock) can rest on.
PRESS_SWITCH = re.compile(r"^Obj_(?:[A-Za-z_]+_)?(?:StepSwitch|Stepswitch)(?:_|$)")
# A movable's reach when the switch or the movable has no room index: places (m) this near the switch.
FOLLOW_REACH_M = 20.0


def is_movable(name):
    """lichii/hook/coop_world.cpp IsMovable (with IsRoller, IsGravestone): what coop_world follows to another game's
    rest place (push blocks, slider boxes, Amos statues, rolling rocks, graves)."""
    if not name.startswith("Obj_") or "BasePosAnchor" in name:
        return False
    return ("MovableBox_" in name or name.startswith("Obj_SliderBox_") or name.startswith("Obj_AmosStatue_")
            or ("RollingRock_" in name and "RollingRockArea" not in name) or name.startswith("Obj_Tombstone_")
            or name in ("Obj_Graveyard_Tombstone_A_01", "Obj_Graveyard_Tombstone_A_02"))


def is_followed(name):
    """coop_world.cpp's followers whose state another game's sync changes here other than by a kill or a key:
    IsMovable (moves, rest place), IsFaller (life 0: a pillar falls, a hammer switch is pressed), IsShifter (put where
    another game left it), IsGimmickMover (the Deku Tree's moving floor). Breakables go by the kill guard (their
    break is a life write or a remove), lying keys and tokens by their keys."""
    return (is_movable(name) or name.startswith("Obj_FireTemple_HammerSlipPillar_")
            or name.startswith("Obj_HammerSwitch_") or name.startswith("Obj_FireTemple_HammerDropFloor_")
            or name.startswith("Obj_FireTemple_HammerDropPillar_") or name.startswith("Obj_FireTemple_HammerStair_")
            or name.startswith("Obj_InsideTheDekuTree_FallingLadder_")
            or name.startswith("Obj_InsideTheDekuTree_FallingFloor_") or name == "Obj_ShadowTemple_FallingFloor_A_03"
            or name.startswith("Obj_Deku_MoveFloor_"))
SILVER_COUNTER = re.compile(r"^Obj_silverRupeeCounter(?:_|$)")


def trigger_guards(askers, keep, shared, keys, one_way, plugs, world=None):
    """(guards, proofs) for row 98(2)'s trigger guards: what another player's game can raise on our asker's
    EventRequestTrigger, so our asker would ask the event here (a replay, review-a38-5). Every leaf of every asker's
    trigger (both classes) is either a guard or a proof line.

    guards: (kind, scene, event, flag, id, group, why), kind
      "flag"  a shared flag the trigger reads (not negated): waits until our game has played the event;
      "key"   a trigger actor whose saved placement key is shared (a silver rupee counter's own key): waits as a flag;
      "token" a key the actor's state counts (a counter's tokens, its Reference links; group = the counter's key):
              the hook keeps the largest one missing here waiting and lets the others land (a split collection), so
              shared keys never complete our counter;
      "kill"  an actor whose Dead (IsDeadNotDelay, IsDeathEventPlay, a negated Logic_Alive) the trigger reads, killed
              or removed by the object sync: the largest live one of the event waits, the others die;
      "follow" a world object coop_world follows (is_followed) that can raise the trigger: the trigger actor itself
              (a hammer switch's press, a statue's or block's stop), each movable that can rest on a press switch the
              trigger reads (one in the switch's room, group 0: each waits), or a block a trigger actor links (a
              puzzle manager's corners: group = that actor's hash: only the LARGEST waits, the others land, so
              followed pushes alone never complete it): another game's move, rest place or fall of it waits until
              our game has played the event, then lands.
    proofs: (event, scene, text) - a leaf shared state cannot raise, and why.
    keys: the shared placement keys (coop_dungeon.cpp kKeys); one_way: shared flags whose 0 is never sent
    (kLiveFlags); plugs: {(scene, switch name): [(place, plug)]} the switch share holds (coop_room_objects.cpp);
    world: switch_events()' world (none: no followed world object is known)."""
    guards, proofs = set(), set()
    world = world or {}

    def room_of(scene, hash_):
        own = world.get(scene, {}).get(hash_)
        return own[1] if own else None

    def follow(scene, event, name, hash_, place):
        """The follow guards of one trigger actor (empty: no followed world object can drive it)."""
        actors = world.get(scene, {})
        found = set()
        if is_followed(name):
            found.add(("follow", scene, event, "", hash_, 0, f"{name}: another game's move or fall of it"))
        own = actors.get(hash_)
        for dst in (own[3] if own else ()):
            other = actors.get(dst)
            if other and is_movable(other[0]):
                found.add(("follow", scene, event, "", dst, hash_, f"{other[0]}, a block {name} links"))
        if PRESS_SWITCH.match(name):
            room = own[1] if own else None
            for h, (other, other_room, at, _) in actors.items():
                if h == hash_ or not is_movable(other):
                    continue
                if room is not None and other_room is not None:
                    near = other_room == room
                else:
                    near = sum((a - b) ** 2 for a, b in zip(at, place)) < FOLLOW_REACH_M ** 2
                if near:
                    found.add(("follow", scene, event, "", h, 0, f"{other} may rest on {name}"))
        return found
    for key in sorted(askers):
        event, scene, _ = key
        if keep is not None and (event, scene) not in keep:
            continue
        for kind, name, hash_, plug, neg, _, place, links in askers[key].get("trigger", []):
            if kind in ("flag", "flag-once"):
                if name not in shared:
                    proofs.add((event, scene, f"flag {name}: not shared (each game's own)"))
                elif kind == "flag-once":
                    proofs.add((event, scene, f"flag {name}: read once at the room's load (UpdateOnce)"))
                elif neg:
                    proofs.add((event, scene, f"flag {name}: read negated: another game's 1 only lowers the trigger; "
                                + ("its 0 is never sent (one way)" if name in one_way else
                                   "a saved room flag the game never clears (a 0 comes only with a save load)")))
                else:
                    guards.add(("flag", scene, event, name, 0, 0, "a shared flag"))
                continue
            if kind == "node":
                proofs.add((event, scene, f"{name}.{plug}: our own game's state (our Link, our room)"))
                continue
            if SILVER_COUNTER.match(name):
                if hash_ not in keys:
                    proofs.add((event, scene, f"{name}.{plug}: counter {hash_:#x} not shared"))
                    continue
                guards.add(("key", scene, event, "", hash_, 0, f"{name}'s own key"))
                for t in links:
                    if t in keys:
                        guards.add(("token", scene, event, "", t, hash_, f"a token of {name}"))
                continue
            if plug in KILL_PLUGS or (plug == "Logic_Alive" and neg):
                guards.add(("kill", scene, event, "", hash_, 0, f"{name}.{plug}"))
                continue
            if plug == "Logic_Alive":
                proofs.add((event, scene, f"{name}.Logic_Alive: a kill or remove only lowers it"))
                continue
            if hash_ in keys:
                guards.add(("key", scene, event, "", hash_, 0, f"{name}'s key"))
                continue
            followed = follow(scene, event, name, hash_, place)
            if followed:
                guards |= followed
                continue
            near = [(p, guard) for at, p, guard in plugs.get((scene, name), [])
                    if p == plug and sum((a - b) ** 2 for a, b in zip(at, place)) < 1.5 ** 2]
            if near and all(guard for _, guard in near):
                # g3-objects' guarded line: the share holds the plug only once the guard flags are all on in our
                # game, and those disarm our asker, so the shared plug cannot make it ask the event.
                proofs.add((event, scene, f"{name}.{plug}: held by the switch share only behind its guard "
                                          f"{' / '.join(sorted({g for _, g in near}))} (on: our asker is disarmed)"))
            elif near:
                proofs.add((event, scene, f"{name}.{plug}: HELD BY THE SWITCH SHARE (not covered)"))
            elif any(is_movable(other) and other_room is not None and other_room == room_of(scene, hash_)
                     for other, other_room, _, _ in world.get(scene, {}).values()):
                proofs.add((event, scene, f"{name}.{plug}: each game's own (a followed block in its room cannot "
                                          "drive it: a block rests only on press switches; no shared flag, key, kill "
                                          "or switch share line)"))
            else:
                proofs.add((event, scene, f"{name}.{plug}: each game's own (no shared flag, key, kill, switch "
                                          "share line or followed world object that can drive it)"))
    # One line per (kind, scene, event, id, group): a block can be a trigger actor and rest on a press switch too.
    once = {}
    for g in sorted(guards):
        once.setdefault(g[:6], g)
    return sorted(once.values()), sorted(proofs)


def shared_keys(path):
    """The placement keys of lichii/hook/coop_dungeon.cpp's kKeys (shared one-time keys)."""
    with open(path, encoding="utf-8") as f:
        return {int(k, 16) for k in re.findall(r'\{0x([0-9a-f]{16}), "\w+", "\w+"\}', f.read())}


def one_way_flags(path):
    """The flag names of coop_dungeon.cpp's kLiveFlags (shared one way: a 0 is never sent)."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    at = text.find("kLiveFlags = ")
    block = text[at: text.find("});", at)] if at >= 0 else ""
    return set(re.findall(r'\{0x[0-9a-f]+, (?:true|false), "([A-Za-z0-9_.]+)"\}', block))


def switch_plugs(path):
    """{(scene, switch name): [(place, plug, guard)]} of lichii/hook/coop_room_objects.cpp kSwitchPlugs. Both line
    forms: {scene, name, {x, y, z}, plug} and g3-objects' {scene, name, {x, y, z}, plug, guard} (guard "" = none;
    flags joined by '+', "load:<flag>" allowed)."""
    out = {}
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for scene, name, x, y, z, plug, guard in re.findall(
                    r'\{"(\w+)", "(\w+)", \{([-\d.]+)f, ([-\d.]+)f, ([-\d.]+)f\}, "(\w+)"(?:, "([^"]*)")?\}', f.read()):
                out.setdefault((scene, name), []).append(((float(x), float(y), float(z)), plug, guard))
    return out


def live_places(path):
    """{(event, scene)} of lichii/hook/coop_live_places.cpp."""
    text = open(path, encoding="utf-8").read()
    return set(re.findall(r'\{"(\w+)", "(\w+)", [-\d.]+f', text))


def shared_flags(path):
    """The flag names of lichii/hook/coop_dungeon.cpp's tables (the flags another player's game sends us)."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    return set(re.findall(r'\{0x[0-9a-f]+, (?:true|false), "([A-Za-z0-9_.]+)"\}', text))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--places", help="coop_live_places.cpp: print only events that have a live place")
    ap.add_argument("--dungeon", help="coop_dungeon.cpp: print only guard flags that are shared")
    ap.add_argument("--room-objects", help="coop_room_objects.cpp (the switch share's plugs; default: beside --dungeon)")
    ap.add_argument("--results", action="store_true",
                    help="print every asker of a live-place event with its result class (needs --dungeon)")
    ap.add_argument("--classify", action="store_true",
                    help="print the A/B class of each line whose plug does not latch (needs --dungeon)")
    args = ap.parse_args(argv)
    keep = live_places(args.places) if args.places else None
    shared = shared_flags(args.dungeon) if args.dungeon else None
    wiring = {} if args.classify else None
    askers = {} if args.results else None
    world = {} if args.results else None
    performers = {} if args.results else None
    lines, guards, extras, others = switch_events(args.romfs, wiring=wiring, askers=askers, world=world,
                                                  performers=performers)
    if args.results:
        plays = event_plays(args.romfs, {e for _, e in performers} & {e for e, _, _ in askers})
        print("// kResults (event, scene, asker place, the outputs to set: none for class live)")
        guarded, all_plays, locks = [], set(), []
        for key in sorted(askers):
            event, scene, at = key
            if keep is not None and (event, scene) not in keep:
                continue
            mine = performer_plays(event, scene, performers, plays)
            all_plays |= {(event, scene, *p) for p in mine}
            cls, why = asker_class(key, askers[key], shared or set(), mine)
            outs = "+".join(o for o in ASKER_OUTPUTS if o in askers[key]["outputs"]) if cls == "asker" else ""
            print(f'    {{"{event}", "{scene}", {{{at[0]}f, {at[1]}f, {at[2]}f}}, "{outs}"}},  '
                  f'// {cls} ({askers[key]["actor"]}): {why}')
            if cls == "asker":
                guarded += [(f, scene, event) for f in askers[key]["delete"] if shared is None or f in shared]
                if lock_only(key, askers[key], shared or set(), mine):
                    locks.append((event, scene))
        # An event of a live place with no asker actor (a shutter's or door's own field asks it): nothing of ours to
        # set, its results are the room's shared flags (class live).
        for event, scene in sorted((keep or set()) - {(e, s) for e, s, _ in askers}):
            print(f'    {{"{event}", "{scene}", {{0.0f, 0.0f, 0.0f}}, ""}},  // live (no asker actor: a field asks it)')
        print("// kLocks (class asker only for a shutter's lock, row 336: a set given up lands their flags as class live)")
        for event, scene in sorted(set(locks)):
            print(f'    {{"{event}", "{scene}"}},')
        print("// kPlays (event, scene, performer, its place, the AS our performer plays: row 165)")
        for event, scene, actor, place, asname in sorted(all_plays):
            print(f'    {{"{event}", "{scene}", "{actor}", {{{place[0]}f, {place[1]}f, {place[2]}f}}, "{asname}"}},')
        print("// kResultGuards (a shared flag that deletes an asker of class asker: waits until our result is set)")
        for flag, scene, event in sorted(set(guarded)):
            print(f'    {{"{flag}", "{scene}", "{event}"}},')
        dungeon = args.dungeon or ""
        objects = args.room_objects or os.path.join(os.path.dirname(dungeon), "coop_room_objects.cpp")
        guards, proofs = trigger_guards(askers, keep, shared or set(), shared_keys(dungeon) if dungeon else set(),
                                        one_way_flags(dungeon) if dungeon else set(), switch_plugs(objects), world)
        print("// kTriggers (what another game can raise on our asker's trigger: waits until our game played the event)")
        for kind, scene, event, flag, id_, group, why in guards:
            print(f'    {{Trigger::{kind}, "{scene}", "{event}", "{flag}", {id_:#018x}, {group:#018x}}},  // {why}')
        print("// a trigger leaf shared state cannot raise, and why")
        for event, scene, text in proofs:
            print(f"//   {event}: {text}")
        return 0
    if args.classify:
        two_way = two_way_flags(wiring)
        rows = []
        for line in lines:
            event, scene, switch, at, plug, _ = line
            if (keep is not None and (event, scene) not in keep) or switch.startswith("Obj_EyeSwitch") or \
                    plug == "FirstOn" or line not in wiring:
                continue
            rows.append((line, *classify(line, wiring[line], shared or set(), two_way)))
        print("// kInputs (class B, the switch's ON input unwired: set it)")
        for (event, scene, switch, at, _, _), _, fix, why in rows:
            if fix.startswith("input "):
                print(f'    {{"{event}", "{scene}", "{switch}", {{{at[0]}f, {at[1]}f, {at[2]}f}}, "{fix[6:]}"}},  // {why}')
        print("// the other lines whose plug does not latch: class (fix) and why")
        for (event, _, switch, _, _, _), cls, fix, why in rows:
            if not fix.startswith("input "):
                print(f"//   {cls}{' (' + fix + ')' if fix else ''} {event} ({switch}): {why}")
        return 0
    wanted = {(e, s) for e, s, *_ in lines if keep is None or (e, s) in keep}
    print("// kSwitches")
    for event, scene, switch, s, plug, a in lines:
        if (event, scene) in wanted:
            print(f'    {{"{event}", "{scene}", "{switch}", {{{s[0]}f, {s[1]}f, {s[2]}f}}, "{plug}", '
                  f'{{{a[0]}f, {a[1]}f, {a[2]}f}}}},')
    print("// kExtras")
    for event, scene, actor, p, plug in extras:
        if (event, scene) in wanted:
            print(f'    {{"{event}", "{scene}", "{actor}", {{{p[0]}f, {p[1]}f, {p[2]}f}}, "{plug}"}},')
    print("// kGuards")
    whys = {}
    for flag, scene, event, why in guards:
        if (event, scene) in wanted and (shared is None or flag in shared):
            whys.setdefault((flag, scene, event), set()).add(why)
    for (flag, scene, event), why in sorted(whys.items()):
        print(f'    {{"{flag}", "{scene}", "{event}"}},  // {" + ".join(sorted(why))}')
    print("// not covered (other inputs on a trigger: a guard flag that is not shared, a player state, an OR branch)")
    for event, scene, what in others:
        if (event, scene) in wanted:
            print(f"//   {event}: {what}")
    for flag, scene, event, why in guards:
        if (event, scene) in wanted and shared is not None and flag not in shared:
            print(f"//   {event}: {flag} ({why}; not shared: each game's own)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
