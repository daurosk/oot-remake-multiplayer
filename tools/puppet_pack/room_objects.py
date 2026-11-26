"""Prints the room-object tables of lichii/hook/coop_room_objects.cpp (kPlacements, kRiderNames).

Run by a developer against their own romfs dump; nothing from the game is in
this tool:

    python -m puppet_pack.room_objects --romfs <romfs>

The rule (room-objects, issues 196, 249, 194): in a game that follows another,
a room object moves only through its own logic, the input its own switch,
sensor or rider would set (CoopEyeSwitch's switch plug, CoopLiftRide's
RideOnPlayer, the push block's node), never by a word written into it from
outside. The clock path (coop_platforms.h) is for free-running movers only:
whose logic node in the room graph has no wired control input, and whose
logic type has no rider plug.

For every placement of a platform name (CoopPlatforms::IsDriver, the same
name rule as below) this prints one line: scene, name, placement, and whether
it is driven by its own logic and why: the wired input names (the room graph
feeds them from a switch, a flag, a sensor or an event) or `RideOnPlayer` (its
logic type has that plug: a rider lift). Inputs that only reset or warp it
(Logic_Create, Logic_Delete, RespawnTrigger, ForceWarp, IisForceWarp) do not
drive it. A placement with no logic node is free.

Then the names whose logic type has the bool output RideOnPlayer (the rider
lifts CoopLiftRide drives). Riders are taken by their logic, whatever their
name (fable-g3-objects: the name rule missed the Fire Temple's
Obj_FireTemple_RiseFloor_A_02 and Ganon's Castle's
Obj_InsideGanonCastle_ElevatedFloor_A_01, the same logic as the Forest
Temple's elevators).

    python -m puppet_pack.room_objects --romfs <romfs> --groups [--dungeon coop_dungeon.cpp]

prints the placement groups with a saved flag (kGroupFlags): actors that share
a Phive.Placement.GroupGUID with a GroundControlActor_ResidentMoveTo, which
moves the whole group when the flag the scene's PlgGdTable gives that group
changes (a water plane, a cage, a hanging floor, enemy air walls). That flag is
the group's only input, so co-op shares the flag and never writes a member's
place. With --dungeon each line says whether coop_dungeon.cpp's list has it.
"""
import argparse
import glob
import os
import re
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml, zs  # noqa: E402
from .eye_switches import SWITCH, _int, _place, _prefix, load_guard, read_graph, switch_events  # noqa: E402

PARTS = ("ElevatorLift", "_Elevator_", "MoveFloor", "RotateFloor", "RotateBlade", "MoveSpike",
         "ExtendingPlatform", "_lift_", "ElevatorMirror", "ElevatorPit", "UpDown")
NOT_DRIVING = {"Logic_Create", "Logic_Delete", "RespawnTrigger", "ForceWarp", "IisForceWarp"}
RIDER_PLUG = "RideOnPlayer"


def is_driver(name):
    """CoopPlatforms::IsDriver."""
    if name.startswith(("GroundControlActor_", "GrouondControlActor_")):
        return True
    if not name.startswith(("Obj_", "Land_")):
        return False
    return any(p in name for p in PARTS)


def wired_inputs(node):
    """The input names of a logic node that the graph feeds (not a constant, not reset-only)."""
    out = set()
    for t in ("bool", "int", "float", "vec3"):
        for name, src, _ in node["in"][t]:
            if name not in NOT_DRIVING and (src >= 0 or src <= -100):
                out.add(name)
    return out


def is_rider(node, name):
    """A room object whose logic has the rider plug (CoopLiftRide sets it for another player on it)."""
    return name.startswith(("Obj_", "Land_")) and RIDER_PLUG in node["out"]["bool"]


CONTROLLER = "GroundControlActor_ResidentMoveTo"


