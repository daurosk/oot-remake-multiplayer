"""The pause menu's pages of ours (issue #264, piece 4; docs/hookshot.md "The pause menu's
Multiplayer page"): the - menu's tabs Multiplayer, Lobby and Mods and their page (0.11.9: co-op's
one home), and Longshot Settings, opened from a button on the game's own System page; all in the
game's own look. Made from the player's own game files, as the puppets are.

Writes into the puppet mod's romfs:
  UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs
      MenuOverlay_00.buil (the tab strip): the - menu's tabs of ours (Pa_Category_04..06) and their page,
          Pa_CoopMinus_00: a part of the System page's layout where a shown + page sits (add_minus_page)
      PlusMenu_00.buil (the pages): Pa_Settings_00, a copy of the System page where a page shows
          (Longshot Settings: Longshot's options)
      PaSystemMenu_00.buil (the System page's own layout, which all three pages use): Pa_List_00..07,
          eight of the game's Options rows (PaOptionBtnLevel_00: a name on the left, a value
          between arrows on the right) down the right half, where the controller picture is: the
          game browser's list (a header, then six games), and the settings' list; and Pa_Button_05
          a copy of Pa_Button_03 below Pa_Button_04: System's Longshot Settings button. Stored hidden, so the System page never shows them; the module
          shows the list on ours (its copy of the controller picture is hidden) and the buttons on
          System's
  Mals/<language>.Product.110.sarc.zs
      LayoutMsg/PlusMenu_00.msbt: the buttons' names and the page's rows (strings/en.txt and its
      translations: labels named for the panes they fill, as the game's own
      Pa_System_00-Pa_Button_00-T_Btn_00)
and grows those files' entries in the resource size table.

The copies are stored hidden (ui_pack's Copy.hidden): the pages sit under N_InOut_00 at x 0, over
every page, so only the co-op module shows them (and the buttons), and a game with an older module
or none looks as it always did.

What the game does with them (read from the game's code):
  - no code of the game binds Pa_Settings_00 (the menu binds its four pages by name), and the game
    makes a page's buttons in that page's own code, so the copy has no game buttons of its own;
    the System page's own code makes a button of every PaSystemMenu_00 button part, ours too;
  - the co-op module (hookshot/src/coop_pause_tab_module.cpp) shows a page in System's place when
    A is pressed on its button, and gives System back on B, L or R.

Where the copies go (ui_pack.py's notes): the pages' pane does not reach the layout's last pane,
so a copy under it would break the pane order. The page goes under N_InOut_00 (the pages' and the
menu's In and Out), where the shown page sits: the pages are side by side on a strip (System at
x 12840) that the menu slides, and the System page shows when the strip has slid by its x.
"""
import struct

from . import ui_pack

COMMON_ARCHIVE = "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs"
OVERLAY_LAYOUT = "MenuOverlay_00.buil"
PAGES_LAYOUT = "PlusMenu_00.buil"
OVERLAY_MSG = "LayoutMsg/MenuOverlay_00.msbt"
PAGES_MSG = "LayoutMsg/PlusMenu_00.msbt"
MALS_GLOB = "Mals/*.Product.*.sarc.zs"

TAB = "Pa_Category_%02d"
GAME_TABS = 4                 # Pa_Category_00..03
# The - menu's tabs of ours (0.11.9: Multiplayer, Lobby, Mods, right of Map; CoopPauseTab::kMinusTabNames):
# Pa_Category_04..06, copies of the fourth tab one game step apart after it.
MINUS_TABS = [TAB % (GAME_TABS + i) for i in range(3)]
MINUS_SOURCE = TAB % (GAME_TABS - 1)
TAB_ROOT = "RootPane"
L_LABEL, R_LABEL = "T_BtnL_00", "T_BtnR_00"
SYSTEM_PAGE = "Pa_System_00"
SETTINGS_PAGE = "Pa_Settings_00"
PAGE_PARENT = "N_InOut_00"
SYSTEM_LAYOUT = "PaSystemMenu_00.buil"
ROW = "Pa_List_%02d"           # the game browser's rows (CoopPauseTab::kListRows: a header and kCells games)
# 8: the eighth (Pa_List_07) is Longshot Settings' alone, its header and seven settings a page (a tester: "it
# fits"); the Multiplayer page uses the first seven, and a module that knows only seven leaves it hidden.
ROWS = 8
ROW_SOURCE = "Pa_Button_00"     # (a part of the page's own, in whose place each row is made)
ROW_LAYOUT = "PaOptionBtnLevel_00"   # the game's Options row: T_Option_00 on the left, T_Text_00 between arrows
ROW_SIZE = (1920.0, 126.0)      # its N_BtnWidth_00
ROW_X = 540.0                   # the centre of the controller picture's half (2600 wide)
ROW_TOP, ROW_STEP = 420.0, -172.0   # the Options screen's rows are about 172 apart (seen in a run, 2026-11-19)

