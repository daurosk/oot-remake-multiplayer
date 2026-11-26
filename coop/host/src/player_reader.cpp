#include "coop/host/player_reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

#include "addresses.hpp"
#include "coop/host/game_flags.hpp"
#include "coop/protocol/codec.hpp"

namespace coop::host {
namespace {

// The longest Banc path read. The map's peek reads 96 bytes.
constexpr std::uint64_t kMaxPathBytes = 256;

bool plausible(const Vec3& v) {
  return std::all_of(v.begin(), v.end(), [](float c) {
    return std::isfinite(c) && std::fabs(c) <= kMaxCoordinate;
  });
}

std::optional<Vec3> read_vec3(GameMemory& memory, std::uint64_t address) {
  Vec3 v{};
  for (std::size_t i = 0; i < 3; ++i) {
    const auto c = read_f32(memory, address + 4 * i);
    if (!c) return std::nullopt;
    v[i] = *c;
  }
  if (!plausible(v)) return std::nullopt;
  return v;
}

}  // namespace

std::optional<std::uint64_t> PlayerReader::game_ui() {
  return read_ptr(memory_, memory_.main_base() + addr::kGameUiModulePtr);
}

std::optional<std::uint64_t> PlayerReader::find_player_actor() {
  if (actor_ != 0) {  // (set_actor: another local player's Link, while it is there)
    const auto state = read_u32(memory_, actor_ + addr::kActorState);
    if (!state || *state != addr::kActorStatePresent) return std::nullopt;
    return actor_;
  }
  const auto scene = read_ptr(memory_, memory_.main_base() + addr::kSceneModulePtr);
  if (!scene) return std::nullopt;
  const auto table = read_ptr(memory_, *scene + addr::kSceneTable);
  if (!table) return std::nullopt;
  const auto count = read_u32(memory_, *table + addr::kTableCount);
  if (!count || *count < addr::kMinTableCount) return std::nullopt;
  const auto array = read_ptr(memory_, *table + addr::kTableArray);
  if (!array) return std::nullopt;
  const auto manager = read_ptr(memory_, *array + addr::kPlayerManagerSlot);
  if (!manager) return std::nullopt;
  if (!read_ptr(memory_, *manager + addr::kManagerMustBeSet)) return std::nullopt;

  // As 0x37cca0: the first link when its actor is there, else the second.
  for (const std::uint64_t link_at : {addr::kManagerLinkFirst, addr::kManagerLinkSecond}) {
    const auto link = read_ptr(memory_, *manager + link_at);
    if (!link) continue;
    const auto id = read_u32(memory_, *link + addr::kLinkId);
    if (!id || *id == addr::kEmptyId) continue;
    const auto holder = read_ptr(memory_, *link + addr::kLinkHolder);
    if (!holder) continue;
    const auto holder_id = read_u32(memory_, *holder + addr::kHolderId);
    if (!holder_id || *holder_id != *id) continue;  // a stale holder
    const auto actor = read_ptr(memory_, *holder + addr::kHolderActor);
    if (!actor) continue;
    const auto state = read_u32(memory_, *actor + addr::kActorState);
    if (!state || *state != addr::kActorStatePresent) continue;
    return actor;
  }
  return std::nullopt;
}

std::optional<Vec3> PlayerReader::read_position() {
  // The GameUI copy, while its valid bit is set (the first Link's: not for set_actor's).
  if (const auto ui = actor_ == 0 ? game_ui() : std::nullopt) {
    const auto flags = read_u16(memory_, *ui + addr::kGameUiPoseFlags);
    if (flags && (*flags & addr::kGameUiPoseValidBit) != 0) {
      if (auto p = read_vec3(memory_, *ui + addr::kGameUiPosition)) return p;
    }
  }
  // Otherwise the actor itself.
  if (const auto actor = find_player_actor()) {
    return read_vec3(memory_, *actor + addr::kActorPosition);
  }
  return std::nullopt;
}

std::optional<Quat> PlayerReader::read_rotation() {
  const auto actor = find_player_actor();
  if (!actor) return std::nullopt;
  Mat3 m{};
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      const auto v = read_f32(memory_, *actor + addr::kActorRotation + 4 * (3 * r + c));
      if (!v) return std::nullopt;
      if constexpr (addr::kActorRotationRowMajor) {
        m[r][c] = *v;
      } else {
        m[c][r] = *v;
      }
    }
  }
  return matrix_to_quaternion(m);
}

