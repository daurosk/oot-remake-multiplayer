"""Readable names for Character Select's unnamed characters (menu-customize-01113, audit item 14).

OoT Character Select (snaildev99, MIT) names its game characters (Saria, Rauru, ...) but gives the game's unnamed
townsfolk their model ids: "Gerudo_GEN_002", "Hylia_MN_005", "Kokiri_WC_001". In his picker those ids are the labels
under the icons (LayoutMsg/CharacterMenu_00.msbt T_Slot_NNN) and the big name beside the character (T_CellName_NNN);
an id has no space, so his label pane cannot wrap it and the long ones run into their neighbours (seen 2026-11-23,
npc-custom-solo page 2: "Gerudo_GEN_002Gerudo_GEN_003"). The same ids are the names of co-op's Settings rows
"Character by age" (his characters.json; hookshot/src/coop_charsel_names.h, the same rule in C++).

The rule (one for both places, so a character has one name everywhere): a name of the form <Race>_<CODE>_<digits>
(an optional _<Word> after it) is a model id; any other name is his and stays. An id's kind is its race and its code's
letters (M a man, W a woman, C a child, O old, S a soldier; checked against his icons on pages 2-6):
  Hylia M.. -> Townsman, W.. -> Townswoman, MO -> Old man, WO -> Old woman, MS -> Soldier
  Kokiri MC -> Kokiri boy, WC -> Kokiri girl
  Gerudo, Goron, Zora, any other race -> the race's word (Hylia -> Hylian)
  a trailing word -> "<kind> <word in lower case>" (Gerudo_GEN_011_Leader -> Gerudo leader)
Kinds with more than one character are numbered 1, 2, ... by their model ids in order (the code, then the id's number,
then its word: Goron_GON_001, _002, _006), never by where they stand in a list: his picker's slots and his
characters.json are two lists (review-01112-h), and a release that orders one of them otherwise still gives each
character the same name in both places. A kind of one has no number. Every name fits his label (at most 13 letters,
"Townswoman 14"); a longer one is broken at a space onto a second line, as his own long names ("Ingo (adult\\nera)").
"""
import re

# <Race>_<CODE>_<digits>, then an optional _<Word>
MODEL_ID = re.compile(r"^([A-Za-z]+)_([A-Z]+)_([0-9]{1,18})(?:_([A-Za-z]+))?$")   # (ASCII digits, 18 at most: the C++ side)
RACE_WORD = {"Hylia": "Hylian"}
LABEL_ROOM = 13   # (letters on one line of his label: "Phantom Ganon", "Darunia's son" fit, "Goron_GON_006" does not)
PICKER_MSG = "LayoutMsg/CharacterMenu_00.msbt"
SLOT = re.compile(r"^T_(Slot|CellName)_(\d+)$")


def _id(name):
    """The model id's parts (race, code, digits, word or None), or None for a name of his own."""
    m = MODEL_ID.match(name.replace("\n", "_"))   # (his wrapped label: "Gerudo_GEN\n011_Leader", the break for a "_")
    return m.groups() if m else None


def kind_of(name):
    """The kind (the readable name before its number) of a model id, or None for a name of his own."""
    parts = _id(name)
    if parts is None:
        return None
    race, code, _, word = parts
    man, woman = code.startswith("M"), code.startswith("W")
    if race == "Hylia" and (man or woman):
        if code[1:2] == "O":
            kind = "Old man" if man else "Old woman"
        elif code == "MS":
            kind = "Soldier"
        else:
            kind = "Townsman" if man else "Townswoman"
    elif race == "Kokiri" and code in ("MC", "WC"):
        kind = "Kokiri boy" if man else "Kokiri girl"
    else:
        kind = RACE_WORD.get(race, race)
    return kind + " " + word.lower() if word else kind


def _order(parts):
    """A model id's place among its kind's: its code, the value of its digits, its word ("" none)."""
    _, code, digits, word = parts
    return (code, int(digits), word or "")


def readable(names):
    """Each name of the list (in its order) as shown: model ids as their kind (numbered by their ids' order when the
    kind has more than one, whatever the list's order), the others unchanged."""
    ids = [_id(n) for n in names]
    kinds = [kind_of(n) for n in names]
    of_kind = {}
    for i, k in enumerate(kinds):
        if k is not None:
            of_kind.setdefault(k, []).append(i)
    number = {}
    for members in of_kind.values():
        for rank, i in enumerate(sorted(members, key=lambda j: _order(ids[j])), 1):
            number[i] = rank if len(members) > 1 else None
    return [n if kinds[i] is None else kinds[i] if number[i] is None else "%s %d" % (kinds[i], number[i])
            for i, n in enumerate(names)]


def label_lines(text, room=LABEL_ROOM):
    """A name as his label under an icon shows it: one line when it fits, else broken at the last space that fits
    (a name with no space stays one line)."""
    if len(text) <= room or " " not in text:
        return text
    cut = text.rfind(" ", 0, room + 1)
    if cut <= 0:
        cut = text.find(" ")
    return text[:cut] + "\n" + text[cut + 1:]


def picker_labels(texts):
    """{label: new text} for his picker's message file, given its {label: text}: every T_Slot_NNN / T_CellName_NNN
    whose name is a model id (each character's name read from its T_CellName, else its T_Slot); {} when none is."""
    slots = {}
    for label, text in texts.items():
        m = SLOT.match(label)
        if m:
            slots.setdefault(int(m.group(2)), {})[m.group(1)] = text
    order = sorted(slots)
    names = [slots[i].get("CellName", slots[i].get("Slot", "")) for i in order]
    out = {}
    for i, old, new in zip(order, names, readable(names)):
        if new == old:
            continue
        if "CellName" in slots[i]:
            out["T_CellName_%03d" % i] = new
        if "Slot" in slots[i]:
            out["T_Slot_%03d" % i] = label_lines(new)
    return out


def tidy_picker(data):
    """His picker's message file (MSBT bytes) with readable names; the same bytes when it has no model id."""
    from . import msbt, ui_pack
    m = msbt.read(data)
    texts = {}
    for label, items in m.rows():
        if SLOT.match(label):
            texts[label] = "".join(i for i in items if isinstance(i, str))
    new = picker_labels(texts)
    return ui_pack.add_labels(data, new, replace=True) if new else data
