#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <cstdint>
#include <string>

#include "coop/presence/presence.hpp"

using namespace coop::presence;
namespace proto = coop::protocol;

namespace {

constexpr std::uint64_t kMs = 1000;

proto::PlayerSample sample(std::uint8_t player, std::uint64_t time_us, std::uint32_t seq,
                           Vec3 position, std::optional<Vec3> velocity = std::nullopt) {
  proto::PlayerSample s;
  s.player_number = player;
  s.time = {time_us, seq};
  s.position = position;
  s.velocity = velocity;
  return s;
}

// A presence that knows it is player 1 and that player 2 is in.
Presence two_players(Settings settings = {}) {
  Presence p(settings);
  proto::Welcome w;
  w.player_number = 1;
  w.players = {{2, "Two"}};
  p.on_welcome(w);
  return p;
}

double dist(const Vec3& a, const Vec3& b) {
  const double x = a[0] - b[0];
  const double y = a[1] - b[1];
  const double z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

double quat_mismatch(const Quat& a, const Quat& b) {
  const double d = std::fabs(static_cast<double>(a[0]) * b[0] + static_cast<double>(a[1]) * b[1] +
                             static_cast<double>(a[2]) * b[2] + static_cast<double>(a[3]) * b[3]);
  return 1.0 - std::fmin(d, 1.0);  // 0 when the same rotation
}

Quat about_y(double radians) {
  return {0.0f, static_cast<float>(std::sin(radians / 2)), 0.0f,
          static_cast<float>(std::cos(radians / 2))};
}

}  // namespace

TEST_CASE("presence: samples in order are buffered oldest first") {
  Presence p = two_players();
  for (std::uint32_t i = 0; i < 5; ++i) {
    CHECK(p.on_sample(sample(2, 1'000'000 + i * 16 * kMs, i, {float(i), 0, 0})) ==
          SampleResult::buffered);
  }
  const auto& s = p.player(2)->samples();
  REQUIRE(s.size() == 5);
  for (std::size_t i = 0; i < 5; ++i) CHECK(s[i].time.sequence == i);
}

TEST_CASE("presence: samples out of order are sorted by time then sequence") {
  Presence p = two_players();
  p.on_sample(sample(2, 300 * kMs, 3, {3, 0, 0}));
  p.on_sample(sample(2, 100 * kMs, 1, {1, 0, 0}));
  p.on_sample(sample(2, 200 * kMs, 2, {2, 0, 0}));
  p.on_sample(sample(2, 200 * kMs, 5, {2.5f, 0, 0}));  // same time, later sequence
  const auto& s = p.player(2)->samples();
  REQUIRE(s.size() == 4);
  CHECK(s[0].time.sequence == 1);
  CHECK(s[1].time.sequence == 2);
  CHECK(s[2].time.sequence == 5);
  CHECK(s[3].time.sequence == 3);
  // At exactly 200 ms the later of the two equal-time samples is the start.
  const auto pose = p.player(2)->pose_at(200 * kMs, p.settings());
  REQUIRE(pose);
  CHECK(pose->position[0] == doctest::Approx(2.5));
}

TEST_CASE("presence: duplicates are dropped") {
  Presence p = two_players();
  CHECK(p.on_sample(sample(2, 100 * kMs, 7, {1, 0, 0})) == SampleResult::buffered);
  CHECK(p.on_sample(sample(2, 100 * kMs, 7, {1, 0, 0})) == SampleResult::duplicate);
  CHECK(p.player(2)->samples().size() == 1);
}

TEST_CASE("presence: only the last 16 samples are kept, older ones refused when full") {
  Presence p = two_players();
  for (std::uint32_t i = 0; i < 20; ++i) p.on_sample(sample(2, (i + 1) * 16 * kMs, i, {0, 0, 0}));
  const auto& s = p.player(2)->samples();
  REQUIRE(s.size() == kSampleBuffer);
  CHECK(s.front().time.sequence == 4);
  CHECK(p.on_sample(sample(2, 1 * kMs, 100, {0, 0, 0})) == SampleResult::too_old);
}

TEST_CASE("presence: samples older than what was shown are dropped") {
  Presence p = two_players();
  p.on_sample(sample(2, 1000 * kMs, 1, {0, 0, 0}));
  p.on_sample(sample(2, 1200 * kMs, 3, {2, 0, 0}));
  REQUIRE(p.shown(2, 1250 * kMs));  // shows 1150 ms
  CHECK(p.on_sample(sample(2, 1100 * kMs, 2, {1, 0, 0})) == SampleResult::too_old);
  CHECK(p.on_sample(sample(2, 1160 * kMs, 4, {1, 0, 0})) == SampleResult::buffered);
}

TEST_CASE("presence: linear interpolation midpoints") {
  Settings st;
  st.hermite = false;
  Presence p = two_players(st);
  p.on_sample(sample(2, 1000 * kMs, 1, {0, 10, 0}));
  p.on_sample(sample(2, 1100 * kMs, 2, {10, 20, -4}));
  const auto pose = p.shown(2, 1150 * kMs);  // 100 ms delay: halfway
  REQUIRE(pose);
  CHECK(pose->how == How::interpolated);
  CHECK(pose->fade == 1.0f);
  CHECK(pose->at_us == 1050 * kMs);
  CHECK(pose->position[0] == doctest::Approx(5));
  CHECK(pose->position[1] == doctest::Approx(15));
  CHECK(pose->position[2] == doctest::Approx(-2));
  CHECK(pose->velocity[0] == doctest::Approx(100));  // 10 units in 0.1 s
  const auto q = p.player(2)->pose_at(1025 * kMs, p.settings());
  REQUIRE(q);
  CHECK(q->position[0] == doctest::Approx(2.5));
}

TEST_CASE("presence: Hermite matches a straight line when velocities agree, follows a curve") {
  Presence p = two_players();
  p.on_sample(sample(2, 0, 1, {0, 0, 0}, Vec3{100, 0, 0}));
  p.on_sample(sample(2, 100 * kMs, 2, {10, 0, 0}, Vec3{100, 0, 0}));
  auto pose = p.player(2)->pose_at(50 * kMs, p.settings());
  REQUIRE(pose);
  CHECK(pose->position[0] == doctest::Approx(5));
  CHECK(pose->velocity[0] == doctest::Approx(100));

  // On a circle, Hermite with the true velocities beats a straight line.
  constexpr double kR = 100.0;
  constexpr double kW = 2.0;  // radians a second
  auto on_circle = [&](double t) {
    return Vec3{float(kR * std::cos(kW * t)), 0, float(kR * std::sin(kW * t))};
  };
  auto speed = [&](double t) {
    return Vec3{float(-kR * kW * std::sin(kW * t)), 0, float(kR * kW * std::cos(kW * t))};
  };
  Settings lin;
  lin.hermite = false;
  Presence h = two_players();
  Presence l = two_players(lin);
  for (std::uint32_t i = 0; i < 2; ++i) {
    const double t = 0.2 * i;
    h.on_sample(sample(2, std::uint64_t(t * 1e6), i, on_circle(t), speed(t)));
    l.on_sample(sample(2, std::uint64_t(t * 1e6), i, on_circle(t), speed(t)));
  }
  const Vec3 truth = on_circle(0.1);
  const double eh = dist(h.player(2)->pose_at(100 * kMs, h.settings())->position, truth);
  const double el = dist(l.player(2)->pose_at(100 * kMs, l.settings())->position, truth);
  CHECK(eh < el / 10);
}

TEST_CASE("presence: slerp takes the shortest path") {
  const Quat id{0, 0, 0, 1};
  const Quat q90 = about_y(1.5707963267948966);
  const Quat neg90{-q90[0], -q90[1], -q90[2], -q90[3]};  // same rotation, other sign
  const Quat want = about_y(0.7853981633974483);
  CHECK(quat_mismatch(slerp(id, q90, 0.5), want) < 1e-6);
  CHECK(quat_mismatch(slerp(id, neg90, 0.5), want) < 1e-6);
  CHECK(quat_mismatch(slerp(id, q90, 0.0), id) < 1e-6);
  CHECK(quat_mismatch(slerp(id, q90, 1.0), q90) < 1e-6);
  // Unnormalised input still gives a unit result.
  const Quat r = slerp(Quat{0, 0, 0, 2}, q90, 0.5);
  CHECK(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3] == doctest::Approx(1.0));

