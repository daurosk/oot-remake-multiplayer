"""No ocarina in either game (issue 190 hs-ocarina-given: "B never had the ocarina"; the old row's B-only prep was
`save_flags.py no-ocarina`, offline/big-run-0115.ps1 profile b-no-ocarina). The same flags as save_flags.NO_OCARINA
(tools/autotest/save_flags.py:279-287) and its EquipOcarina String64 (save_flags.EQUIP_OCARINA, :289, emptied as
no_ocarina() does). Both games: test_songs.test_ocarina_given_saria gives A the Fairy Ocarina by the bridge, so B gets
it only through the item sync, the issue's case. Hashes: coop/host/include/coop/host/game_items.hpp namespace ocarina.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import save_flags as SF   # noqa: E402  (ONE source: save_flags.NO_OCARINA / EQUIP_OCARINA, pinned to game_items.hpp
#                                       by tests/test_ingame_songs.py)

FLAGS = ["%s:%#010x=%d" % e for e in SF.NO_OCARINA]   # both ocarinas' counts, IsGet, slot and got Bools = 0
CLEAR_TEXT = [SF.EQUIP_OCARINA]                         # PlayerStatus.EquipOcarina emptied
# every flag above and EquipOcarina are in the kit save (coop-kit/ls/lane1-A/autotest-save/slot_00..05 read with
# save_flags.py on 2026-11-26: Fairy count 1, its four Bools 1, the Ocarina of Time's three 0, EquipOcarina there)
EXPECT = {"age": "child"}  # the kit save's Link (live-kakariko-child boot)