std::optional<protocol::Age> PlayerReader::read_age() {
  const auto ui = game_ui();
  if (!ui) return std::nullopt;
  const auto is_child = read_u8(memory_, *ui + addr::kGameUiIsChild);
  if (!is_child) return std::nullopt;
  if (*is_child == 1) return protocol::Age::child;
  if (*is_child == 0) return protocol::Age::adult;
  return std::nullopt;  // not a bool: something else is there
}

std::optional<std::string> PlayerReader::read_scene() {
  const auto banc = read_ptr(memory_, memory_.main_base() + addr::kBancModulePtr);
  if (!banc) return std::nullopt;
  if (!read_ptr(memory_, *banc + addr::kBancMustBeSet)) return std::nullopt;
  const auto path = read_ptr(memory_, *banc + addr::kBancPathPtr);
  const auto capacity = read_u64(memory_, *banc + addr::kBancPathCapacity);
  if (!path || !capacity || *capacity == 0) return std::nullopt;
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min(*capacity, kMaxPathBytes)));
  if (!memory_.read(*path, bytes)) return std::nullopt;
  const auto end = std::find(bytes.begin(), bytes.end(), std::uint8_t{0});
  if (end == bytes.end()) return std::nullopt;  // unterminated
  const std::string text(bytes.begin(), end);
  return scene_from_path(text);
}

std::optional<bool> PlayerReader::read_event_word() {
  const auto events = read_ptr(memory_, memory_.main_base() + addr::kEventManagerPtr);
  if (!events) return std::nullopt;
  const auto running = read_u64(memory_, *events + addr::kEventRunning);
  if (!running) return std::nullopt;
  return *running != 0;
}

// The game's own QueryEventMemberIsEventMemver (0x29f3770) for our Link: its EventMember M, M's
// record L (0: in no event), and a member when one of L's slots holds an event pointer with flag
// bit 0. nullopt when M or L cannot be read; a slot that cannot be read counts as free.
std::optional<bool> PlayerReader::read_link_bound() {
  const auto actor = find_player_actor();
  if (!actor) return std::nullopt;
  const auto parts = read_ptr(memory_, *actor + addr::kActorComponents);
  if (!parts) return std::nullopt;
  const auto member = read_ptr(memory_, *parts + addr::kEventMemberSlot);
  if (!member) return std::nullopt;
  const auto record = read_u64(memory_, *member + addr::kMemberRecord);
  if (!record) return std::nullopt;
  if (*record == 0) return false;
  for (std::uint64_t i = 0; i < addr::kMemberSlots; ++i) {
    const std::uint64_t slot = *record + addr::kRecordSlots + addr::kMemberSlotSize * i;
    const auto event = read_u64(memory_, slot);
    const auto flags = read_u8(memory_, slot + addr::kSlotFlags);
    if (event && *event != 0 && flags && (*flags & 1) != 0) return true;
  }
  return false;
}

std::optional<std::int32_t> PlayerReader::read_life() {
  const auto actor = find_player_actor();
  if (!actor) return std::nullopt;
  const auto parts = read_ptr(memory_, *actor + addr::kActorComponents);
  const auto life = parts ? read_ptr(memory_, *parts + addr::kComponentLife) : std::nullopt;
  const auto value = life ? read_ptr(memory_, *life + addr::kLifeValue) : std::nullopt;
  const auto word = value ? read_u32(memory_, *value + addr::kLifeValueWord) : std::nullopt;
  if (!word) return std::nullopt;
  const auto quarters = static_cast<std::int32_t>(*word);
  if (quarters < 0 || quarters > protocol::kMaxLifeQuarters) return std::nullopt;
  return quarters;
}

