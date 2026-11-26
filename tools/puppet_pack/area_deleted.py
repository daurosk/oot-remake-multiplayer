"""Writes lichii/hook/coop_area_deleted.inc: enemy kinds whose delete the game gates on our own Link.

    python -m puppet_pack.area_deleted --romfs <romfs> --out lichii/hook/coop_area_deleted.inc
    python -m puppet_pack.area_deleted --romfs <romfs> --list   (every Enemy_ delete input and its class)

enemy-left-0116: an enemy we own that the game deletes by its own request with no death read is
gone for the others too (OBJECT_REMOVE gone, coop_objects_sync.cpp `LeavesForGood`). Not when the
delete comes from our own Link leaving its area: then the enemy only went in OUR game, and the
copy another player fights must live on (the server hands it over). Such deletes are an
`Enemy_*` placement's delete input (`Logic_Delete`, ...) in its AI group's logic graph whose value
is fed by a node that senses our own Link (`HoldActorObserver*BySensorHitInfo`, `HoldPlayer*`,
an area check). Every other delete input reads what is the same in every game: a game-data flag
(shared by the flag sync), the time of day, a challenge step, a timer.

The classes (--list): `link` (excluded: listed in the .inc) and `shared` (flag / time / step /
timer: gone is told). A trace stops at another placement's output (its own logic is not this
input's); --list names each such stop (`stop:<Gyaml>`), so a delete gated on an Area actor or a
ride (the Shadow Temple boat's Stalfos <- Obj_ShadowTemple_PaddleShip_A_01) is seen, not hidden. The kinds are listed by name, for every scene: one placement gated on our
Link is enough to keep the kind out everywhere.
"""
import argparse
import collections
import glob
import os
import re
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml, zs  # noqa: E402
from .eye_switches import _int, _prefix, read_graph  # noqa: E402

DELETE_INPUT = re.compile(r"Delete|Vanish|Disappear|Erase|Kill|Despawn|Remove", re.I)
OUR_LINK = re.compile(r"Sensor|Player|InArea|Area", re.I)
MAX_DEPTH = 10


def is_enemy(gyaml):
    return gyaml.startswith("Enemy_") and "Dictionary" not in gyaml


STOP = "stop:"


def classify(node_types):
    """`link` when any node feeding the input senses our own Link, else `shared` (stops are not nodes)."""
    return "link" if any(OUR_LINK.search(t) for t in node_types if not t.startswith(STOP)) else "shared"


def delete_inputs(romfs):
    """{(scene, enemy, input): set of node types feeding it} over every scene's AI groups."""
    dicts = zs.Dicts.from_romfs(romfs)
    graphs = {}

    def graph(logic):
        p = os.path.join(romfs, "Logic", os.path.basename(str(logic)) + "b")
        if p not in graphs:
            graphs[p] = read_graph(open(p, "rb").read()) if os.path.exists(p) else None
        return graphs[p]

    found = collections.defaultdict(set)
    for folder in sorted(glob.glob(os.path.join(romfs, "Banc", "*"))):
        scene = os.path.basename(folder)
        actors, groups = [], []
        for path in glob.glob(os.path.join(folder, "*.bcett.byml.zs")):
            root = byml.loads(dicts.decompress(open(path, "rb").read()))
            actors += root.get("Actors", []) or []
            groups += root.get("AiGroups", []) or []
        by_hash = {_int(a["Hash"]): a for a in actors if "Hash" in a}
        for g in groups:
            refs = g.get("References", []) or []
            mods = [("", g.get("Logic", ""))] + [(_prefix(r["Path"]), r["Logic"]) for r in refs if "Logic" in r]
            by_id = {(_prefix(r["Path"]), r["Id"]): by_hash[_int(r["Reference"])]
                     for r in refs if "Reference" in r and _int(r["Reference"]) in by_hash}
            for mpre, logic in mods:
                gg = graph(logic) if logic else None
                if not gg:
                    continue
                nodes, multi = gg

                def back(src, outp, depth, seen, nodes=nodes, multi=multi, mpre=mpre, by_id=by_id):
                    if src <= -100:
                        out = []
                        for a, b in multi(-100 - src, outp):
                            out += back(a, b, depth + 1, seen)
                        return out
                    if src < 0 or src >= len(nodes) or depth > MAX_DEPTH or src in seen:
                        return []
                    seen.add(src)
                    n = nodes[src]
                    inst = n["imm"].get("InstanceName")
                    if inst and (mpre, inst) in by_id:
                        return [STOP + str(by_id[(mpre, inst)].get("Gyaml"))]  # another placement's output
                    out = [n["type"]]
                    for _, a, b in n["in"]["bool"]:
                        if a != -1:
                            out += back(a, b, depth + 1, seen)
                    return out

                for m in nodes:
                    a = by_id.get((mpre, m["imm"].get("InstanceName")))
                    if a is None or not is_enemy(str(a.get("Gyaml"))):
                        continue
                    for name, s, o in m["in"]["bool"]:
                        if s != -1 and DELETE_INPUT.search(name):
                            found[(scene, str(a.get("Gyaml")), name)].update(back(s, o, 0, set()))
    return found


def render(kinds):
    lines = ["// Generated by tools/puppet_pack/area_deleted.py from the romfs: do not edit.",
             "// Enemy kinds whose delete input reads our own Link (enemy-left-0116, LeavesForGood)."]
    lines += [f'"{k}",' for k in sorted(kinds)]
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--out")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args(argv)
    found = delete_inputs(args.romfs)
    kinds = {enemy for (_, enemy, _), types in found.items() if classify(types) == "link"}
    if args.list:
        for (scene, enemy, name), types in sorted(found.items()):
            nodes = sorted(t for t in set(types) if not t.startswith(STOP))
            stops = sorted(t for t in set(types) if t.startswith(STOP))
            print(f"{classify(types):6} | {scene} | {enemy}.{name} | {nodes}" + (f" | {stops}" if stops else ""))
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(render(kinds))
    print(f"{len(found)} enemy delete inputs, {len(kinds)} kinds gated on our Link: {sorted(kinds)}",
          file=sys.stderr)


if __name__ == "__main__":
    main()
