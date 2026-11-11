#pragma once

// Builds the local player's PLAYER_SAMPLE, PLAYER_STATUS and PLAYER_EQUIP
// messages, once per emulated frame, as docs/protocol.md asks: a sample every
// frame the player moves (at most one per min_gap_us) and at least 4 a second
// while still, a status and
// the equipment on every change. It returns encoded messages; the caller
// sends them (coop::net::Client).

#include <cstdint>
#include <optional>
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
};

class Sampler {
 public:
  explicit Sampler(SamplerSettings settings = {}) : settings_(settings) {}

  // One emulated frame. `server_now_us` is coop::net::Client::server_now_us();
  // call only once the client is welcomed (it has a clock offset). The
  // status, if any, comes first in the result.
  std::vector<Outgoing> frame(const std::optional<PlayerReading>& reading,
                              std::uint64_t server_now_us);
  std::vector<Outgoing> frame(PlayerReader& reader, std::uint64_t server_now_us) {
    return frame(reader.read(), server_now_us);
  }

  // After a reconnect: send the status and equipment again and start
  // velocity afresh. The sequence keeps counting.
  void reset();

  // The last messages built, for tests and the debug overlay.
  const std::optional<protocol::PlayerSample>& last_sample() const { return last_sent_; }
  std::uint8_t status_flags() const { return flags_; }
  const std::optional<protocol::Equipment>& last_equipment() const { return equipment_sent_; }
  std::uint32_t next_sequence() const { return sequence_; }

 private:
  bool should_send(const PlayerReading& reading, std::uint64_t now) const;
  std::optional<Vec3> velocity(const PlayerReading& reading, std::uint64_t now) const;

  SamplerSettings settings_;
  std::uint32_t sequence_ = 0;
  std::uint8_t flags_ = 0;
  bool status_sent_ = false;
  std::optional<PlayerReading> previous_;   // last frame's reading
  std::uint64_t previous_time_us_ = 0;
  std::optional<protocol::PlayerSample> last_sent_;
  std::optional<protocol::Equipment> equipment_sent_;
};

}  // namespace coop::host
