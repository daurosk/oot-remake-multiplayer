"""The puppet pack tool as staged in a kit (tools/release/kit_pack.ps1), run from that copy alone: every module build.py
imports loads from the kit, every path a loaded module names inside the kit (DATA, MANIFEST, CODE_PATCH, STRINGS, ...)
is there, and the data folders hold what their modules read (charsel_ui's hash and manifest deltas, coop_menu's
deltas, ui_pack's words). Run by kit_pack_test.ps1 with python -I (no repo, no script folder on sys.path):

    python -I kit_pack_check.py <kit folder>

Prints one line per problem; exits 1 when there is any."""
import json
import os
import sys


def _varint(d, i):
    n = shift = 0
    while True:
        if i >= len(d):
            raise ValueError("ends inside a number")
        b = d[i]
        i += 1
        n |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return n, i


def delta_shape(path):
    """'' when the file is a whole CDLT1 delta (charsel_ui's format: magic, the source's and the result's length and
    SHA-1, ops 0x01 copy / 0x02 insert until 0x00, nothing after) whose ops make exactly the result's length, else why
    not. Without the game's file only the shape can be checked: a copy cut short, bytes added, a wrong length."""
    with open(path, "rb") as f:
        d = f.read()
    magic = b"CDLT1\0"
    if d[:len(magic)] != magic or len(d) < len(magic) + 48:
        return "no CDLT1 header"
    i = len(magic)
    dlen = int.from_bytes(d[i + 24:i + 28], "little")
    i += 48
    made = 0
    try:
        while True:
            if i >= len(d):
                return "no end op"
            op = d[i]
            i += 1
            if op == 0:
                break
            if op == 1:
                _, i = _varint(d, i)
                n, i = _varint(d, i)
            elif op == 2:
                n, i = _varint(d, i)
                if i + n > len(d):
                    return "an insert runs past the file's end"
                i += n
            else:
                return f"unknown op {op}"
            made += n
    except ValueError as e:
        return str(e)
    if i != len(d):
        return f"{len(d) - i} bytes after the end op"
    if made != dlen:
        return f"its ops make {made} bytes, the result is {dlen}"
    return ""


def main(kit):
    tools = os.path.abspath(os.path.join(kit, "tools"))
    stage = os.path.join(tools, "puppet_pack")
    sys.path.insert(0, tools)
    problems = []
    try:
        import puppet_pack.build  # noqa: F401  (imports every module the pack is made with)
    except Exception as e:  # noqa: BLE001  (any failure to import is the finding)
        print(f"FAIL import puppet_pack.build from the kit: {type(e).__name__}: {e}")
        return 1
    inside = os.path.normcase(stage) + os.sep
    mods = {n: m for n, m in sys.modules.items() if n == "puppet_pack" or n.startswith("puppet_pack.")}
    for name, mod in sorted(mods.items()):
        f = os.path.normcase(os.path.abspath(getattr(mod, "__file__", "") or ""))
        if not f.startswith(inside):
            problems.append(f"{name} loaded from outside the kit: {f}")
            continue
        # (every path a module names inside the kit's tool folder: its data folders, files and strings)
        for key, value in sorted(vars(mod).items()):
            if not isinstance(value, str) or not os.path.isabs(value):
                continue
            p = os.path.normcase(os.path.abspath(value))
            if p.startswith(inside) and not os.path.exists(p):
                problems.append(f"{name}.{key} names {os.path.relpath(p, stage)}, not in the kit")
    from puppet_pack import charsel_ui, coop_menu, ui_pack
    try:
        # (charsel_ui's own hash of its data folder, where the module has one: the charsel-native branch adds it)
        if hasattr(charsel_ui, "data_hash") and charsel_ui.data_hash() != charsel_ui.DATA_SHA256:
            problems.append("charsel/data differs from charsel_ui.DATA_SHA256 (a file missing, extra or changed)")
        with open(charsel_ui.MANIFEST, encoding="utf-8") as f:
            manifest = json.load(f)
        for m, data_dir in ((manifest, charsel_ui.DATA), (coop_menu.MANIFEST, coop_menu.DATA)):
            for entry in m["files"]:
                # (a plain file's entry names its delta; a SARC archive's entry ("rel" or "glob") names one per
                # member: charsel_ui.make's two kinds)
                where = entry.get("rel") or entry.get("glob")
                deltas = [entry["delta"]] if "members" not in entry else [mem["delta"] for mem in entry["members"]]
                for delta in deltas:
                    path = os.path.join(data_dir, delta)
                    if not os.path.isfile(path):
                        problems.append(f"{os.path.relpath(data_dir, stage)}/{delta} ({where}) not in the kit")
                        continue
                    why = delta_shape(path)
                    if why:
                        problems.append(f"{os.path.relpath(data_dir, stage)}/{delta} ({where}) is not the delta "
                                        f"it should be: {why}")
        for lang in ("USen", "CNzh", "TWzh"):   # (en.txt and each translation the repo has)
            if not ui_pack.load_strings(lang):
                problems.append(f"ui_pack.load_strings('{lang}') read no words from the kit's strings")
    except Exception as e:  # noqa: BLE001
        problems.append(f"reading the kit's pack data: {type(e).__name__}: {e}")
    for p in problems:
        print("FAIL " + p)
    if not problems:
        print(f"ok   {len(mods)} pack modules load from the kit; every path they name there is there")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
