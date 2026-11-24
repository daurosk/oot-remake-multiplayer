"""The title screen's own menu (docs/hookshot.md "The title menu"): Start, Multiplayer and Options on
the game's own title buttons, and Multiplayer's Host, Join, Co-op Settings and Back, in the game's
own look. Made from the player's own game files, as the puppets are.

Writes into the puppet mod's romfs:
  UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs   Title_00.buil with one more part,
      Pa_CoopMenu_00: a copy of the last title button, stored hidden. It is never shown; the co-op
      module finds it by name and so knows this pack has the menu's texts; then Pa_Button_04..13 (the
      rows of co-op's pages on the title, stored hidden) and Pa_CoopHint_00 (a page's hint line)
  Mals/<language>.Product.110.sarc.zs   LayoutMsg/Title_00.msbt with the menu's texts (labels
      0100..0106), for every language the game has
and grows those files' entries in the resource size table.

The rows are the game's own four buttons (Pa_Button_00..03): its BtnNum animation shows three or
four of them, and the co-op module (hookshot/src/coop_title.cpp, CoopTitleMenu::MenuFor) sets their
texts from these labels and answers the presses that are not the game's. Older packs made three more
buttons (Pa_Button_04..06, Host, Join and Co-op Settings) under the game's three; the module still
knows that look (no marker, seven buttons).

The edits are ui_pack.py's (the Layout2 notes are there): the marker is a copy of the last button,
Pa_Button_03 (clone_parts), and the texts are new labels (add_labels). The title's last pane is that
button, so the copy is the last pane of the alignment pane's subtree, in pre-order, and no existing
offset or index moves.
"""

import struct

from . import ui_pack
from .ui_pack import read_layout, read_msbt, write_msbt  # noqa: F401  (the title's tests use them here)

TITLE_ARCHIVE = "UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs"
TITLE_LAYOUT = "Title_00.buil"
MSG_FILE = "LayoutMsg/Title_00.msbt"
MALS_GLOB = "Mals/*.Product.*.sarc.zs"
SCREEN_GLOB = ui_pack.SCREEN_GLOB
SCREEN = "Title_00"                                     # its row: the title screen
SCREEN_ARCHIVES = ["Title", "Common"]                   # its layouts' archives: the toggle's part is in Common
BUTTON = "Pa_Button_%02d"
FIRST_NEW = 4                 # Pa_Button_04: after the game's 00..03 (the older packs' first row)
MARKER = "Pa_CoopMenu_00"     # the hidden part that says this pack has the menu (CoopTitleMenu::kMenuMarker)
# The menu's texts (CoopTitleMenu::kMenuLabels): 0100..0102 are the older packs' rows too.
ROWS = (("0100", "Host a game"), ("0101", "Join a game"), ("0102", "Co-op Settings"), ("0103", "Start"),
        ("0104", "Multiplayer"), ("0105", "Options"), ("0106", "Back"))
