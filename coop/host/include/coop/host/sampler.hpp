#pragma once

// Builds the local player's PLAYER_SAMPLE, PLAYER_STATUS and PLAYER_EQUIP
// messages, once per emulated frame, as docs/protocol.md asks: a sample every
// frame the player moves (at most one per min_gap_us) and at least 4 a second
// while still, a status and
// the equipment on every change. It returns encoded messages; the caller
// sends them (coop::net::Client).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/outgoing.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/protocol/messages.hpp"

namespace coop::host {

struct SamplerSettings {
  // Longest time between two samples while still. 200 ms keeps above 4 a
  // second even when frames come late.
  std::uint64_t keepalive_us = 200'000;
  // Shortest time between two samples, except for a scene or age change
  // (issue 106): frame() runs once per presented frame, and high refresh
  // (Ctrl+U) presents up to 165 a second. 14 ms lets every frame through at
  // 60 (with jitter) and gives 48-60 a second at 120-165.
  std::uint64_t min_gap_us = 14'000;
  // Smaller changes than this, in game units, are not movement.
  float still_distance = 1.0e-3f;
  // Smaller changes in any quaternion component are not turning.
  float still_rotation = 1.0e-4f;
  // Game units per metre: velocity on the wire is metres a second. 1 until a
  // run measures the game's unit.
  float units_per_metre = 1.0f;
  // Two readings further apart than this give no velocity (a stall or load).
  std::uint64_t max_velocity_gap_us = 250'000;
  // Faster than this, in metres a second, is a warp: no velocity is sent.
  float max_speed = 200.0f;
  // The player's colour, 0xRRGGBB (coop-options.ini `color`), sent in
  // PLAYER_EQUIP with the equipment. Unset: none is sent, and the others use
  // this player's slot colour (protocol::slot_color).
  std::optional<std::uint32_t> color;
};

// jitter-b: the server time of the game frame a reading is from: `server_now_us` less `age_us`
// (how long ago that frame ran; 0 not known). An age past kFrameAgeMaxUs (a pause, a load) is
// not that frame: now.
inline constexpr std::uint64_t kFrameAgeMaxUs = 250'000;  // a 4 fps frame (review-jitter)
inline std::uint64_t frame_aligned_us(std::uint64_t server_now_us, std::uint64_t age_us) {
  if (age_us == 0 || age_us > kFrameAgeMaxUs || age_us > server_now_us) return server_now_us;
  return server_now_us - age_us;
}

// PLAYER_STATUS's bits for this frame (docs/protocol.md, docs/puppet.md "Away"), from the bits sent last:
// - loading: no Link to read (the game loads a scene or a save, or shows its title), or the title's menu is up;
//   menus count for nothing then, and in_cutscene and dead keep what they were (a load inside an event, a Game Over's
//   reload);
// - in_cutscene: our Link in an event (G1: our_link_in_event); an unknown event state keeps the bit;
// - in_menu: a game menu is in front (PlayerReading::in_menu);
// - paused: a game menu is in front and our Link is in no event: the pause menu or the panel, which stop the game's
//   world (a talk's choices are a menu inside an event: the world runs on);
// - dead: our Link's life at 0 (PlayerReading::dead); unknown keeps the bit.
std::uint8_t status_bits(std::uint8_t previous, const std::optional<PlayerReading>& reading, bool title_up);

// Our world stands still with no game menu in front (row 164 follow-up): the quick-switch item menu stops the world
// as the pause menu does, but patch 0018's menu state does not name it (it is no screen with a focused button), so a
// player there was "playing" to every away rule: a lift they led held the others, a boss they owned stood still on
// every screen. Read from the game itself: our Link's main animation (its AS slot 0) at a rate other than 0 whose frame
// and place do not move for kStillUs while no event runs. Any change, or an animation at rate 0 (a pose held on
// purpose), is a running world. One per game, fed every reading.
// LICHII_COOP_STILL's value: WorldStill feeds `paused` only when it is "1" (pr-navi-01112: opt-in, off by default;
// a held animation frame is no paused world: aiming, a ledge, a ladder, ice, a Like-Like's hold, a shock).
inline bool world_still_on(const char* value) { return value != nullptr && std::string_view(value) == "1"; }

class WorldStill {
 public:
  static constexpr std::uint64_t kStillUs = 300'000;  // 0.3 s: a held animation frame for that long is no game frame
  static constexpr float kFrameEpsilon = 1e-3f;      // AS frames
  static constexpr float kPlaceEpsilon = 1e-3f;      // metres

