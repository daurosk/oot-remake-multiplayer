# The game's flag store

Where Ocarina of Time keeps its world flags, and how `coop_host` reads and writes them (`coop/host/include/coop/host/game_flags.hpp`). Game: main module of build e0f3ccfaf50065ae (1.1.1). Guest address = main base (0x7100000000 in the runs so far) + offset.

Sources: the integration project's `actor-map.md` (section 4), `player-state-map.md` and `save-format.md`, plus a static read of `main` for this page. **Checked** means read from the instructions. **Not run** means no live game confirmed it. Nothing on this page is confirmed in a run yet; see "Not done".

## Summary

- One object, `gmd::GameDataMgr`, holds every flag. Its pointer is at **main + 0x4e842a8**. Checked: the 272 code references load it through GOT slot 0x4c7e180.
- Named flags are keyed by the **Murmur3 x86 32-bit hash, seed 0** of their name (`player-state-map.md`). Each type has its own table.
- Chests, pots, enemies and other placed actors are **64-bit-key flags** (`Bool64bitKey`), keyed by the placement's 64-bit `Hash` in the Banc files, not by a name.
- Setters do not write the value. They queue the write, and the GameData module applies the queue on its next update.

## Layout

`M` is the manager. Type `t` (the save type ids of `save-format.md`) has its table at `M + 0x10 + 0x60 × t`:

| Offset in the table | Field |
|---|---|
| +0x00 | u32 entry count |
| +0x08 | Pointer to the entries |
| +0x10 | Pointer to the hash slots: 8 bytes each, {u32 hash, u32 entry index}, hash 0 = empty |
| +0x1c | u32 slot count |
| +0x20 | u32 queue size (slots) |
| +0x28 | Pointer to the queue |
| +0x30 | u32 queue word |

A lookup starts at `hash % slot count` and steps forward by one, wrapping, until it finds the hash or an empty slot. An entry index of the entry count or more is taken as 0 by the game. Checked at 0x833920.

| Type | Table | Entry size | Live value | Queue slot |
|---|---|---|---|---|
| Bool (0) | M + 0x10 | 0x18 | u8 at +0x16; saved copy at +0x17 | 8 bytes: u8 value at +0, u32 hash at +4 |
| BoolArray (1) | M + 0x70 | 0x40, has a vtable | bits at `*(+0x28)` in u32 words, lowest bit first; element count u32 at +0x20 | not read |
| Int (2) | M + 0xd0 | 0x28 | u32 at +0x1c | 12 bytes: u32 value at +0, u32 hash at +4 |
| UInt (20) | M + 0x790 | 0x28 | u32 at +0x1c | 12 bytes: u32 value at +0, u32 hash at +4 |

All checked in the getters and setters below. The BoolArray element count at +0x20 is from a bounds test in the applier, so its meaning is inferred.

64-bit keys are separate, at **M + 0xc58**:

| Offset | Field |
|---|---|
| M + 0xc58 | u32 record count |
| M + 0xc60 | Pointer to the records, **sorted by key**, 16 bytes each: u64 key +0, u32 save-file mask +8, u8 live value +0xc, s8 file index +0xd, u8 flags +0xe (bit 1 is the saved copy) |
| M + 0xc68 | u32 queue size |
| M + 0xc70 | Pointer to the queue, 8 bytes a slot: u8 value +0, u32 **record index** +4, empty = 0xffffffff |
| M + 0xc78 | u32 queue word |

Checked at 0x838960 (binary search, value at +0xc) and 0xba24b0 (applier). The save-file mask and file index are as `save-format.md` reads them.

### The queue word

Bits 0–19 count slots taken, bits 20–27 count slots finished (they go back to 0 after 255), bits 28–31 are a round number that the applier moves on (wrapping after 14) each time it empties the queue. A setter takes a slot with compare-and-swap on the word, writes the slot, then counts it finished with a second compare-and-swap. When the round changed between the two, or the queue was full, it calls 0x34fc280 (not read). Checked at 0x682b20.

## Functions

| Address | What | Status |
|---|---|---|
| 0x833920, 0x20d9c0 | Bool get (M, out, hash). Bool table, then BoolArray element 0 | Checked |
| 0x20fd90 | Bool get that first looks through the queue, newest first | Checked |
| 0x682b20 | Bool set (M, value, hash): queues | Checked |
| 0x837070 | Int get | Checked |
| 0x163b70 | Int set: queues | Checked |
| 0x57a5e0 | UInt get: the Int getter with every table offset 0x6c0 further on | Checked (issue 125) |
| 0x57b030 (a second copy near 0x57bdd8) | UInt set: queues, as the Int setter, at +0x7b0 / +0x7b8 / +0x7c0 | Checked (issue 125) |
| 0x838960, 0x3761680 | 64-bit key get (M, out, key) | Checked |
| 0x3761720 | 64-bit key set (M, value, key): finds the record, then 0xf071c0 queues it | Checked |
| 0xb99980 | Applies the queues (M). Called from 0x3b500a0, in the GameData module | Checked for Bool and 64-bit keys |
| 0xba24b0 | Applies the 64-bit key queue, from 0xb99980 | Checked |
| 0x1739e0 | Flag name hash (string, seed) | `player-state-map.md` |

