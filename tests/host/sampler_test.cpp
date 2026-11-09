#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <variant>
#include <vector>

#include "coop/host/sampler.hpp"
#include "coop/protocol/codec.hpp"
#include "fake_memory.hpp"

using namespace coop::host;
namespace proto = coop::protocol;

namespace {

constexpr std::uint64_t kFrameUs = 16'667;  // 60 frames a second
constexpr std::uint64_t kStart = 5'000'000;

PlayerReading at(float x, std::optional<bool> event = false) {
  PlayerReading r;
  r.position = {x, 0.0f, 0.0f};
  r.rotation = Quat{0, 0, 0, 1};
  r.age = proto::Age::adult;
  r.scene = "spot00";
  r.event_running = event;
  return r;
}

struct Decoded {
  std::vector<proto::PlayerSample> samples;
  std::vector<proto::PlayerStatus> statuses;
  std::vector<proto::PlayerEquip> equips;
};

// Decodes what one frame returned, checking each message's channel.
Decoded decode(const std::vector<Outgoing>& out) {
  Decoded d;
  for (const auto& o : out) {
    proto::Message m;
    REQUIRE(proto::decode(o.bytes, m) == proto::Error::none);
    CHECK(proto::check_received(m, proto::Sender::client, o.channel) == proto::Error::none);
    if (auto* s = std::get_if<proto::PlayerSample>(&m)) {
      CHECK(o.channel == proto::kChannelPlayerSamples);
      d.samples.push_back(*s);
    } else if (auto* st = std::get_if<proto::PlayerStatus>(&m)) {
      CHECK(o.channel == proto::kChannelSession);
      d.statuses.push_back(*st);
    } else if (auto* e = std::get_if<proto::PlayerEquip>(&m)) {
      CHECK(o.channel == proto::kChannelSession);
      d.equips.push_back(*e);
    } else {
      FAIL("unexpected message");
    }
  }
  return d;
}

}  // namespace

TEST_CASE("host sampler: the first frame sends a status and a full sample") {
  Sampler s;
  const auto d = decode(s.frame(at(1.0f), kStart));
  REQUIRE(d.statuses.size() == 1);
  CHECK(d.statuses[0].player_number == proto::kPlayerSelf);
  CHECK(d.statuses[0].flags == 0);
  REQUIRE(d.samples.size() == 1);
  const auto& p = d.samples[0];
  CHECK(p.player_number == proto::kPlayerSelf);
  CHECK(p.time.server_time_us == kStart);
  CHECK(p.time.sequence == 0);
  CHECK(p.position == Vec3{1.0f, 0.0f, 0.0f});
  CHECK_FALSE(p.velocity);  // nothing to take it from yet
  CHECK(p.rotation == Quat{0, 0, 0, 1});
  CHECK(p.scene == std::string("spot00"));
  CHECK(p.age == proto::Age::adult);
}

TEST_CASE("host sampler: every frame while moving") {
  Sampler s;
  std::uint32_t count = 0;
  for (int i = 0; i < 120; ++i) {
    const auto d = decode(s.frame(at(0.1f * static_cast<float>(i)), kStart + i * kFrameUs));
    REQUIRE(d.samples.size() == 1);
    CHECK(d.samples[0].time.sequence == count);
    CHECK(d.samples[0].time.server_time_us == kStart + i * kFrameUs);
    ++count;
  }
  CHECK(s.next_sequence() == 120);
}

