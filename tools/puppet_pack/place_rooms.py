"""Adds each live-event place's room to lichii/hook/coop_live_places.cpp (the last field of a Place).

Run by a developer against their own romfs dump; nothing from the game is in this tool:

    python -m puppet_pack.place_rooms --romfs <romfs> --places lichii/hook/coop_live_places.cpp [--write]

Why (deku-ladder, LANE1 run hs-and-gate 2026-11-16): the live-event decider starts another player's room event
here only when our Link is in the room of the event's place, so a place near our Link "through a wall" waits. It
guessed the place's room like a player's: the room of the nearest placement that has a room index
(coop_rooms.h). An asker (EventRequestActor) has no room index of its own and often stands high on a wall, nearer
another room's placements: Jabu-Jabu's room-1 switch asker (7.3 109.7 -48.1) was guessed in room 5 while it, its
switch and the players at the switch are in room 17, so the event never started. 46 of the 204 places were guessed
in another room than their own.

Their own room: the asker belongs to its room's logic graph (an AiGroup in Banc/<scene>/*.bcett), and the graph's
other members carry `Dynamic.DefaultDungeonRoomIndex`. The room most of them carry is the asker's room (doors and
shutters left out: they stand between two rooms). The group's own `Dynamic.DungeonRoomID` is not used: it differs
from its members' rooms in 79 of 383 cases (Ganon's Castle's room-12 chest graph says 1). A place with no such
group keeps -1: the hook guesses it as before.

Kept only when usable (review round 5): the decider compares the place's room with OUR LINK's guessed room, so a
vote is kept only if (a) it is not a tie (Jabu's GreenTentacle asker: {3: 1, 2: 1}) and (b) a Link near the place
is guessed in that room often enough: on a 3 m grid within kNearM (18 m) of the place, Link's guess (the same
rule and points as coop_rooms.h: nearest placement, height x2, within kFar 25 m) names that room in at least
REACH_SHARE of the samples (Ganon's Castle's BarrierRemove_Water votes room 7 from one member 45 m off; Link near
it is only ever in 5 or 6). Otherwise -1 (the live guess, as before), and the tool says which and why.
"""
import argparse
import collections
import glob
import itertools
import math
import os
import re
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml, dungeon_rooms, zs  # noqa: E402

KNEAR_M = 18.0     # coop_live_events.h kNearM
KFAR_M = 25.0      # coop_rooms.h kFar
HEIGHT_W = 2.0     # coop_rooms.h kHeightWeight
GRID_M = 3.0
REACH_SHARE = 0.10

LINE = re.compile(r'^(\s*\{"(\w+)", "(\w+)", ([-\d.]+)f, ([-\d.]+)f, ([-\d.]+)f)(?:, -?\d+)?(\},.*)$')


def _int(v):
    return int(v.v if hasattr(v, "v") else v)


def _place(actor):
    return tuple(round(float(x), 1) for x in (actor.get("Translate") or [0, 0, 0]))


def choose(votes):
    """The room of a vote ({room: count}), or None for no vote or a tie."""
    if not votes:
        return None
    ranked = collections.Counter(votes).most_common()
    if len(ranked) > 1 and ranked[0][1] == ranked[1][1]:
        return None
    return ranked[0][0]


def guess(points, at):
    """Link's room at `at` (coop_rooms.h): the room of the nearest placement, height weighted, within kFar; or None.
    `points`: [(room, x, y, z)] of one scene."""
    best = None
    for room, x, y, z in points:
        d = math.sqrt((at[0] - x) ** 2 + (HEIGHT_W * (at[1] - y)) ** 2 + (at[2] - z) ** 2)
        if best is None or d < best[0]:
            best = (d, room)
    return best[1] if best is not None and best[0] <= KFAR_M else None


