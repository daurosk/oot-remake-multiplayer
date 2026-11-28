"""The other players on the game's own maps (native-ui-audit item 8): copies of the game's map icon part in the HUD's
map layout, so a co-op module shows them as the game shows Link's arrow, in place of drawn names.

MapMenu_00 (the HUD screen's layout: the minimap and the - menu's Map page) has one part, Pa_MapIcon_00 (layout
PaMapIcon_00), under S_MapIcon_00 inside N_MapIcon_00 > N_PatternField_01 > N_PatternFieldMini_01 >
N_PatternDungeon_01 > N_PatternDungeonMini_01: the game's map icons are copies of it, each showing one of its types
(N_Player_00 the player's arrow PaMapIconPlayer_00, N_Ruto_00, N_Door_00, ...), and the chain above it follows the
map shown (its pan and zoom). This step adds MAP_ICONS more copies of that part beside it, named COPY_NAME % n,
stored hidden: a game without the co-op module looks as it always did. Nothing of the game's own is changed; the part's
layout is Common's own (ui_pack.check_own_parts).
"""
from . import ui_pack

COMMON_ARCHIVE = "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs"
MAP_LAYOUT = "MapMenu_00.buil"
ICON_PART = "Pa_MapIcon_00"
ICON_PARENT = "S_MapIcon_00"
COPY_NAME = "Pa_CoopMapIcon_%02d"   # (hookshot/src/coop_map_icons.h kCopyName: the same names)
MAP_ICONS = 15                      # one per other player (a session holds up to 16: kMaxPlayers)
# Copy 0's z turn word (its pane record's +0x18): 1.2345678f (hookshot/src/coop_map_icons.h kTurnMarkBits, the same
# word). The module reads it on the loaded pane at kTurnMarkAt (+0x68): seen there, the layout's turn is stored there
# (the arrows then turn natively) and it is put back to 0 before the copy is ever shown; not seen, the copies stay down
# and the overlay's arrows stay (map-icons-255-5, B #175; opt-in only, LICHII_COOP_MAP_TURN_AT=check, B #178).
# The copy is stored hidden, so it is never shown turned (no module, or the default path).
TURN_MARK_BITS = 0x3F9E0651


class MapIconsError(ValueError):
    pass


def copy_names():
    return [COPY_NAME % i for i in range(MAP_ICONS)]


def add_icons(d):
    """MapMenu_00.buil with MAP_ICONS hidden copies of ICON_PART under ICON_PARENT, in its place (no move)."""
    panes, _ = ui_pack.read_layout(d)
    names = {p.name for p in panes}
    for name in (ICON_PART, ICON_PARENT):
        if name not in names:
            raise MapIconsError("the layout has no %s" % name)
    if COPY_NAME % 0 in names:
        raise MapIconsError("the layout has %s already" % (COPY_NAME % 0))
    copies = [ui_pack.Copy(ICON_PART, n, parent=ICON_PARENT, hidden=True, turn_bits=TURN_MARK_BITS if i == 0 else None)
              for i, n in enumerate(copy_names())]
    return ui_pack.clone_parts(d, copies, out_of_order=True)


def make(reader, dicts, recompress):
    """({rel: compressed bytes}, growth) for the Common archive with the icons (ui_pack.PackRun's growth)."""
    from . import sarc
    raw = reader.read(COMMON_ARCHIVE)
    plain = dicts.decompress(raw)
    read = sarc.read_entries(plain)
    olds = [data for name, data in read if name == MAP_LAYOUT]
    if not olds:
        raise MapIconsError("%s has no %s" % (COMMON_ARCHIVE, MAP_LAYOUT))
    entries, new = ui_pack.edit_entries(read, MAP_LAYOUT, add_icons)   # (every copy of the layout)
    ui_pack.check_own_parts(read, entries, COMMON_ARCHIVE)
    archive = sarc.write(entries, data_align=16)
    growth = [(MAP_LAYOUT, len(olds[0]), len(new[olds[0]]), False), (COMMON_ARCHIVE, len(plain), len(archive), True)]
    return {COMMON_ARCHIVE: recompress(dicts, raw, archive)}, growth


def build(romfs, out_romfs, dicts, table, write, recompress, log=print, run=None):
    """The Common archive with the icons, into out_romfs (write(out_romfs, rel, bytes)), its size entry grown; all or
    nothing. `run`: the pack run's shared view (ui_pack.PackRun), so the menus' steps' edits of Common are kept."""
    run = run or ui_pack.PackRun(romfs, table)
    files, growth = make(run, dicts, recompress)
    run.check(growth)
    run.commit(files, growth)
    for rel, data in files.items():
        write(out_romfs, rel, data)
    log("map icons: %d hidden copies of %s in %s for the other players" % (MAP_ICONS, ICON_PART, MAP_LAYOUT))
    return list(files)
