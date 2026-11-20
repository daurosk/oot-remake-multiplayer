"""The title screen's own menu (docs/hookshot.md "The title menu"): Start, Multiplayer and Options on
the game's own title buttons, and Multiplayer's Host, Join, Co-op Settings and Back, in the game's
own look. Made from the player's own game files, as the puppets are.

Writes into the puppet mod's romfs:
  UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs   Title_00.buil with one more part,
      Pa_CoopMenu_00: a copy of the last title button, stored hidden. It is never shown; the co-op
      module finds it by name and so knows this pack has the menu's texts
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
ROWS = (("0100", "Host"), ("0101", "Join"), ("0102", "Co-op Settings"), ("0103", "Start"),
        ("0104", "Multiplayer"), ("0105", "Options"), ("0106", "Back"))

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


# ---------------------------------------------------------------------------------------- MSBT

def add_rows(msbt, rows=ROWS):
    """Title_00.msbt with the rows' texts (label, text) added, in the file's encoding; a label the
    file has already gets the row's text."""
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
    entries, new = ui_pack.edit_entries(entries, TITLE_LAYOUT, add_marker)   # (every copy of the layout)
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
        packed = sarc.write([(name, add_rows(data) if name == MSG_FILE else data) for name, data in msgs],
                            data_align=16)
        growth.append((rel, len(plain), len(packed), True))
        files[rel] = recompress(dicts, raw, packed)
    if len(files) < 2:
        raise TitleMenuError("no language has %s" % MSG_FILE)
    return files, growth