std::optional<bool> PlayerReader::read_event_running() {
  const auto word = read_event_word();
  const auto bound = read_link_bound();
  if ((word && *word) || (bound && *bound)) return true;
  if (word || bound) return false;
  return std::nullopt;
}

std::optional<protocol::Animation> PlayerReader::read_animation() {
  const auto actor = find_player_actor();
  if (!actor) return std::nullopt;
  const auto components = read_ptr(memory_, *actor + addr::kActorComponents);
  if (!components) return std::nullopt;
  const auto as = read_ptr(memory_, *components + addr::kComponentsAs);
  if (!as) return std::nullopt;
  const auto inner = read_ptr(memory_, *as + addr::kAsComponentInner);
  if (!inner) return std::nullopt;
  const auto controller = read_ptr(memory_, *inner + addr::kAsInnerController);
  if (!controller) return std::nullopt;
  const auto count = read_u32(memory_, *controller + addr::kAsSlotCount);
  // A signed count, as the game compares it.
  if (!count || static_cast<std::int32_t>(*count) <= static_cast<std::int32_t>(addr::kAsMainSlot)) {
    return std::nullopt;
  }
  const auto array = read_ptr(memory_, *controller + addr::kAsSlotArray);
  if (!array) return std::nullopt;
  const auto slot = read_ptr(memory_, *array + addr::kAsMainSlot * addr::kAsSlotStride);
  if (!slot) return std::nullopt;
  const auto name_at = read_ptr(memory_, *slot + addr::kAsSlotCommandName);
  if (!name_at) return std::nullopt;

  // The name, in 16-byte reads that never cross a 16-byte boundary, so a
  // name near the end of a mapped page still reads.
  std::string name;
  std::uint64_t at = *name_at;
  bool terminated = false;
  while (!terminated && name.size() <= protocol::kMaxAnimationChars) {
    std::array<std::uint8_t, 16> chunk{};
    const std::size_t size = 16 - static_cast<std::size_t>(at & 15);
    if (!memory_.read(at, std::span(chunk.data(), size))) return std::nullopt;
    for (std::size_t i = 0; i < size; ++i) {
      if (chunk[i] == 0) {
        terminated = true;
        break;
      }
      name.push_back(static_cast<char>(chunk[i]));
    }
    at += size;
  }
  if (!terminated || name.empty() || name.size() > protocol::kMaxAnimationChars) {
    return std::nullopt;
  }
  for (const char c : name) {
    if (c < 0x20 || c > 0x7e) return std::nullopt;
  }

  const auto frame = read_f32(memory_, *slot + addr::kAsSlotFrame);
  const auto rate = read_f32(memory_, *slot + addr::kAsSlotRate);
  if (!frame || !rate) return std::nullopt;
  if (!std::isfinite(*frame) || std::fabs(*frame) > kMaxAnimationFrame) return std::nullopt;
  if (!std::isfinite(*rate) || std::fabs(*rate) > kMaxAnimationRate) return std::nullopt;
  return protocol::Animation{std::move(name), *frame, *rate};
}

