#pragma once

// Presence: where to draw the other players, from the messages they send.
//
// Game-free: no sockets, no ENet, no game addresses. Feed it the messages
// decoded by coop/protocol (PLAYER_SAMPLE, PLAYER_STATUS, PLAYER_JOINED,
// PLAYER_LEFT, WELCOME) and ask it each frame what to draw for each player.
// See docs/presence.md for the defaults and how to tune them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::presence {

using protocol::Age;
using protocol::Quat;  // x, y, z, w
using protocol::Vec3;

// How many samples are kept per player (docs/protocol.md, PLAYER_SAMPLE).
inline constexpr std::size_t kSampleBuffer = 16;

struct Settings {
  // How far behind the receiver's estimate of the server clock the players
  // are drawn. Bigger hides more jitter and loss; smaller is less late.
  std::uint64_t show_delay_us = 100'000;
  // How far past the last sample the pose is carried on with its velocity.
  std::uint64_t extrapolate_us = 150'000;
  // After the extrapolation ends, how long the held pose takes to fade from
  // 1 to 0. Zero: gone at once.
  std::uint64_t fade_us = 250'000;
  // Two samples further apart than this are not joined: the player paused
  // or stalled in between, so it is shown held at the first one, then
  // jumps to the second. Senders send at least 4 a second while still, so
  // this must stay above 250 ms.
  std::uint64_t max_gap_us = 500'000;
  // Cubic Hermite between two samples using their velocities (when both
  // have one); otherwise a straight line.
  bool hermite = true;
  // Position units per velocity unit (velocity is "per second" in both).
  // 1 when the sender's position and velocity use the same unit. Not above
  // zero: taken as 1.
  double velocity_scale = 1.0;
};

// How a shown pose was made.
enum class How : std::uint8_t {
  interpolated,  // between two samples
  extrapolated,  // past the last sample, within Settings::extrapolate_us
  held,          // past the extrapolation: held still and fading
};

struct Pose {
  Vec3 position{};
  Vec3 velocity{};  // in the samples' velocity unit
  // The newest rotation the samples have up to the shown time; none if no
  // sample so far carried one. Not extrapolated (no spin on the wire).
  std::optional<Quat> rotation;
  How how = How::interpolated;
  float fade = 1.0f;  // 1 fully shown, 0 gone
  std::uint64_t at_us = 0;  // the server time this pose is for
};

// One buffered sample. Only samples with a position are buffered.
struct Sample {
  protocol::SampleTime time;
  Vec3 position{};
  std::optional<Vec3> velocity;
  std::optional<Quat> rotation;
};

// What happened to a PLAYER_SAMPLE handed to Presence.
enum class SampleResult : std::uint8_t {
  buffered,        // kept for drawing (and its scene and age used)
  state_only,      // no position: only its scene and age were used
  duplicate,       // a sample with that sequence is already buffered
  too_old,         // older than what was already shown, or than a full buffer
  unknown_player,  // no PLAYER_JOINED for that number (or it left)
  self,            // about this player, not a remote one
};

class Player {
 public:
  explicit Player(std::uint8_t number, std::string nickname = {});

  std::uint8_t number() const { return number_; }
  const std::string& nickname() const { return nickname_; }

  // PLAYER_STATUS flags as last received (protocol::status bits; unknown
  // bits kept).
  std::uint8_t flags() const { return flags_; }
  bool has_flag(std::uint8_t bit) const { return (flags_ & bit) != 0; }

  // Scene and age from the newest sample (by server time, then sequence)
  // that carried them. A late, older sample does not overwrite them.
  const std::optional<std::string>& scene() const { return scene_; }
  std::optional<Age> age() const { return age_; }
  // False while the player's scene is not known.
  bool same_scene(std::string_view local_scene) const;

  // Buffered samples, oldest first, ordered by server time then sequence.
  const std::deque<Sample>& samples() const { return samples_; }

  // The pose at server time `show_us` (the delay already taken off).
  // Nothing while every sample is newer than that, or none is buffered.
  std::optional<Pose> pose_at(std::uint64_t show_us, const Settings& settings) const;

 private:
  friend class Presence;

  SampleResult add(const protocol::PlayerSample& sample);

  std::uint8_t number_;
  std::string nickname_;
  std::uint8_t flags_ = 0;
  std::optional<std::string> scene_;
  std::optional<protocol::SampleTime> scene_time_;
  std::optional<Age> age_;
  std::optional<protocol::SampleTime> age_time_;
  std::deque<Sample> samples_;
  std::optional<std::uint64_t> last_shown_us_;
};

class Presence {
 public:
  explicit Presence(Settings settings = {});

  const Settings& settings() const { return settings_; }
  void set_settings(const Settings& settings) { settings_ = settings; }

  // This player's own number (1 to 4), from WELCOME. Messages about it are
  // ignored. 0 while not known.
  std::uint8_t self() const { return self_; }

  // Hand over any decoded message; the ones presence does not use are
  // ignored. Returns true if it changed anything.
  bool apply(const protocol::Message& message);

  // WELCOME: forgets everyone, then adds the players already in.
  void on_welcome(const protocol::Welcome& welcome);
  // PLAYER_JOINED: adds the player. A number already in is started afresh
  // (it left and came back while a PLAYER_LEFT was lost or late).
  bool on_joined(const protocol::PlayerJoined& joined);
  bool on_left(const protocol::PlayerLeft& left);
  bool on_status(const protocol::PlayerStatus& status);
  // A sample for a number with no PLAYER_JOINED is dropped: samples come on
  // an unreliable channel and may arrive after PLAYER_LEFT.
  SampleResult on_sample(const protocol::PlayerSample& sample);

  void clear();

  // Numbers of the remote players in, lowest first.
  std::vector<std::uint8_t> players() const;
  // Those whose scene is `local_scene`.
  std::vector<std::uint8_t> players_in_scene(std::string_view local_scene) const;
  const Player* player(std::uint8_t number) const;

  // What to draw now for `number`, given the receiver's estimate of the
  // server clock. Shows Settings::show_delay_us behind it, and remembers the
  // time shown so that later samples older than it are dropped.
  std::optional<Pose> shown(std::uint8_t number, std::uint64_t server_now_us);
  bool same_scene(std::uint8_t number, std::string_view local_scene) const;

 private:
  Player* find(std::uint8_t number);

  Settings settings_;
  std::uint8_t self_ = 0;
  std::array<std::optional<Player>, 4> players_;
};

// Shortest-path spherical interpolation of two rotations, t in 0..1. The
// inputs need not be normalised; the result is.
Quat slerp(const Quat& a, const Quat& b, double t);

}  // namespace coop::presence
