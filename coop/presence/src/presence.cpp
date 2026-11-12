#include "coop/presence/presence.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace coop::presence {
namespace {

bool earlier(const protocol::SampleTime& a, const protocol::SampleTime& b) {
  if (a.server_time_us != b.server_time_us) return a.server_time_us < b.server_time_us;
  return a.sequence < b.sequence;
}

using DVec = std::array<double, 3>;

// Sequence a after b, allowing for the u32 wrap.
bool sequence_after(std::uint32_t a, std::uint32_t b) {
  return static_cast<std::int32_t>(a - b) > 0;
}

DVec to_d(const Vec3& v) { return {v[0], v[1], v[2]}; }

// A double made into a float: converting one out of the float range is
// undefined, so clamp to the largest finite float; a NaN becomes 0.
float to_float(double x) {
  constexpr double kMax = std::numeric_limits<float>::max();
  if (std::isnan(x)) return 0.0f;
  return static_cast<float>(std::clamp(x, -kMax, kMax));
}

Vec3 to_f(const DVec& v) { return {to_float(v[0]), to_float(v[1]), to_float(v[2])}; }

template <std::size_t N>
bool all_finite(const std::array<float, N>& v) {
  return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); });
}

// Normalise in place; false if it has no length or a part that is not
// finite. In double, so no float input can overflow the squares.
bool normalise(std::array<double, 4>& q) {
  const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!std::isfinite(n) || !(n > 0.0)) return false;
  for (double& c : q) c /= n;
  return true;
}

std::optional<Quat> unit_quat(const Quat& q) {
  std::array<double, 4> d{q[0], q[1], q[2], q[3]};
  if (!normalise(d)) return std::nullopt;
  return Quat{to_float(d[0]), to_float(d[1]), to_float(d[2]), to_float(d[3])};
}

DVec velocity_or_zero(const Sample& s) {
  return s.velocity ? to_d(*s.velocity) : DVec{0.0, 0.0, 0.0};
}

double seconds(std::uint64_t us) { return static_cast<double>(us) / 1e6; }

// The newest rotation among samples[0..i].
std::optional<Quat> rotation_up_to(const std::deque<Sample>& samples, std::size_t i) {
  for (std::size_t k = i + 1; k-- > 0;) {
    if (samples[k].rotation) return samples[k].rotation;
  }
  return std::nullopt;
}

bool valid_number(std::uint8_t n) { return n >= 1 && n <= 4; }

}  // namespace

Quat slerp(const Quat& a, const Quat& b, double t) {
  std::array<double, 4> q0{a[0], a[1], a[2], a[3]};
  std::array<double, 4> q1{b[0], b[1], b[2], b[3]};
  constexpr std::array<double, 4> kIdentity{0.0, 0.0, 0.0, 1.0};
  if (!normalise(q0)) q0 = kIdentity;
  if (!normalise(q1)) q1 = kIdentity;
  if (!std::isfinite(t)) return {0.0f, 0.0f, 0.0f, 1.0f};
  double dot = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3];
  if (dot < 0.0) {  // q and -q are the same rotation: take the short way
    for (double& c : q1) c = -c;
    dot = -dot;
  }
  std::array<double, 4> out{};
  if (dot > 0.9995) {  // nearly the same: a normalised straight line is exact enough
    for (std::size_t k = 0; k < 4; ++k) out[k] = q0[k] + (q1[k] - q0[k]) * t;
    if (!normalise(out)) out = kIdentity;
  } else {
    const double theta = std::acos(std::clamp(dot, -1.0, 1.0));
    const double s = std::sin(theta);
    const double w0 = std::sin((1.0 - t) * theta) / s;
    const double w1 = std::sin(t * theta) / s;
    for (std::size_t k = 0; k < 4; ++k) out[k] = w0 * q0[k] + w1 * q1[k];
  }
  return {to_float(out[0]), to_float(out[1]), to_float(out[2]), to_float(out[3])};
}

Player::Player(std::uint8_t number, std::string nickname)
    : number_(number), nickname_(std::move(nickname)) {}

bool Player::same_scene(std::string_view local_scene) const {
  return scene_ && *scene_ == local_scene;
}

