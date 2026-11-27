#pragma once

// The player's items as flags in the game's flag store, and how co-op merges
// them (docs/items.md). Alex's rule: key items are given to everyone,
// rupees and other non-key items stay with each player, bottles are given to
// everyone but what is in them stays with each player.
//
// Items travel as ordinary shared flags (FlagWatcher). Two pieces here:
// - the item table: every item flag with its kind and rule, and the
//   SharedFlags list built from it;
// - ItemFlagStore, a FlagStore over the game's (GameFlagStore) that merges
//   an incoming item value with the one the game holds instead of
//   overwriting it. The host answers a FLAG_SET with what its game holds
//   after the write, so the merged value is what everyone gets.
//
// No game data beyond the flag names and their hashes.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/flags.hpp"
#include "coop/host/ticks.hpp"
#include "coop/protocol/messages.hpp"

namespace coop::host {

// The game's most hearts: PlayerStatus.MaxLife in quarter hearts (20 hearts). Our Link's Life component never keeps
// more: a write of 84 came back as 80 within 2 frames with no co-op line (volvagia-rocks-309d7180 B, 2026-11-27
// 03:47). The one value the give, the hearts-to-Link walk and NG+ use.
inline constexpr std::int32_t kMostMaxLife = 20 * 4;

enum class ItemRule : std::uint8_t {
  max,         // shared: everyone takes the highest value (owned items, upgrade tiers)
  any,         // shared: true for everyone once true for anyone (IsGet, songs, map, ...)
  mirror,      // shared: the last change is copied (trade items: handing one over takes it)
  count,       // shared: each change (+1 got, -1 used) is added to everyone's value
  per_player,  // never sent (rupees, ammo, life, bottle contents, equipped gear)
  bits,        // shared: a bit set for anyone is set for everyone (a dungeon's floors on its map)
};

struct ItemFlag {
  std::string_view name;    // the flag's name in the game
  std::uint32_t hash;       // flag_hash(name)
  protocol::FlagKind kind;  // boolean, int32, or uint32 (bottle contents, a dungeon's floors)
  ItemRule rule;
  // False for a rule not turned on yet. All are on now: counts travel as
  // count deltas (docs/protocol.md, "Counts").
  bool on;
};

// Every item flag of docs/items.md, sorted by hash, each hash once.
std::span<const ItemFlag> item_flags();
// The entry for `hash`, or null when it is not an item flag.
const ItemFlag* find_item(std::uint32_t hash);

// The flags to share: every entry that is on and not per player. With
// `with_off`, also the shared ones not on yet (for tests and trials).
SharedFlags item_shared_flags(bool with_off = false);

// A dungeon's map or compass (DungeonInfo.<dungeon>.IsGetMap, .IsGetCompass;
// issue 146): a Bool the game never takes back once got. Only entries that
// are on.
bool is_one_way_item(FlagKey key);
// Rows 63/76, #146: a dungeon item the HUD screen keeps its own copy of for the Map page and the minimap
// (DungeonInfo.<dungeon>.IsGetMap, IsGetCompass, IsGetBossKey, EnteredFloorBit; every dungeon): a co-op write of
// one asks the game's HUD refresh (lichii/hook/coop_map_refresh.h).
bool is_hud_dungeon_item(FlagKey key);
// Every such flag, sorted by hash.
std::vector<FlagKey> one_way_item_flags();
// dungeon-sync-b (#63/#76): a dungeon's floors flag (DungeonInfo.<dungeon>.EnteredFloorBit) gives the same
// dungeon's map flag (DungeonInfo.<dungeon>.IsGetMap); anything else gives nothing.
std::optional<FlagKey> map_of_floors(FlagKey floors);

// A shared UInt item (DungeonInfo.<dungeon>.EnteredFloorBit, issue 146): the
// game's UInt table is read and written for it (GameFlagStore). Other UInts
// stay unread there, as before.
bool is_shared_uint_item(std::uint32_t hash);

// What the game should hold after `incoming` arrives for a flag of `rule`
// and `kind` when it holds `local`. Values are raw FlagEntry bits. `base` is,
// for count, the value the sender's change started from (its change is
// incoming - base); without one, count takes `incoming`. per_player keeps
// `local`.
std::uint32_t merge_item(ItemRule rule, protocol::FlagKind kind, std::uint32_t local,
                         std::uint32_t incoming, std::optional<std::uint32_t> base = {});

// Gold Skulltula tokens (issue 89): PouchData.Item_Token_Gold.StockNum, a
// count raised only by a Skulltula's token. Taking the token sets the 64-bit
// key of that Skulltula's placement (Enemy_Skulltula_Gold) in the same scan.
inline constexpr std::uint32_t kGoldTokenCount = 0x04735978;
// IsGet.Item_Token_Gold (item_flags): the first token's get flag.
inline constexpr std::uint32_t kGoldTokenGet = 0x6ba394fe;
// The placement keys of all 100 Gold Skulltulas (the Enemy_Skulltula_Gold
// placements of every map, Child and Adult the same key; each key is in the
// game's key list), sorted. The hook shares them all from the start and
// guards the token count with them (FlagWatcher::guard_count).
std::span<const std::uint64_t> gold_skulltula_keys();
// Gold Skulltulas Not shared (row 63, review-01112-h): everything of the Gold Skulltulas is each racer's own, so
// beside the Skulltulas' keys, the token count and its IsGet each racer keeps these named flags (Bool or Int): the
// Skulltula House's five rewards (KakarikoVillage_GatheringTokenGold_GetReward10..50, each racer claims its own)
// and what only those rewards give: the wallet tiers (rewards 10 and 30; PouchData.Item_Wallet_*.StockNum and
// IsGet.Item_Wallet_*, the row 272 wallet family's units) and the Stone of Agony (20; IsGet and StockNum). The 40
// (bombchus) and 50 (a heart piece) give items other places give too: those items stay the group's.
std::span<const std::uint32_t> gold_race_flags();

// What the game gives with an item, besides the item itself: the ammo
// capacity (Int struct 0xf80f8077, one member per ammo) and the ammo it
// starts with. The game writes these in its own get scene; an item that
// arrives from another player has no get scene, so ItemFlagStore writes them
// when it gives a game an item it did not have (issue 128: a shared
// Slingshot came with 0/0 seeds). Each value is a floor: a flag already at
// or above it is left alone. Values follow the original game's get-item
// code: Slingshot and Bow come with 30 and the small bag or quiver, a bag
// upgrade fills the new capacity.
struct ItemGrant {
  std::uint32_t item;       // the item's PouchData.<item>.StockNum hash, or IsGet.<item>
  std::string_view name;    // the granted flag's name in the game
  std::uint32_t hash;       // the granted flag's hash
  protocol::FlagKind kind;  // boolean or int32
  std::int32_t value;       // at least this after the grant (bool: 1 = true)
};

// The grants of the item with StockNum hash `item` (empty for most items).
std::span<const ItemGrant> item_grants(std::uint32_t item);
// Every grant, sorted by item.
std::span<const ItemGrant> all_item_grants();

// Strength (issue 125). What Link can lift is PlayerStatus.LiftGrade, an
// Enum (None, Low, Middle, High), not the bracelet or gauntlet flags. A
// bomb is lifted out of the bag, so with None every pulled bomb is deleted
// at once. The game sets the grade when Link gets the item itself; a
// strength item shared by another player (or written into a save) leaves
// it at None. Hashes are flag_hash of the names.
namespace lift {
constexpr std::uint32_t kGrade = 0x59979321;  // PlayerStatus.LiftGrade
constexpr std::uint32_t kNone = 0xb6eede09, kLow = 0xd9e40095, kMiddle = 0xb6a1ccdf,
                        kHigh = 0x165af889;
constexpr std::uint32_t kGoron = 0xd1317941;   // PouchData.Armor_Child_Gauntlet_Goron.StockNum
constexpr std::uint32_t kSilver = 0x0012b8c0;  // PouchData.Armor_Adult_Gauntlet_Silver.StockNum
constexpr std::uint32_t kGold = 0x40ae1183;    // PouchData.Armor_Adult_Gauntlet_Gold.StockNum
}  // namespace lift

// The grade the held strength items give (Goron's Bracelet Low, Silver
// Gauntlets Middle, Golden Gauntlets High), when it is above `current`.
// Nothing to write otherwise, or when `current` is not one of the four.
std::optional<std::uint32_t> lift_grade_raise(std::int32_t goron, std::int32_t silver,
                                              std::int32_t gold, std::uint32_t current);

// Scales (randomizer items, docs/mods.md). How deep Link dives is PlayerStatus.DiveDepth, a Float
// (3.0 by default), not the scale items: OOTRR's starting scales write 6.0 (Silver) and 9.0 (Golden)
// beside the item (start_items/give.rs SCALE_DEPTH), so a scale shared by another player dived no deeper.
namespace dive {
constexpr std::uint32_t kDepth = 0x61466c21;   // PlayerStatus.DiveDepth
constexpr std::uint32_t kSilver = 0x1240044b;  // PouchData.Item_Scale_Silver.StockNum
constexpr std::uint32_t kGold = 0x4aeb140a;    // PouchData.Item_Scale_Gold.StockNum
constexpr float kSilverDepth = 6.0f, kGoldDepth = 9.0f;
}  // namespace dive

// The depth the held scales give, when it is above `current`. Nothing otherwise (never lowered).
std::optional<float> dive_depth_raise(std::int32_t silver, std::int32_t gold, float current);

// Issue 190 (docs/items.md, "The ocarina"): D-pad Left takes out the ocarina
// named in PlayerStatus.EquipOcarina (String64, an actor name, "" by
// default). The game writes it when Link gets an ocarina himself; an ocarina
// shared by another player leaves it "", so nothing comes out. Every save of
// a player who got it from Saria holds "Item_Ocarina_Fairy".
namespace ocarina {
constexpr std::uint32_t kEquip = 0xb72cd3fc;  // PlayerStatus.EquipOcarina
constexpr std::uint32_t kFairy = 0xb7df60f1;  // PouchData.Item_Ocarina_Fairy.StockNum
constexpr std::uint32_t kTime = 0x54e2a3d1;   // PouchData.Item_Ocarina_Time.StockNum
constexpr std::string_view kFairyName = "Item_Ocarina_Fairy";
constexpr std::string_view kTimeName = "Item_Ocarina_Time";
// The Bools a real get sets beside the count (tools/autotest/save_flags.py
// NO_OCARINA clears the same ones for a player who never met Saria). A
// shared ocarina brought only the count (ocarina-given, bigrun 2026-11-12:
// B's EquipOcarina was mended, but D-pad Left still took nothing out).
constexpr std::uint32_t kIsGetFairy = 0x42bd296f;   // IsGet.Item_Ocarina_Fairy
constexpr std::uint32_t kIsGetTime = 0x1da2c511;    // IsGet.Item_Ocarina_Time
constexpr std::uint32_t kSlotA = 0xaacdef1f;        // 0x541947d0.Ocarina
constexpr std::uint32_t kSlotB = 0xda41879d;        // 0x57f52eb8.Ocarina
constexpr std::uint32_t kGotFairy = 0x137df1ac;     // 0x41f3e410.Item_Ocarina_Fairy
constexpr std::uint32_t kGotTime = 0xac6a85e3;      // 0x41f3e410.Item_Ocarina_Time
// Set with Saria's gift and with OOTRR's starting ocarina (start_items/give.rs): an
// ocarina shared without it could not be played until a reload (playtest 11).
constexpr std::uint32_t kPrepared = 0x171de71f;     // HavePreparedToPlayOcarina
}  // namespace ocarina

// The ocarina to equip for the held ones (Ocarina of Time over the Fairy
// Ocarina), when `current` is "" or the Fairy Ocarina and a better one is
// held. Nothing otherwise: never lowered, and a name we do not know is left
// alone.
std::optional<std::string_view> ocarina_equip_mend(std::int32_t fairy, std::int32_t time,
                                                   std::string_view current);

// The ocarina Bools to raise for the held ones (ocarina::kIsGet*, kSlot*,
// kGot*): those `get` reads as false (not true, not unreadable). Only ever
// raised; nothing for an ocarina not held.
std::vector<std::uint32_t> ocarina_bools_mend(std::int32_t fairy, std::int32_t time,
                                              const std::function<std::optional<bool>(std::uint32_t)>& get);

// Rows 93 and 103 (docs/items.md, "The game's own equip"): the flags alone never let a shared ocarina out. The
// game equips what PlayerStatus.Equip<slot> names only when it asks Link's equipment for it: at a save load, and
// in the AI node OneShotPlayerEquipDynamicEquipmentFromGameData (main+0x290b1b0), which calls
// f_7102df6c80(equipment, slot). Slots 13 and 16 both read EquipOcarina; the game's own refreshes (main+0xf201b0,
// +0xf202c0, +0x2df6290) ask both, 13 first. Link's equipment: actor +0x270, then +0x200 (f_71007f7690).
namespace ocarina {
constexpr std::uint64_t kEquipFromGameData = 0x2df6c80;  // main offset: (equipment, slot)
constexpr std::uint64_t kActorEquipment = 0x270;         // Player actor -> its equipment holder
constexpr std::uint64_t kHolderEquipment = 0x200;        // holder -> the equipment the node passes
constexpr std::array<std::uint32_t, 2> kEquipSlots{13, 16};
}  // namespace ocarina

// The best ocarina the flags hold when EquipOcarina already names it (else empty: none held, or the mend still has
// to write the name, and wants it then).
std::string_view ocarina_held(std::int32_t fairy, std::int32_t time, std::string_view current);

// Asks the game to equip the ocarina from its flags: once after co-op changed them, and once per stay of our Link
// while one is held (OcarinaEquip::link). Thread-safe: the hook's frame arms it, the game's calc thread steps it (a
// CoopCallers caller).
class OcarinaEquip {
 public:
  // Co-op wrote the ocarina's flags for `name` (the equipped name, or the Bools of an ocarina already named): the
  // game is asked once the live name reads `name`.
  void want(std::string_view name);
  // Each frame: `live` is EquipOcarina as read now (nothing: unread). Ready to ask once it has read the wanted name
  // for kSettleFrames frames in a row (a queued String64 already reads so before the game's applier wrote it),
  // with Link's actor known and nothing running (`busy`: an event, a menu, the pause or a load).
  static constexpr int kSettleFrames = 30;
  void arm(const std::optional<std::string_view>& live, bool link_known, bool busy);
  // The calc thread: the next slot to ask, or nothing. `after_call` false starts a frame. After the last slot the
  // want is spent (a note says so).
  std::optional<std::uint32_t> step(bool after_call);
  // Each frame: whether our Link is in the game, and `held` (ocarina_held). Once per stay of Link (a save load or a
  // scene change ends one) a held ocarina is wanted. Asking only after a write of ours (0.11.3) missed every ocarina
  // whose flags were already there and needed no mend: a save an earlier co-op mend wrote them into, the join
  // snapshot, a load of the host's file (row 103). The game's own load asks the equipment for every slot the same way.
  void link(bool in_game, std::string_view held);
  // The equipment could not be read: the want stays, and is asked again on a later frame.
  void not_now();
  // Lines for the log since the last call.
  std::vector<std::string> take_notes();
  bool waiting() const;