  // Through presence: rotations between samples are slerped.
  Presence p = two_players();
  auto a = sample(2, 0, 1, {0, 0, 0});
  auto b = sample(2, 100 * kMs, 2, {0, 0, 0});
  a.rotation = id;
  b.rotation = neg90;
  p.on_sample(a);
  p.on_sample(b);
  const auto pose = p.player(2)->pose_at(50 * kMs, p.settings());
  REQUIRE(pose);
  REQUIRE(pose->rotation);
  CHECK(quat_mismatch(*pose->rotation, want) < 1e-6);
}

TEST_CASE("presence: extrapolation stops at its limit, then holds and fades") {
  Presence p = two_players();  // 150 ms extrapolation, 250 ms fade
  p.on_sample(sample(2, 1000 * kMs, 1, {0, 0, 0}, Vec3{10, 0, 0}));
  const Player& pl = *p.player(2);

  auto at = [&](std::uint64_t ms) { return *pl.pose_at(ms * kMs, p.settings()); };
  CHECK(at(1000).how == How::extrapolated);
  CHECK(at(1100).how == How::extrapolated);
  CHECK(at(1100).position[0] == doctest::Approx(1.0));
  CHECK(at(1150).how == How::extrapolated);
  CHECK(at(1150).position[0] == doctest::Approx(1.5));
  CHECK(at(1150).fade == 1.0f);

  CHECK(at(1151).how == How::held);
  CHECK(at(1400).position[0] == doctest::Approx(1.5));  // no further than the limit
  CHECK(at(1400).velocity[0] == 0.0f);
  CHECK(at(1275).fade == doctest::Approx(0.5));
  CHECK(at(1400).fade == 0.0f);
  CHECK(at(9000).fade == 0.0f);

  Settings st;
  st.fade_us = 0;
  CHECK(pl.pose_at(1151 * kMs, st)->fade == 0.0f);
}

