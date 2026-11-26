"""Your own Link's tunic files at each start (colour-agree): the Python twin of the Windows kit's
Set-KitSelfTunic (tools/release/kit/scripts/kit-paths.ps1), for the Linux kit (start.sh) and the
autotest's Hookshot start (tools/autotest/run.ps1), so the colour your own screen draws is the
colour the others are sent, on every start.

The colour is decided as the co-op module decides it: LICHII_COOP_COLOR (a switch), else the kit's
coop-options.ini `color`, else coop.ini's `color`; none or green: green's own files (npc-colour: your own Link
never wears the game's tunic textures, which Mido's and Saria's models share; the module then
wears your slot colour live on your own copies only). Both users start Longshot, whose co-op module paints
your exact colour on green's files while the game runs (hookshot/src/coop_look_live.cpp, from the pack's
coop_look/green.bin): a pack with green's own files and coop_look/green.bin gets green's whatever the colour
(`painted`), so the game's memory always holds the texels the module paints from. The pack's coop_puppet.json
`self_files` ({colour: {romfs path: file beside romfs}}) names the files of each colour; every one
any colour may put in is taken away first, so no file of an earlier start stays.

    python -I self_tunic.py --pack <coop-puppet folder> [--color C] [--options F] [--coop-ini F]

Prints the colour put in, or nothing. Never fails the start: a broken pack puts nothing in.
"""
import argparse
import json
import os
import re
import shutil
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from puppet_pack import tunic  # noqa: E402
else:
    from . import tunic

# The colours the module knows by name (lichii/hook/coop_colors.cpp kNamed; keep them the same).
NAMED = {
    "red": 0xFF3030, "orange": 0xFF8020, "yellow": 0xFFC020, "green": 0x30D040, "cyan": 0x20D0E0,
    "blue": 0x3070FF, "purple": 0xA040FF, "pink": 0xFF60C0, "white": 0xFFFFFF, "black": 0x202020,
    "brown": 0x8B5A2B, "gray": 0xA0A0A0, "grey": 0xA0A0A0,
}
# The tunic presets: the pack's colours (tunic.COLORS), each at its name's colour above.
TUNICS = [c for c in tunic.COLORS]

# Every romfs path a colour may put in, also those of packs before colour-agree: taken away first.
FIXED = [
    "Tex/Angin_Tile_A_Alb.bntx.zs", "Tex/Angin_Tile_A_Dark_Alb.bntx.zs",
    "Tex/Mt_Vest_Player_Adult_Alb.bntx.zs", "Tex/Mt_Angin_Acs_Player_Adult_Alb.bntx.zs",
    "Tex/Mt_Cloth_Parts_Player_Child_Alb.bntx.zs", "Tex/Mt_Vest_Player_Child_Alb.bntx.zs",
    "Model/Player_Adult.Player_Adult.bfres.zs", "Model/Player_Child.Player_Child.bfres.zs",
]
_OK = re.compile(r"^(Tex/[A-Za-z0-9_]+\.bntx\.zs|Model/Player_(Adult|Child)\.Player_(Adult|Child)\.bfres\.zs)$")
_MAIN = re.compile(r"^Tex/Angin_Tile_[A-Z]_Alb\.bntx\.zs$")


def parse(text):
    """A colour as the module reads it (CoopColors::Parse): 0xRRGGBB, or None."""
    t = (text or "").strip()
    if len(t) >= 2 and t[0] in "\"'" and t[-1] == t[0]:
        t = t[1:-1].strip()
    low = t.lower()
    if low in NAMED:
        return NAMED[low]
    m = re.fullmatch(r"rgba?\(\s*(\d{1,3})\s*,\s*(\d{1,3})\s*,\s*(\d{1,3})\s*(?:,[^)]*)?\)", low)
    if m:
        p = [int(x) for x in m.groups()]
        return None if any(x > 255 for x in p) else (p[0] << 16) | (p[1] << 8) | p[2]
    m = re.fullmatch(r"(?:#|0x)?([0-9a-f]{6})(?:[0-9a-f]{2})?", low)
    if m:
        return int(m.group(1), 16)
    m = re.fullmatch(r"#([0-9a-f])([0-9a-f])([0-9a-f])", low)
    if m:
        return int("".join(d * 2 for d in m.groups()), 16)
    return None


