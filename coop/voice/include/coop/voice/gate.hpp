// SPDX-License-Identifier: GPL-3.0-or-later
//
// When our microphone is sent (docs/voice.md, "Talking"): push to talk (only
// while the key is held, plus a short tail so the last word is not cut),
// open mic (while the level is above the noise, plus a hangover), or off.
// Mute overrides both. One call per 20 ms frame.
#pragma once

#include <cstdint>
#include <span>

namespace coop::voice {

enum class Mode : std::uint8_t { off = 0, push_to_talk = 1, open_mic = 2 };

struct GateConfig {
  Mode mode = Mode::push_to_talk;
  // Open mic: a frame is speech when its level is this far above the noise
  // floor and above threshold_db (dBFS).
  float threshold_db = -50.0f;
  float above_floor_db = 12.0f;
  int hangover_frames = 15;  // 300 ms of open after the last speech
  int tail_frames = 5;       // push to talk: 100 ms after the key is let go
};

class Gate {
 public:
  explicit Gate(GateConfig config = {});
  void configure(const GateConfig& config);

  enum class Step {
    closed,  // send nothing
    open,    // send this frame
    ended,   // the gate just closed: send the end of the spurt
  };
  Step update(std::span<const std::int16_t> frame, bool key_held, bool muted);

  bool open() const { return open_; }
  float level_db() const { return level_db_; }
  float floor_db() const { return floor_db_; }

 private:
  GateConfig config_;
  bool open_ = false;
  int hold_ = 0;
  float level_db_ = -96.0f;
  float floor_db_ = -70.0f;
};

// The frame's RMS level in dBFS, -96 for silence.
float level_db(std::span<const std::int16_t> frame);

}  // namespace coop::voice
