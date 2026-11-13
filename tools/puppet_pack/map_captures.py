"""Writes coop_maps.txt: where each area's and dungeon's map picture stands.

Made on the player's PC from their own game, with the puppet pack: build.py
writes <mod folder>/coop_maps.txt beside coop_rooms.txt, and the hook reads it
at start (coop/overlay/map_icons.hpp, parse_captures) to put the other players
on the minimap and the pause menu's map. Nothing from the game is in this tool.
On its own (to look at a table):

    python -m puppet_pack.map_captures --romfs <romfs> --out <file>

The game makes each map picture by looking straight down from above:
Game/MapCaptureInfo's entries (CameraPos, Height: the metres from the
picture's top to its bottom; per floor the camera's Near and Far planes).
Its entries carry no names; they are in the order of Game/MapMenuInfo's
entries (LocationName, BancName), which do: entry i of one is the map of
entry i of the other (the dungeons' entries are the same in both: they are
the ones with floors and IsDungeon). Entries past MapMenuInfo's, and ones
without a camera, are left out.
"""
import argparse
import glob
import os
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml  # noqa: E402

CAPTURE = "Game/MapCaptureInfo/*.bgyml"
MENU = "Game/MapMenuInfo/*.bgyml"
FILES = (CAPTURE, MENU)


def _num(v):
    return float(v)


def _plane(v):
    return "-" if v is None else f"{_num(v):g}"


def captures(capture_root, menu_root):
    """[{location, scenes, x, z, camera_y, height, dungeon, floors: [(near, far)]}] from the two
    files' roots."""
    caps = capture_root.get("MapCaptureParamArray") or []
    menu = menu_root.get("MapMenuParamArray") or []
    out = []
    for cap, entry in zip(caps, menu):
        pos, height, name = cap.get("CameraPos"), cap.get("Height"), entry.get("LocationName")
        if not pos or height is None or not name or _num(height) <= 0:
            continue
        scenes = [s for s in dict.fromkeys(entry.get("BancName") or []) if s]
        scenes += [c["BancName"] for c in entry.get("ChangeBancInfo") or [] if c.get("BancName")]
        floors = []
        for key in sorted(cap.get("CaptureParamData") or {}):
            p = (cap["CaptureParamData"][key] or {}).get("MapCaptureParam") or {}
            floors.append((p.get("Near"), p.get("Far")))
        dungeon = bool(cap.get("IsDungeon")) or len(set(entry.get("BancName") or [])) == 1
        out.append({"location": str(name), "scenes": [str(s) for s in scenes] or [str(name)],
                    "x": _num(pos.get("X", 0)), "z": _num(pos.get("Z", 0)), "camera_y": _num(pos.get("Y", 0)),
                    "height": _num(height), "dungeon": dungeon, "floors": floors or [(None, None)]})
    return out


def render(caps):
    """The coop_maps.txt text (map_icons.hpp parse_captures)."""
    lines = ["# coop_maps.txt, made by tools/puppet_pack from this game's Game/MapCaptureInfo and MapMenuInfo.",
             "# map location centre_x centre_z camera_y height dungeon scenes near:far per floor"]
    for c in caps:
        floors = " ".join(f"{_plane(n)}:{_plane(f)}" for n, f in c["floors"])
        lines.append(f"map {c['location']} {c['x']:g} {c['z']:g} {c['camera_y']:g} {c['height']:g} "
                     f"{1 if c['dungeon'] else 0} {','.join(c['scenes'])} {floors}")
    return "\n".join(lines) + "\n"


def _load(romfs, pattern):
    hits = sorted(glob.glob(os.path.join(romfs, *pattern.split("/"))))
    if not hits:
        return {}
    with open(hits[0], "rb") as f:
        return byml.loads(f.read())


def write_table(romfs, path):
    """Writes the table to `path`; returns how many maps it has (0: the files are missing)."""
    caps = captures(_load(romfs, CAPTURE), _load(romfs, MENU))
    with open(path, "w", newline="\n", encoding="utf-8") as f:
        f.write(render(caps))
    return len(caps)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    n = write_table(args.romfs, args.out)
    print(f"{args.out}: {n} maps")
    return 0 if n else 1


if __name__ == "__main__":
    sys.exit(main())