def groups_of(actors, table):
    """[(flag, type, controller place, member names)] of one scene's Banc.

    `actors`: the placements; `table`: its PlgGdTable ([{PlgGuid, GameDataPath, GameDataType}]). A group is every
    placement with one Phive.Placement.GroupGUID; only groups the table gives a flag are returned. The place is the
    group's ResidentMoveTo controller's (else its first member's); the members are the other names, sorted.
    """
    flags = {_int(e["PlgGuid"]): (str(e["GameDataPath"]).split("?")[-1], str(e.get("GameDataType", "Bool")))
             for e in table or []}
    members = {}
    for a in actors:
        guid = ((a.get("Phive") or {}).get("Placement") or {}).get("GroupGUID")
        if guid is not None and _int(guid) in flags:
            members.setdefault(_int(guid), []).append(a)
    out = []
    for guid, group in members.items():
        ctl = [a for a in group if str(a.get("Gyaml", "")) == CONTROLLER]
        names = sorted(str(a.get("Gyaml", "")) for a in group if str(a.get("Gyaml", "")) != CONTROLLER)
        flag, kind = flags[guid]
        out.append((flag, kind, _place((ctl or group)[0]), tuple(names)))
    return sorted(out)


def placement_groups(romfs, dicts=None):
    """[(scene, flag, type, place, member names)] for every scene: groups_of over its Banc files."""
    if dicts is None:
        dicts = zs.Dicts.from_romfs(romfs)
    rows = []
    for folder in sorted(glob.glob(os.path.join(romfs, "Banc", "*"))):
        scene = os.path.basename(folder)
        actors, table = [], []
        for path in sorted(glob.glob(os.path.join(folder, "*.bcett.byml.zs"))):
            with open(path, "rb") as f:
                root = byml.loads(dicts.decompress(f.read()))
            actors += root.get("Actors", []) or []
            table += root.get("PlgGdTable", []) or []
        rows += [(scene,) + g for g in groups_of(actors, table)]
    return sorted(set(rows))


def shared_names(path):
    """The flag names of lichii/hook/coop_dungeon.cpp's kFlags (the list before the placement keys)."""
    text = open(path, encoding="utf-8").read()
    text = text[text.index("kFlags"):]
    text = text[: text.index("});")]
    return set(re.findall(r'\{0x[0-9a-f]{8}, (?:false|true), "([^"]+)"\}', text))


def room_objects(romfs, dicts=None):
    """([(scene, name, place, why)], rider names): why is '' for a free placement."""
    if dicts is None:
        dicts = zs.Dicts.from_romfs(romfs)
    graphs = {}

    def graph(logic):
        path = os.path.join(romfs, "Logic", os.path.basename(str(logic)) + "b")
        if path not in graphs:
            graphs[path] = read_graph(open(path, "rb").read()) if os.path.exists(path) else None
        return graphs[path]

    rows, riders = {}, set()
    for folder in sorted(glob.glob(os.path.join(romfs, "Banc", "*"))):
        scene = os.path.basename(folder)
        actors, groups = [], []
        for path in sorted(glob.glob(os.path.join(folder, "*.bcett.byml.zs"))):
            with open(path, "rb") as f:
                root = byml.loads(dicts.decompress(f.read()))
            actors += root.get("Actors", []) or []
            groups += root.get("AiGroups", []) or []
        why = {}
        for a in actors:
            name = str(a.get("Gyaml", ""))
            if is_driver(name) and "Hash" in a:
                why.setdefault(_int(a["Hash"]), set())
        by_hash = {_int(a["Hash"]): a for a in actors if "Hash" in a}
        for g in groups:
            refs = g.get("References", []) or []
            mods = [("", g.get("Logic", ""))] + [(_prefix(r["Path"]), r["Logic"]) for r in refs if "Logic" in r]
            by_id = {(_prefix(r["Path"]), r["Id"]): _int(r["Reference"])
                     for r in refs if "Reference" in r and _int(r["Reference"]) in by_hash}
            for pre, logic in mods:
                g_ = graph(logic) if logic else None
                if g_ is None:
                    continue
                for node in g_[0]:
                    h = by_id.get((pre, node["imm"].get("InstanceName")))
                    if h is None:
                        continue
                    rider = is_rider(node, str(by_hash[h].get("Gyaml", "")))
                    if rider:
                        riders.add(str(by_hash[h]["Gyaml"]))  # by its logic, whatever its name
                    if h not in why:
                        continue
                    why[h] |= wired_inputs(node)
                    if rider:
                        why[h].add(RIDER_PLUG)
        # A part with no logic of its own placed where a driven one is (the Deku Tree's ElevatorLiftLim_A_0n on its
        # ElevatorLift_A_0n) moves with it: driven too.
        driven = [(by_hash[h], r) for h, r in why.items() if r]
        for h, reasons in why.items():
            if reasons:
                continue
            p = _place(by_hash[h])
            for other, r in driven:
                q = _place(other)
                if sum((p[i] - q[i]) ** 2 for i in range(3)) <= 0.25:
                    reasons.add("with " + str(other["Gyaml"]))
        for h, reasons in why.items():
            a = by_hash[h]
            key = (scene, str(a["Gyaml"]), _place(a))
            rows[key] = "+".join(sorted(reasons | set(rows.get(key, "").split("+")) - {""}))
    return sorted((s, n, p, w) for (s, n, p), w in rows.items()), sorted(riders)