TEST_CASE("presence: nothing to show before the first sample's time") {
  Presence p = two_players();
  CHECK_FALSE(p.shown(2, 5000 * kMs));  // no samples
  p.on_sample(sample(2, 5000 * kMs, 1, {0, 0, 0}));
  CHECK_FALSE(p.shown(2, 5050 * kMs));  // shows 4950 ms, before the sample
  CHECK(p.shown(2, 5100 * kMs));
  CHECK_FALSE(p.shown(3, 5100 * kMs));  // not in
}

TEST_CASE("presence: a gap after a long pause is not joined") {
  Presence p = two_players();
  p.on_sample(sample(2, 1000 * kMs, 1, {0, 0, 0}));
  p.on_sample(sample(2, 6000 * kMs, 2, {500, 0, 0}));
  const Player& pl = *p.player(2);
  // Inside the gap: held at the first sample and faded, not sliding across.
  auto mid = pl.pose_at(3500 * kMs, p.settings());
  REQUIRE(mid);
  CHECK(mid->how == How::held);
  CHECK(mid->position[0] == doctest::Approx(0));
  CHECK(mid->fade == 0.0f);
  // Just after the pause the new sample shows at once and fully.
  auto after = pl.pose_at(6010 * kMs, p.settings());
  REQUIRE(after);
  CHECK(after->position[0] == doctest::Approx(500));
  CHECK(after->fade == 1.0f);
  // A gap just under the limit is still joined.
  p.on_sample(sample(2, 6400 * kMs, 3, {540, 0, 0}));
  auto joined = pl.pose_at(6200 * kMs, p.settings());
  REQUIRE(joined);
  CHECK(joined->how == How::interpolated);
  CHECK(joined->position[0] == doctest::Approx(520));
}