def reach_share(points, at, room):
    """The share of grid samples within kNearM of `at` that Link's guess puts in `room`."""
    n = int(KNEAR_M // GRID_M)
    steps = [i * GRID_M for i in range(-n, n + 1)]
    seen = collections.Counter()
    for dx, dy, dz in itertools.product(steps, steps, steps):
        if dx * dx + dy * dy + dz * dz > KNEAR_M * KNEAR_M:
            continue
        g = guess(points, (at[0] + dx, at[1] + dy, at[2] + dz))
        if g is not None:
            seen[g] += 1
    total = sum(seen.values())
    return seen[room] / total if total else 0.0


def place_room(votes, points, at):
    """(room or -1, why) for one place: its vote unless a tie or a room Link near it is (almost) never in."""
    room = choose(votes)
    if room is None:
        return -1, (f"a tie {dict(votes)}" if votes else "no room graph")
    share = reach_share(points, at, room)
    if share < REACH_SHARE:
        return -1, f"room {room} voted, but Link near it is guessed there in {share:.0%} of samples"
    return room, ""


def placement_votes(romfs, dicts=None):
    """{(scene, (x, y, z)): {room: count}} for every placement without a room index of its own in a room's graph."""
    if dicts is None:
        dicts = zs.Dicts.from_romfs(romfs)
    out = {}
    for folder in sorted(glob.glob(os.path.join(romfs, "Banc", "*"))):
        scene = os.path.basename(folder)
        actors, groups = [], []
        for path in glob.glob(os.path.join(folder, "*.bcett.byml.zs")):
            with open(path, "rb") as f:
                root = byml.loads(dicts.decompress(f.read()))
            actors += root.get("Actors", []) or []
            groups += root.get("AiGroups", []) or []
        by_hash = {_int(a["Hash"]): a for a in actors if "Hash" in a}
        for g in groups:
            members = [by_hash.get(_int(r["Reference"])) for r in g.get("References", []) or [] if "Reference" in r]
            members = [a for a in members if a is not None]
            votes = collections.Counter(
                int(a["Dynamic"]["DefaultDungeonRoomIndex"]) for a in members
                if (a.get("Dynamic") or {}).get("DefaultDungeonRoomIndex") is not None
                and "Shutter" not in str(a.get("Gyaml")) and "Door" not in str(a.get("Gyaml")))
            if not votes:
                continue
            for a in members:
                if (a.get("Dynamic") or {}).get("DefaultDungeonRoomIndex") is None:
                    out.setdefault((scene, _place(a)), dict(votes))
    return out


def room_points(romfs, dicts=None):
    """{scene: [(room, x, y, z)]}: the placements Link's room is guessed from (dungeon_rooms.py, thinned)."""
    rooms = dungeon_rooms.scene_rooms(romfs, dicts)
    return {scene: [(room, *p) for room, pts in by_room.items() for p in dungeon_rooms.thin(pts)]
            for scene, by_room in rooms.items()}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--places", required=True)
    ap.add_argument("--write", action="store_true", help="rewrite the places file (else print it)")
    args = ap.parse_args(argv)
    dicts = zs.Dicts.from_romfs(args.romfs)
    votes = placement_votes(args.romfs, dicts)
    points = room_points(args.romfs, dicts)
    lines = open(args.places, encoding="utf-8").read().split("\n")
    known = 0
    for i, line in enumerate(lines):
        m = LINE.match(line)
        if not m:
            continue
        head, event, scene, x, y, z, tail = m.groups()
        at = (round(float(x), 1), round(float(y), 1), round(float(z), 1))
        room, why = place_room(votes.get((scene, at), {}), points.get(scene, []), at)
        if room < 0 and why != "no room graph":
            print(f"place_rooms: {event} ({scene}): -1, {why}", file=sys.stderr)
        known += room >= 0
        lines[i] = f"{head}, {room}{tail}"
    text = "\n".join(lines)
    if args.write:
        open(args.places, "w", encoding="utf-8", newline="\n").write(text)
    else:
        print(text)
    print(f"place_rooms: {known} places with their own room", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
