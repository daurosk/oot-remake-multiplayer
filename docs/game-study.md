# Studying the game: the study tools

Three tools for mapping the game's code and text (issue #264, the native UI work): who calls a function, which class a
vtable belongs to, and what a message label says. They read the player's own files on the player's PC. Nothing they
read or print goes into the repository; only names, addresses and notes go into docs.

All three are plain Python 3.10 or newer, with `zstandard` only for the message archives. Run them with `python -I`.

What they found about the game's UI is in [game-ui.md](game-ui.md); the names go in `tools/game_scan/game_names.tsv`.

## The game's main

The tools read main as an NSO, plain or compressed. A Longshot game folder holds a plain one:
`<game>/.longshot_exefs/main`. Supported build: 1.1.1, main build id `e0f3ccfaf50065ae` ([game-builds.md](game-builds.md)).

Addresses are the ones co-op's code and docs use: Longshot loads rtld at `0x80000000` and main right after it at
**`0x80020000`**. So `main+0x3406710` is `0x83426710`, and `f_8088ffc0` is main `+0x86ffc0`. Every tool takes an
address as `0x84c106e8`, `84c106e8`, `f_8088ffc0` or `main+0x3406710`, and `--base` for another load address.

`tools/game_scan/nso.py` is the reader they share: the three segments placed, the relocations applied (RELR and RELA),
and the imported symbols noted by the slot that holds them.

## vtables.py: the classes and their names

```
python -I tools/game_scan/vtables.py <main> [--name TEXT] [--vtable ADDR] [--all]
```

Main has C++ type information only for a few dozen standard library classes. The game names its own classes another
way: thousands of tiny functions return a class's name (`ADRP x0`, `ADD x0`, `RET`), and most classes keep one in their
vtable. The tool:

1. finds the vtables (an offset-to-top word, a type info word, then pointers into code or pure virtual imports; a
   single code pointer after two zeros counts only with type info, since it is as often a global);
2. names each from its type info, else from the name functions in its slots;
3. follows the functions of a small named vtable (a factory: up to 8 slots) two calls deep to the vtables they store
   at offset 0 of the object they make, and gives the largest one the factory's name. This is how the screens get
   their names: `UITitleScreen` is `0x84c106e8`, named through its factory `0x84c10a50`.

On build 1.1.1: about 17,800 vtables, about 8,600 named, 58 `UI...Screen` classes with their own vtable named.

Output: one line per named vtable (address, slot count, name, how it was named); `--vtable ADDR` lists one vtable's
slots with the name functions and imports in them (an address inside a vtable works too). It is a heuristic: a name
from a name function is the class that returns it, which is usually but not always the vtable's own class.

## callers.py: who uses an address

```
python -I tools/game_scan/callers.py <main> ADDR [ADDR ...] [--size N] [--functions FILE]
```

For each address (or range with `--size`): calls and jumps (`BL`, `B`); an `ADRP` followed within 8 instructions by an
`ADD`, a load or a store that reaches it (`address`, `read`, `write`); `ADR` and literal loads; loads of an 8-byte slot
that holds it (`via pointer`: a GOT entry or a global pointer); and slots in ro or data that hold it (`pointer`, with
the vtable and slot index when the slot is in one). Each code hit names the function it is in: with `--functions`
from Longshot's `work/sa/gen/functions.txt`, else the nearest call target or vtable entry before it ("near").
A scan of the whole main takes a few seconds.

## msbt.py: the message files

```
python -I tools/puppet_pack/msbt.py <file.msbt | <language>.Product.110.sarc.zs> [--file NAME] [--label NAME] [--list]
```

The game's texts are in one archive per language, `romfs/Mals/<language>.Product.110.sarc.zs` (zstd with the game's
dictionaries; `--romfs` defaults to the folder above `Mals`). `--list` lists the `.msbt` files in it; with `--file`
the tool prints one `label<TAB>text` line per label, with control tags written as `{group.type}` or
`{group.type:hex}`. The format is in the tool's docstring. On build 1.1.1 every message file is UTF-8 with only the
`LBL1` and `TXT2` sections.

## Tests

`tools/game_scan/tests/test_game_scan.py` and `tools/puppet_pack/tests/test_msbt.py` build small made-up files in the
test code (an NSO with RELR and RELA relocations, vtables, a factory and a name function; MSBT files in each encoding
and byte order); no game file is needed. CI runs both in the python-tools job.
