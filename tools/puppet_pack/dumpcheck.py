"""Checks the game's RomFS dump before the puppet pack is built.

    python dumpcheck.py --user-dir <emulator user folder>            finds the dump
    python dumpcheck.py --user-dir <emulator user folder> --romfs <folder>

Prints `romfs=<folder>` and exits 0 when the dump is complete. Otherwise prints one
line per problem, each with what to do, and exits 1 (a dump is there but is not
usable) or 2 (no dump found). Needs nothing but Python: no game files are read past
their first bytes, and nothing is written.

The emulator's Dump RomFS (Fritchii, src/yuzu/main_window.cpp) shows the same
"RomFS Extraction Failed!" box for almost every failure and writes no log line of its
own, so this looks at what it left behind instead:
  - nothing: the dump failed or was cancelled (the emulator deletes the folder then);
  - folders without files: a Skeleton dump instead of Full;
  - files missing, or files of zeros: the emulator was closed or crashed mid-dump
    (it sizes each file first, then fills it);
  - another title's content (the manual): the wrong entry was picked in the list;
  - another data version: the game update is not installed in the kit's emulator,
    so the base game was dumped.
"""
import argparse
import glob
import os
import shutil
import sys

TITLE_ID = "0400486024DEC000"
# The data version of the game build co-op is made for (build id e0f3ccfaf50065ae).
EXPECTED_VERSION = "111"
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"

# What the pack tool reads (puppet_pack/build.py).
REQUIRED = (
    "Pack/ZsDic.pack.zs",
    "Pack/Actor/Player_Child.pack.zs",
    "Pack/Actor/Player_Adult.pack.zs",
    "RSDB/ActorInfo.Product.*.rstbl.byml.zs",
    "RSDB/GameActorInfo.Product.*.rstbl.byml.zs",
    "System/Resource/ResourceSizeTable.Product.*.rsizetable.zs",
)
VERSIONED = REQUIRED[3]

REDUMP = ("Delete the folder {romfs} and dump again: Join.bat, right-click the game, "
          "Dump RomFS, Full. Keep the progress window open until it says it is done.")


class Problem:
    def __init__(self, code, message, fix):
        self.code, self.message, self.fix = code, message, fix

    def __str__(self):
        return f"{self.message}\n  Fix: {self.fix}"

    def __repr__(self):
        return f"Problem({self.code!r})"


