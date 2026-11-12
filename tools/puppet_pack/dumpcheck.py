"""Finds and checks the game files the puppet pack is built from.

    python dumpcheck.py --user-dir <emulator user folder>            finds them
    python dumpcheck.py --user-dir <emulator user folder> --romfs <.nup file or folder>

Prints `romfs=<path>` and exits 0 when it found something usable. Otherwise prints one
line per problem, each with what to do, and exits 1 (found but not usable) or 2
(nothing found). Needs nothing but Python, reads no more than the files' headers, and
writes nothing.

First choice is the game's .nup file in a folder of the emulator's game list: its RomFS
is not encrypted, so no keys and no Dump RomFS are needed (puppet_pack/nup.py). Else
the emulator's RomFS dump. Dump RomFS (Fritchii, src/yuzu/main_window.cpp) shows the
same "RomFS Extraction Failed!" box for almost every failure and writes no log line
of its own, so this looks at what it left behind instead:
  - nothing: the dump failed or was cancelled (the emulator deletes the folder then);
    the emulator log may say "Cannot decrypt ...: keys for gen 0xNN+ required."
    (src/core/file_sys/content_archive.cpp): prod.keys older than the game's update;
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
import re
import shutil
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from puppet_pack import nup  # noqa: E402
else:
    from . import nup  # noqa: E402

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
# Read when there, not needed: the shots' sources, for the stand-ins (build.py STAND_INS). A
# .nup build extracts only what nup_files names, so without these the pack had no stand-ins
# ("shots": []) and other players' seeds, arrows, hookshot tips, lures and boomerangs had no copy to show.
OPTIONAL = (
    "Pack/Actor/Shootable_DekuSeed.pack.zs",
    "Pack/Actor/NormalArrow.pack.zs",
    "Pack/Actor/Shootable_Hook.pack.zs",
    "Pack/Actor/Shootable_HookLong.pack.zs",
    "Pack/Actor/Weapon_Lure.pack.zs",
    "Pack/Actor/Weapon_SinkingLure.pack.zs",
    "Pack/Actor/Item_Bomb.pack.zs",
    "Pack/Actor/Weapon_Boomerang.pack.zs",
    # Issue 199: the horse another player rides (build.py STAND_INS, CoopRide_Epona).
    "Pack/Actor/Animal_Epona_Adult.pack.zs",
    # Issue 166: the AI the impact actors are made from (build.py IMPACT_SOURCE).
    "Pack/Actor/Shootable_ScrubBusinessSees.pack.zs",
    # The tunic colours (puppet_pack/tunic.py): the green tile and the two player models.
    "Tex/Angin_Tile_A_Alb.bntx.zs",
    "Tex/Angin_Tile_A_Dark_Alb.bntx.zs",
    "Tex/Mt_Vest_Player_Adult_Alb.bntx.zs",
    "Tex/Mt_Angin_Acs_Player_Adult_Alb.bntx.zs",
    "Model/Player_Child.Player_Child.bfres.zs",
    "Model/Player_Adult.Player_Adult.bfres.zs",
)

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


def _version_of(rel):
    parts = os.path.basename(rel).split(".")
    return parts[2] if len(parts) > 2 else None


def nup_files(n):
    """{pattern: RomFS path} of the files the pack tool reads, from an open Nup.
    Where a pattern matches more than one version, the expected one wins. The OPTIONAL files
    are in it when the .nup has them."""
    out = {}
    for pattern in REQUIRED + OPTIONAL:
        hits = n.glob(pattern)
        if len(hits) > 1:
            hits = [h for h in hits if _version_of(h) == EXPECTED_VERSION] or hits[-1:]
        if hits:
            out[pattern] = hits[0]
    return out


def check_nup(path, any_version=False):
    """Problems with a .nup game file; empty when the pack tool can use it."""
    try:
        with nup.Nup(path) as n:
            tids = {p["title_id"] & ~0xFFF for p in n.packages}
            if int(TITLE_ID, 16) not in tids:
                return [Problem("nup-other", f"{path} is another game (title "
                                f"{', '.join(f'{t:016X}' for t in sorted(tids))}), not {TITLE_ID}.",
                                "Give the .nup of the Ocarina of Time remake.")]
            found = nup_files(n)
    except nup.NupError as e:
        return [Problem("nup-bad", str(e), "Copy the .nup again from where you made it, then run this again.")]
    except OSError as e:
        return [Problem("nup-bad", f"Cannot read {path}: {e}.", "Check the file is there and not open elsewhere.")]
    missing = [p for p in REQUIRED if p not in found]
    if missing:
        return [Problem("nup-partial", f"{path} has no {', '.join(missing)}.",
                        "This .nup does not hold the whole game. Use one made from the game with its update.")]
    v = _version_of(found[VERSIONED])
    if v != EXPECTED_VERSION and not any_version:
        return [Problem("version", f"{path} holds game data version {v}; co-op needs {EXPECTED_VERSION} "
                        "(the updated game).", "Use a .nup made from the game with its update.")]
    return []


def game_dirs(user_dir):
    """The folders of the emulator's game list (Paths\\gamedirs\\N\\path in qt-config.ini)."""
    ini = os.path.join(user_dir, "config", "qt-config.ini")
    try:
        with open(ini, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return []
    out = []
    for line in lines:
        m = re.match(r"^Paths\\gamedirs\\\d+\\path=(.+)$", line.strip())
        if m and m.group(1) not in ("SDMC", "UserNAND", "SysNAND"):
            out.append(os.path.normpath(m.group(1)))
    return out


def find_nups(user_dir, extra_dirs=()):
    """.nup files in the game list's folders (and one level below)."""
    found = []
    for d in list(game_dirs(user_dir)) + list(extra_dirs):
        for pattern in ("*.nup", os.path.join("*", "*.nup")):
            for p in sorted(glob.glob(os.path.join(glob.escape(d), pattern))):
                if p not in found and nup.is_nup(p):
                    found.append(p)
    return found


def check_keys(user_dir):
    """prod.keys older than the game: the log's "keys for gen 0xNN+ required" against the
    newest master_key_XX in prod.keys. Gen N needs master_key_(N-1) (content_archive.cpp)."""
    need = None
    log = os.path.join(user_dir, "log", "eden_log.txt")
    try:
        with open(log, encoding="utf-8", errors="replace") as f:
            for m in re.finditer(r"keys for gen 0x([0-9A-Fa-f]+)\+ required", f.read()):
                need = max(need or 0, int(m.group(1), 16))
    except OSError:
        return []
    if need is None:
        return []
    have = None
    try:
        with open(os.path.join(user_dir, "keys", "prod.keys"), encoding="utf-8", errors="replace") as f:
            for m in re.finditer(r"^\s*master_key_([0-9A-Fa-f]{2})\s*=", f.read(), re.M):
                have = max(have if have is not None else -1, int(m.group(1), 16))
    except OSError:
        pass
    want = max(need, 1) - 1
    if have is not None and have >= want:
        return []
    has = f"your prod.keys stop at master_key_{have:02x}" if have is not None else "no master keys were read"
    return [Problem("keys-old", f"Your prod.keys are too old for this game's update: the emulator log says "
                    f"\"keys for gen 0x{need:02X}+ required\" (master_key_{want:02x} or newer); {has}. "
                    "Dump RomFS cannot decrypt the update.",
                    "Use your .nup instead: run make-puppet-pack.cmd and give it the .nup file (it needs "
                    "no keys). Or dump newer keys from your console.")]


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
    ap.add_argument("--romfs", help="the game's .nup file or a RomFS dump folder "
                    "(default: a .nup in the game list's folders, then the dump folders)")
    ap.add_argument("--any-version", action="store_true", help="accept another game data version")
    a = ap.parse_args(argv)

    def report(problems):
        for p in problems:
            print(f"- {p}")

    if a.romfs:
        path = a.romfs
        if os.path.isfile(path):
            problems = (check_nup(path, a.any_version) if nup.is_nup(path) else
                        [Problem("not-nup", f"{path} is not a .nup file.",
                                 "Give the game's .nup file, or the romfs folder of a RomFS dump.")])
        else:
            path = resolve(path)
            problems = check_romfs(path, a.any_version)
            if problems:
                problems += check_keys(a.user_dir)
        report(problems)
        if problems:
            return 1
        print(f"romfs={path}")
        return 0

    nup_problems = []
    for p in find_nups(a.user_dir):
        found = check_nup(p, a.any_version)
        if not found:
            print(f"romfs={p}")
            return 0
        nup_problems += found
    dumps = find_dumps(a.user_dir)
    if dumps:
        problems = check_romfs(dumps[0], a.any_version)
        if problems:
            report(nup_problems + problems + check_keys(a.user_dir))
            return 1
        print(f"romfs={dumps[0]}")
        return 0
    where = " or ".join(os.path.join(d, TITLE_ID, "romfs") for d in dump_dirs(a.user_dir))
    print("Found no .nup game file in the emulator's game list folders, and no RomFS dump "
          f"(looked in {where}).")
    print("  Fix: give the path of your game's .nup file (it needs no keys and no dump). With an "
          ".nsp or .xci game: Join.bat, right-click the game, Dump RomFS, Full. If that fails, check these:")
    report(nup_problems + check_keys(a.user_dir) + check_setup(a.user_dir))
    return 2


if __name__ == "__main__":
    sys.exit(main())
