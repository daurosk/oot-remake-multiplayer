"""Queen Gohma alive, the quest at her room (rows 137/138 hs-boss-away-reset; the old form was the 'gohma-alive'
profile of offline/boss-away-0115-probe.ps1 Set-Saves). Hashes and names: lichii/hook/coop_boss_beaten.inc
(kBools0 and the Queen Gohma row) and lichii/hook/coop_quest.cpp:258 (Get_StoneOfForest GoToBossRoom)."""
FLAGS = [
    "enum:0x7d776b61=0xb5df43cb",   # Get_StoneOfForest = GoToBossRoom (her body is in the room)
    "bool:0x6b093b51=0",            # Gohma_DefeatBoss
    "bool:0x3419ccb3=0",            # HavePlayedEvent.InsideTheDekuTree_BossDie
    "bool:0x00c0a2f6=0",            # HavePlayedEvent.Dm_Deku_BossIn
    "bool:0x5882768c=0",            # HavePlayedEvent.Dm_Deku_BossOut
]
# What a game that loaded this save shows (testsave.prove, in the test's set-up): child Link (the lane baseline is the
# child save: bt_plugin.py DEFAULT_BOOT comment "child save"; the old gohma-alive run's B walked her room as a child).
# The FLAGS above are read back from the game too.
EXPECT = {"age": "child"}