SampleResult Player::add(const protocol::PlayerSample& in) {
  const protocol::SampleTime& t = in.time;
  // The decoder refuses NaN and infinity, but this may be fed from elsewhere.
  if ((in.position && !all_finite(*in.position)) || (in.velocity && !all_finite(*in.velocity))) {
    return SampleResult::invalid;
  }
  std::optional<Quat> rotation;
  if (in.rotation) {
    rotation = unit_quat(*in.rotation);
    if (!rotation) return SampleResult::invalid;
  }
  for (const Sample& s : samples_) {
    if (s.time.sequence == t.sequence) return SampleResult::duplicate;
  }

  // Scene and age: only from a sample newer than the one that last set them.
  // Taken before the drawing checks, so a scene change in a sample too late
  // to draw is still used.
  if (in.scene && (!scene_sequence_ || sequence_after(t.sequence, *scene_sequence_))) {
    scene_ = in.scene;
    scene_sequence_ = t.sequence;
  }
  if (in.age && (!age_sequence_ || sequence_after(t.sequence, *age_sequence_))) {
    age_ = in.age;
    age_sequence_ = t.sequence;
  }
  // The cutscene is in every sample while it plays and in none after it.
  if (!cutscene_sequence_ || sequence_after(t.sequence, *cutscene_sequence_)) {
    cutscene_ = in.cutscene;
    if (in.position) cutscene_place_ = in.position;
    cutscene_sequence_ = t.sequence;
  }

  if (!in.position) return SampleResult::state_only;
  // Older than the time already shown: it would rewrite a past already
  // drawn, so it is dropped, unless it is newer than every buffered sample.
  // Then it is the best there is, and a sender whose samples all arrive
  // later than the show delay is still shown (extrapolated).
  if (last_shown_us_ && t.server_time_us < *last_shown_us_ && !samples_.empty() &&
      earlier(t, samples_.back().time)) {
    return SampleResult::too_old;
  }
  if (samples_.size() >= kSampleBuffer && earlier(t, samples_.front().time)) {
    return SampleResult::too_old;
  }

  Sample s;
  s.time = t;
  s.position = *in.position;
  s.velocity = in.velocity;
  s.rotation = rotation;
  const auto at = std::upper_bound(
      samples_.begin(), samples_.end(), t,
      [](const protocol::SampleTime& key, const Sample& x) { return earlier(key, x.time); });
  samples_.insert(at, s);
  while (samples_.size() > kSampleBuffer) samples_.pop_front();
  return SampleResult::buffered;
}

std::optional<Pose> Player::pose_at(std::uint64_t show_us, const Settings& settings) const {
  if (samples_.empty() || samples_.front().time.server_time_us > show_us) return std::nullopt;
  std::size_t i = 0;
  while (i + 1 < samples_.size() && samples_[i + 1].time.server_time_us <= show_us) ++i;
  const Sample& a = samples_[i];
  const double scale = std::isfinite(settings.velocity_scale) && settings.velocity_scale > 0.0
                           ? settings.velocity_scale
                           : 1.0;

  Pose pose;
  pose.at_us = show_us;

  if (i + 1 < samples_.size()) {
    const Sample& b = samples_[i + 1];
    const std::uint64_t gap = b.time.server_time_us - a.time.server_time_us;  // > 0 here
    if (gap <= settings.max_gap_us) {
      const double s = static_cast<double>(show_us - a.time.server_time_us) /
                       static_cast<double>(gap);
      const double d = seconds(gap);
      const DVec p0 = to_d(a.position);
      const DVec p1 = to_d(b.position);
      DVec p{};
      DVec v{};
      if (settings.hermite && a.velocity && b.velocity) {
        const DVec m0 = to_d(*a.velocity);
        const DVec m1 = to_d(*b.velocity);
        const double s2 = s * s;
        const double s3 = s2 * s;
        const double h00 = 2 * s3 - 3 * s2 + 1;
        const double h10 = s3 - 2 * s2 + s;
        const double h01 = -2 * s3 + 3 * s2;
        const double h11 = s3 - s2;
        const double g00 = 6 * s2 - 6 * s;
        const double g10 = 3 * s2 - 4 * s + 1;
        const double g01 = -6 * s2 + 6 * s;
        const double g11 = 3 * s2 - 2 * s;
        for (std::size_t k = 0; k < 3; ++k) {
          const double t0 = m0[k] * scale * d;
          const double t1 = m1[k] * scale * d;
          p[k] = h00 * p0[k] + h10 * t0 + h01 * p1[k] + h11 * t1;
          v[k] = (g00 * p0[k] + g10 * t0 + g01 * p1[k] + g11 * t1) / d / scale;
        }
      } else {
        for (std::size_t k = 0; k < 3; ++k) {
          p[k] = p0[k] + (p1[k] - p0[k]) * s;
          v[k] = (p1[k] - p0[k]) / d / scale;
        }
      }
      pose.position = to_f(p);
      pose.velocity = to_f(v);
      const std::optional<Quat> ra = rotation_up_to(samples_, i);
      if (ra && b.rotation) {
        pose.rotation = slerp(*ra, *b.rotation, s);
      } else {
        pose.rotation = ra;
      }
      pose.how = How::interpolated;
      pose.fade = 1.0f;
      return pose;
    }
    // Too far apart to join: treat `a` as the last sample until `b`.
  }

  const std::uint64_t ahead = show_us - a.time.server_time_us;
  const std::uint64_t used = std::min(ahead, settings.extrapolate_us);
  const DVec p0 = to_d(a.position);
  const DVec v0 = velocity_or_zero(a);
  DVec p{};
  for (std::size_t k = 0; k < 3; ++k) p[k] = p0[k] + v0[k] * scale * seconds(used);
  pose.position = to_f(p);
  pose.rotation = rotation_up_to(samples_, i);
  if (ahead <= settings.extrapolate_us) {
    pose.velocity = to_f(v0);
    pose.how = How::extrapolated;
    pose.fade = 1.0f;
  } else {
    pose.velocity = {0.0f, 0.0f, 0.0f};
    pose.how = How::held;
    const std::uint64_t past = ahead - settings.extrapolate_us;
    if (settings.fade_us == 0 || past >= settings.fade_us) {
      pose.fade = 0.0f;
    } else {
      pose.fade = static_cast<float>(1.0 - static_cast<double>(past) /
                                               static_cast<double>(settings.fade_us));
    }
  }
  return pose;
}

