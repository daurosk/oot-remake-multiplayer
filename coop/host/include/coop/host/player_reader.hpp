#pragma once

// Reads the local player's state from guest memory. Every value is checked:
// a failed or implausible read gives nothing, never garbage.

#include <array>
#include <optional>
#include <string>
#include <string_view>

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
  std::optional<bool> event_running;    // an event (cutscene) has control
  // The running event's flow name while its current part is a cinematic
  // (Dm_...); nothing otherwise (talks, signs, item gets).
  std::optional<std::string> cutscene;
  // The ocarina note that sounds now (0 to 18: Ocarina_LowB ... HighF),
  // nothing between notes; read only while our Link plays
  // the ocarina (an AS command with "Ocarina" in it) and no cutscene runs.
  std::optional<std::uint8_t> ocarina_note;
  std::optional<protocol::Animation> animation;  // AS command in the main slot
  std::optional<protocol::Equipment> equipment;  // what the player wears
  bool operator==(const PlayerReading&) const = default;
};

// The tunic a player chose to be seen in (LICHII_COOP_TUNIC): the adult
// tunic actor for "green"/"verde", "red"/"roja"/"goron", "blue"/"azul"/"zora"
// (any case); nothing for anything else. Child Link has no other tunic model.
std::optional<std::string> tunic_actor(std::string_view choice);

// What the others are sent: the chosen tunic replaces the worn one while the
// player is adult and the game says what it wears. Only the others' view of
// this player changes; the game's own Link and its flags stay as they are.
void apply_tunic_choice(PlayerReading& reading, const std::optional<std::string>& tunic);

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

  // The parts, for tests and for the emulator's debug overlay.
  std::optional<Vec3> read_position();      // GameUI copy, else the actor's
  std::optional<Quat> read_rotation();      // from the actor's matrix
  std::optional<protocol::Age> read_age();
  std::optional<std::string> read_scene();
  std::optional<bool> read_event_running();
  std::optional<std::string> read_cutscene();
  std::optional<std::uint8_t> read_ocarina_note();
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
  // The actor name of the piece in an equipment slot (3: slingshot, bow);
  // "" when the slot is empty or its piece is gone. Nothing when the slot
  // or the name could not be read.
  std::optional<std::string> read_slot_name(std::uint32_t slot_index);
  GameMemory& memory_;
  DrawnState own_drawn_{};
  DrawnState& drawn_memo_;
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
