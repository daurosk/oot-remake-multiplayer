"""A made-up third-party mod for the size-table proof (player report 101, "bestbuddy").

It copies RSDB tables from the player's own game (never into the repo), makes each larger by padding (zero bytes after
the BYML data: the game reads nothing past it, but its loader needs a buffer that size), and writes the mod's own
resource size table: the game's table with those files' entries raised by the same padding. That is what bestbuddy
looks like to Longshot: RSDB files larger than the game's entries, plus a table that knows it.

With the mod named so it sorts under `contents/` ("bestbuddy") and no Mods list row for it, Longshot serves the
PUPPET PACK's table. Before the fix (merge_tables.py, report 101) that table has the game's smaller entries and the
game aborts about 1 s into the start; with it, the kit's start merges this mod's entries into the pack's table.

    python -I tools/autotest/fake_size_mod.py --romfs <game>/romfs --out <game>/mods/bestbuddy [--pad 1048576]
    python -I tools/autotest/fake_size_mod.py --remove --out <game>/mods/bestbuddy

Prints one line per file. Only for test lanes: a mod made here is the player's game data and stays on this PC.
"""
import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from puppet_pack import rstb, zs  # noqa: E402

FILES = ("RSDB/Challenge.Product.111.rstbl.byml.zs", "RSDB/EventSetting.Product.111.rstbl.byml.zs",
         "RSDB/GameAnalyzedEventInfo.Product.111.rstbl.byml.zs")
RESOURCE = ("System", "Resource")
TAIL = ".rsizetable.zs"
MARK = "fake-size-mod.txt"   # (so --remove never deletes a real mod)


def dict_name(dicts, raw):
    did = zs.frame_dict_id(raw)
    if did == 0:
        return None
    for name, d in dicts.by_name.items():
        if d.dict_id() == did:
            return name
    raise ValueError(f"no dictionary with id {did}")


def make(romfs, out, pad, files=FILES, say=print):
    dicts = zs.Dicts.from_romfs(romfs)
    res = os.path.join(romfs, *RESOURCE)
    tables = sorted(f for f in os.listdir(res) if f.endswith(TAIL))
    if not tables:
        raise ValueError(f"no *{TAIL} in {res}")
    if os.path.isdir(out):
        if not os.path.isfile(os.path.join(out, MARK)):
            raise ValueError(f"{out} exists and was not made by this tool: left as it is")
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "romfs", *RESOURCE))
    with open(os.path.join(out, MARK), "w", encoding="utf-8") as f:
        f.write("made by tools/autotest/fake_size_mod.py (report 101 proof); delete freely\n")
    raised = {}
    for rel in files:
        with open(os.path.join(romfs, *rel.split("/")), "rb") as f:
            raw = f.read()
        data = dicts.decompress(raw) + b"\0" * pad
        dst = os.path.join(out, "romfs", *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(dicts.compress(data, dict_name(dicts, raw)))
        raised[rel[:-3]] = pad   # (the table's path has no .zs)
        say(f"fake mod: {rel}: {len(data)} bytes ({pad} more than the game's)")
    for name in tables:
        with open(os.path.join(res, name), "rb") as f:
            raw = f.read()
        table = rstb.Rstb.loads(zs.maybe_decompress(dicts, raw))
        for path, more in raised.items():
            have = table.get(path)
            if have is None:
                raise ValueError(f"{name} has no entry for {path}")
            table.set(path, have + more)
            say(f"fake mod: {name}: {path} {have} -> {have + more}")
        data = table.dumps()
        if raw[:4] == zs.ZSTD_MAGIC:
            data = dicts.compress(data, dict_name(dicts, raw))
        with open(os.path.join(out, "romfs", *RESOURCE, name), "wb") as f:
            f.write(data)
    return raised


def remove(out, say=print):
    if not os.path.isdir(out):
        return False
    if not os.path.isfile(os.path.join(out, MARK)):
        raise ValueError(f"{out} was not made by this tool: left as it is")
    shutil.rmtree(out)
    say(f"fake mod: {out} removed")
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--romfs")
    ap.add_argument("--out", required=True)
    ap.add_argument("--pad", type=int, default=1 << 20)
    ap.add_argument("--remove", action="store_true")
    a = ap.parse_args(argv)
    try:
        if a.remove:
            remove(a.out)
        else:
            if not a.romfs:
                ap.error("--romfs is needed")
            make(a.romfs, a.out, a.pad)
    except (OSError, ValueError) as e:
        print(f"fake mod: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
