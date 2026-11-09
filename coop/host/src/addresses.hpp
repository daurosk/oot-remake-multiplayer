#pragma once

// Every guest address and offset the host library reads, in one place.
//
// Game: main module of build e0f3ccfaf50065ae, display version 1.1.1.
// Sources (read only, never copied from game files):
//   [plan]  docs/plan.md, "Game facts we start from"
//   [map]   ..\integration\docs/player-state-map.md (Reads, Derivations, Run 1)
// "Checked" below means the map read it from the instructions; "run" means
// Run 1 of the map saw it in the live game; "inferred" means neither.
// Offsets into main are added to GameMemory::main_base(). Pointers are 8 bytes
// (the guest is AArch64), little-endian.

#include <cstdint>

namespace coop::host::addr {

// --- GameUI module: the player block it fills each frame ------------------
// [plan][map] Pointer to the GameUI module, at main + 0x4e7cfa8. Checked.
inline constexpr std::uint64_t kGameUiModulePtr = 0x4e7cfa8;
// [plan][map] Player position, 3 x f32, copied from actor +0x31c by 0x32a4c40.
// Checked; that it updates every frame is likely, not proven (Run 1 saw it
// change once, not follow a moving player).
inline constexpr std::uint64_t kGameUiPosition = 0x590;
// [map] u16 flags, bit 0 = the position above is valid (set by 0x32a4c40,
// cleared by 0x32a510c when the actor is not in state 4). Checked.
inline constexpr std::uint64_t kGameUiPoseFlags = 0x58a;
inline constexpr std::uint16_t kGameUiPoseValidBit = 0x1;
// [plan][map] u8 is-child, 1 for child (0x32a53f8 stores 0x4b6db0(actor),
// the same test QueryPlayerIsChild uses). Checked. 0 is taken as adult.
inline constexpr std::uint64_t kGameUiIsChild = 0x5c0;

// --- Scene module down to the player actor --------------------------------
// [map] "From the Scene module to the player", shown whole at 0x37c97c and
// the link choice at 0x37cca0. Checked. The map's peek is
// 7104e97648 0 0x1f0 0x58 0x78 0x40 8 0x40 +0x31c.
inline constexpr std::uint64_t kSceneModulePtr = 0x4e97648;
inline constexpr std::uint64_t kSceneTable = 0x1f0;          // S -> C
inline constexpr std::uint64_t kTableCount = 0x50;           // C: u32 count
inline constexpr std::uint64_t kTableArray = 0x58;           // C -> array
inline constexpr std::uint64_t kPlayerManagerSlot = 0x78;    // array[15] -> O
inline constexpr std::uint32_t kMinTableCount = 0x10;        // needs index 15
inline constexpr std::uint64_t kManagerMustBeSet = 0x30;     // O: pointer, non-zero
inline constexpr std::uint64_t kManagerLinkFirst = 0x40;     // O -> L, tried first
inline constexpr std::uint64_t kManagerLinkSecond = 0x50;    // O -> L, if not alive
inline constexpr std::uint64_t kLinkHolder = 0x08;           // L -> H
inline constexpr std::uint64_t kLinkId = 0x10;               // L: u32 id, ~0 empty
inline constexpr std::uint64_t kHolderActor = 0x40;          // H -> actor
inline constexpr std::uint64_t kHolderId = 0x48;             // H: u32, must equal L id
inline constexpr std::uint32_t kEmptyId = 0xffffffffu;
// The width of the two ids and of the count is not stated in the map; 4 bytes
// is assumed. Unconfirmed.

// --- Actor ----------------------------------------------------------------
// [map] u32 state; 4 is what GameUI requires before it copies the position,
// and is taken here as "the player is present". Checked; names unknown.
inline constexpr std::uint64_t kActorState = 0x2c;
inline constexpr std::uint32_t kActorStatePresent = 4;
// [plan][map] Position, 3 x f32. Checked.
inline constexpr std::uint64_t kActorPosition = 0x31c;
// [plan][map] Rotation, 9 x f32, three rows of three. The layout is checked;
// that it is rows (not columns) of the matrix that turns local into world is
// inferred. If a run shows the puppet turning the wrong way, flip this.
inline constexpr std::uint64_t kActorRotation = 0x328;
inline constexpr bool kActorRotationRowMajor = true;

// --- AS (animation) controller of an actor ----------------------------------
// [dump] actor +0x270 (component table) -> +0x40 -> +0x18 -> +0x18 is the AS
// controller, the same walk in 0x974b80, 0x1081f60, 0x3abebc0 and 0x2022a0.
// Checked.
inline constexpr std::uint64_t kActorComponents = 0x270;
inline constexpr std::uint64_t kComponentsAs = 0x40;
inline constexpr std::uint64_t kAsComponentInner = 0x18;
inline constexpr std::uint64_t kAsInnerController = 0x18;
// [dump] Controller: s32 slot count at +0x18, slot array at +0x20, 0x48 bytes
// an entry, whose first 8 bytes point to the slot. Every reader bounds the
// index by the count (0x3abebc0, 0x37b5d70, 0x2022a0). Checked.
inline constexpr std::uint64_t kAsSlotCount = 0x18;
inline constexpr std::uint64_t kAsSlotArray = 0x20;
inline constexpr std::uint64_t kAsSlotStride = 0x48;
// [dump] Slot 0 is the default slot: the slot-name lookup 0xacd6d0 gives 0
// for an empty or unknown name. That it is Link's whole-body slot is inferred.
inline constexpr std::uint32_t kAsMainSlot = 0;
// [dump] Slot: char* to the AS command now playing, at +0x10. 0x2022a0 with
// "skip if the same" (request +0x20 = 1) compares the new name with it, and
// ExecuteASSyncPartialAnimation (0x3abec10) replays it on another slot. An
// empty string (0x3cddd43) means nothing. Checked.
inline constexpr std::uint64_t kAsSlotCommandName = 0x10;
// [dump] Slot: f32 play rate at +0xd4 (0xdf49e0; 0x3abec10 passes it as the
// rate of the replay request). Checked as a read; that it is the rate is
// inferred from the request field it fills.
inline constexpr std::uint64_t kAsSlotRate = 0xd4;
// [dump] Slot: f32 current frame at +0xe8 (0x3abebc0). Checked as a read;
// that it is the frame is inferred (the ASFrame plug, see docs/puppet.md).
inline constexpr std::uint64_t kAsSlotFrame = 0xe8;

// --- Equipment ---------------------------------------------------------------
// [items] What the player has equipped: String64 flags holding actor names,
// docs/items.md. Hash = Murmur3 x86 32, seed 0, of the name (checked against
// protocol::flag_hash in tests/host/reader_test.cpp). Checked in the flag
// list. Run (2026-11-09): EquipWeapon, EquipShield and EquipOcarina follow
// the menu (Hylian <-> Deku shield seen live); armor, boots and masks were
// not changed in that run. Before a save is loaded they read as "".
inline constexpr std::uint32_t kEquipWeapon = 0xa95a3521;     // PlayerStatus.EquipWeapon
inline constexpr std::uint32_t kEquipShield = 0xee463235;     // PlayerStatus.EquipShield
inline constexpr std::uint32_t kEquipArmor = 0x008d7f0c;      // PlayerStatus.EquipArmor
inline constexpr std::uint32_t kEquipBoot = 0xd107a85d;       // PlayerStatus.EquipBoot
inline constexpr std::uint32_t kEquipHeadArmor = 0x59d9c7b3;  // PlayerStatus.EquipHeadArmor
inline constexpr std::uint32_t kEquipHandArmor = 0x7ce1cb81;  // PlayerStatus.EquipHandArmor
inline constexpr std::uint32_t kEquipOcarina = 0xb72cd3fc;    // PlayerStatus.EquipOcarina
inline constexpr std::uint32_t kEquipArrow = 0x3615e379;      // PlayerStatus.EquipArrow
// [dump] The actor's EquipmentUser component: component table +0x200, as
// 0x7f7690 reads it (QueryEquipmentUserIsEquipStateDrawnAny, 0x29f1f90).
// Checked.
inline constexpr std::uint64_t kComponentsEquipmentUser = 0x200;
// [dump] EquipmentUser: 8 dynamic equipment slots at +0x20, 0x18 bytes each,
// holding a link to the equipment actor (holder pointer +0x8, u32 id +0x10,
// ~0 empty; the holder as in the player link above). A byte per slot at
// +0x13bc, bit 1 = drawn. 0x4a33c0 walks them so, skipping a slot whose
// actor is in state 7 to 11 (gone). Checked.
// The byte is a request, not the state (two-player run, 2026-11-09: a drawn
// sword showed bit 1 for about 40 ms only). The setters write it and the
// game clears it when the change is done: 0x6b6820 (in hand) writes 2,
// 0xe34c70 (put away, also when a piece is created) writes 4, 0x2dfdee0
// writes 1; 0xce7ab0 reads the state from the piece itself once the byte is
// 0. So the reader takes the requests as they pass and remembers the last
// one (PlayerReader::read_drawn); a piece made anew starts put away.
// The slot records (+0x4b0, 0xa8 bytes each) are not the state: the byte at
// +0xa4 read 1 with the sword out and put away alike (autotest, 2026-11-09).
inline constexpr std::uint8_t kEquipRequestNone = 0x1;
inline constexpr std::uint8_t kEquipRequestAway = 0x4;
inline constexpr std::uint64_t kEquipSlots = 0x20;
inline constexpr std::uint64_t kEquipSlotStride = 0x18;
inline constexpr std::uint32_t kEquipSlotCount = 8;
inline constexpr std::uint64_t kEquipSlotHolder = 0x08;
inline constexpr std::uint64_t kEquipSlotId = 0x10;
inline constexpr std::uint64_t kEquipSlotState = 0x13bc;
inline constexpr std::uint8_t kEquipSlotDrawnBit = 0x2;
// Run (2026-11-09), child Link: slot 0 holds the sword, slot 4 the shield
// (its actor and id change on every shield change), slot 3 a held item.
// Slots 1, 2, 5, 6 and 7 stayed empty.
inline constexpr std::uint32_t kEquipSlotSword = 0;
inline constexpr std::uint32_t kEquipSlotHeldItem = 3;
inline constexpr std::uint32_t kEquipSlotShield = 4;
inline constexpr std::uint32_t kActorStateGoneFirst = 7;
inline constexpr std::uint32_t kActorStateGoneLast = 11;

// --- Banc module: the current scene ---------------------------------------
// [plan][map] Banc module pointer. Checked (actor-map.md).
inline constexpr std::uint64_t kBancModulePtr = 0x4e919d0;
// [map] 0x58aac0 gives no name when B +0x38 is null. Checked.
inline constexpr std::uint64_t kBancMustBeSet = 0x38;
// [plan][map] Pointer to the path string and its capacity (u64 assumed).
// Checked. 0x58aac0 cuts it after the last '/' or '\' and at the first '.'.
// It has an earlier branch (B +0x38 -> +0x150 -> +0x1a8) that was not read,
// so in some states this may not be the name. Likely, not sure.
inline constexpr std::uint64_t kBancPathPtr = 0x40;
inline constexpr std::uint64_t kBancPathCapacity = 0x48;

// --- Event manager ----------------------------------------------------------
// [plan][map] Event manager pointer; IsEventRunning (0xbcc100) is +0x68 != 0.
// Checked, and seen in Run 1 (set in the opening movie and timeline). 8 bytes,
// as the map's peek reads.
inline constexpr std::uint64_t kEventManagerPtr = 0x4e94840;
inline constexpr std::uint64_t kEventRunning = 0x68;

}  // namespace coop::host::addr
