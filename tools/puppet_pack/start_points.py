"""Writes lichii/hook/coop_start_points.inc: the entrance start points of every scene.

    python -m puppet_pack.start_points --romfs <romfs> --out lichii/hook/coop_start_points.inc

/teleport to another scene must name a start point of that scene
(Sequence_ReservedNextStartPosName). Without one the game keeps the start name of
the scene it came from, which the new scene does not have, and the picture stays
black after the load (docs/scenes.md, "The start point").

A scene's start points are its `StartPos` placements (Banc/<area>/<scene>_*.bcett.byml.zs).
Kept: the named ones a loading zone uses, found by the `EntrancePos*` placement the game
puts at (or within 5 m of) each of them. A scene with none of those keeps its plain
named ones; names of cutscene, warp-song, boss-rush or mini-game starts are left out
when anything else is there.
"""
import argparse
import glob
import math
import os
import re
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml, zs  # noqa: E402

SPECIAL = re.compile(
    r"SubEvent|Defeat|Sage|Stone|Bolero|Prelude|Nocturne|Serenade|Minuet|Requiem|_Air|Return|Warp|"
    r"After_|MiniGame|HitTarget|Ending|BeforeBoss|InBattle|Restart|UnderWater"
)
ENTRANCE_NEAR = 5.0


def scene_starts(romfs):
    """{scene: [(name, [x, y, z]), ...]} in file order, every StartPos placement."""
    dicts = zs.Dicts.from_romfs(romfs)
    out = {}
    for path in sorted(glob.glob(os.path.join(romfs, "Banc", "*", "*.bcett.byml.zs"))):
        scene = os.path.basename(path)[: -len(".bcett.byml.zs")]
        for suffix in ("_Static", "_Dynamic"):
            if scene.endswith(suffix):
                scene = scene[: -len(suffix)]
        with open(path, "rb") as f:
            root = byml.loads(dicts.decompress(f.read()))
        for actor in root.get("Actors", []) or []:
            if str(actor.get("Gyaml", "")) != "StartPos":
                continue
            t = actor.get("Translate") or [0, 0, 0]
            out.setdefault(scene, []).append((str(actor.get("Name", "")), [float(v) for v in t]))
    return out


def choose(starts):
    """The start points /teleport may use, from one scene's StartPos list."""
    named, seen = [], set()
    for name, t in starts:
        if name and not name.startswith("EntrancePos") and name not in seen:
            seen.add(name)
            named.append((name, t))
    marks = [t for name, t in starts if name.startswith("EntrancePos")]
    plain = [(n, t) for n, t in named if not SPECIAL.search(n)]
    entrances = [(n, t) for n, t in plain if any(math.dist(t, m) < ENTRANCE_NEAR for m in marks)]
    return entrances or plain or named


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    rows = []
    for scene, starts in sorted(scene_starts(args.romfs).items()):
        for name, t in choose(starts):
            rows.append('    {"%s", "%s", %.2ff, %.2ff, %.2ff},\n' % (scene, name, t[0], t[1], t[2]))
    with open(args.out, "w", newline="\n") as f:
        f.write("// Written by tools/puppet_pack/start_points.py from the game's scene files; do not edit.\n")
        f.write("// {scene, start point name, x, y, z}: the entrance start points /teleport may use.\n")
        f.writelines(rows)
    print(f"{len(rows)} start points written to {args.out}")


if __name__ == "__main__":
    main()