# Co-op's pages on the title (Join's list of games, Host, Enter a code) show more rows than the game's four buttons:
# ten more buttons, Pa_Button_04..13, copies of Pa_Button_03 continuing its column (ROW_STEP_Y apart, as the older
# packs' Pa_Button_04..06 were: the game makes a button of every Pa_Button_NN, so their focus bar, move and press
# sounds and mouse are the game's own), stored hidden: the module shows the ones a page uses and keeps the title's
# cursor off the hidden ones (CoopTitleNative). Then HINT, a hidden copy that is no button (not a Pa_Button_NN name):
# the page's one line under its rows (what the focused row does, the page "2 of 3").
LIST_BUTTONS = 10              # Pa_Button_04..13 (CoopTitleNative::kMaxButtons 14 with the game's four)
ROW_STEP_Y = -110.0            # the game's buttons are 110 apart (Title_00: Pa_Button_00..03 at y -65 .. -395)
HINT = "Pa_CoopHint_00"
# The rows' live texts (CoopTitle's native pages on the title's own buttons): a placeholder each, written over in the
# game's memory by the co-op module (as the pause menu's Coop_Live_NN): one per button, then the hint line's (longer).
# The Host page's "Who can join" row is the game's own Options toggle (owner, 2026-11-21: "a slider for either public or
# private"): TOGGLE, a part of the Options screen's two-cell layout TOGGLE_LAYOUT (SystemOption_00's Pa_ToggleBtn_00: its
# name T_Option_00 on the left, two cells W_CheckL_00 / T_BtnON_00 and W_CheckR_00 / T_BtnOFF_00, its own Select,
# UnSelect and Check animations), in Pa_Button_00's place, stored hidden. That layout and its textures are the Common
# archive's (the title's own archive has neither): the part names it, and the game finds it there (a probe run settles
# it; docs/hookshot.md). Drawn at TOGGLE_SCALE so it keeps to a title row's height (110), moved by TOGGLE_MOVE so its
# name and cells sit centred under the column. Its three texts are live texts of their own (TOGGLE_LIVE): the module
# shows the part over the row's title button (which stays the cursor's) and gives it its words.
TOGGLE = "Pa_CoopToggle_00"
TOGGLE_LAYOUT = "PaOptionBtnL_00"
TOGGLE_SIZE = (1920.0, 126.0)     # its N_BtnWidth_00, as SystemOption_00 sizes it
TOGGLE_SCALE = (0.8, 0.8)         # 126 high at 1.0: about 100 here, inside a row's 110
TOGGLE_MOVE = (150.0, 0.0)        # its name (x -840) and cells (to x +920) about the column's middle
TOGGLE_LIVE = tuple(("Coop_Live_Toggle%d" % i, "." * 64) for i in range(3))   # its name, the left cell, the right
LIVE = (tuple(("Coop_Live_T%d" % i, "." * 64) for i in range(FIRST_NEW + LIST_BUTTONS)) + (("Coop_Live_Hint", "." * 160),)
        + TOGGLE_LIVE)
# The list's ZL and ZR as the game's own button icons (menu-title-01113; row 264 did it for the - menu): the game draws a
# button only as a letter of its icon font External_00 (StaticMsg/GuideKeyIcon: ZL U+E046, ZR U+E047) in a text pane of
# that font, and the Title archive has none (its words panes are Normal: the letters drew as empty boxes, row 264). A
# part of another archive's layout crashed the title's load (title-toggle-0119), so the Common archive's controller keys
# PaControllerKeyL_00 / R_00 (text panes only: the icon T_IconL_00 / T_IconR_00 in External_00 beside a label, no
# picture, no part, no texture) are COPIED into the Title archive under names of ours (KEY_LAYOUTS; four copies as every
# layout there, and listed under Title in the layout category table as its own layouts are), and Title_00 gets two
# hidden parts of them (KEYS) level with the first row, their icons KEY_ICON_X outside the column's middle: past the
# title button's ornaments (PaCommonBtnTitle_00's N_Deco_L_00 / R_00 at -+464). The icons' letters are messages of
# Title_00.msbt (KEY_LABELS), set by the co-op module (coop_title.cpp, TitleKeys), which shows them only on a page of
# more than one; without them (an older pack, or a merge with another mod's title: merge_tables keeps add_menu) the hint
# names the buttons in words. The parts' place: the key layout's icon sits KEY_ICON_IN inward of its root (the layout's
# T_IconL_00 at x +213.3, T_IconR_00 at -213.3, 8 up).
KEY_LAYOUTS = (("PaControllerKeyL_00", "PaCoopKeyTitleL_00"), ("PaControllerKeyR_00", "PaCoopKeyTitleR_00"))
KEYS = ("Pa_CoopKeyZL_00", "Pa_CoopKeyZR_00")
KEY_ICON_X = 660.0
KEY_ICON_IN = 213.3
KEY_SIZE = (60.0, 80.0)          # (the key layout's own N_IconGuideL_00 / R_00)
KEY_LABELS = (("Coop_Key_ZL", "\ue046"), ("Coop_Key_ZR", "\ue047"))
COMMON_ARCHIVE = "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs"
CATEGORY = "UI/LayoutArchiveCategory.Product.100.byml.zs"
CATEGORY_ARCHIVE = "Title"
COPIES = 4                       # (every layout is stored four times in its archive)
# The same rows for the languages whose game text is Chinese (the kit's Chinese layer keeps the
# game's own tables, as its menus and messages do): the pack carries these words in that
# language's Title_00.msbt, since the module only names the label (CoopTitleMenu::kMenuLabels)
# and the game reads the text from the message file of the language the game runs in.
ROWS_BY_LANGUAGE = {
    "CNzh": (("0100", "创建房间"), ("0101", "加入房间"), ("0102", "联机设置"), ("0103", "开始"),
             ("0104", "联机"), ("0105", "选项"), ("0106", "返回")),
    "TWzh": (("0100", "建立房間"), ("0101", "加入房間"), ("0102", "連線設定"), ("0103", "開始"),
             ("0104", "連線"), ("0105", "選項"), ("0106", "返回")),
}


