// Puppets: a remote player shown as an actor in the local game (plan stage 1
// step 2, docs/puppet.md). No emulator code here, so it is tested on its own.
//
// Two threads:
// - the present thread (CoopHook::OnFrame) calls Publish: it decides what
//   should be shown (Planner) and hands it to the Director;
// - the game's calc thread runs the code cave once per game frame
//   (coop_svc.s). Each `svc #0xf4` from it reaches Director::Step, which
//   reads and writes guest memory directly and returns at most one game
//   function for the cave to call: malloc for a puppet's memory block, the
//   create request, the delete request.
// The pose is written straight into the actor every game frame.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_native_player.h"
#include "coop_ocarina.h"
#include "coop_follow.h"
#include "coop_snap.h"
#include "coop_link_sound.h"
#include "coop_puppet_flame.h"
#include "coop_puppet_sound.h"

namespace coop::protocol {
struct Equipment;
}
namespace coop::host {
class Session;
}
namespace coop::net {
struct Event;
}

namespace CoopPuppet {

using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;  // x, y, z, w
using Mat3 = std::array<float, 9>;  // rows of 3, as the actor holds it

// Offsets into main of build e0f3ccfaf50065ae (docs/puppet.md).
namespace addr {
inline constexpr std::uint64_t kCreate = 0x372a70;          // requestCreateActorAsync
inline constexpr std::uint64_t kActorManagerPtr = 0x4e91888;
inline constexpr std::uint64_t kMalloc = 0x14c6c90;
inline constexpr std::uint64_t kRequestDelete = 0xa6490;    // (actor, 0)
// What the create puts in the handle slot is a request: its low 16 bits at
// +0 are its state, +8 the actor. SeqExecuteCreateActor's check (0x32bcb30)
// waits for state 3, then takes the actor with 0x36fbd0(request, 1, 0),
// which wakes it (0x94f30) and returns it, and clears the slot. State 4 it
// hands to 0x36fde0(request); 5 and 6 are failures.
inline constexpr std::uint64_t kTakeCreated = 0x36fbd0;     // (request, 1, 0) -> actor
inline constexpr std::uint64_t kDropCreate = 0x36fde0;      // (request)
inline constexpr std::uint64_t kRequestActor = 0x08;
inline constexpr std::uint32_t kRequestReady = 3;
inline constexpr std::uint32_t kRequestDrop = 4;
// The GOT entry the preload walker reads for create argument +0x40.
inline constexpr std::uint64_t kGotCreateDefault = 0x4c7dad0;

inline constexpr std::uint64_t kActorState = 0x2c;
inline constexpr std::uint64_t kActorPosition = 0x31c;
inline constexpr std::uint64_t kActorRotation = 0x328;
// The pending transform the engine's own setter 0xc6fd60(actor, mtx34, 0)
// (the run of OneShotActorRequestSetMtx, 0x28b2050) leaves on an actor with
// no physics body: position at +0x358, rotation rows at +0x364. The actor's
// calc applies it to +0x31c and +0x328; written with them every frame.
inline constexpr std::uint64_t kActorPositionRequest = 0x358;
inline constexpr std::uint64_t kActorRotationRequest = 0x364;
inline constexpr std::uint32_t kStatePresent = 4;
inline constexpr std::uint64_t kLinkHolder = 0x08;
inline constexpr std::uint64_t kLinkId = 0x10;
inline constexpr std::uint64_t kHolderActor = 0x40;
inline constexpr std::uint64_t kHolderId = 0x48;

// Animation (docs/puppet.md, Animation). The AS controller is actor +0x270,
// then +0x40, +0x18, +0x18; its slot count at +0x18, slots at +0x20 (0x48
// bytes each, the first 8 point to the slot); slot +0x10 the command name,
// +0xe8 the frame.
inline constexpr std::uint64_t kPlayAs = 0x2022a0;      // (request)
inline constexpr std::uint64_t kSetAsFrame = 0x37b5d70; // (controller, slot, s0 = frame)
inline constexpr std::array<std::uint64_t, 4> kControllerPath{0x270, 0x40, 0x18, 0x18};
inline constexpr std::uint64_t kSlotCount = 0x18;
inline constexpr std::uint64_t kSlots = 0x20;
inline constexpr std::uint64_t kSlotName = 0x10;
inline constexpr std::uint64_t kSlotFrame = 0xe8;
// ExecuteASSyncPartialAnimation's call: 0x3abec10(actor, from slot, to slot,
// node 0, 0) plays the command of one slot on another (request skip-if-same
// 1, the source's rate) through 0x37b59c0. Link's upper body and other
// partial slots are filled this way from slot 0.
inline constexpr std::uint64_t kSyncAsSlot = 0x3abec10;
inline constexpr std::int32_t kMaxAsSlots = 8;  // slots synced from slot 0

// Gear (docs/puppet.md, Equipment): the EquipmentUser is [[actor + 0x270] +
// 0x200]; 0x2df7b20(eu, kind, &char*, state 2, -1, 1, 0) wears the named
// piece in a slot kind, as the game's own GameData path does.
inline constexpr std::uint64_t kEquip = 0x2df7b20;
inline constexpr std::uint64_t kActorParts = 0x270;
inline constexpr std::uint64_t kActorName = 0x260;  // -> the actor's name, a C string
inline constexpr std::uint64_t kPartsEquipmentUser = 0x200;
// In hand or put away: 0xe34c40(eu, slot, state) is the game's own change of
// an equipment slot's state (the shield node 0x2797850 calls it with slot 4,
// state 1). State 1 is in hand (0x6b6820), 2 put away on the back
// (0xe34c70, the state a piece is worn in). Each sets a bit in the slot's
// byte at EquipmentUser + 0x13bc + slot (2 in hand, 4 put away), which is how
// a call that was taken is told from one that was not. The slot's link is at
// EquipmentUser + 0x20 + 0x18 * slot (u32 id at +0x10, ~0 empty).
inline constexpr std::uint64_t kEquipChangeState = 0xe34c40;
// The game's own read of a slot's state: 0xce7ab0(eu, slot) returns 1 in
// hand, 2 put away, 0 none (read from the dump: a request in the slot's
// byte gives the rodata table at 0x3ef9474: bit 0 -> 0, bit 1 -> 1,
// bit 2 -> 2; with no request, the state the piece itself keeps). The
// puppet's own played animations can draw or put away its pieces too, so
// the director asks this now and then and changes a piece again when the
// game has it otherwise than its player (held-fix).
inline constexpr std::uint64_t kEquipState = 0xce7ab0;
// Taking armor off: 0x10aceb0(eu, index, 0) empties the EquipmentUser's
// armor link `index` (eu + 0xe0 + 0x18 * index, the same {holder, id} shape
// as a hand slot) and deletes its piece. The game's own GameData path
// (0x2df6c80) calls it this way when a PlayerStatus.Equip* armor flag is "":
// kind 7 (head) index 0, 8 (upper) 1, 9 (lower) 2, 10 (hand) 3. It returns
// 0 when there was a piece it could not take off.
inline constexpr std::uint64_t kUnequipArmor = 0x10aceb0;
// A worn head piece shown (#91). The EquipmentUser has one byte per armor
// link at +0x13c4 (link 0 the head; the hand slots' bytes are just before it,
// +0x13bc + slot), whose low two bits say whether the piece is drawn: 2
// shown, 1 not. The equip call and the GameData path leave it alone, so a
// mask worn so was made but never drawn, on the player's own Link too
// (wear-masks, 20261110-155536 and probe2 of 11-18). The game sets it where
// a mask is really put on: its ZR use (0x4b7b20: 2 and 0x4b84b0 to wear, 1
// and 0x4b8240 to take off) and the pause menu's equipment (0xa7aa60), which
// calls 0x33abfa0(owner): eu = [owner + 0x470]; byte = (byte & ~3) | 2; then
// 0x4b84b0(eu), which moves the hair texture for the mask (MaskParam's UV,
// material Mt_Hairs). The director calls it with `owner` = a cell of the
// puppet's block holding its EquipmentUser, minus 0x470.
// 0x4b8240(eu) puts the hair back (the game's take-off).
inline constexpr std::uint64_t kShowHeadArmor = 0x33abfa0;
inline constexpr std::uint64_t kShowHeadArmorEu = 0x470;   // [owner + this] = EquipmentUser
inline constexpr std::uint64_t kHeadArmorHairBack = 0x4b8240;
inline constexpr std::uint64_t kArmorShownBytes = 0x13c4;  // + armor link: low bits 2 shown, 1 not
inline constexpr std::uint32_t kArmorKindFirst = 7;   // ArmorHead
inline constexpr std::uint32_t kArmorKindLast = 10;   // ArmorHand
inline constexpr std::uint64_t kArmorLinks = 0xe0;    // kArmorLinkCount links of kEquipSlotStride
inline constexpr std::uint32_t kArmorLinkCount = 13;
inline constexpr std::uint32_t kEquipStateInHand = 1;
inline constexpr std::uint32_t kEquipStateAway = 2;
inline constexpr std::uint64_t kEquipSlots = 0x20;
inline constexpr std::uint64_t kEquipSlotStride = 0x18;
inline constexpr std::uint64_t kEquipSlotHolder = 0x08;
inline constexpr std::uint64_t kEquipSlotId = 0x10;
inline constexpr std::uint64_t kEquipSlotBits = 0x13bc;
inline constexpr std::uint8_t kEquipBitInHand = 0x2;
inline constexpr std::uint8_t kEquipBitAway = 0x4;
// The byte is a request the game clears when the change is done (seen in a
// two-player run, 2026-11-09), and a setter does nothing when the piece is in
// that state already. The user's slot records (EquipmentUser + 0x4b0, 0xa8
// each) are only logged (LICHII_COOP_DEBUG_ANIM): their byte at +0xa4 read 1
// with the sword out and put away alike (autotest, 2026-11-09), so it is not
// the state.
inline constexpr std::uint64_t kEquipRecords = 0x4b0;
inline constexpr std::uint64_t kEquipRecordStride = 0xa8;
inline constexpr std::uint64_t kEquipRecordInHand = 0xa4;
inline constexpr std::uint64_t kEquipRecordFlags = 0xa0;  // u32, logged only
// The game's own way to move an actor that has a physics body, as the AI
// node ExecuteActorForceSetMtx (run 0x2622e50): write +0x31c..+0x34b, then
// call vtable slot 0x218, which is 0x4c6580(actor, 0) for the vtable every
// actor seen has (docs/objects.md). A plain write is undone within a frame.
inline constexpr std::uint64_t kActorVtable = 0x4b25e18;
inline constexpr std::uint64_t kSetMatrix = 0x4c6580;
}  // namespace addr

// The slot kinds the puppet wears, in Target::gear order: weapon, shield,
// head, armor, boots (reports/s2-equip-create: kinds 0, 6, 7, 8, 9), and the
// item in the hand slot (kind 5, the bow kind: 0x2df7b20 tests slot 3's link
// for it; the slingshot is a bow to the game).
//
// The rest (branch held-items-2) follows the game's own two enums, read from
// rodata. The 18 slot kinds the equip call takes (0x3ef9490): 0 Weapon,
// 1 SubWeapon, 2 ThirdWeapon, 3 Sheath, 4 Quiver, 5 Bow, 6 Shield,
// 7 ArmorHead, 8 ArmorUpper, 9 ArmorLower, 10 ArmorHand, 11 Tool, 12 SubTool,
// 13 to 16 Accessory1 to 4, 17 Extra. The 8 hand slots (0x3ef2000): 0 Weapon,
// 1 SubWeapon, 2 ThirdWeapon, 3 Bow, 4 Shield, 5 Tool, 6 SubTool, 7 Helmet.
// So the pieces after the held item are: the gauntlets (kind 10, no hand
// slot), and whatever sits in hand slots 1, 2, 5 and 6 (kinds 1, 2, 11, 12:
// which of the Deku stick, boomerang, hookshot, bottle and ocarina uses
// which is for a game run to show; all four are sent and worn the same way).
// The Ocarina (issue 152) is no hand slot: the player wears it as two
// accessories on the belt (Item_Ocarina_Fairy_Holder and Item_Ocarina_Fairy,
// or _Time; both packs have an AccessoryParam, and the player's
// EquipmentUserParam binds Accessory1 and Accessory4 to the bone Pod_B), and
// its playing animations take it in hand. The puppet wears the same two from
// PLAYER_EQUIP's ocarina, in the player's own slots: the Ocarina as
// Accessory1 (kind 13) and the holder as Accessory4 (kind 16). Seen in two
// Hookshot runs (held-ocarina-b, 2026-11-14): the player's EquipmentUser has
// the Ocarina in armor link 8 and the holder in link 12, and a puppet that
// wore them the other way round (holder kind 13, Ocarina kind 16) had the
// holder in link 8 and the Ocarina in link 12, so the link follows the kind.
// Worn swapped, the puppet's Ocarina sat in the holder's slot, and players
// saw it vanish (or something else in the hand) while it played.
inline constexpr std::size_t kGearCount = 13;
inline constexpr std::size_t kGearHead = 2;  // the mask
inline constexpr std::size_t kGearHeld = 5;  // index of the held item
inline constexpr std::size_t kGearHand = 6;  // the gauntlets
inline constexpr std::size_t kGearSubWeapon = 7;
inline constexpr std::size_t kGearThirdWeapon = 8;
inline constexpr std::size_t kGearTool = 9;
inline constexpr std::size_t kGearSubTool = 10;
inline constexpr std::size_t kGearOcarinaHolder = 11;
inline constexpr std::size_t kGearOcarina = 12;
using Gear = std::array<std::optional<std::string>, kGearCount>;
inline constexpr std::array<std::uint32_t, kGearCount> kGearKinds{0, 6, 7, 8, 9, 5,
                                                                  10, 1, 2, 11, 12, 16, 13};
inline constexpr std::array<const char*, kGearCount> kGearNames{
    "weapon", "shield",     "head",         "armor", "boots",   "held",
    "hand",   "sub weapon", "third weapon", "tool",  "sub tool", "ocarina holder", "ocarina"};
// The child's Goron's Bracelet comes as `hand` too (PlayerReader reads it
// from the player's own EquipmentUser), but the game wears it as an
// accessory, not as hand armor: its pack has an AccessoryParam (the adult
// gauntlets an ArmorParam), and the GameData equip path 0x2df6c80 wears
// `Armor_Child_Gauntlet_Goron` with kind 14 (Accessory2, which
// Player_Child's EquipmentUserParam binds to the bone Ph_Bangle). Worn with
// kind 10 it would go to the hand armor link, not where the player has it.
inline constexpr std::uint32_t kBraceletKind = 14;  // Accessory2
// The kind piece `i` named `name` is worn with.
constexpr std::uint32_t GearKind(std::size_t i, std::string_view name) {
    return i == kGearHand && name.starts_with(coop::protocol::kChildBraceletPrefix)
               ? kBraceletKind
               : kGearKinds[i];
}
// Armor (mask, tunic, boots, gauntlets): taken off with addr::kUnequipArmor.
// The bracelet is no armor: it is taken off as the belt pieces are.
constexpr bool GearIsArmor(std::size_t i, std::string_view name) {
    return i < kGearCount && GearKind(i, name) >= addr::kArmorKindFirst &&
           GearKind(i, name) <= addr::kArmorKindLast;
}
// Pieces the game keeps elsewhere while they are put away (seen for the
// slingshot: state 6), so the left-behind check looks at them only in hand.
constexpr bool GearIsCarried(std::size_t i) {
    return i == kGearHeld || (i >= kGearSubWeapon && i <= kGearSubTool);
}
// A piece the game must not make on a puppet (issue 232, 2026-11-16). Worn on
// the puppet, `Weapon_Range_Hookshot` takes the other game down a few seconds
// later: the game's own update of that piece reads a pointer a puppet does not
// have (guest 0x7d8 on BaseProcExecutor, the same missing-Player read patch
// 0002's null page turns into 0 for the create) and the read faults. Seen
// three times in a row, on the frame after `wears held
// 'Weapon_Range_Hookshot' (returned 0x1)`, with every other piece of the same
// player worn fine, and the player never fired it: the piece is stepped by the
// game and cannot follow the puppet, parked or not. The Longshot is the same
// actor family. By name and not by slot: it is the piece the game steps,
// whichever slot sends it.
//
// Root cause (held-piece, 2026-11-16): the read is the one every held piece
// makes (f_7102dcee60 after a held-item change, the wearer's Player
// component at component table +0x358, which a puppet has not). In the
// 0.11.1 to 0.11.3 packs a puppet carries the perception's Physics in its
// component slot 10, so patch 0002's guard (armed by a Link with no slot 10)
// never armed the null page and the read faulted. The module now arms that
// page itself before its first puppet (hookshot/src/coop_null_page.h), and
// the emulator reads 0 there anyway. So such a piece is left off a puppet
// only where null reads cannot give 0 (Director::SetNullReadsZero(false));
// LICHII_COOP_PUPPET_HOOKSHOT=0 leaves it off, =1 wears it, everywhere.
constexpr bool GearIsUnsafe(std::string_view name) {
    return name.find("Hookshot") != std::string_view::npos ||
           name.find("Longshot") != std::string_view::npos;
}
// Row 198 (0.11.9): a coloured copy the puppet wears (`<item>_Look<k>`, or a
// wearer copy `<item>_Blade<n>`) that the game did not make: the piece in its
// equipment slot is another actor (`made`, the game's own item kept from
// before). The equip call returns 1 all the same: the 0.11.9 pack sized the
// copies' AnimationParam too small, the game refused every shield copy, and a
// puppet kept the game's shield, drawn in the viewer's own colours. Said once
// per piece so a refused copy is seen in the log.
constexpr bool CopyRefused(std::string_view worn, std::string_view made) {
    const bool copy = worn.find("_Look") != std::string_view::npos ||
                      worn.find("_Blade") != std::string_view::npos;
    return copy && !made.empty() && made != worn;
}
// The equipment slot each of them lands in (seen in the game: the sword in
// slot 0, the shield in slot 4, the slingshot in slot 3); -1: not a piece
// that is drawn or put away.
inline constexpr std::array<int, kGearCount> kGearSlots{0, 4, -1, -1, -1, 3, -1, 1, 2, 5, 6, -1, -1};

// A puppet's memory block, from the game's malloc, kept for the whole run.
// 0.11.9: the look part of the eyes' colour (CoopLook::Part::eyes), dressed on the puppets by co-op's wardrobe.
inline constexpr std::uint8_t kEyesPart = 18;
inline constexpr std::uint8_t kHairPart = 19;   // (CoopLook::Part::hair)

namespace block {
inline constexpr std::uint64_t kSize = 0x880;  // to 0x778: coop_puppet_flame.h's key and handle; then kDress
// Character Select's colours on the puppet (coop_char_colours.h: names and arguments of its calls).
inline constexpr std::uint64_t kDress = 0x780;      // CoopCharColours::kCellBytes
inline constexpr std::uint64_t kCreateArg = 0x000;  // 0x100 bytes
inline constexpr std::uint64_t kName = 0x100;       // {char*, u32 capacity}
inline constexpr std::uint64_t kHandles = 0x110;    // kHandleCount slots of 8 bytes
inline constexpr std::uint64_t kHandleCount = 6;
inline constexpr std::uint64_t kNameText = 0x140;   // kNameCapacity bytes
inline constexpr std::uint32_t kNameCapacity = 0x40;
// The play request for 0x2022a0: +0 actor, +8 name, +0x10 slot, +0x14 rate,
// +0x18 and +0x1c -1.0, +0x20 skip-if-same, +0x28 node (0, as 0x2a95c4).
inline constexpr std::uint64_t kPlayRequest = 0x200;  // 0x30 bytes
// Two name buffers, used in turn: the slot may keep the pointer it was
// given, and a new name in the same buffer would look like the same one.
inline constexpr std::uint64_t kAnimText = 0x240;
inline constexpr std::uint64_t kAnimCapacity = 0x50;  // 64 characters and the 0
// One name cell per gear kind (kGearKinds order), and the char* to each that
// the equip call is given.
inline constexpr std::uint64_t kGearText = 0x300;
inline constexpr std::uint64_t kGearCapacity = 0x40;
inline constexpr std::uint64_t kGearPtr = 0x640;  // kGearCount x 8 bytes (to 0x6a8)
// The EquipmentUser, for the head show call (addr::kShowHeadArmor reads it at
// owner + 0x470: the call is given this cell's address minus 0x470).
inline constexpr std::uint64_t kHeadOwnerEu = 0x6a8;  // 8 bytes (then coop_puppet_sound.h's)
}  // namespace block

// Guest memory, as the director sees it. Failures are quiet.
class Guest {
public:
    virtual ~Guest() = default;
    virtual bool Read(std::uint64_t address, void* out, std::size_t size) = 0;
    virtual bool Write(std::uint64_t address, const void* in, std::size_t size) = 0;
};

// One game function for the cave to call.
struct Call {
    std::uint64_t function = 0;          // absolute
    std::array<std::uint64_t, 8> x{};    // x0 to x7
    std::array<std::uint64_t, 4> stack{};// what the function sees at sp
    float s0 = 0.0f;                     // the first float argument
    // The call runs a copy's own game code (its AI: the native boss sync's brain and action calls): what the game
    // creates or deletes inside it is the game's own doing, judged as any (a copy enemy's own shot or minion is
    // stopped), not the module's own (CoopActorEvents::Ours).
    bool copy_game = false;
};

// A remote player's animation, as PLAYER_SAMPLE field 7 carries it.
struct Anim {
    std::string name;  // the AS command
    float frame = 0.0f;
    float rate = 1.0f;
};

// Whether a sample's frame follows its player's animation, given the sample before it (same slot). A playing pose
// (rate above 0) whose frame stands still from one sample to the next (same command, the same frame) is sent at a
// fixed frame: the sender does not follow the frame (the carried pose of a lifted player, coop_lift_player.h
// CarriedAnimation, sends frame 0 at rate 1). The copy then plays on at the rate, and its frame is not set back to
// the sample's (which would snap the loop back to frame 0 on every sample). A real player's playing animation
// moves on between samples, so its frame is followed as before; a new command or the first sample is followed.
inline bool FrameFollowed(const std::string& before_name, float before_frame, const Anim& now) {
    return !(now.rate > 0.0f && !before_name.empty() && now.name == before_name && now.frame == before_frame);
}

// coop/host/scene.hpp's Visibility, for a puppet that exists.
enum class Show { show, hold, hide };

// What one puppet should show.
struct Target {
    std::uint8_t number = 0;  // the player; 0 is the debug puppet
    std::string actor;        // the actor pack name
    Vec3 position{};
    // jitter-b (coop_follow.h): how fast `position` moves, metres a second, as of
    // Input::made_ms. Unset (local players, the debug puppet): drawn where `position` is.
    std::optional<Vec3> velocity;
    Mat3 rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    // jitter-3: the same rotation as a quaternion, as of Input::made_ms (remote players only):
    // followed to each game frame (coop_follow.h Turner). Unset: `rotation` as it is.
    std::optional<Quat> turn;
    Show show = Show::show;   // hold: frozen as it is; hide: parked out of sight
    const char* why = "";     // for the log when `show` changes ("our cutscene", ...); string literals only
    std::optional<Anim> anim;
    std::uint32_t anim_serial = 0;  // goes up with each received animation
    // What the player wears, in kGearKinds order; unset: not known yet,
    // "" nothing (not taken off yet; the puppet keeps what it has).
    Gear gear;
    // PLAYER_EQUIP's drawn bits: bit i set when the player's equipment slot i
    // is in hand (sword out, shield on the arm). Unset: not known.
    std::optional<std::uint8_t> drawn;
    // PLAYER_EQUIP's lit bits (issue 248): bit i set when the piece in their
    // slot i burns. Unset: not known (an older build), as 0.
    std::optional<std::uint8_t> lit;
    // The player's colour, 0xRRGGBB (coop_colors.h): the puppet is tinted
    // with it while live. Unset: no tint.
    std::optional<std::uint32_t> color;
    // Character Select: for a character's puppet, what its colour routine is given (Director::Dress): the
    // player's Customize options (CoopLookShare::CustomOf, bits 0 to 23) and their character from bit 32.
    // 0: no character's puppet.
    std::uint64_t custom = 0;
    // The player's newest OCARINA_NOTE, played on the puppet (issue 152).
    CoopPuppetSound::Wanted ocarina;
    // LINK_SOUNDs received since the last frame, near enough to hear
    // (coop_link_sound.h): the director takes each one once.
    std::vector<CoopLinkSound::Sound> sounds;
};

struct Input {
    std::uint64_t main_base = 0;  // 0: no game, nothing is done
    std::string scene;            // the local scene; "" not known
    std::vector<Target> targets;
    // When the targets were worked out (steady ms; 0 not known): the director takes
    // each place forward from then to its own game frame (coop_follow.h).
    std::uint64_t made_ms = 0;
    // The local game's drawing, from the renderer's camera (coop_camera.h).
    // A puppet created while the game loads a scene (the scene is already
    // readable) is made but never drawn, so a create waits for the drawing
    // (Director::Drawn). `camera`: a camera has been seen since start; false
    // (no camera, or a test) creates at once, as before.
    bool camera = false;
    std::uint64_t drawn_since_ms = 0;  // the current unbroken run of drawn frames; 0 none
    // A create waits until the wanted actor has stayed the same this long:
    // while the other game loads, its age reads adult for a moment, and a
    // wrong-age puppet is made and dropped. 0 (a test) creates at once.
    std::uint64_t settle_ms = 0;
};

Mat3 QuatToMatrix(const Quat& q);  // rows; normalises q, identity if it is 0

enum class Phase { no_block, allocating, idle, creating, live, deleting };
const char* PhaseText(Phase phase);
const char* ShowText(Show show);  // "show", "hold", "hide"

class Director {
public:
    using Log = std::function<void(const std::string&)>;
    // `verbose`: a status line per puppet every second (LICHII_COOP_DEBUG_PUPPET).
    // `anim`: a line for each animation played or frame set (LICHII_COOP_DEBUG_ANIM).
    explicit Director(Log log = {}, bool verbose = false, bool anim = false);
    // The Link puppets' director runs the pose copy (coop_pose.h); others, such
    // as the fairies', do not.
    void SetPoseCopy(bool on) { pose_copy_ = on; }
    // Character Select's colour routine (PackCharacterColours): main's offset and the 16 bytes it begins with.
    // offset 0: none, no colours on a character's puppet.
    void SetCharacterApply(std::uint64_t offset, const std::array<std::uint8_t, 16>& head) {
        character_apply_ = offset;
        character_apply_head_ = head;
        character_apply_state_ = 0;
        dress_not_made_ = 0;
    }
    // Move each puppet with the game's own setter as well (addr::kSetMatrix),
    // once a frame: for an actor with a physics body, such as the fairy.
    void SetMatrixCall(bool on) { matrix_call_ = on; }
    // Tint each live puppet in its target's colour with this strength (the
    // alpha of the game's hit-flash colour, coop_colors.h); 0, the default,
    // writes no colour (the fairies' director).
    void SetTint(float strength) { tint_ = strength; }
    // Whether the game's reads through a puppet's missing components give 0
    // here (the emulator; Hookshot once its null page is armed). Without that
    // a GearIsUnsafe piece stays off the puppet (issue 232). Default: yes.
    // (Atomic: Hookshot's create hook may turn it off on another thread.)
    void SetNullReadsZero(bool on) { null_reads_zero_.store(on); }
    // A piece Equip leaves off the puppet: nothing is made for it, and the
    // piece its slot may still hold is neither put away nor re-worn.
    bool LeftOff(const std::string& name) const;
    // Tunic puppets (CoopPuppet_<Age>_<Colour>) found not to work: the mod is
    // a pack from before the colours (two create timeouts of that actor while
    // no tunic puppet has spawned yet), or one spawned without a Player_ model
    // (kTunicModelCheckMs). The planner uses the plain puppet for these. Any
    // thread.
    std::vector<std::string> FailedTunics() const {
        std::lock_guard lock(tunic_mutex_);
        return {tunic_failed_.begin(), tunic_failed_.end()};
    }
    // How long after a tunic puppet spawns its model is checked.
    static constexpr std::uint64_t kTunicModelCheckMs = 3000;
    // CoopPuppet_<Age>_<Colour>, not the plain CoopPuppet_<Age>.
    static bool IsTunicActor(std::string_view name) {
        constexpr std::string_view kBase = "CoopPuppet_";
        return name.substr(0, kBase.size()) == kBase &&
               name.find('_', kBase.size()) != std::string_view::npos;
    }

