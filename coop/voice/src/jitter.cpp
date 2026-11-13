// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop/voice/jitter.hpp"

#include <algorithm>
#include <cmath>

namespace coop::voice {

JitterBuffer::JitterBuffer(JitterConfig config) : config_(config), target_(config.min_target) {}

void JitterBuffer::reset() {
  frames_.clear();
  playing_ = false;
  have_next_ = false;
  lost_run_ = 0;
  target_ = config_.min_target;
  first_wait_us_ = 0;
  have_last_ = false;
  last_end_ = false;
  jitter_us_ = 0;
}

void JitterBuffer::update_jitter(std::uint16_t sequence, bool end, std::uint64_t now_us) {
  if (!have_last_ || (last_end_ && sequence_distance(sequence, last_sequence_) > 0)) {
    have_last_ = true;
    last_sequence_ = sequence;
    last_arrival_us_ = now_us;
    last_end_ = end;
    return;
  }
  const int steps = sequence_distance(sequence, last_sequence_);
  if (steps <= 0) return;  // older or the same: the estimate follows the newest
  if (steps < 50) {
    // RFC 3550 6.4.1: the change in transit time between two packets.
    const double gap = static_cast<double>(now_us) - static_cast<double>(last_arrival_us_);
    const double d = gap - static_cast<double>(steps) * static_cast<double>(kFrameUs);
    // One outlier (a hitch) counts as at most 200 ms.
    jitter_us_ += (std::min(std::abs(d), 200'000.0) - jitter_us_) / 16.0;
  }
  last_sequence_ = sequence;
  last_arrival_us_ = now_us;
  last_end_ = end;
}

int JitterBuffer::wanted_target() const {
  const int frames =
      1 + static_cast<int>(std::ceil(3.0 * jitter_us_ / static_cast<double>(kFrameUs)));
  return std::clamp(frames, config_.min_target, config_.max_target);
}

void JitterBuffer::push(std::uint16_t sequence, bool end, std::span<const std::uint8_t> payload,
                        std::uint64_t now_us) {
  ++stats_.pushed;
  if (have_next_) {
    const int d = sequence_distance(sequence, next_);
    if (d < 0) {
      // Far older while quiet: the speaker's game started counting again
      // (a rejoin). Otherwise a straggler of what already played.
      if (playing_ || d > -50) {
        ++stats_.late;
        return;
      }
      frames_.clear();
      have_next_ = false;
    } else if (d > 1000) {
      // Far ahead: the stream went on without us (a long stall). Start over.
      frames_.clear();
      playing_ = false;
      have_next_ = false;
    }
  }
  update_jitter(sequence, end, now_us);
  if (frames_.contains(sequence)) {
    ++stats_.repeated;
    return;
  }
  if (end && !playing_ && frames_.empty()) {
    // The end of a spurt whose frames all came late or never: nothing to end.
    if (!have_next_ || sequence_distance(sequence, next_) >= 0) {
      next_ = static_cast<std::uint16_t>(sequence + 1);
      have_next_ = true;
    }
    return;
  }
  if (!playing_ && frames_.empty()) {
    first_wait_us_ = now_us;
    target_ = wanted_target();
  }
  Held held;
  held.end = end;
  held.payload.assign(payload.begin(), payload.end());
  frames_.emplace(sequence, std::move(held));
  // A stall: drop the oldest until it fits.
  while (frames_.size() > static_cast<std::size_t>(config_.max_queued)) {
    const std::uint16_t ref = have_next_ ? next_ : sequence;
    auto oldest = frames_.begin();
    for (auto it = frames_.begin(); it != frames_.end(); ++it) {
      if (sequence_distance(it->first, ref) < sequence_distance(oldest->first, ref)) oldest = it;
    }
    if (playing_) next_ = static_cast<std::uint16_t>(oldest->first + 1);
    frames_.erase(oldest);
    ++stats_.overflow;
  }
}

void JitterBuffer::stop() {
  playing_ = false;
  lost_run_ = 0;
}

JitterBuffer::Out JitterBuffer::pop(std::uint64_t now_us) {
  Out out;
  if (!playing_) {
    if (frames_.empty()) return out;
    // The oldest waiting frame, relative to any one of them (they are close).
    const std::uint16_t ref = frames_.begin()->first;
    auto oldest = frames_.begin();
    for (auto it = frames_.begin(); it != frames_.end(); ++it) {
      if (sequence_distance(it->first, ref) < sequence_distance(oldest->first, ref)) oldest = it;
    }
    if (oldest->second.end) {  // a spurt that is only its end
      next_ = static_cast<std::uint16_t>(oldest->first + 1);
      have_next_ = true;
      frames_.erase(oldest);
      return out;
    }
    const bool enough = frames_.size() >= static_cast<std::size_t>(target_);
    const bool waited = now_us >= first_wait_us_ &&
                        now_us - first_wait_us_ >= static_cast<std::uint64_t>(target_) * kFrameUs;
    if (!enough && !waited) return out;
    playing_ = true;
    have_next_ = true;
    next_ = oldest->first;
    lost_run_ = 0;
    ++stats_.spurts;
  }
  // Clock drift or a burst: the buffer grew well past the target. Skip one.
  if (frames_.size() > static_cast<std::size_t>(target_ + 4)) {
    auto it = frames_.find(next_);
    if (it != frames_.end() && !it->second.end) {
      frames_.erase(it);
      next_ = static_cast<std::uint16_t>(next_ + 1);
      ++stats_.overflow;
    }
  }
  auto it = frames_.find(next_);
  if (it != frames_.end()) {
    Held held = std::move(it->second);
    frames_.erase(it);
    next_ = static_cast<std::uint16_t>(next_ + 1);
    if (held.end) {
      stop();
      if (!frames_.empty()) first_wait_us_ = now_us;
      return out;
    }
    lost_run_ = 0;
    ++stats_.played;
    out.kind = Kind::frame;
    out.payload = std::move(held.payload);
    return out;
  }
  // Missing: the next frame's FEC when it is here, else concealment.
  ++stats_.concealed;
  ++lost_run_;
  auto after = frames_.find(static_cast<std::uint16_t>(next_ + 1));
  if (after != frames_.end() && !after->second.end) {
    out.kind = Kind::fec;
    out.payload = after->second.payload;
  } else {
    out.kind = Kind::lost;
  }
  next_ = static_cast<std::uint16_t>(next_ + 1);
  if (lost_run_ >= config_.max_lost) {
    stop();
    if (!frames_.empty()) first_wait_us_ = now_us;
    out = Out{};
  }
  return out;
}

}  // namespace coop::voice