TEST_CASE("presence: join, leave and welcome") {
  Presence p;
  proto::Welcome w;
  w.player_number = 2;
  w.players = {{1, "One"}, {4, "Four"}};
  p.on_welcome(w);
  CHECK(p.self() == 2);
  CHECK(p.players() == std::vector<std::uint8_t>{1, 4});
  CHECK(p.player(1)->nickname() == "One");

  CHECK(p.apply(proto::Message{proto::PlayerJoined{3, "Three"}}));
  CHECK(p.players() == std::vector<std::uint8_t>{1, 3, 4});
  CHECK_FALSE(p.on_joined({2, "Me"}));  // own number
  CHECK_FALSE(p.on_joined({17, "Seventeen"}));

  CHECK(p.on_sample(sample(3, 100, 1, {0, 0, 0})) == SampleResult::buffered);
  CHECK(p.apply(proto::Message{proto::PlayerLeft{3, proto::LeaveReason::timed_out}}));
  CHECK(p.players() == std::vector<std::uint8_t>{1, 4});
  CHECK_FALSE(p.on_left({3, proto::LeaveReason::left}));
  // A late sample after the leave does not bring the player back.
  CHECK(p.on_sample(sample(3, 200, 2, {0, 0, 0})) == SampleResult::unknown_player);
  CHECK(p.on_sample(sample(2, 200, 2, {0, 0, 0})) == SampleResult::self);

  // Joining again starts afresh.
  p.on_sample(sample(1, 100, 1, {0, 0, 0}));
  p.on_joined({1, "One again"});
  CHECK(p.player(1)->samples().empty());
  CHECK(p.player(1)->nickname() == "One again");

  // Messages presence does not use are ignored.
  CHECK_FALSE(p.apply(proto::Message{proto::Ping{5}}));
}

TEST_CASE("presence: status flags are kept per player") {
  Presence p = two_players();
  CHECK(p.player(2)->flags() == 0);
  CHECK(p.apply(proto::Message{
      proto::PlayerStatus{2, std::uint8_t(proto::status::paused | proto::status::in_menu | 0x80)}}));
  CHECK(p.player(2)->has_flag(proto::status::paused));
  CHECK(p.player(2)->has_flag(proto::status::in_menu));
  CHECK_FALSE(p.player(2)->has_flag(proto::status::dead));
  CHECK(p.player(2)->flags() == 0x89);  // unknown bit kept
  CHECK_FALSE(p.on_status({3, proto::status::dead}));
}

TEST_CASE("presence: scene and age from the newest sample, and the scene filter") {
  Presence p;
  proto::Welcome w;
  w.player_number = 1;
  w.players = {{2, "Two"}, {3, "Three"}};
  p.on_welcome(w);
  CHECK_FALSE(p.same_scene(2, "spot00"));  // not known yet

  auto s = sample(2, 1000 * kMs, 5, {0, 0, 0});
  s.scene = "spot00";
  s.age = Age::child;
  p.on_sample(s);
  proto::PlayerSample only_state;
  only_state.player_number = 3;
  only_state.time = {1000 * kMs, 1};
  only_state.scene = "ydan";
  CHECK(p.on_sample(only_state) == SampleResult::state_only);

  CHECK(p.player(2)->scene() == "spot00");
  CHECK(p.player(2)->age() == Age::child);
  CHECK(p.same_scene(2, "spot00"));
  CHECK_FALSE(p.same_scene(3, "spot00"));
  CHECK(p.players_in_scene("spot00") == std::vector<std::uint8_t>{2});
  CHECK(p.players_in_scene("ydan") == std::vector<std::uint8_t>{3});

  // A late, older sample does not overwrite them; a newer one does.
  auto old = sample(2, 900 * kMs, 4, {0, 0, 0});
  old.scene = "ydan";
  old.age = Age::adult;
  p.on_sample(old);
  CHECK(p.player(2)->scene() == "spot00");
  CHECK(p.player(2)->age() == Age::child);
  auto newer = sample(2, 1100 * kMs, 6, {0, 0, 0});
  newer.scene = "ydan";
  p.on_sample(newer);
  CHECK(p.player(2)->scene() == "ydan");
  CHECK(p.player(2)->age() == Age::child);  // no age in it: kept
  CHECK(p.players_in_scene("ydan") == std::vector<std::uint8_t>{2, 3});
}

TEST_CASE("presence: velocity scale converts the velocity unit") {
  Settings st;
  st.velocity_scale = 50.0;  // e.g. velocity in metres, positions in 2 cm units
  Presence p = two_players(st);
  p.on_sample(sample(2, 0, 1, {0, 0, 0}, Vec3{1, 0, 0}));
  const auto pose = p.player(2)->pose_at(100 * kMs, p.settings());
  REQUIRE(pose);
  CHECK(pose->position[0] == doctest::Approx(5.0));
  CHECK(pose->velocity[0] == doctest::Approx(1.0));
}

