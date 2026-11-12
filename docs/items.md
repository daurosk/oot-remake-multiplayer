# Items and inventory

Where Ocarina of Time keeps the player's items, and which of them co-op shares. The values are flags in the game's flag store ([flags.md](flags.md)): every name below is a flag name, its hash is the Murmur3 x86 32-bit hash with seed 0, and `GameFlags` reads and writes it.

**Status.** Every name and hash on this page is checked: each one was hashed and found in `GameData/GameDataList.Product.110.byml.zs` with the type shown, by walking the list's `Struct` entries (a struct member is `{member name hash, child flag hash}`, and child = `Parent.Member`). Member names came from strings in `main` and the RomFS. What is confirmed in a run is in [Live check](#live-check-2026-11-09); the rest is not, including most of what each value means (`StockNum` 1 = owned is inferred from the name and from `PouchData.Item_Token_Gold.StockNum` being read as a count by the `Dm_SY_Get` event flow).

## Caleb's rule (2026-11-09)

Key items are given to everyone. Rupees and non-key items stay with each player. Bottles are given to everyone, but what is in a bottle stays with each player.

## Where things are

| What | Flags | Type |
|---|---|---|
| Owned items, equipment, upgrades, quest items, bottles | `PouchData.<item>.StockNum`, 110 items, table below | Int, reset type 32 |
| "Was ever got" (first-get messages, collection) | `IsGet.<item>`, 199 names, including every pouch item | Bool |
| Songs | `IsGet.Item_Melody_*` only: songs have no `PouchData` entry | Bool |
| Rupees | `PlayerStatus.CurrentRupee` (0xa77921d7) | Int |
| Hearts | `PlayerStatus.Life` (0x31ab5580), `PlayerStatus.MaxLife` (0xfbe01da1); both default 12, so the unit is a quarter heart (inferred) | Int |
| Heart pieces | `PouchData.Obj_Heart_Piece.StockNum` (0x51ec97b3) | Int |
| Magic | `PlayerStatus.MP` (0x76b18200), `PlayerStatus.MPMax` (0x12d63a97) | Int |
| Strength, scale | `PlayerStatus.LiftGrade` (0x59979321, enum None/Low/Middle/High), `PlayerStatus.DiveDepth` (0x61466c21, float, default 3.0), besides the gauntlet and scale pouch items | Enum, Float |
| Double defence | `PlayerStatus.IsDefenseUP` (0x56bbe07c) | Bool |
| Equipped gear | `PlayerStatus.EquipWeapon` 0xa95a3521, `EquipShield` 0xee463235, `EquipArmor` 0x008d7f0c, `EquipBoot` 0xd107a85d, `EquipHeadArmor` 0x59d9c7b3, `EquipHandArmor` 0x7ce1cb81, `EquipOcarina` 0xb72cd3fc, `EquipArrow` 0x3615e379: actor names. `AnotherEraEquip*` hold the other age's | String64 |
| Item buttons | `PlayerStatus.AssignableItem.Child` 0xa0029837 and `.Adult` 0xbc32ce24 | Enum |
| Bottle contents | `PlayerStatus.AssignableItem.BottleInfo.Bottle<A–E>.BottleItem` (Enum: Empty, RedPotion, GreenPotion, BluePotion, BlueFire, Fish, Letter, BigPoe, Poe, Bug, Fairy, MaxMilk, HalfMilk) and `.IsValid` (Bool) | Enum, Bool |
| Dungeon items | `DungeonInfo.<dungeon>.IsGetMap`, `IsGetCompass`, `IsGetBossKey` (Bool), `SmallKeyNum` (Int), table below | Bool, Int |

Bottle hashes: A `IsValid` 0xf39ce410 / `BottleItem` 0x7d48df76; B 0x000ff6ea / 0x1edd164a; C 0xf63b1194 / 0xb80c70a8; D 0xb7f052ce / 0xe4c1b1fc; E 0xe27b136d / 0x365e1953.

Song hashes (`IsGet.Item_Melody_…`): ZeldaLullaby 0x01c20002, EponaSong 0xb492ac33, SariaSong 0xd3f8bfcc, SunSong 0x165dd1ad, TimeSong 0x03b3450e, StormSong 0x2b39dd4f, ForestMinuet 0xdc0a0b54, FireBolero 0x49041ab9, WaterSerenade 0x3fa63986, ShadowNocturne 0xc2fb95f4, SpiritRequiem 0x802a36a8, LightPrelude 0xd6320ba4, ScarecrowSong 0x5e86fb4d.

