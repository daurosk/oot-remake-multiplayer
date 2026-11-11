#include "coop/host/game_items.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace coop::host {
namespace {

using K = protocol::FlagKind;
using R = ItemRule;

// docs/items.md, sorted by hash. Every name is checked against its hash by
// tests/host/game_items_test.cpp.
constexpr ItemFlag kItems[] = {
  {"PlayerStatus.AssignableItem.BottleInfo.BottleB.IsValid", 0x000ff6ea, K::boolean, R::any, true},
  {"PouchData.Armor_Adult_Gauntlet_Silver.StockNum", 0x0012b8c0, K::int32, R::max, true},
  {"IsGet.Item_NayruLove", 0x00b44241, K::boolean, R::any, true},
  {"IsGet.Item_DekuNut", 0x00c2ee37, K::boolean, R::any, true},
  {"PouchData.Item_Medal_Water.StockNum", 0x0151a68f, K::int32, R::max, true},
  {"PouchData.Item_PocketCucco.StockNum", 0x0165e7ca, K::int32, R::mirror, true},
  {"IsGet.Item_Melody_ZeldaLullaby", 0x01c20002, K::boolean, R::any, true},
  {"IsGet.Item_Melody_TimeSong", 0x03b3450e, K::boolean, R::any, true},
  {"PouchData.Item_Token_Gold.StockNum", 0x04735978, K::int32, R::count, true},
  {"DungeonInfo.InsideTheDekuTree.IsGetCompass", 0x06c51ff2, K::boolean, R::any, true},
  {"IsGet.Armor_Child_Mask_Amiibo0", 0x06f13bb2, K::boolean, R::any, true},
  {"PouchData.Item_OddMushroom.StockNum", 0x07de3e6f, K::int32, R::mirror, true},
  {"PouchData.Item_PocketEgg.StockNum", 0x07f10aaf, K::int32, R::mirror, true},
  {"PouchData.Weapon_Shield_Hylian.StockNum", 0x08befd3a, K::int32, R::max, true},
  {"DungeonInfo.InsideTheDekuTree.SmallKeyNum", 0x0914ba07, K::int32, R::count, true},
  {"IsGet.Pouch_Range_BowAndArrow_Ice", 0x0989e62d, K::boolean, R::any, true},
  {"IsGet.Armor_Child_Mask_Amiibo1", 0x09df9d25, K::boolean, R::any, true},
  {"IsGet.Item_Quiver_Small", 0x0a550531, K::boolean, R::any, true},
  {"DungeonInfo.FireTemple.IsGetCompass", 0x0aa068b6, K::boolean, R::any, true},
  {"PouchData.Item_Medal_Spirit.StockNum", 0x0c7a5fd0, K::int32, R::max, true},
  {"IsGet.Item_Token_Gerudo", 0x0e6264c7, K::boolean, R::any, true},
  {"PouchData.Armor_Child_Mask_Gerudo.StockNum", 0x0e94a8df, K::int32, R::max, true},
  {"IsGet.Weapon_Range_SlingShot", 0x0ec22ee1, K::boolean, R::any, true},
  {"IsGet.Item_Medal_Fire", 0x103a0dc7, K::boolean, R::any, true},
  {"IsGet.Armor_Adult_HoverBoot", 0x1109c920, K::boolean, R::any, true},
  {"IsGet.Item_Stone_Forest", 0x110b83ec, K::boolean, R::any, true},
  {"IsGet.Item_Scale_Silver", 0x1208fb3e, K::boolean, R::any, true},
  {"PouchData.Item_Scale_Silver.StockNum", 0x1240044b, K::int32, R::max, true},
  {"PlayerStatus.MPMax", 0x12d63a97, K::int32, R::max, true},
  {"IsGet.Item_Medal_Shadow", 0x12d63ac6, K::boolean, R::any, true},
  {"DungeonInfo.InsideJabuJabuBelly.SmallKeyNum", 0x1470a816, K::int32, R::count, true},
  {"IsGet.Item_AlarmCucco", 0x14909166, K::boolean, R::any, true},
  {"IsGet.Item_Melody_SunSong", 0x165dd1ad, K::boolean, R::any, true},
  {"DungeonInfo.SpiritTemple.IsGetBossKey", 0x1984c03f, K::boolean, R::any, true},
  {"PouchData.Item_NutBag_Small.StockNum", 0x19b80dbc, K::int32, R::max, true},
  {"PouchData.Item_Hammer.StockNum", 0x1aafdb66, K::int32, R::max, true},
  {"PouchData.Item_Wallet_Small.StockNum", 0x1d74617e, K::int32, R::max, true},
  {"IsGet.Item_Ocarina_Time", 0x1da2c511, K::boolean, R::any, true},
  {"DungeonInfo.FireTemple.SmallKeyNum", 0x1df11487, K::int32, R::count, true},
  {"DungeonInfo.ShadowTemple.IsGetCompass", 0x1e2feec5, K::boolean, R::any, true},
  {"IsGet.Weapon_Sword_Giant", 0x1e824efa, K::boolean, R::any, true},
  {"PlayerStatus.AssignableItem.BottleInfo.BottleB.BottleItem", 0x1edd164a, K::uint32, R::per_player, true},
  {"PouchData.Item_Medal_Shadow.StockNum", 0x204d2063, K::int32, R::max, true},
  {"PouchData.Pouch_Amiibo.StockNum", 0x20820479, K::int32, R::max, true},
  {"IsGet.Item_Quiver_Large", 0x21985b09, K::boolean, R::any, true},
  {"PouchData.Armor_Adult_Goron.StockNum", 0x21ecbd19, K::int32, R::max, true},
  {"PouchData.Item_Medal_Light.StockNum", 0x235c7d9a, K::int32, R::max, true},
  {"PouchData.Item_WeirdEgg.StockNum", 0x24596e8e, K::int32, R::mirror, true},
  {"DungeonInfo.IceCavern.IsGetBossKey", 0x275f4512, K::boolean, R::any, true},
  {"IsGet.Armor_Child_Mask_Zora", 0x280eb00e, K::boolean, R::any, true},
  {"PouchData.Item_BrokenSword.StockNum", 0x2815c82a, K::int32, R::mirror, true},
  {"PouchData.Pouch_Range_BowAndArrow_Light.StockNum", 0x28298287, K::int32, R::max, true},
  {"IsGet.Item_DekuSeed", 0x29888e1e, K::boolean, R::any, true},
  {"IsGet.Item_Melody_StormSong", 0x2b39dd4f, K::boolean, R::any, true},
  {"PouchData.Armor_Adult_HeavyBoot.StockNum", 0x2bad189a, K::int32, R::max, true},
  {"IsGet.Item_Bomb", 0x2bb00768, K::boolean, R::any, true},
  {"IsGet.Item_NutBag_Small", 0x2bedd82e, K::boolean, R::any, true},
  {"PouchData.Item_Wallet_Medium.StockNum", 0x2d2c874b, K::int32, R::max, true},
  {"DungeonInfo.WaterTemple.IsGetCompass", 0x2e82c8fe, K::boolean, R::any, true},
  {"IsGet.Weapon_Shield_Hylian", 0x2f817d54, K::boolean, R::any, true},
  {"DungeonInfo.InsideGanonCastle.IsGetBossKey", 0x2fd59293, K::boolean, R::any, true},
  {"PouchData.Item_StickBag_Small.StockNum", 0x2fe6ab1a, K::int32, R::max, true},
  {"PlayerStatus.Life", 0x31ab5580, K::int32, R::per_player, true},
  {"DungeonInfo.IceCavern.IsGetMap", 0x322f2a94, K::boolean, R::any, true},
  {"IsGet.Weapon_Sword_Biggoron", 0x33466812, K::boolean, R::any, true},
  {"IsGet.Pouch_Amiibo", 0x3506a557, K::boolean, R::any, true},
  {"IsGet.Armor_Child_Mask_Keaton", 0x35c60e1d, K::boolean, R::any, true},
  {"PlayerStatus.AssignableItem.BottleInfo.BottleE.BottleItem", 0x365e1953, K::uint32, R::per_player, true},
  {"IsGet.Item_Longshot", 0x38aea3bb, K::boolean, R::any, true},
  {"PouchData.Weapon_Sword_Giant.StockNum", 0x396a5c02, K::int32, R::max, true},
  {"DungeonInfo.GerudoFortress.SmallKeyNum", 0x3a28b6a4, K::int32, R::count, true},
  {"PouchData.Item_Quiver_Medium.StockNum", 0x3bb370e1, K::int32, R::max, true},
  {"DungeonInfo.GerudoTrainingGround.IsGetCompass", 0x3d9a2750, K::boolean, R::any, true},
  {"IsGet.Armor_Adult_Gauntlet_Gold", 0x3ef15ec4, K::boolean, R::any, true},
  {"IsGet.Item_Melody_WaterSerenade", 0x3fa63986, K::boolean, R::any, true},
  {"DungeonInfo.DodongoCavern.SmallKeyNum", 0x3fdbc3d0, K::int32, R::count, true},
  {"IsGet.Weapon_Sword_Kokiri", 0x405d8a0f, K::boolean, R::any, true},
  {"PouchData.Item_Arrow_Ice.StockNum", 0x40735a8d, K::int32, R::max, true},
  {"PouchData.Armor_Adult_Gauntlet_Gold.StockNum", 0x40ae1183, K::int32, R::max, true},
  {"PouchData.Item_StickBag_Medium.StockNum", 0x41db7ab5, K::int32, R::max, true},
  {"DungeonInfo.ForestTemple.IsGetBossKey", 0x427333a0, K::boolean, R::any, true},
  {"IsGet.Item_Ocarina_Fairy", 0x42bd296f, K::boolean, R::any, true},
  {"IsGet.Item_WeirdEgg", 0x44ab1487, K::boolean, R::any, true},
  {"PouchData.Armor_Child_Mask_Skull.StockNum", 0x47b2d775, K::int32, R::max, true},
  {"PouchData.Item_FaroreWind.StockNum", 0x47f2ca87, K::int32, R::max, true},
  {"PouchData.Armor_Child_Mask_Amiibo1.StockNum", 0x486c59a4, K::int32, R::max, true},
  {"IsGet.Item_BulletBag_Small", 0x488ca1be, K::boolean, R::any, true},
  {"PouchData.Item_StickBag_Large.StockNum", 0x48e53698, K::int32, R::max, true},
  {"IsGet.Item_Melody_FireBolero", 0x49041ab9, K::boolean, R::any, true},
  {"PouchData.Armor_GlassTruth.StockNum", 0x49d1c374, K::int32, R::max, true},
  {"DungeonInfo.WaterTemple.IsGetMap", 0x4a4402ab, K::boolean, R::any, true},
  {"PouchData.Item_Scale_Gold.StockNum", 0x4aeb140a, K::int32, R::max, true},
  {"DungeonInfo.GerudoTrainingGround.IsGetMap", 0x4b36041e, K::boolean, R::any, true},
  {"IsGet.Item_Arrow_Fire", 0x4db7eae0, K::boolean, R::any, true},
  {"IsGet.Armor_Adult_Zora", 0x4f5a929c, K::boolean, R::any, true},
  {"PouchData.Item_Arrow_Normal.StockNum", 0x518ec74b, K::int32, R::per_player, true},
  {"PouchData.Item_CojiroCucco.StockNum", 0x51d89ef0, K::int32, R::mirror, true},
  {"PouchData.Obj_Heart_Piece.StockNum", 0x51ec97b3, K::int32, R::count, true},
  {"IsGet.Weapon_Shield_Deku", 0x5217da1e, K::boolean, R::any, true},
  {"PouchData.BottleD.StockNum", 0x5324f9d3, K::int32, R::any, true},
  {"PouchData.Item_Ocarina_Time.StockNum", 0x54e2a3d1, K::int32, R::max, true},
  {"PouchData.Item_NormalBoot.StockNum", 0x55e804de, K::int32, R::max, true},
  {"PlayerStatus.IsDefenseUP", 0x56bbe07c, K::boolean, R::any, true},
  {"IsGet.Item_Arrow_Normal", 0x572701ea, K::boolean, R::any, true},
  {"PouchData.Item_OddPowder.StockNum", 0x58c9bc78, K::int32, R::mirror, true},
  {"IsGet.Item_Bombchu", 0x5ab49c8e, K::boolean, R::any, true},
  {"DungeonInfo.FireTemple.IsGetMap", 0x5b3ac0ba, K::boolean, R::any, true},
  {"PouchData.Item_EyeDrop.StockNum", 0x5c41a6b1, K::int32, R::mirror, true},
  {"IsGet.Item_Arrow_Ice", 0x5c8f3099, K::boolean, R::any, true},
  {"PouchData.Item_Medal_Fire.StockNum", 0x5ce254a6, K::int32, R::max, true},
  {"PouchData.Item_Arrow_Fire.StockNum", 0x5e529ab9, K::int32, R::max, true},
  {"PouchData.Item_FairyBless_MP.StockNum", 0x5e671d92, K::int32, R::per_player, true},
  {"IsGet.Item_Melody_ScarecrowSong", 0x5e86fb4d, K::boolean, R::any, true},
  {"DungeonInfo.GerudoFortress.IsGetCompass", 0x5eb80679, K::boolean, R::any, true},
  {"DungeonInfo.ShadowTemple.SmallKeyNum", 0x5edda9e3, K::int32, R::count, true},
  {"PouchData.Item_FairyBless_MPDouble.StockNum", 0x5f043856, K::int32, R::per_player, true},
  {"IsGet.Armor_Adult_HeavyBoot", 0x5fa190e6, K::boolean, R::any, true},
  {"PouchData.Item_DekuSeed.StockNum", 0x5fadf7fc, K::int32, R::per_player, true},
  {"PouchData.Item_ScrubBusinessSeed.StockNum", 0x600e29eb, K::int32, R::max, true},
  {"IsGet.Item_BrokenSword", 0x645a8175, K::boolean, R::any, true},
  {"DungeonInfo.InsideJabuJabuBelly.IsGetBossKey", 0x656bd5ee, K::boolean, R::any, true},
  {"IsGet.Armor_Child_Mask_Bunny", 0x65aebe17, K::boolean, R::any, true},
  {"PouchData.Pouch_Range_BowAndArrow_Ice.StockNum", 0x667c7ba3, K::int32, R::max, true},
  {"PouchData.Item_Medal_Forest.StockNum", 0x692d57cb, K::int32, R::max, true},
  {"IsGet.Weapon_Range_Bow", 0x69ec28be, K::boolean, R::any, true},
  {"IsGet.Item_Token_Gold", 0x6ba394fe, K::boolean, R::any, true},
  {"PouchData.Item_ZeldaLetter.StockNum", 0x6be5c3b0, K::int32, R::mirror, true},
  {"DungeonInfo.FinalBattleField.IsGetBossKey", 0x6d0fea11, K::boolean, R::any, true},
  {"DungeonInfo.GerudoTrainingGround.IsGetBossKey", 0x6d155841, K::boolean, R::any, true},
  {"IsGet.Item_MagicBean", 0x6d39c600, K::boolean, R::any, true},
  {"PouchData.Armor_Adult_Normal.StockNum", 0x6f631388, K::int32, R::max, true},
  {"IsGet.Weapon_Sword_BrokenGiant", 0x720df203, K::boolean, R::any, true},
  {"IsGet.Item_NormalBoot", 0x7215060c, K::boolean, R::any, true},
  {"IsGet.Item_Hammer", 0x7227efd8, K::boolean, R::any, true},
  {"IsGet.Weapon_Shield_Mirror", 0x75e94e42, K::boolean, R::any, true},
  {"PlayerStatus.MP", 0x76b18200, K::int32, R::per_player, true},
  {"DungeonInfo.BottomOfTheWell.IsGetMap", 0x77c47ec9, K::boolean, R::any, true},
  {"DungeonInfo.WaterTemple.IsGetBossKey", 0x79daf80a, K::boolean, R::any, true},
  {"DungeonInfo.SpiritTemple.SmallKeyNum", 0x79eeec8e, K::int32, R::count, true},
  {"IsGet.Weapon_Sword_Master", 0x7ae38e5b, K::boolean, R::any, true},
  {"PouchData.Item_BulletBag_Large.StockNum", 0x7b9673f5, K::int32, R::max, true},
  {"PouchData.Weapon_Shield_Deku.StockNum", 0x7ca64f23, K::int32, R::max, true},
  {"PlayerStatus.AssignableItem.BottleInfo.BottleA.BottleItem", 0x7d48df76, K::uint32, R::per_player, true},
  {"PouchData.Item_Stone_Fire.StockNum", 0x7d69ad47, K::int32, R::max, true},
  {"DungeonInfo.BottomOfTheWell.SmallKeyNum", 0x7dd79d10, K::int32, R::count, true},
  {"PouchData.Armor_Child_Mask_Bunny.StockNum", 0x7e607033, K::int32, R::max, true},
  {"IsGet.Item_Boomerang", 0x7f4ce38d, K::boolean, R::any, true},
  {"IsGet.Armor_GlassTruth", 0x7fe2a5b2, K::boolean, R::any, true},
  {"IsGet.Item_Melody_SpiritRequiem", 0x802a36a8, K::boolean, R::any, true},
  {"PouchData.Weapon_Range_SlingShot.StockNum", 0x80d34ce9, K::int32, R::max, true},
  {"PouchData.Item_Longshot.StockNum", 0x8499ea34, K::int32, R::max, true},
  {"PouchData.Obj_Armor_Child_Normal.StockNum", 0x84ba0d24, K::int32, R::max, true},
  {"PouchData.Item_Hookshot.StockNum", 0x8573d2cd, K::int32, R::max, true},
  {"IsGet.Item_Arrow_Light", 0x8637bab4, K::boolean, R::any, true},
  {"DungeonInfo.FinalBattleField.IsGetCompass", 0x874493ce, K::boolean, R::any, true},
  {"DungeonInfo.WaterTemple.SmallKeyNum", 0x877bdcee, K::int32, R::count, true},
  {"IsGet.Item_FairyBless_MPDouble", 0x87ca4826, K::boolean, R::any, true},
  {"IsGet.Armor_Adult_Gauntlet_Silver", 0x87e58de3, K::boolean, R::any, true},
  {"IsGet.Item_PoacherSaw", 0x888d5059, K::boolean, R::any, true},
  {"IsGet.Item_Stone_Fire", 0x8908d6ad, K::boolean, R::any, true},
  {"PouchData.BottleC.StockNum", 0x893216a1, K::int32, R::any, true},
  {"DungeonInfo.InsideTheDekuTree.IsGetMap", 0x89787395, K::boolean, R::any, true},
  {"PouchData.Item_Prescription.StockNum", 0x897cf7eb, K::int32, R::mirror, true},
  {"IsGet.Item_ClaimCheck", 0x8b54e680, K::boolean, R::any, true},
  {"DungeonInfo.DodongoCavern.IsGetCompass", 0x8b93edf9, K::boolean, R::any, true},
  {"PouchData.Weapon_Sword_BrokenGiant.StockNum", 0x8d02f5ea, K::int32, R::max, true},
  {"IsGet.Item_EyeballFrog", 0x8e02d1ca, K::boolean, R::any, true},
  {"DungeonInfo.ForestTemple.SmallKeyNum", 0x90237092, K::int32, R::count, true},
  {"IsGet.Item_Medal_Light", 0x9026c187, K::boolean, R::any, true},
  {"PouchData.Item_EyeballFrog.StockNum", 0x919ea324, K::int32, R::mirror, true},
  {"PouchData.Armor_Child_Mask_Zora.StockNum", 0x924e9a46, K::int32, R::max, true},
  {"PouchData.Weapon_Range_Bow.StockNum", 0x938d56bf, K::int32, R::max, true},
  {"DungeonInfo.BottomOfTheWell.IsGetBossKey", 0x9402715c, K::boolean, R::any, true},
  {"IsGet.Item_FairyBless_DefenseDouble", 0x944b78be, K::boolean, R::any, true},
  {"DungeonInfo.GerudoFortress.IsGetBossKey", 0x95fbcb7c, K::boolean, R::any, true},
  {"IsGet.Item_NutBag_Medium", 0x960820db, K::boolean, R::any, true},
  {"PouchData.Armor_Child_Mask_Truth.StockNum", 0x96459cd3, K::int32, R::max, true},
  {"IsGet.Item_BombBag_Large", 0x968a5a79, K::boolean, R::any, true},
  {"IsGet.Armor_Child_Gauntlet_Goron", 0x9880707e, K::boolean, R::any, true},
  {"DungeonInfo.InsideJabuJabuBelly.IsGetCompass", 0x9a2218db, K::boolean, R::any, true},
  {"PouchData.Item_BulletBag_Medium.StockNum", 0x9a30a9e5, K::int32, R::max, true},
  {"IsGet.Item_NutBag_Large", 0x9a5db0b6, K::boolean, R::any, true},
  {"PouchData.Item_FairyBless_DefenseDouble.StockNum", 0x9a826fab, K::int32, R::per_player, true},
  {"DungeonInfo.InsideGanonCastle.IsGetMap", 0x9bbc0c4a, K::boolean, R::any, true},
  {"DungeonInfo.SpiritTemple.IsGetCompass", 0x9bd1181c, K::boolean, R::any, true},
  {"DungeonInfo.BottomOfTheWell.IsGetCompass", 0x9d6efaac, K::boolean, R::any, true},
  {"IsGet.Item_Quiver_Medium", 0x9d872202, K::boolean, R::any, true},
  {"DungeonInfo.FinalBattleField.SmallKeyNum", 0xa04bfd48, K::int32, R::count, true},
  {"IsGet.Item_StickBag_Large", 0xa10f2dcf, K::boolean, R::any, true},
  {"PouchData.Item_Boomerang.StockNum", 0xa1d76d31, K::int32, R::max, true},
  {"DungeonInfo.DodongoCavern.IsGetMap", 0xa52c3a19, K::boolean, R::any, true},
  {"IsGet.Item_BombBag_Small", 0xa63f1e2d, K::boolean, R::any, true},
  {"PlayerStatus.CurrentRupee", 0xa77921d7, K::int32, R::per_player, true},
  {"PouchData.Item_PoacherSaw.StockNum", 0xa862d276, K::int32, R::mirror, true},
  {"PouchData.Item_Quiver_Small.StockNum", 0xa8c3e1fb, K::int32, R::max, true},
  {"IsGet.Armor_Child_Mask_Goron", 0xadfccd91, K::boolean, R::any, true},
  {"IsGet.Armor_Adult_Goron", 0xae5611af, K::boolean, R::any, true},
  {"PouchData.Item_Wallet_Large.StockNum", 0xafa4bb52, K::int32, R::max, true},
  {"PouchData.Item_MagicBean.StockNum", 0xb003124d, K::int32, R::per_player, true},
  {"IsGet.Pouch_Range_BowAndArrow_Light", 0xb03ae135, K::boolean, R::any, true},
  {"PouchData.Item_BombBag_Small.StockNum", 0xb0a1bf51, K::int32, R::max, true},
  {"PouchData.Weapon_Sword_Biggoron.StockNum", 0xb0b3e399, K::int32, R::max, true},
  {"IsGet.Item_FairyBless_MP", 0xb1309073, K::boolean, R::any, true},
  {"IsGet.Armor_Child_Mask_Gerudo", 0xb1fc4c1d, K::boolean, R::any, true},
  {"IsGet.Item_BombBag_Medium", 0xb2c478da, K::boolean, R::any, true},
  {"DungeonInfo.FinalBattleField.IsGetMap", 0xb3b65c1a, K::boolean, R::any, true},
  {"IsGet.Item_Melody_EponaSong", 0xb492ac33, K::boolean, R::any, true},
  {"IsGet.Item_Wallet_Large", 0xb5031190, K::boolean, R::any, true},
  {"PouchData.Armor_Child_Mask_Keaton.StockNum", 0xb5c10899, K::int32, R::max, true},
  {"IsGet.Item_BulletBag_Large", 0xb669f025, K::boolean, R::any, true},
  {"PouchData.Item_AlarmCucco.StockNum", 0xb6ca6093, K::int32, R::mirror, true},
  {"IsGet.Item_Wallet_Small", 0xb6e5a3a5, K::boolean, R::any, true},
  {"DungeonInfo.FireTemple.IsGetBossKey", 0xb72d73df, K::boolean, R::any, true},
  {"IsGet.Item_OddPowder", 0xb75acaad, K::boolean, R::any, true},
  {"PouchData.Item_DinFire.StockNum", 0xb796c16d, K::int32, R::max, true},
  {"IsGet.Item_EyeDrop", 0xb7bdd577, K::boolean, R::any, true},
  {"PouchData.Item_Ocarina_Fairy.StockNum", 0xb7df60f1, K::int32, R::max, true},
  {"PlayerStatus.AssignableItem.BottleInfo.BottleD.IsValid", 0xb7f052ce, K::boolean, R::any, true},
  {"PlayerStatus.AssignableItem.BottleInfo.BottleC.BottleItem", 0xb80c70a8, K::uint32, R::per_player, true},
  {"PouchData.Armor_Adult_Zora.StockNum", 0xb816af94, K::int32, R::max, true},
  {"DungeonInfo.SpiritTemple.IsGetMap", 0xb85002ba, K::boolean, R::any, true},
  {"PouchData.Item_Stone_Water.StockNum", 0xb8e3bf45, K::int32, R::max, true},
  {"PouchData.Item_DekuStick.StockNum", 0xb9494411, K::int32, R::per_player, true},
  {"DungeonInfo.GerudoFortress.IsGetMap", 0xbcaad6ce, K::boolean, R::any, true},
  {"PouchData.Item_BombBag_Large.StockNum", 0xbd3ca2c6, K::int32, R::max, true},
  {"PouchData.Item_Arrow_Light.StockNum", 0xbfb285c8, K::int32, R::max, true},
  {"IsGet.Armor_Child_Mask_Spooky", 0xc0cf68a7, K::boolean, R::any, true},
  {"IsGet.Item_Melody_ShadowNocturne", 0xc2fb95f4, K::boolean, R::any, true},
  {"DungeonInfo.ShadowTemple.IsGetBossKey", 0xc33c66cf, K::boolean, R::any, true},
  {"DungeonInfo.InsideGanonCastle.IsGetCompass", 0xc4d4734e, K::boolean, R::any, true},
  {"IsGet.Item_DekuStick", 0xc4e21ac2, K::boolean, R::any, true},
  {"PouchData.Armor_Adult_HoverBoot.StockNum", 0xc9220366, K::int32, R::max, true},
  {"PouchData.BottleE.StockNum", 0xc92b16be, K::int32, R::any, true},
  {"PouchData.Item_Stone_Forest.StockNum", 0xcbd1e58c, K::int32, R::max, true},
  {"IsGet.Item_OddMushroom", 0xcbd7852a, K::boolean, R::any, true},
  {"IsGet.Item_StickBag_Medium", 0xcbf00480, K::boolean, R::any, true},
  {"IsGet.Item_Scale_Gold", 0xccc1909a, K::boolean, R::any, true},
  {"IsGet.Item_Medal_Water", 0xcf64e780, K::boolean, R::any, true},
  {"IsGet.Item_ScrubBusinessSeed", 0xcfbb16f6, K::boolean, R::any, true},
  {"IsGet.Armor_Child_Mask_Skull", 0xd097482a, K::boolean, R::any, true},
  {"PouchData.Armor_Child_Gauntlet_Goron.StockNum", 0xd1317941, K::int32, R::max, true},
  {"IsGet.Item_Wallet_Medium", 0xd1360de8, K::boolean, R::any, true},
  {"PouchData.Item_Quiver_Large.StockNum", 0xd331c7da, K::int32, R::max, true},
  {"IsGet.Item_Melody_SariaSong", 0xd3f8bfcc, K::boolean, R::any, true},
  {"PouchData.BottleA.StockNum", 0xd53ce3a7, K::int32, R::any, true},
  {"PouchData.Item_NutBag_Large.StockNum", 0xd60e4b23, K::int32, R::max, true},
  {"IsGet.Item_Melody_LightPrelude", 0xd6320ba4, K::boolean, R::any, true},
  {"IsGet.Item_Prescription", 0xd8f1521e, K::boolean, R::any, true},
  {"PouchData.Item_Bomb.StockNum", 0xdb1bcd4f, K::int32, R::per_player, true},
  {"IsGet.Item_Melody_ForestMinuet", 0xdc0a0b54, K::boolean, R::any, true},
  {"PouchData.Item_BulletBag_Small.StockNum", 0xdc52acaf, K::int32, R::max, true},
  {"IsGet.Item_AgonyStone", 0xdd587ed8, K::boolean, R::any, true},
  {"IsGet.Item_StickBag_Small", 0xde2389d4, K::boolean, R::any, true},
  {"IsGet.Armor_Adult_Normal", 0xde513abd, K::boolean, R::any, true},
  {"PouchData.Item_ClaimCheck.StockNum", 0xded8e8d3, K::int32, R::mirror, true},
  {"PouchData.Item_Bombchu.StockNum", 0xdf7c517a, K::int32, R::per_player, true},
  {"IsGet.Item_Medal_Spirit", 0xe269df38, K::boolean, R::any, true},
  {"PlayerStatus.AssignableItem.BottleInfo.BottleE.IsValid", 0xe27b136d, K::boolean, R::any, true},
  {"PouchData.Pouch_Range_BowAndArrow_Fire.StockNum", 0xe32ad20f, K::int32, R::max, true},
  {"PlayerStatus.AssignableItem.BottleInfo.BottleD.BottleItem", 0xe4c1b1fc, K::uint32, R::per_player, true},
  {"PouchData.Item_BombBag_Medium.StockNum", 0xe5ff45aa, K::int32, R::max, true},
  {"IsGet.Obj_Heart_Piece", 0xe6aaab65, K::boolean, R::any, true},
  {"IsGet.Item_BulletBag_Medium", 0xe6cd55d2, K::boolean, R::any, true},
  {"DungeonInfo.ForestTemple.IsGetMap", 0xe704f9b4, K::boolean, R::any, true},
  {"IsGet.Item_PocketCucco", 0xe82e6413, K::boolean, R::any, true},
  {"DungeonInfo.IceCavern.IsGetCompass", 0xea1846e6, K::boolean, R::any, true},
  {"DungeonInfo.GerudoTrainingGround.SmallKeyNum", 0xea9786f5, K::int32, R::count, true},
  {"PouchData.Item_NutBag_Medium.StockNum", 0xeaa1785e, K::int32, R::max, true},
  {"IsGet.Item_PocketEgg", 0xec9c3136, K::boolean, R::any, true},
  {"PouchData.Weapon_Sword_Kokiri.StockNum", 0xeeb495b9, K::int32, R::max, true},
  {"IsGet.Item_Stone_Water", 0xeec383f5, K::boolean, R::any, true},
  {"PouchData.Armor_Child_Mask_Spooky.StockNum", 0xf046629e, K::int32, R::max, true},
  {"PouchData.Item_NayruLove.StockNum", 0xf0828cd0, K::int32, R::max, true},
  {"IsGet.Item_ZeldaLetter", 0xf19393b5, K::boolean, R::any, true},
  {"PouchData.Weapon_Sword_Master.StockNum", 0xf1c45294, K::int32, R::max, true},
  {"DungeonInfo.ForestTemple.IsGetCompass", 0xf240eeb2, K::boolean, R::any, true},
  {"PouchData.Item_DekuNut.StockNum", 0xf30d9f54, K::int32, R::per_player, true},
  {"PlayerStatus.AssignableItem.BottleInfo.BottleA.IsValid", 0xf39ce410, K::boolean, R::any, true},
  {"PouchData.Item_AgonyStone.StockNum", 0xf3b14ce2, K::int32, R::max, true},
  {"PouchData.Weapon_Shield_Mirror.StockNum", 0xf58e0e2f, K::int32, R::max, true},
  {"PouchData.Armor_Child_Mask_Amiibo0.StockNum", 0xf5e647da, K::int32, R::max, true},
  {"DungeonInfo.ShadowTemple.IsGetMap", 0xf6177b93, K::boolean, R::any, true},
  {"PlayerStatus.AssignableItem.BottleInfo.BottleC.IsValid", 0xf63b1194, K::boolean, R::any, true},
  {"DungeonInfo.InsideTheDekuTree.IsGetBossKey", 0xf6995cee, K::boolean, R::any, true},
  {"IsGet.Armor_Child_Mask_Truth", 0xf7d5bd9c, K::boolean, R::any, true},
  {"IsGet.Obj_Armor_Child_Normal", 0xf9594156, K::boolean, R::any, true},
  {"IsGet.Pouch_Range_BowAndArrow_Fire", 0xf9fe4ffc, K::boolean, R::any, true},
  {"IsGet.Item_DinFire", 0xfa04e611, K::boolean, R::any, true},
  {"DungeonInfo.IceCavern.SmallKeyNum", 0xfa5d133d, K::int32, R::count, true},
  {"DungeonInfo.DodongoCavern.IsGetBossKey", 0xfaabc084, K::boolean, R::any, true},
  {"IsGet.Item_CojiroCucco", 0xfb4442de, K::boolean, R::any, true},
  {"PlayerStatus.MaxLife", 0xfbe01da1, K::int32, R::max, true},
  {"DungeonInfo.InsideGanonCastle.SmallKeyNum", 0xfbfd2910, K::int32, R::count, true},
  {"PouchData.Armor_Child_Mask_Goron.StockNum", 0xfc508da7, K::int32, R::max, true},
  {"PouchData.BottleB.StockNum", 0xfca29a04, K::int32, R::any, true},
  {"DungeonInfo.InsideJabuJabuBelly.IsGetMap", 0xfcadd210, K::boolean, R::any, true},
  {"IsGet.Item_Medal_Forest", 0xfcd8d653, K::boolean, R::any, true},
  {"PouchData.Item_Token_Gerudo.StockNum", 0xfeb4d56b, K::int32, R::max, true},
  {"IsGet.Item_Hookshot", 0xff98ed58, K::boolean, R::any, true},
  {"IsGet.Item_FaroreWind", 0xffabf058, K::boolean, R::any, true},
};

std::int32_t as_int(std::uint32_t bits) { return static_cast<std::int32_t>(bits); }
std::uint32_t as_bits(std::int64_t value) {
  value = std::clamp<std::int64_t>(value, std::numeric_limits<std::int32_t>::min(),
                                      std::numeric_limits<std::int32_t>::max());
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

}  // namespace

std::span<const ItemFlag> item_flags() { return kItems; }

const ItemFlag* find_item(std::uint32_t hash) {
  const auto it = std::lower_bound(std::begin(kItems), std::end(kItems), hash,
                                   [](const ItemFlag& e, std::uint32_t h) { return e.hash < h; });
  return it != std::end(kItems) && it->hash == hash ? &*it : nullptr;
}

SharedFlags item_shared_flags(bool with_off) {
  std::vector<FlagKey> keys;
  for (const auto& item : kItems) {
    if (item.rule == ItemRule::per_player) continue;
    if (!item.on && !with_off) continue;
    keys.push_back(FlagKey{item.hash, 0});
  }
  return SharedFlags(std::move(keys));
}

std::uint32_t merge_item(ItemRule rule, protocol::FlagKind kind, std::uint32_t local,
                         std::uint32_t incoming, std::optional<std::uint32_t> base) {
  const bool signed_int = kind == protocol::FlagKind::int32;
  switch (rule) {
    case ItemRule::max:
      if (signed_int) return as_int(incoming) > as_int(local) ? incoming : local;
      return std::max(incoming, local);
    case ItemRule::any:
      // Owned once anyone owns it; an owned value is never replaced.
      return local != 0 ? local : incoming;
    case ItemRule::mirror:
      return incoming;
    case ItemRule::count:
      if (!base || !signed_int) return incoming;
      return as_bits(std::int64_t{as_int(local)} + as_int(incoming) - as_int(*base));
    case ItemRule::per_player:
      return local;
  }
  return local;
}

namespace {

// Ammo capacities (Int struct 0xf80f8077; member names Rupee, DekuSeed, Bomb,
// Arrow, DekuStick, DekuNut found by hashing them against the struct's
// member hashes in GameDataList). Default 0: no ammo of that kind is held.
constexpr std::uint32_t kCapSeed = 0xae8cc742, kCapBomb = 0xe753508b, kCapArrow = 0x418db791,
                        kCapStick = 0x457616b0, kCapNut = 0x735b00f7, kCapRupee = 0xe0010a50;
// Ammo counts (per player) and their IsGet flags (docs/items.md).
constexpr std::uint32_t kSeeds = 0x5fadf7fc, kSeedsGet = 0x29888e1e;
constexpr std::uint32_t kArrows = 0x518ec74b, kArrowsGet = 0x572701ea;
constexpr std::uint32_t kBombs = 0xdb1bcd4f, kBombsGet = 0x2bb00768;
constexpr std::uint32_t kNuts = 0xf30d9f54, kNutsGet = 0x00c2ee37;
constexpr std::uint32_t kSticks = 0xb9494411, kSticksGet = 0xc4e21ac2;

#define COOP_CAP(item, member, hash, n) {item, "0xf80f8077." member, hash, K::int32, n}
#define COOP_AMMO(item, name, hash, n) {item, "PouchData." name ".StockNum", hash, K::int32, n}
#define COOP_GOT(item, name, hash) {item, "IsGet." name, hash, K::boolean, 1}

// Sorted by item. A capacity's name is the struct hash and the member name:
// the struct's own name is not known, so its hash is not flag_hash(name).
constexpr ItemGrant kGrants[] = {
  // IsGet.Item_DekuNut: nuts were got once (a pickup), so there is room for
  // them; the first pickup is what gives it in the game (issue 138).
  COOP_CAP(kNutsGet, "DekuNut", kCapNut, 20),
  // Item_NutBag_Small: 20, the original's first nut capacity.
  COOP_CAP(0x19b80dbc, "DekuNut", kCapNut, 20),
  COOP_AMMO(0x19b80dbc, "Item_DekuNut", kNuts, 20),
  COOP_GOT(0x19b80dbc, "Item_DekuNut", kNutsGet),
  // Item_Wallet_Small: 200 (an adult's wallet, read from a save). No fill.
  COOP_CAP(0x1d74617e, "Rupee", kCapRupee, 200),
  // Item_Wallet_Medium: 500 (the original's giant's wallet). No fill.
  COOP_CAP(0x2d2c874b, "Rupee", kCapRupee, 500),
  // Item_StickBag_Small: 10.
  COOP_CAP(0x2fe6ab1a, "DekuStick", kCapStick, 10),
  COOP_AMMO(0x2fe6ab1a, "Item_DekuStick", kSticks, 10),
  COOP_GOT(0x2fe6ab1a, "Item_DekuStick", kSticksGet),
  // Item_Quiver_Medium: 40.
  COOP_CAP(0x3bb370e1, "Arrow", kCapArrow, 40),
  COOP_AMMO(0x3bb370e1, "Item_Arrow_Normal", kArrows, 40),
  // Item_StickBag_Medium: 20.
  COOP_CAP(0x41db7ab5, "DekuStick", kCapStick, 20),
  COOP_AMMO(0x41db7ab5, "Item_DekuStick", kSticks, 20),
  // Item_StickBag_Large: 30.
  COOP_CAP(0x48e53698, "DekuStick", kCapStick, 30),
  COOP_AMMO(0x48e53698, "Item_DekuStick", kSticks, 30),
  // Item_BulletBag_Large: 50.
  COOP_CAP(0x7b9673f5, "DekuSeed", kCapSeed, 50),
  COOP_AMMO(0x7b9673f5, "Item_DekuSeed", kSeeds, 50),
  // Weapon_Range_SlingShot: the small bullet bag and 30 seeds.
  COOP_CAP(0x80d34ce9, "DekuSeed", kCapSeed, 30),
  COOP_AMMO(0x80d34ce9, "Item_DekuSeed", kSeeds, 30),
  COOP_GOT(0x80d34ce9, "Item_DekuSeed", kSeedsGet),
  // Weapon_Range_Bow: the small quiver and 30 arrows.
  COOP_CAP(0x938d56bf, "Arrow", kCapArrow, 30),
  COOP_AMMO(0x938d56bf, "Item_Arrow_Normal", kArrows, 30),
  COOP_GOT(0x938d56bf, "Item_Arrow_Normal", kArrowsGet),
  // Item_BulletBag_Medium: 40.
  COOP_CAP(0x9a30a9e5, "DekuSeed", kCapSeed, 40),
  COOP_AMMO(0x9a30a9e5, "Item_DekuSeed", kSeeds, 40),
  // Item_Quiver_Small: 30.
  COOP_CAP(0xa8c3e1fb, "Arrow", kCapArrow, 30),
  COOP_AMMO(0xa8c3e1fb, "Item_Arrow_Normal", kArrows, 30),
  // Item_BombBag_Small: 20.
  COOP_CAP(0xb0a1bf51, "Bomb", kCapBomb, 20),
  COOP_AMMO(0xb0a1bf51, "Item_Bomb", kBombs, 20),
  COOP_GOT(0xb0a1bf51, "Item_Bomb", kBombsGet),
  // Item_BombBag_Large: 40.
  COOP_CAP(0xbd3ca2c6, "Bomb", kCapBomb, 40),
  COOP_AMMO(0xbd3ca2c6, "Item_Bomb", kBombs, 40),
  // IsGet.Item_DekuStick: sticks were got once (issue 138: a player could
  // not pick up sticks after the other's first one).
  COOP_CAP(kSticksGet, "DekuStick", kCapStick, 10),
  // Item_Quiver_Large: 50.
  COOP_CAP(0xd331c7da, "Arrow", kCapArrow, 50),
  COOP_AMMO(0xd331c7da, "Item_Arrow_Normal", kArrows, 50),
  // Item_NutBag_Large: 40.
  COOP_CAP(0xd60e4b23, "DekuNut", kCapNut, 40),
  COOP_AMMO(0xd60e4b23, "Item_DekuNut", kNuts, 40),
  // Item_BulletBag_Small: 30.
  COOP_CAP(0xdc52acaf, "DekuSeed", kCapSeed, 30),
  COOP_AMMO(0xdc52acaf, "Item_DekuSeed", kSeeds, 30),
  // Item_BombBag_Medium: 30.
  COOP_CAP(0xe5ff45aa, "Bomb", kCapBomb, 30),
  COOP_AMMO(0xe5ff45aa, "Item_Bomb", kBombs, 30),
  // Item_NutBag_Medium: 30.
  COOP_CAP(0xeaa1785e, "DekuNut", kCapNut, 30),
  COOP_AMMO(0xeaa1785e, "Item_DekuNut", kNuts, 30),
};

#undef COOP_CAP
#undef COOP_AMMO
#undef COOP_GOT

constexpr bool is_capacity(const ItemGrant& g) { return g.name.starts_with("0xf80f8077."); }

struct GrantLess {
  bool operator()(const ItemGrant& a, std::uint32_t b) const { return a.item < b; }
  bool operator()(std::uint32_t a, const ItemGrant& b) const { return a < b.item; }
};

}  // namespace

std::span<const ItemGrant> all_item_grants() { return kGrants; }

std::span<const ItemGrant> item_grants(std::uint32_t item) {
  const auto [first, last] =
      std::equal_range(std::begin(kGrants), std::end(kGrants), item, GrantLess{});
  return {first, last};
}

void ItemFlagStore::grant(std::uint32_t item, bool new_item) {
  if (!ready_) {
    // No Link: a load would undo it. A new item gets it all at the mend.
    if (new_item && load_ != nullptr) load_->note_new(item);
    return;
  }
  for (const ItemGrant& g : item_grants(item)) {
    if (!new_item && !is_capacity(g)) continue;  // an item held already: capacity only
    const FlagKey key{g.hash, 0};
    const auto now = inner_.read(key);
    // Unknown value: written over nothing, so a grant can never lower one.
    if (!now || now->kind != g.kind) continue;
    if (as_int(now->value) >= g.value) continue;
    if (inner_.write(protocol::FlagEntry{key, g.kind, as_bits(g.value)})) ++stats_.granted;
  }
}

bool ItemFlagStore::write(const protocol::FlagEntry& entry) {
  const FlagKey key = key_of(entry);
  const bool named = key.kind == protocol::FlagKeyKind::named && key.index == 0;
  const ItemFlag* item = named ? find_item(key.hash) : nullptr;
  if (entry.kind == protocol::FlagKind::count_delta) {
    // A change, not a value: added to what the game holds, which must be known.
    const auto now = item != nullptr && item->rule == ItemRule::count &&
                             item->kind == protocol::FlagKind::int32
                         ? inner_.read(key)
                         : std::nullopt;
    if (!now || now->kind != protocol::FlagKind::int32) {
      ++stats_.refused;
      return false;
    }
    const std::int64_t sum = std::int64_t{as_int(now->value)} + as_int(entry.value);
    protocol::FlagEntry total = *now;
    total.key = key;
    total.value = as_bits(std::max<std::int64_t>(sum, 0));  // a count is never below 0
    if (total.value != now->value && !inner_.write(total)) return false;
    ++stats_.added;
    return true;
  }
  if (item == nullptr) return inner_.write(entry);
  if (entry.kind != item->kind || item->rule == ItemRule::per_player) {
    ++stats_.refused;
    return false;
  }
  const auto now = inner_.read(key);
  if (!now || now->kind != entry.kind) return inner_.write(entry);
  protocol::FlagEntry merged = entry;
  merged.value = exact_ ? entry.value : merge_item(item->rule, item->kind, now->value, entry.value);
  const bool int_item = item->kind == protocol::FlagKind::int32;
  const auto held = [&](std::uint32_t v) { return int_item ? as_int(v) >= 1 : v != 0; };
  if (merged.value == now->value) {
    ++stats_.kept;
  } else {
    if (!inner_.write(merged)) return false;
    ++stats_.merged;
  }
  // What the game gives with the item (issues 128, 138): all of it for an
  // item new to this game; for one it holds already, the capacity only, so
  // a game that got the item before without it (test5 saves: sticks and
  // seeds owned with room for 0) is mended on the next write or snapshot.
  if (held(merged.value)) grant(key.hash, !held(now->value));
  return true;
}

std::size_t ItemFlagStore::mend() {
  const std::uint64_t before = stats_.granted;
  std::uint32_t last = 0;
  bool any = false;
  for (const ItemGrant& g : kGrants) {
    if (any && g.item == last) continue;  // each item once
    any = true;
    last = g.item;
    const ItemFlag* item = find_item(g.item);
    if (item == nullptr) continue;
    const auto now = inner_.read(FlagKey{g.item, 0});
    if (!now || now->kind != item->kind) continue;
    const bool held = item->kind == protocol::FlagKind::int32 ? as_int(now->value) >= 1
                                                              : now->value != 0;
    if (held) grant(g.item, load_ != nullptr && load_->pending(g.item));
  }
  return static_cast<std::size_t>(stats_.granted - before);
}

bool ItemFlagStore::is_one_way(FlagKey key) {
  if (key.kind != protocol::FlagKeyKind::named || key.index != 0) return false;
  const ItemFlag* item = find_item(key.hash);
  return item != nullptr && item->on && item->kind == protocol::FlagKind::boolean &&
         item->name.starts_with("DungeonInfo.") &&
         (item->name.ends_with(".IsGetMap") || item->name.ends_with(".IsGetCompass"));
}

bool ItemFlagStore::write_exact(const protocol::FlagEntry& entry) {
  if (entry.kind == protocol::FlagKind::count_delta) return write(entry);  // a change, never exact
  exact_ = true;
  const bool ok = write(entry);
  exact_ = false;
  return ok;
}

bool ItemFlagStore::is_count(FlagKey key) {
  if (key.kind != protocol::FlagKeyKind::named || key.index != 0) return false;
  const ItemFlag* item = find_item(key.hash);
  return item != nullptr && item->on && item->rule == ItemRule::count;
}

}  // namespace coop::host