ASKERS = ("ActorLogicEventRequestActor", "ActorLogicEventProxyRequestActor")


def leaf_kind(leaf, eye_lines):
    """How a leaf's value reaches a game that follows another (room-objects delivery map)."""
    kind = leaf[0]
    if kind == "actor":
        _, scene, gyaml, place, plug, ltype = leaf
        if SWITCH.match(gyaml):
            if (scene, gyaml, place, plug) in eye_lines:
                return f"{gyaml}.{plug}: CoopEyeSwitch line (the remote event sets it)"
            return f"{gyaml}.{plug}: SWITCH_STATE (the presser's plug, set on ours)"
        if ltype in ASKERS:
            return f"{gyaml}.{plug}: our asker, started by its switch (CoopEyeSwitch) or DUNGEON_EVENT"
        return f"{gyaml}.{plug}: that actor's own state"
    _, ntype, imm = leaf
    if ntype.startswith("HoldGameDataGetBool") or ntype.startswith("OperateGameData"):
        return f"{ntype}: a game flag (shared flags)"
    if "SensorHitInfo" in ntype:
        return f"{ntype}: a player's contact (our Link; another player's by CoopLiftRide where it has RideOnPlayer)"
    if "Blackboard" in ntype:
        return f"{ntype}: the actor's own blackboard (its own state)"
    if not ntype:
        return f"module input {imm.get('Name')} with no caller"
    return f"{ntype}: own logic node"