std::optional<protocol::Equipment> PlayerReader::read_equipment() {
  protocol::Equipment e;
  GameFlags flags(memory_);
  e.weapon = flags.get_string64(addr::kEquipWeapon);
  e.shield = flags.get_string64(addr::kEquipShield);
  e.armor = flags.get_string64(addr::kEquipArmor);
  e.boots = flags.get_string64(addr::kEquipBoot);
  e.head = flags.get_string64(addr::kEquipHeadArmor);
  e.hand = flags.get_string64(addr::kEquipHandArmor);
  wear_from_links(e, read_age() == protocol::Age::child);
  // The shield the game holds (hand slot 4) over the EquipShield flag, as the
  // director takes slot 0's sword over EquipWeapon: at an age change the
  // flag can still name the other age's shield for a sample (Mirror on a
  // child, Deku on an adult) while the slot holds the one the game draws.
  // An empty or unreadable slot (no shield yet, a load) leaves the flag.
  if (const auto held_shield = read_slot_name(addr::kEquipSlotShield);
      held_shield && held_shield->starts_with("Weapon_Shield_")) {
    e.shield = held_shield;
  }
  drop_lost(e);
  e.ocarina = flags.get_string64(addr::kEquipOcarina);
  e.arrow = flags.get_string64(addr::kEquipArrow);
  e.drawn = read_drawn();
  e.held = read_slot_name(addr::kEquipSlotHeldItem);
  e.weapon_slot = read_slot_name(addr::kEquipSlotSword);
  e.sub_weapon = read_slot_name(addr::kEquipSlotSubWeapon);
  e.third_weapon = read_slot_name(addr::kEquipSlotThirdWeapon);
  e.tool = read_slot_name(addr::kEquipSlotTool);
  e.sub_tool = read_slot_name(addr::kEquipSlotSubTool);
  if (e.drawn) e.lit = read_lit(*e.drawn);
  if (e == protocol::Equipment{}) return std::nullopt;
  return e;
}

std::optional<std::uint8_t> PlayerReader::read_drawn() {
  const auto actor = find_player_actor();
  if (!actor) return std::nullopt;
  const auto components = read_ptr(memory_, *actor + addr::kActorComponents);
  if (!components) return std::nullopt;
  const auto user = read_ptr(memory_, *components + addr::kComponentsEquipmentUser);
  if (!user) return std::nullopt;
  std::uint8_t drawn = 0;
  for (std::uint32_t i = 0; i < addr::kEquipSlotCount; ++i) {
    const std::uint64_t slot = *user + addr::kEquipSlots + addr::kEquipSlotStride * i;
    const auto id = read_u32(memory_, slot + addr::kEquipSlotId);
    if (!id) return std::nullopt;
    DrawnMemo& memo = drawn_memo_[i];
    // As 0x4a33c0: the holder must still hold this id and its actor must
    // not be gone.
    bool live = false;
    bool kept_away = false;
    bool carried_out = false;
    if (*id != addr::kEmptyId) {
      if (const auto holder = read_ptr(memory_, slot + addr::kEquipSlotHolder)) {
        const auto holder_id = read_u32(memory_, *holder + addr::kHolderId);
        if (holder_id && *holder_id == *id) {
          if (const auto equipment = read_ptr(memory_, *holder + addr::kHolderActor)) {
            const auto state = read_u32(memory_, *equipment + addr::kActorState);
            live = state && !(*state >= addr::kActorStateGoneFirst &&
                              *state <= addr::kActorStateGoneLast);
            kept_away = state && *state == addr::kActorStateKeptAway &&
                        addr::equip_slot_carried(i);
            carried_out = state && *state == addr::kActorStatePresent &&
                          addr::equip_slot_carried(i);
          }
        }
      }
    }
    if (!live) {
      memo = DrawnMemo{};
      continue;
    }
    const auto bits = read_u8(memory_, *user + addr::kEquipSlotState + i);
    if (!bits) return std::nullopt;
    if (memo.id != *id) {
      memo = DrawnMemo{*id, false};  // a new piece: the game makes it put away
    }
    // The request byte lasts a moment; the last request seen stands until
    // the next one.
    if (*bits & addr::kEquipSlotDrawnBit) {
      memo.drawn = true;
    } else if (*bits & (addr::kEquipRequestAway | addr::kEquipRequestNone)) {
      memo.drawn = false;
    } else if (*bits == 0 && kept_away) {
      // No request in flight and the game keeps the piece elsewhere: put
      // away, also when its put-away request passed unseen.
      memo.drawn = false;
    } else if (*bits == 0 && carried_out) {
      // And the other way: a carried piece the game shows (state 4) is out,
      // also when its draw request was cleared before the state changed.
      memo.drawn = true;
    }
    if (memo.drawn) drawn = static_cast<std::uint8_t>(drawn | (1u << i));
  }
  return drawn;
}