PauseMenuError = ui_pack.UiPackError


def _new_id(name):
    """The game's own id for a pane of that name (ui_pack.part_id, CityHash64): it finds panes by it."""
    return ui_pack.part_id(name)




def _x(d, name):
    panes, _ = ui_pack.read_layout(d)
    for p in panes:
        if p.name == name:
            return struct.unpack_from("<f", d, p.record + 0x1c)[0]
    raise PauseMenuError("the layout has no %s" % name)


def add_tab(d):
    """MenuOverlay_00.buil with the - menu's tabs of ours (0.11.9; the owner: Character Select's Characters, then
    Multiplayer, Lobby and Mods right of Map): Pa_Category_04..06, copies of the fourth tab (Pa_Category_03, the - menu's
    Map with Character Select's header) one game step apart after it, under RootPane, stored hidden. The strip counts
    its Pa_Category_%02d parts up to the first missing, so ours follow the game's four; the game's tabs, L and R stay:
    the strip's animation sets the tabs' places, so the module places and shows ours (the way 0.11.6's fifth tab
    ran)."""
    panes, _ = ui_pack.read_layout(d)
    names = {p.name for p in panes}
    for i in range(GAME_TABS):
        if TAB % i not in names:
            raise PauseMenuError("the layout has no %s" % (TAB % i))
    for name in MINUS_TABS:
        if name in names:
            raise PauseMenuError("the layout has %s already" % name)
    for name in (L_LABEL, R_LABEL, TAB_ROOT):
        if name not in names:
            raise PauseMenuError("the layout has no %s" % name)
    step = _x(d, TAB % 1) - _x(d, TAB % 0)
    if step <= 0 or any(abs(_x(d, TAB % (i + 1)) - _x(d, TAB % i) - step) > 0.5 for i in range(GAME_TABS - 1)):
        raise PauseMenuError("the game's tabs are not evenly spaced from left to right")
    src_off = ui_pack.frame_offset(d, MINUS_SOURCE, TAB_ROOT)
    return ui_pack.clone_parts(d, [ui_pack.Copy(MINUS_SOURCE, name, ((i + 1) * step + src_off[0], src_off[1]),
                                                parent=TAB_ROOT, hidden=True) for i, name in enumerate(MINUS_TABS)],
                               new_id=_new_id)


MINUS_PAGE = "Pa_CoopMinus_00"   # the - menu's page of ours (CoopPauseTab: Multiplayer and Lobby's rows)
MINUS_PAGE_SOURCE = MINUS_SOURCE  # (a part of the strip's own, in whose place it is made)


def shown_page(pages_layout):
    """((x, y), (w, h)) of a shown + page in the frame of PlusMenu_00's root: where the System page sits once the
    strip has slid to it (add_page's place, N_InOut_00's own place added), and the System page's size."""
    panes, _ = ui_pack.read_layout(pages_layout)
    p = next((p for p in panes if p.name == SYSTEM_PAGE), None)
    if p is None:
        raise PauseMenuError("the layout has no %s" % SYSTEM_PAGE)
    full = ui_pack.frame_offset(pages_layout, SYSTEM_PAGE, TAB_ROOT)
    off = ui_pack.frame_offset(pages_layout, SYSTEM_PAGE, PAGE_PARENT)
    _, y = struct.unpack_from("<ff", pages_layout, p.record + 0x1c)
    return (full[0] - off[0], full[1] + y), struct.unpack_from("<ff", pages_layout, p.record + 0x2c)


def add_minus_page(d, place, size):
    """MenuOverlay_00.buil with the - menu's page of ours: MINUS_PAGE, a part of the System page's own layout
    (PaSystemMenu_00, the same Options rows Pa_List_00..07 add_rows makes, in the game's System look) under the root,
    at `place` and `size` (shown_page: where and how big a shown + page is; both layouts fill the screen from the same
    centre), stored hidden. The - menu's pages of the game draw from other layouts (the Map page: MapMenu_00), so no
    page layout of the - menu has room for ours; the tab strip's layout shows over every - page, so the module shows
    ours there while one of our tabs is lit and hides the game's page under it. No code of the game binds it (the
    strip binds its Pa_Category parts by name): its rows are the module's alone."""
    panes, _ = ui_pack.read_layout(d)
    names = {p.name for p in panes}
    for name in (TAB_ROOT, MINUS_PAGE_SOURCE):
        if name not in names:
            raise PauseMenuError("the layout has no %s" % name)
    if MINUS_PAGE in names:
        raise PauseMenuError("the layout has %s already" % MINUS_PAGE)
    sx, sy = _xy(d, MINUS_PAGE_SOURCE)
    move = (place[0] - sx, place[1] - sy)   # (the copy's translate under the root is the source's plus the move)
    return ui_pack.clone_parts(d, [ui_pack.Copy(MINUS_PAGE_SOURCE, MINUS_PAGE, move, parent=TAB_ROOT, hidden=True,
                                                part_layout=SYSTEM_LAYOUT[:-len(".buil")], size=tuple(size))],
                               new_id=_new_id)


