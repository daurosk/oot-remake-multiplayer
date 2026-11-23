"""Developer tool (not run by the kit): build co-op's own Characters tab, Link only, with Snail's MIT code.

The owner (2026-11-22): "our mod itself should natively include the character tab even without downloading any of
Snails mod." Snail (snaildev99) gave permission; his OoT Character Select (github.com/snaildev99/OoT-Character-Select,
v0.2.1, MIT, Copyright (c) 2026 snaildev99) builds the - menu's four-tab header, the Characters page, the picker and
the Customize screen from the game's own files. This runs HIS code, as a library, for Link alone:

  stage 1 (his mods/build.py): "Characters page", "Header four tabs", "Character models" with CHARACTERS = [Link]
      (the flags, the page view and keep-camera hooks; no model files), then picker_page() with PICKER_ROSTER = [0]
      (one cell; Link's picture: a placeholder here, the pack points the cell at a texture of the game's own);
  stage 2 (his mods/customize.py): the Customize screen's layout, messages, sounds, game data and graph over stage 1,
      and its camera glide (cv_hook) without his hair / eye / gear colour code (co-op's look is the customizer).

Three of his self-checks assume two or more characters (a picker with more than one sheet, a cursor that moves);
they are relaxed in a temporary copy of his checkout (TEXT_EDITS, each must match once). His checkout is not changed.

Needs a Python with his requirements.txt (keystone-engine, capstone, ainb, numpy, pyzstd, pillow, astc-encoder-py,
texture2ddecoder, unicorn, mmh3), a vanilla main image (image.bin of a game built without code mods) and the game's
romfs:

  python make_data.py --snail <his checkout at v0.2.1> --image <image.bin> --romfs <romfs> --work <empty folder>

Writes <work>/cs (the copy), <work>/cs/build and <work>/cs/build-custom (his folders), and <work>/charsel-native.ips
(every code patch of the two stages in one IPS32 file; the build fails when two write the same byte).

  stage 3 (--data <repo>/tools/puppet_pack/charsel/data; --reuse keeps <work>'s stage 1 and 2 builds): the
      repository's data: manifest.json, one CDLT1 delta per file (charsel_ui.make_delta) against the --romfs files,
      and charsel-native.ips. Left out: the size table (the pack grows its own) and Common's .bntx (244 MB, only for
      Link's picture: his cell's texture name points at one of the game's own, MapIconPlayer_00, instead). His English
      texts are in the new message files' deltas; strings/*.txt put ours over them per language at pack time.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

TEXT_EDITS = [
    ("mods/build.py",
     "    assert pk.turn_to(0, 1) == ui.PER_SHEET and",
     "    assert pk.n <= ui.PER_SHEET or pk.turn_to(0, 1) == ui.PER_SHEET and"),
    ("mods/build.py",
     "    assert pk.turn_to(last, 1) == last % ui.PER_SHEET and",
     "    assert pk.n <= ui.PER_SHEET or pk.turn_to(last, 1) == last % ui.PER_SHEET and"),
    ("mods/customize.py",
     "assert len(loops) == len(set(loops)) >= 2, len(loops)",
     "assert len(loops) == len(set(loops)) >= 1, len(loops)"),
    ("mods/customize.py",
     "assert moved >= len(loops)",
     "assert len(loops) == 1 or moved >= len(loops)"),
]

STAGE1 = r'''
import os, sys
sys.path.insert(0, os.path.join(sys.argv[1], "mods"))
import build as B
import numpy as np
from PIL import Image
B.CHARACTERS = B.CHARACTERS[:1]
B.CHARACTER_BUTTONS = {}
B.PICKER_ROSTER = [0]
B.SWAP_SLIDES = True
os.makedirs(B.PORTRAIT_DIR, exist_ok=True)
ph = B.portrait_placeholder()
ph[:, :, 3] = np.minimum(255, ph[:, :, 3].astype(int) * 255 // 110).astype(np.uint8)
Image.fromarray(ph, "RGBA").save(os.path.join(B.PORTRAIT_DIR, "00_link.png"))
for mod, fn in B.MODS.items():
    print(mod)
    fn(mod)
notes, gone = B.picker_page()
print("\n".join(notes))
for mod in B.RSTB_MODS:
    B.write_rstb(mod, B.SIZES)
'''

STAGE2 = r'''
import os, sys, shutil
sys.path.insert(0, os.path.join(sys.argv[1], "mods"))
sys.path.insert(0, os.path.join(sys.argv[1], "tools"))
import build as B
import customize as C
import layout, nxfmt
ui = C.ui
B.BUILD, B.SIZES, B.EXE_PATCHES = C.BUILD_CUSTOM, {}, {}
shutil.rmtree(os.path.join(C.BUILD_CUSTOM, C.MOD), ignore_errors=True)
os.makedirs(C.BUILD_CUSTOM, exist_ok=True)
print("over", C.installed())
C.write_slink()
archive = nxfmt.unzs(C.read_installed(ui.COMMON), ui.COMMON)
page_layout = nxfmt.sarc_files(archive)[ui.LAYOUT + ".buil"]
known = {n[:-5] for n, _, _ in nxfmt.sarc_entries(archive)}
import uiedit
# Co-op's line on the page (ours, C #67: credit to Snail and where his playable characters are): one text pane in his
# picker's own container (it fades and hides with the page), a copy of its title, under the grid's first row (Link's
# cell; the rows under it stay empty without his characters), centred on the grid, two lines.
lay = layout.Layout(page_layout)
ed = uiedit.Editor(lay)
ed.validate()
assert "N_Sandbox_00" in ed.names() and "T_Note_00" not in ed.names()
note = ed.clone("T_Title_00", "T_Note_00")
note.node.set("<2f", 0x1C, (ui.GRID_LEFT + ui.GRID_RIGHT) / 2, ui.PICK_TOP - ui.PICK_PITCH_Y - 110.0)
note.node.set("<2f", 0x2C, ui.GRID_RIGHT - ui.GRID_LEFT, 130.0)
uiedit.set_text(note, ref="*", font_size=40.0, h_align=1)
ed.insert("N_Sandbox_00", [note])
page_layout = layout.build(lay)
uiedit.Editor(layout.Layout(page_layout)).validate()
page_new, page_msgs = C.add_page_anims(page_layout)
page_msgs["T_Note_00"] = ("This Characters tab is from OoT Character Select by Snail.\n"
                          "Get his mod for the other playable characters.")
ours, msgs = C.make_layout()
C.check_layout(ours, msgs, known)
C.write_files(page_new, ours, msgs, page_msgs)
C.write_gamedata()
owner, path = C.resolve(C.GRAPH)
g = B.Graph(path)
assert g.build() == g.src
info = C.customize_graph(g)
cmd = g.d["Commands"][0]["Root Node Index"]
layouts = {C.SCREEN: C.anim_frames(layout.Layout(ours)), B.PAGE_SCREEN: C.anim_frames(layout.Layout(page_new))}
print("graph runs", C.check_graph(g.nodes, cmd, info, layouts))
data = g.build()
B.write(C.MOD, C.GRAPH, data, len(data), B.SIZES)
C.colour_patch = lambda p, facts: (p.cave_ptr, 0)     # the camera glide only: no hair / eye / gear code
p, _end, _t = C.custom_patch([])
B.write_ips(C.MOD, p)
print("glide patch runs emulated:", C.check_patch(p))
C.write_table()
'''

BUILD_ID = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348"
STAGE1_MODS = ("Header four tabs", "Characters page", "Character models")
STAGE2_MOD = "Character (customize test)"


def ips32_records(data):
    """[(offset, bytes)] of an IPS32 file (RLE records expanded)."""
    assert data[:5] == b"IPS32", "not an IPS32 file"
    i, out = 5, []
    while data[i:i + 4] != b"EEOF":
        off, n = struct.unpack_from(">IH", data, i)
        i += 6
        if n:
            out.append((off, data[i:i + n]))
            i += n
        else:
            run, val = struct.unpack_from(">HB", data, i)
            i += 3
            out.append((off, bytes([val]) * run))
    return out


def ips32(records):
    out = bytearray(b"IPS32")
    for off, blob in sorted(records):
        for k in range(0, len(blob), 0xFFFF):
            part = blob[k:k + 0xFFFF]
            out += struct.pack(">IH", off + k, len(part)) + part
    return bytes(out + b"EEOF")


def merge_ips(paths):
    """One IPS32 of several; AssertionError when two write the same byte."""
    taken, records = [], []
    for path in paths:
        for off, blob in ips32_records(open(path, "rb").read()):
            for a, b, who in taken:
                assert off + len(blob) <= a or b <= off, "%s and %s write %x" % (path, who, off)
            taken.append((off, off + len(blob), path))
            records.append((off, blob))
    return ips32(records)


# Stage 3: what the pack makes, and where each file of his builds is (the first folder that has it).
PLAIN = {   # (plain files) rel -> the game's file the delta is made from
    "Sequence/ImportantMenu.module.ainb": "Sequence/ImportantMenu.module.ainb",
    "Sequence/MinusMenu.module.ainb": "Sequence/MinusMenu.module.ainb",
    "Sequence/MinusMenuToMain.module.ainb": "Sequence/MinusMenuToMain.module.ainb",
    "Sequence/CharacterMenu.module.ainb": "Sequence/ImportantMenu.module.ainb",
}
ZS_SELF = ("RSDB/UIScreen.Product.111.rstbl.byml.zs", "UI/LayoutArchiveCategory.Product.100.byml.zs",
           "GameData/GameDataList.Product.110.byml.zs", "SLink2/slink2.Product.111.bslnk.zs")
COMMON = "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs"
COMMON_MEMBERS = {   # member -> the game's member its delta is made from
    "MenuOverlay_00.buil": "MenuOverlay_00.buil", "PaMenuCategory_00.buil": "PaMenuCategory_00.buil",
    "CharacterMenu_00.buil": "ImportantMenu_01.buil", "CharacterCustomize_00.buil": "ImportantMenu_01.buil",
    "PaCharacterSlot_00.buil": "PaItemMenuBtn_00.buil", "PaCharacterKey_00.buil": "PaGuideKey_00.buil",
}
MALS_GLOB = "Mals/*.Product.110.sarc.zs"
MALS_ANY = "Mals/USen.Product.110.sarc.zs"
MALS_NEW = ("LayoutMsg/CharacterMenu_00.msbt", "LayoutMsg/CharacterCustomize_00.msbt")
CUSTOM_MSG = "LayoutMsg/CharacterCustomize_00.msbt"
PORTRAIT = (b"\0CharacterPortrait_000\0", b"\0MapIconPlayer_00\0")   # (his cell's texture -> the game's own)
SLOTS, PLACEHOLDER = 13, "." * 64     # (merge_tables.SNAIL_SLOTS / SNAIL_PLACEHOLDER: co-op's words, module-written)


def stage3(cs, romfs, data_dir):
    sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
    from puppet_pack import charsel_ui, sarc, zs
    from puppet_pack.merge_tables import SNAIL_SLOTS, SNAIL_PLACEHOLDER
    assert (SNAIL_SLOTS, SNAIL_PLACEHOLDER) == (SLOTS, PLACEHOLDER)
    dicts = zs.Dicts.from_romfs(romfs)
    folders = [os.path.join(cs, "build-custom", STAGE2_MOD, "romfs")] + \
        [os.path.join(cs, "build", m, "romfs") for m in ("Characters page", "Header four tabs", "Character models")]

    def built(rel):
        for f in folders:
            p = os.path.join(f, *rel.split("/"))
            if os.path.exists(p):
                return open(p, "rb").read()
        raise SystemExit("no build has " + rel)

    def game(rel):
        return open(os.path.join(romfs, *rel.split("/")), "rb").read()

    os.makedirs(data_dir, exist_ok=True)
    for old in os.listdir(data_dir):
        if old.endswith(".cdelta") or old == "manifest.json":
            os.remove(os.path.join(data_dir, old))
    sizes = {}

    def put(name, src, dst):
        delta = charsel_ui.make_delta(src, dst)
        assert charsel_ui.apply_delta(src, delta) == dst
        open(os.path.join(data_dir, name), "wb").write(delta)
        sizes[name] = len(delta)
        return name

    files = []
    for i, (rel, src) in enumerate(sorted(PLAIN.items())):
        files.append({"rel": rel, "zs": False, "from": src,
                      "delta": put("p%02d.cdelta" % i, game(src), built(rel))})
    for i, rel in enumerate(ZS_SELF):
        files.append({"rel": rel, "zs": True, "from": rel,
                      "delta": put("z%02d.cdelta" % i, dicts.decompress(game(rel)), dicts.decompress(built(rel)))})
    van = sarc.read_entries(dicts.decompress(game(COMMON)))
    his = sarc.read_entries(dicts.decompress(built(COMMON)))
    vd, hd = dict(van), dict(his)
    changed = {n for n, d in his if n not in vd or vd[n] != d}
    assert changed == set(COMMON_MEMBERS), sorted(changed ^ set(COMMON_MEMBERS))
    members = []
    for i, (name, src) in enumerate(sorted(COMMON_MEMBERS.items())):
        dst = hd[name]
        if name == "CharacterMenu_00.buil":
            old, new = PORTRAIT
            assert dst.count(old) == 1 and len(new) <= len(old)
            dst = dst.replace(old, new + b"\0" * (len(old) - len(new)))
            sys.path.insert(0, os.path.join(cs, "tools"))
            import layout   # (his parser: the layout with the game's texture name still reads)
            layout.Layout(dst)
        copies = sum(1 for n, _ in his if n == name)
        m = {"name": name, "from": src, "delta": put("c%02d.cdelta" % i, vd[src], dst)}
        if name not in vd:
            m["copies"] = copies
        else:
            assert copies == sum(1 for n, _ in van if n == name)
        members.append(m)
    files.append({"rel": COMMON, "zs": True, "align": 16, "members": members})
    hm = dict(sarc.read_entries(dicts.decompress(built(MALS_ANY))))
    msgs = [{"name": n, "from": None, "delta": put("m%02d.cdelta" % i, b"", hm[n])} for i, n in enumerate(MALS_NEW)]
    files.append({"glob": MALS_GLOB, "zs": True, "align": 16, "members": msgs, "strings": list(MALS_NEW),
                  "labels": {CUSTOM_MSG: {"Coop_Custom_%02d" % i: PLACEHOLDER for i in range(SLOTS)}}})
    manifest = {"format": "coop-charsel 1",
                "about": "Link's Characters page, picker and Customize: Snail's OoT Character Select v0.2.1 (MIT), "
                         "made by charsel/make_data.py; deltas against the game's own files",
                "files": files}
    with open(os.path.join(data_dir, "manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    print("data:", data_dir, sum(sizes.values()), "bytes of deltas in", len(sizes), "files")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--snail", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--data", help="stage 3: write the repository's data here")
    ap.add_argument("--reuse", action="store_true", help="keep <work>'s stage 1 and 2 builds")
    a = ap.parse_args(argv)
    cs = os.path.join(a.work, "cs")
    if not a.reuse:
        build_stages(a, cs)
    if a.data:
        stage3(cs, a.romfs, a.data)
        shutil.copyfile(os.path.join(a.work, "charsel-native.ips"), os.path.join(a.data, "charsel-native.ips"))


def build_stages(a, cs):
    shutil.rmtree(cs, ignore_errors=True)
    shutil.copytree(a.snail, cs, ignore=shutil.ignore_patterns(".git", "build", "build-custom", "build-npc"))
    for rel, old, new in TEXT_EDITS:
        path = os.path.join(cs, *rel.split("/"))
        text = open(path, encoding="utf-8").read()
        assert text.count(old) == 1, (rel, old)
        open(path, "w", encoding="utf-8", newline="").write(text.replace(old, new))
    load = os.path.join(a.work, "appdata", "lichii", "load", "0400486024DEC000")
    env = dict(os.environ, OOT_IMAGE=a.image, OOT_ROMFS=a.romfs, APPDATA=os.path.join(a.work, "appdata"))
    for name, code in (("stage1", STAGE1), ("stage2", STAGE2)):
        if name == "stage2":     # his Customize build reads "the installed folders": stage 1's
            shutil.rmtree(load, ignore_errors=True)
            for mod in STAGE1_MODS:
                shutil.copytree(os.path.join(cs, "build", mod), os.path.join(load, mod))
        script = os.path.join(a.work, name + ".py")
        open(script, "w", encoding="utf-8").write(code)
        subprocess.run([sys.executable, "-W", "ignore", script, cs], cwd=os.path.join(cs, "mods"), env=env, check=True)
    ips = [os.path.join(cs, "build", m, "exefs", BUILD_ID + ".ips") for m in STAGE1_MODS] + \
        [os.path.join(cs, "build-custom", STAGE2_MOD, "exefs", BUILD_ID + ".ips")]
    out = os.path.join(a.work, "charsel-native.ips")
    open(out, "wb").write(merge_ips([p for p in ips if os.path.exists(p)]))
    print("code patch:", out, os.path.getsize(out), "bytes")


if __name__ == "__main__":
    main()