    // Any thread. The next frame of the cave uses it.
    void SetInput(Input input);

    // The game's calc thread, at each `svc #0xf4`. `after_call`: the cave
    // made the call returned last time and `result` is its x0. Returns the
    // next call, or nothing for "done for this frame".
    std::optional<Call> Step(Guest& guest, bool after_call, std::uint64_t result,
                             std::uint64_t now_ms);

    struct SlotView {
        std::uint8_t number = 0;
        Phase phase = Phase::no_block;
        std::uint64_t block = 0;
        std::uint64_t actor = 0;
        std::string actor_name;
        std::string anim_name;  // what was last played on it
        std::size_t strays = 0; // actors of its player still being deleted
    };
    std::vector<SlotView> Slots() const;  // the calc thread, or tests
    // Every live puppet, for co-op's own Customize colours (coop_char_colours.h): its actor, its block's dress
    // cells, its player's options (Target::custom, 0 for Link) and whether it is a character's puppet that the
    // mod's own routine dresses (Dress, verified at its place). The calc thread.
    struct DressView {
        std::uint8_t number = 0;
        std::uint64_t actor = 0;
        std::uint64_t cell = 0;
        std::uint32_t custom = 0;
        bool character = false;
        bool mods_routine = false;
        std::string actor_name;   // (a puppet made again as another actor starts anew)
        std::optional<std::uint32_t> eyes;   // 0.11.9: the player's eye colour (their look's part 18), exact
        std::optional<std::uint32_t> hair;   // 0.11.9: the player's hair colour (part 19), a Link puppet's only
    };
    std::vector<DressView> DressTargets() const;
    // Any thread: bit n set while puppet n's actor is live in this scene and
    // shown or held (not parked for a hide), as of the last calc frame. The
    // fairies follow it, so a Navi never shows without her puppet (issue 145).
    std::uint32_t LiveMask() const { return live_mask_.load(std::memory_order_relaxed); }
    // Any thread (native puppets phase 4, coop_pvp_native.h): player `number`'s copy actor while it is a live,
    // present `_Pvp` actor in this scene, as of the last calc frame; 0 otherwise (a plain copy, none, a load).
    std::uint64_t LivePvpActor(std::uint8_t number) const {
        return number < live_pvp_actor_.size() ? live_pvp_actor_[number].load(std::memory_order_relaxed) : 0;
    }
    // Any thread (row 177): player `number`'s puppet actor while it is live, shown and seen by enemies here (a
    // pack with perception, not its quiet copy), as of the last calc frame; 0 otherwise. A hit that player made
    // on an enemy of ours names it as the attacker (CoopHitReplay::ReplayAttacker).
    std::uint64_t LiveSeenActor(std::uint8_t number) const {
        return number < live_seen_actor_.size() ? live_seen_actor_[number].load(std::memory_order_relaxed) : 0;
    }
    // The pack's puppets carry a PerceptionEmitter (PlannerSettings::perceived).
    void SetPerceived(bool on) { perceived_.store(on); }
    static_assert(coop::protocol::kMaxPlayers < 32, "LiveMask has one bit per player number");
    std::uint64_t Frames() const { return frames_; }
    void Note(const std::string& text) const { Say(text); }  // any thread

