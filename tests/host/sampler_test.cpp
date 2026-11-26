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

TEST_CASE("host sampler: high refresh does not send more samples (issue 106)") {
  // 165 presents a second, moving every one: about 55 samples a second, not 165.
  Sampler s;
  constexpr std::uint64_t kFastUs = 6'061;
  std::vector<std::uint64_t> times;
  for (int i = 0; i < 165; ++i) {
    for (const auto& p : decode(s.frame(at(0.05f * static_cast<float>(i)), kStart + i * kFastUs)).samples) {
      times.push_back(p.time.server_time_us);
    }
  }
  CHECK(times.size() >= 48);
  CHECK(times.size() <= 60);
  for (std::size_t i = 1; i < times.size(); ++i) CHECK(times[i] - times[i - 1] >= 14'000);
  // The last place is sent once the player stops, even from a frame held back.
  const std::uint64_t stop = kStart + 165 * kFastUs;
  for (int i = 0; i < 4; ++i) s.frame(at(9.0f), stop + i * kFastUs);
  REQUIRE(s.last_sample());
  CHECK(s.last_sample()->position == Vec3{9.0f, 0.0f, 0.0f});
}

TEST_CASE("host sampler: every frame at 60 with uneven frames") {
  Sampler s;
  std::uint64_t now = kStart;
  for (int i = 0; i < 60; ++i) {
    now += (i % 2 == 0) ? 15'000 : 18'334;
    CHECK(decode(s.frame(at(0.1f * static_cast<float>(i)), now)).samples.size() == 1);
  }
}

TEST_CASE("host sampler: a scene or age change is sent at once, inside the gap") {
  Sampler s;
  s.frame(at(1.0f), kStart);
  auto r = at(1.0f);
  r.scene = "spot01";
  CHECK(decode(s.frame(r, kStart + 2'000)).samples.size() == 1);
  r.age = proto::Age::child;
  CHECK(decode(s.frame(r, kStart + 4'000)).samples.size() == 1);
  r.position = {2.0f, 0.0f, 0.0f};
  CHECK(decode(s.frame(r, kStart + 6'000)).samples.empty());  // a move waits
  CHECK(decode(s.frame(r, kStart + 4'000 + 14'000)).samples.size() == 1);
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
    s.frame(at(0.0f), kStart - kFrameUs);  // sent, no velocity: the next still frame is not
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

TEST_CASE("host sampler: held, the last sample again each keep-alive, standing, with a new sequence") {
  Sampler s;
  for (int i = 0; i < 10; ++i) s.frame(at(0.1f * static_cast<float>(i)), kStart + i * kFrameUs);
  const std::uint64_t moved = kStart + 9 * kFrameUs;
  REQUIRE(s.last_sample());
  const auto last = *s.last_sample();
  CHECK(last.velocity != Vec3{0, 0, 0});
  // The game is held from here: no frame() comes.
  CHECK_FALSE(s.hold(moved + 100'000));  // keep-alive time not passed yet
  const std::uint32_t seq = s.next_sequence();
  const auto again = s.hold(moved + 200'000);
  REQUIRE(again);
  const auto d = decode({*again});
  REQUIRE(d.samples.size() == 1);
  CHECK(d.samples[0].position == last.position);
  CHECK(d.samples[0].rotation == last.rotation);
  CHECK(d.samples[0].scene == last.scene);
  CHECK(d.samples[0].velocity == Vec3{0, 0, 0});  // standing, not sliding on
  CHECK(d.samples[0].time.server_time_us == moved + 200'000);
  CHECK(d.samples[0].time.sequence == seq);
  CHECK(s.next_sequence() == seq + 1);  // presence drops a repeated sequence
  CHECK_FALSE(s.hold(moved + 300'000));
  CHECK(s.hold(moved + 400'000));
  // Ten held seconds: never a gap longer than the keep-alive and a step.
  std::uint64_t at_us = moved + 400'000, sent = at_us;
  for (int i = 0; i < 1000; ++i) {
    at_us += 10'000;
    if (s.hold(at_us)) {
      CHECK(at_us - sent <= 210'000);
      sent = at_us;
    }
  }
  // Released: the frames go on with a higher sequence (Link walks on).
  const auto next = decode(s.frame(at(1.5f), at_us + kFrameUs));
  REQUIRE(next.samples.size() == 1);
  CHECK(next.samples[0].time.sequence > seq);
}

TEST_CASE("host sampler: frames after a hold stamp after the last held sample") {
  Sampler s;
  std::uint64_t now = kStart;
  s.frame(at(1.0f), now);
  const auto held = s.hold(now + 1'000'000);
  REQUIRE(held);
  const std::uint64_t held_at = decode({*held}).samples.at(0).time.server_time_us;
  // Released: the game frame's time is up to 250 ms older than the held sample (frame_aligned_us).
  std::uint64_t frame_us = held_at - 200'000;
  std::uint64_t last = held_at;
  int sent = 0;
  for (int i = 0; i < 40; ++i, frame_us += kFrameUs) {
    for (const auto& p : decode(s.frame(at(1.0f + 0.1f * static_cast<float>(i)), frame_us)).samples) {
      CHECK(p.time.server_time_us > last);  // presence would drop an older one as too old
      last = p.time.server_time_us;
      ++sent;
    }
  }
  CHECK(sent > 0);
}

TEST_CASE("host sampler: a reconnect while held sends the status, the equipment and the sample") {
  Sampler s;
  s.set_color(0x00ff00ffu);
  s.frame(at(1.0f), kStart);
  CHECK(s.hold_reconnected(kStart).size() == 3);
  const auto d = decode(s.hold_reconnected(kStart + 1));
  CHECK(d.statuses.size() == 1);
  REQUIRE(d.equips.size() == 1);
  CHECK(d.equips[0].equipment.color == std::optional<std::uint32_t>(0x00ff00ffu));
  REQUIRE(d.samples.size() == 1);
  CHECK(d.samples[0].position == at(1.0f).position);
  CHECK(d.samples[0].velocity == Vec3{0, 0, 0});
  // In a load: nothing.
  s.frame(std::nullopt, kStart + kFrameUs);
  CHECK(s.hold_reconnected(kStart + 2 * kFrameUs).empty());
}

TEST_CASE("host sampler: a reconnect while held to a new server uses its clock") {
  Sampler s;
  const std::uint64_t old_clock = 3'600'000'000ull;  // the old server ran an hour
  s.frame(at(1.0f), old_clock);
  REQUIRE(s.hold(old_clock + 1'000'000));
  // The new server started a second ago.
  const std::uint64_t now = 1'000'000;
  const auto d = decode(s.hold_reconnected(now));
  REQUIRE(d.samples.size() == 1);
  CHECK(d.samples[0].time.server_time_us == now);
  // Still held: the keep-alive goes on.
  CHECK(s.hold(now + 100'000) == std::nullopt);
  REQUIRE(s.hold(now + 200'000));
  // Released, without and with the WELCOME's reset: frames send as Link moves, at their own times.
  for (const bool reset : {false, true}) {
    Sampler r = s;
    if (reset) r.reset();
    std::uint64_t frame_us = now + 300'000;
    int sent = 0;
    for (int i = 0; i < 30; ++i, frame_us += kFrameUs) {
      for (const auto& p : decode(r.frame(at(2.0f + 0.1f * static_cast<float>(i)), frame_us)).samples) {
        CHECK(p.time.server_time_us == frame_us);
        ++sent;
      }
    }
    CHECK(sent >= 5);
  }
}

TEST_CASE("host sampler: the floor after a hold covers only a frame's age") {
  // A held sample in one clock, then frames in a clock far behind it (a new server).
  for (const bool reset : {false, true}) {
    Sampler s;
    s.frame(at(1.0f), 3'600'000'000ull);
    REQUIRE(s.hold(3'601'000'000ull));
    if (reset) s.reset();  // the WELCOME's reset
    std::uint64_t frame_us = 1'000'000;
    int sent = 0;
    for (int i = 0; i < 30; ++i, frame_us += kFrameUs) {
      for (const auto& p : decode(s.frame(at(2.0f + 0.1f * static_cast<float>(i)), frame_us)).samples) {
        CHECK(p.time.server_time_us == frame_us);
        ++sent;
      }
    }
    CHECK(sent >= 5);
  }
}

TEST_CASE("host sampler: held in a load or before any sample, nothing") {
  Sampler s;
  CHECK_FALSE(s.hold(kStart));  // nothing sent yet
  s.frame(at(1.0f), kStart);
  s.frame(std::nullopt, kStart + kFrameUs);  // a load: no Link
  CHECK_FALSE(s.hold(kStart + 2'000'000));
}

TEST_CASE("host sampler: no reading, no sample, the status loading") {
  Sampler s;
  const auto d = decode(s.frame(std::nullopt, kStart));
  CHECK(d.samples.empty());
  REQUIRE(d.statuses.size() == 1);
  CHECK(d.statuses[0].flags == proto::status::loading);
  CHECK(s.next_sequence() == 0);
  CHECK(s.frame(std::nullopt, kStart + kFrameUs).empty());  // no change, nothing
}

TEST_CASE("host sampler: status_bits, every away bit (issue 150, away-status)") {
  namespace st = proto::status;
  PlayerReading r = at(1.0f, false);

  SUBCASE("play, nothing away") { CHECK(status_bits(0, r, false) == 0); }
  SUBCASE("no Link: loading, the menus clear, the cutscene and dead keep") {
    const std::uint8_t all = st::paused | st::in_menu | st::in_cutscene | st::dead;
    CHECK(status_bits(all, std::nullopt, false) == (st::loading | st::in_cutscene | st::dead));
    CHECK(status_bits(0, std::nullopt, false) == st::loading);
  }
  SUBCASE("the title's menu up with a Link read: loading, its menus are not the pause menu") {
    r.in_menu = true;
    CHECK(status_bits(0, r, true) == st::loading);
  }
  SUBCASE("Link read again: loading clears") {
    CHECK(status_bits(st::loading | st::dead, r, false) == st::dead);
  }
  SUBCASE("a game menu in front, no event: in_menu and paused") {
    r.in_menu = true;
    CHECK(status_bits(0, r, false) == (st::in_menu | st::paused));
    r.in_menu = false;
    CHECK(status_bits(st::in_menu | st::paused, r, false) == 0);
  }
  SUBCASE("a menu inside our event (a talk's choices): in_menu, not paused") {
    r.in_menu = true;
    r.event_running = true;
    CHECK(status_bits(0, r, false) == (st::in_cutscene | st::in_menu));
  }
  SUBCASE("a menu during another player's event replayed here: paused (our world stops)") {
    r.in_menu = true;
    r.event_running = true;
    r.event_whose = EventWhose::others;
    CHECK(status_bits(0, r, false) == (st::in_menu | st::paused));
  }
  SUBCASE("menu state not known (emulator, Linux): never set") {
    r.in_menu = std::nullopt;
    CHECK(status_bits(0, r, false) == 0);
    CHECK(status_bits(st::in_menu | st::paused, r, false) == 0);
  }
  SUBCASE("dead: set, cleared, unknown keeps") {
    r.dead = true;
    CHECK(status_bits(0, r, false) == st::dead);
    r.dead = false;
    CHECK(status_bits(st::dead, r, false) == 0);
    r.dead = std::nullopt;
    CHECK(status_bits(st::dead, r, false) == st::dead);
    CHECK(status_bits(0, r, false) == 0);
  }
}

TEST_CASE("host sampler: a world standing still with no menu named is paused (row 164 follow-up, quick switch)") {
  namespace st = proto::status;
  PlayerReading r = at(1.0f, false);
  SUBCASE("paused, not in_menu, cleared when it runs again") {
    r.world_still = true;
    CHECK(status_bits(0, r, false) == st::paused);
    r.world_still = false;
    CHECK(status_bits(st::paused, r, false) == 0);
  }
  SUBCASE("in our event: not paused (a cutscene may hold Link)") {
    r.world_still = true;
    r.event_running = true;
    CHECK(status_bits(0, r, false) == st::in_cutscene);
  }
  SUBCASE("no Link: loading only") {
    CHECK(status_bits(st::paused, std::nullopt, false) == st::loading);
  }
}

TEST_CASE("host sampler: WorldStill is opt-in, a held frame is no pause by default (pr-navi-01112)") {
  // Aiming at a boss's eye, a ledge, a ladder, ice, a Like-Like's hold, a shock: each holds Link's frame while the
  // world runs; as `paused` it would hand the boss away after fight_owner_paused_us. The quick-switch menu comes
  // from the game's own screens (in_menu) instead.
  CHECK_FALSE(world_still_on(nullptr));
  CHECK_FALSE(world_still_on(""));
  CHECK_FALSE(world_still_on("0"));
  CHECK(world_still_on("1"));
  namespace st = proto::status;
  PlayerReading r = at(1.0f, false);
  r.animation = proto::Animation{"Bow_Aim", 12.0f, 1.0f};  // a held aim frame, Link's place unchanged
  CHECK(status_bits(0, r, false) == 0);                    // world_still not set: playing
  SUBCASE("the quick-switch menu read from the game: in_menu, paused") {
    r.in_menu = true;
    CHECK(status_bits(0, r, false) == (st::in_menu | st::paused));
  }
}

TEST_CASE("host sampler: WorldStill reads our Link's animation frame and place") {
  constexpr std::uint64_t kStep = 16'667;
  PlayerReading r = at(1.0f, false);
  r.animation = proto::Animation{"Wait", 0.0f, 1.0f};
  WorldStill w;
  std::uint64_t now = 1'000'000;
  SUBCASE("a frame that steps every reading: running") {
    for (int i = 0; i < 60; ++i) {
      r.animation->frame += 1.0f;
      CHECK_FALSE(w.step(r, now += kStep));
    }
  }
  SUBCASE("a frame held 0.3 s: still, until it steps again") {
    CHECK_FALSE(w.step(r, now));
    CHECK_FALSE(w.step(r, now + WorldStill::kStillUs - 1));
    CHECK(w.step(r, now + WorldStill::kStillUs));
    CHECK(w.step(r, now + 10'000'000));   // a long pause stays still
    r.animation->frame += 1.0f;
    CHECK_FALSE(w.step(r, now + 10'000'000 + kStep));
  }
  SUBCASE("rate 0 (a pose held on purpose): never still") {
    r.animation->rate = 0.0f;
    CHECK_FALSE(w.step(r, now));
    CHECK_FALSE(w.step(r, now + 5'000'000));
  }
  SUBCASE("our event: never still, and judged anew after it") {
    r.event_running = true;
    CHECK_FALSE(w.step(r, now));
    CHECK_FALSE(w.step(r, now + 5'000'000));
    r.event_running = false;
    CHECK_FALSE(w.step(r, now + 5'000'001));
    CHECK(w.step(r, now + 5'000'001 + WorldStill::kStillUs));
  }
  SUBCASE("a new animation or a move starts the count again") {
    CHECK_FALSE(w.step(r, now));
    r.animation->name = "Run";
    CHECK_FALSE(w.step(r, now + 200'000));
    CHECK_FALSE(w.step(r, now + 400'000));   // 0.2 s since the change
    r.position[0] += 0.5f;
    CHECK_FALSE(w.step(r, now + 600'000));
    CHECK(w.step(r, now + 900'000));
  }
  SUBCASE("no animation read: not judged") {
    r.animation.reset();
    CHECK_FALSE(w.step(r, now));
    CHECK_FALSE(w.step(r, now + 5'000'000));
  }
  SUBCASE("pr-navi-01112: a carried Link (its reading's pose is not the game's own) is not judged, and anew after") {
    // The hook passes no animation while our Link is lifted (CoopLiftPlayerGame::Carried): the carried pose is
    // 'Eaten' at frame 0, rate 1, unmoved while the carrier stands.
    const proto::Animation carried{"Eaten", 0.0f, 1.0f};
    CHECK_FALSE(w.step(&carried, r.position, false, now));  // (were it judged, this would start the count)
    CHECK_FALSE(w.step(nullptr, r.position, false, now));
    CHECK_FALSE(w.step(nullptr, r.position, false, now + 5'000'000));
    CHECK_FALSE(w.step(&*r.animation, r.position, false, now + 5'000'001));
    CHECK(w.step(&*r.animation, r.position, false, now + 5'000'001 + WorldStill::kStillUs));
  }
}

TEST_CASE("host sampler: each away bit goes out once on its change and back") {
  namespace st = proto::status;
  Sampler s;
  std::uint64_t now = kStart;
  auto statuses = [&](const std::optional<PlayerReading>& r, bool title = false) {
    return decode(s.frame(r, now += kFrameUs, title)).statuses;
  };
  PlayerReading r = at(1.0f, false);
  REQUIRE(statuses(r).size() == 1);

  r.in_menu = true;  // the pause menu opens
  auto paused = statuses(r);
  REQUIRE(paused.size() == 1);
  CHECK(paused[0].flags == (st::in_menu | st::paused));
  CHECK(statuses(r).empty());
  r.in_menu = false;  // and closes
  auto back = statuses(r);
  REQUIRE(back.size() == 1);
  CHECK(back[0].flags == 0);

  r.dead = true;  // Game Over
  auto dead = statuses(r);
  REQUIRE(dead.size() == 1);
  CHECK(dead[0].flags == st::dead);
  auto load = statuses(std::nullopt);  // Try Again: the reload
  REQUIRE(load.size() == 1);
  CHECK(load[0].flags == (st::dead | st::loading));
  r.dead = false;  // Link back with his hearts
  auto up = statuses(r);
  REQUIRE(up.size() == 1);
  CHECK(up[0].flags == 0);

  auto title = statuses(r, true);  // the title's menu (Quit Game)
  REQUIRE(title.size() == 1);
  CHECK(title[0].flags == st::loading);
  CHECK(s.last_sample().has_value());
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

TEST_CASE("host sampler: animation goes in the sample, a new command sends, a new frame does not") {
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

TEST_CASE("host sampler: the colour goes out with the equipment") {
  SamplerSettings settings;
  settings.color = 0x40C0FFu;
  Sampler sampler(settings);
  std::uint64_t now = kStart;
  // No equipment read yet: the colour goes alone.
  PlayerReading r = at(0.0f);
  auto d = decode(sampler.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK(d.equips[0].equipment.color == 0x40C0FFu);
  CHECK_FALSE(d.equips[0].equipment.weapon);
  // The equipment joins it, and it stays in every later one.
  r.equipment.emplace();
  r.equipment->weapon = "Weapon_Sword_Kokiri";
  now += kFrameUs;
  d = decode(sampler.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK(d.equips[0].equipment.color == 0x40C0FFu);
  CHECK(d.equips[0].equipment.weapon == "Weapon_Sword_Kokiri");
  now += kFrameUs;
  CHECK(decode(sampler.frame(r, now)).equips.empty());

  // Without a colour setting none is sent.
  Sampler plain;
  d = decode(plain.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK_FALSE(d.equips[0].equipment.color);
}

TEST_CASE("host sampler: a colour changed in the game goes out at once, and again after a reconnect") {
  Sampler sampler;
  std::uint64_t now = kStart;
  PlayerReading r = at(0.0f);
  r.equipment.emplace();
  r.equipment->weapon = "Weapon_Sword_Kokiri";
  auto d = decode(sampler.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK_FALSE(d.equips[0].equipment.color);
  // The pause menu set a colour: the next frame sends it, with the equipment.
  sampler.set_color(0xFF0000u);
  now += kFrameUs;
  d = decode(sampler.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK(d.equips[0].equipment.color == 0xFF0000u);
  CHECK(d.equips[0].equipment.weapon == "Weapon_Sword_Kokiri");
  sampler.set_color(0x0000FFu);
  now += kFrameUs;
  d = decode(sampler.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK(d.equips[0].equipment.color == 0x0000FFu);
  // The same colour again: nothing new to send.
  sampler.set_color(0x0000FFu);
  now += kFrameUs;
  CHECK(decode(sampler.frame(r, now)).equips.empty());
  // A late joiner or a reconnect: reset() sends the new colour again.
  sampler.reset();
  now += kFrameUs;
  d = decode(sampler.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK(d.equips[0].equipment.color == 0x0000FFu);
}

TEST_CASE("host sampler: 'by player number' after a colour sends the slot colour") {
  Sampler sampler;
  sampler.set_player_number(3);
  std::uint64_t now = kStart;
  PlayerReading r = at(0.0f);
  r.equipment.emplace();
  // Unset from the start: nothing (the others use the slot colour already).
  auto d = decode(sampler.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK_FALSE(d.equips[0].equipment.color);
  sampler.set_color(0xFF0000u);
  now += kFrameUs;
  d = decode(sampler.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK(d.equips[0].equipment.color == 0xFF0000u);
  // Back to "by player number": the slot colour goes out, not nothing.
  sampler.set_color(std::nullopt);
  now += kFrameUs;
  d = decode(sampler.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK(d.equips[0].equipment.color == coop::protocol::slot_color(3));
  // A reconnect with another number: that number's slot colour.
  sampler.reset();
  sampler.set_player_number(1);
  now += kFrameUs;
  d = decode(sampler.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK(d.equips[0].equipment.color == coop::protocol::slot_color(1));
  // A colour again wins over the slot colour.
  sampler.set_color(0x00FF00u);
  now += kFrameUs;
  d = decode(sampler.frame(r, now));
  REQUIRE(d.equips.size() == 1);
  CHECK(d.equips[0].equipment.color == 0x00FF00u);
}

// jitter-b: presents at 60 a second read a game that moves 30 times a second. Stamped with the
// present's time, every other step's velocity came out double (a 16 ms gap over a 33 ms move) and
// the sample times were off by up to a present; stamped with the game frame's time
// (frame_aligned_us), every sample is 33 ms apart and moves at the real speed.
TEST_CASE("host sampler: samples stamped with the game frame read at 60 presents a second") {
  constexpr std::uint64_t kGameUs = 33'333;
  Sampler s;
  std::vector<proto::PlayerSample> sent;
  const std::uint64_t present_after[] = {1'000, 9'000, 4'000, 15'000, 2'000, 12'000};
  for (int frame = 0; frame < 12; ++frame) {
    const std::uint64_t frame_at = kStart + static_cast<std::uint64_t>(frame) * kGameUs;
    const float x = 0.2f * static_cast<float>(frame);  // 6 m/s
    // Two presents read this frame, the first some time after it, the second a present later.
    for (const std::uint64_t later : {present_after[frame % 6], present_after[frame % 6] + kFrameUs}) {
      const std::uint64_t now = frame_at + later;
      const std::uint64_t age = later <= kFrameAgeMaxUs ? later : 0;
      for (const auto& smp : decode(s.frame(at(x), frame_aligned_us(now, age))).samples) {
        sent.push_back(smp);
      }
    }
  }
  REQUIRE(sent.size() >= 10);
  for (std::size_t i = 1; i < sent.size(); ++i) {
    CHECK(sent[i].time.server_time_us - sent[i - 1].time.server_time_us == kGameUs);
    if (sent[i].velocity) {
      CHECK((*sent[i].velocity)[0] == doctest::Approx(6.0f).epsilon(0.01));
    }
  }
}

TEST_CASE("host sampler: frame_aligned_us") {
  CHECK(frame_aligned_us(1'000'000, 0) == 1'000'000);        // not known
  CHECK(frame_aligned_us(1'000'000, 12'000) == 988'000);
  CHECK(frame_aligned_us(1'000'000, kFrameAgeMaxUs + 1) == 1'000'000);  // a pause: now
  CHECK(frame_aligned_us(10, 20) == 10);                      // no wrap
}

// review-jitter: a 10 fps game (the owner's games ran at 7-18 fps) read at 60 presents a second
// stays on frame stamps: samples 100 ms apart at the real speed.
TEST_CASE("host sampler: a 10 fps game read at 60 presents a second") {
  constexpr std::uint64_t kGameUs = 100'000;
  Sampler s;
  std::vector<proto::PlayerSample> sent;
  for (int frame = 0; frame < 10; ++frame) {
    const std::uint64_t frame_at = kStart + static_cast<std::uint64_t>(frame) * kGameUs;
    const float x = 0.6f * static_cast<float>(frame);  // 6 m/s
    for (std::uint64_t later = 3'000; later < kGameUs; later += kFrameUs) {  // six presents a frame
      for (const auto& smp : decode(s.frame(at(x), frame_aligned_us(frame_at + later, later))).samples) {
        sent.push_back(smp);
      }
    }
  }
  REQUIRE(sent.size() >= 9);
  for (std::size_t i = 1; i < sent.size(); ++i) {
    CHECK(sent[i].time.server_time_us - sent[i - 1].time.server_time_us == kGameUs);
    REQUIRE(sent[i].velocity.has_value());
    CHECK((*sent[i].velocity)[0] == doctest::Approx(6.0f).epsilon(0.01));
  }
}