TEST_CASE("host sampler: at least 4 a second while still, and not every frame") {
  Sampler s;
  std::vector<std::uint64_t> times;
  for (int i = 0; i < 600; ++i) {  // 10 seconds
    const std::uint64_t now = kStart + i * kFrameUs;
    for (const auto& p : decode(s.frame(at(2.0f), now)).samples) times.push_back(p.time.server_time_us);
  }
  CHECK(times.size() >= 40);
  CHECK(times.size() <= 60);
  for (std::size_t i = 1; i < times.size(); ++i) CHECK(times[i] - times[i - 1] <= 250'000);
  CHECK(kStart + 599 * kFrameUs - times.back() <= 250'000);
}

TEST_CASE("host sampler: keep-alive holds with slow, uneven frames") {
  Sampler s;
  std::uint64_t now = kStart, last = 0;
  bool first = true;
  for (int i = 0; i < 200; ++i) {
    now += (i % 3 == 0) ? 50'000 : 20'000;  // 20 to 50 ms frames
    const auto d = decode(s.frame(at(2.0f), now));
    if (!d.samples.empty()) {
      if (!first) CHECK(now - last <= 250'000);
      last = now;
      first = false;
    }
  }
}

TEST_CASE("host sampler: one still sample right after stopping") {
  Sampler s;
  for (int i = 0; i < 10; ++i) s.frame(at(0.1f * static_cast<float>(i)), kStart + i * kFrameUs);
  REQUIRE(s.last_sample());
  CHECK(s.last_sample()->velocity.has_value());

  // Stopped at 0.9: the next frame still sends, with zero velocity.
  auto d = decode(s.frame(at(0.9f), kStart + 10 * kFrameUs));
  REQUIRE(d.samples.size() == 1);
  CHECK(d.samples[0].velocity == Vec3{0, 0, 0});
  // Then nothing until the keep-alive.
  d = decode(s.frame(at(0.9f), kStart + 11 * kFrameUs));
  CHECK(d.samples.empty());
}

TEST_CASE("host sampler: velocity") {
  SUBCASE("from the last two readings, in metres a second") {
    SamplerSettings settings;
    settings.units_per_metre = 100.0f;  // a game in centimetres
    Sampler s(settings);
    s.frame(at(0.0f), kStart);
    const auto d = decode(s.frame(at(10.0f), kStart + 100'000));  // 10 units in 0.1 s
    REQUIRE(d.samples.size() == 1);
    REQUIRE(d.samples[0].velocity);
    CHECK((*d.samples[0].velocity)[0] == doctest::Approx(1.0));  // 100 units/s = 1 m/s
    CHECK((*d.samples[0].velocity)[1] == 0.0f);
  }
  SUBCASE("none after a gap in the readings") {
    Sampler s;
    s.frame(at(0.0f), kStart);
    s.frame(std::nullopt, kStart + kFrameUs);
    const auto d = decode(s.frame(at(1.0f), kStart + 2 * kFrameUs));
    REQUIRE(d.samples.size() == 1);
    CHECK_FALSE(d.samples[0].velocity);
  }
  SUBCASE("none across a long stall") {
    Sampler s;
    s.frame(at(0.0f), kStart);
    const auto d = decode(s.frame(at(1.0f), kStart + 400'000));
    REQUIRE(d.samples.size() == 1);
    CHECK_FALSE(d.samples[0].velocity);
  }
  SUBCASE("none for a warp") {
    Sampler s;
    s.frame(at(0.0f), kStart);
    const auto d = decode(s.frame(at(5000.0f), kStart + kFrameUs));
    REQUIRE(d.samples.size() == 1);
    CHECK_FALSE(d.samples[0].velocity);
  }
  SUBCASE("none across a scene change") {
    Sampler s;
    s.frame(at(0.0f), kStart);
    auto r = at(0.5f);
    r.scene = "spot01";
    const auto d = decode(s.frame(r, kStart + kFrameUs));
    REQUIRE(d.samples.size() == 1);
    CHECK(d.samples[0].scene == std::string("spot01"));
    CHECK_FALSE(d.samples[0].velocity);
  }
  SUBCASE("none when the clock did not move on") {
    Sampler s;
    s.frame(at(0.0f), kStart);
    const auto d = decode(s.frame(at(1.0f), kStart));
    REQUIRE(d.samples.size() == 1);
    CHECK_FALSE(d.samples[0].velocity);
  }
}

TEST_CASE("host sampler: turning, scene and age changes send a sample while still") {
  Sampler s;
  s.frame(at(1.0f), kStart);
  s.frame(at(1.0f), kStart + kFrameUs);
  std::uint64_t now = kStart + kFrameUs;
  auto next = [&](const PlayerReading& r) { return decode(s.frame(r, now += kFrameUs)).samples.size(); };
  CHECK(next(at(1.0f)) == 0);
  auto r = at(1.0f);
  r.rotation = Quat{0, 0.7071068f, 0, 0.7071068f};
  CHECK(next(r) == 1);
  CHECK(next(r) == 0);
  r.age = proto::Age::child;
  CHECK(next(r) == 1);
  r.scene = "spot02";
  CHECK(next(r) == 1);
  r.rotation.reset();
  CHECK(next(r) == 1);
  CHECK(next(r) == 0);
}

TEST_CASE("host sampler: no reading, no sample") {
  Sampler s;
  CHECK(s.frame(std::nullopt, kStart).empty());
  CHECK(s.next_sequence() == 0);
}

TEST_CASE("host sampler: status on every change") {
  Sampler s;
  std::uint64_t now = kStart;
  auto statuses = [&](std::optional<bool> event) {
    return decode(s.frame(at(1.0f, event), now += kFrameUs)).statuses;
  };
  auto first = statuses(false);
  REQUIRE(first.size() == 1);
  CHECK(first[0].flags == 0);
  CHECK(statuses(false).empty());
  auto in = statuses(true);
  REQUIRE(in.size() == 1);
  CHECK(in[0].flags == proto::status::in_cutscene);
  CHECK(statuses(true).empty());
  CHECK(statuses(std::nullopt).empty());  // unknown: keep what was sent
  CHECK(s.status_flags() == proto::status::in_cutscene);
  auto out = statuses(false);
  REQUIRE(out.size() == 1);
  CHECK(out[0].flags == 0);

  // After a reconnect the status goes again, and so does a sample.
  s.reset();
  const auto d = decode(s.frame(at(1.0f, false), now += kFrameUs));
  CHECK(d.statuses.size() == 1);
  CHECK(d.samples.size() == 1);
  CHECK(d.samples[0].time.sequence == s.next_sequence() - 1);
}

TEST_CASE("host sampler: straight from guest memory") {
  test::FakeMemory m;
  const auto l = test::make_game(m);
  PlayerReader reader(m);
  Sampler s;
  auto d = decode(s.frame(reader, kStart));
  REQUIRE(d.samples.size() == 1);
  CHECK(d.samples[0].position == Vec3{1.0f, 2.0f, 3.0f});
  CHECK(d.samples[0].scene == std::string("spot00"));

  m.put_u64(l.events + addr::kEventRunning, 1);
  m.put_vec3(l.game_ui + addr::kGameUiPosition, {1.5f, 2.0f, 3.0f});
  d = decode(s.frame(reader, kStart + kFrameUs));
  REQUIRE(d.statuses.size() == 1);
  CHECK(d.statuses[0].flags == proto::status::in_cutscene);
  REQUIRE(d.samples.size() == 1);
  REQUIRE(d.samples[0].velocity);
  CHECK((*d.samples[0].velocity)[0] == doctest::Approx(0.5 / (kFrameUs / 1e6)));
}

TEST_CASE("host sampler: animation goes in the sample; a new command sends, a new frame does not") {
  Sampler s;
  std::uint64_t now = kStart;
  auto r = at(1.0f);
  r.animation = proto::Animation{"Wait", 0.0f, 1.0f};
  auto d = decode(s.frame(r, now));
  REQUIRE(d.samples.size() == 1);
  CHECK(d.samples[0].animation == r.animation);
  s.frame(r, now += kFrameUs);  // the first still frame after the first sample
  auto next = [&](const PlayerReading& reading) {
    return decode(s.frame(reading, now += kFrameUs)).samples;
  };
  r.animation->frame = 2.0f;
  CHECK(next(r).empty());  // the frame runs on by itself
  r.animation->name = "Wait_Look";
  auto sent = next(r);
  REQUIRE(sent.size() == 1);
  CHECK(sent[0].animation->name == "Wait_Look");
  CHECK(next(r).empty());
  r.animation->rate = 0.5f;
  CHECK(next(r).size() == 1);
  r.animation.reset();
  sent = next(r);
  REQUIRE(sent.size() == 1);
  CHECK_FALSE(sent[0].animation);
  CHECK(next(r).empty());
}

TEST_CASE("host sampler: equipment on every change, not every frame") {
  Sampler sampler;
  std::uint64_t now = kStart;
  PlayerReading r = at(0.0f);
  r.equipment.emplace();
  r.equipment->weapon = "Weapon_Sword_Kokiri";
  r.equipment->drawn = 0;

  auto d = decode(sampler.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK(d.equips[0].player_number == proto::kPlayerSelf);
  CHECK(d.equips[0].equipment == *r.equipment);

  // The same again: nothing.
  now += kFrameUs;
  CHECK(decode(sampler.frame(r, now)).equips.empty());

  // A read that fails for one frame is no change.
  PlayerReading failed = r;
  failed.equipment->weapon.reset();
  now += kFrameUs;
  CHECK(decode(sampler.frame(failed, now)).equips.empty());
  failed.equipment.reset();
  now += kFrameUs;
  CHECK(decode(sampler.frame(failed, now)).equips.empty());

  // Drawing the sword is.
  r.equipment->drawn = 1;
  now += kFrameUs;
  d = decode(sampler.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK(d.equips[0].equipment.drawn == 1);
  CHECK(sampler.last_equipment() == r.equipment);

  // After a reconnect it goes out again.
  sampler.reset();
  now += kFrameUs;
  CHECK(decode(sampler.frame(r, now)).equips.size() == 1);
}