namespace {

bool finite(const Vec3& v) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

bool unit(const Quat& q) {
  for (float c : q) {
    if (!std::isfinite(c)) return false;
  }
  const double n = double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] +
                   double(q[3]) * q[3];
  return std::fabs(n - 1.0) < 1e-5;
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kBig = std::numeric_limits<float>::max();

}  // namespace

TEST_CASE("presence: a scene change in a sample too late to draw is still taken") {
  Presence p = two_players();
  p.on_sample(sample(2, 1000 * kMs, 1, {0, 0, 0}));
  p.on_sample(sample(2, 1200 * kMs, 3, {2, 0, 0}));
  REQUIRE(p.shown(2, 1250 * kMs));  // shows 1150 ms
  auto late = sample(2, 1100 * kMs, 2, {1, 0, 0});
  late.scene = "ydan";
  late.age = Age::adult;
  CHECK(p.on_sample(late) == SampleResult::too_old);  // not drawn...
  CHECK(p.player(2)->scene() == "ydan");             // ...but its scene is used
  CHECK(p.player(2)->age() == Age::adult);
}

TEST_CASE("presence: a sender whose samples all arrive after the show delay is still shown") {
  Presence p = two_players();  // 100 ms delay; these arrive 130 ms after their time
  CHECK_FALSE(p.shown(2, 1130 * kMs));
  CHECK(p.on_sample(sample(2, 1000 * kMs, 1, {0, 0, 0}, Vec3{10, 0, 0})) ==
        SampleResult::buffered);
  auto pose = p.shown(2, 1146 * kMs);  // shows 1046, past the only sample
  REQUIRE(pose);
  CHECK(pose->how == How::extrapolated);
  CHECK(pose->position[0] == doctest::Approx(0.46));
  CHECK(p.on_sample(sample(2, 1016 * kMs, 2, {0.16f, 0, 0}, Vec3{10, 0, 0})) ==
        SampleResult::buffered);
  pose = p.shown(2, 1162 * kMs);
  REQUIRE(pose);
  CHECK(pose->how == How::extrapolated);
  CHECK(pose->position[0] == doctest::Approx(0.62));
  // A late one that falls between buffered samples already shown is dropped.
  CHECK(p.on_sample(sample(2, 1008 * kMs, 9, {9, 9, 9})) == SampleResult::too_old);
}

