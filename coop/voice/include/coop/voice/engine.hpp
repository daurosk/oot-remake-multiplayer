// SPDX-License-Identifier: GPL-3.0-or-later
//
// Proximity voice chat, game-free (docs/voice.md). The game side hands it:
// - our microphone, 48 kHz mono 16-bit, in any chunk size (capture), and gets
//   back the VOICE_FRAME messages to send;
// - the VOICE_FRAME messages relayed to us (receive, the network thread);
// - where the camera and each other player's Link are (set_listener,
//   set_speaker, the game thread);
// and the audio mixer asks it for the voices, already placed, added into the
// game's own mix buffers (render, the audio thread, 5 ms blocks).
//
// The codec is the caller's (Opus in the game; a stand-in in the tests), made
// through CodecFactory: one encoder, and one decoder per speaker.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop/voice/gate.hpp"
#include "coop/voice/jitter.hpp"
#include "coop/voice/spatial.hpp"

namespace coop::voice {

// The players a game may hear: player numbers 1 to kMaxSpeakers.
inline constexpr std::size_t kMaxSpeakers = 16;

class Encoder {
 public:
  virtual ~Encoder() = default;
  // One 20 ms frame (kSamplesPerFrame samples) into at most out.size()
  // bytes; the number of bytes, or 0 on failure.
  virtual std::size_t encode(std::span<const std::int16_t> pcm, std::span<std::uint8_t> out) = 0;
};

class Decoder {
 public:
  virtual ~Decoder() = default;
  // One 20 ms frame into pcm (kSamplesPerFrame samples): from `packet`, from
  // the FEC of `packet` (the next frame's) when fec, or concealed when
  // packet is empty. False on failure (pcm is then silence).
  virtual bool decode(std::span<const std::uint8_t> packet, bool fec, std::span<std::int16_t> pcm) = 0;
};

struct CodecFactory {
  std::function<std::unique_ptr<Encoder>()> encoder;
  std::function<std::unique_ptr<Decoder>()> decoder;
};

struct Settings {
  GateConfig gate;
  bool muted = false;           // our microphone
  bool deafened = false;        // the others' voices
  float volume = 1.0f;          // the others' voices, 0 to 2
  SpatialConfig spatial;        // range_m is the hearing range
  JitterConfig jitter;
  std::array<float, kMaxSpeakers + 1> player_volume{};  // by player number; 0 = default 1
};

class Engine {
 public:
  explicit Engine(CodecFactory codecs, Settings settings = {});
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void configure(const Settings& settings);
  Settings settings() const;

  // --- capture (one thread) ---
  // Our microphone's next samples. Every complete 20 ms frame goes through
  // the gate and, when open, the encoder; the messages to send are appended
  // to `out` (each with player 0, as a game sends them).
  void capture(std::span<const std::int16_t> samples, bool key_held,
               std::vector<protocol::VoiceFrame>& out);
  // Ends our talk spurt now (the microphone goes away): the end frame when
  // we were talking, else nothing. Pending samples are dropped.
  void stop_talking(std::vector<protocol::VoiceFrame>& out);
  // True while our gate is open (we are talking).
  bool talking() const;
  float mic_level_db() const;

  // --- network thread ---
  void receive(const protocol::VoiceFrame& frame, std::uint64_t now_us);
  // A player left: its voice stops and its state goes.
  void forget(std::uint8_t player);

  // --- game thread ---
  void set_listener(const Listener& listener);
  // Where a player's Link is; empty when not in our scene or not known (its
  // voice is then not heard, whatever comes).
  void set_speaker(std::uint8_t player, std::optional<Vec3> position);

  // --- audio thread ---
  // Adds `frames` samples (48 kHz) of every voice into `out`: `channels` 2
  // (left, right) or 6 (front left, front right, centre, LFE, rear left,
  // rear right), each out[c] pointing at `frames` floats, full scale 1.0.
  void render(float* const* out, int channels, int frames, std::uint64_t now_us);

  // --- any thread ---
  // True while a player's voice plays here (it is talking and in range):
  // a frame of it was heard within the last 250 ms of render's clock.
  bool speaking(std::uint8_t player, std::uint64_t now_us) const;

  struct Stats {
    std::uint64_t sent = 0;
    std::uint64_t received = 0;
    std::uint64_t decoded = 0;
    std::uint64_t concealed = 0;
    std::uint64_t encode_failed = 0;
    std::uint64_t decode_failed = 0;
    std::uint64_t unplaced = 0;  // frames from a player whose place is not known
  };
  Stats stats() const;

 private:
  struct Speaker;
  Speaker* speaker(std::uint8_t player);
  // The speaker's next 20 ms decoded into its frame buffer; false while it
  // is quiet (nothing decoded).
  bool fill(Speaker& s, std::uint64_t now_us);

  CodecFactory codecs_;
  mutable std::mutex settings_mutex_;
  Settings settings_;
  // Capture.
  std::unique_ptr<Encoder> encoder_;
  Gate gate_;
  std::vector<std::int16_t> pending_;
  std::uint16_t sequence_ = 0;
  std::atomic_bool talking_{false};
  std::atomic<float> level_db_{-96.0f};
  // Listener.
  mutable std::mutex listener_mutex_;
  Listener listener_;
  // Speakers, by player number.
  std::array<std::unique_ptr<Speaker>, kMaxSpeakers + 1> speakers_;
  mutable std::mutex stats_mutex_;
  Stats stats_;
};

}  // namespace coop::voice
