"""The title screen's co-op rows (docs/hookshot.md "The title menu"): Host, Join and Co-op
Settings as three more buttons on the game's own title list, under Continue, New Game and
Options, in the game's own look. Made from the player's own game files, as the puppets are.

Writes into the puppet mod's romfs:
  UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs   Title_00.buil with three more
      title button parts (Pa_Button_04..06, the game's PaCommonBtnTitle_00, under A_BtnAlign_00
      as the game's four)
  Mals/<language>.Product.110.sarc.zs   LayoutMsg/Title_00.msbt with the rows' texts (labels
      0100..0102), for every language the game has
and grows those files' entries in the resource size table.

The game's own title code makes a button for every Pa_Button_NN it finds and focuses them with
the pad; the co-op module (hookshot/src/coop_title.cpp) sets the three rows' texts from these
labels, and opens Host, Join or Co-op Settings when one is pressed.

The edits are ui_pack.py's (the Layout2 notes are there): the rows are three copies of the last
button, Pa_Button_03 (clone_parts), and their texts are new labels (add_labels). The title's last
pane is that button, so each copy is the last pane of the alignment pane's subtree, in pre-order,
and no existing offset or index moves.
"""

from . import ui_pack
from .ui_pack import read_layout, read_msbt, write_msbt  # noqa: F401  (the title's tests use them here)

TITLE_ARCHIVE = "UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs"
TITLE_LAYOUT = "Title_00.buil"
MSG_FILE = "LayoutMsg/Title_00.msbt"
MALS_GLOB = "Mals/*.Product.*.sarc.zs"
BUTTON = "Pa_Button_%02d"
FIRST_NEW = 4                 # Pa_Button_04: after the game's 00..03
ROWS = (("0100", "Host"), ("0101", "Join"), ("0102", "Co-op Settings"))
ROW_STEP_Y = -110.0           # the game's buttons are 110 apart

TitleMenuError = ui_pack.UiPackError   # one error for the rows and the library they use
_sections = ui_pack.sections
_label_hash = ui_pack.label_hash


def _new_id(name):
    """The game's own id for a pane of that name (ui_pack.part_id: CityHash64), so it finds the row by name."""
    return ui_pack.part_id(name)


def add_buttons(d, count=len(ROWS)):
    """Title_00.buil with `count` more title buttons after the last one (Pa_Button_03): copies of it,
    ROW_STEP_Y further down each, under the same alignment pane."""
    panes, _ = read_layout(d)
    names = [p[0] for p in panes]
    last = BUTTON % (FIRST_NEW - 1)
    if last not in names or names[-1] != last:
        raise TitleMenuError("%s is not the layout's last pane" % last)
    if any(BUTTON % (FIRST_NEW + i) in names for i in range(count)):
        raise TitleMenuError("the layout has the rows already")
    return ui_pack.clone_parts(d, [ui_pack.Copy(last, BUTTON % (FIRST_NEW + i), (0.0, ROW_STEP_Y * (i + 1)))
                                   for i in range(count)], new_id=_new_id)


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
    log("title menu: Host, Join and Co-op Settings on the title (%s, %d languages)" % (TITLE_LAYOUT, len(files) - 1))
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
    entries, new = ui_pack.edit_entries(entries, TITLE_LAYOUT, add_buttons)   # (every copy of the layout)
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


