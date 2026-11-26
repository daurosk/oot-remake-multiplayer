#pragma once

// Reads the local player's state from guest memory. Every value is checked:
// a failed or implausible read gives nothing, never garbage.

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "coop/host/event_owner.hpp"
#include "coop/host/game_memory.hpp"
#include "coop/protocol/messages.hpp"

namespace coop::host {

using Vec3 = protocol::Vec3;
using Quat = protocol::Quat;  // x, y, z, w
using Mat3 = std::array<std::array<float, 3>, 3>;  // [row][column]

// Positions further than this from the origin, in game units, are refused.
inline constexpr float kMaxCoordinate = 1.0e6f;
// Animation frames and rates beyond these are refused as garbage.
inline constexpr float kMaxAnimationFrame = 1.0e6f;
inline constexpr float kMaxAnimationRate = 100.0f;

// One frame's reading. The position is required; without it there is no
// reading. Each other field is nothing when its own read failed or was
// implausible.
struct PlayerReading {
  Vec3 position{};
  std::optional<Quat> rotation;
  std::optional<protocol::Age> age;
  std::optional<std::string> scene;     // for example "spot00"
  // An event has control of our Link (a cutscene, a talk): the event manager's running word,
  // or our Link an event member by the game's own test (row 97: a Deku Scrub's talk and
  // AttackedEvent leave the word 0). Every "our Link is in an event" rule reads this.
  std::optional<bool> event_running;
  // event_running is true only because our Link is an event member (the word is 0): for the log.
  bool event_by_link = false;
  // Our Link is an event member, the game's own QueryEventMemberIsEventMemver (with the word or
  // without): G1's "ours" (coop/host/event_owner.hpp).
  bool link_bound = false;
  // G1 (event_owner.hpp): whose the running event is. The reader cannot know (it reads one game
  // frame); the hook's EventOwnerTracker sets it. `ours` by default: what event_running meant before.
  EventWhose event_whose = EventWhose::ours;
  std::optional<protocol::Animation> animation;  // AS command in the main slot
  std::optional<protocol::Equipment> equipment;  // what the player wears
  // PLAYER_STATUS's away bits (docs/protocol.md), which the reader cannot see in one frame of memory: the hook sets
  // them from the game's own state (docs/puppet.md, "Away"). Nothing: not known in this build (kept as 0, never sent).
  // A game menu is in front: one of the game's menu screens (its update f_802f1380) has a focused button: the pause
  // menu, the Key Items / Threads of Time / Map panel, a dialog (Longshot's patch 0018, hos_coop_menu_state).
  std::optional<bool> in_menu;
  // Our world stands still while no menu says so (WorldStill, sampler.hpp): the quick-switch item menu, a menu
  // patch 0018 does not track, a stall. The hook sets it; false: running, or not judged.
  bool world_still = false;
  // The player actor's own Life component at 0 (read_life): Game Over. Down but not out keeps a downed Link at a
  // quarter heart, so not then. Nothing: not read (the last value sent is kept).
  std::optional<bool> dead;
  bool operator==(const PlayerReading&) const = default;
};

class PlayerReader {
 public:
  // What read_drawn last saw of each equipment slot. The game shows a change
  // of state only for a moment (addresses.hpp), so the last one is kept: give
  // every frame's reader the same DrawnState, or drawn pieces are lost.
  struct DrawnMemo {
    std::uint32_t id = 0xFFFFFFFFu;  // the piece in the slot; ~0 none
    bool drawn = false;
  };
  using DrawnState = std::array<DrawnMemo, 8>;

  explicit PlayerReader(GameMemory& memory, DrawnState* drawn_state = nullptr)
      : memory_(memory), drawn_memo_(drawn_state != nullptr ? *drawn_state : own_drawn_) {}

  // This frame's reading, or nothing when the player cannot be read.
  std::optional<PlayerReading> read();
  // Split screen: read this Player actor (another local player's Link)
  // instead of the one the game's lookup finds; its place is the actor's own,
  // never the GameUI copy (that one is the first Link's). 0: the lookup again.
  void set_actor(std::uint64_t actor) { actor_ = actor; }