def rows_for(rel):
    """The menu's rows for a language's message file ("Mals/CNzh.Product.110.sarc.zs"): that
    language's words when it has them, the English above otherwise, and the live texts' placeholders."""
    name = rel.replace("\\", "/").rsplit("/", 1)[-1]
    return ROWS_BY_LANGUAGE.get(name.split(".", 1)[0], ROWS) + LIVE + KEY_LABELS


TitleMenuError = ui_pack.UiPackError   # one error for the rows and the library they use
_sections = ui_pack.sections
_label_hash = ui_pack.label_hash


def _new_id(name):
    """The game's own id for a pane of that name (ui_pack.part_id: CityHash64), so it finds the row by name."""
    return ui_pack.part_id(name)


def add_marker(d):
    """Title_00.buil with the marker part after the last button (Pa_Button_03): a copy of it, stored
    hidden, under the same alignment pane. No new title button: the game makes buttons only for the
    Pa_Button_NN parts."""
    panes, _ = read_layout(d)
    names = [p[0] for p in panes]
    last = BUTTON % (FIRST_NEW - 1)
    if last not in names or names[-1] != last:
        raise TitleMenuError("%s is not the layout's last pane" % last)
    if MARKER in names or BUTTON % FIRST_NEW in names:
        raise TitleMenuError("the layout has the menu already")
    return ui_pack.clone_parts(d, [ui_pack.Copy(last, MARKER, (0.0, 0.0), hidden=True)], new_id=_new_id)


def add_list(d):
    """Title_00.buil (with the marker) with the list's buttons Pa_Button_04..13 and the hint line HINT after it:
    copies of Pa_Button_03 under the same alignment pane, each ROW_STEP_Y under the one before, stored hidden."""
    panes, _ = read_layout(d)
    names = [p[0] for p in panes]
    if MARKER not in names:
        raise TitleMenuError("the layout has no %s (add_marker first)" % MARKER)
    if HINT in names or BUTTON % FIRST_NEW in names:
        raise TitleMenuError("the layout has the list already")
    last = BUTTON % (FIRST_NEW - 1)
    copies = [ui_pack.Copy(last, BUTTON % (FIRST_NEW + i), (0.0, ROW_STEP_Y * (i + 1)), hidden=True)
              for i in range(LIST_BUTTONS)]
    copies.append(ui_pack.Copy(last, HINT, (0.0, ROW_STEP_Y * (LIST_BUTTONS + 1)), hidden=True))
    return ui_pack.clone_parts(d, copies, new_id=_new_id)


def add_toggle(d):
    """Title_00.buil (with the list) with TOGGLE after it: a part of TOGGLE_LAYOUT in Pa_Button_00's place (moved by
    TOGGLE_MOVE, at TOGGLE_SCALE), under the same alignment pane, stored hidden. No title button (not a Pa_Button_NN)."""
    panes, _ = read_layout(d)
    names = [p[0] for p in panes]
    if HINT not in names:
        raise TitleMenuError("the layout has no %s (add_list first)" % HINT)
    if TOGGLE in names:
        raise TitleMenuError("the layout has the toggle already")
    return ui_pack.clone_parts(d, [ui_pack.Copy(BUTTON % 0, TOGGLE, TOGGLE_MOVE, hidden=True, part_layout=TOGGLE_LAYOUT,
                                                size=TOGGLE_SIZE, scale=TOGGLE_SCALE)], new_id=_new_id)


