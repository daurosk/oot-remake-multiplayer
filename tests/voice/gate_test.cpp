#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>

#include "coop/voice/gate.hpp"
#include "coop/voice/jitter.hpp"

using namespace coop::voice;
using Step = Gate::Step;

namespace {

using Frame = std::array<std::int16_t, kSamplesPerFrame>;

Frame tone(float amplitude) {
  Frame f{};
  for (std::size_t i = 0; i < f.size(); ++i) {
    f[i] = static_cast<std::int16_t>(amplitude * 32767.0f *
                                     std::sin(2.0f * 3.14159265f * 300.0f * static_cast<float>(i) / 48000.0f));
  }
  return f;
}

Frame noise(int amplitude, std::uint32_t& seed) {
  Frame f{};
  for (auto& s : f) {
    seed = seed * 1664525u + 1013904223u;
    s = static_cast<std::int16_t>(static_cast<int>(seed >> 16) % (2 * amplitude + 1) - amplitude);
  }
  return f;
}

}  // namespace

TEST_CASE("voice gate: levels in dBFS") {
  CHECK(level_db(Frame{}) == doctest::Approx(-96.0f));
  CHECK(level_db(tone(1.0f)) == doctest::Approx(-3.0f).epsilon(0.05));
  CHECK(level_db(tone(0.1f)) == doctest::Approx(-23.0f).epsilon(0.05));
}

TEST_CASE("voice gate: push to talk sends while held, a short tail, then the end") {
  GateConfig c;
  c.mode = Mode::push_to_talk;
  c.tail_frames = 2;
  Gate g(c);
  const Frame quiet{};
  CHECK(g.update(quiet, false, false) == Step::closed);
  CHECK(g.update(quiet, true, false) == Step::open);
  CHECK(g.update(quiet, true, false) == Step::open);
  CHECK(g.update(quiet, false, false) == Step::open);  // tail
  CHECK(g.update(quiet, false, false) == Step::open);
  CHECK(g.update(quiet, false, false) == Step::ended);
  CHECK(g.update(quiet, false, false) == Step::closed);
  // Mute wins: held but muted ends at once.
  CHECK(g.update(quiet, true, false) == Step::open);
  CHECK(g.update(quiet, true, true) == Step::ended);
  CHECK(g.update(quiet, true, true) == Step::closed);
}

TEST_CASE("voice gate: open mic opens on speech over the noise and holds a hangover") {
  GateConfig c;
  c.mode = Mode::open_mic;
  c.hangover_frames = 3;
  Gate g(c);
  std::uint32_t seed = 1;
  // Room noise for a second: never opens, the floor settles near it.
  for (int i = 0; i < 50; ++i) CHECK(g.update(noise(60, seed), false, false) == Step::closed);
  CHECK(g.floor_db() < -50.0f);
  CHECK(g.update(tone(0.2f), false, false) == Step::closed);  // the attack: two speech frames
  CHECK_FALSE(g.just_opened());
  CHECK(g.update(tone(0.2f), false, false) == Step::open);
  CHECK(g.just_opened());
  CHECK(g.update(noise(60, seed), false, false) == Step::open);  // hangover
  CHECK_FALSE(g.just_opened());
  CHECK(g.update(noise(60, seed), false, false) == Step::open);
  CHECK(g.update(noise(60, seed), false, false) == Step::open);
  CHECK(g.update(noise(60, seed), false, false) == Step::ended);
  // The key does nothing in open mic; off never opens.
  CHECK(g.update(noise(60, seed), true, false) == Step::closed);
  c.mode = Mode::off;
  g.configure(c);
  CHECK(g.update(tone(0.5f), true, false) == Step::closed);
}

TEST_CASE("voice gate: a loud steady hum does not hold open mic open") {
  GateConfig c;
  c.mode = Mode::open_mic;
  c.threshold_db = -60.0f;
  Gate g(c);
  int open = 0;
  for (int i = 0; i < 1000; ++i) open += g.update(tone(0.05f), false, false) != Step::closed;
  // The floor climbs to the hum (3 dB a second, from -70 to -41 here): the
  // gate closes for good after about 10 s of the 20.
  CHECK(open > 400);
  CHECK(open < 600);
  CHECK(g.update(tone(0.05f), false, false) == Step::closed);
}

TEST_CASE("voice gate: a one-frame click does not open open mic") {
  GateConfig c;
  c.mode = Mode::open_mic;
  Gate g(c);
  std::uint32_t seed = 7;
  for (int i = 0; i < 50; ++i) g.update(noise(60, seed), false, false);
  for (int k = 0; k < 5; ++k) {
    CHECK(g.update(tone(0.5f), false, false) == Step::closed);  // a key click, a bump
    for (int i = 0; i < 5; ++i) CHECK(g.update(noise(60, seed), false, false) == Step::closed);
  }
  CHECK(g.update(tone(0.5f), false, false) == Step::closed);
  CHECK(g.update(tone(0.5f), false, false) == Step::open);  // a word
  CHECK(g.update(noise(60, seed), false, false) == Step::open);
}

TEST_CASE("voice gate: while the others are heard, open mic needs a louder voice") {
  GateConfig c;
  c.mode = Mode::open_mic;
  Gate g(c);
  std::uint32_t seed = 3;
  for (int i = 0; i < 50; ++i) g.update(noise(60, seed), false, false);
  g.set_hearing(true);
  // Their voice from our speakers, picked up quietly: about -43 dBFS.
  for (int i = 0; i < 10; ++i) CHECK(g.update(tone(0.01f), false, false) == Step::closed);
  // We talk over it, louder: it opens.
  CHECK(g.update(tone(0.2f), false, false) == Step::closed);
  CHECK(g.update(tone(0.2f), false, false) == Step::open);
  // Nobody heard: the same quiet level is speech again.
  Gate quiet(c);
  for (int i = 0; i < 50; ++i) quiet.update(noise(60, seed), false, false);
  quiet.update(tone(0.01f), false, false);
  CHECK(quiet.update(tone(0.01f), false, false) == Step::open);
}