  // This reading at `now_us`; true while our world stands still.
  bool step(const PlayerReading& reading, std::uint64_t now_us) {
    return step(reading.animation ? &*reading.animation : nullptr, reading.position,
                reading.event_running.value_or(false), now_us);
  }
  // The same from its parts: `animation` null is not judged (none read, or not the game's own: a carried Link's
  // reading plays the carried pose, CoopLiftPlayerGame::CarriedPose, held at one frame while its carrier stands).
  bool step(const protocol::Animation* animation, const Vec3& position, bool event, std::uint64_t now_us);
  bool still() const { return still_; }

 private:
  bool seen_ = false;
  bool still_ = false;
  std::string name_;
  float frame_ = 0.0f;
  Vec3 place_{};
  std::uint64_t changed_us_ = 0;
};

class Sampler {
 public:
  explicit Sampler(SamplerSettings settings = {}) : settings_(settings) {}

  // One emulated frame. `server_now_us` is coop::net::Client::server_now_us();
  // call only once the client is welcomed (it has a clock offset). The
  // status, if any, comes first in the result. No reading (no Link) sends no
  // sample, only the status `loading`; so does `title_up` (status_bits).
  std::vector<Outgoing> frame(const std::optional<PlayerReading>& reading,
                              std::uint64_t server_now_us, bool title_up = false);
  std::vector<Outgoing> frame(PlayerReader& reader, std::uint64_t server_now_us) {
    return frame(reader.read(), server_now_us);
  }

  // After a reconnect: send the status and equipment again and start
  // velocity afresh. The sequence keeps counting.
  void reset();

  // The game is held and runs no frame() (the play-test bridge's hold,
  // docs/playtest.md "Frame control"): the last sample sent again with this
  // time and no velocity, once the keep-alive time has passed since it. The
  // others then see a player standing still, not one whose samples stopped:
  // presence fades those out after extrapolate_us + fade_us, and a faded
  // player has no puppet (docs/puppet.md). Nothing while not in play (the
  // last frame() had no reading) or before any sample.
  // frame() after a hold stamps no sample at or before the last held one: its
  // time is the game frame's (now less the frame's age), which can be older
  // than the held sample's, and presence would drop it as too old.
  std::optional<Outgoing> hold(std::uint64_t server_now_us);
  // A reconnect while held (no frame() comes to send them): the status, the
  // equipment and the last sample, as the first frame after a WELCOME sends
  // them. Empty while not in play or before any sample.
  std::vector<Outgoing> hold_reconnected(std::uint64_t server_now_us);

  // The player changed their colour while the game runs (docs/puppet.md,
  // "Changing your colour in the game"): the next frame() sends it in
  // PLAYER_EQUIP. Unset after a colour ("by player number" in the menu):
  // from then on it sends this player's slot colour (protocol::slot_color
  // of set_player_number's number), because an unset colour in PLAYER_EQUIP
  // leaves the others on the old one (protocol::merge_equipment).
  void set_color(std::optional<std::uint32_t> color) {
    if (!color && settings_.color) by_number_ = true;
    if (color) by_number_ = false;
    settings_.color = color;
  }
  // This player's number from the last WELCOME (0 before): the slot colour
  // sent after set_color went back to unset.
  void set_player_number(std::uint8_t number) { player_number_ = number; }

  // The last messages built, for tests and the debug overlay.
  const std::optional<protocol::PlayerSample>& last_sample() const { return last_sent_; }
  std::uint8_t status_flags() const { return flags_; }
  const std::optional<protocol::Equipment>& last_equipment() const { return equipment_sent_; }
  std::uint32_t next_sequence() const { return sequence_; }

 private:
  bool should_send(const PlayerReading& reading, std::uint64_t now) const;
  std::optional<Vec3> velocity(const PlayerReading& reading, std::uint64_t now) const;

  SamplerSettings settings_;
  bool by_number_ = false;           // set_color went back to unset after a colour
  std::uint8_t player_number_ = 0;   // set_player_number's
  std::uint32_t sequence_ = 0;
  std::uint8_t flags_ = 0;
  bool status_sent_ = false;
  std::optional<PlayerReading> previous_;   // last frame's reading
  std::uint64_t previous_time_us_ = 0;
  std::optional<protocol::PlayerSample> last_sent_;
  std::optional<protocol::Equipment> equipment_sent_;
  std::uint64_t hold_floor_us_ = 0;  // the last held sample's time: frame() stamps after it
};

}  // namespace coop::host