TEST_CASE("presence: scene and age follow the sequence across its wrap, not a clock jump") {
  Presence p = two_players();
  auto s = [](std::uint64_t t, std::uint32_t seq, const char* scene) {
    proto::PlayerSample x;
    x.player_number = 2;
    x.time = {t, seq};
    x.scene = scene;
    return x;
  };
  p.on_sample(s(1000 * kMs, 0xFFFF'FFFEu, "spot00"));
  // The sender's clock estimate jumps a day ahead for one sample.
  p.on_sample(s(86'400'000 * kMs, 0xFFFF'FFFFu, "spot01"));
  CHECK(p.player(2)->scene() == "spot01");
  // Back on time, and the sequence wraps to 0: still newer.
  p.on_sample(s(1100 * kMs, 0, "spot02"));
  CHECK(p.player(2)->scene() == "spot02");
  p.on_sample(s(1200 * kMs, 1, "spot03"));
  CHECK(p.player(2)->scene() == "spot03");
  // A late one from before the wrap does not overwrite it.
  p.on_sample(s(1050 * kMs, 0xFFFF'FFFDu, "ydan"));
  CHECK(p.player(2)->scene() == "spot03");
}

TEST_CASE("presence: buffer order across the sequence wrap and with equal times") {
  Presence p = two_players();
  CHECK(p.on_sample(sample(2, 116 * kMs, 0, {1, 0, 0})) == SampleResult::buffered);
  CHECK(p.on_sample(sample(2, 100 * kMs, 0xFFFF'FFFFu, {0, 0, 0})) == SampleResult::buffered);
  const auto& b = p.player(2)->samples();
  REQUIRE(b.size() == 2);
  CHECK(b[0].time.sequence == 0xFFFF'FFFFu);
  CHECK(b[1].time.sequence == 0);

  // Two samples with the same server time: no divide by zero, the later
  // sequence is the one joined to the next sample.
  Settings lin;
  lin.hermite = false;
  Presence q = two_players(lin);
  q.on_sample(sample(2, 100 * kMs, 1, {0, 0, 0}, Vec3{1, 0, 0}));
  q.on_sample(sample(2, 100 * kMs, 2, {4, 0, 0}, Vec3{1, 0, 0}));
  auto at = q.player(2)->pose_at(100 * kMs, q.settings());
  REQUIRE(at);
  CHECK(at->how == How::extrapolated);
  CHECK(at->position[0] == doctest::Approx(4));
  q.on_sample(sample(2, 200 * kMs, 3, {8, 0, 0}));
  at = q.player(2)->pose_at(150 * kMs, q.settings());
  REQUIRE(at);
  CHECK(at->how == How::interpolated);
  CHECK(at->position[0] == doctest::Approx(6));
  CHECK(finite(at->velocity));
}

TEST_CASE("presence: NaN, infinity and zero rotations are refused") {
  Presence p = two_players();
  CHECK(p.on_sample(sample(2, 0, 1, {kNaN, 0, 0})) == SampleResult::invalid);
  CHECK(p.on_sample(sample(2, 0, 2, {0, kInf, 0})) == SampleResult::invalid);
  CHECK(p.on_sample(sample(2, 0, 3, {0, 0, 0}, Vec3{0, 0, -kInf})) == SampleResult::invalid);
  auto r = sample(2, 0, 4, {0, 0, 0});
  r.rotation = Quat{0, 0, 0, 0};
  r.scene = "ydan";
  CHECK(p.on_sample(r) == SampleResult::invalid);
  r.rotation = Quat{0, kNaN, 0, 1};
  CHECK(p.on_sample(r) == SampleResult::invalid);
  r.rotation = Quat{kInf, 0, 0, 1};
  CHECK(p.on_sample(r) == SampleResult::invalid);
  CHECK(p.player(2)->samples().empty());
  CHECK_FALSE(p.player(2)->scene());  // nothing of a refused sample is used

  // A rotation that is not of length 1 is normalised when buffered, so a
  // held pose (not slerped) is still a unit rotation.
  r.rotation = Quat{0, 0, 0, 1e30f};
  CHECK(p.on_sample(r) == SampleResult::buffered);
  const auto pose = p.player(2)->pose_at(1000 * kMs, p.settings());
  REQUIRE(pose);
  REQUIRE(pose->rotation);
  CHECK(unit(*pose->rotation));
  CHECK((*pose->rotation)[3] == doctest::Approx(1.0));
}

TEST_CASE("presence: huge finite values from a sender still give a finite pose") {
  Presence p = two_players();
  auto a = sample(2, 0, 1, {kBig, -kBig, kBig}, Vec3{kBig, kBig, -kBig});
  auto b = sample(2, 1, 2, {-kBig, kBig, -kBig}, Vec3{-kBig, kBig, kBig});  // 1 us later
  a.rotation = Quat{kBig, kBig, kBig, kBig};
  b.rotation = Quat{-kBig, kBig, -kBig, kBig};
  REQUIRE(p.on_sample(a) == SampleResult::buffered);
  REQUIRE(p.on_sample(b) == SampleResult::buffered);
  const Player& pl = *p.player(2);
  for (Settings st : {Settings{}, [] {
                        Settings s;
                        s.hermite = false;
                        return s;
                      }()}) {
    for (std::uint64_t at : {0ull, 1ull, 2ull, 100'000ull, 151'000ull, 10'000'000ull}) {
      const auto pose = pl.pose_at(at, st);
      REQUIRE(pose);
      CHECK(finite(pose->position));
      CHECK(finite(pose->velocity));
      REQUIRE(pose->rotation);
      CHECK(unit(*pose->rotation));
    }
  }
  // Interpolation a third of the way through the 1 us gap.
  Presence q = two_players();
  q.on_sample(sample(2, 0, 1, {kBig, 0, 0}, Vec3{kBig, 0, 0}));
  q.on_sample(sample(2, 3, 2, {-kBig, 0, 0}, Vec3{kBig, 0, 0}));
  const auto mid = q.player(2)->pose_at(1, q.settings());
  REQUIRE(mid);
  CHECK(finite(mid->position));
  CHECK(finite(mid->velocity));
}

TEST_CASE("presence: slerp with bad input or nearly the same rotations") {
  const Quat id{0, 0, 0, 1};
  CHECK(unit(slerp(Quat{kNaN, 0, 0, 1}, id, 0.5)));
  CHECK(unit(slerp(Quat{kInf, kInf, 0, 0}, Quat{0, kInf, 0, 0}, 0.5)));
  CHECK(unit(slerp(Quat{0, 0, 0, 0}, Quat{0, 0, 0, 0}, 0.5)));
  CHECK(unit(slerp(Quat{kBig, kBig, kBig, kBig}, Quat{-kBig, kBig, kBig, -kBig}, 0.3)));
  CHECK(quat_mismatch(slerp(id, about_y(1.0), std::nan("")), id) < 1e-6);
  CHECK(quat_mismatch(slerp(Quat{0, 0, 0, 0}, about_y(1.0), 1.0), about_y(1.0)) < 1e-6);
  // Nearly the same (and nearly opposite signs of the same): unit, between.
  const Quat a = about_y(1e-4);
  const Quat b = about_y(2e-4);
  const Quat nb{-b[0], -b[1], -b[2], -b[3]};
  // (Compared by component: at angles this small, a dot product cannot
  // tell them apart.)
  for (const Quat& to : {b, nb}) {
    const Quat m = slerp(a, to, 0.5);
    CHECK(unit(m));
    CHECK(m[1] == doctest::Approx(std::sin(0.75e-4)).epsilon(1e-3));
    CHECK(m[3] == doctest::Approx(1.0));  // same sign as a: the short way
  }
  const Quat same = slerp(a, a, 0.5);
  CHECK(same[1] == doctest::Approx(a[1]).epsilon(1e-5));
  // Exactly opposite rotations by half a turn: still a unit result.
  CHECK(unit(slerp(id, Quat{0, 1, 0, 0}, 0.5)));
}

TEST_CASE("presence: a velocity scale that is not finite is taken as 1") {
  for (double bad : {std::nan(""), double(kInf), -1.0, 0.0}) {
    Settings st;
    st.velocity_scale = bad;
    Presence p = two_players(st);
    p.on_sample(sample(2, 0, 1, {0, 0, 0}, Vec3{1, 0, 0}));
    const auto pose = p.player(2)->pose_at(100 * kMs, p.settings());
    REQUIRE(pose);
    CHECK(pose->position[0] == doctest::Approx(0.1));
  }
}

TEST_CASE("presence: unknown player numbers are ignored") {
  Presence p = two_players();
  for (std::uint8_t n : {std::uint8_t{0}, std::uint8_t{3}, std::uint8_t{5}, std::uint8_t{255}}) {
    CHECK(p.on_sample(sample(n, 0, 1, {0, 0, 0})) == SampleResult::unknown_player);
    CHECK_FALSE(p.on_left(proto::PlayerLeft{n}));
    CHECK_FALSE(p.on_status(proto::PlayerStatus{n, 1}));
    CHECK_FALSE(p.shown(n, 1000 * kMs));
    CHECK_FALSE(p.player(n));
  }
  proto::PlayerJoined j;
  j.player_number = 17;
  CHECK_FALSE(p.on_joined(j));
  j.player_number = 0;
  CHECK_FALSE(p.on_joined(j));
  CHECK(p.players() == std::vector<std::uint8_t>{2});
  CHECK(p.on_sample(sample(1, 0, 1, {0, 0, 0})) == SampleResult::self);
}

TEST_CASE("presence holds 16 players (protocol 11): a WELCOME of 15 and joins up to 16") {
  Presence p;
  proto::Welcome w;
  w.player_number = 16;
  for (std::uint8_t n = 1; n <= 12; ++n) w.players.push_back({n, "p" + std::to_string(n)});
  p.on_welcome(w);
  CHECK(p.players().size() == 12);
  CHECK(p.on_joined({13, "p13"}));
  CHECK(p.on_joined({14, "p14"}));
  CHECK(p.on_joined({15, "p15"}));
  CHECK_FALSE(p.on_joined({16, "Me"}));  // own number
  CHECK_FALSE(p.on_joined({17, "p17"}));
  const auto all = p.players();
  REQUIRE(all.size() == 15);
  CHECK(all.front() == 1);
  CHECK(all.back() == 15);
  CHECK(p.on_left(proto::PlayerLeft{15}));
  CHECK(p.players().size() == 14);
}
