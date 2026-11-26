"""Shared block anchors: every Obj_MovableBox_BasePosAnchor that more than one placed movable links to (row 175).

lichii/hook/coop_world.cpp moves a block's base anchor to where another player left the block (issue 147). It may only
move the block's OWN anchor. Some puzzles push several movables from ONE anchor. Amy's blocks in the Forest Temple
are one case; the two Lon Lon boxes in Hyrule Castle are another, and there one box stands exactly on the anchor.
Moving such an anchor for one block would shift the others in that game. This prints the C++ table
coop_world.cpp's kSharedAnchors (scene, place) from the romfs Banc.

    python -I tools/puppet_pack/shared_anchors.py --romfs <dump>/romfs
"""
import argparse
import collections
import glob
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from puppet_pack import byml, zs  # noqa: E402
from puppet_pack.eye_switches import is_movable  # noqa: E402

ANCHOR = "Obj_MovableBox_BasePosAnchor"


def shared_in(actors):
    """[(anchor place, [(movable name, place), ...])] for each anchor that two or more movables link to."""
    by_hash = {str(a.get("Hash")): a for a in actors if "Hash" in a}
    users = collections.defaultdict(list)
    for a in actors:
        if not is_movable(a.get("Gyaml", "")):
            continue
        for link in a.get("Links", []) or []:
            dst = by_hash.get(str(link.get("Dst")))
            if dst is not None and dst.get("Gyaml") == ANCHOR:
                users[str(link.get("Dst"))].append(a)
    out = []
    for key, movables in users.items():
        if len(movables) > 1:
            place = [float(v) for v in by_hash[key]["Translate"]]
            out.append((place, sorted((m["Gyaml"], [float(v) for v in m["Translate"]]) for m in movables)))
    return sorted(out)


def scan(romfs):
    """{scene: shared_in(its actors)} for every Banc scene that has one."""
    d = zs.Dicts.from_romfs(romfs)
    got = {}
    for scene in sorted(os.listdir(os.path.join(romfs, "Banc"))):
        actors = []
        for path in sorted(glob.glob(os.path.join(romfs, "Banc", scene, "*.bcett.byml.zs"))):
            with open(path, "rb") as f:
                actors += byml.loads(d.decompress(f.read())).get("Actors", []) or []
        found = shared_in(actors)
        if found:
            got[scene] = found
    return got


def cpp_lines(got):
    lines = []
    for scene, found in got.items():
        for place, movables in found:
            near = min(math.dist(place, p) for _, p in movables)
            lines.append('    {"%s", {%.4ff, %.4ff, %.4ff}},  // %d movables, nearest %.2f m' %
                         (scene, place[0], place[1], place[2], len(movables), near))
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--romfs", required=True)
    args = ap.parse_args(argv)
    print("\n".join(cpp_lines(scan(args.romfs))))


if __name__ == "__main__":
    main()
