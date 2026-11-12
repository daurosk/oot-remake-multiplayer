// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop/voice/engine.hpp"

#include <algorithm>
#include <cmath>

namespace coop::voice {

namespace {

constexpr std::uint64_t kSpeakingUs = 250'000;

}  // namespace

struct Engine::Speaker {
  std::mutex mutex;
  JitterBuffer jitter;
  std::unique_ptr<Decoder> decoder;
  std::array<float, kSamplesPerFrame> pcm{};
  int pcm_at = kSamplesPerFrame;  // consumed up to here; kSamplesPerFrame = empty
  bool pcm_voice = false;         // the frame in pcm is speech (not concealment)
  std::optional<Vec3> position;
  Placement placed;               // the gains the last block ended on
  float lowpass_state = 0;
  std::atomic<std::uint64_t> heard_us{0};  // render's clock when a frame of it was last heard
};

Engine::Engine(CodecFactory codecs, Settings settings)
    : codecs_(std::move(codecs)), settings_(settings), gate_(settings.gate) {
  if (codecs_.encoder) encoder_ = codecs_.encoder();
  for (std::size_t i = 1; i <= kMaxSpeakers; ++i) {
    speakers_[i] = std::make_unique<Speaker>();
    speakers_[i]->jitter = JitterBuffer(settings.jitter);
  }
}

Engine::~Engine() = default;

void Engine::configure(const Settings& settings) {
  std::lock_guard lock(settings_mutex_);
  settings_ = settings;
}

Settings Engine::settings() const {
  std::lock_guard lock(settings_mutex_);
  return settings_;
}

Engine::Speaker* Engine::speaker(std::uint8_t player) {
  if (player == 0 || player > kMaxSpeakers) return nullptr;
  return speakers_[player].get();
}

void Engine::capture(std::span<const std::int16_t> samples, bool key_held,
                     std::vector<protocol::VoiceFrame>& out) {
  const Settings s = settings();
  gate_.configure(s.gate);
  pending_.insert(pending_.end(), samples.begin(), samples.end());
  std::size_t at = 0;
  std::array<std::uint8_t, protocol::kMaxVoiceBytes> packet{};
  while (pending_.size() - at >= static_cast<std::size_t>(kSamplesPerFrame)) {
    const std::span<const std::int16_t> frame(pending_.data() + at, kSamplesPerFrame);
    at += kSamplesPerFrame;
    const Gate::Step step = gate_.update(frame, key_held, s.muted || encoder_ == nullptr);
    level_db_.store(gate_.level_db());
    if (step == Gate::Step::closed) continue;
    protocol::VoiceFrame v;
    v.player_number = protocol::kPlayerSelf;
    if (step == Gate::Step::ended) {
      v.flags = protocol::voice::end;
    } else {
      const std::size_t n = encoder_->encode(frame, packet);
      if (n == 0 || n > packet.size()) {
        std::lock_guard lock(stats_mutex_);
        ++stats_.encode_failed;
        continue;
      }
      v.opus.assign(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(n));
    }
    v.sequence = sequence_++;
    out.push_back(std::move(v));
    std::lock_guard lock(stats_mutex_);
    ++stats_.sent;
  }
  pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(at));
  talking_.store(gate_.open());
}

void Engine::stop_talking(std::vector<protocol::VoiceFrame>& out) {
  pending_.clear();
  if (!gate_.open()) return;
  const std::array<std::int16_t, kSamplesPerFrame> silence{};
  if (gate_.update(silence, false, true) == Gate::Step::ended) {  // muted: closes at once
    protocol::VoiceFrame v;
    v.flags = protocol::voice::end;
    v.sequence = sequence_++;
    out.push_back(std::move(v));
    std::lock_guard lock(stats_mutex_);
    ++stats_.sent;
  }
  talking_.store(false);
}

bool Engine::talking() const { return talking_.load(); }
float Engine::mic_level_db() const { return level_db_.load(); }

void Engine::receive(const protocol::VoiceFrame& frame, std::uint64_t now_us) {
  Speaker* s = speaker(frame.player_number);
  if (s == nullptr) return;
  if (settings().deafened) return;
  {
    std::lock_guard lock(stats_mutex_);
    ++stats_.received;
  }
  std::lock_guard lock(s->mutex);
  if (!s->position) {
    std::lock_guard stats(stats_mutex_);
    ++stats_.unplaced;
    return;
  }
  if (!s->decoder && codecs_.decoder) s->decoder = codecs_.decoder();
  s->jitter.push(frame.sequence, (frame.flags & protocol::voice::end) != 0, frame.opus, now_us);
}

void Engine::forget(std::uint8_t player) {
  Speaker* s = speaker(player);
  if (s == nullptr) return;
  std::lock_guard lock(s->mutex);
  s->jitter.reset();
  s->decoder.reset();
  s->pcm_at = kSamplesPerFrame;
  s->position.reset();
  s->placed = {};
  s->lowpass_state = 0;
  s->heard_us.store(0);
}