    static constexpr int kMaxCallsPerFrame = 8;      // the cave allows 16
    static constexpr std::uint64_t kCreateTimeoutMs = 10000;
    // A create the game refuses (it returns 0: the scene is still being
    // unloaded or loaded) made nothing; it is asked again after this, not
    // after the create timeout and the 5 s wait.
    static constexpr std::uint64_t kRefusedRetryMs = 1000;
    // After this many refusals in a row (a long load or cutscene), every 5 s.
    static constexpr int kRefusedFast = 3;
    static constexpr std::uint64_t kRefusedSlowMs = 5000;
    // A tunic actor (CoopPuppet_<Age>_<Colour>) refused this many times in a
    // row, before any tunic puppet spawned, is not in the pack: the plain
    // puppet from then on (about 3 x 1 s + 3 x 5 s, longer than a load).
    static constexpr int kRefusedTunicFallback = 6;
    // A live puppet whose player drops out of the plan in the same scene
    // (a stalled or late peer fades to 0 for a moment) is kept this long
    // before it is deleted, so a hiccup does not delete and create it again.
    static constexpr std::uint64_t kLeaveGraceMs = 2000;
    // A received frame further than this from the puppet's is set on it.
    static constexpr float kFrameSlack = 3.0f;
    // Backstop (puppet-anim, player reports #36/#41): on the AS path the
    // command the puppet's own slot 0 plays is read this often; found other
    // than its player's at kBackstopMisses reads in a row (a play the game
    // dropped, or a one-shot it ended and moved on from), it is played again.
    static constexpr std::uint64_t kBackstopMs = 750;
    static constexpr int kBackstopMisses = 2;
    // A hidden puppet is moved this far down, under the floor.
    static constexpr float kHideDrop = 500.0f;
    // A worn piece this far from its shown puppet was left behind (made while
    // the puppet was parked under the floor, seen 2026-11-09): worn again.
    static constexpr float kGearFar = 5.0f;
    // A piece set in hand or put away is checked against the game this often.
    static constexpr std::uint64_t kHoldCheckMs = 500;
    static constexpr std::uint64_t kRewearMs = 1000;  // at most once a second
    static constexpr std::uint64_t kFarForMs = 100;   // far on two readings this far apart
    static constexpr std::uint64_t kRefusedForMs = 2000;   // a copy not made on readings this far apart: said
    static constexpr std::uint8_t kRewearTries = 3;   // then given up, with one line
    static constexpr std::uint64_t kTakeOffMs = 1500;  // a slot empty this long: the piece is taken off
    // A create waits until the game has drawn this long without a break,
    // begun after the last scene change or puppet loss (a load drops the
    // camera)...
    static constexpr std::uint64_t kDrawnMs = 750;
    // ...or, when the drawing never broke, this long after it.
    static constexpr std::uint64_t kSteadyMs = 3000;
    // A stray (an actor of a puppet's player that is not its puppet any more:
    // a create that timed out while its actor still loaded, a delete that did
    // not finish) is asked to go with the game's own delete this often until
    // it is gone (owner's playtest 2026-11-14: two puppets of one player).
    static constexpr std::uint64_t kStrayDeleteMs = 1000;
    // ...at most this many times (half a minute), then let go with a log line.
    static constexpr int kStrayMaxDeletes = 30;

private:
    struct Slot {
        std::uint8_t number = 0;
        Phase phase = Phase::no_block;
        std::uint64_t block = 0;
        std::uint64_t handle = 0;   // the handle slot of the current create
        std::uint64_t handle_index = 0;
        std::uint64_t actor = 0;    // resolved this frame; 0 none
        std::uint64_t request = 0;  // what the handle slot held
        std::uint64_t taken = 0;    // the actor 0x36fbd0 gave
        std::uint64_t vtable = 0;   // its vtable then, to notice reuse
        bool take = false;          // call 0x36fbd0 next
        bool drop = false;          // call 0x36fde0 next
        std::string actor_name;     // what was created
        std::string scene;          // where it was created
        std::uint64_t since_ms = 0; // when the phase began
        std::uint64_t retry_ms = 0; // no new malloc or create before this
        std::uint64_t missing_ms = 0; // when its player left the plan; 0 wanted
        std::uint64_t lost_ms = 0;    // when the engine deleted it (a load, often)
        bool wait_said = false;       // "waiting for the game to draw" logged
        std::string wanted_actor;     // the actor its target asked for, last seen
        std::uint64_t wanted_since_ms = 0;  // since when (Input::settle_ms)
        int refused = 0;                    // creates refused in a row
        std::string anim_name;        // played on it; "" none yet
        CoopOcarina::HandBind ocarina_hand;  // issue 152: Ocarina in hand while it plays
        std::string their_anim;              // their last AS command (debug log)
        std::uint32_t anim_serial = 0;
        std::string sample_name;     // the last sample's command and frame (FrameFollowed)
        float sample_frame = 0.0f;
        std::uint64_t anim_buffer = 0;  // which name buffer is next
        std::optional<float> frame_to_set;  // after a play
        std::optional<float> held_frame;    // while held
        std::uint64_t backstop_ms = 0;      // next backstop read (kBackstopMs)
        int backstop_misses = 0;            // reads in a row that found another command
        unsigned backstops = 0;             // times the backstop played it again (logged)
        std::string cut_refused;            // their cutscene's command the puppet lacks (this hold)
        std::uint64_t cut_verify_frame = 0; // a hold reads its play back from this frame
        CoopFollow::Follower follow;        // jitter-b: their place taken to this game frame
        CoopFollow::Steps steps;            // jitter-2: how even its drawn steps are (logged)
        CoopFollow::Follower raw_follow;    // jitter-2: the same on the raw frame time (not drawn;
        CoopFollow::Steps raw_steps;        //   the log's before number)
        CoopFollow::Turner turner;          // jitter-3: their rotation taken to this game frame
        CoopFollow::Stillness stillness;    // idle-twitch: how still it is drawn while they stand (logged)
        CoopSnap::Smoother smooth;          // issue 194: where it is drawn, jumps eased
        CoopSnap::Meter meter;              // issue 194: its place read back, snaps logged
        bool meter_skip = false;            // the next read is still the parked spot
        Show shown = Show::show;            // the last `show` acted on (log on change)
        bool model_checked = false;         // a tunic puppet's model (kTunicModelCheckMs)
        bool native = false;                // a real Player_<Age> (coop_native_player.h): left alone
        CoopNativePlayer::Probe probe;      // what the game does with it, for the log
        std::uint64_t frame_set_in = 0;     // the cave frame of the last frame set
        std::string play_name;              // the play being made
        float play_frame = 0.0f;
        std::uint32_t play_serial = 0;
        std::int32_t sync_next = 0;         // next slot to sync from slot 0; 0 none
        std::uint64_t pose_log_ms = 0;      // last read-back line (debug anim)
        std::array<std::string, kGearCount> worn;    // equipped on it, by gear index
        std::size_t equip_index = 0;        // the equip being made
        std::string equip_name;
        std::uint64_t gear_log_ms = 0;      // last gear line (debug anim)
        std::uint8_t in_hand = 0;           // equipment slots asked into the hand (bits)
        std::uint8_t hold_known = 0;        // slots whose state we have set (bits)
        int hold_slot = 0;                  // the change being made
        bool hold_in_hand = false;
        std::uint64_t hold_retry_ms = 0;    // no new change before this
        std::uint64_t hold_check_ms = 0;    // no state check (kEquipState) before this
        std::size_t hold_check_next = 0;    // the gear piece the next check starts from
        std::uint8_t hold_said = 0;         // slots whose "set again" was logged and not yet held (bits)
        std::array<std::uint32_t, kGearCount> hold_id{};  // the piece each change was made for
        std::uint64_t tint_actor = 0;       // the actor the colour line was logged for
        std::uint32_t tint_color = 0;       // and its colour
        std::uint64_t rewear_ms = 0;        // no new "left behind" check before this
        std::array<std::uint64_t, kGearCount> far_since_ms{};  // first far reading of each piece; 0 near
        std::array<std::uint8_t, kGearCount> rewear_tries{};   // re-wears since the piece was last near
        std::array<std::uint64_t, kGearCount> refused_since_ms{};  // first reading of a copy not made (CopyRefused)
        std::array<std::string, kGearCount> refused_said{};         // the copy whose refusal was said
        // A worn piece its player took off: armor (mask, tunic, boots,
        // gauntlets) with the game's own call (addr::kUnequipArmor); a sword
        // or a shield, for which no call is known, or armor the call
        // refused, makes the puppet again without it (Equip, issue 91).
        bool remake = false;
        std::array<std::uint64_t, kGearCount> off_since_ms{};  // its player's slot empty since (0: not)
        std::array<bool, kGearCount> unequip_refused{};        // the take-off call returned 0
        // The head piece worn and not yet shown (addr::kShowHeadArmor, #91),
        // and the hair to put back after a head piece was taken off.
        bool head_show = false;
        bool head_shown = false;        // the show call was made for the head piece worn
        bool head_hair_back = false;
        std::uint64_t moved_frame = 0;      // the cave frame of the last matrix call
        CoopPuppetSound::State sound;       // the player's Ocarina on it (issue 152)
        CoopLinkSound::Player link_sound;   // the player's own sounds on it (issues 131, 152)
        CoopPuppetFlame::State flame;       // their lit Deku stick's flame on it (issue 248)
        // Character Select: what the colour routine was last given for this actor (Target::custom), when,
        // and what the call under way gives it.
        std::optional<std::uint64_t> dressed;
        std::uint64_t dress_ms = 0;
        std::uint64_t dress_asked = 0;
        // Actors made for this player that are not `taken` any more (kStrayDeleteMs):
        // deleted until gone, so there is never a second puppet of one player.
        struct Stray {
            std::uint64_t actor = 0;
            std::uint64_t vtable = 0;  // as taken: a reused address is left alone
            std::string name;          // the actor made, read back when the game has it
            std::uint64_t next_ms = 0; // the next delete call
            int deletes = 0;
        };
        std::vector<Stray> strays;
    };
    enum class Pending {
        none, malloc, create, take, drop, remove, play, frame, sync, equip, unequip, hold,
        hold_check, move, sound, link_sound, stray, own_state, head_show, head_hair, flame, dress
    };
    // sword-3: once a frame, the game's state of one of the local Link's own
    // slots (CoopPuppetOwn::kSlots, in turn), for the sender (coop_puppet_own.h).
    std::optional<Call> OwnState(Guest& guest);
    // The slot's taken actor becomes a stray (above); the slot then has none.
    void Abandon(Slot& slot, Guest& guest, std::uint64_t now_ms, const char* why);
    // Strays gone, reused or given up are dropped; the next delete call for one, if due.
    std::string StrayCheck(const Slot::Stray& s, Guest& guest) const;
    void KeepStrays(Slot& slot, Guest& guest);
    std::optional<Call> DeleteStray(Slot& slot, Guest& guest, std::uint64_t now_ms);

