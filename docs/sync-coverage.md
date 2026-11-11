# Sync coverage (test7)

Caleb (2026-11-11): everything one player does to the world is the same in every game, live, and nothing one player does may stop another from playing. Each sync branch keeps its rows here.

## Softlock

Branch `softlock-audit`: every way co-op could leave a player unable to go on, and whether the code on main handles it. Read from the code, the docs and the romfs (`Product.111` dump: `Logic/*.ainb`, `AI/*.ainb`, `Event/EventFlow`, `Banc/*`, `GameData/GameDataList`); nothing here was run.

How the game keeps progress, in short: items are shared by rule ([items.md](items.md)); a dungeon's rooms are named saved flags (reset type 8) and placement keys (reset type 24), shared by [dungeon.md](dungeon.md)'s lists; chests and one-time pickups everywhere are keys ([items.md](items.md), "Chests"). **Overworld story flags are not shared**: each game plays its own story scenes, and the shared items let a player who is behind skip what the items stand for.

Status: **Fixed** (on main, or in this branch), **OK** (the rules already cover it; the reason is given), **P1** (can block a player; to do), **P2** (can annoy or cost time, no hard block), **Check** (needs a run to know).

### Dungeons

| # | Case | Status | Why / what to do |
|---|---|---|---|
| D1 | Room state the dungeon list missed: 20 Jabu-Jabu flags spelt `Insidejabujabu_` (rooms cleared, the Room 7 switches and gates, the Room 12 chest gimmick), `Firetemple_`/`Foresttemple_SongOfTimeBlock_A_01_Disappearance`, the enemies-beaten and torch shutters `Gimmick_DefeatEnemy_Open_{DodongoCave,FireTemple,ForestTemple}_*` and `Gimmick_Ignition_Open_WaterTemple_*`, `Gimmick_DeleteFire_FireTemple`, Jabu-Jabu's water `Gimmick_Insidejabujabu_*MoveWater*`, the Spirit Temple stone tablets' played events | **Fixed (this branch)** | The scan took names that start with the dungeon's exact spelling. Enemies are shared and die once in every game, so a room whose shutter opens on "all enemies beaten" never opened for the second player: their enemies were already gone. 37 flags added to `coop_dungeon.cpp`. Left out: `Insidejabujabu_Room7_Switch*_InsideArea` (where a player stands) and `…_OnlyLinkOrLuto` (Ruto). |
| D2 | Small key spent twice for one door: both players open the same locked door before the other's unlock flag arrives | **P1** | `SmallKeyNum` is a shared count (delta rule); each game sends −1. The count ends one short, and every key in a dungeon is needed, so the last door cannot open. Nothing on main catches it. Fix idea: the host remembers each `…_UnLock`/`…_LockOpne`/`…_Unlocked`/`…_DoorOpen` flag's 0→1 with the player and time; a second 0→1 of the same flag from another player within ~3 s, plus a −1 of that dungeon's `SmallKeyNum` from that player in the same window, gets +1 back. Needs the lock-flag list per dungeon (D3). |
| D3 | A locked door opened in one game stays locked in the other while the key is spent for both | **Check, then P1** | The 98 `Obj_DoorChainLock_*` placements have no key and no flag in their placement. Saved lock flags found by name: Forest 6 (`…_LockOpne`, `…_LockOpen`), Spirit 21 (`…_Unlocked*`), Ice Cavern 4, Well 1, Ganon's Castle 1, Jabu 1, Fire `…_DoorOpen*` 3: all shared. Fire (16 locks), Water (9), Shadow (10), Gerudo Training Ground (18) have few or none found, so their door state may live elsewhere. If A's door opens in A only, B is one key short for it: a hard block. In-game check 2 tells. |
| D4 | Key chest opened by both at once (the near rule leaves a chest loaded beside a player) | **Check** | Two +1 for one key: more keys, never fewer, so no block. deku-softlock check 5. |
| D5 | Loose Shadow Temple small key (no key of its own) taken in both games | **OK** | Two +1: a spare key, no block. |
| D6 | Enemy-counter rooms | **OK** | The 51 `Obj_EnemyCounter_*` keys and the `…_Clear`/`…_Cleared` flags are shared; D1 adds the misspelt and `Gimmick_` ones. |
| D7 | One-way rides (Shadow Temple boat, Forest Temple elevator and room spins) | **OK** | Left out of the list on purpose: each player rides their own. |
| D8 | Water Temple water level changed by one player while the other is elsewhere | **P2** | Live, last write wins. A player can find the water at another height and must go to a level switch. No hard block known (the three switches are reachable at each level in the original), but it can cost time. Players should agree before playing Zelda's Lullaby at a switch. |
| D9 | Boss beaten in one game; the other enters the boss room later | **Check** | The boss flags and heart container key are shared, and the boss is one for all ([dungeons-boss.md](dungeons-boss.md), #109). The boss room's only way out is the blue warp: check that it is there for a player who comes in after the boss died. boss-walkout (chunk 5) owns this. |
| D10 | Push blocks needed to go on (Forest Temple, Kakariko well, Spirit Temple) | **Check** | `WORLD_MOVE`/`WORLD_PLACE` move them live (unit tests only). [world.md](world.md) check 4: B can push the block afterwards. |
| D11 | Things removed from the other game when taken | **OK** | `CoopObjects::RemovedWhenTaken` takes only things that go away (chests, pickups, webs, red ice, cracked walls, broken rocks, the Deku Tree's falling floors); counters, hammer pillars and the Shadow Temple's falling floor stay. |
| D12 | Gerudo Fortress: the cell keys (`IsGet_SmallKey_GerudoFortress.SmallKey01`–`04`) and cell doors are per game; `DungeonInfo.GerudoFortress.SmallKeyNum` is shared | **P2** | Each game has 4 keys and 4 cells, and every pickup and spend counts once for both, so the sums match when both free all four. Halfway, one player can spend a key the other picked up. Not a block: the membership card (shared) is what the rest of the game needs. Sharing the key flags alone would make it worse (4 keys for 8 cells). |
| D13 | Ammo for items shared without it (sticks, seeds, nuts, bombs, arrows) | **Fixed (main)** | #128 / #138: capacity with the item, the mend after a load. |

### Items, trades and bottles

| # | Case | Status | Why / what to do |
|---|---|---|---|
| I1 | Ruto's letter | **OK** | `IsGet.Item_Bottle_Letter` is not shared, so the letter is still in the other game's lake (`Obj_Bottle_Letter`, no key). Bottle contents are per player, so each player takes their own letter for King Zora (whose state is per game). Side effect: the second letter's bottle is shared as a new bottle (one extra empty bottle for everyone); harmless. |
| I2 | Fish, Blue Fire, bugs and other bottle contents needed to go on (Jabu-Jabu, Zora's Domain ice, the Ice Cavern) | **OK** | Per player, and every source can be used again. |
| I3 | Trade sequence items (mirror rule): one player hands an item over and it is gone for everyone | **P2** | Optional quests (Biggoron's Sword, masks). A player halfway through loses their step. Later: share the trade step too, or make the trade items per player. |
| I4 | Zelda's Letter, Ocarina of Time, songs, stones and medallions | **OK** | Shared items let the other player do the per-game event themselves (Kakariko gate, Temple of Time door) or skip it. |
| I5 | Deku Shield bought once for everyone (#130) | **P2** | Shared as owned. A shield that burns can be bought again only if the shop sells it again in that game (open issue #130). |
| I6 | Keys and dungeon items with a join at another save point | **P2** | A late joiner takes the host's totals. A key the joiner's save had in hand but the host's did not is lost to the joiner; the chest it came from stays open in the joiner's game but closed in the host's, so the host player can take it again for both. Advice: join with saves from the same point. |

### Story, time travel, saves

| # | Case | Status | Why / what to do |
|---|---|---|---|
| S1 | Story scenes (Deku Tree, Zelda, Darunia, King Zora, Gerudo carpenters, sages) | **OK** | Not shared on purpose (`Dm_`, `Demo_`, `SageOf`, talks). Each game plays its own when its player gets there; the shared items carry what they give. |
| S2 | One player adult, one child | **OK** | Shared flags belong to scenes; a child cannot be in an adult-only room. Small keys and dungeon flags are the same for both ages, as in one save. The Master Sword and time travel are per game. |
| S3 | Ganon's Castle escape and the last fights | **Check** | The escape flags (`EscapeFromCrumblingGanonCastle_*`) are per game; Ganondorf is one boss for all ([dungeons-boss.md](dungeons-boss.md)). Check what a second player inside the castle sees when the first beats Ganondorf. |
| S4 | Saves loaded at different points | **P2** | The joiner gets the host's table (`FLAG_SNAPSHOT`); its own 1s that the host has at 0 stay in its game and do not cross until they change. See I6. |
| S5 | Overworld entrances that need a per-game action (Kakariko well drained, the graveyard's Shadow Temple entrance `GraveYard_Field_ShadowTemple_ShutterOpen`, the Gerudo Valley bridge) | **OK** | Each player can do them: the songs, Din's Fire and the card are shared. |

### In-game checks (integrator)

1. Jabu-Jabu (D1): A beats a room's enemies (Room 1 or Room 5). B, in the same room or entering after: the shutter is open. B's log (`LICHII_COOP_DEBUG_FLAGS=1`): `bool 0x2ea0d05d 0 -> 1` (`Insidejabujabu_Room1_Cleared`) or `0x695a1137` (Room 5). Dodongo's Cavern `Gimmick_DefeatEnemy_Open_DodongoCave_01` (`0xdba3c3ef`): the same.
2. Locked door (D3): Water Temple or Fire Temple, both with one key. A opens a locked door. On B's screen the door opens (or is open when B comes), and B's key count drops by 1 only. If B's door stays locked: report the room; that dungeon's lock flag must be found (P1).
3. Same door at once (D2): both stand at a locked door with one key; both press open within a second. Read `DungeonInfo.<dungeon>.SmallKeyNum` in both logs: it must drop by 1. If by 2: D2 is real (P1).
4. Boss room late (D9): A beats Gohma; B walks into the boss room afterwards: the blue warp is there and takes B out.