 private:
  void want_locked(std::string_view name);

  mutable std::mutex mutex_;
  std::optional<std::string> wanted_;
  bool armed_ = false;
  int seen_ = 0;
  std::size_t next_ = 0;
  bool asked_this_stay_ = false;
  std::vector<std::string> notes_;
};

// The two letters (issue 58, docs/items.md, "The letters").
namespace letters {
// Zelda's Letter: the Kakariko guard takes it from the player who shows it and
// sets his Bool (shared story). Each player kept theirs (max, issue 171), so it
// stayed in the pouch of every player who never showed it.
constexpr std::uint32_t kZelda = 0x6be5c3b0;     // PouchData.Item_ZeldaLetter.StockNum
constexpr std::uint32_t kGateOpen = 0x19898d14;  // Npc_Hylia_Soldier_003_GateOpen
// Ruto's Letter is a bottle's contents (BottleItem Enum "Letter"), per player as
// every bottle's contents: another player got only the bottle, empty. King Zora
// moves in each game by its own ChallengeMgr step (Get_StoneOfWater Step07),
// which the shared quest catch-up (coop_quest.cpp) raises there, separately
// from the scene's Bool below.
constexpr std::uint32_t kRutoGot = 0x170b5df2;   // IsGet.Item_Bottle_Letter
constexpr std::uint32_t kLetter = 0x0f24e808;    // the Enum value "Letter"
// The scene of King Zora reading the letter and moving aside (romfs
// Event/EventFlow/Dm_Zora_0010: the flow takes Item_Bottle_Letter out of the
// bottle). Its HavePlayedEvent Bool is shared story (coop_story.cpp), so once
// any player has shown the letter, every game has it: never given again, and
// spent in a game once its own King Zora has moved too (tracker row 117).
constexpr std::uint32_t kKingRead = 0xbf9dfc7e;  // HavePlayedEvent.Dm_Zora_0010
constexpr std::uint32_t kEmpty = 0xbe69de49;     // the Enum value "Empty"
struct Bottle {
  std::uint32_t valid;  // PlayerStatus.AssignableItem.BottleInfo.Bottle<X>.IsValid
  std::uint32_t item;   // ... .BottleItem
};
constexpr std::array<Bottle, 5> kBottles{{{0xf39ce410, 0x7d48df76},
                                          {0x000ff6ea, 0x1edd164a},
                                          {0xf63b1194, 0xb80c70a8},
                                          {0xb7f052ce, 0xe4c1b1fc},
                                          {0xe27b136d, 0x365e1953}}};
// King Zora steps aside at Get_StoneOfWater Step07 (OOTRR start_state.rs
// FOUNTAIN_STEPS): the quest's steps in the game's order, as name hashes.
constexpr std::uint32_t kStoneOfWater = 0x0aa38424;  // the challenge Get_StoneOfWater
constexpr std::array<std::uint32_t, 10> kKingWaits{0x57b849f0, 0x17bfaa46, 0xeb62add9, 0x0e5afad8, 0x6f917dfe,
                                                   0x8b4b69e2, 0x27f660fe, 0x5cde171f, 0xec4c4a24, 0x21d6fa38};
constexpr std::array<std::uint32_t, 10> kKingMoved{0xee867c27, 0xa594ed54, 0x5daeca58, 0x0be69809, 0x02428c32,
                                                   0xc25a29d8, 0x2f0ed242, 0xa6447454, 0x84be6ff1, 0x4c0a63f4};
}  // namespace letters

// The adult trading sequence (tracker row 117, review): each trade item is
// shared by mirror, but every trade NPC's flow also tests that game's own
// quest step (SubEvent_GetSwordBigGoron_IsAfter_StepNN_Exp, GameData), and the
// quest is each game's own (CoopQuests leaves it out: its steps go back on a
// spoiled item). So a player who did not trade held the next item while their
// NPCs still waited for the last one. The step follows the item instead: the
// step at which the game's own flow hands that item over (read from the romfs
// flows' EventExecuteProgressChallengeStep: Anju gives the Pocket Egg and sets
// StartSubEvent, the egg hatches at EggHatched, Anju's Cojiro Step03, the
// Lost Woods' mushroom Step04, Granny's potion Step05, Fado's saw Step06, the
// carpenter's Broken Sword Step07, Biggoron's Prescription Step08, King Zora's
// frog Step09, the lab's Eye Drops Step10, Biggoron's Claim Check Step11).
namespace trade {
constexpr std::uint32_t kQuest = 0x26a2e01c;      // GameData Enum Step_SubEvent_GetSwordBigGoron
constexpr std::uint32_t kChallenge = 0x01e5c1d7;  // the ChallengeMgr entry SubEvent_GetSwordBigGoron
// The Enum's values in the game's order (NotReady, the RSDB Challenge row's
// steps, Complete), as name hashes.
constexpr std::array<std::uint32_t, 27> kSteps{
    0x57b849f0,  // NotReady
    0x17bfaa46,  // Ready
    0xd157b76f,  // StartSubEvent
    0x777ea18c,  // WaitEggHatch
    0x3e6c49bc,  // EggHatched
    0xeb62add9,  // Step01
    0x93e1d7d7,  // Step01_01
    0x0e5afad8,  // Step02
    0x6f917dfe,  // Step03
    0x14376953,  // Step03_01
    0xd932a31c,  // Step03_02
    0x057ef5f8,  // Step03_03 (the mushroom spoiled)
    0x27f660fe,  // Step04
    0x5cde171f,  // Step05
    0xec4c4a24,  // Step05_01
    0x21d6fa38,  // Step06
    0xd7a3defb,  // Step06_01
    0x6ef85d0f,  // Step06_02
    0x03c56929,  // Step06_03
    0xee867c27,  // Step07
    0xa594ed54,  // Step08
    0xd677164b,  // Step08_01 (the frog spoiled)
    0x5daeca58,  // Step09
    0xd2f8add5,  // Step09_01 (the Eye Drops spoiled)
    0x0be69809,  // Step10
    0x02428c32,  // Step11
    0x4c0a63f4,  // Complete
};
struct Hold {
  std::uint32_t item;  // PouchData.<item>.StockNum
  std::uint32_t step;  // the step the game's own hand-over of it sets
  const char* name;
};
constexpr std::array<Hold, 11> kHolds{{
    {0x07f10aaf, 0xd157b76f, "Item_PocketEgg"},     // StartSubEvent
    {0x0165e7ca, 0x3e6c49bc, "Item_PocketCucco"},   // EggHatched
    {0x51d89ef0, 0x6f917dfe, "Item_CojiroCucco"},   // Step03
    {0x07de3e6f, 0x27f660fe, "Item_OddMushroom"},   // Step04
    {0x58c9bc78, 0x5cde171f, "Item_OddPowder"},     // Step05
    {0xa862d276, 0x21d6fa38, "Item_PoacherSaw"},    // Step06
    {0x2815c82a, 0xee867c27, "Item_BrokenSword"},   // Step07
    {0x897cf7eb, 0xa594ed54, "Item_Prescription"},  // Step08
    {0x919ea324, 0x5daeca58, "Item_EyeballFrog"},   // Step09
    {0x5c41a6b1, 0x0be69809, "Item_EyeDrop"},       // Step10
    {0xded8e8d3, 0x02428c32, "Item_ClaimCheck"},    // Step11
}};
}  // namespace trade

// The step to raise our SubEvent_GetSwordBigGoron to, or nothing: the step of
// the furthest trade item held (StockNum > 0), when it is above ours. Only
// forward; never from NotReady (the quest manager readies it; a child or a
// game before it is left alone), never to Complete; our step unread or not in
// the list leaves it alone, and so does an unread item.
std::optional<std::uint32_t> trade_step_raise(std::optional<std::uint32_t> step,
                                              const std::function<std::optional<std::int32_t>(std::uint32_t)>& stock);
// Index of `step` in trade::kSteps, or nothing.
std::optional<std::size_t> trade_step_rank(std::uint32_t step);

// True when the held Zelda's Letter is spent: the gate's Bool is set (by any
// player), so the guard never asks for it again. The guard takes it from the
// shower; this takes it from the others.
bool zelda_letter_spent(std::int32_t held, std::optional<bool> gate_open);

// Whether King Zora has moved in this game, from the step its ChallengeMgr
// holds for Get_StoneOfWater. Nothing for a step not in the quest.
std::optional<bool> king_zora_moved(std::uint32_t live_step);

// Why Ruto's Letter goes into a bottle or waits (issue 58), for the log: a
// run where it never came said nothing about which test held it back.
enum class RutoWait {
  not_got,        // IsGet.Item_Bottle_Letter unset (or unread)
  shown,          // the letter was shown to King Zora in some game (kKingRead): never given
  shown_king_waits,  // shown elsewhere, but our King Zora has not moved (or is unread): ours is kept
  shown_unread,   // kKingRead unread: neither given nor taken
  king_unread,    // our ChallengeMgr's Get_StoneOfWater unread or at a step not in the quest
  king_moved,     // King Zora has moved here: no letter needed
  bottle_unread,  // a bottle's IsValid or BottleItem unread
  has_letter,     // a bottle holds it already
  no_empty,       // no owned empty bottle yet
  give,           // `bottle` gets it
};
struct RutoLetter {
  RutoWait why = RutoWait::not_got;
  std::optional<std::uint32_t> bottle;  // the BottleItem flag, with `give`
  // With `shown` and our King Zora moved: a bottle that still holds the
  // letter, to empty as his own scene empties the shower's.
  std::optional<std::uint32_t> spent;
};
std::string_view ruto_wait_text(RutoWait why);
RutoLetter ruto_letter_plan(std::optional<bool> got, std::optional<bool> shown, std::optional<bool> king_moved,
                            const std::function<std::optional<bool>(std::uint32_t)>& valid,
                            const std::function<std::optional<std::uint32_t>(std::uint32_t)>& item);

// Tracker row 162: whether a bottle we own holds a fairy (BottleItem Enum
// "Fairy"), which the game's own death at no life uses to revive Link. An
// unread bottle counts as no fairy (down but not out then holds the hit).
constexpr std::uint32_t kBottledFairy = 0x6f49e357;  // the Enum value "Fairy"
bool bottled_fairy(const std::function<std::optional<bool>(std::uint32_t)>& valid,
                   const std::function<std::optional<std::uint32_t>(std::uint32_t)>& item);

// The BottleItem flag to put Ruto's Letter in, or nothing: when the letter was
// got (by anyone), nobody has shown it yet (kKingRead), King Zora has not moved here, no bottle holds it, and a
// bottle is owned and empty (the first). Anything unread leaves it alone.
std::optional<std::uint32_t> ruto_letter_bottle(std::optional<bool> got, std::optional<bool> shown,
                                                std::optional<bool> king_moved,
                                                const std::function<std::optional<bool>(std::uint32_t)>& valid,
                                                const std::function<std::optional<std::uint32_t>(std::uint32_t)>& item);

// Row 272 (docs/items.md, "Rewards given once per save"): a reward an NPC or a minigame gives once per save.
// Its flow sets a saved Bool as it gives it (read from the game's Event/EventFlow), and that Bool was each
// player's own, so every game gave the reward again; where the game gives it relative to what it holds (the
// first empty bottle, the tier after the one it has, one more heart piece), the share added the copies up
// (4 players, 4 bottles from Anju). The Bools are shared now (FlagWatcher::guard_reward /
// share_pickup_named), and the bottle and upgrade gains are guarded against the same reward got in two games
// at once (FlagWatcher::guard_reward).
namespace rewards {
// What a reward gives: each family is guarded on its own (FlagWatcher::guard_reward), so a reward of one
// never pays for a gain of another.
enum class Family : std::uint8_t { bottle, wallet, seed_bag, quiver, bomb_bag, stick_bag, nut_bag, none };
inline constexpr std::array<Family, 7> kFamilies{Family::bottle,   Family::wallet,    Family::seed_bag, Family::quiver,
                                                 Family::bomb_bag, Family::stick_bag, Family::nut_bag};
struct Source {
  std::uint32_t hash;     // flag_hash(name)
  std::string_view name;  // the Bool in the game (each one is in GameDataList)
  Family family;
};
// Rewards that are a bottle or an upgrade's next tier (each upgrade flow asks the pouch's tier,
// EventQueryIsExistInPouch, and gives the next).
inline constexpr std::array<Source, 14> kGuarded{{
    {0x7a7961c3, "MiniGame_CatchCucco_IsCompleted", Family::bottle},  // Anju's cuccos, child (Minigame_CatchCucco)
    {0x084da83f, "IsClear_Minigame_FindOutCucco", Family::bottle},    // Talon's cuccos, child (Minigame_FindOutCucco)
    {0x9e47f045, "Npc_Hylia_048_BottleShop_Full", Family::bottle},    // the poe collector's 1000 points, adult
    {0x170b5df2, "IsGet.Item_Bottle_Letter", Family::bottle},         // Ruto's Letter (shared since issue 58)
    {0x9086a22e, "KakarikoVillage_GatheringTokenGold_GetReward10", Family::wallet},  // 10 tokens
    {0xcdcc9add, "KakarikoVillage_GatheringTokenGold_GetReward30", Family::wallet},  // 30 tokens
    {0x4bafdf70, "LostWood_HitTargetOfDekuScrub_FirstClear", Family::seed_bag},      // the Lost Woods target
    {0x893fa7cb, "MiniGame_HitTarget_HrlCity_Get_BulletBag", Family::seed_bag},      // the Market gallery
    {0xdda8aaf7, "MiniGame_HitTarget_Kakariko_Get_Quiver", Family::quiver},          // the Kakariko gallery
    {0x7c6e3b6b, "MiniGame_HorseBackArchery_Get_Quiver", Family::quiver},            // horseback archery
    {0x19e5a36b, "MiniGame_BombchuBowling_ClearFirstTime", Family::bomb_bag},        // bombchu bowling's first prize
    {0x1fb5294d, "GoronCity_GetBombBag_fromGoron006", Family::bomb_bag},             // the rolling Goron
    {0x5db0e8ed, "LostWood_ScrubForestStage_SkullMaskEventLaunched", Family::stick_bag},  // Deku Theater
    {0xcdf86c43, "LostWood_ScrubForestStage_TrueMaskEventLaunched", Family::nut_bag},     // Deku Theater
}};
// Other once-per-save rewards: shared only (a heart piece is a count: two at once still add up, docs/items.md).
inline constexpr std::array<Source, 10> kOther{{
    {0x6b8fc751, "DeathMountain_Enemy_Scrub_Business_HeartPiece_01", Family::none},
    {0xe41e1597, "HyruleField_Enemy_Scrub_Business_HeartPiece_01", Family::none},
    {0x1d1793e9, "GoronLargePot_IsGetHeartPiece", Family::none},
    {0x1657d7d8, "Minigame_SkullKidSession_IsGetHeartPiece", Family::none},
    {0xcdd1c1dd, "MiniGame_BombchuBowling_ClearSecondTime", Family::none},         // a heart piece
    {0x39dad76d, "KakarikoVillage_GatheringTokenGold_GetReward20", Family::none},  // the Stone of Agony
    {0x8dc37ac6, "KakarikoVillage_GatheringTokenGold_GetReward40", Family::none},  // bombchus
    {0xe3c6e4a3, "KakarikoVillage_GatheringTokenGold_GetReward50", Family::none},  // a heart piece
    {0x3f43245b, "MiniGame_HorsebackArchery_GotaHeart", Family::none},             // horseback archery: a heart piece
    {0x46e5bc53, "MiniGame_HorsebackArchery_ISGetSpecialPrize", Family::none},
}};
// A family's units, each one or more keys got together: a bottle slot (PouchData.Bottle<X>.StockNum and
// BottleInfo.Bottle<X>.IsValid, A to E); an upgrade's tiers (PouchData.<tier>.StockNum and IsGet.<tier>, small
// to large).
std::vector<std::vector<FlagKey>> units(Family family);
// A family's reward Bools, from kGuarded.
std::vector<FlagKey> sources(Family family);
// C2-5 (0.12.0): a Business Scrub's one-time sale of a family's next tier (docs/items.md "One-time sales"; the
// hook shares them, coop_scrub_sale.h kSaleFlags). No reward, but a Bool of the buyer's own: given to the guard
// as one more source, so a bought tier is paid by its own Bool and never taken for another player's reward.
inline constexpr std::array<Source, 2> kSales{{
    {0x8c98cf5a, "LostWood_Enemy_Scrub_Business_DekuStickPouch_01", Family::stick_bag},
    {0x5ef24799, "Lostwood_Enemy_Scrub_Business_DekuNutsPouch_01", Family::nut_bag},
}};
// A family's sale Bools, from kSales.
std::vector<FlagKey> sales(Family family);
}  // namespace rewards

// When to mend (ItemFlagStore::mend), kept by the hook across frames: for
// the first frames after Link comes into the game (a save or a scene
// loaded: no Link, then Link), once a frame. The load may still be writing
// the save's values in the first frames, and a mend is only floors, so it
// is run for a while rather than once. It also keeps the items that came
// while there was no Link: they get their whole grant (ammo too) in the
// mend frames (issues 128, 138).
class LoadMend {
 public:
  static constexpr unsigned kFrames = 90;  // 1.5 s: 1/60 s frames of set_clock's clock

