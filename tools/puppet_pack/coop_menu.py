"""Co-op's own - menu screen, CoopMenu_00: Multiplayer, Lobby and Mods as a page of the game's own, right of the Map
(tabs-overhaul-01110 step 2; the owner, 2026-11-22: "the tabs should look native to the game ... as good as the
character select tab by snail").

0.11.9 put the three tabs' page (pause_menu.py's Pa_CoopMinus_00) in the tab strip's layout while the - menu stayed on
the Map: no page backdrop of its own and no In / Out animation of the game's. Snail's Characters page (OoT Character
Select v0.2.1, MIT, used with his permission) is a screen of its own opened by the - menu's sequence; this is the same
for ours, built from the player's own files at pack time:

  UI/Layout2Archive/Common: CoopMenu_00.buil, four copies as the game's layouts: Key Items' layout (ImportantMenu_01:
      the vignette window W_Window_00, its In / InChallenge / InFromChallenge animations) under our name, Key Items'
      own content stored hidden (its ring of items, the detail box, the demo message), and Pa_CoopMinus_00 (a part of
      the System page's layout, PaSystemMenu_00: its rows, the Options list and the help caption, as pause_menu.py
      makes it) under the root (the vignette and its window come and go with the page's own In / Out animations, the
      co-op module slides our rows with them on the game's curve); stored hidden, the module shows it once its rows
      have their words (no placeholder dots on the way in).
  UI/LayoutArchiveCategory: CoopMenu_00 listed under Common (in name order, as the game's list).
  RSDB/UIScreen: the screen's row (no screen class, the menu's draw unit: Snail's page's row; its sort key one above
      the pages', so it draws over the Map's screen left open under it).
  Mals/*: LayoutMsg/CoopMenu_00.msbt, a copy of Key Items' messages (a layout's text panes read their layout's file);
      our rows' words stay the strip's (LayoutMsg/MenuOverlay_00, loaded while the - menu shows).
  Sequence/Map.module.ainb: R / ZR on the Map open CoopMenu_00, B / - / + close it as the Map page closes, L goes back
      (coopmenu/make_data.py, Snail's sequence helpers; a CDLT1 delta checked by both SHA-1s, charsel_ui's format).

All or nothing, and only where every one of the game's files is as this was made against (the Map's sequence edited by
another mod, or no Key Items layout: CoopMenuError, the pack keeps the 0.11.9 page in the strip: pause_menu.py with
minus_page=True). With the Character Select mod in the mods his files win the layout category and the screens table,
so build.py builds this only with co-op's own Characters tab (charsel_native).
"""
import copy
import os
import struct

from . import charsel_ui, pause_menu, ui_pack

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "coopmenu", "data")
SCREEN = "CoopMenu_00"                 # the screen's and its layout's name (CoopPauseTab: kCoopScreen)
SOURCE = "ImportantMenu_01"            # Key Items' layout
COMMON_ARCHIVE = pause_menu.COMMON_ARCHIVE
CATEGORY = "UI/LayoutArchiveCategory.Product.100.byml.zs"
MALS_GLOB = pause_menu.MALS_GLOB
MSG = "LayoutMsg/%s.msbt"
COPIES = 4                             # (every layout is stored four times in its archive)
# Key Items' own content, stored hidden in our copy: the ring of items (Pa_Parts_00, N_Info_00..08), the detail box
# (N_DetailIn_00: the item's picture, name and caption) and the demo message (N_DemoMessage).
HIDE = ("Pa_Parts_00",) + tuple("N_Info_%02d" % i for i in range(9)) + ("N_DetailIn_00", "N_DemoMessage")
# (under the root: a new pane is the last one, and only the root's subtree reaches it (ui_pack's notes), not the guide
# group's; the co-op module slides the rows in and out on the game's curve with the page's own In and Out)
PAGE_PARENT = "RootPane"
PAGE_SOURCE = "Pa_Parts_00"            # (a part of the layout's own, in whose place ours goes, moved to the page's place)
MAP = "Sequence/Map.module.ainb"
MAP_SRC_SHA1 = "5de2c395f07e09f5b3ea071954e1708c66bfe0bf"
MAP_DST_SHA1 = "f20e905dea4ce87172182223a336651a5183b55f"
MANIFEST = {"format": "coop-menu 1", "files": [{"rel": MAP, "zs": False, "from": MAP, "delta": "map.cdelta"}]}
# The screen's row: Snail's page's (Key Items' draw unit, loaded on demand), its sort key one above the menu pages' 5000.
# The Map's screen stays open under ours (Snail's page opens after Key Items has closed; ours from the Map's own state),
# and at the same key the Map drew over ours: its full-screen backdrop BG_00 (outside the Map's In / Out animations,
# kept by the co-op module) left the whole page black, rows and vignette too (LANE1 rerun 2026-11-22, s02..s10 all
# 0,0,0 under the strip). 5001 is still under everything above the pages (the HUD 6000+, the strip 8749, the system
# windows 8751); the row has no IsEnableControl, so which screen reads the pad does not change.
SORT_KEY = 5001
ROW = {"ArchiveCategory": ["Common"], "BodyLayoutFile": SCREEN, "DrawUnitId": 1, "IsHeapReverse": True,
       "Name": SCREEN, "SortKey": SORT_KEY}