What the applier does for a Bool slot: when the value differs, it stores the live byte (+0x16), the saved copy (+0x17, unless entry +0x10 bit 31 is set or M + 0xd0c is not 0), marks the entry changed (+0xc = 1, +0x14 = 0), ORs the entry's file mask (+8) into M + 0x60, and adds the hash to a change ring at M + 0x48. It stops at the first empty slot, clears the slots it walked, and moves the round on. For a 64-bit key slot it stores +0xc, the saved bit in +0xe (when the file index is not negative), and ORs the mask into M + 0xca8. Checked.

So writing the live byte directly would skip the saved copy and the save-file mask: the change would be lost on save. `game_flags` queues instead, exactly as the setters do, and its reads look through the queue first, as 0x20fd90 does, so a value read straight after a write is the new one.

## Which flags are which

| Kind | Where | Evidence |
|---|---|---|
| Chest opened | 64-bit key = the chest's placement `Hash` | Of the Banc placements whose `Hash` is a Bool64bitKey, 235 are `Obj_TBox_*`. A chest's blackboard has `IsOpened`. Checked statically, **not run** |
| Pots, rupees, Gold Skulltula tokens, rocks, enemies | 64-bit keys, same way | The other placements in the key list (`Obj_Pot` 511, `Obj_Rupee` 170, `Obj_RupeeToken` 85, enemies). Checked statically |
| Events seen, songs, dungeon state | Named Bool and Int flags | Names hash into the Bool table (`player-state-map.md`). Which name is which was not mapped |

`ResetTypeValue` in the flag list tells persistent keys from ones reset on a scene change: chests are 24 (0x18), most pots and enemies 88 or 280. Which value means what is inferred, not read.

## What this means for shared progress

- Chests, small-key chests and heart pieces in the world are 64-bit keys. The protocol's flag key has a 64-bit kind for them (`protocol.md`, "Shared flags"), and `GameFlagStore` gives `FlagWatcher` the named Bool and Int flags and the 64-bit keys (table 0, through `get_key64` and `set_key64`).
- The emulator must implement `WritableGameMemory`: a write and a 32-bit compare-and-swap that is atomic against the game's threads (or run them while the guest is paused).
- The list of shared flags should take only persistent keys (reset type 24), or a pot broken by one player would break for everyone.

## Live check

Run in the patched emulator (`lichii/hook/coop_flags.cpp`, settings in [lichii.md](lichii.md)) with a child save in the Kakariko graveyard, two instances and a local server, 2026-11-08.

- The tables read live: `main at 0x80d68000` (it moves per run), `bool table, 11292 entries`, `int table, 1259 entries`, `key64 table, 11657 records`.
- **The flower-grave chest is a named Bool, not a 64-bit key.** Opening it changed one flag that stays: `bool 0x39e4b7ec (entry 9361) 0 -> 1` (with the rupee Ints). No 64-bit key changed. So at least some chests are Bool flags; the "Chest opened" row above holds for the Banc chests, not for every chest.
- The ReDead under that grave is a 64-bit key, `0xf9b880ab096a9cd6` (record 11382): 0 -> 1 when killed, back to 0 on the next scene load, like the other enemy and pot keys (a scene load flips about 40 keys at once).
- `set_key64` and `set_bool` write through the queue and the game takes them: `set_key64(0xf9b880ab096a9cd6, 1) queued (before 0, now 1)`, then the table scan shows `0 -> 1`. A write made before the save has loaded is lost: loading the save puts the old value back. So a build must not write flags until the save is in (`LICHII_COOP_SET_AFTER` delays the debug writes).
- **A flag crosses from A to B.** Both instances shared `0x39e4b7ec`. Opening the chest in A logged `0 -> 1` in A, and B logged `0 -> 1` within the same second (after allowing for B starting 3.5 s later); in B the chest was already open when its player got there. The same worked earlier with a debug write (`LICHII_COOP_SET_BOOL`) on another Bool: moving A's write later moved B's change with it.

Not checked live: an Int or 64-bit key crossing, B to A, a late joiner getting the host's table.

## Not done

1. Float, Enum and the other types (UInt is read, above): their tables follow the same pattern (M + 0x10 + 0x60 × t) but were not read.
2. 0x34fc280, called when a write is late or the queue is full.
3. BoolArray writes, Struct (+0xc20) and BoolExp (+0xc48).
