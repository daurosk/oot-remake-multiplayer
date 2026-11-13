// SPDX-License-Identifier: GPL-3.0-or-later
//
// One speaker's jitter buffer (docs/voice.md, "Receiving"). VOICE_FRAME
// messages come in on the network thread, out of order, late or not at all;
// the mixer takes one 20 ms frame at a time at the audio device's pace. The
// buffer holds frames by their sequence number and says for each 20 ms what
// to decode: the frame, the next one's forward error correction, a loss to
// conceal, or nothing (the speaker is quiet).
//
// A talk spurt starts playing once `target` frames wait, or once the first
// one has waited that long; the target follows the measured arrival jitter
// (RFC 3550's estimate) between min_target and max_target and is taken at the
// start of each spurt, so playback never jumps inside one. A frame with the
// end flag, or max_lost concealed frames in a row, ends the spurt. Not thread
// safe: the caller locks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace coop::voice {

inline constexpr std::uint64_t kFrameUs = 20'000;  // one Opus frame, 960 samples at 48 kHz
inline constexpr int kSamplesPerFrame = 960;
inline constexpr int kSampleRate = 48'000;

struct JitterConfig {
  int min_target = 2;    // frames waiting before a spurt plays (40 ms)
  int max_target = 10;   // (200 ms)
  int max_lost = 20;     // concealed frames in a row that end a spurt (400 ms)
  int max_queued = 25;   // more than this waiting: the oldest go (a stall)
};

class JitterBuffer {
 public:
  enum class Kind {
    quiet,  // nothing to play
    frame,  // decode `payload`
    fec,    // the frame is missing; decode `payload` (the next frame) with FEC
    lost,   // the frame is missing; conceal
  };
  struct Out {
    Kind kind = Kind::quiet;
    std::vector<std::uint8_t> payload;
  };
  struct Stats {
    std::uint64_t pushed = 0;
    std::uint64_t played = 0;
    std::uint64_t late = 0;       // came after its turn: dropped
    std::uint64_t repeated = 0;   // a sequence already held
    std::uint64_t concealed = 0;  // lost or fec
    std::uint64_t overflow = 0;   // dropped to catch up
    std::uint64_t spurts = 0;
  };

  explicit JitterBuffer(JitterConfig config = {});

  // A frame from the network at `now_us` (any monotonic clock). An end frame
  // has no payload.
  void push(std::uint16_t sequence, bool end, std::span<const std::uint8_t> payload,
            std::uint64_t now_us);
  // The next 20 ms, at `now_us` on the same clock.
  Out pop(std::uint64_t now_us);

  bool playing() const { return playing_; }
  int target() const { return target_; }
  std::size_t queued() const { return frames_.size(); }
  // The arrival jitter estimate, microseconds.
  double jitter_us() const { return jitter_us_; }
  const Stats& stats() const { return stats_; }
  void reset();

 private:
  struct Held {
    bool end = false;
    std::vector<std::uint8_t> payload;
  };
  void update_jitter(std::uint16_t sequence, bool end, std::uint64_t now_us);
  int wanted_target() const;
  void stop();

  JitterConfig config_;
  // By sequence. The map's order breaks at the wrap, so the oldest is found
  // with sequence_distance, never from begin().
  std::map<std::uint16_t, Held> frames_;
  bool playing_ = false;
  std::uint16_t next_ = 0;   // the sequence to play next while playing
  bool have_next_ = false;   // next_ also holds after a spurt: older ones are late
  int lost_run_ = 0;
  int target_ = 2;
  std::uint64_t first_wait_us_ = 0;  // when the first frame of a waiting spurt came
  // Jitter estimate.
  bool have_last_ = false;
  std::uint16_t last_sequence_ = 0;
  std::uint64_t last_arrival_us_ = 0;
  bool last_end_ = false;  // the newest was an end: the gap to the next spurt is no jitter
  double jitter_us_ = 0;
  Stats stats_;
};

// Sequence a relative to b, wrapping: negative when a is older.
inline int sequence_distance(std::uint16_t a, std::uint16_t b) {
  return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b));
}

}  // namespace coop::voice