def add_page(d):
    """PlusMenu_00.buil with Longshot Settings' page: a copy of the System page (Pa_System_00) under
    N_InOut_00 where a shown page sits (the strip's slide taken off: x 0, the System page's y). (0.11.9, the
    owner: one home per thing: co-op's pages are the - menu's, so the + menu has no Multiplayer page any more.)"""
    panes, _ = ui_pack.read_layout(d)
    names = {p.name for p in panes}
    for name in (SYSTEM_PAGE, PAGE_PARENT):
        if name not in names:
            raise PauseMenuError("the layout has no %s" % name)
    if SETTINGS_PAGE in names:
        raise PauseMenuError("the layout has %s already" % SETTINGS_PAGE)
    off = ui_pack.frame_offset(d, SYSTEM_PAGE, PAGE_PARENT)
    move = (-_x(d, SYSTEM_PAGE), off[1])   # x: the page's place on the strip taken off (the strip itself sits at 0)
    return ui_pack.clone_parts(d, [ui_pack.Copy(SYSTEM_PAGE, SETTINGS_PAGE, move, parent=PAGE_PARENT, hidden=True)],
                               new_id=_new_id)


def add_rows(d):
    """PaSystemMenu_00.buil with the game browser's list: ROWS of the game's Options rows
    (ROW_LAYOUT) down the right half, ROW_STEP apart from ROW_TOP, stored hidden. Each is made in
    the place of the page's first button (Copy.part_layout); the page's own code binds its five
    Pa_Button_NN by name, so the rows are no buttons of the game's, and the module gives them their
    words (live texts) and their look."""
    panes, _ = ui_pack.read_layout(d)
    names = [p.name for p in panes]
    if ROW_SOURCE not in names:
        raise PauseMenuError("the layout has no %s" % ROW_SOURCE)
    if ROW % 0 in names:
        raise PauseMenuError("the layout has the list already")
    sx, sy = _xy(d, ROW_SOURCE)
    copies = [ui_pack.Copy(ROW_SOURCE, ROW % i, (ROW_X - sx, ROW_TOP + ROW_STEP * i - sy), hidden=True,
                           part_layout=ROW_LAYOUT, size=ROW_SIZE) for i in range(ROWS)]
    return ui_pack.clone_parts(d, copies, new_id=_new_id)


SETTINGS_BUTTON = "Pa_Button_05"    # Longshot Settings (System's only button of ours, 0.11.9: Multiplayer and Customize
                                    # Link are the - menu's Multiplayer tab now, one home per thing)
BUTTON_SOURCE = "Pa_Button_03"   # (the one of System's buttons without overrides or properties: clone_parts copies it)
BUTTON_AFTER = "Pa_Button_04"    # (Title Screen: ours row steps below it)


def add_system_buttons(d):
    """PaSystemMenu_00.buil with System's button of ours: SETTINGS_BUTTON (Longshot Settings), a
    copy of BUTTON_SOURCE one row step (Pa_Button_03 to Pa_Button_04, 188 in the game's layout)
    below BUTTON_AFTER, stored hidden. (0.11.9: co-op's pages are the - menu's tabs, one home per
    thing, so System has no Multiplayer or Customize Link button any more.)
    Every page made of this layout has it (System's, Longshot Settings' and the - page's): the module shows
    System's and places its lower buttons around them (Title Screen last), shows Longshot
    Settings' sixth row (its Debug category), and keeps the rest hidden. The game's System code
    binds its five buttons by name, so A on ours reaches only the module (its selectors send an
    unknown name to their default branch)."""
    panes, _ = ui_pack.read_layout(d)
    names = [p.name for p in panes]
    for name in (BUTTON_SOURCE, BUTTON_AFTER):
        if name not in names:
            raise PauseMenuError("the layout has no %s" % name)
    if SETTINGS_BUTTON in names:
        raise PauseMenuError("the layout has %s already" % SETTINGS_BUTTON)
    sx, sy = _xy(d, BUTTON_SOURCE)
    ax, ay = _xy(d, BUTTON_AFTER)
    step = ay - sy   # (down: y up)
    if step >= 0:
        raise PauseMenuError("%s is not below %s" % (BUTTON_AFTER, BUTTON_SOURCE))
    return ui_pack.clone_parts(d, [ui_pack.Copy(BUTTON_SOURCE, SETTINGS_BUTTON, (ax - sx, ay + step - sy), hidden=True)],
                               new_id=_new_id)


