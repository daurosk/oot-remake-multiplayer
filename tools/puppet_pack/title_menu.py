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

from . import ui_pack
from .ui_pack import read_layout, read_msbt, write_msbt  # noqa: F401  (the title's tests use them here)

TITLE_ARCHIVE = "UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs"
TITLE_LAYOUT = "Title_00.buil"
MSG_FILE = "LayoutMsg/Title_00.msbt"
MALS_GLOB = "Mals/*.Product.*.sarc.zs"
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
LIVE = tuple(("Coop_Live_T%d" % i, "." * 64) for i in range(FIRST_NEW + LIST_BUTTONS)) + (("Coop_Live_Hint", "." * 160),)
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
    return ROWS_BY_LANGUAGE.get(name.split(".", 1)[0], ROWS) + LIVE


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


def add_menu(d):
    """The marker, then the list's buttons and the hint line."""
    return add_list(add_marker(d))


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
    entries, new = ui_pack.edit_entries(entries, TITLE_LAYOUT, add_menu)   # (every copy of the layout)
    archive = sarc.write(entries, data_align=16)   # (as the game's own: 16)
    growth.append((TITLE_ARCHIVE, len(dicts.decompress(raw)), len(archive), True))
    growth.append((TITLE_LAYOUT, len(olds[0]), len(new[olds[0]]), False))
    files[TITLE_ARCHIVE] = recompress(dicts, raw, archive)
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