  // Issue 106: frame() is called once per presented frame (up to 165 a
  // second with high refresh); with a clock kFrames counts real 1/60 s.
  // Without one each call is one frame.
  void set_clock(GameTicks::Clock clock) { ticks_.set_clock(std::move(clock)); }

  // Once per emulated frame, with whether Link is in the game. True: mend
  // this frame.
  bool frame(bool link) {
    const unsigned step = ticks_.step();
    if (link && !link_) left_ = kFrames;
    link_ = link;
    if (!link_) return false;
    if (left_ == 0) {
      pending_.clear();  // their mend frames are over
      return false;
    }
    left_ -= (std::min)(left_, step);
    return true;
  }
  // Link is in the game (as of the last frame()).
  bool ready() const { return link_; }
  // An item new to the game came while not ready (ItemFlagStore).
  void note_new(std::uint32_t item) {
    if (std::find(pending_.begin(), pending_.end(), item) == pending_.end()) pending_.push_back(item);
  }
  bool pending(std::uint32_t item) const {
    return std::find(pending_.begin(), pending_.end(), item) != pending_.end();
  }

 private:
  bool link_ = false;
  unsigned left_ = 0;
  GameTicks ticks_;
  std::vector<std::uint32_t> pending_;
};

// A FlagStore that merges item flags on write and passes everything else
// (other world flags, 64-bit keys) through unchanged. Reads are the inner
// store's. Keeps no state beyond counters.
class ItemFlagStore final : public FlagStore {
 public:
  // With `load`: grants only while Link is in the game (load->ready()).
  // Before that (the title screen, a save or scene loading) items are
  // merged but nothing is granted: the load would put the capacities back
  // to the save's (issue 138). An item new to the game then is noted in
  // `load` and gets its whole grant at the mend. Without `load`: always.
  explicit ItemFlagStore(FlagStore& inner, LoadMend* load = nullptr)
      : inner_(inner), load_(load), ready_(load == nullptr || load->ready()) {}