// Issue 248: the owner's game burns the stick (its chemistry); others are sent
// only that it burns, and show the stick's own flame (docs/puppet.md).
std::uint8_t PlayerReader::read_lit(std::uint8_t drawn) {
  if (drawn == 0) return 0;
  const auto actor = find_player_actor();
  if (!actor) return 0;
  const auto components = read_ptr(memory_, *actor + addr::kActorComponents);
  if (!components) return 0;
  const auto user = read_ptr(memory_, *components + addr::kComponentsEquipmentUser);
  if (!user) return 0;
  std::uint8_t lit = 0;
  for (std::uint32_t i = 0; i < addr::kEquipSlotCount; ++i) {
    if ((drawn & (1u << i)) == 0) continue;
    const std::uint64_t slot = *user + addr::kEquipSlots + addr::kEquipSlotStride * i;
    const auto id = read_u32(memory_, slot + addr::kEquipSlotId);
    if (!id || *id == addr::kEmptyId) continue;
    const auto holder = read_ptr(memory_, slot + addr::kEquipSlotHolder);
    if (!holder) continue;
    const auto holder_id = read_u32(memory_, *holder + addr::kHolderId);
    if (!holder_id || *holder_id != *id) continue;
    const auto piece = read_ptr(memory_, *holder + addr::kHolderActor);
    if (piece && plays_effect(*piece, addr::kBurnKey)) lit = static_cast<std::uint8_t>(lit | (1u << i));
  }
  return lit;
}

bool PlayerReader::plays_effect(std::uint64_t actor, std::string_view key) {
  const auto components = read_ptr(memory_, actor + addr::kActorComponents);
  if (!components) return false;
  const auto xlink = read_ptr(memory_, *components + addr::kComponentsXLink);
  if (!xlink) return false;
  const auto user = read_ptr(memory_, *xlink + addr::kXLinkEffectUser);
  if (!user) return false;
  const auto offset = read_u32(memory_, *user + addr::kUserLinkOffset);
  auto link = read_ptr(memory_, *user + addr::kUserFirstLink);
  // (as CoopPuppetSound's walk: a link offset past 0x400 is not a user's)
  if (!offset || *offset > 0x400 || !link) return false;
  const std::uint64_t head = *user + addr::kUserEvents;
  const auto back = static_cast<std::int64_t>(static_cast<std::int32_t>(*offset));
  for (std::uint32_t n = 0; link && *link != head && n < addr::kMaxUserEvents; ++n) {
    const std::uint64_t event = *link - static_cast<std::uint64_t>(back);
    if (const auto call = read_ptr(memory_, event + addr::kEventCall)) {
      if (const auto text = read_ptr(memory_, *call)) {
        std::array<std::uint8_t, 32> bytes{};
        const auto want = std::span(bytes).first(std::min(key.size() + 1, bytes.size()));
        if (key.size() < bytes.size() && memory_.read(*text, want) &&
            std::equal(key.begin(), key.end(), bytes.begin()) && bytes[key.size()] == 0) {
          return true;
        }
      }
    }
    link = read_ptr(memory_, *link + addr::kLinkNext);
  }
  return false;
}

std::optional<std::string> PlayerReader::read_slot_name(std::uint32_t slot_index) {
  if (slot_index >= addr::kEquipSlotCount) return std::nullopt;
  return read_link_name(addr::kEquipSlots + addr::kEquipSlotStride * slot_index);
}

std::optional<std::string> PlayerReader::read_armor_link_name(std::uint32_t link_index) {
  if (link_index >= addr::kArmorLinkCount) return std::nullopt;
  return read_link_name(addr::kArmorLinks + addr::kEquipSlotStride * link_index);
}