def _ini_value(user_dir, key):
    ini = os.path.join(user_dir, "config", "qt-config.ini")
    try:
        with open(ini, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return None
    for line in lines:
        k, sep, v = line.partition("=")
        if sep and k.strip() == key and v.strip():
            return os.path.normpath(v.strip())
    return None


def dump_dirs(user_dir):
    """Where the emulator dumps to: the Dump Root setting first, then <user>/dump."""
    out = []
    for d in (_ini_value(user_dir, "dump_directory"), os.path.join(user_dir, "dump")):
        if d and os.path.normcase(os.path.abspath(d)) not in [os.path.normcase(os.path.abspath(x)) for x in out]:
            out.append(d)
    return out


def find_dumps(user_dir):
    """The game's romfs folders that exist under the dump folders."""
    return [r for r in (os.path.join(d, TITLE_ID, "romfs") for d in dump_dirs(user_dir))
            if os.path.isdir(r)]


def resolve(romfs):
    """Accept the dump folder, the title folder or the romfs folder itself."""
    for sub in ("", "romfs", os.path.join(TITLE_ID, "romfs")):
        p = os.path.join(romfs, sub) if sub else romfs
        if os.path.isdir(os.path.join(p, "Pack")):
            return p
    return romfs


def _files(romfs):
    for base, _dirs, names in os.walk(romfs):
        for n in names:
            yield os.path.join(base, n)


def check_romfs(romfs, any_version=False):
    """Problems with a romfs folder; empty when the pack tool can use it."""
    redump = REDUMP.format(romfs=romfs)
    if not os.path.isdir(romfs):
        return [Problem("missing", f"No folder {romfs}.", redump)]
    first = next(_files(romfs), None)
    if first is None:
        return [Problem("skeleton", f"{romfs} has folders but no files: that is a Skeleton dump.",
                        redump.replace("Full.", "Full (not Skeleton)."))]
    if not os.path.isdir(os.path.join(romfs, "Pack")):
        top = sorted(os.listdir(romfs))[:5]
        return [Problem("not-game", f"{romfs} is not the game's RomFS (it holds {', '.join(top)}): "
                        "the manual or legal text was dumped instead.",
                        redump + f" If it asks which RomFS, pick 'Program [{TITLE_ID}]'.")]
    problems = []
    missing, damaged = [], []
    for pattern in REQUIRED:
        hits = glob.glob(os.path.join(romfs, pattern))
        if not hits:
            missing.append(pattern)
            continue
        for h in hits:
            try:
                with open(h, "rb") as f:
                    head = f.read(4)
            except OSError:
                head = b""
            if head != ZSTD_MAGIC:
                damaged.append(os.path.relpath(h, romfs).replace("\\", "/"))
    if missing:
        problems.append(Problem("partial", f"The dump is not complete: no {', '.join(missing)}. "
                                "It stopped early (emulator closed or crashed, or the disk filled up).",
                                redump))
    if damaged:
        problems.append(Problem("damaged", f"The dump has empty or damaged files: {', '.join(damaged)}. "
                                "It stopped while copying.", redump))
    versions = sorted({os.path.basename(h).split(".")[2]
                       for h in glob.glob(os.path.join(romfs, VERSIONED))})
    if len(versions) > 1:
        problems.append(Problem("mixed", f"The folder holds dumps of two game versions ({', '.join(versions)}).",
                                redump))
    elif versions and versions[0] != EXPECTED_VERSION and not any_version:
        problems.append(Problem("version", f"This dump is game data version {versions[0]}; co-op needs "
                                f"{EXPECTED_VERSION} (the updated game). The kit's emulator dumped the game "
                                "without its update.",
                                "Install your game update in the kit's emulator (File > Install Files to "
                                f"NAND), then delete {romfs} and dump again (Full)."))
    return problems


def check_setup(user_dir):
    """Hints for when there is no dump at all: what usually stops the emulator."""
    hints = []
    keys = os.path.join(user_dir, "keys")
    if not os.path.isfile(os.path.join(keys, "prod.keys")):
        deeper = glob.glob(os.path.join(keys, "*", "prod.keys"))
        if deeper:
            hints.append(Problem("keys-deep", f"prod.keys is one folder too deep: {deeper[0]}.",
                                 f"Move the files in {os.path.dirname(deeper[0])} up into {keys}, "
                                 "then restart the emulator."))
        else:
            hints.append(Problem("keys", f"No {os.path.join(keys, 'prod.keys')}: the emulator cannot read "
                                 "the game, so it cannot dump it.",
                                 "Copy your keys folder into the kit (SETUP.md step 2)."))
    nand_update = os.path.join(user_dir, "nand", "user", "Contents", "registered")
    if not os.path.isdir(nand_update) or not os.listdir(nand_update):
        hints.append(Problem("no-update", "Nothing is installed in the kit's NAND, so the game update is "
                             "not there either.",
                             "Copy your nand folder into the kit, or use File > Install Files to NAND "
                             "with your update (SETUP.md step 2)."))
    dump = dump_dirs(user_dir)[0]
    probe = dump
    while probe and not os.path.isdir(probe):
        parent = os.path.dirname(probe)
        probe = parent if parent != probe else None
    if probe:
        free = shutil.disk_usage(probe).free
        if free < 20 * 1024 ** 3:
            hints.append(Problem("space", f"Only {free / 1024 ** 3:.1f} GB free on the drive of {dump}. "
                                 "The full dump needs the game's size plus 1 GB "
                                 "(the emulator says 'There is not enough free space').",
                                 "Free up space there, then dump again."))
    if len(os.path.abspath(os.path.join(dump, TITLE_ID, "romfs"))) > 120:
        hints.append(Problem("deep", f"The kit is in a deep folder ({dump}); long paths inside the dump "
                             "can fail.", "Move the kit to a short folder such as C:\\oot-coop."))
    return hints


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--user-dir", required=True, help="the emulator's user folder")
    ap.add_argument("--romfs", help="the romfs folder (default: look in the dump folders)")
    ap.add_argument("--any-version", action="store_true", help="accept another game data version")
    a = ap.parse_args(argv)
    if a.romfs:
        romfs = resolve(a.romfs)
    else:
        found = find_dumps(a.user_dir)
        if not found:
            where = " or ".join(os.path.join(d, TITLE_ID, "romfs") for d in dump_dirs(a.user_dir))
            print(f"No RomFS dump found (looked in {where}).")
            print("  Fix: Join.bat, right-click the game, Dump RomFS, Full. If the game is not in the list "
                  "or the dump fails, check these:")
            for p in check_setup(a.user_dir):
                print(f"- {p}")
            return 2
        romfs = found[0]
    problems = check_romfs(romfs, a.any_version)
    for p in problems:
        print(f"- {p}")
    if problems:
        return 1
    print(f"romfs={romfs}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
