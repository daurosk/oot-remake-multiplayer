# Items and inventory

Where Ocarina of Time keeps the player's items, and which of them co-op shares. The values are flags in the game's flag store ([flags.md](flags.md)): every name below is a flag name, its hash is the Murmur3 x86 32-bit hash with seed 0, and `GameFlags` reads and writes it.

**Status.** Every name and hash on this page is checked: each one was hashed and found in `GameData/GameDataList.Product.110.byml.zs` with the type shown, by walking the list's `Struct` entries (a struct member is `{member name hash, child flag hash}`, and child = `Parent.Member`). Member names came from strings in `main` and the RomFS. **Nothing here is confirmed in a run yet**, including what each value means (`StockNum` 1 = owned is inferred from the name and from `PouchData.Item_Token_Gold.StockNum` being read as a count by the `Dm_SY_Get` event flow).

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
| Bottle contents | `PlayerStatus.AssignableItem.BottleInfo.Bottle<Aâ€“E>.BottleItem` (Enum: Empty, RedPotion, GreenPotion, BluePotion, BlueFire, Fish, Letter, BigPoe, Poe, Bug, Fairy, MaxMilk, HalfMilk) and `.IsValid` (Bool) | Enum, Bool |
| Dungeon items | `DungeonInfo.<dungeon>.IsGetMap`, `IsGetCompass`, `IsGetBossKey` (Bool), `SmallKeyNum` (Int), table below | Bool, Int |

Bottle hashes: A `IsValid` 0xf39ce410 / `BottleItem` 0x7d48df76; B 0x000ff6ea / 0x1edd164a; C 0xf63b1194 / 0xb80c70a8; D 0xb7f052ce / 0xe4c1b1fc; E 0xe27b136d / 0x365e1953.

Song hashes (`IsGet.Item_Melody_â€¦`): ZeldaLullaby 0x01c20002, EponaSong 0xb492ac33, SariaSong 0xd3f8bfcc, SunSong 0x165dd1ad, TimeSong 0x03b3450e, StormSong 0x2b39dd4f, ForestMinuet 0xdc0a0b54, FireBolero 0x49041ab9, WaterSerenade 0x3fa63986, ShadowNocturne 0xc2fb95f4, SpiritRequiem 0x802a36a8, LightPrelude 0xd6320ba4, ScarecrowSong 0x5e86fb4d.

Not part of the inventory: `PlayerStatus` also holds position, retry and UI state. Two unnamed structs look item-related and were not needed: 0xf5ab6461 (UInt per assignable item and bottle, perhaps a get order for the menu) and 0xf80f8077 (Int for Rupee, Bomb, Arrow, DekuStick, DekuNut and one more, perhaps capacities). Both are open.

## Rules

| Rule | Meaning | Used for |
|---|---|---|
| Shared, max | Everyone takes the highest value anyone has | Pouch items that are owned or not, and the upgrade tiers |
| Shared, OR | True for everyone once true for anyone | `IsGet.*`, songs, map, compass, boss key, `IsDefenseUP`, a bottle being owned |
| Shared, mirror | The last change anyone makes is copied to everyone | Trade sequence items (egg, cucco, mushroom, saw, â€¦): handing one over takes it away, so max would give it back |
| Count (delta) | A change by one player (+1 got, âˆ’1 used) is added to everyone's value | Small keys, Gold Skulltula count, heart pieces |
| Per player | Never sent | Rupees, ammo and other consumables, life, magic now, bottle contents, equipped gear, item buttons |

Choices made here:

- **Small keys** use the delta rule per dungeon (as Anchor does): one player opening a door spends the key for everyone, and a key picked up counts for everyone. The chest itself is a 64-bit-key flag ([flags.md](flags.md)) and must be shared too, or the key could be taken twice.
- **Heart containers and pieces** are key items. `MaxLife` uses max; `Obj_Heart_Piece.StockNum` uses the delta rule. Whether the game turns four pieces into a container by raising `MaxLife` and resetting the count is not known: check it live before turning the heart rules on. `Life` stays per player.
- **Bottles**: `PouchData.Bottle<Aâ€“E>.StockNum` and `BottleInfo.Bottle<X>.IsValid` are shared (OR); `BottleItem` stays per player. A bottle given to a player who did not have it arrives empty.
- **Ammo**: the bag, quiver and wallet tiers are shared (max); the count in them is per player.
- **Magic**: `MPMax` and the spells are shared (max); `MP` is per player.
- `FairyBless_*` items are per player for now: they are temporary blessings, not owned items (inferred).

## Pouch items

Each row: `PouchData.<item>.StockNum` and `IsGet.<item>` (â€” when there is none).

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
| `Weapon_Shield_Deku` | 0x7ca64f23 | 0x5217da1e | Shared, max |
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

## Not done

1. `GameItems` in `coop_host` (read and write these flags with the rules above, tests on a fake manager).
2. A live check: rupees and one key item changing in a run.
3. What `StockNum` holds for an owned item (1?) and how heart pieces turn into containers.
4. The unnamed structs and dungeon members above.