def room_switches(romfs, eye_lines=frozenset(), dicts=None, shared_flags=None):
    """(shared switch plugs, delivery) from the room graphs.

    `shared_flags` (coop_dungeon.cpp's flag names, g3-objects): a flag leaf not in it is followed on to its setters' inputs
    in the same group, since that flag is each game's own; None: every flag leaf ends the walk (as before).

    Shared: [(scene, switch name, place, plug, consumers)] for every switch output plug that a room object (not a
    switch, not an event asker) reads, straight or through gates, timers, delays and module inputs, and that is not a
    CoopEyeSwitch line (`eye_lines`: (scene, name, place, plug)). These are the presses SWITCH_STATE carries.
    Delivery: {(scene, name, place): [(input, leaf kind)]} for the platform placements (is_driver).
    """
    if dicts is None:
        dicts = zs.Dicts.from_romfs(romfs)
    graphs = {}

    def graph(logic):
        path = os.path.join(romfs, "Logic", os.path.basename(str(logic)) + "b")
        if path not in graphs:
            graphs[path] = read_graph(open(path, "rb").read()) if os.path.exists(path) else None
        return graphs[path]

    shared, delivery = {}, {}
    for folder in sorted(glob.glob(os.path.join(romfs, "Banc", "*"))):
        scene = os.path.basename(folder)
        actors, groups = [], []
        for path in sorted(glob.glob(os.path.join(folder, "*.bcett.byml.zs"))):
            with open(path, "rb") as f:
                root = byml.loads(dicts.decompress(f.read()))
            actors += root.get("Actors", []) or []
            groups += root.get("AiGroups", []) or []
        by_hash = {_int(a["Hash"]): a for a in actors if "Hash" in a}
        for g in groups:
            refs = g.get("References", []) or []
            mods = [("", g.get("Logic", ""), None)]
            mods += [(_prefix(r["Path"]), r["Logic"], r.get("InstanceName")) for r in refs if "Logic" in r]
            by_id = {(_prefix(r["Path"]), r["Id"]): by_hash[_int(r["Reference"])]
                     for r in refs if "Reference" in r and _int(r["Reference"]) in by_hash}

            def leaves(pre, g_, src, outp, depth, seen):
                nodes, multi = g_
                if src <= -100:
                    return [x for a, b in multi(-100 - src, outp) for x in leaves(pre, g_, a, b, depth + 1, seen)]
                if src < 0 or src >= len(nodes) or depth > 16 or (id(g_), src) in seen:
                    return []
                seen.add((id(g_), src))
                node = nodes[src]
                outs = node["out"]["bool"]
                plug = outs[outp] if 0 <= outp < len(outs) else ""
                inst = node["imm"].get("InstanceName")
                if inst:
                    actor = by_id.get((pre, inst))
                    if actor is None:
                        return []
                    return [("actor", scene, str(actor.get("Gyaml", "")), _place(actor), plug, node["type"])]
                if not node["type"] and "Name" in node["imm"]:  # a module input: its caller's input
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
                                            found += leaves(ppre, parent, a, b, depth + 1, seen)
                    return found or [("node", "", dict(node["imm"]))]
                flag = node["sdef"].get("GameDataNameBool", "") if node["type"].startswith("HoldGameDataGetBool") else ""
                if flag and shared_flags is not None and flag not in shared_flags:
                    # g3-objects: a flag each game keeps on its own reaches no follower; what sets it in this group
                    # (its setters' inputs: a switch plug, a sensor) is what must reach it.
                    found = []
                    for spre, slogic, _ in mods:
                        sg = graph(slogic) if slogic else None
                        for setter in (sg[0] if sg else []):
                            if setter["type"].startswith("TriggerGameDataSetBool") and \
                                    setter["sdef"].get("GameDataNameBool") == flag:
                                for _, a, b in setter["in"]["bool"]:
                                    found += leaves(spre, sg, a, b, depth + 1, seen)
                    return found or [("node", node["type"], dict(node["imm"]))]
                wired = [(a, b) for _, a, b in node["in"]["bool"] if a >= 0 or a <= -100]
                if not wired:
                    return [("node", node["type"], dict(node["imm"]))]
                return [x for a, b in wired for x in leaves(pre, g_, a, b, depth + 1, seen)]

            for pre, logic, _ in mods:
                g_ = graph(logic) if logic else None
                if g_ is None:
                    continue
                for node in g_[0]:
                    obj = by_id.get((pre, node["imm"].get("InstanceName")))
                    if obj is None or node["type"] in ASKERS:
                        continue
                    name = str(obj.get("Gyaml", ""))
                    if SWITCH.match(name):
                        continue
                    for inp, a, b in node["in"]["bool"]:
                        if inp in NOT_DRIVING or not (a >= 0 or a <= -100):
                            continue
                        found = leaves(pre, g_, a, b, 0, set())
                        if is_driver(name):
                            key = (scene, name, _place(obj))
                            delivery.setdefault(key, set()).update((inp, leaf_kind(x, eye_lines)) for x in found)
                        for x in found:
                            if x[0] == "actor" and SWITCH.match(x[2]) and x[4] and (x[1], x[2], x[3], x[4]) not in eye_lines:
                                shared.setdefault((x[1], x[2], x[3], x[4]), set()).add(name)
    rows = sorted((s, n, p, plug, "+".join(sorted(c))) for (s, n, p, plug), c in shared.items())
    return rows, {k: sorted(v) for k, v in delivery.items()}