// Issue 91 and row 148, one cause: the PlayerStatus.Equip* flags are what the
// pause menu chose, and they outlive an age change; the EquipmentUser's armor
// links hold what the game really draws on Link. Seen: the child's Goron's
// Bracelet (kind 14) is in a link with EquipHandArmor "" (held-ocarina-b,
// 2026-11-14); masks and the Hover/Iron Boots are ZR items
// (PlayerStatus.AssignableItem), put on by the game's own use call, and
// through the menu's GameData path (state 2) the game drew no mask even on its
// own Link (wear-masks 20261110-155536); a young Link whose EquipHandArmor
// still named the adult's Armor_Adult_Gauntlet_Gold was sent with it, and the
// other games' child puppet wore Golden Gauntlets (row 148, bladeedge02's
// 0.11.4 log). The adult's gauntlets are a piece in a link too: the game's
// take-off for an empty hand flag empties armor link 3 (0x10aceb0, docs/
// puppet.md).
// Seen in the game (hs-gear-adult run 20261118-161054, a child with the Golden
// Gauntlets, Goron Tunic and Hover Boots flags made again by a warp): the
// game drew none of them, only the bracelet, and its links held only that.
// So for a CHILD each kind the game draws as a piece (below) is decided by
// the links, by name (the links' index per kind is not proven): a piece of
// that kind in a link is sent; a flag naming a piece of that kind that no
// link holds is not worn ("") with every link read, and not known (the
// others keep what they had) while a link is unreadable, as during a load,
// so the flag is never sent for a moment. A flag naming anything else stands
// (Armor_Adult_Normal is no piece in a child's links).
// An ADULT keeps the rule before row 148 (its gear on a puppet is not yet
// checked against the links in a game): a flag with a name stands, an empty
// or unknown flag is filled from a link piece of its kind. The one exception
// (puppet-gear-148, the other direction): a flag naming a CHILD's piece
// (Armor_Child_*: the bracelet, a mask) is decided by the links at any age,
// so an adult whose hand or head flag outlived the age change the other way
// is not sent the child's piece; those pieces are proven link pieces (the
// bracelet in link 9, a ZR mask in its link), so a worn one is never lost.
// An empty hand flag with a link unreadable is not known (not a bracelet
// taken off), as before.
namespace {
constexpr std::string_view kChildPiecePrefix = "Armor_Child_";
bool drawn_head(std::string_view n) { return protocol::is_drawn_head(n); }
bool drawn_armor(std::string_view n) {
  return n == "Armor_Adult_Goron" || n == "Armor_Adult_Zora";
}
bool drawn_boots(std::string_view n) {
  return n == "Armor_Adult_HoverBoot" || n == "Armor_Adult_HeavyBoot";
}
bool drawn_hand(std::string_view n) {
  return n.starts_with("Armor_Adult_Gauntlet_") || n.starts_with(protocol::kChildBraceletPrefix);
}
}  // namespace

// A piece the player no longer has (puppet-gear-148, review-0116-j): a shield
// burnt or eaten by a Like Like, a tunic a Like Like took. Its hand slot or
// armor link empties, but the Equip* flag can still name it, and an adult's
// named flag (or an empty shield slot) leaves the flag. The game's own count,
// PouchData.<piece>.StockNum (the max-drop the flag watcher already keeps for
// a lost shield, docs/items.md issue 130), is 0 then: such a piece is not
// worn. A count not read (no such flag, the manager not up) leaves the name.
void PlayerReader::drop_lost(protocol::Equipment& e) {
  GameFlags flags(memory_);
  const auto check = [&](std::optional<std::string>& field, bool (*of_kind)(std::string_view)) {
    if (!field || field->empty() || !of_kind(*field)) return;
    const std::string stock = "PouchData." + *field + ".StockNum";
    if (const auto n = flags.get_int(protocol::flag_hash(stock)); n && *n <= 0) {
      field = std::string{};
    }
  };
  check(e.shield, [](std::string_view n) { return n.starts_with("Weapon_Shield_"); });
  check(e.armor, drawn_armor);
  check(e.boots, drawn_boots);
  check(e.hand, drawn_hand);
}

