"""No ocarina in either game (issue 190 hs-ocarina-given: "B never had the ocarina"; the old row's B-only prep was
`save_flags.py no-ocarina`, offline/big-run-0115.ps1 profile b-no-ocarina). The same flags as save_flags.NO_OCARINA
(tools/autotest/save_flags.py:279-287) and its EquipOcarina String64 (save_flags.EQUIP_OCARINA, :289, emptied as
no_ocarina() does). Both games: test_songs.test_ocarina_given_saria gives A the Fairy Ocarina by the bridge, so B gets
it only through the item sync, the issue's case. Hashes: coop/host/include/coop/host/game_items.hpp namespace ocarina.
"""
FLAGS = [
    "int:0xb7df60f1=0",    # PouchData.Item_Ocarina_Fairy.StockNum (ocarina::kFairy)
    "int:0x54e2a3d1=0",    # PouchData.Item_Ocarina_Time.StockNum (ocarina::kTime)
    "bool:0x42bd296f=0",   # IsGet.Item_Ocarina_Fairy (ocarina::kIsGetFairy)
    "bool:0x1da2c511=0",   # IsGet.Item_Ocarina_Time (ocarina::kIsGetTime)
    "bool:0xaacdef1f=0",   # 0x541947d0.Ocarina (ocarina::kSlotA)
    "bool:0xda41879d=0",   # 0x57f52eb8.Ocarina (ocarina::kSlotB)
    "bool:0x137df1ac=0",   # 0x41f3e410.Item_Ocarina_Fairy (ocarina::kGotFairy)
    "bool:0xac6a85e3=0",   # 0x41f3e410.Item_Ocarina_Time (ocarina::kGotTime)
]
CLEAR_TEXT = [0xb72cd3fc]  # PlayerStatus.EquipOcarina (ocarina::kEquip; save_flags.EQUIP_OCARINA)
# every flag above and EquipOcarina are in the kit save (coop-kit/ls/lane1-A/autotest-save/slot_00..05 read with
# save_flags.py on 2026-11-26: Fairy count 1, its four Bools 1, the Ocarina of Time's three 0, EquipOcarina there)
EXPECT = {"age": "child"}  # the kit save's Link (live-kakariko-child boot)
