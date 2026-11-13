// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop/voice/gate.hpp"

#include <algorithm>
#include <cmath>

namespace coop::voice {

float level_db(std::span<const std::int16_t> frame) {
  if (frame.empty()) return -96.0f;
  double sum = 0;
  for (std::int16_t s : frame) sum += static_cast<double>(s) * static_cast<double>(s);
  const double rms = std::sqrt(sum / static_cast<double>(frame.size())) / 32768.0;
  if (rms <= 0.0) return -96.0f;
  return std::max(-96.0f, static_cast<float>(20.0 * std::log10(rms)));
}

Gate::Gate(GateConfig config) : config_(config) {}

void Gate::configure(const GateConfig& config) { config_ = config; }

Gate::Step Gate::update(std::span<const std::int16_t> frame, bool key_held, bool muted) {
  level_db_ = coop::voice::level_db(frame);
  // The noise floor: falls at once to a quieter frame, rises slowly (about
  // 3 dB a second), so speech never lifts it much.
  if (level_db_ < floor_db_) {
    floor_db_ = level_db_;
  } else {
    floor_db_ = std::min(floor_db_ + 0.06f, level_db_);
  }
  bool want = false;
  switch (config_.mode) {
    case Mode::off:
      break;
    case Mode::push_to_talk:
      if (key_held) {
        hold_ = config_.tail_frames;
        want = true;
      } else if (open_ && hold_ > 0) {
        --hold_;
        want = true;
      }
      break;
    case Mode::open_mic: {
      const bool speech = level_db_ > config_.threshold_db &&
                          level_db_ > floor_db_ + config_.above_floor_db;
      if (speech) {
        hold_ = config_.hangover_frames;
        want = true;
      } else if (open_ && hold_ > 0) {
        --hold_;
        want = true;
      }
      break;
    }
  }
  if (muted) want = false;
  if (want) {
    open_ = true;
    return Step::open;
  }
  if (open_) {
    open_ = false;
    hold_ = 0;
    return Step::ended;
  }
  return Step::closed;
}

}  // namespace coop::voice