def eye_switch_lines(path, driven_only=True):
    """{(scene, switch, place, plug)} of the lichii/hook/coop_eye_switch.cpp lines CoopEyeSwitch DRIVES (all of its
    kSwitches lines with driven_only=False).

    g3-objects: only a line whose plug latches (CoopEyeSwitch::Latches: an eye's OnState, a FirstOn plug, or a
    kInputs line) is set by the remote event; a step, hit, hammer or sun switch's OnState/isLightMax is rewritten by
    the switch's own AI every frame and is not driven (deku-ladder), so a room object reading it needs SWITCH_STATE
    like any other switch plug - but such a plug also raises our own event asker (main prints those apart: holding
    it would replay the gimmick event in the follower's game). Without kSwitches (an old file or a test line): every line, as before.
    """
    text = open(path, encoding="utf-8").read()
    pat = r'\{"\w+", "(\w+)", "(\w+)", \{([-\d.]+)f, ([-\d.]+)f, ([-\d.]+)f\}, "(\w+)"'

    def block(name):
        at = text.find(name)
        if at < 0:
            return None
        body = text[at:]
        return body[: body.index("};")] if "[] = {" in body[:80] else ""

    def parse(chunk):
        return {(sc, sw, (float(x), float(y), float(z)), plug) for sc, sw, x, y, z, plug in re.findall(pat, chunk)}

    switches = block("kSwitches")
    if switches is None:
        return parse(text)
    if not driven_only:
        return parse(switches)
    inputs = {(sc, sw, at) for sc, sw, at, _ in parse(block("kInputs") or "")}
    return {line for line in parse(switches)
            if line[1].startswith("Obj_EyeSwitch") or line[3] == "FirstOn" or line[:3] in inputs}


def asking_plug(key, wiring, shared):
    """How a switch plug that also raises event askers (a CoopEyeSwitch line it does not drive) reaches a follower
    (g3-objects, A #44; owner 07:40 UTC 2026-11-17: gimmick events give the result only, no replay).

    `key` (scene, switch, place, plug); `wiring` eye_switches.switch_events' per-line dict; `shared` the flags
    coop_dungeon.cpp shares. Returns (guards, events, native):
    guards: the flags that, all on in our game, stop EVERY asker this plug raises (each asker's own asker_guards(),
            its shared ones when it has any, else its unshared ones; joined), so a SWITCH_STATE hold then runs only
            the room objects: the result. None when some asker has no guard at all: a hold could start our own copy
            of the event (a replay), never held. A flag the asker reads only at the room's load stays marked
            "load:<flag>" (eye_switches.asker_guards): the C++ gate counts it only if it was on when our room
            loaded (Deku Tree room D's compass asker; a live turn-on deletes nothing).
    events: the askers' events.
    native: (input, flag) for the switch's own ON inputs that read a shared flag some asker's event sets: the
            result reaches the follower's switch through that flag, the game's own path (what a not-held plug
            relies on; [] with guards None: open).
    """
    lines = [(line, w) for line, w in wiring.items() if (line[1], line[2], line[3], line[4]) == key]
    events = sorted({line[0] for line, _ in lines})
    guards = set()
    for _, w in lines:
        # Each guard alone stops its asker. A shared one arrives from the presser's game; when an asker has none, its
        # own (unshared) ones still stop it once our game has them (review-a38-3, Dodongo 17's NaviHelp_17_00/01: on
        # after this player's own first camera focus; until then the plug is not held and that asker never replays).
        own = [f for f in w.get("guards", []) if load_guard(f)[0] in shared] or list(w.get("guards", []))
        if not own:
            guards = None
            break
        guards.update(own)
    sets = {what for _, w in lines for kind, what, detail in w["results"] if kind == "flag" and detail == "set"}
    native = sorted({(inp, what) for _, w in lines for inp, srcs in w["on"]
                     for kind, what, neg in srcs if kind == "flag" and not neg and what in sets and what in shared})
    return (sorted(guards) if guards is not None and lines else None), events, native