class CoopMenuError(ValueError):
    pass


def rename(d, name):
    """A layout (.buil bytes) under another name: the name's string added at the string pool's end (the pool grows to
    the file's end, as ui_pack.clone_parts adds its names) and the root's name pointed at it."""
    pool_hdr, pool_off, pool_size = ui_pack.sections(d)["StringPool"]
    if pool_off + pool_size != len(d):
        raise CoopMenuError("the string pool is not the layout's end")
    out = bytearray(d)
    while len(out) % 8:
        out.append(0)
    at = len(out)
    out += name.encode("ascii") + b"\0"
    while len(out) % 8:
        out.append(0)
    struct.pack_into("<Q", out, ui_pack._root(d) + 0x10, at)
    struct.pack_into("<I", out, pool_hdr + 8, len(out) - pool_off)
    struct.pack_into("<I", out, 0x08, len(out))
    return bytes(out)


def layout_name(d):
    return ui_pack._cstr(d, ui_pack._q(d, ui_pack._root(d) + 0x10))


def hide(d, names):
    """The layout with each pane in `names` stored hidden (bit 0 of its kind word cleared, the game's own way)."""
    out = bytearray(d)
    panes, _ = ui_pack.read_layout(d)
    by = {p.name: p for p in panes}
    for n in names:
        if n not in by:
            raise CoopMenuError("%s has no %s" % (SOURCE, n))
        rec = by[n].record
        struct.pack_into("<I", out, rec + 0x38, struct.unpack_from("<I", out, rec + 0x38)[0] & ~1)
    return bytes(out)


def make_layout(source, place, size):
    """CoopMenu_00.buil from Key Items' layout (see the notes); place / size: pause_menu.shown_page."""
    panes, _ = ui_pack.read_layout(source)
    names = {p.name for p in panes}
    for n in (PAGE_SOURCE, "W_Window_00"):
        if n not in names:
            raise CoopMenuError("%s has no %s" % (SOURCE, n))
    if pause_menu.MINUS_PAGE in names:
        raise CoopMenuError("%s has %s already" % (SOURCE, pause_menu.MINUS_PAGE))
    d = hide(rename(source, SCREEN), HIDE)
    sx, sy = struct.unpack_from("<ff", d, next(p for p in panes if p.name == PAGE_SOURCE).record + 0x1c)
    move = (place[0] - sx, place[1] - sy)   # (the copy's translate under the root is the source's plus the move)
    return ui_pack.clone_parts(d, [ui_pack.Copy(PAGE_SOURCE, pause_menu.MINUS_PAGE, move, parent=PAGE_PARENT,
                                                hidden=True, part_layout=pause_menu.SYSTEM_LAYOUT[:-len(".buil")],
                                                size=tuple(size))], new_id=pause_menu._new_id)


def add_category(cat):
    """The layout category table with SCREEN under Common, before the first name greater (case ignored)."""
    names = cat.get("Common") if isinstance(cat, dict) else None
    if not isinstance(names, list) or SOURCE not in names:
        raise CoopMenuError("the layout category table has no Common list with %s" % SOURCE)
    if SCREEN not in names:
        at = next((i for i, n in enumerate(names) if str(n).lower() > SCREEN.lower()), len(names))
        names.insert(at, SCREEN)
    return cat