def _ycc(v):
    r, g, b = (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF
    y = 0.299 * r + 0.587 * g + 0.114 * b
    return y, 0.564 * (b - y), 0.713 * (r - y)


def tunic_of(text):
    """The tunic preset nearest a colour (CoopColors::TunicOf: luma and chroma apart, chroma
    counted double), lower case; '' when it is not a colour."""
    rgb = parse(text)
    if rgb is None:
        return ""
    want = _ycc(rgb)
    best, best_d = "", -1.0
    for name in TUNICS:
        have = _ycc(NAMED[name])
        d = (want[0] - have[0]) ** 2 + 2.0 * ((want[1] - have[1]) ** 2 + (want[2] - have[2]) ** 2)
        if best_d < 0 or d < best_d:
            best, best_d = name, d
    return best


def ini_color(path):
    """A settings file's `color` (the last line without a '#' in front), or ''."""
    try:
        with open(path, encoding="utf-8-sig", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return ""
    value = ""
    for line in lines:
        m = re.match(r"^\s*color\s*=\s*(.*?)\s*$", line)
        if m:
            value = m.group(1)
    return value


def start_color(switch, options_ini=None, coop_ini=None, tunics=""):
    """The colour the module will wear and send at this start, as text; '' for none."""
    if tunics.strip().lower() in ("0", "off"):
        return ""
    if switch:
        return switch
    if options_ini and ini_color(options_ini):
        return ini_color(options_ini)
    return ini_color(coop_ini) if coop_ini else ""


def painted_here(pack, manifest):
    """Whether the pack lets the module paint any colour on green's files: green's own files and
    coop_look/green.bin (look_files)."""
    files = manifest.get("self_files") if isinstance(manifest.get("self_files"), dict) else {}
    looks = manifest.get("look_files") if isinstance(manifest.get("look_files"), dict) else {}
    green = looks.get("green")
    return (isinstance(files.get("green"), dict) and isinstance(green, str) and bool(green)
            and os.path.isfile(os.path.join(pack, green)))


def put(pack, color, painted=True):
    """Puts the colour's own files in the pack's romfs (Set-KitSelfTunic); no colour: green's.
    `painted` (Set-KitSelfTunic -Painted, Longshot): green's whatever the colour when the pack has
    what the module paints from (painted_here); else the nearest preset's. Returns the tunic put
    in, or '' (no such colour in the pack, an older or broken pack)."""
    romfs = os.path.join(pack, "romfs")
    want = tunic_of(color) or "green"
    try:
        with open(os.path.join(pack, "coop_puppet.json"), encoding="utf-8") as f:
            manifest = json.load(f)
    except (OSError, ValueError):
        manifest = {}
    if not isinstance(manifest, dict):
        manifest = {}
    if painted and painted_here(pack, manifest):
        want = "green"
    targets = list(FIXED)
    files = manifest.get("self_files") if isinstance(manifest.get("self_files"), dict) else {}
    for per in files.values():
        if isinstance(per, dict):
            targets += [p for p in per if isinstance(p, str) and _OK.match(p)]
    chosen = {}
    tunics = manifest.get("tunics") if isinstance(manifest.get("tunics"), dict) else {}
    if tunics.get("green") and isinstance(files.get(want), dict):
        for rel, src in files[want].items():
            if isinstance(rel, str) and _OK.match(rel) and isinstance(src, str) and src:
                chosen[rel] = os.path.join(pack, src)
    # A main tile must be there (the own S tile, or a pack's older game-name tile).
    if not any(_MAIN.match(r) and os.path.isfile(p) for r, p in chosen.items()):
        chosen = {}
    try:
        for rel in dict.fromkeys(targets):
            dst = os.path.join(romfs, rel)
            if rel not in chosen and os.path.isfile(dst):
                os.remove(dst)
        for rel, src in chosen.items():
            dst = os.path.join(romfs, rel)
            if os.path.isfile(src):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(src, dst)
            elif os.path.isfile(dst):
                os.remove(dst)
    except OSError:
        return ""
    return want if chosen else ""


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pack", required=True, help="the coop-puppet folder (coop_puppet.json, romfs/)")
    ap.add_argument("--color", default=os.environ.get("LICHII_COOP_COLOR", ""), help="LICHII_COOP_COLOR")
    ap.add_argument("--options", help="the kit's coop-options.ini")
    ap.add_argument("--coop-ini", help="the game's coop.ini")
    ap.add_argument("--tunics", default=os.environ.get("LICHII_COOP_TUNICS", ""), help="LICHII_COOP_TUNICS")
    a = ap.parse_args(argv)
    got = put(a.pack, start_color(a.color, a.options, a.coop_ini, a.tunics))
    if got:
        print(got)
    return 0


if __name__ == "__main__":
    sys.exit(main())