void Engine::set_listener(const Listener& listener) {
  std::lock_guard lock(listener_mutex_);
  listener_ = listener;
}

void Engine::set_speaker(std::uint8_t player, std::optional<Vec3> position) {
  Speaker* s = speaker(player);
  if (s == nullptr) return;
  std::lock_guard lock(s->mutex);
  s->position = position;
}

bool Engine::fill(Speaker& s, std::uint64_t now_us) {
  const JitterBuffer::Out next = s.jitter.pop(now_us);
  if (next.kind == JitterBuffer::Kind::quiet) return false;
  std::array<std::int16_t, kSamplesPerFrame> pcm{};
  bool ok = s.decoder != nullptr;
  if (ok) {
    switch (next.kind) {
      case JitterBuffer::Kind::frame:
        ok = s.decoder->decode(next.payload, false, pcm);
        break;
      case JitterBuffer::Kind::fec:
        ok = s.decoder->decode(next.payload, true, pcm);
        break;
      default:
        ok = s.decoder->decode({}, false, pcm);
        break;
    }
  }
  if (!ok) pcm.fill(0);
  for (int i = 0; i < kSamplesPerFrame; ++i) s.pcm[static_cast<std::size_t>(i)] = pcm[static_cast<std::size_t>(i)] / 32768.0f;
  s.pcm_at = 0;
  s.pcm_voice = next.kind == JitterBuffer::Kind::frame && ok;
  std::lock_guard lock(stats_mutex_);
  if (!ok) ++stats_.decode_failed;
  if (next.kind == JitterBuffer::Kind::frame) {
    ++stats_.decoded;
  } else {
    ++stats_.concealed;
  }
  return true;
}

void Engine::render(float* const* out, int channels, int frames, std::uint64_t now_us) {
  if (out == nullptr || frames <= 0 || (channels != 2 && channels != 6)) return;
  const Settings set = settings();
  Listener listener;
  {
    std::lock_guard lock(listener_mutex_);
    listener = listener_;
  }
  thread_local std::vector<float> mono;  // the mixer's thread: no allocation once grown
  mono.resize(static_cast<std::size_t>(frames));
  for (std::size_t p = 1; p <= kMaxSpeakers; ++p) {
    Speaker& s = *speakers_[p];
    std::lock_guard lock(s.mutex);
    if (s.pcm_at >= kSamplesPerFrame && s.jitter.queued() == 0 && !s.jitter.playing()) continue;
    // This block's samples, one 20 ms frame decoded whenever the last is used.
    int made = 0;
    bool voice = false;
    while (made < frames) {
      if (s.pcm_at >= kSamplesPerFrame && !fill(s, now_us)) break;
      const int take = std::min(frames - made, kSamplesPerFrame - s.pcm_at);
      std::copy_n(s.pcm.begin() + s.pcm_at, take, mono.begin() + made);
      voice = voice || s.pcm_voice;
      s.pcm_at += take;
      made += take;
    }
    if (made == 0) continue;
    std::fill(mono.begin() + made, mono.end(), 0.0f);
    // Where it sounds from; nothing when its place is not known or we are deaf.
    Placement now{};
    if (s.position && !set.deafened) now = place(listener, *s.position, set.spatial);
    const float pv = set.player_volume[p] > 0.0f ? set.player_volume[p] : 1.0f;
    const float volume = std::clamp(set.volume, 0.0f, 2.0f) * std::clamp(pv, 0.0f, 2.0f);
    if (voice && now.audible && volume > 0.0f) s.heard_us.store(now_us == 0 ? 1 : now_us);
    const Placement from = s.placed;
    s.placed = now;
    if (!now.audible && !from.audible) continue;
    // The gains glide from the last block's to this one's (no clicks).
    const float a = now.lowpass;
    for (int i = 0; i < frames; ++i) {
      const float t = static_cast<float>(i + 1) / static_cast<float>(frames);
      s.lowpass_state += a * (mono[static_cast<std::size_t>(i)] - s.lowpass_state);
      const float x = s.lowpass_state * volume;
      auto g = [t](float f0, float f1) { return f0 + (f1 - f0) * t; };
      if (channels == 2) {
        out[0][i] += x * g(from.left, now.left);
        out[1][i] += x * g(from.right, now.right);
      } else {
        out[0][i] += x * g(from.front_left, now.front_left);
        out[1][i] += x * g(from.front_right, now.front_right);
        out[4][i] += x * g(from.rear_left, now.rear_left);
        out[5][i] += x * g(from.rear_right, now.rear_right);
      }
    }
  }
}

bool Engine::speaking(std::uint8_t player, std::uint64_t now_us) const {
  if (player == 0 || player > kMaxSpeakers) return false;
  const std::uint64_t heard = speakers_[player]->heard_us.load();
  return heard != 0 && now_us >= heard && now_us - heard < kSpeakingUs;
}

Engine::Stats Engine::stats() const {
  std::lock_guard lock(stats_mutex_);
  return stats_;
}

}  // namespace coop::voice