void PlayerReader::wear_from_links(protocol::Equipment& e, bool child) {
  std::array<std::optional<std::string>, addr::kArmorLinkCount> links;
  bool every_link_read = true;
  for (std::uint32_t i = 0; i < addr::kArmorLinkCount; ++i) {
    links[i] = read_armor_link_name(i);
    if (!links[i]) every_link_read = false;
  }
  const auto decide = [&](std::optional<std::string>& field, bool (*of_kind)(std::string_view)) {
    // A child's piece (Armor_Child_*: the bracelet, the masks) is decided by
    // the links at any age: an adult (or an age not read) whose flag still
    // names one wears it only if a link holds it, as a child's adult flags.
    const bool links_decide =
        child || (field && field->starts_with(kChildPiecePrefix) && of_kind(*field));
    if (!links_decide && field && !field->empty()) return;
    for (const auto& name : links) {
      if (name && !name->empty() && of_kind(*name)) {
        field = name;
        return;
      }
    }
    if (links_decide && field && of_kind(*field)) {
      if (every_link_read) {
        field = std::string{};
      } else {
        field.reset();
      }
    }
  };
  // A head flag that is no mask (the Lens of Truth's `Armor_GlassTruth`, row
  // 197) is nothing the game draws on Link, at any age: taken as an empty
  // flag, so a mask in a link still wins and none sends nothing worn.
  if (e.head && !e.head->empty() && !drawn_head(*e.head)) e.head = std::string{};
  decide(e.head, drawn_head);
  decide(e.armor, drawn_armor);
  decide(e.boots, drawn_boots);
  decide(e.hand, drawn_hand);
  if (e.hand && e.hand->empty() && !every_link_read) e.hand.reset();
}

std::optional<std::string> PlayerReader::read_link_name(std::uint64_t link_offset) {
  const auto actor = find_player_actor();
  if (!actor) return std::nullopt;
  const auto components = read_ptr(memory_, *actor + addr::kActorComponents);
  if (!components) return std::nullopt;
  const auto user = read_ptr(memory_, *components + addr::kComponentsEquipmentUser);
  if (!user) return std::nullopt;
  const std::uint64_t slot = *user + link_offset;
  const auto id = read_u32(memory_, slot + addr::kEquipSlotId);
  if (!id) return std::nullopt;
  if (*id == addr::kEmptyId) return std::string{};
  // The same test of the link as read_drawn.
  const auto holder = read_ptr(memory_, slot + addr::kEquipSlotHolder);
  if (!holder) return std::string{};
  const auto holder_id = read_u32(memory_, *holder + addr::kHolderId);
  if (!holder_id || *holder_id != *id) return std::string{};
  const auto piece = read_ptr(memory_, *holder + addr::kHolderActor);
  if (!piece) return std::string{};
  const auto state = read_u32(memory_, *piece + addr::kActorState);
  if (!state) return std::nullopt;
  if (*state >= addr::kActorStateGoneFirst && *state <= addr::kActorStateGoneLast) {
    return std::string{};
  }
  const auto text = read_ptr(memory_, *piece + addr::kActorName);
  if (!text) return std::nullopt;
  std::array<std::uint8_t, protocol::kMaxEquipNameChars + 1> bytes{};
  if (!memory_.read(*text, bytes)) return std::nullopt;
  std::string name;
  for (const std::uint8_t b : bytes) {
    if (b == 0) return name.empty() ? std::nullopt : std::optional<std::string>(name);
    if (b < 0x20 || b > 0x7e) return std::nullopt;
    name.push_back(static_cast<char>(b));
  }
  return std::nullopt;  // longer than a name can be
}