def _system_layout(d):
    """The System page's own layout with the browser's list and our two System buttons."""
    return add_system_buttons(add_rows(d))


def _xy(d, name):
    panes, _ = ui_pack.read_layout(d)
    for p in panes:
        if p.name == name:
            return struct.unpack_from("<ff", d, p.record + 0x1c)
    raise PauseMenuError("the layout has no %s" % name)


# ---------------------------------------------------------------------------------------- build

def build(romfs, out_romfs, dicts, table, write, recompress, log=print, strings=ui_pack.STRINGS, run=None):
    """The Common archive and every language's messages from the romfs folder, into out_romfs
    (write(out_romfs, rel, bytes)); their size table entries grown (`table`: rstb.Rstb). Returns
    the files written. `run`: the pack run's shared view (ui_pack.PackRun), the one the title rows'
    step (title_menu.build) used too, so each language archive keeps both steps' labels and grows
    once; a view of its own when None. All or nothing, as title_menu.build: a failure raises before
    anything is written, so the pack never holds the tab without its page and texts."""
    run = run or ui_pack.PackRun(romfs, table)
    files, growth = make(run, dicts, recompress, strings)
    run.check(growth)
    run.commit(files, growth)
    for rel, data in files.items():
        write(out_romfs, rel, data)
    log("pause menu: the - menu's tabs, System's Multiplayer and Longshot Settings and their pages (%s, %s, %s; "
        "%d languages)" % (OVERLAY_LAYOUT, PAGES_LAYOUT, SYSTEM_LAYOUT, len(files) - 1))
    return list(files)


def make(reader, dicts, recompress, strings=ui_pack.STRINGS):
    """({rel: compressed bytes}, [(rel, plain length read, plain length made, required)]) for the
    Common archive and every language's messages (ui_pack.PackRun's growth); raises PauseMenuError
    (or the reader's error) when any one cannot be made."""
    from . import sarc
    files, growth = {}, []
    raw = reader.read(COMMON_ARCHIVE)
    plain = dicts.decompress(raw)
    entries = sarc.read_entries(plain)
    # (the tab strip, MenuOverlay_00, gets the - menu's tabs and their page; our other pages open from System's buttons)
    pages = [data for name, data in entries if name == PAGES_LAYOUT]
    if not pages:
        raise PauseMenuError("%s has no %s" % (COMMON_ARCHIVE, PAGES_LAYOUT))
    place, size = shown_page(pages[0])
    overlay = lambda d: add_minus_page(add_tab(d), place, size)
    for layout, edit in ((OVERLAY_LAYOUT, overlay), (PAGES_LAYOUT, add_page), (SYSTEM_LAYOUT, _system_layout)):
        olds = [data for name, data in entries if name == layout]
        if not olds:
            raise PauseMenuError("%s has no %s" % (COMMON_ARCHIVE, layout))
        entries, new = ui_pack.edit_entries(entries, layout, edit)   # (every copy of the layout)
        growth.append((layout, len(olds[0]), len(new[olds[0]]), False))
    archive = sarc.write(entries, data_align=16)   # (as the game's own: 16)
    growth.append((COMMON_ARCHIVE, len(plain), len(archive), True))
    files[COMMON_ARCHIVE] = recompress(dicts, raw, archive)
    for rel in sorted(reader.glob(MALS_GLOB)):
        ours = ui_pack.load_strings(ui_pack.language_of(rel), strings)
        raw = reader.read(rel)
        plain = dicts.decompress(raw)
        msgs = sarc.read_entries(plain)
        have = {name for name, _ in msgs}
        if OVERLAY_MSG not in have or PAGES_MSG not in have:
            continue
        for f in (OVERLAY_MSG, PAGES_MSG):
            if not ours.get(f):
                raise PauseMenuError("the strings have no [%s]" % f)
        packed = sarc.write([(name, ui_pack.add_labels(data, ours[name], replace=True)
                              if name in (OVERLAY_MSG, PAGES_MSG) else data) for name, data in msgs], data_align=16)
        growth.append((rel, len(plain), len(packed), True))
        files[rel] = recompress(dicts, raw, packed)
    if len(files) < 2:
        raise PauseMenuError("no language has %s and %s" % (OVERLAY_MSG, PAGES_MSG))
    return files, growth
