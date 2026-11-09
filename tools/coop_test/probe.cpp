#include "probe.hpp"

#include <algorithm>
#include <string_view>
#include <utility>
#include <variant>

namespace coop::test_tool {

namespace proto = coop::protocol;

namespace {

constexpr std::uint64_t kUnreliablePeriodUs = 1'000'000 / 60;
constexpr std::uint64_t kReliablePeriodUs = 1'000'000;
constexpr std::uint64_t kSamplePeriodUs = 1'000'000 / 60;
constexpr std::uint64_t kRenderPeriodUs = 1'000'000 / 60;
constexpr std::uint64_t kClockPeriodUs = 250'000;
constexpr std::size_t kFillerBytes = 32;
// A sequence this far past the first one seen is not tracked (it would cost
// memory for nothing: at 60 a second it is over 12 days away).
constexpr std::uint32_t kMaxTrackedSpan = 1u << 26;

net::ClientConfig client_config(const ProbeConfig& config) {
  net::ClientConfig c;
  c.host = config.host;
  c.port = config.port;
  c.hello.build_id = "coop_test 0.0.1";
  c.hello.game_build_id = "e0f3ccfaf50065ae";
  c.hello.game_version = "1.1.1";
  c.hello.nickname = config.nickname;
  return c;
}

// Moves `next` on by `period` for each one due by `now` and calls `fire` for
// each; when far behind (the process was paused), skips ahead instead of
// sending a burst that the server's rate limit would cut.
template <typename Fire>
void run_due(std::uint64_t now, std::uint64_t period, std::uint64_t& next, Fire fire) {
  if (now >= next + 3 * period) next = now;
  while (next <= now) {
    fire();
    next += period;
  }
}

void print_spread(std::FILE* out, const char* label, const Spread& s, const char* unit) {
  if (s.count == 0) {
    std::fprintf(out, "  %-14s no samples\n", label);
    return;
  }
  std::fprintf(out, "  %-14s min %.3f avg %.3f max %.3f %s (%llu samples)\n", label, s.min,
               s.avg(), s.max, unit, static_cast<unsigned long long>(s.count));
}

void print_counts(std::FILE* out, const char* label, const SequenceCounts& c,
                  std::uint64_t extra_lost) {
  std::fprintf(out, "  %-14s received %llu, lost %llu, out of order %llu, repeated %llu\n", label,
               static_cast<unsigned long long>(c.received),
               static_cast<unsigned long long>(c.lost + extra_lost),
               static_cast<unsigned long long>(c.out_of_order),
               static_cast<unsigned long long>(c.repeated));
}

}  // namespace

void Spread::add(double value) {
  min = std::min(min, value);
  max = std::max(max, value);
  sum += value;
  ++count;
}

Spread& Spread::operator+=(const Spread& other) {
  min = std::min(min, other.min);
  max = std::max(max, other.max);
  sum += other.sum;
  count += other.count;
  return *this;
}

SequenceCounts& SequenceCounts::operator+=(const SequenceCounts& other) {
  received += other.received;
  lost += other.lost;
  out_of_order += other.out_of_order;
  repeated += other.repeated;
  return *this;
}

ShownError& ShownError::operator+=(const ShownError& other) {
  units += other.units;
  interpolated += other.interpolated;
  extrapolated += other.extrapolated;
  held += other.held;
  hidden += other.hidden;
  return *this;
}

void SequenceTracker::add(std::uint32_t sequence) {
  if (!any_) {
    any_ = true;
    first_ = sequence;
    highest_ = sequence;
    seen_.assign(1, true);
    counts_.received = 1;
    return;
  }
  if (sequence < first_) {
    // Older than the first one seen: it was late, but there is no slot for it.
    ++counts_.out_of_order;
    return;
  }
  const std::uint32_t index = sequence - first_;
  if (index >= kMaxTrackedSpan) return;
  if (index >= seen_.size()) seen_.resize(static_cast<std::size_t>(index) + 1, false);
  if (seen_[index]) {
    ++counts_.repeated;
    return;
  }
  seen_[index] = true;
  ++counts_.received;
  if (sequence < highest_) {
    ++counts_.out_of_order;
  } else {
    highest_ = sequence;
  }
  counts_.lost = static_cast<std::uint64_t>(highest_ - first_) + 1 - counts_.received;
}

std::optional<std::uint32_t> SequenceTracker::highest() const {
  if (!any_) return std::nullopt;
  return highest_;
}

Probe::Probe(ProbeConfig config) : config_(std::move(config)), client_(client_config(config_)) {}

bool Probe::start() { return client_.start(); }

void Probe::stop() { client_.stop(); }

void Probe::step(bool sending) {
  client_.service(0);
  while (auto event = client_.next_event()) handle(*event);

  const std::uint64_t now = client_.now_us();
  if (client_.state() != net::State::connected) return;

  if (now >= next_clock_) {
    next_clock_ = now + kClockPeriodUs;
    if (auto rtt = client_.round_trip_us()) {
      round_trip_ms_.add(static_cast<double>(*rtt) / 1000.0);
    }
    if (auto offset = client_.clock_offset_us()) {
      last_offset_ms_ = static_cast<double>(*offset) / 1000.0;
      offset_ms_.add(*last_offset_ms_);
    }
  }
  // Samples are stamped in server time, so wait for the first clock estimate.
  if (!sending) return;
  if (client_.clock_offset_us()) send_due(now);
  render(now);
}

void Probe::handle(const net::Event& event) {
  switch (event.kind) {
    case net::EventKind::welcomed:
    case net::EventKind::reconnected:
      ++welcomes_;
      presence_.on_welcome(event.welcome);
      break;
    case net::EventKind::refused:
      refused_ = event.refuse_reason;
      break;
    case net::EventKind::connect_failed:
      ++failed_attempts_;
      break;
    case net::EventKind::disconnected:
      ++disconnects_;
      archive_all();
      presence_.clear();
      break;
    case net::EventKind::message:
      on_message(event.message, event.channel);
      break;
    case net::EventKind::chat:
    case net::EventKind::player_joined:
    case net::EventKind::player_left:
    case net::EventKind::player_status:
      break;  // the `message` event before each one is enough here
  }
}

void Probe::on_message(const proto::Message& message, std::uint8_t channel) {
  presence_.apply(message);
  auto heard = [this](std::uint8_t player) {
    if (std::find(heard_from_.begin(), heard_from_.end(), player) == heard_from_.end()) {
      heard_from_.push_back(player);
    }
  };
  if (const auto* left = std::get_if<proto::PlayerLeft>(&message)) {
    archive(left->player_number);
  } else if (const auto* sample = std::get_if<proto::PlayerSample>(&message)) {
    const std::uint8_t player = sample->player_number;
    heard(player);
    remote(player).samples.add(sample->time.sequence);
  } else if (const auto* test = std::get_if<proto::TestPayload>(&message)) {
    if (test->filler.empty() || test->filler[0] < 1 || test->filler[0] > proto::kMaxPlayers) {
      ++unattributed_;
      return;
    }
    const std::uint8_t player = test->filler[0];
    heard(player);
    Remote& r = remote(player);
    if (channel == proto::kChannelSession) {
      r.reliable.add(test->sequence);
    } else {
      r.unreliable.add(test->sequence);
    }
  }
}

Probe::Remote& Probe::remote(std::uint8_t player) { return remotes_[player]; }

void Probe::archive(std::uint8_t player) {
  auto it = remotes_.find(player);
  if (it == remotes_.end()) return;
  archived_reliable_ += it->second.reliable.counts();
  archived_unreliable_ += it->second.unreliable.counts();
  archived_samples_ += it->second.samples.counts();
  archived_shown_ += it->second.shown;
  remotes_.erase(it);
}

void Probe::archive_all() {
  while (!remotes_.empty()) archive(remotes_.begin()->first);
}

bool Probe::send(const proto::Message& message, std::uint8_t channel) {
  if (client_.send_on(message, channel)) return true;
  ++send_failures_;
  return false;
}

void Probe::send_due(std::uint64_t now) {
  if (!schedule_started_) {
    schedule_started_ = true;
    next_unreliable_ = now;
    next_reliable_ = now;
    next_sample_ = now;
  }
  const std::uint8_t me = client_.player_number();
  auto payload = [me](std::uint32_t sequence) {
    proto::TestPayload p;
    p.sequence = sequence;
    p.filler.assign(kFillerBytes, 0);
    p.filler[0] = me;
    return p;
  };
  run_due(now, kUnreliablePeriodUs, next_unreliable_, [&] {
    if (send(payload(unreliable_sequence_), proto::kChannelPlayerSamples)) {
      ++unreliable_sequence_;
    }
  });
  run_due(now, kReliablePeriodUs, next_reliable_, [&] {
    if (send(payload(reliable_sequence_), proto::kChannelSession)) {
      sent_reliable_ = true;
      ++reliable_sequence_;
    }
  });
  run_due(now, kSamplePeriodUs, next_sample_, [&] {
    const std::uint64_t server_now = client_.server_now_us();
    const PathPoint p = true_point(me, server_now);
    proto::PlayerSample s;
    s.time = {server_now, sample_sequence_};
    s.position = proto::Vec3{static_cast<float>(p.position[0]),
                             static_cast<float>(p.position[1]),
                             static_cast<float>(p.position[2])};
    s.velocity = proto::Vec3{static_cast<float>(p.velocity[0]),
                             static_cast<float>(p.velocity[1]),
                             static_cast<float>(p.velocity[2])};
    if (send(s, proto::kChannelPlayerSamples)) ++sample_sequence_;
  });
}

void Probe::render(std::uint64_t now) {
  if (next_render_ == 0) next_render_ = now;
  if (now < next_render_) return;
  next_render_ += kRenderPeriodUs;
  if (next_render_ <= now) next_render_ = now + kRenderPeriodUs;

  const std::uint64_t server_now = client_.server_now_us();
  if (server_now < presence_.settings().show_delay_us) return;
  for (const std::uint8_t player : presence_.players()) {
    const auto pose = presence_.shown(player, server_now);
    if (!pose) continue;
    const PathPoint truth = true_point(player, pose->at_us);
    const Vec shown_at{pose->position[0], pose->position[1], pose->position[2]};
    ShownError& e = remote(player).shown;
    e.units.add(distance(shown_at, truth.position));
    switch (pose->how) {
      case presence::How::interpolated: ++e.interpolated; break;
      case presence::How::extrapolated: ++e.extrapolated; break;
      case presence::How::held:
        ++e.held;
        if (pose->fade <= 0.0f) ++e.hidden;
        break;
    }
  }
}

std::optional<std::uint32_t> Probe::last_reliable_sent() const {
  if (!sent_reliable_) return std::nullopt;
  return reliable_sequence_ - 1;
}

std::optional<std::uint32_t> Probe::last_reliable_from(std::uint8_t player) const {
  auto it = remotes_.find(player);
  if (it == remotes_.end()) return std::nullopt;
  return it->second.reliable.highest();
}

SequenceCounts Probe::reliable() const {
  SequenceCounts c = archived_reliable_;
  for (const auto& [player, r] : remotes_) c += r.reliable.counts();
  return c;
}

SequenceCounts Probe::unreliable() const {
  SequenceCounts c = archived_unreliable_;
  for (const auto& [player, r] : remotes_) c += r.unreliable.counts();
  return c;
}

SequenceCounts Probe::samples() const {
  SequenceCounts c = archived_samples_;
  for (const auto& [player, r] : remotes_) c += r.samples.counts();
  return c;
}

ShownError Probe::shown() const {
  ShownError e = archived_shown_;
  for (const auto& [player, r] : remotes_) e += r.shown;
  return e;
}

void Probe::print(std::FILE* out, std::uint64_t extra_reliable_lost) const {
  const std::string_view state = net::to_string(client_.state());
  std::fprintf(out,
               "client \"%s\": %.*s, player %u, welcomed %u, dropped %u, failed attempts %u, "
               "rejected %llu, send failures %llu\n",
               config_.nickname.c_str(), static_cast<int>(state.size()), state.data(),
               static_cast<unsigned>(client_.player_number()), welcomes_, disconnects_,
               failed_attempts_, static_cast<unsigned long long>(client_.rejected_count()),
               static_cast<unsigned long long>(send_failures_));
  if (refused_) {
    const std::string_view why = net::refuse_reason_text(*refused_);
    std::fprintf(out, "  refused: %.*s\n", static_cast<int>(why.size()), why.data());
  }
  print_spread(out, "round trip", round_trip_ms_, "ms");
  if (last_offset_ms_) {
    std::fprintf(out, "  %-14s now %+.3f ms, min %+.3f max %+.3f (spread %.3f ms)\n",
                 "clock offset", *last_offset_ms_, offset_ms_.min, offset_ms_.max,
                 offset_ms_.max - offset_ms_.min);
  } else {
    std::fprintf(out, "  %-14s no estimate\n", "clock offset");
  }
  print_counts(out, "reliable", reliable(), extra_reliable_lost);
  print_counts(out, "unreliable", unreliable(), 0);
  print_counts(out, "samples", samples(), 0);
  const ShownError e = shown();
  if (e.units.count == 0) {
    std::fprintf(out, "  %-14s no frames\n", "shown error");
  } else {
    std::fprintf(out,
                 "  %-14s min %.3f avg %.3f max %.3f units over %llu frames "
                 "(interpolated %llu, extrapolated %llu, held %llu, hidden %llu), "
                 "shown %llu ms behind\n",
                 "shown error", e.units.min, e.units.avg(), e.units.max,
                 static_cast<unsigned long long>(e.units.count),
                 static_cast<unsigned long long>(e.interpolated),
                 static_cast<unsigned long long>(e.extrapolated),
                 static_cast<unsigned long long>(e.held),
                 static_cast<unsigned long long>(e.hidden),
                 static_cast<unsigned long long>(presence_.settings().show_delay_us / 1000));
  }
  if (unattributed_ != 0) {
    std::fprintf(out, "  %-14s %llu test payloads with no sender byte\n", "unattributed",
                 static_cast<unsigned long long>(unattributed_));
  }
}

}  // namespace coop::test_tool