  // The parts, for tests and for the emulator's debug overlay.
  std::optional<Vec3> read_position();      // GameUI copy, else the actor's
  std::optional<Quat> read_rotation();      // from the actor's matrix
  std::optional<protocol::Age> read_age();
  std::optional<std::string> read_scene();
  std::optional<bool> read_event_running();  // the word or our Link bound, as PlayerReading
  std::optional<bool> read_event_word();     // the event manager's running word alone
  std::optional<bool> read_link_bound();     // our Link is an event member (the game's own test)
  // The player actor's life, in quarter hearts, from its own Life component (addresses.hpp kComponentLife). Nothing
  // when a step of the walk fails, or the value is below 0 or above protocol::kMaxLifeQuarters.
  std::optional<std::int32_t> read_life();
  // The AS command playing in the player's main slot, its frame and rate.
  // Nothing when no command plays, or the name is not 1 to
  // protocol::kMaxAnimationChars printable ASCII characters, or the frame or
  // rate is not finite or out of range.
  std::optional<protocol::Animation> read_animation();
  // What the player has equipped (PlayerStatus.Equip* flags) and which
  // equipment slots are drawn, and the item in the hand slot. Nothing when neither could be read; a field
  // is nothing when its own read failed.
  std::optional<protocol::Equipment> read_equipment();
  // The player actor's guest address, after the link choice and id test.
  std::optional<std::uint64_t> find_player_actor();

 private:
  std::optional<std::uint64_t> game_ui();
  std::optional<std::uint8_t> read_drawn();
  // Bit i set: the piece in drawn slot i burns (its ELink user plays
  // Chemical_Fire; issue 248). Only the bits of `drawn` are looked at.
  std::uint8_t read_lit(std::uint8_t drawn);
  // Whether the actor's ELink user plays a call with this key now.
  bool plays_effect(std::uint64_t actor, std::string_view key);
  // The actor name of the piece in an equipment slot (3: slingshot, bow);
  // "" when the slot is empty or its piece is gone. Nothing when the slot
  // or the name could not be read.
  std::optional<std::string> read_slot_name(std::uint32_t slot_index);
  // The same for one of the EquipmentUser's armor links (after the slots).
  std::optional<std::string> read_armor_link_name(std::uint32_t link_index);
  // The pieces the game draws as an actor in its armor links (masks, the
  // Goron/Zora Tunics, the Hover/Iron Boots, the gauntlets and the child's
  // bracelet) are sent as the links hold them, not as the PlayerStatus.Equip*
  // flags name them (issue 91, row 148): a flag outlives an age change, so a
  // child whose flag still names the adult's Golden Gauntlets wears none.
  // For an adult (`child` false) a flag with a name stands, as before.
  void wear_from_links(protocol::Equipment& e, bool child);
  // A shield, tunic, boots or gauntlets the player no longer has (the game's
  // PouchData.<piece>.StockNum is 0: burnt, eaten by a Like Like) is not worn.
  void drop_lost(protocol::Equipment& e);
  // The piece behind the link at EquipmentUser + `link_offset`.
  std::optional<std::string> read_link_name(std::uint64_t link_offset);
  GameMemory& memory_;
  DrawnState own_drawn_{};
  DrawnState& drawn_memo_;
  std::uint64_t actor_ = 0;  // set_actor's
};

// A rotation matrix (row-major, v' = M v) to a unit quaternion with w >= 0.
// Nothing when the matrix is not finite or not close to a rotation (rows of
// length 1, at right angles, determinant +1, within `tolerance`).
std::optional<Quat> matrix_to_quaternion(const Mat3& m, float tolerance = 0.05f);

// The scene name from the Banc path: the part after the last '/' or '\', cut
// at the first '.' after that. Nothing when the result is empty, longer than
// protocol::kMaxSceneChars, or holds anything but printable ASCII.
std::optional<std::string> scene_from_path(std::string_view path);

}  // namespace coop::host
