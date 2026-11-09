"""Test helper: places actors into one scene file of a mod the builder made.

    python -m puppet_pack.place_test --romfs <romfs> --mod-dir <mod folder> \
        --banc Banc/KakarikoVillage/KakarikoVillage_Child_Dynamic.bcett.byml.zs \
        --place CoopPuppet_Child -117.1 96.7 58.7 --place Animal_Cow -121.1 96.7 58.7

For a load test only, in a throwaway mod folder: the scene file it writes is the
game's own with extra actors, and it raises that file's size-table entry in the
mod's size table (the builder's output, which must already be in --mod-dir).
"""
import argparse
import os
import struct
import sys
import zlib

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import build, byml, rstb, zs  # noqa: E402


def placement(actor, pos, used):
    h = zlib.crc32(f"coop-test/{actor}/{pos}".encode()) | (0xC00F << 48)
    while h in used:
        h += 1
    used.add(h)
    srt = zlib.crc32(struct.pack("<3f", *pos))
    return {
        "Gyaml": actor,
        "Hash": byml.UInt64(h),
        "Phive": {"Placement": {"ID": byml.UInt64(h)}},
        "Rotate": [0.0, 0.0, 0.0],
        "SRTHash": byml.UInt(srt),
        "Translate": [float(p) for p in pos],
    }


def add_actors(banc_doc, places):
    actors = banc_doc["Actors"]
    used = {a["Hash"].v for a in actors if isinstance(a.get("Hash"), byml.UInt64)}
    for actor, pos in places:
        actors.append(placement(actor, pos, used))
    return banc_doc


def place(romfs, mod_dir, banc_rel, places, log=print):
    dicts = zs.Dicts.from_romfs(romfs)
    src = os.path.join(romfs, banc_rel)
    raw = build.read(src)
    old = dicts.decompress(raw)
    new = byml.dumps(add_actors(byml.loads(old), places))
    out_romfs = os.path.join(mod_dir, "romfs")
    rs_rel = build.rel(romfs, build.find_one(romfs, "System/Resource/ResourceSizeTable.Product.*.rsizetable.zs"))
    rs_mod = os.path.join(out_romfs, rs_rel)
    if not os.path.isfile(rs_mod):
        raise build.BuildError(f"run the builder into {mod_dir} first (no {rs_rel} there)")
    rs_raw = build.read(rs_mod)
    table = rstb.Rstb.loads(dicts.decompress(rs_raw))
    key = build.strip_zs(banc_rel.replace("\\", "/"))
    size = table.get(key)
    if size is not None:
        table.set(key, size + max(0, len(new) - len(old)) + build.PACK_SIZE_MARGIN)
    build.write(os.path.join(out_romfs, banc_rel), build.recompress(dicts, raw, new))
    build.write(rs_mod, build.recompress(dicts, rs_raw, table.dumps()))
    log(f"wrote {banc_rel} with {len(places)} more actors; size entry {size} -> {table.get(key)}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--mod-dir", required=True)
    ap.add_argument("--banc", required=True, help="scene file, romfs-relative, ending in .bcett.byml.zs")
    ap.add_argument("--place", nargs=4, action="append", required=True,
                    metavar=("ACTOR", "X", "Y", "Z"))
    a = ap.parse_args(argv)
    places = [(p[0], tuple(float(x) for x in p[1:])) for p in a.place]
    try:
        place(a.romfs, a.mod_dir, a.banc, places)
    except build.BuildError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
