"""The game's own item names the in-game tests compare with the bridge state (docs/playtest.md "State") and send in
`wear` / `give` lines: ONE table, imported by every test and by the fake lane (fakegame.py), never copied.

The bridge's `equips` member is the value NAME of the game's Enum flag PlayerStatus.Equip<slot> (hookshot/src/
coop_playtest.cpp StateMembers -> CoopTestInput::FlagText, lichii/hook/coop_testinput.cpp WearFlag), which is the item's
own name, the same one its IsGet.<name> / PouchData.<name>.StockNum flags carry. Not a short form: 'KokiriSword' was
never a value (gate-hilt-309d7180 SETUP-FAIL 2026-11-27 02:59: 'A holds Weapon_Sword_Kokiri'). A name here must be an
item of coop/host/src/game_items.cpp (tests/test_ingame_game_names.py checks it).
"""

KOKIRI_SWORD = "Weapon_Sword_Kokiri"         # gate-hilt-309d7180 state equips.weapon (offline-queue END 02:59 UTC 11-27)
SKULL_MASK = "Armor_Child_Mask_Skull"        # hs-masks.A.txt give / wear lines (test_rows.test_masks_on_puppet)
GOLD_GAUNTLETS = "Armor_Adult_Gauntlet_Gold"  # hs-gear-age.A.txt / hs-gear-adult.A.txt wear lines
GORON_TUNIC = "Armor_Adult_Goron"
HOVER_BOOTS = "Armor_Adult_HoverBoot"

ALL = (KOKIRI_SWORD, SKULL_MASK, GOLD_GAUNTLETS, GORON_TUNIC, HOVER_BOOTS)