def add_row(rows):
    if not isinstance(rows, list) or not any(r.get("BodyLayoutFile") == SOURCE for r in rows):
        raise CoopMenuError("the screens table has no row for %s" % SOURCE)
    rows = [r for r in rows if r.get("Name") != SCREEN] + [copy.deepcopy(ROW)]
    rows.sort(key=lambda r: str(r.get("Name", "")).encode())
    return rows


def make(reader, dicts, recompress):
    """({rel: file bytes as the game reads them}, growth) for every file of the screen; raises CoopMenuError (or
    CharselError, UiPackError, the reader's error) when any one cannot be made. Reads through `reader` (the pack run's
    view), so it goes after the pause menu's step (pause_menu.make with minus_page=False)."""
    from . import byml, sarc
    files, growth, new = charsel_ui.make(reader, dicts, recompress, manifest=MANIFEST, data_dir=DATA)
    if new:
        raise CoopMenuError("the Map's sequence is the game's own file, not a new one")
    import hashlib
    if hashlib.sha1(files[MAP]).hexdigest() != MAP_DST_SHA1:
        raise CoopMenuError("the Map's sequence came out unlike the one made against")
    raw = reader.read(COMMON_ARCHIVE)
    plain = dicts.decompress(raw)
    entries = sarc.read_entries(plain)
    have = dict(entries)
    for n in (SOURCE + ".buil", pause_menu.PAGES_LAYOUT, pause_menu.SYSTEM_LAYOUT):
        if n not in have:
            raise CoopMenuError("%s has no %s" % (COMMON_ARCHIVE, n))
    if SCREEN + ".buil" in have:
        raise CoopMenuError("%s has %s already" % (COMMON_ARCHIVE, SCREEN + ".buil"))
    place, size = pause_menu.shown_page(have[pause_menu.PAGES_LAYOUT])
    ours = make_layout(have[SOURCE + ".buil"], place, size)
    ui_pack.check_own_parts(entries, entries + [(SCREEN + ".buil", ours)], COMMON_ARCHIVE)
    entries = entries + [(SCREEN + ".buil", ours)] * COPIES   # (Pa_CoopMinus_00's PaSystemMenu_00 is Common's)
    archive = sarc.write(entries, data_align=16)
    growth.append((COMMON_ARCHIVE, len(plain), len(archive), True))
    files[COMMON_ARCHIVE] = recompress(dicts, raw, archive)
    raw = reader.read(CATEGORY)
    plain = dicts.decompress(raw)
    made = byml.dumps(add_category(byml.loads(plain)))
    growth.append((CATEGORY, len(plain), len(made), True))
    files[CATEGORY] = recompress(dicts, raw, made)
    rel, data, grew = ui_pack.edit_screens(reader, dicts, recompress, add_row)
    files[rel] = data
    growth.append(grew)
    langs = 0
    for rel in sorted(reader.glob(MALS_GLOB)):
        raw = reader.read(rel)
        plain = dicts.decompress(raw)
        msgs = sarc.read_entries(plain)
        names = {n for n, _ in msgs}
        if MSG % SOURCE not in names or MSG % SCREEN in names:
            continue
        msgs.append((MSG % SCREEN, dict(msgs)[MSG % SOURCE]))
        packed = sarc.write(msgs, data_align=16)
        growth.append((rel, len(plain), len(packed), True))
        files[rel] = recompress(dicts, raw, packed)
        langs += 1
    if langs == 0:
        raise CoopMenuError("no language has %s" % (MSG % SOURCE))
    return files, growth


def build(romfs, out_romfs, dicts, table, write, recompress, log=print, run=None):
    """The files into out_romfs (write(out_romfs, rel, bytes)), their size-table entries grown; all or nothing.
    Returns the files written."""
    run = run or ui_pack.PackRun(romfs, table)
    files, growth = make(run, dicts, recompress)
    run.check(growth)
    run.commit(files, growth)
    for rel, data in files.items():
        write(out_romfs, rel, data)
    log("co-op's - menu screen: %s right of the Map (%d files)" % (SCREEN, len(files)))
    return list(files)


def sources():
    """The game's files this is made from besides the pause menu's (build.py nup_menu_files)."""
    return [MAP, CATEGORY]