def add_menu(d):
    """The marker, then the list's buttons and the hint line. Not the Host page's toggle (add_toggle): its part's
    layout is Common's and the title's archive is Title, so every boot crashed as the archive loaded (LANE1
    2026-11-22, also with Common in Title_00's archives: add_common_archive); ui_pack.check_own_parts refuses it now.
    Without the part the module keeps the row's own text (coop_title.cpp's toggle notes)."""
    return add_list(add_marker(d))


def add_keys(d):
    """Title_00.buil (with the list) with KEYS after it: parts of KEY_LAYOUTS' copies (the Title archive's own, as
    make adds them), in Pa_Button_00's place moved out to either side so their icons stand KEY_ICON_X from the column's
    middle, under the same alignment pane, stored hidden. No title button (not a Pa_Button_NN)."""
    panes, _ = read_layout(d)
    names = [p[0] for p in panes]
    if HINT not in names:
        raise TitleMenuError("the layout has no %s (add_list first)" % HINT)
    if any(k in names for k in KEYS):
        raise TitleMenuError("the layout has the keys already")
    x = KEY_ICON_X + KEY_ICON_IN
    copies = [ui_pack.Copy(BUTTON % 0, KEYS[i], (-x if i == 0 else x, 0.0), hidden=True,
                           part_layout=KEY_LAYOUTS[i][1], size=KEY_SIZE) for i in range(2)]
    return ui_pack.clone_parts(d, copies, new_id=_new_id)


def add_menu_keys(d):
    """add_menu, then the list's ZL and ZR keys (add_keys): for the Title archive make writes with the keys' layouts."""
    return add_keys(add_menu(d))


def key_layouts(common_entries):
    """[(name.buil, bytes)]: the Common archive's PaControllerKeyL_00 / R_00 under KEY_LAYOUTS' names, checked to be
    what add_keys expects (text panes only, no part, the icon pane there); raises TitleMenuError otherwise."""
    from .coop_menu import rename
    have = dict(common_entries)
    out = []
    for source, ours in KEY_LAYOUTS:
        data = have.get(source + ".buil")
        if data is None:
            raise TitleMenuError("%s has no %s" % (COMMON_ARCHIVE, source + ".buil"))
        panes, parts = read_layout(data)
        icon = "T_Icon%s_00" % source[-4]
        if parts or icon not in [p[0] for p in panes] or b"External_00" not in data:
            raise TitleMenuError("%s is not the controller key it was made against" % source)
        rec = dict((p[0], p[1]) for p in panes)[icon]
        tx, = struct.unpack_from("<f", data, rec + 0x1c)
        if abs(abs(tx) - KEY_ICON_IN) > 1.0:
            raise TitleMenuError("%s's icon is at x %.1f, not %.1f" % (source, tx, KEY_ICON_IN))
        out.append((ours + ".buil", rename(data, ours)))
    return out


def add_category(cat):
    """The layout category table with KEY_LAYOUTS' names under Title (in name order, as the game's list)."""
    names = cat.get(CATEGORY_ARCHIVE) if isinstance(cat, dict) else None
    if not isinstance(names, list) or SCREEN not in names:
        raise TitleMenuError("the layout category table has no %s list with %s" % (CATEGORY_ARCHIVE, SCREEN))
    for _, ours in KEY_LAYOUTS:
        if ours not in names:
            at = next((i for i, n in enumerate(names) if str(n).lower() > ours.lower()), len(names))
            names.insert(at, ours)
    return cat


# ---------------------------------------------------------------------------------------- MSBT

def add_rows(msbt, rows=ROWS + LIVE):
    """Title_00.msbt with the rows' texts (label, text) added, and the live texts' placeholders, in
    the file's encoding; a label the file has already gets the row's text."""
    return ui_pack.add_labels(msbt, dict(rows), replace=True)


# ---------------------------------------------------------------------------------------- build

