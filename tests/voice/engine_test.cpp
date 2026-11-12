#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "coop/voice/engine.hpp"

using namespace coop::voice;
namespace proto = coop::protocol;

namespace {

// A stand-in codec: a frame is sent as its first sample (2 bytes) and comes
// back as a frame of that sample; concealment is silence.
struct FakeEncoder : Encoder {
  std::size_t encode(std::span<const std::int16_t> pcm, std::span<std::uint8_t> out) override {
    if (pcm.size() != static_cast<std::size_t>(kSamplesPerFrame) || out.size() < 2) return 0;
    const auto v = static_cast<std::uint16_t>(pcm[0]);
    out[0] = static_cast<std::uint8_t>(v & 0xff);
    out[1] = static_cast<std::uint8_t>(v >> 8);
    return 2;
  }
};

struct FakeDecoder : Decoder {
  int* fec_count = nullptr;
  bool decode(std::span<const std::uint8_t> packet, bool fec, std::span<std::int16_t> pcm) override {
    if (fec && fec_count != nullptr) ++*fec_count;
    std::int16_t v = 0;
    if (packet.size() == 2) v = static_cast<std::int16_t>(packet[0] | (packet[1] << 8));
    std::fill(pcm.begin(), pcm.end(), v);
    return true;
  }
};

CodecFactory fake_codecs(int* fec_count = nullptr) {
  CodecFactory f;
  f.encoder = [] { return std::make_unique<FakeEncoder>(); };
  f.decoder = [fec_count] {
    auto d = std::make_unique<FakeDecoder>();
    d->fec_count = fec_count;
    return d;
  };
  return f;
}

proto::VoiceFrame frame_from(std::uint8_t player, std::uint16_t sequence, std::int16_t value) {
  proto::VoiceFrame v;
  v.player_number = player;
  v.sequence = sequence;
  const auto u = static_cast<std::uint16_t>(value);
  v.opus = {static_cast<std::uint8_t>(u & 0xff), static_cast<std::uint8_t>(u >> 8)};
  return v;
}

struct Out {
  explicit Out(int channels) : buffers(static_cast<std::size_t>(channels), std::vector<float>(240, 0.0f)) {
    for (auto& b : buffers) pointers.push_back(b.data());
  }
  std::vector<std::vector<float>> buffers;
  std::vector<float*> pointers;
  float peak(int channel) const {
    float p = 0;
    for (float v : buffers[static_cast<std::size_t>(channel)]) p = std::max(p, std::abs(v));
    return p;
  }
};

// Renders `blocks` 5 ms blocks from `start_us`; returns the last block.
Out render(Engine& e, int blocks, std::uint64_t start_us, int channels = 2) {
  Out last(channels);
  for (int b = 0; b < blocks; ++b) {
    Out o(channels);
    e.render(o.pointers.data(), channels, 240, start_us + static_cast<std::uint64_t>(b) * 5'000);
    last = o;
    for (std::size_t c = 0; c < last.buffers.size(); ++c) last.pointers[c] = last.buffers[c].data();
  }
  return last;
}

}  // namespace

TEST_CASE("voice engine: push to talk makes one frame per 20 ms, numbered, then the end") {
  Settings s;
  s.gate.mode = Mode::push_to_talk;
  s.gate.tail_frames = 0;
  Engine e(fake_codecs(), s);
  std::vector<proto::VoiceFrame> out;
  std::vector<std::int16_t> mic(1000, 1234);
  e.capture(mic, true, out);
  REQUIRE(out.size() == 1);  // 40 samples wait for the next frame
  CHECK(out[0].player_number == proto::kPlayerSelf);
  CHECK(out[0].sequence == 0);
  CHECK(out[0].opus.size() == 2);
  CHECK(e.talking());
  e.capture(std::vector<std::int16_t>(920, 1234), true, out);
  REQUIRE(out.size() == 2);
  CHECK(out[1].sequence == 1);
  e.capture(std::vector<std::int16_t>(960, 0), false, out);
  REQUIRE(out.size() == 3);
  CHECK(out[2].flags == proto::voice::end);
  CHECK(out[2].opus.empty());
  CHECK_FALSE(e.talking());
  // Not held: nothing.
  e.capture(std::vector<std::int16_t>(960 * 3, 99), false, out);
  CHECK(out.size() == 3);
  // Muted: nothing even when held.
  s.muted = true;
  e.configure(s);
  e.capture(std::vector<std::int16_t>(960, 99), true, out);
  CHECK(out.size() == 3);
  CHECK(e.stats().sent == 3);
}

TEST_CASE("voice engine: a voice plays at the speaker's Link, on the side it stands") {
  Engine e(fake_codecs());
  e.set_listener(Listener{});            // origin, looking down -z
  e.set_speaker(2, Vec3{5, 0, 0});       // on our right
  for (std::uint16_t i = 0; i < 6; ++i) e.receive(frame_from(2, i, 16384), 0);
  CHECK_FALSE(e.speaking(2, 0));
  const Out o = render(e, 8, 0);
  CHECK(o.peak(1) > 0.3f);
  CHECK(o.peak(0) < 0.01f);
  CHECK(e.speaking(2, 35'000));
  CHECK_FALSE(e.speaking(2, 35'000 + 300'000));
  CHECK_FALSE(e.speaking(3, 35'000));
  CHECK(e.stats().decoded >= 2);
}

TEST_CASE("voice engine: 5.1 puts a voice behind us in the rear speakers") {
  Engine e(fake_codecs());
  e.set_listener(Listener{});
  e.set_speaker(1, Vec3{0, 0, 6});  // behind
  for (std::uint16_t i = 0; i < 6; ++i) e.receive(frame_from(1, i, 16384), 0);
  const Out o = render(e, 8, 0, 6);
  CHECK(o.peak(4) > 0.1f);
  CHECK(o.peak(5) > 0.1f);
  CHECK(o.peak(0) < 0.01f);
  CHECK(o.peak(2) == 0.0f);  // centre and LFE untouched
  CHECK(o.peak(3) == 0.0f);
}

TEST_CASE("voice engine: out of range, unplaced, deafened or forgotten is silent") {
  Engine far(fake_codecs());
  far.set_speaker(3, Vec3{0, 0, -100});
  for (std::uint16_t i = 0; i < 6; ++i) far.receive(frame_from(3, i, 16384), 0);
  const Out f = render(far, 8, 0);
  CHECK(f.peak(0) == 0.0f);
  CHECK_FALSE(far.speaking(3, 35'000));

  Engine unplaced(fake_codecs());
  for (std::uint16_t i = 0; i < 6; ++i) unplaced.receive(frame_from(4, i, 16384), 0);
  CHECK(unplaced.stats().unplaced == 6);
  CHECK(render(unplaced, 8, 0).peak(0) == 0.0f);

  Settings deaf;
  deaf.deafened = true;
  Engine d(fake_codecs(), deaf);
  d.set_speaker(1, Vec3{1, 0, 0});
  for (std::uint16_t i = 0; i < 6; ++i) d.receive(frame_from(1, i, 16384), 0);
  CHECK(render(d, 8, 0).peak(1) == 0.0f);

  Engine gone(fake_codecs());
  gone.set_speaker(1, Vec3{1, 0, 0});
  for (std::uint16_t i = 0; i < 6; ++i) gone.receive(frame_from(1, i, 16384), 0);
  gone.forget(1);
  CHECK(render(gone, 8, 0).peak(1) == 0.0f);
  // Not player numbers: ignored.
  gone.receive(frame_from(0, 1, 1), 0);
  gone.receive(frame_from(17, 1, 1), 0);
  CHECK(gone.stats().received == 6);
}

TEST_CASE("voice engine: sixteen players talking at once all play") {
  int fec = 0;
  Engine e(fake_codecs(&fec));
  e.set_listener(Listener{});
  for (std::uint8_t p = 1; p <= 16; ++p) {
    // A ring around us, 4 m out.
    const float a = static_cast<float>(p) * 0.3927f;
    e.set_speaker(p, Vec3{4.0f * std::sin(a), 0, -4.0f * std::cos(a)});
    for (std::uint16_t i = 0; i < 6; ++i) {
      if (p == 16 && i == 2) continue;  // one lost: the next one's FEC fills it
      e.receive(frame_from(p, i, 1000), 0);
    }
  }
  render(e, 40, 0);
  for (std::uint8_t p = 1; p <= 16; ++p) CHECK(e.speaking(p, 195'000));
  CHECK(fec == 1);
}

TEST_CASE("voice engine: the volume scales every voice; a player's own volume scales its") {
  Settings s;
  s.volume = 0.5f;
  s.player_volume[2] = 2.0f;
  Engine e(fake_codecs(), s);
  e.set_speaker(1, Vec3{0, 0, -1});
  e.set_speaker(2, Vec3{0, 0, -1});
  for (std::uint16_t i = 0; i < 6; ++i) e.receive(frame_from(1, i, 8192), 0);
  const float one = render(e, 8, 0).peak(0);
  Engine e2(fake_codecs(), s);
  e2.set_speaker(2, Vec3{0, 0, -1});
  for (std::uint16_t i = 0; i < 6; ++i) e2.receive(frame_from(2, i, 8192), 0);
  const float two = render(e2, 8, 0).peak(0);
  CHECK(two == doctest::Approx(one * 2.0f).epsilon(0.02));
}
