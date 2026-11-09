#include "coop/host/sampler.hpp"

#include <algorithm>
#include <cmath>

#include "coop/protocol/codec.hpp"

namespace coop::host {
namespace {

double distance(const Vec3& a, const Vec3& b) {
  const double x = static_cast<double>(a[0]) - b[0];
  const double y = static_cast<double>(a[1]) - b[1];
  const double z = static_cast<double>(a[2]) - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

bool turned(const std::optional<Quat>& a, const std::optional<Quat>& b, float still) {
  if (a.has_value() != b.has_value()) return true;
  if (!a) return false;
  double dot = 0.0;
  for (std::size_t i = 0; i < 4; ++i) dot += static_cast<double>((*a)[i]) * (*b)[i];
  // q and -q are the same rotation.
  return 1.0 - std::fabs(dot) > still;
}

bool nonzero(const std::optional<Vec3>& v) {
  return v && std::any_of(v->begin(), v->end(), [](float c) { return c != 0.0f; });
}

void push(std::vector<Outgoing>& out, const protocol::Message& message) {
  if (auto bytes = protocol::encode(message)) {
    out.push_back({protocol::channel_of(message), std::move(*bytes)});
  }
}

}  // namespace

void Sampler::reset() {
  status_sent_ = false;
  previous_.reset();
  last_sent_.reset();
}

std::optional<Vec3> Sampler::velocity(const PlayerReading& reading, std::uint64_t now) const {
  if (!previous_ || now <= previous_time_us_) return std::nullopt;
  if (now - previous_time_us_ > settings_.max_velocity_gap_us) return std::nullopt;
  if (reading.scene && previous_->scene && *reading.scene != *previous_->scene) {
    return std::nullopt;  // a scene change is not movement
  }
  if (distance(reading.position, previous_->position) <= settings_.still_distance) {
    return Vec3{0.0f, 0.0f, 0.0f};
  }
  const double seconds = static_cast<double>(now - previous_time_us_) / 1e6;
  const double scale = 1.0 / (seconds * settings_.units_per_metre);
  Vec3 v{};
  double speed2 = 0.0;
  for (std::size_t i = 0; i < 3; ++i) {
    const double c = (static_cast<double>(reading.position[i]) - previous_->position[i]) * scale;
    speed2 += c * c;
    v[i] = static_cast<float>(c);
  }
  if (!(std::sqrt(speed2) <= settings_.max_speed)) return std::nullopt;  // a warp
  return v;
}

bool Sampler::should_send(const PlayerReading& reading, std::uint64_t now) const {
  if (!last_sent_) return true;
  const std::uint64_t last = last_sent_->time.server_time_us;
  if (now < last || now - last >= settings_.keepalive_us) return true;  // keep-alive
  if (distance(reading.position, *last_sent_->position) > settings_.still_distance) return true;
  // The first still frame after moving, so receivers stop extrapolating.
  if (nonzero(last_sent_->velocity)) return true;
  if (turned(reading.rotation, last_sent_->rotation, settings_.still_rotation)) return true;
  if (reading.scene != last_sent_->scene) return true;
  if (reading.age != last_sent_->age) return true;
  return false;
}

std::vector<Outgoing> Sampler::frame(const std::optional<PlayerReading>& reading,
                                     std::uint64_t server_now_us) {
  std::vector<Outgoing> out;
  if (!reading) {
    previous_.reset();  // no velocity across a gap in the readings
    return out;
  }

  std::uint8_t flags = flags_;
  if (reading->event_running) {
    flags = static_cast<std::uint8_t>(
        (flags & ~protocol::status::in_cutscene) |
        (*reading->event_running ? protocol::status::in_cutscene : 0));
  }
  if (!status_sent_ || flags != flags_) {
    flags_ = flags;
    status_sent_ = true;
    push(out, protocol::PlayerStatus{protocol::kPlayerSelf, flags_});
  }

  if (should_send(*reading, server_now_us)) {
    protocol::PlayerSample sample;
    sample.player_number = protocol::kPlayerSelf;
    sample.time = {server_now_us, sequence_};
    sample.position = reading->position;
    sample.velocity = velocity(*reading, server_now_us);
    sample.rotation = reading->rotation;
    sample.scene = reading->scene;
    sample.age = reading->age;
    ++sequence_;
    push(out, sample);
    last_sent_ = std::move(sample);
  }

  previous_ = reading;
  previous_time_us_ = server_now_us;
  return out;
}

}  // namespace coop::host
