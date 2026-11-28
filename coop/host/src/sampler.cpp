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

bool animation_changed(const std::optional<protocol::Animation>& a,
                       const std::optional<protocol::Animation>& b) {
  if (a.has_value() != b.has_value()) return true;
  if (!a) return false;
  return a->name != b->name || a->rate != b->rate;
}

void push(std::vector<Outgoing>& out, const protocol::Message& message) {
  if (auto bytes = protocol::encode(message)) {
    out.push_back({protocol::channel_of(message), std::move(*bytes)});
  }
}

}  // namespace

std::uint8_t status_bits(std::uint8_t previous, const std::optional<PlayerReading>& reading, bool title_up) {
  namespace st = protocol::status;
  std::uint8_t flags = previous;
  const auto set = [&flags](std::uint8_t bit, bool on) {
    flags = static_cast<std::uint8_t>(on ? (flags | bit) : (flags & ~bit));
  };
  const bool play = reading.has_value() && !title_up;
  set(st::loading, !play);
  if (reading && reading->event_running) {
    // G1 (event_owner.hpp): only an event our Link is in. Another player's event replayed here
    // and our free Ocarina (its take-out is a plain event, ~4 s: issue 86) do not hold our puppet
    // in the other games.
    set(st::in_cutscene, our_link_in_event(*reading->event_running, reading->event_whose));
  }
  const bool menu = play && reading->in_menu.value_or(false) && !reading->menu_world_runs;
  set(st::in_menu, menu);
  // Row 164 follow-up: a world standing still with no menu named (the quick-switch menu) is paused too, not in_menu
  // (no menu is known to be in front).
  const bool still = play && reading->world_still;
  set(st::paused, (menu || still) && (flags & st::in_cutscene) == 0);
  if (reading && reading->dead) set(st::dead, *reading->dead);
  return flags;
}

bool WorldStill::step(const protocol::Animation* animation, const Vec3& position, bool event, std::uint64_t now_us) {
  if (animation == nullptr || animation->rate == 0.0f || event) {
    // No animation read, a pose held on purpose, or our event (a cutscene may hold Link): not judged.
    seen_ = false;
    still_ = false;
    return false;
  }
  const protocol::Animation& a = *animation;
  const bool moved = !seen_ || a.name != name_ || std::fabs(a.frame - frame_) > kFrameEpsilon ||
                     distance(position, place_) > kPlaceEpsilon;
  if (moved) {
    seen_ = true;
    name_ = a.name;
    frame_ = a.frame;
    place_ = position;
    changed_us_ = now_us;
    still_ = false;
    return false;
  }
  still_ = now_us >= changed_us_ && now_us - changed_us_ >= kStillUs;
  return still_;
}

void Sampler::reset() {
  status_sent_ = false;
  previous_.reset();
  last_sent_.reset();
  equipment_sent_.reset();
  hold_floor_us_ = 0;  // a new connection's clock
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
  if (reading.scene != last_sent_->scene) return true;
  if (reading.age != last_sent_->age) return true;
  // Anything else waits for the gap: the change is still there next frame,
  // as it is compared with the last sample sent, not the last frame.
  if (now - last < settings_.min_gap_us) return false;
  if (distance(reading.position, *last_sent_->position) > settings_.still_distance) return true;
  // The first still frame after moving, so receivers stop extrapolating.
  if (nonzero(last_sent_->velocity)) return true;
  if (turned(reading.rotation, last_sent_->rotation, settings_.still_rotation)) return true;
  // A new AS command or rate, not a new frame: receivers run the frame on
  // from the rate, and the keep-alive corrects it.
  if (animation_changed(reading.animation, last_sent_->animation)) return true;
  return false;
}

std::vector<Outgoing> Sampler::frame(const std::optional<PlayerReading>& reading,
                                     std::uint64_t frame_us, bool title_up) {
  // Just after a hold the frame's time can be older than the last held
  // sample's: stamp after it (hold), each frame a little later, until the
  // frames pass it.
  std::uint64_t server_now_us = frame_us;
  // The floor covers only a frame's age (kFrameAgeMaxUs). A frame further back is another clock (a new server).
  if (hold_floor_us_ != 0) {
    if (frame_us <= hold_floor_us_ && hold_floor_us_ - frame_us <= kFrameAgeMaxUs) {
      server_now_us = ++hold_floor_us_;
    } else {
      hold_floor_us_ = 0;
    }
  }
  std::vector<Outgoing> out;
  const std::uint8_t flags = status_bits(flags_, reading, title_up);
  if (!status_sent_ || flags != flags_) {
    flags_ = flags;
    status_sent_ = true;
    push(out, protocol::PlayerStatus{protocol::kPlayerSelf, flags_});
  }
  if (!reading) {
    previous_.reset();  // no velocity across a gap in the readings
    return out;
  }

  std::optional<protocol::Equipment> read = reading->equipment;
  std::optional<std::uint32_t> color = settings_.color;
  if (!color && by_number_ && player_number_ != 0) color = protocol::slot_color(player_number_);
  if (color) {
    if (!read) read = protocol::Equipment{};
    read->color = color;
  }
  if (read) {
    const protocol::Equipment now =
        equipment_sent_ ? protocol::merge_equipment(*equipment_sent_, *read) : *read;
    if (now != equipment_sent_) {
      push(out, protocol::PlayerEquip{protocol::kPlayerSelf, now});
      equipment_sent_ = now;
    }
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
    sample.animation = reading->animation;
    ++sequence_;
    push(out, sample);
    last_sent_ = std::move(sample);
  }

  previous_ = reading;
  previous_time_us_ = server_now_us;
  return out;
}

std::optional<Outgoing> Sampler::hold(std::uint64_t server_now_us) {
  if (!previous_ || !last_sent_ || !last_sent_->position) return std::nullopt;
  const std::uint64_t last = last_sent_->time.server_time_us;
  if (server_now_us <= last || server_now_us - last < settings_.keepalive_us) return std::nullopt;
  protocol::PlayerSample sample = *last_sent_;
  sample.time = {server_now_us, sequence_};
  sample.velocity = Vec3{0.0f, 0.0f, 0.0f};  // held: standing, not moving on
  ++sequence_;
  std::vector<Outgoing> out;
  push(out, sample);
  if (out.empty()) return std::nullopt;
  last_sent_ = std::move(sample);
  hold_floor_us_ = server_now_us;
  return std::move(out.front());
}

std::vector<Outgoing> Sampler::hold_reconnected(std::uint64_t server_now_us) {
  std::vector<Outgoing> out;
  if (!previous_ || !last_sent_ || !last_sent_->position) return out;
  push(out, protocol::PlayerStatus{protocol::kPlayerSelf, flags_});
  status_sent_ = true;
  if (equipment_sent_) push(out, protocol::PlayerEquip{protocol::kPlayerSelf, *equipment_sent_});
  protocol::PlayerSample sample = *last_sent_;
  // The new connection's clock: after a server restart the old sample's time is the old server's uptime, and the
  // others' presence forgot the old samples with their own WELCOME.
  const std::uint64_t at = server_now_us;
  sample.time = {at, sequence_};
  sample.velocity = Vec3{0.0f, 0.0f, 0.0f};
  ++sequence_;
  push(out, sample);
  last_sent_ = std::move(sample);
  hold_floor_us_ = at;
  return out;
}

}  // namespace coop::host