std::optional<PlayerReading> PlayerReader::read() {
  const auto position = read_position();
  if (!position) return std::nullopt;
  PlayerReading reading;
  reading.position = *position;
  reading.rotation = read_rotation();
  reading.age = read_age();
  reading.scene = read_scene();
  {
    const auto word = read_event_word();
    const auto bound = read_link_bound();
    if ((word && *word) || (bound && *bound)) {
      reading.event_running = true;
      reading.event_by_link = !(word && *word);
      reading.link_bound = bound && *bound;
    } else if (word || bound) {
      reading.event_running = false;
    }
  }
  reading.animation = read_animation();
  reading.equipment = read_equipment();
  if (const auto life = read_life()) reading.dead = *life <= 0;
  return reading;
}

std::optional<Quat> matrix_to_quaternion(const Mat3& m, float tolerance) {
  double a[3][3];
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      if (!std::isfinite(m[r][c])) return std::nullopt;
      a[r][c] = m[r][c];
    }
  }
  const double tol = tolerance;
  for (std::size_t i = 0; i < 3; ++i) {
    for (std::size_t j = i; j < 3; ++j) {
      const double dot = a[i][0] * a[j][0] + a[i][1] * a[j][1] + a[i][2] * a[j][2];
      if (std::fabs(dot - (i == j ? 1.0 : 0.0)) > tol) return std::nullopt;
    }
  }
  const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                     a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                     a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (std::fabs(det - 1.0) > tol) return std::nullopt;  // also refuses mirrors

  // Shepperd's method: divide by the largest of 4w^2, 4x^2, 4y^2, 4z^2, so it
  // stays accurate near 180 degrees, where w is close to 0.
  double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
  const double trace = a[0][0] + a[1][1] + a[2][2];
  if (trace > a[0][0] && trace > a[1][1] && trace > a[2][2]) {
    const double s = 2.0 * std::sqrt(std::max(0.0, 1.0 + trace));
    w = 0.25 * s;
    x = (a[2][1] - a[1][2]) / s;
    y = (a[0][2] - a[2][0]) / s;
    z = (a[1][0] - a[0][1]) / s;
  } else if (a[0][0] >= a[1][1] && a[0][0] >= a[2][2]) {
    const double s = 2.0 * std::sqrt(std::max(0.0, 1.0 + a[0][0] - a[1][1] - a[2][2]));
    w = (a[2][1] - a[1][2]) / s;
    x = 0.25 * s;
    y = (a[0][1] + a[1][0]) / s;
    z = (a[0][2] + a[2][0]) / s;
  } else if (a[1][1] >= a[2][2]) {
    const double s = 2.0 * std::sqrt(std::max(0.0, 1.0 + a[1][1] - a[0][0] - a[2][2]));
    w = (a[0][2] - a[2][0]) / s;
    x = (a[0][1] + a[1][0]) / s;
    y = 0.25 * s;
    z = (a[1][2] + a[2][1]) / s;
  } else {
    const double s = 2.0 * std::sqrt(std::max(0.0, 1.0 + a[2][2] - a[0][0] - a[1][1]));
    w = (a[1][0] - a[0][1]) / s;
    x = (a[0][2] + a[2][0]) / s;
    y = (a[1][2] + a[2][1]) / s;
    z = 0.25 * s;
  }
  const double len = std::sqrt(x * x + y * y + z * z + w * w);
  if (!(len > 0.0)) return std::nullopt;
  const double k = (w < 0.0 ? -1.0 : 1.0) / len;  // one sign: w >= 0
  return Quat{static_cast<float>(k * x), static_cast<float>(k * y), static_cast<float>(k * z),
              static_cast<float>(k * w)};
}

std::optional<std::string> scene_from_path(std::string_view path) {
  const auto slash = path.find_last_of("/\\");
  if (slash != std::string_view::npos) path.remove_prefix(slash + 1);
  const auto dot = path.find('.');
  if (dot != std::string_view::npos) path = path.substr(0, dot);
  if (path.empty() || path.size() > protocol::kMaxSceneChars) return std::nullopt;
  for (const char c : path) {
    if (c < 0x20 || c > 0x7e) return std::nullopt;
  }
  return std::string(path);
}

}  // namespace coop::host