    void NewFrame(Guest& guest, std::uint64_t now_ms);
    void PoseFrame(Guest& guest, std::uint64_t now_ms);
    void Finish(Guest& guest, std::uint64_t result, std::uint64_t now_ms);
    std::optional<Call> Next(Slot& slot, Guest& guest, std::uint64_t now_ms);
    std::optional<Call> Create(Slot& slot, const Target& target, Guest& guest);
    std::uint64_t Resolve(Slot& slot, Guest& guest);
    std::optional<Call> Animate(Slot& slot, const Target& target, Guest& guest,
                                std::uint64_t now_ms);
    // The command slot 0 of `controller` plays (slot +0x10), when readable.
    static std::optional<std::string> Playing(Guest& guest, std::uint64_t controller);
    std::optional<Call> Equip(Slot& slot, const Target& target, Guest& guest,
                              std::uint64_t now_ms);
    // Character Select: the player's Customize options on a character's puppet, by that mod's own routine
    // in the game's code (cz_puppet: x0 the actor, x1 Target::custom). Called when they change, and again
    // every kDressAgainMs while any is set (a model that was not loaded yet). Never when the bytes at its
    // place are not the ones the pack names (another build of the mod, or its code is not in the game).
    std::optional<Call> Dress(Slot& slot, const Target& target, Guest& guest, std::uint64_t now_ms);
    static constexpr std::uint64_t kDressAgainMs = 3000;
    std::optional<Call> Hold(Slot& slot, const Target& target, Guest& guest,
                             std::uint64_t now_ms);
    void LeftBehind(Slot& slot, Guest& guest, std::uint64_t now_ms);
    void RefusedCopy(Slot& slot, Guest& guest, std::size_t i, std::uint64_t piece, std::uint64_t now_ms);
    std::optional<Call> Move(Slot& slot, Guest& guest);
    std::optional<Call> SetFrame(Slot& slot, std::uint64_t controller, float frame);
    static std::uint64_t Controller(Guest& guest, std::uint64_t actor);
    static std::optional<float> Frame(Guest& guest, std::uint64_t controller);
    // The colour into the live puppet's model (coop_colors.h), every frame
    // it is not there yet (a new actor, or the game wrote its own).
    void Tint(Slot& slot, Guest& guest, std::uint32_t color);
    void Pose(Slot& slot, Guest& guest, const Vec3& position, const Mat3* rotation,
              std::uint64_t now_ms);
    // Park the puppet under the floor with everything it holds: a held piece is an
    // actor of its own (the Hookshot's claw is bound to a bone, a bottle to the hand),
    // and moving only the puppet leaves the pieces where they were. The game keeps
    // stepping them, and one whose owner it can no longer follow crashes it
    // (0xc0000005 reading a null + 0x7d8, seen 2026-11-15 while the other player
    // changed to the Hookshot as the puppet came and went).
    void Park(Slot& slot, Guest& guest, const Vec3& position, std::uint64_t now_ms);
    // The actors the game keeps for the puppet's equipment slots (a worn piece, a
    // held one); empty when it has no EquipmentUser yet.
    struct PieceActor {
        std::uint64_t actor = 0;
        std::uint32_t state = 0;
    };
    std::vector<PieceActor> Pieces(Slot& slot, Guest& guest) const;
    void DrawAt(Slot& slot, Guest& guest, const Target& target, std::uint64_t now_ms);
    void Poll(Slot& slot, Guest& guest, std::uint64_t now_ms);
    const Target* Wanted(std::uint8_t number) const;
    bool Drawn(const Slot& slot, std::uint64_t now_ms) const;
    Slot& SlotFor(std::uint8_t number);
    void Say(const std::string& text) const;
    void Status(Guest& guest, std::uint64_t now_ms);