def build(romfs, out_romfs, dicts, table, write, recompress, log=print, run=None):
    """The title archive and every language's messages from the romfs folder, into out_romfs
    (write(out_romfs, rel, bytes)); their size table entries grown (`table`: rstb.Rstb). Returns
    the files written. `run`: the pack run's shared view (ui_pack.PackRun) when other UI builders
    edit the same files (the language archives); a view of its own when None.

    All or nothing: every file and size is made in memory first; only when all of them were made
    (the layout and every language that has Title_00.msbt) is anything written or the table
    changed. A failure raises before that, so the pack never holds rows without their texts."""
    run = run or ui_pack.PackRun(romfs, table)
    files, growth = make(run, dicts, recompress)
    run.check(growth)
    run.commit(files, growth)
    for rel, data in files.items():
        write(out_romfs, rel, data)
    log("title menu: Start, Multiplayer and Options on the title (%s, %d languages)" % (TITLE_LAYOUT, len(files) - 1))
    return list(files)


def add_common_archive(rows):
    """Title_00's row of the screens table (ui_pack.edit_screens) with its ArchiveCategory holding Common after
    Title: the game's layout loader finds a part's layout only in the screen's own archives, and the Host page's
    toggle (PaOptionBtnL_00, Options' two-cell button) is Common's (the LANE1 probe of 2026-11-22: a part from
    another archive made the title crash on a null read)."""
    row = next((r for r in rows if isinstance(r, dict) and r.get("Name") == SCREEN), None)
    if row is None:
        raise TitleMenuError("the screens table has no %s" % SCREEN)
    have = list(row.get("ArchiveCategory", []))
    if not have or have[0] != SCREEN_ARCHIVES[0]:
        raise TitleMenuError("%s's archives are %r, not Title first" % (SCREEN, have))
    row["ArchiveCategory"] = have + [a for a in SCREEN_ARCHIVES if a not in have]


def make(reader, dicts, recompress):
    """({rel: compressed bytes}, [(rel, plain length read, plain length made, required)]) for the
    title archive and every language's messages (ui_pack.PackRun's growth); raises TitleMenuError (or
    the reader's error) when any one cannot be made."""
    from . import sarc
    files, growth = {}, []
    raw = reader.read(TITLE_ARCHIVE)
    entries = sarc.read_entries(dicts.decompress(raw))
    olds = [data for name, data in entries if name == TITLE_LAYOUT]
    if not olds:
        raise TitleMenuError("%s has no %s" % (TITLE_ARCHIVE, TITLE_LAYOUT))
    # (the list's ZL / ZR keys when the Common archive has the controller keys as made against: their layouts copied
    # in, the category told; else the title as before, its hint naming them in words)
    from . import byml
    try:
        keys = key_layouts(sarc.read_entries(dicts.decompress(reader.read(COMMON_ARCHIVE))))
        cat_raw = reader.read(CATEGORY)
        cat_plain = dicts.decompress(cat_raw)
        cat_made = byml.dumps(add_category(byml.loads(cat_plain)))
    except (TitleMenuError, OSError, KeyError, ValueError, struct.error):
        keys = []
    read, (entries, new) = entries, ui_pack.edit_entries(entries, TITLE_LAYOUT, add_menu_keys if keys else add_menu)
    for name, data in keys:
        entries = entries + [(name, data)] * COPIES
    ui_pack.check_own_parts(read, entries, TITLE_ARCHIVE)
    archive = sarc.write(entries, data_align=16)   # (as the game's own: 16)
    growth.append((TITLE_ARCHIVE, len(dicts.decompress(raw)), len(archive), True))
    growth.append((TITLE_LAYOUT, len(olds[0]), len(new[olds[0]]), False))
    files[TITLE_ARCHIVE] = recompress(dicts, raw, archive)
    if keys:
        growth.append((CATEGORY, len(cat_plain), len(cat_made), True))
        files[CATEGORY] = recompress(dicts, cat_raw, cat_made)
    for rel in sorted(reader.glob(MALS_GLOB)):
        raw = reader.read(rel)
        plain = dicts.decompress(raw)
        msgs = sarc.read_entries(plain)
        if not any(name == MSG_FILE for name, _ in msgs):
            continue
        rows = rows_for(rel)
        packed = sarc.write([(name, add_rows(data, rows) if name == MSG_FILE else data) for name, data in msgs],
                            data_align=16)
        growth.append((rel, len(plain), len(packed), True))
        files[rel] = recompress(dicts, raw, packed)
    if len(files) < 2:
        raise TitleMenuError("no language has %s" % MSG_FILE)
    return files, growth