Not part of the inventory: `PlayerStatus` also holds position, retry and UI state. Two unnamed structs look item-related (both now read for bombs, see [Bombs from the cheat](#bombs-from-the-cheat-issue-125)): 0xf5ab6461 (UInt per assignable item and bottle, perhaps a get order for the menu) and 0xf80f8077, the ammo capacities ([below](#ammo-capacity-and-what-comes-with-an-item-issue-128)).

## Rules

| Rule | Meaning | Used for |
|---|---|---|
| Shared, max | Everyone takes the highest value anyone has | Pouch items that are owned or not, and the upgrade tiers |
| Shared, OR | True for everyone once true for anyone | `IsGet.*`, songs, map, compass, boss key, `IsDefenseUP`, a bottle being owned |
| Shared, mirror | The last change anyone makes is copied to everyone | Trade sequence items (egg, cucco, mushroom, saw, …): handing one over takes it away, so max would give it back |
| Count (delta) | A change by one player (+1 got, −1 used) is added to everyone's value | Small keys, Gold Skulltula count, heart pieces |
| Per player | Never sent | Rupees, ammo and other consumables, life, magic now, bottle contents, equipped gear, item buttons |

Choices made here:

- **Small keys** use the delta rule per dungeon (as Anchor does): one player opening a door spends the key for everyone, and a key picked up counts for everyone. The chest itself is a 64-bit-key flag ([flags.md](flags.md)) and must be shared too, or the key could be taken twice.
- **Heart containers and pieces** are key items. `MaxLife` uses max; `Obj_Heart_Piece.StockNum` uses the delta rule. Whether the game turns four pieces into a container by raising `MaxLife` and resetting the count is not known: the rules are on; check it live. `Life` stays per player.
- **Bottles**: `PouchData.Bottle<A–E>.StockNum` and `BottleInfo.Bottle<X>.IsValid` are shared (OR); `BottleItem` stays per player. A bottle given to a player who did not have it arrives empty.
- **Ammo**: the bag, quiver and wallet tiers are shared (max); the count in them is per player.
- **Magic**: `MPMax` and the spells are shared (max); `MP` is per player.
- `FairyBless_*` items are per player for now: they are temporary blessings, not owned items (inferred).

## Ammo capacity and what comes with an item (issue 128)

Int struct 0xf80f8077 holds how much of each ammo a player can carry; its members were named by hashing candidates against the struct's member hashes in `GameDataList`. The struct's own name is not known, so the flags have only hashes. All default to 0, reset type 32, saved.

| Member | Flag hash | Values seen in saves |
|---|---|---|
| Rupee | 0xe0010a50 | 200 with a wallet |
| DekuSeed | 0xae8cc742 | 50 |
| Bomb | 0xe753508b | 20 with the small bomb bag |
| Arrow | 0x418db791 | 0 without a quiver |
| DekuStick | 0x457616b0 | 10 |
| DekuNut | 0x735b00f7 | 20 |

The game's own get scene sets the capacity and the starting ammo with the item. An item that arrives from another player has no get scene, so the receiving game had the item with 0/0 (a player test: the shared Slingshot with no seeds, and no way out of the slingshot room). Now `ItemFlagStore`, when it gives a game an owned item that game did not have (0 to 1 or more), also raises that item's grants (`item_grants()` in `coop/host/game_items.hpp`) to at least these values, the original game's numbers:

| Item | Capacity | Ammo (count, at least) | Also |
|---|---|---|---|
| Slingshot | DekuSeed 30 | `Item_DekuSeed` 30 | `IsGet.Item_DekuSeed` |
| Bullet bag small / medium / large | DekuSeed 30 / 40 / 50 | filled to it | |
| Bow | Arrow 30 | `Item_Arrow_Normal` 30 | `IsGet.Item_Arrow_Normal` |
| Quiver small / medium / large | Arrow 30 / 40 / 50 | filled to it | |
| Bomb bag small / medium / large | Bomb 20 / 30 / 40 | filled to it | `IsGet.Item_Bomb` (small) |
| Nut bag small / medium / large | DekuNut 20 / 30 / 40 | filled to it | `IsGet.Item_DekuNut` (small) |
| Stick bag small / medium / large | DekuStick 10 / 20 / 30 | filled to it | `IsGet.Item_DekuStick` (small) |
| Wallet small / medium | Rupee 200 / 500 | none | |
| `IsGet.Item_DekuStick` / `IsGet.Item_DekuNut` (the other player's first stick or nut pickup) | DekuStick 10 / DekuNut 20 | none | |

A value already at or above the floor is left alone, so nothing is taken away. An item a game already holds gets its capacity again on every write of it through the store (ammo only for a new item): a save that got a shared item with room for 0 (issue 138: sticks and seeds that could not be picked up) was meant to be mended by the snapshot when it joins. A run of integrate-p7 showed the snapshot arrives on the title screen, before the save loads, and the load then puts the save's capacities back (room for 0 again). So grants now wait for Link: while there is no Link (the title screen, a save or a scene loading), items are merged but nothing is granted, and an item new to the game is noted. For 90 frames (about 1.5 s) after Link appears, the hook mends every held item's capacity, and a noted item gets its whole grant, ammo too (`LoadMend`, `ItemFlagStore::mend`, log `coop: items: N capacities mended after a load`). So a Slingshot that comes during a door's loading screen, or before a joiner's save loads, still comes with 30 seeds. A game that gets the item itself is left to the game. The ammo stays per player afterwards: only this one fill is written. The large wallet is left out (its size is not known). Not checked in a game: that the game reads the capacity from these flags (the cheat that sets them, `LICHII_COOP_OBJECTS_CHEAT`, gave working bombs), and the sizes of the remake's bags beyond the ones in the table above.

## Pouch items

Each row: `PouchData.<item>.StockNum` and `IsGet.<item>` (— when there is none).

| Item | StockNum hash | IsGet hash | Rule |
|---|---|---|---|
| `Armor_Adult_Gauntlet_Gold` | 0x40ae1183 | 0x3ef15ec4 | Shared, max |
| `Armor_Adult_Gauntlet_Silver` | 0x0012b8c0 | 0x87e58de3 | Shared, max |
| `Armor_Adult_Goron` | 0x21ecbd19 | 0xae5611af | Shared, max |
| `Armor_Adult_HeavyBoot` | 0x2bad189a | 0x5fa190e6 | Shared, max |
| `Armor_Adult_HoverBoot` | 0xc9220366 | 0x1109c920 | Shared, max |
| `Armor_Adult_Normal` | 0x6f631388 | 0xde513abd | Shared, max |
| `Armor_Adult_Zora` | 0xb816af94 | 0x4f5a929c | Shared, max |
| `Armor_Child_Gauntlet_Goron` | 0xd1317941 | 0x9880707e | Shared, max |
| `Armor_Child_Mask_Amiibo0` | 0xf5e647da | 0x06f13bb2 | Shared, max |
| `Armor_Child_Mask_Amiibo1` | 0x486c59a4 | 0x09df9d25 | Shared, max |
| `Armor_Child_Mask_Bunny` | 0x7e607033 | 0x65aebe17 | Shared, max |
| `Armor_Child_Mask_Gerudo` | 0x0e94a8df | 0xb1fc4c1d | Shared, max |
| `Armor_Child_Mask_Goron` | 0xfc508da7 | 0xadfccd91 | Shared, max |
| `Armor_Child_Mask_Keaton` | 0xb5c10899 | 0x35c60e1d | Shared, max |
| `Armor_Child_Mask_Skull` | 0x47b2d775 | 0xd097482a | Shared, max |
| `Armor_Child_Mask_Spooky` | 0xf046629e | 0xc0cf68a7 | Shared, max |
| `Armor_Child_Mask_Truth` | 0x96459cd3 | 0xf7d5bd9c | Shared, max |
| `Armor_Child_Mask_Zora` | 0x924e9a46 | 0x280eb00e | Shared, max |
| `Armor_GlassTruth` | 0x49d1c374 | 0x7fe2a5b2 | Shared, max |
| `BottleA` | 0xd53ce3a7 | — | Shared, OR (the bottle; contents per player) |
| `BottleB` | 0xfca29a04 | — | Shared, OR (the bottle; contents per player) |
| `BottleC` | 0x893216a1 | — | Shared, OR (the bottle; contents per player) |
| `BottleD` | 0x5324f9d3 | — | Shared, OR (the bottle; contents per player) |
| `BottleE` | 0xc92b16be | — | Shared, OR (the bottle; contents per player) |
| `Item_AgonyStone` | 0xf3b14ce2 | 0xdd587ed8 | Shared, max |
| `Item_AlarmCucco` | 0xb6ca6093 | 0x14909166 | Shared, mirror (trade item) |
| `Item_Arrow_Fire` | 0x5e529ab9 | 0x4db7eae0 | Shared, max |
| `Item_Arrow_Ice` | 0x40735a8d | 0x5c8f3099 | Shared, max |
| `Item_Arrow_Light` | 0xbfb285c8 | 0x8637bab4 | Shared, max |
| `Item_Arrow_Normal` | 0x518ec74b | 0x572701ea | Per player |
| `Item_Bomb` | 0xdb1bcd4f | 0x2bb00768 | Per player |
| `Item_BombBag_Large` | 0xbd3ca2c6 | 0x968a5a79 | Shared, max |
| `Item_BombBag_Medium` | 0xe5ff45aa | 0xb2c478da | Shared, max |
| `Item_BombBag_Small` | 0xb0a1bf51 | 0xa63f1e2d | Shared, max |
| `Item_Bombchu` | 0xdf7c517a | 0x5ab49c8e | Per player |
| `Item_Boomerang` | 0xa1d76d31 | 0x7f4ce38d | Shared, max |
| `Item_BrokenSword` | 0x2815c82a | 0x645a8175 | Shared, mirror (trade item) |
| `Item_BulletBag_Large` | 0x7b9673f5 | 0xb669f025 | Shared, max |
| `Item_BulletBag_Medium` | 0x9a30a9e5 | 0xe6cd55d2 | Shared, max |
| `Item_BulletBag_Small` | 0xdc52acaf | 0x488ca1be | Shared, max |
| `Item_ClaimCheck` | 0xded8e8d3 | 0x8b54e680 | Shared, mirror (trade item) |
| `Item_CojiroCucco` | 0x51d89ef0 | 0xfb4442de | Shared, mirror (trade item) |
| `Item_DekuNut` | 0xf30d9f54 | 0x00c2ee37 | Per player |
| `Item_DekuSeed` | 0x5fadf7fc | 0x29888e1e | Per player |
| `Item_DekuStick` | 0xb9494411 | 0xc4e21ac2 | Per player |
| `Item_DinFire` | 0xb796c16d | 0xfa04e611 | Shared, max |
| `Item_EyeballFrog` | 0x919ea324 | 0x8e02d1ca | Shared, mirror (trade item) |
| `Item_EyeDrop` | 0x5c41a6b1 | 0xb7bdd577 | Shared, mirror (trade item) |
| `Item_FairyBless_DefenseDouble` | 0x9a826fab | 0x944b78be | Per player |
| `Item_FairyBless_MP` | 0x5e671d92 | 0xb1309073 | Per player |
| `Item_FairyBless_MPDouble` | 0x5f043856 | 0x87ca4826 | Per player |
| `Item_FaroreWind` | 0x47f2ca87 | 0xffabf058 | Shared, max |
| `Item_Hammer` | 0x1aafdb66 | 0x7227efd8 | Shared, max |
| `Item_Hookshot` | 0x8573d2cd | 0xff98ed58 | Shared, max |
| `Item_Longshot` | 0x8499ea34 | 0x38aea3bb | Shared, max |
| `Item_MagicBean` | 0xb003124d | 0x6d39c600 | Per player |
| `Item_Medal_Fire` | 0x5ce254a6 | 0x103a0dc7 | Shared, max |
| `Item_Medal_Forest` | 0x692d57cb | 0xfcd8d653 | Shared, max |
| `Item_Medal_Light` | 0x235c7d9a | 0x9026c187 | Shared, max |
| `Item_Medal_Shadow` | 0x204d2063 | 0x12d63ac6 | Shared, max |
| `Item_Medal_Spirit` | 0x0c7a5fd0 | 0xe269df38 | Shared, max |
| `Item_Medal_Water` | 0x0151a68f | 0xcf64e780 | Shared, max |
| `Item_NayruLove` | 0xf0828cd0 | 0x00b44241 | Shared, max |
| `Item_NormalBoot` | 0x55e804de | 0x7215060c | Shared, max |
| `Item_NutBag_Large` | 0xd60e4b23 | 0x9a5db0b6 | Shared, max |
| `Item_NutBag_Medium` | 0xeaa1785e | 0x960820db | Shared, max |
| `Item_NutBag_Small` | 0x19b80dbc | 0x2bedd82e | Shared, max |
| `Item_Ocarina_Fairy` | 0xb7df60f1 | 0x42bd296f | Shared, max |
| `Item_Ocarina_Time` | 0x54e2a3d1 | 0x1da2c511 | Shared, max |
| `Item_OddMushroom` | 0x07de3e6f | 0xcbd7852a | Shared, mirror (trade item) |
| `Item_OddPowder` | 0x58c9bc78 | 0xb75acaad | Shared, mirror (trade item) |
| `Item_PoacherSaw` | 0xa862d276 | 0x888d5059 | Shared, mirror (trade item) |
| `Item_PocketCucco` | 0x0165e7ca | 0xe82e6413 | Shared, mirror (trade item) |
| `Item_PocketEgg` | 0x07f10aaf | 0xec9c3136 | Shared, mirror (trade item) |
| `Item_Prescription` | 0x897cf7eb | 0xd8f1521e | Shared, mirror (trade item) |
| `Item_Quiver_Large` | 0xd331c7da | 0x21985b09 | Shared, max |
| `Item_Quiver_Medium` | 0x3bb370e1 | 0x9d872202 | Shared, max |
| `Item_Quiver_Small` | 0xa8c3e1fb | 0x0a550531 | Shared, max |
| `Item_Scale_Gold` | 0x4aeb140a | 0xccc1909a | Shared, max |
| `Item_Scale_Silver` | 0x1240044b | 0x1208fb3e | Shared, max |
| `Item_ScrubBusinessSeed` | 0x600e29eb | 0xcfbb16f6 | Shared, max |
| `Item_StickBag_Large` | 0x48e53698 | 0xa10f2dcf | Shared, max |
| `Item_StickBag_Medium` | 0x41db7ab5 | 0xcbf00480 | Shared, max |
| `Item_StickBag_Small` | 0x2fe6ab1a | 0xde2389d4 | Shared, max |
| `Item_Stone_Fire` | 0x7d69ad47 | 0x8908d6ad | Shared, max |
| `Item_Stone_Forest` | 0xcbd1e58c | 0x110b83ec | Shared, max |
| `Item_Stone_Water` | 0xb8e3bf45 | 0xeec383f5 | Shared, max |
| `Item_Token_Gerudo` | 0xfeb4d56b | 0x0e6264c7 | Shared, max |
| `Item_Token_Gold` | 0x04735978 | 0x6ba394fe | Count (delta) |
| `Item_Wallet_Large` | 0xafa4bb52 | 0xb5031190 | Shared, max |
| `Item_Wallet_Medium` | 0x2d2c874b | 0xd1360de8 | Shared, max |
| `Item_Wallet_Small` | 0x1d74617e | 0xb6e5a3a5 | Shared, max |
| `Item_WeirdEgg` | 0x24596e8e | 0x44ab1487 | Shared, mirror (trade item) |
| `Item_ZeldaLetter` | 0x6be5c3b0 | 0xf19393b5 | Shared, mirror (trade item) |
| `Obj_Armor_Child_Normal` | 0x84ba0d24 | 0xf9594156 | Shared, max |
| `Obj_Heart_Piece` | 0x51ec97b3 | 0xe6aaab65 | Count (delta), see Hearts |
| `Pouch_Amiibo` | 0x20820479 | 0x3506a557 | Shared, max |
| `Pouch_Range_BowAndArrow_Fire` | 0xe32ad20f | 0xf9fe4ffc | Shared, max |
| `Pouch_Range_BowAndArrow_Ice` | 0x667c7ba3 | 0x0989e62d | Shared, max |
| `Pouch_Range_BowAndArrow_Light` | 0x28298287 | 0xb03ae135 | Shared, max |
| `Weapon_Range_Bow` | 0x938d56bf | 0x69ec28be | Shared, max |
| `Weapon_Range_SlingShot` | 0x80d34ce9 | 0x0ec22ee1 | Shared, max |
| `Weapon_Shield_Deku` | 0x7ca64f23 | 0x5217da1e | Max (issue 130); a loss stays in that game |
| `Weapon_Shield_Hylian` | 0x08befd3a | 0x2f817d54 | Shared, max |
| `Weapon_Shield_Mirror` | 0xf58e0e2f | 0x75e94e42 | Shared, max |
| `Weapon_Sword_Biggoron` | 0xb0b3e399 | 0x33466812 | Shared, max |
| `Weapon_Sword_BrokenGiant` | 0x8d02f5ea | 0x720df203 | Shared, max |
| `Weapon_Sword_Giant` | 0x396a5c02 | 0x1e824efa | Shared, max |
| `Weapon_Sword_Kokiri` | 0xeeb495b9 | 0x405d8a0f | Shared, max |
| `Weapon_Sword_Master` | 0xf1c45294 | 0x7ae38e5b | Shared, max |

## Dungeon items

`DungeonInfo.<dungeon>.<member>`, all saved in `progress.sav`.

| Dungeon | IsGetMap | IsGetCompass | IsGetBossKey | SmallKeyNum |
|---|---|---|---|---|
| `InsideTheDekuTree` | 0x89787395 | 0x06c51ff2 | 0xf6995cee | 0x0914ba07 |
| `DodongoCavern` | 0xa52c3a19 | 0x8b93edf9 | 0xfaabc084 | 0x3fdbc3d0 |
| `InsideJabuJabuBelly` | 0xfcadd210 | 0x9a2218db | 0x656bd5ee | 0x1470a816 |
| `ForestTemple` | 0xe704f9b4 | 0xf240eeb2 | 0x427333a0 | 0x90237092 |
| `FireTemple` | 0x5b3ac0ba | 0x0aa068b6 | 0xb72d73df | 0x1df11487 |
| `WaterTemple` | 0x4a4402ab | 0x2e82c8fe | 0x79daf80a | 0x877bdcee |
| `ShadowTemple` | 0xf6177b93 | 0x1e2feec5 | 0xc33c66cf | 0x5edda9e3 |
| `SpiritTemple` | 0xb85002ba | 0x9bd1181c | 0x1984c03f | 0x79eeec8e |
| `BottomOfTheWell` | 0x77c47ec9 | 0x9d6efaac | 0x9402715c | 0x7dd79d10 |
| `IceCavern` | 0x322f2a94 | 0xea1846e6 | 0x275f4512 | 0xfa5d133d |
| `GerudoTrainingGround` | 0x4b36041e | 0x3d9a2750 | 0x6d155841 | 0xea9786f5 |
| `InsideGanonCastle` | 0x9bbc0c4a | 0xc4d4734e | 0x2fd59293 | 0xfbfd2910 |
| `FinalBattleField` | 0xb3b65c1a | 0x874493ce | 0x6d0fea11 | 0xa04bfd48 |
| `GerudoFortress` | 0xbcaad6ce | 0x5eb80679 | 0x95fbcb7c | 0x3a28b6a4 |

Each dungeon also has `BossRetryNum` (not saved) and three members not named yet (a Bool, an Int and a UInt). `IsClear` is a string in `main` and may be one of them.

## How the game shows "you got X"

From the event flows, not traced in code:

- `Event/EventFlow/Dm_SY_Get.bfevfl` is the get-item scene. The item is a guest actor (`PouchContentGuestPerformer`); `PouchSystemActor` runs `EventTriggerAddOwnToPouch` (adds it to `PouchData`), `SoundSystemActor` plays the fanfare (`EventTriggerSoundStartFanfare`, music `BGM_ItemGet_Small/Medium/Large`), `EventTriggerSetOwnNameToMessage` puts the name in the message, and `UISystemActor` opens the window (`EventTriggerRequestOpenPickUpWindow`). It ends with `EventTriggerRequestAutoSave`.
- `Dm_SY_Get_WithoutPlayer` is the same without Link's animation, and `Dm_SY_GetInEvent` the one used inside other events.
- `HavePlayedGetEvent.<item>` (Bool) and `HavePlayedGetEventAtShop.<item>` mark that the long scene was seen.

For a shared item the cheapest notice is our own overlay line ("Player 2 got the Hookshot"). Using the game's own window would mean starting `Dm_SY_Get_WithoutPlayer` with the item as the guest actor, which needs the event system from the host; not tried.

## One-time pickups (Caleb, 2026-11-09)

Caleb: Gold Skulltulas "should be sync'd since they are a one time thing to grab". So every one-time pickup in the world is shared: once any player takes it, it is gone for everyone, and what it gives counts for everyone.

| Pickup | The pickup itself (gone for everyone) | What it gives (everyone) |
|---|---|---|
| Gold Skulltula | its placement's 64-bit key (`Obj_RupeeToken`, 85 placements, [flags.md](flags.md)) | `PouchData.Item_Token_Gold.StockNum`, count (delta), on |
| Heart piece in the world | its placement's 64-bit key | `PouchData.Obj_Heart_Piece.StockNum`, count (delta), on |
| Heart container (boss) | its placement's 64-bit key, if it has one | `PlayerStatus.MaxLife`, max, on |
| Small key lying in the world or in a chest | a chest: its 64-bit key; a lying key: none (the game keeps no 64-bit key for one), so its taking is a `WORLD_BREAK` ([world.md](world.md)) | the dungeon's `SmallKeyNum`, count (delta), on |

The "what it gives" flags are named and on in `item_shared_flags()`. The pickups are 64-bit keys. Those placed in dungeons (chests, Gold Skulltulas, heart containers, silver rupees) are listed and shared by `CoopDungeon::Keys()` ([dungeon.md](dungeon.md)); the others are not listed: they go in the user's shared-flags file ([host.md](host.md)), each as `key64:`, **whatever its `ResetTypeValue`** (the rule for chests and pots is persistent keys only; a one-time pickup is in the list even when its type is not 24). Not mapped yet, needs the keys read from the Banc placements or live: which `Obj_RupeeToken` key is which Skulltula, and the placement actor names and keys of heart pieces and heart containers. Also to check live: whether a Skulltula's key is set when it is killed or when its token is picked up (if killed, a player who kills it and leaves before taking the token takes it from everyone).

## In the code

`coop/host/game_items.hpp` holds the tables above as data: 302 flags, each with its kind and rule, and a test hashes every name. `item_shared_flags()` is the shared list built from it (284 flags: every rule but per player). `ItemFlagStore` wraps the game's `GameFlagStore` and merges on write: max, OR and mirror as in the rules table, per-player flags and a wrong kind refused, anything that is not an item passed through. The host's `FlagWatcher` answers a `FLAG_SET` with what its game holds after the write, so the merged value is what everyone gets; a player's game takes the host's `FLAG_STATE` through the same store, so an item it has is not taken away by an answer that lacks it.

In the emulator (`lichii/hook`): the shared list is `item_shared_flags()` plus the flags of the user's `LICHII_COOP_FLAGS` file, and `Session::frame` gets an `ItemFlagStore` over the game's `GameFlagStore` every frame, so items are shared whenever co-op is on (`LICHII_COOP_ITEMS=0` turns them off, [lichii.md](lichii.md)). With `LICHII_COOP_DEBUG_FLAGS=1` an Int or Bool change of an item flag ends with `item <name>` in the log. `LICHII_COOP_CHEATS=1` adds a debug window that gives an item flag the way a pickup changes it (`lichii/hook/coop_cheats.*`, [lichii.md](lichii.md)).

Counts (small keys, Gold Skulltula tokens, heart pieces) travel as count deltas ([protocol.md](protocol.md), "Counts"): a player's `FlagWatcher` sends its change (kind 4) when `FlagStore::is_count` says so, which `ItemFlagStore` does for every count item; the host's `ItemFlagStore` adds a delta to the game's value (never below 0) and the host's watcher sends the total; a player writes a total on top of any change of its own not sent yet. So two players gaining at once both count (tested on fake games: two players and the host gaining and spending in the same frame end on the same total). Every rule is on now, `MaxLife` (max) too.

Still to check in a run for hearts: whether the game turns four pieces into a container by raising `MaxLife` and setting the piece count back. If it does, the deltas carry it (+1 and then −4 to the piece count, `MaxLife` by max).

With the host's save load (protocol 7, [flags.md](flags.md), "Saves") items go back to the host's save exactly: a `FLAG_RESET` is not merged by max, so an item the host's save lacks is taken away again. Effects of max to know about: an item a player loses (a Hylian Shield eaten, the Giant's Knife breaking) comes back, because the host keeps the highest value (the Deku Shield is per player, below). `LiftGrade` (Enum) and `DiveDepth` (Float) are not in the table yet; if the game reads strength from them and not from the gauntlet items, a shared gauntlet does not lift for the other player. Check in a run.

**Shops (issue 130, found by Lixyas).** A Deku Shield one player buys reaches every player (Caleb, 2026-11-12: "buying deku shield was not sync'd"): `PouchData.Weapon_Shield_Deku.StockNum` is max and `IsGet.Weapon_Shield_Deku` any again (branch `items-progress` had made them per player, so a bought shield stayed in the buyer's game). The purchase is the game's own pouch write (`DisplayShop` event flow), so the flag watcher sends it like any other item; nothing in the shop is shared (stock, sold-out state). The other players then hold one, and the shop refuses them a second ("You already have a Deku Shield. We have a one-shield-per-person policy here", `Npc_Kokiri_001_Shop_Child` message `Shop_31`, which reads the player's own StockNum): by design. A shield lost in one game (burnt, eaten by a Like Like) is a max drop: it stays lost in that game and is not sent (the MaxLife echo rule, `FlagWatcher` `max_drops_kept`), so that player buys a new one, as in the one-player game, and the others keep theirs; the new one goes out but changes nothing elsewhere (max 1). The host's save load (`FLAG_RESET`) writes the host's value exactly. The Hylian and Mirror Shields are shared (max) too. Unit tests: "a Deku Shield one player buys reaches every player", "a Deku Shield the player buys first reaches the host". The shop itself opens only from Step02 of the game's quest `Meet_DekuTree` (Mido's talk); a player whose Mido stepped aside for another player gets that step from the shared story ([flags.md](flags.md), "Quests that follow the story"), so a player who lost the shield can buy one. Bombs, nuts, seeds, sticks and arrows (counts per player, bag tiers max), potions (bottle contents per player) and rupees (per player) are bought by each player from their own pouch and wallet, so the shop checks ("bag's full", "not enough rupees") read only that player's values. Not checked in game.

## Live check (2026-11-09)

Two copies of the emulator (A and B, the same child save, a local server, `LICHII_COOP_DEBUG_FLAGS=1`, `LICHII_COOP_CHEATS=1`); Caleb played, gifts came from the cheat window.

- **Counts cross both ways and add up.** A Gold Skulltula token given in A (`Item_Token_Gold.StockNum` 5 -> 6) showed in B within a quarter second, and in B's pause menu. Tokens, heart pieces and Forest Temple small keys given in both games, several in a row, ended on the same totals in both: tokens 13, heart pieces 11, keys 3.
- **Key items cross.** The Hookshot (`StockNum` and `IsGet`) and the Song of Storms given in B showed in A's menus. A dungeon's `IsGetBossKey` given in A reached B.
- **A reload in A** (its tables read 0 for one scan, then the same values again) changed nothing in B.
- **Heart pieces do not make hearts by themselves.** Raising `Obj_Heart_Piece.StockNum` (to 11) adds no heart in the game: the conversion happens in the pickup, not from the count. Hearts follow `MaxLife` (max rule); the cheat window has a heart container button for it. Not checked live yet: `MaxLife` crossing, and what a real heart piece pickup writes.
- `StockNum` 1 = owned holds for the Hookshot: 1 made it show; the first cheat build raised it past 1 on repeated clicks (2, 3) and the game still showed it; the cheat now stops at 1.

## Live check 2 (2026-11-09): late joiner, hearts, a real Gold Skulltula

The same kit, both saves reset to the child save; game A and the server first, game B started minutes later.

- **A late joiner gets the items.** The Hookshot given in A before B started was in B's menu when B loaded the save, with nobody clicking anything in B. (The unit test "a late joiner gets the host's items and keeps its own" covers the same path on fake games: the server's `FLAG_SNAPSHOT`, merged by `ItemFlagStore`, the joiner's own extra items sent back.)
- **`PlayerStatus.MaxLife` is not where the game keeps its hearts.** The cheat's 16 -> 20 showed a fifth heart in both games (so the flag crosses), but each game wrote 16 back on its own within a quarter second (`int 0xfbe01da1 20 -> 16` with game B not yet running), and the heart was gone once the menu opened. The flag is a copy; the real maximum is elsewhere (likely the player), not mapped. Anchor, for comparison, raises the game's own `healthCapacity` and turns 4 pieces into a heart itself when one arrives. So heart containers do not cross yet.
- **A Gold Skulltula is two 64-bit keys, set at token pickup.** The Kakariko tree Skulltula: in each game its own token pickup set `0x6a8d9b2373d08fc9` and `0x6a8d9b2373d08fcb` 0 -> 1 in the same scan as `Item_Token_Gold.StockNum` +1 (the clocks of A and B line up 412 s apart, so each game's pickup and the other's token arriving match). Other placements show the same pairs (key and key + 2). Not split yet: whether the kill alone sets either.
- **Shared, the keys cross but a loaded Skulltula stays.** With both keys in the run's shared-flags file, A's pickup set them in B in the same second, yet B, already in the scene, still got the Skulltula out of the tree and its token: both games ended +2. The game reads the key when the actor is placed, not after. Anchor does the same flag sync and also deletes a Gold Skulltula (`EN_SW`) or token (`EN_SI`) actor every frame its flag is set; we need that step too (object side). Not checked: that the Skulltula is gone in B after a scene change.

## Bombs from the cheat (issue 125)

With `LICHII_COOP_OBJECTS_CHEAT=1` the bomb bag, 20 bombs and the bomb capacity were set, the bombs showed on ZR with 20, but every ZR made an `Item_Bomb` over Link's head that was gone 0.1 s later, the count stayed 20 and nothing was thrown (autotest `spawn-bomb`, runs 20261109-054214 and 20261110-010524).

**Not the co-op code.** In run 010524 the spawn sync was off (no `coop: spawn: on` line), no delete was asked by the objects sync, the drops or the director (none of their delete lines or counts), and the bomb is not a shared object (`IsShared`, `IsDrop` say no). Writing the cheat every 2 frames instead of once a second changed nothing (s5-spawn-sync-2).

**What a real bomb bag sets that the cheat did not.** The autotest kits' save baselines hold a second user profile whose saves come from a player who got the bag in the game (slots 0 to 3 and 5: 10 to 14 bombs). Compared flag by flag with the autotest profile's six saves (no bombs) and that profile's slot 4 (no bombs), with the flag types from `GameDataList`, these bomb flags are set in every bomb save and 0 in every other:

| Flag | Struct, member | Real saves | Cheat before |
|---|---|---|---|
| `PouchData.Item_Bomb.StockNum`, `IsGet.Item_Bomb`, bag `StockNum` and `IsGet`, capacity 0xe753508b | (known) | set | set |
| UInt 0xc7ecb4a1 | 0xf5ab6461 (one UInt per assignable item and bottle), `Item_Bomb` | 35 (the slingshot 34): the order the items were got in | 0 |
| Bool 0x6e5e7d9a | 0x541947d0 (53 members: `Bomb`, `Bottle`, `DekuNut`, `DekuStick`, `Ocarina`, `SlingShot`, `Bow`, ...), `Bomb` | 1 | 0 |
| Bool 0xfced7932 | 0x57f52eb8 (the same 53 members), `Bomb` | 1 | 0 |
| Bool 0x4d345bbe | 0x41f3e410 (199 members, one per item, like `IsGet`), `Item_BombBag_Small` | 1 | 0 |

The struct `PouchData.Item_Bomb` has no member but `StockNum`. The capacity struct 0xf80f8077 is Rupee, DekuSeed, Bomb, Arrow, DekuStick and DekuNut (member names hashed). The two 53-member structs are most likely the item help boxes (shown and closed): the slingshot's members are 1 in every save that has the slingshot, and the cheat's first ZR opened the "Bomb" help box. `0x41f3e410.Item_Bomb` is 0 in one real save, so it is not needed. Which of the four the game checks when Link takes a bomb out is not known.

**The fix (not run yet).** The cheat (`lichii/hook/coop_cheat_items.cpp`, tested in `tests/lichii/cheat_items_test.cpp`) now also sets the three Bools and gives the bomb the next get order after the highest of the 42 items. UInt flags are written through the game's UInt queue (`GameFlags::set_uint`, [flags.md](flags.md)). A save with real bombs is left as it is.

**Checks in the next run.** A taken bomb lowers the count, so the cheat tops it up again and logs `coop: objects: cheat Item_Bomb 19 -> 20`; the bomb stays on the list (`coop: objects: + Item_Bomb`, no `-` line for over a second). Without that line, the bombs are still the problem; the other way is to load one of the real bomb saves.

**Run of 2026-11-10 (chunk 3, spawn-bomb 20261110-094355): still 0.1 s.** All four flags were written (`cheat 0x541947d0.Bomb 0 -> 1` and the rest, `get order Item_Bomb 0 -> 19`, no "not found"), and they reached the game: the "Bomb" help box no longer came up at the first ZR. Yet every ZR made an `Item_Bomb` that left the list 0.05 to 0.11 s later, and the count stayed 20. So the flags a real bag sets are not enough when they are written into a running game. What all the cheat's runs share is that the bombs arrive after the save has loaded (the load puts the saved 0 back, and the cheat writes again about a second later), in a scene loaded without them. Likely (not checked in the code): the game sets up what an item needs (the bomb actor's resources, or a copy of the pouch) when the save or the scene loads, from what is owned then.

**Next: bombs in the save itself.** `tools/autotest/save_flags.py bombs` writes the same flags into the kits' baseline saves ([testing.md](testing.md)), so the game loads them as it loads a player's own bombs. If the bombs then stay, the cheat is the problem, not the flags. A shared bomb bag then needs a scene change before its bombs work, which is the same for every item a player gets from another player (a follow-up for #128). If they still go after 0.1 s with the bombs in the save, the cause is elsewhere, and the next try is the real bomb save (the kits' second user profile, slot 0).

**Later runs (2026-11-10 and 11, branch bomb-vanish-4).** With the bombs in the save it still happened, with co-op off too (`-NoCoop`, spawn-bomb 20261110-131336), and by hand on two co-op games at the Graveyard (20261111-005330): one ZR made a new `Item_Bomb` every 0.1 to 0.2 s while it was held, each gone 0.09 to 0.14 s later, 0.02 m from where it was made, the count still 20. The game keeps trying to take a bomb out and the bomb dies before Link holds it. What is ruled out:

- Our code: no co-op write in that window (no delete, drop, remover, director or spawn-copy line; the spawn sync only reports the bomb and its end), and co-op off does the same. The puppet patch is not applied with co-op off.
- The puppet pack (`coop-puppet`, loaded with co-op off too): its three tables, read back with `tools/puppet_pack`, hold every row of the game's own `ActorInfo` and `GameActorInfo` unchanged (8363 of 8363, `Item_Bomb` included), and the size table changes only its two own entries and adds 18 for the pack's files.
- The bomb flags: every flag with `Bomb` in its name (named from `GameDataList` by murmur3 of `IsGet.<actor>`, `PouchData.<actor>.StockNum` and the struct members) are the same in the tool's saves and the kits' second profile, whose bombs were taken out and used by a player.

**The cause: no lift grade (Caleb's hint, the Goron's Bracelet).** A bomb is lifted out of the bag, and what Link can lift is `PlayerStatus.LiftGrade` (0x59979321, Enum None/Low/Middle/High), not the bracelet's own flags. Every real bomb save holds the Goron's Bracelet and `Low`; the test saves had no bracelet and `None`, so every pulled bomb was deleted at once. The game sets the grade when Link gets the bracelet himself. Co-op shares the bracelet's `StockNum`/`IsGet` (max), not the grade, so a player who got the bracelet from another player had the same problem with bombs (and anything else lifted). Fixed both ways: `save_flags.py bombs` also gives the bracelet and `Low`, and the hook raises the grade to what the held strength items give whenever Link is in the game (`lift_grade_raise`: bracelet Low, Silver Gauntlets Middle, Golden Gauntlets High; never lowered, an unknown value left alone; log `coop: items: lift grade ... -> ...`).

## The ocarina (issue 190)

A player given the ocarina by another player could not take it out. D-pad Left takes out the ocarina named in `PlayerStatus.EquipOcarina` (String64, an actor name, "" by default). The game writes it when Link gets an ocarina himself; co-op shares the ocarina's `StockNum` and `IsGet` (max, OR) but not the equipped name, so the player who never met Saria had the ocarina in the menu and "" in `EquipOcarina`.

Checked offline on 2026-11-11 (no game run): every one of the 63 distinct saves in `coop-kit` holds the Fairy Ocarina and `EquipOcarina = "Item_Ocarina_Fairy"`. Besides it, they hold three Bools set to 1 (default false), named by walking `GameDataList`'s structs: `0x541947d0.Ocarina` (0xaacdef1f), `0x57f52eb8.Ocarina` (0xda41879d), the two help-box structs that the bombs showed ([above](#bombs-from-the-cheat-issue-125)), and `0x41f3e410.Item_Ocarina_Fairy` (0x137df1ac). The ocarina has no get order (it is not one of struct 0xf5ab6461's 42 members) and no item button (D-pad Left is its own).

The fix: whenever Link is in the game, the hook equips the best held ocarina when `EquipOcarina` is "" or is the Fairy Ocarina while the Ocarina of Time is held (`ocarina_equip_mend`; log `coop: items: ocarina '' -> 'Item_Ocarina_Fairy' for the held ocarina`). It is written straight into the String64's buffer, as the teleport does; never lowered, and a name it does not know is left alone. The help-box Bools are left to the game: at worst the first D-pad Left shows the ocarina's help box. Songs are `IsGet.Item_Melody_*` (shared, OR).

Not checked in a run yet: that the written name is enough for D-pad Left, and that songs learned by another player can be played. The autotest `ocarina-given` checks both on B, after `tools/autotest/save_flags.py no-ocarina` has taken the ocarinas out of B's baseline save (what a save without them holds: the flags above at their defaults and `EquipOcarina` "").

## Not done

1. Gold Skulltula placement keys: one known (above); the rest (`Obj_RupeeToken`, 85 placements, and any others) to read from the Banc placements; a loaded Skulltula or token removed when its key is set by another player; heart piece placement keys.
2. Heart containers: find the game's real maximum life (not `PlayerStatus.MaxLife`) and write it, with the 4-pieces conversion.
3. What `StockNum` holds for other owned items (1 for the Hookshot), and what a real heart piece pickup writes.
4. The unnamed structs and dungeon members above.

## One-time pickups and heart containers (branch `shared-pickups`, issues 89 and 88)

**Gold Skulltulas, tokens, heart containers and heart pieces (issue 89).** Their "taken" state is the placement's 64-bit key, which no list names, so it was never shared and the pickup stayed in the other games. Now:

- The object walk (`LICHII_COOP_OBJECTS=1`) notes the placement hash of every loaded `Enemy_Skulltula_Gold`, `Obj_Token_Gold`, `Obj_RupeeToken_*`, `Obj_Heart_Container` and `Obj_*Heart_Piece*`, and since branch `breakables` also every placed `Obj_Rupee_*`, `Obj_Heart` and `Obj_Heart_InWater` (not the minigames') (`coop_pickups.cpp`). All 170 placed rupees and 25 placed hearts have a 64-bit key the game never resets (ResetTypeValue 24, read from the romfs), so none of them respawns. The lying small keys (`Obj_Dungeon_SmallKey*`, 10 placements: Bottom of the Well, Shadow Temple, Gerudo Fortress) have no 64-bit key at all: their taking goes through the world sync instead ([world.md](world.md), small keys). The hook hands each new one to the flag watcher (`Session::share_pickup`), which shares `key64:<hash>` from then on (`coop: flags: one-time pickup ... is shared from now on`).
- A game that is not in that scene still takes it: with learning on, an incoming 64-bit key set to 1 that the watcher does not share yet is added as a pickup and written (at most 4,096 a session). The host does the same for a player's `FLAG_SET`, so the key is in its snapshot for players who join later.
- One way only: a pickup's key going back to 0 (the game resetting it on a scene change) is neither sent nor written over a 1.
- The game that did not take it deletes its loaded copy of the actor as before (`coop_objects_sync.cpp`, a key64 flag arriving set to 1). The token count (`Item_Token_Gold`, count rule) was already shared.
- `LICHII_COOP_PICKUPS=0` turns both off.

Not checked in a game: that a Skulltula's placement hash is the key the game sets when it is taken (flags.md says the 85 `Obj_RupeeToken` placements are in the key list, by placement hash; the enemy's own placement may be another key, then the token's key carries it). A pickup already taken before the session started is not told to the others.

**Gold Skulltulas (issue 89, branch `items-progress`).** A player test: one player killed a Gold Skulltula and took its token, both counts rose, but the token stayed in the other game, and the other player took it too, so the shared count rose twice. Two things now, besides the world sync's token removal ([world.md](world.md), tokens):

- **All 100 Skulltula keys are shared from the start.** `gold_skulltula_keys()` (`coop/host/src/game_items.cpp`) lists the placement hash of every `Enemy_Skulltula_Gold` (read from the romfs Banc files; 100 keys, the Child and Adult maps share each; every one is in the game's key list). Taking a token sets its Skulltula's key in the same scan (live check 2, the Kakariko tree: `0x6a8d9b2373d08fcb` is that placement). Before, an overworld Skulltula's key was shared only once a game had seen it loaded.
- **The token count is guarded by those keys** (`FlagWatcher::guard_count`). A token gain in our own game is sent only against one of the 100 keys going 0 to 1 in our own game, within `kGuardHoldFrames` (90 frames) before or after it. A gain with none, while one of the keys was written 0 to 1 here for another player (live, not the join snapshot) within the last `kGuardRecentFrames` (5 minutes at 60 frames a second), is our game counting again a token another player took: the count is put back to the group's and nothing is sent. The log says `coop: flags: count 0x04735978 +1 here was a pickup another player had taken: put back, not sent`. A gain with no take anywhere (the cheat window's give, an unknown source) is sent after the hold, as before. If the world sync gives the same token back as well (both took it at once), that give-back cancels against the held or put-back gain, so the count drops once only. The host does the same: its answers and totals leave a held gain out.

What is left to see in a game: that the token is gone in B's game (world sync), and if it is not, that B's count goes back after taking it (the log line above). Unit tests: "token guard: ..." in `tests/host/flag_watcher_test.cpp`.

**Heart containers (issue 88).** `PlayerStatus.MaxLife` is shared with the max rule, so a raised value should reach every game; what is not known is whether the game draws the hearts from that flag or from Link's own Life component (its max at `[Life + 0x1c18] + 8`). Prepared for one run:

- The walk logs `coop: objects: our Link's life L of M, MaxLife flag F (quarter hearts)` each time M or F changes, so B's log shows whether F rose after A's container and whether M followed.
- `LICHII_COOP_DEBUG_FLAGS=1` logs the int flag change itself.
- The walk writes F into the component's max when F is higher (at most 80 quarters; never lowers it), and logs `our Link's max life M -> F (from MaxLife)`. On by default since branch `hearts-default`; `LICHII_COOP_HEARTS_TO_LINK=0` turns it off. It runs in the object walk, so it needs `LICHII_COOP_OBJECTS=1` (on in the kit).

The run: both games with `LICHII_COOP_OBJECTS=1 LICHII_COOP_DEBUG_FLAGS=1`, A raises MaxLife (the cheat window's heart container button, or a real container), then read B's log and screen. If F rises and M does not and B's hearts stay: run again with `LICHII_COOP_HEARTS_TO_LINK=1` on B; if B's hearts then grow, make that the default. Done: the integrate-p4 run (autotest `hearts`, the cheat's MaxLife 16 -> 20) showed that without the switch B's game wrote 16 back within 1 s, and with it B logged `our Link's max life 16 -> 20 (from MaxLife)` and the flag stayed 20. So it is on by default now. Not run yet: a real heart container pickup, and four real heart pieces (does the game raise MaxLife when the fourth makes a heart?). Heart pieces stay as they are: the count is shared, and four pieces become a heart only in the pickup itself (live check above).

**A max you cannot keep does not echo (2026-11-11).** When nothing raises Link's own hearts (the object walk off, or `LICHII_COOP_HEARTS_TO_LINK=0`), the game writes its own lower `MaxLife` back. That drop used to be sent; the host keeps the higher of the two (max rule) and answered with it, the game lowered it again, and the two went round for ever: in a two-game run, after the host loaded a save with one more heart, B's hearts went to 4 and back many times a second with the heal sound and a "got a Heart Container" notice in bursts of 4 every 3 s. Now a drop of any max-rule int (`FlagStore::is_max`) that the game makes by itself is kept in that game and never sent (it could not win anywhere: every store keeps the higher value), and the notice is said once per new maximum (a join or a host's save sets the maximum again). Scenario `maxlife-echo` checks it (issue 186).

## Chests (branch `chest-sync`, issues 87 and 102)

Found by Lixyas in a two-player test (2026-11-10): the Deku Tree map reached both players, but the chest stayed closed in the other game, and the other player could open it again. In that build no chest's key was shared: the dungeons' chest keys came in the next one (`CoopDungeon::Keys()`, [dungeon.md](dungeon.md)).

**What a chest is.** Read from the romfs (`Obj_TBox_A_01`, `_A_02`): the opened state is the 64-bit key of the chest's placement (`ResetTypeValue` 24, saved). The actor has `ActorGameDataSetting` `AlwaysRespawn` and a blackboard Bool `IsOpened` that is not an instance parameter, so a chest takes its open state from the key when it is made. Writing the key into a game does not open a chest that game has loaded already. Where the blackboard sits in this build's memory was not read, so the hook cannot set `IsOpened` itself.

**What the hook does now.**

- **What a real opening sets (issue 87).** Opening a chest does not set its placement's 64-bit key: the TreasureBox AI (`AI/TreasureBox.module` in `Pack/AI.Global`) looks up its instance id in the GameData struct `0xce0c4dfe`, whose member is Murmur3 of the placement hash written in decimal and whose value is a saved (reset type 24) Bool, the chest's own "opened" flag. All 206 members are chests. That is why players saw nothing cross: the lists below share the 64-bit keys, which only the autotest's debug write ever set (integrate-p7's deku-map); the Graveyard's flower-grave chest, opened by hand, set exactly its Bool `0x39e4b7ec` and no key ([flags.md](flags.md)). `CoopChests::OpenedFlags()` now lists the 194 chests' opened flags (the 12 shop chests left out), with each chest's placement key. They are shared one way (`FlagWatcher::share_pickup_named`, the pickup rule: a 1 goes to everyone, a 0 is never sent nor written over a 1), and a flag written 0 to 1 for another player is noted like a key, so the hook (`CoopChests::KeyOfOpened`) hands that chest's placement key to the objects walk below.
- **Shown open, not removed (issue 195).** Autotest logs settled what the earlier rule waited for: a loaded chest's AI does **not** react to the shared opened flag (B's chest kept `CloseWait` until our remove, `chest-notice`, `deku-ladder`, `deku-scrubs` runs), so every chest the other player opened was deleted in front of the player, and Caleb's play-test saw chests vanish. Now the hook plays the chest's own open idle `OpenWait` on it (the puppets' play request, `PlayWish`), played again only over its closed idle (`only_over` "Close") so its own opening is never fought. Log: `... shown open here (OpenWait played)`, then `... shows open here`. A chest the game made open already (`OpenWait` when its flag comes) is left: `... already shows open here` (the old rule deleted those too). Only a chest that never shows `OpenWait` within 3 s (`kChestShowFailUs`) is still removed: `... could not be shown open here, removed`. That clock starts again on every frame CoopGuard holds the play back (our Link in an event near it), so a long event never turns into a remove. If its AI puts `CloseWait` back after it showed open, the Mover plays `OpenWait` again on the next game frame (it checks every chest play each frame), so the lid is closed for at most one frame; the log counts it (`... the lid may flicker`), and the autotest `chest-other` fails on it. When the room loads again, the chest is made from its shared flag, open, and nothing is played on it. Not known yet: whether the chest's AI still lets our Link open a chest shown open this way (`... opened here too after another game ... N this session`, `Sync::ChestsOpenedTwice`). If it does, its content is not counted twice: the hook tells the flag watcher (`CoopObjects::TakeChestsOpenedTwice`, `Session::expect_doubled_gain`), which puts this game's count gain (a small key, a heart piece; not the guarded token count) back to the group's value and sends nothing, but only inside that chest's own get scene: the hook ends it (`Session::end_doubled_gain`) as soon as our Link's event ends, or after 1 s if no event started (the opening was not ours), and 5 s (`kDoubledGainFrames`) is a cap. There is no table of what a chest holds; the scene bounds it instead: it holds our Link, so the only count gain inside it is the chest's own. A map, compass or rupee chest gives none, and a key taken after its scene counts as usual. Log: `coop: flags: a chest another player had opened was opened here too: this game's count gain in its get scene is taken back`, then `coop: flags: count 0x<hash> +1 here was a pickup another player had taken: put back, not sent`. The map and compass are one way and shared anyway; rupees are each player's own.
- **Every saved chest is shared.** The dungeons' 140 chests were already shared. The 60 chests outside the dungeons are now listed in `lichii/hook/coop_chests.cpp` (generated from the Banc files and GameDataList: Gyaml with `TBox`, key of reset type 24). The 12 chests of the Treasure Chest Shop minigame (Market, `Obj_HrlCity_Shop_TBox_*`) are left out, since each player plays that for themselves. The keys are one way, like the pickups: a key never goes back to 0 through the session. `LICHII_COOP_CHESTS=0` turns this list off.
- **A loaded chest that another player opened is removed.** The flag watcher notes each 64-bit key it writes 0 to 1 for another player (`FlagWatcher::take_written_for_others`). A key the game had at 1 already is not noted: the player who opened the chest gets the host's echo of it, and their chest stays. The hook hands the noted keys to the objects sync (`CoopObjects::TakenForOthers`), and the actors with that placement hash that are loaded in the next two walks are deleted. The log says `coop: objects: <actor> <key>: taken or opened in another game, removed here`. So the chest cannot be opened twice, and it shows open the next time its room loads.
- **Not beside our Link, except a chest.** A pickup within 3 m of our own Link is left: the player may be taking it too. A chest has no near rule: beside our Link it is shown open like any other (issue 195; issue 87 removed it at once, which is what players saw vanish). An opening counts only as a **change** of the chest's animation to one with `Open` in it: within 2 s before the take (`kChestOpeningUs`), or after the chest was bound. The name alone never counts. In integrate-p8 an untouched closed chest was taken for "both at once" and stayed closed, since its idle animation has `Open` in its name. Such an opening seen while our Link is not beside the chest is the chest's own reaction to the flag, and the chest is kept. Only when ours started opening just before the flag came, with our Link beside it (both opened it at once), does it stay with `... opened here and in another game at once (its content may count twice)`: that double count cannot be taken back here. The log says `... left here: our Link is beside it` for a pickup.
- **Only things that go away (issue 138).** The remove step takes every 64-bit key written for another player, and the dungeons' keys are more than chests. So only these actors are removed: chests, pickups (rupees, hearts, heart pieces and containers, silver rupees, Gold Skulltulas and tokens), spider webs, red and blue ice, cracked walls and floors, broken rocks and the Deku Tree's falling floors (`CoopObjects::RemovedWhenTaken`; the Shadow Temple's falling floor stays as ground once fallen, so it is kept). Enemy and silver rupee counters, the Fire Temple's hammer pillars, the Well's torches and waterfalls, the Forest Temple's paintings and enemies keep their actor: they change state instead of going away, and removing them could break a room (a hammer pillar is the path). Their key still crosses, so the room shows them done when it loads again. A removed one that has a life (a bomb web, a cracked wall, a rock or crate) first gets life 0, so the game plays its own break; it is deleted only if it still stands 1.5 s later (branch breakables-2; before, a web burnt in one game just vanished in the other). One with no life (the Deku Tree floor web) is deleted at once. Not checked in a game: that a small key chest kept by the 3 m rule gives no second key when opened again (the key count is shared by delta, so a second +1 would count for everyone).
- **Only while the walks run.** Keys written while no objects walk ran are not used. That covers the title screen, a save or scene load, and so the join snapshot. Any gap of more than 1.5 s counts, and so does a scene change. The actors loaded after those keys were made knowing them. The log says `N keys written for others while no walk ran: nothing removed for them`.
- **Only those.** An actor with that hash that loads later, or after a scene change, is left alone: the game made it knowing the key, so a chest shows open. Before this branch every key seen at 1 in a flag message stayed in the list for the whole run. That included the join snapshot and the host's echo of one's own chest. The hook then deleted the chest each time its room loaded, even when it was open, and could delete a player's own chest while it was opening.
- The same rule now covers the other one-time pickups that go through the flag watcher: Gold Skulltulas, tokens, heart pieces and containers, and silver rupees.

**Not known until a run:** whether the game's delete call takes a chest away cleanly (it has `AlwaysRespawn`). If it refuses, the chest stays and could be opened twice. Not known either: whether a chest whose key is written while its room is loading shows open.

## Loot: one rule for every drop (issue 102)

Lixyas (2026-11-10): enemy loot is shared (one player takes a Deku Stick and it disappears for the other), but bush loot is not (each can take it).

The rule is the same for every drop, whatever left it (an enemy, a pot, a bush, grass, a rock): **one roll for all games, and one pickup takes it away in every game.** Every drop actor in `CoopDrops` goes through the same path ([objects.md](objects.md), "Drops"): `DROP_SPAWN` when it appears, one roll kept per thing, `DROP_TAKE` when a player picks it up. Bushes (`Obj_Plant_Grass_*`) and grass are world breakables ([world.md](world.md)), so they already break in every game. `Obj_Heart_InWater` and `Obj_Heart_Shootable` were missing from the drop list and are in it now. Placed pickups (with a placement hash) stay flags, as before.

Why a bush drop could still be taken twice was not found in the code. Lixyas's game was an older build (2026-11-10), and the in-game check below should show it. Two things in the logs would explain it. One is two `sent` lines for the same bush, one from each game, that did not become one roll (`kept as another's` or `own rolls deleted` stays 0). The other is a pickup with no `picked up here` line, so it was taken as an unload or a blink-out further than 2.5 m away.

## The fishing pond (issue 151)

Testers (2026-11-11): the fishing pond did not work with several players. Read from the romfs (no run): the pond is part of the Hyrule Field Banc (Lake Hylia prefabs): 18 `Animal_Fish_Bass`, one `Animal_Fish_Loach`, 11 `Animal_Fish_Blue`, the owner `Npc_Hylia_Fishman_001_Child`/`_Adult`, five `Obj_SinkingLure` and a `MiniGame_Supporter`. The game runs it from the `MiniGame_Fishing` event flows and `Logic/MiniGame_Fishing_*` graphs. The rod and lure (`Weapon_FishingRod`, `Weapon_Lure`) and the owner's hat were never shared.

- **What broke.** Fish (`Animal_`) and the owner (`Npc_`) were shared as copies with one owner game ([objects.md](objects.md)). In the other game each fish was moved and animated from the owner's samples, so it could not follow or bite that player's lure, and a hooked fish was pulled back to where the owner's copy was; the owner's talk and hand-over animations came from the other game.
- **First fix (branch `fishing`): each game ran its own pond.** Testers then saw the fish follow the host only and ignore other players' lures, and asked for every fish shared, every lure working on every fish, and everyone seeing the other lures and catches. So since fishing-2:
- **The owner is each game's own** (`CoopObjects::IsOwnPuzzle`: `Npc_Hylia_Fishman*`): each player pays, takes the rod and is judged in their own game's events.
- **Every fish is shared** (`CoopObjects::IsFish`: `Animal_Fish_*`), with one owner game at a time, like any animal ([objects.md](objects.md), "Fish and lures"). The game that owns a fish runs it; the others show it where the owner's is.
- **Any lure works on any fish.** A fish goes where its owner game's AI sends it, and only a game's own lure is real there. So each game tells the server how near its lure is to every fish (`FISH_LURE`, fishing-3), and the server gives each fish to the game whose lure is nearest (or that the fish swims at), before the fish reaches it; that game runs the approach, the bite, the fight and the reel-in. The owner keeps a hooked fish, so it is never taken away mid-fight, and the players' places do not move fish. Two players fish at once: each one's fish are its own game's, each catch is the catcher's.
- **Everyone sees the other lures.** A cast lure (`Weapon_Lure`, `Weapon_SinkingLure`) is shared by the spawn sync; the copy is a model-only stand-in (`CoopShot_Lure`, `CoopShot_SinkingLure`), so no fish in another game can see or bite it. Without the stand-in in the puppet pack no lure copy is made at all (never the game's own lure). The line (`Weapon_FishingLine`, a physics rope) is not copied.
- **One catch per fish.** A fish its owner lands leaves that game's walk in Link's hands (within 3 m of Link): `OBJECT_REMOVE` reason dead, and every other game deletes its copy at once. The server keeps it as a kill for games that come later, and a game that loads the pond again brings the fish back for everyone (as a room's enemies come back).
- **Every fishing flag is saved per player** (all reset type 8 in `GameDataList`, checked by hash) and none was shared: the catch records `MiniGame_Fishing_RecordFishSize_Child` (default 40) and `_Adult` (45), `MiniGame_Fishing_OnceCatchFish`, `_SpawnedLoach`, `_IsRevenged`, `_Rainy`, `_PlayCountUntileRainy`, the tutorial flags, `IsGet.Item_FishingRod`, `IsGet.Weapon_SinkingLure`, `Npc_Hylia_Fishman_TakeOffHat`/`_SinkHat`. They stay each player's own.
- **Only the prizes are shared.** The heart piece (count, delta) and the Golden Scale (`IsGet.Item_Scale_Gold`, max) already were. New: `MiniGame_Fishing_GetHeartPiece` (0xf1891f6e) and `MiniGame_Fishing_GetGoldScale` (0x2079723b), rule any: once one player wins a prize, every game knows it was given, so the owner does not give it again and the shared heart-piece count is not raised twice.
- **Both win at the same moment.** If A and B each land a prize fish before the other's `GetHeartPiece` arrives (within one flag round trip), both owners give the heart piece and the shared count goes up by 2 (it is a delta). The Golden Scale is harmless (max). Rare enough to leave as it is.

In a game (integrator), both players at the pond, the in-game checks of `reports/fishing-2.md`; the first fix's checks were: (1) A and B each pay, take the rod and fish at the same time: each lands a fish; no `objects: share` line names `Npc_Hylia_Fishman` in either log. (2) A wins the heart piece (child, 10 lb or more): A's log sends `MiniGame_Fishing_GetHeartPiece`, B's takes it, the heart-piece count goes up once in both; B then catches a big fish and is not given a second heart piece. (3) The records stay each player's own (B's record is not A's).