def asking_lines(wiring, driven=frozenset()):
    """{(scene, switch, place, plug)} of every switch plug that raises an event asker in the romfs, less the lines
    CoopEyeSwitch drives. `wiring` is eye_switches.switch_events' per-line dict for EVERY asker (review-a38-3: the
    coop_eye_switch.cpp table lists only askers with a live place, so taking the askers from it missed Dodongo 17's
    eye switches and Forest 11's step switch, and those plugs were held blindly = a replay in the follower's game)."""
    return {(line[1], line[2], line[3], line[4]) for line in wiring} - set(driven)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--eye-switches", help="coop_eye_switch.cpp: its lines are left to CoopEyeSwitch")
    ap.add_argument("--delivery", action="store_true", help="print how each driven placement's input reaches a follower")
    ap.add_argument("--groups", action="store_true", help="print the placement groups with a saved flag (kGroupFlags)")
    ap.add_argument("--dungeon", help="coop_dungeon.cpp: with --groups, say which group flags its list has; "
                    "else follow the flags it does not share to their setters")
    args = ap.parse_args(argv)
    if args.groups:
        shared = shared_names(args.dungeon) if args.dungeon else None
        for scene, flag, kind, p, names in placement_groups(args.romfs):
            what = "+".join(dict.fromkeys(names)) or "(nothing else)"  # each member name once
            note = "" if shared is None else ("  // shared" if flag in shared else "  // NOT in coop_dungeon.cpp")
            is_int = "true" if kind == "Int" else "false"
            print(f'    {{"{scene}", "{flag}", {is_int}, {{{p[0]}f, {p[1]}f, {p[2]}f}}, "{what}"}},{note}')
        return 0
    rows, riders = room_objects(args.romfs)
    eye = eye_switch_lines(args.eye_switches) if args.eye_switches else frozenset()
    flags = shared_names(args.dungeon) if args.dungeon else None
    switches, delivery = room_switches(args.romfs, eye, shared_flags=flags)
    if args.delivery:
        for scene, name, p, why in rows:
            if why and not why.startswith("with "):
                print(f"{scene} {name} {p} [{why}]")
                for inp, kind in delivery.get((scene, name, p), []):
                    print(f"    {inp} <- {kind}")
            elif why:
                print(f"{scene} {name} {p} [{why}]: moves with it")
        return 0
    for scene, name, p, why in rows:
        print(f'    {{"{scene}", "{name}", {{{p[0]}f, {p[1]}f, {p[2]}f}}, "{why}"}},')
    print()
    for name in riders:
        print(f'    "{name}",')
    print()
    wiring = {}
    switch_events(args.romfs, wiring=wiring)
    asking = asking_lines(wiring, eye)
    not_held = []
    for scene, name, p, plug, users in switches:
        key = (scene, name, p, plug)
        if key not in asking:
            print(f'    {{"{scene}", "{name}", {{{p[0]}f, {p[1]}f, {p[2]}f}}, "{plug}", ""}},  // {users}')
            continue
        guards, events, native = asking_plug(key, wiring, flags or set())
        if guards is None:
            not_held.append((key, users, events, native))
            continue
        print(f'    {{"{scene}", "{name}", {{{p[0]}f, {p[1]}f, {p[2]}f}}, "{plug}", "{"+".join(guards)}"}},  '
              f'// {users}; asks {"+".join(events)}')
    print("// read by room objects, but the plug also raises an event asker that no flag stops: not held")
    for (scene, name, p, plug), users, events, native in not_held:
        how = ", ".join(f"{inp} <- {flag}" for inp, flag in native) or "OPEN: no shared flag reaches the switch"
        print(f"//   {scene} {name} {{{p[0]}, {p[1]}, {p[2]}}} {plug}: {users}; asks {'+'.join(events)}; {how}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
