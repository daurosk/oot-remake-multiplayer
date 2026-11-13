// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where a voice sounds from (docs/voice.md, "Playback"): the speaker's Link
// heard from the game's camera. Distance gives the loudness (full up to
// `full_m`, then falling as 1/distance, faded to silence at `range_m`) and
// the high frequencies (a one-pole low-pass that closes with distance, as far
// sounds are duller); the direction on the camera's horizontal plane gives an
// equal-power pan, and for 5.1 the split between front and rear speakers.
// Positions are the game's world units, metres.
#pragma once

#include <array>
#include <optional>

namespace coop::voice {

using Vec3 = std::array<float, 3>;

struct Listener {
  Vec3 position{};
  Vec3 right{1, 0, 0};    // unit, the camera's right
  Vec3 forward{0, 0, -1}; // unit, where the camera looks
};

struct SpatialConfig {
  float full_m = 3.0f;    // full loudness this close
  float range_m = 30.0f;  // silent from here
  float fade_m = 6.0f;    // the last stretch before range_m fades to 0
  float min_cutoff_hz = 2500.0f;  // the low-pass at range_m (none up close)
  // False when the listener's facing is not known (no camera): every voice
  // in the middle, loudness and dullness by distance only.
  bool pan = true;
};

struct Placement {
  bool audible = false;
  float distance = 0;
  float gain = 0;          // 0 to 1
  // Per speaker, gain included. Stereo uses left/right; 5.1 uses all four
  // (front pair and rear pair; the centre and LFE get nothing).
  float front_left = 0, front_right = 0, rear_left = 0, rear_right = 0;
  float left = 0, right = 0;  // the stereo pair: front and rear folded
  float lowpass = 1;       // one-pole coefficient at 48 kHz: 1 = open
};

Placement place(const Listener& listener, const Vec3& source, const SpatialConfig& config);

// The listener from the game's world-to-clip matrix, row-major as
// CoopCamera::Frame keeps it (clip = m * p): the camera sits where the x, y
// and w rows are all zero, looks along the w row and has the x row to its
// right. Empty for a matrix with no such point (not a perspective).
std::optional<Listener> listener_from_clip(const std::array<float, 16>& clip_from_world);

// The listener at a Link, looking along its facing (yaw about +y, radians,
// 0 = looking down -z): for when no camera is known.
Listener listener_at(const Vec3& position, float yaw);

}  // namespace coop::voice