Presence::Presence(Settings settings) : settings_(settings) {}

Player* Presence::find(std::uint8_t number) {
  if (!valid_number(number) || number == self_) return nullptr;
  auto& slot = players_[number - 1];
  return slot ? &*slot : nullptr;
}

const Player* Presence::player(std::uint8_t number) const {
  return const_cast<Presence*>(this)->find(number);
}

bool Presence::apply(const protocol::Message& message) {
  if (const auto* m = std::get_if<protocol::Welcome>(&message)) {
    on_welcome(*m);
    return true;
  }
  if (const auto* m = std::get_if<protocol::PlayerJoined>(&message)) return on_joined(*m);
  if (const auto* m = std::get_if<protocol::PlayerLeft>(&message)) return on_left(*m);
  if (const auto* m = std::get_if<protocol::PlayerStatus>(&message)) return on_status(*m);
  if (const auto* m = std::get_if<protocol::PlayerSample>(&message)) {
    const SampleResult r = on_sample(*m);
    return r == SampleResult::buffered || r == SampleResult::state_only;
  }
  return false;
}

void Presence::on_welcome(const protocol::Welcome& welcome) {
  clear();
  self_ = valid_number(welcome.player_number) ? welcome.player_number : 0;
  for (const protocol::PlayerInfo& p : welcome.players) {
    if (valid_number(p.number) && p.number != self_) players_[p.number - 1].emplace(p.number, p.nickname);
  }
}

bool Presence::on_joined(const protocol::PlayerJoined& joined) {
  const std::uint8_t n = joined.player_number;
  if (!valid_number(n) || n == self_) return false;
  players_[n - 1].emplace(n, joined.nickname);
  return true;
}

bool Presence::on_left(const protocol::PlayerLeft& left) {
  Player* p = find(left.player_number);
  if (p == nullptr) return false;
  players_[left.player_number - 1].reset();
  return true;
}

bool Presence::on_status(const protocol::PlayerStatus& status) {
  Player* p = find(status.player_number);
  if (p == nullptr) return false;
  p->flags_ = status.flags;
  return true;
}

SampleResult Presence::on_sample(const protocol::PlayerSample& sample) {
  if (self_ != 0 && sample.player_number == self_) return SampleResult::self;
  Player* p = find(sample.player_number);
  if (p == nullptr) return SampleResult::unknown_player;
  return p->add(sample);
}

void Presence::clear() {
  self_ = 0;
  for (auto& slot : players_) slot.reset();
}

std::vector<std::uint8_t> Presence::players() const {
  std::vector<std::uint8_t> out;
  for (const auto& slot : players_) {
    if (slot) out.push_back(slot->number());
  }
  return out;
}

std::vector<std::uint8_t> Presence::players_in_scene(std::string_view local_scene) const {
  std::vector<std::uint8_t> out;
  for (const auto& slot : players_) {
    if (slot && slot->same_scene(local_scene)) out.push_back(slot->number());
  }
  return out;
}

std::optional<Pose> Presence::shown(std::uint8_t number, std::uint64_t server_now_us) {
  Player* p = find(number);
  if (p == nullptr) return std::nullopt;
  const std::uint64_t show_us =
      server_now_us > settings_.show_delay_us ? server_now_us - settings_.show_delay_us : 0;
  if (!p->last_shown_us_ || show_us > *p->last_shown_us_) p->last_shown_us_ = show_us;
  return p->pose_at(show_us, settings_);
}

bool Presence::same_scene(std::uint8_t number, std::string_view local_scene) const {
  const Player* p = player(number);
  return p != nullptr && p->same_scene(local_scene);
}

}  // namespace coop::presence
