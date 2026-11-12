"""Writes coop_rooms.txt: where each dungeon room's placements stand.

Made on the player's PC from their own game, with the puppet pack: build.py
writes <mod folder>/coop_rooms.txt beside coop_puppet.json, and the hook reads
it at start (lichii/hook/coop_rooms.h). Nothing from the game is in this tool.
On its own (to look at a table):

    python -m puppet_pack.dungeon_rooms --romfs <romfs> --out <file>

A dungeon is one scene (InsideTheDekuTree, ...), and its rooms are not scenes
of their own: a placement's room is its `Dynamic.DefaultDungeonRoomIndex`
(docs/dungeons-boss.md). The game's own "which room is Link in" is not mapped,
so the hook guesses it from these points: a position is in the room of the
nearest placement (coop_rooms.h). Issue 145: another player's Navi is shown
only while that player is in our room.

Kept: every placement with a room index, in Banc/<area>/<scene>_Static and
_Dynamic, thinned so two points of one room are at least THIN apart.
"""
import argparse
import glob
import math
import os
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml, zs  # noqa: E402

THIN = 1.0


def scene_rooms(romfs, dicts=None):
    """{scene: {room: [[x, y, z], ...]}}, every placement with a room index."""
    if dicts is None:
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
            room = (actor.get("Dynamic") or {}).get("DefaultDungeonRoomIndex")
            if room is None:
                continue
            t = actor.get("Translate") or [0, 0, 0]
            out.setdefault(scene, {}).setdefault(int(room), []).append([float(v) for v in t])
    return out


def thin(points, step=THIN):
    """`points` in order, without any closer than `step` to one already kept."""
    kept = []
    for p in points:
        if all(math.dist(p, k) >= step for k in kept):
            kept.append(p)
    return kept


def render(rooms):
    """The coop_rooms.txt text: "scene room x y z" a line (coop_rooms.h Parse)."""
    lines = ["# coop_rooms.txt, made by tools/puppet_pack from this game's scene files.",
             "# scene room(DefaultDungeonRoomIndex) x y z"]
    for scene in sorted(rooms):
        for room in sorted(rooms[scene]):
            for x, y, z in thin(rooms[scene][room]):
                lines.append(f"{scene} {room} {x:.2f} {y:.2f} {z:.2f}")
    return "\n".join(lines) + "\n"


def write_table(romfs, path, dicts=None):
    """Writes the table to `path`; returns (scenes, rooms, points)."""
    rooms = scene_rooms(romfs, dicts)
    text = render(rooms)
    with open(path, "w", newline="\n", encoding="utf-8") as f:
        f.write(text)
    points = sum(1 for line in text.splitlines() if not line.startswith("#"))
    return len(rooms), sum(len(r) for r in rooms.values()), points


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    scenes, rooms, points = write_table(args.romfs, args.out)
    print(f"{args.out}: {scenes} scenes, {rooms} rooms, {points} placements")
    return 0 if scenes else 1


if __name__ == "__main__":
    sys.exit(main())