  std::optional<protocol::FlagEntry> read(FlagKey key) override { return inner_.read(key); }
  // An item flag: reads what the game holds, merges, and writes only when
  // that changes the value (true, without a write, when it does not). A
  // per-player flag or a kind that does not match the table is refused.
  // When the game's value cannot be read, writes `entry` as it came.
  // A count delta (FlagKind::count_delta) on a count item is added to the
  // game's value, held to 0 and up; it is refused on anything else, or when
  // the game's value cannot be read.
  bool write(const protocol::FlagEntry& entry) override;
  // A named count item (ItemRule::count).
  bool is_count(FlagKey key) override;
  // A named int32 item merged by the highest value (ItemRule::max).
  bool is_max(FlagKey key) override;
  // DungeonInfo.<dungeon>.IsGetMap and .IsGetCompass (issue 146).
  bool is_one_way(FlagKey key) override;
  bool busy(FlagKey key) override { return inner_.busy(key); }
  // The value as it is (no max, OR or mirror merging; per-player flags still
  // refused): the host's save decides (docs/flags.md, "Saves").
  bool write_exact(const protocol::FlagEntry& entry) override;
  // As write_exact, but a dungeon's floors are ORed (FlagStore::write_load_back).
  bool write_load_back(const protocol::FlagEntry& entry) override;
  // An item merged from 0 (or less, or false) to 1 or more (true) in the
  // game also raises its grants (item_grants) to their floor, per-player
  // flags included. An item the game holds already gets its capacity grants
  // only (mends a game that got it without them). Only writes through this
  // store do that; the game's own gets are left to the game.

  struct Stats {
    std::uint64_t merged = 0;   // item writes that changed the game's value
    std::uint64_t kept = 0;     // item writes that left the game's value as it was
    std::uint64_t refused = 0;  // item writes with the wrong kind, or per player
    std::uint64_t added = 0;    // count deltas added to the game's value
    std::uint64_t granted = 0;  // grant flags raised (ammo, capacity) with a new item
  };
  const Stats& stats() const { return stats_; }

  // Every item the game holds gets its capacity grants; one that came while
  // there was no Link (LoadMend::pending) gets its whole grant, ammo too.
  // Run in LoadMend's frames after a load: the load puts back the save's
  // capacities, which a save from before the fix has at 0. Returns how many
  // flags it raised.
  std::size_t mend();

 private:
  void grant(std::uint32_t item, bool new_item);

  bool exact_ = false;      // write_exact: take the value as it is
  bool keep_bits_ = false;  // write_load_back: and OR a bits item (floors)
  FlagStore& inner_;
  LoadMend* load_ = nullptr;
  bool ready_ = true;
  Stats stats_;
};

}  // namespace coop::host