    Log log_;
    bool verbose_;
    bool anim_;
    bool pose_copy_ = false;
    std::uint64_t character_apply_ = 0;
    std::array<std::uint8_t, 16> character_apply_head_{};
    int character_apply_state_ = 0;   // 0 not checked, 1 the routine is there, 2 it is not (or cannot be called)
    // Its calls in a row that the runtime could not make (a routine that is not a game function the build knows):
    // at kDressNotMade co-op's own dressing takes over (DressTargets).
    int dress_not_made_ = 0;
    static constexpr int kDressNotMade = 2;
    bool matrix_call_ = false;
    float tint_ = 0.0f;
    std::atomic<bool> null_reads_zero_{true};
    mutable std::mutex tunic_mutex_;
    std::set<std::string> tunic_failed_;           // guarded by tunic_mutex_
    std::map<std::string, int> tunic_timeouts_;    // by actor name
    bool tunic_proven_ = false;  // a tunic puppet spawned with its model: the pack has them
    void TunicFails(const std::string& actor, const std::string& why);

    mutable std::mutex input_mutex_;
    Input next_input_;  // guarded

    // The calc thread only.
    Input input_;
    CoopFollow::Cadence cadence_;  // jitter-2: the game frames' time made even (coop_follow.h)
    std::vector<Slot> slots_;
    CoopLinkSound::Budget sound_budget_;  // every puppet's sound starts together
    Pending pending_ = Pending::none;
    std::uint8_t pending_slot_ = 0;
    std::uint64_t stray_actor_ = 0;  // the stray a Pending::stray call deletes
    std::uint64_t stray_frame_ = ~std::uint64_t{0};  // the frame of the last stray delete
    int calls_ = 0;  // this frame
    bool own_asked_ = false;      // OwnState asked this frame
    std::size_t own_next_ = 0;    // the CoopPuppetOwn::kSlots index asked next
    std::size_t own_index_ = 0;   // the one a Pending::own_state call asks
    std::size_t first_slot_ = 0;  // where this frame's Step starts (round robin)
    std::uint64_t frames_ = 0;
    std::atomic<std::uint32_t> live_mask_{0};  // LiveMask(); written by the calc thread
    std::array<std::atomic<std::uint64_t>, coop::protocol::kMaxPlayers + 1> live_pvp_actor_{};  // LivePvpActor()
    std::array<std::atomic<std::uint64_t>, coop::protocol::kMaxPlayers + 1> live_seen_actor_{};  // LiveSeenActor()
    std::atomic<bool> perceived_{false};  // SetPerceived
    std::uint64_t frames_at_status_ = 0;
    std::uint64_t status_ms_ = 0;
    std::string scene_;               // input_.scene as last seen
    std::uint64_t scene_since_ms_ = 0;
};

// The present thread's side: what to show this frame.
// A player's PLAYER_EQUIP as the puppet's pieces, in kGearKinds order.
Gear GearOf(const coop::protocol::Equipment& equipment);

struct Remote {
    std::uint8_t number = 0;
    std::optional<std::string> scene;
    Vec3 position{};
    std::optional<Vec3> velocity;  // presence's, metres a second
    std::optional<Quat> rotation;
    float fade = 1.0f;
    std::uint8_t status = 0;  // PLAYER_STATUS flags
    bool adult = false;
    // Presence says child (native puppets phase 3: only a young Link is liftable). Age not known: false.
    bool young = false;
    std::optional<Anim> anim;
    std::uint32_t anim_serial = 0;
    Gear gear;  // as Target::gear
    std::optional<std::uint8_t> drawn;               // as Target::drawn
    std::optional<std::uint8_t> lit;                 // as Target::lit
    // PLAYER_EQUIP's colour; unset: the player chose none, and Plan gives the
    // player's slot colour (coop::protocol::slot_color).
    std::optional<std::uint32_t> color;
    // 0.11.9: the tunic copy given to this player (SetTunicCopies: "Red".."Black", "" the plain puppet), repainted
    // live in their exact colour; none: the copy nearest their colour, as before.
    std::optional<std::string> tunic_copy;
    CoopPuppetSound::Wanted ocarina;                 // as Target::ocarina
    std::vector<CoopLinkSound::Sound> sounds;        // received since the last Collect
    // The item copy painted in the player's gear colours
    // (CoopLookShare::CopyOf): their puppet wears it.
    std::optional<std::size_t> look_copy;
    // The character they picked in the Character Select mod
    // (CoopLookShare::CharacterOf): 0 is Link.
    std::uint16_t character = 0;
    // ... and their Customize options there (CoopLookShare::CustomOf): 0 is none set.
    std::uint32_t custom = 0;
};

struct PlannerSettings {
    // The actor pack; "" picks CoopPuppet_Child or CoopPuppet_Adult by the
    // player's age (LICHII_COOP_PUPPET_ACTOR overrides it).
    std::string actor;
    bool debug_anim = false;      // LICHII_COOP_DEBUG_ANIM
    // Each player's tunic in their colour (docs/puppet.md, "Colour"): the
    // puppet pack's CoopPuppet_<Age>_<Colour>. LICHII_COOP_TUNICS=0: off.
    bool tunics = true;
    // The pack has the green puppets (CoopPuppetConfig): the plain puppet's
    // model is your own Link's (the kit's colour-agree copy, whatever colour
    // you wear), so it would wear your tunic; a green player, and any puppet
    // whose colour is off or failed, gets CoopPuppet_<Age>_Green instead.
    bool green_puppet = false;
    // LICHII_COOP_NATIVE_PLAYER (a test, coop_native_player.h): the other
    // player is the game's own Player_<Age>, made and then left alone.
    bool native_player = false;
    bool debug = false;           // LICHII_COOP_TEST_PUPPET: one more, beside you,
                                  // wearing your own gear
    std::uint64_t debug_delay_ms = 3000;  // after the scene is known
    Vec3 debug_offset{2.0f, 0.0f, 0.0f};  // from you, in game units (about metres)
    // Gear colours on the puppets (docs/puppet.md, "Gear colours on the
    // puppets"): the items the pack has copies of, and how many copies
    // (PackLookItems). None: every puppet wears the game's items.
    std::vector<std::string> look_items;
    std::size_t look_copies = 0;
    std::size_t look_neutral = 0;   // (row 198: LookItems::neutral; 0 none)
    // Wearer copies (coop_puppet.json "blade_items", look_items.is_wearer_item):
    // the items the pack has one copy of per player number (every player item
    // with the game's weapon component: swords, hammer, Deku Stick, bottles,
    // fishing rod), and how many numbers. That component writes the model's
    // shader values from the wearer every frame (a sword's blade), into the
    // model resource every actor of one name shares, so a puppet in the
    // game's own sword showed its blade only while our own sword was out. A
    // puppet with no ready Look copy wears `<item>_Blade<its number>`
    // (DressInBlades): a resource of its own, on every runtime, for every
    // player number. LICHII_COOP_BLADE_COPIES=0: none.
    std::vector<std::string> blade_items;
    std::size_t blade_copies = 0;
    // Scenes with stealth guards (coop_puppet.json "quiet_scenes",
    // tools/puppet_pack/build.py guard_scenes): there each puppet is its
    // quiet copy (QuietActor), which no perception sees, so a guard of this
    // game never takes another player's puppet for our Link (issue 150).
    std::vector<std::string> quiet_scenes;
    // Every scene, not only quiet_scenes: the runtime cannot make the reads of
    // a perceived puppet's missing Player component give 0 (Hookshot's null
    // page cannot be armed, hookshot/src/coop_null_page.h), so no puppet
    // carries the perception's Physics.
    bool quiet_everywhere = false;
    // Reads through a puppet's missing components give 0 (the emulator, or
    // Hookshot's null page armable): Director::SetNullReadsZero.
    bool null_reads_zero = true;
    // The puppets the pack has for the characters of the Character Select mod
    // (PackCharacterActors, sorted): a player who picked one is shown as
    // CharacterActor(their age, the pack's file of their character) when it is
    // among them.
    std::vector<std::string> character_actors;
    // The pack's map of the mod's stored character numbers to its model files
    // (PackCharacterNumbers): a number it does not list shows the colour puppet.
    std::map<std::uint16_t, std::uint16_t> character_files;
    // That mod's colour routine for a puppet (PackCharacterColours): main's offset (0: none) and the
    // 16 bytes it begins with.
    std::uint64_t character_apply = 0;
    std::array<std::uint8_t, 16> character_apply_head{};
    // The pack was built with perception (PackPerception): its plain (not quiet) puppets are seen by enemies.
    bool perceived = false;
};

// Character Select (github.com/snaildev99/OoT-Character-Select): a player there
// is one of over a hundred characters, each a player model file of that mod,
// Model/Player_<Age>_C<file>.Player_<Age>.bfres. The player's pick (the mod's
// saved Int, sent as is: the mod's own id of the character) is not the file:
// the mod's characters.json maps each number to its file, and tools/puppet_pack
// writes that map into coop_puppet.json ("characters": {"numbers": {file:
// number}}). The pack makes a puppet per file (CoopPuppet_<Age>_C<file>: the
// plain puppet with that model project, as a tunic colour's), which the game
// loads from the mod's own file. The numbers are mapped with the VIEWER's pack,
// so another version of the mod that numbers a character the same shows it,
// and one it does not know shows the colour puppet.
// "CoopPuppet_Adult_C007" for (true, 7); "" past C999.
std::string CharacterActor(bool adult, std::uint16_t file);
// The pack's stored number -> file map ("characters": {"numbers": {...}}): empty for a pack without characters.
std::map<std::uint16_t, std::uint16_t> PackCharacterNumbers(std::string_view manifest);
// `actor` is a character's puppet (its quiet copy too).
bool IsCharacterActor(std::string_view actor);
// The character puppets a pack's coop_puppet.json lists, sorted.
std::vector<std::string> PackCharacterActors(std::string_view manifest);
// The mod's colour routine as a pack's coop_puppet.json names it ("colours": {"offset": n, "head": "32 hex
// digits"}): main's offset and its first 16 bytes; nothing when the manifest has none or it does not read.
std::optional<std::pair<std::uint64_t, std::array<std::uint8_t, 16>>> PackCharacterColours(std::string_view manifest);

// The puppet pack's item copies (coop_puppet.json "look_items",
// tools/puppet_pack/look_items.py): the items and the copy count (two per
// slot). Nothing for a pack without them.
struct LookItems {
    std::vector<std::string> actors;
    std::size_t copies = 0;
    // Row 198: the neutral copy's number ("neutral": the game's colours, marked, never painted), worn by a puppet with
    // no slot of its own; 0 for a pack made before it.
    std::size_t neutral = 0;
};
// `key`: "look_items", or "blade_items" (the same shape, "copies" per player number).
LookItems PackLookItems(std::string_view manifest, std::string_view key = "look_items");

// The pack's "quiet_scenes" (a list of scene names; none for a pack made
// before them, or without perception).
std::vector<std::string> PackQuietScenes(std::string_view manifest);
// The pack was built with perception (coop_puppet.json "perception": true):
// its puppets are seen by enemies and each has a quiet copy.
bool PackPerception(std::string_view manifest);
// Row 177: a puppet actor enemies see (a pack with perception): a CoopPuppet_ actor that is not a quiet copy.
bool SeenPuppetActor(std::string_view actor, bool perceived);
// A puppet's quiet copy: CoopPuppet_Child -> CoopPuppet_ChildQuiet,
// CoopPuppet_Adult_Red -> CoopPuppet_AdultQuiet_Red; any other name as it is.
std::string QuietActor(std::string_view actor);

// A copy of an item: "Weapon_Shield_Hylian", 2 -> "..._Look2".
std::string LookItemName(std::string_view item, std::size_t copy);
// Every piece of `gear` the pack has a copy of becomes that copy.
void DressInLook(Gear& gear, std::size_t copy, const std::vector<std::string>& items);
// A blade copy: "Weapon_Sword_Kokiri", 5 -> "Weapon_Sword_Kokiri_Blade5".
std::string BladeItemName(std::string_view item, std::size_t number);
// Every piece of `gear` that is still one of `items` (a Look copy is left
// as it is) becomes player `number`'s blade copy, if the pack has that many.
void DressInBlades(Gear& gear, std::size_t number, const std::vector<std::string>& items, std::size_t copies);

// A puppet pack's coop_puppet.json lists both green puppets
// (CoopPuppet_Child_Green, CoopPuppet_Adult_Green): only then may
// PlannerSettings::green_puppet ask for them. An older pack has neither, and a
// green player would wait for an actor ActorInfo does not know.
bool PackHasGreenPuppets(std::string_view manifest);

// A puppet pack's coop_puppet.json lists at least one tunic puppet
// (CoopPuppet_Child_<Colour> or CoopPuppet_Adult_<Colour>). A pack made
// before the tunics (or with --tunics none) lists only the plain ones; the
// game refuses an actor its ActorInfo does not have, so with such a pack every
// puppet must be the plain one (Hookshot proof 3: no puppet at all).
bool PackHasTunicPuppets(std::string_view manifest);

class Planner {
public:
    explicit Planner(PlannerSettings settings = {}) : settings_(std::move(settings)) {}
    // `local_event`: our own cutscene runs (it hides every puppet;
    // coop::host::local_event_hides). Each remote's case is
    // coop::host::visibility, with its animation (riding shows it, issue 199).
    Input Plan(std::uint64_t main_base, const std::optional<std::string>& scene,
               const std::optional<Vec3>& position, bool local_event,
               const std::vector<Remote>& remotes, std::uint64_t now_ms,
               const Gear& own_gear = {},
               std::optional<std::uint8_t> own_drawn = {},
               std::optional<std::uint32_t> own_color = {});
    // The actor for a player of that age and colour: CoopPuppet_<Age>, or
    // CoopPuppet_<Age>_<Colour> (CoopColors::TunicOf) while tunics are on.
    // `tunic`: the tunic copy given to the player (Remote::tunic_copy); none: the one nearest `color`.
    std::string ActorFor(bool adult, std::optional<std::uint32_t> color = {}, std::uint16_t character = 0,
                         const std::optional<std::string>& tunic = std::nullopt) const;
    void SetTunics(bool on) { settings_.tunics = on; }
    void SetGreenPuppet(bool on) { settings_.green_puppet = on; }
    // Tunic actors that do not work here (Director::FailedTunics): the plain
    // puppet for them.
    void SetFailedTunics(std::vector<std::string> names) { failed_tunics_ = std::move(names); }
    const PlannerSettings& settings() const { return settings_; }

private:
    PlannerSettings settings_;
    std::vector<std::string> failed_tunics_;
    std::string scene_;
    std::uint64_t scene_since_ms_ = 0;
    std::optional<Vec3> debug_spot_;
};

// The process-wide pair the hook and the SVC handler share.
// Configure once, before the game starts (CoopSvc does it).
void Configure(PlannerSettings settings, bool verbose, Director::Log log);
Director& TheDirector();

// Native puppets phase 3 (coop_lift_player.h): lifting another player. All of it does nothing until
// WatchGamePlaces(true) (the switch LICHII_COOP_LIFT_PLAYER=1).
// - WatchGamePlaces: the director keeps, for each copy it draws, where it drew it last and where the game had it
//   when the director came back to it (read before its write).
// - SetGameHolds: the copy of player `number` is left to the game (our Link lifted it): the director neither
//   places, turns nor moves it with the setter while set, and keeps reading where the game has it. 0: none.
// - GamePlaces: what WatchGamePlaces kept, for the copies drawn in the last 250 ms. Any thread.
struct GamePlace {
    std::uint8_t number = 0;
    Vec3 drawn{};
    std::optional<Vec3> game;
};
void WatchGamePlaces(bool on);
void SetGameHolds(std::uint8_t number);
std::uint8_t GameHolds();
std::vector<GamePlace> GamePlaces();
// Native puppets phase 4 (coop_pvp_native.h, docs/native-puppets.md 5.0): bit n set = player n's copy is the
// `_Pvp` actor (CoopPvpNative::PvpActor). Set each frame by coop_pvp_native_game.cpp; 0 (the default, and with
// LICHII_COOP_PVP_NATIVE off) keeps every copy plain. Any thread.
void SetPvpCopies(std::uint32_t mask);
std::uint32_t PvpCopies();
// Any thread: TheDirector's LiveMask(), 0 while the puppets are off.
std::uint32_t LivePuppets();
// The calc thread: TheDirector's DressTargets() (co-op's own Customize colours), none while the puppets are off.
std::vector<Director::DressView> DressTargets();

// Every remote player presence shows at `server_now_us`, with scene, status,
// age, newest animation and gear: what Publish plans from. CoopFairy plans
// from the same list, so a Navi hides when its puppet does. Null session or
// zero time: none.
std::vector<Remote> Collect(coop::host::Session* session, std::uint64_t server_now_us);

// The present thread: fills `input`'s camera, drawn_since_ms and settle_ms
// from the renderer's camera (coop_camera.h), so a create waits until the
// game draws again after a load. Publish does it for the puppets; CoopFairy
// does it for the fairies, whose creates were refused or never drawn while a
// scene loaded (issue 93).
void StampDrawing(Input& input, std::uint64_t now_ms);

// CoopHook::OnFrame, once per present, with no lock held. `session` may be
// null (not connected): then only the debug puppet is planned.
// `local_event`: coop::host::local_event_hides (our cutscene; not the Ocarina).
void Publish(std::uint64_t main_base, const std::optional<std::string>& scene,
             const std::optional<Vec3>& position, bool local_event,
             coop::host::Session* session, std::uint64_t server_now_us);

// The session's on_event (network thread): keeps each remote player's latest
// animation from PLAYER_SAMPLE field 7 and gear from PLAYER_EQUIP.
void OnEvent(const coop::net::Event& event);

// The colour another player chose (their PLAYER_EQUIP), unset when they chose
// none or are not known; and ours (SetOwnColor, from the settings). For the
// co-op window's names (coop_window.cpp). Any thread.
std::optional<std::uint32_t> PlayerColor(std::uint8_t number);
// 0.11.9: the tunic copies given out (CoopColors::AssignTunicCopies, by the look worker): each player's puppet wears
// its copy, the one the worker repaints in their exact colour. Any thread.
void SetTunicCopies(std::map<std::uint8_t, std::string> copies);
std::optional<std::string> TunicCopyOf(std::uint8_t number);
void SetOwnColor(std::optional<std::uint32_t> rgb);
std::optional<std::uint32_t> OwnColor();
// The colour to show player `number` in (we are `self`): theirs (ours), else
// their slot's (coop::protocol::slot_color).
std::uint32_t NameColor(std::uint8_t number, std::uint8_t self);

} // namespace CoopPuppet
