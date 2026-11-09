#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "fake_memory.hpp"

using namespace coop::host;
using namespace coop::host::test;
namespace proto = coop::protocol;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Rotation by `angle` about the unit axis (x, y, z), row-major, v' = M v.
Mat3 axis_angle(double x, double y, double z, double angle) {
  const double len = std::sqrt(x * x + y * y + z * z);
  x /= len;
  y /= len;
  z /= len;
  const double c = std::cos(angle), s = std::sin(angle), t = 1.0 - c;
  auto f = [](double v) { return static_cast<float>(v); };
  return Mat3{{{f(t * x * x + c), f(t * x * y - s * z), f(t * x * z + s * y)},
               {f(t * x * y + s * z), f(t * y * y + c), f(t * y * z - s * x)},
               {f(t * x * z - s * y), f(t * y * z + s * x), f(t * z * z + c)}}};
}

// Rotates v by the unit quaternion q.
Vec3 rotate(const Quat& q, const Vec3& v) {
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  // v' = v + 2w (u x v) + 2 u x (u x v), u = (x, y, z)
  const double cx = y * v[2] - z * v[1], cy = z * v[0] - x * v[2], cz = x * v[1] - y * v[0];
  const double dx = y * cz - z * cy, dy = z * cx - x * cz, dz = x * cy - y * cx;
  return {static_cast<float>(v[0] + 2 * (w * cx + dx)), static_cast<float>(v[1] + 2 * (w * cy + dy)),
          static_cast<float>(v[2] + 2 * (w * cz + dz))};
}

Vec3 apply(const Mat3& m, const Vec3& v) {
  Vec3 r{};
  for (std::size_t i = 0; i < 3; ++i) r[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
  return r;
}

void check_same_rotation(const Mat3& m, const Quat& q) {
  const double len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  CHECK(len == doctest::Approx(1.0).epsilon(1e-5));
  CHECK(q[3] >= 0.0f);
  for (const Vec3 v : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}, Vec3{0.3f, -0.7f, 0.2f}}) {
    const Vec3 a = apply(m, v), b = rotate(q, v);
    for (std::size_t i = 0; i < 3; ++i) CHECK(a[i] == doctest::Approx(b[i]).epsilon(1e-4).scale(1));
  }
}

}  // namespace

TEST_CASE("host reader: every field of a healthy game") {
  FakeMemory m;
  make_game(m);
  PlayerReader reader(m);
  const auto r = reader.read();
  REQUIRE(r);
  CHECK(r->position == Vec3{1.0f, 2.0f, 3.0f});
  REQUIRE(r->rotation);
  CHECK(*r->rotation == Quat{0.0f, 0.0f, 0.0f, 1.0f});
  CHECK(r->age == proto::Age::adult);
  CHECK(r->scene == std::string("spot00"));
  CHECK(r->event_running == false);
}

TEST_CASE("host reader: child, event running, and the actor's matrix") {
  FakeMemory m;
  const Layout l = make_game(m);
  m.put_u8(l.game_ui + addr::kGameUiIsChild, 1);
  m.put_u64(l.events + addr::kEventRunning, 0x10000);
  const Mat3 turn = axis_angle(0, 1, 0, kPi / 2);
  put_matrix(m, l.actor1, turn);
  PlayerReader reader(m);
  const auto r = reader.read();
  REQUIRE(r);
  CHECK(r->age == proto::Age::child);
  CHECK(r->event_running == true);
  REQUIRE(r->rotation);
  check_same_rotation(turn, *r->rotation);
  CHECK((*r->rotation)[1] == doctest::Approx(std::sqrt(0.5)));
}

TEST_CASE("host reader: position falls back to the actor when the GameUI copy is not valid") {
  FakeMemory m;
  const Layout l = make_game(m);
  m.put_u16(l.game_ui + addr::kGameUiPoseFlags, 0);
  m.put_vec3(l.actor1 + addr::kActorPosition, {5.0f, 6.0f, 7.0f});
  PlayerReader reader(m);
  CHECK(reader.read_position() == Vec3{5.0f, 6.0f, 7.0f});

  // A NaN in the GameUI copy also falls back to the actor.
  m.put_u16(l.game_ui + addr::kGameUiPoseFlags, addr::kGameUiPoseValidBit);
  m.put_f32(l.game_ui + addr::kGameUiPosition + 4, std::numeric_limits<float>::quiet_NaN());
  CHECK(reader.read_position() == Vec3{5.0f, 6.0f, 7.0f});
}

TEST_CASE("host reader: the link choice and the id test") {
  FakeMemory m;
  const Layout l = make_game(m);
  PlayerReader reader(m);
  CHECK(reader.find_player_actor() == l.actor1);

  SUBCASE("first actor not present: the second link") {
    m.put_u32(l.actor1 + addr::kActorState, 9);
    put_actor(m, l.link2, l.holder2, l.actor2, 3, addr::kActorStatePresent, {0, 0, 0});
    CHECK(reader.find_player_actor() == l.actor2);
  }
  SUBCASE("a stale holder is refused") {
    m.put_u32(l.holder1 + addr::kHolderId, 8);
    CHECK_FALSE(reader.find_player_actor());
  }
  SUBCASE("an empty link is refused") {
    m.put_u32(l.link1 + addr::kLinkId, addr::kEmptyId);
    CHECK_FALSE(reader.find_player_actor());
  }
  SUBCASE("a table too short for index 15") {
    m.put_u32(l.table + addr::kTableCount, 0x0f);
    CHECK_FALSE(reader.find_player_actor());
  }
  SUBCASE("the manager's +0x30 not set") {
    m.put_u64(l.manager + addr::kManagerMustBeSet, 0);
    CHECK_FALSE(reader.find_player_actor());
  }
  SUBCASE("a null pointer on the way") {
    m.put_u64(l.array + addr::kPlayerManagerSlot, 0);
    CHECK_FALSE(reader.find_player_actor());
  }
  SUBCASE("unmapped memory on the way") {
    m.erase(l.holder1 + addr::kHolderActor, 8);
    CHECK_FALSE(reader.find_player_actor());
  }
}

TEST_CASE("host reader: bad positions give no reading") {
  FakeMemory m;
  const Layout l = make_game(m);
  PlayerReader reader(m);
  m.put_u16(l.game_ui + addr::kGameUiPoseFlags, 0);  // only the actor is left

  SUBCASE("NaN") {
    m.put_f32(l.actor1 + addr::kActorPosition, std::numeric_limits<float>::quiet_NaN());
    CHECK_FALSE(reader.read());
  }
  SUBCASE("infinity") {
    m.put_f32(l.actor1 + addr::kActorPosition + 8, std::numeric_limits<float>::infinity());
    CHECK_FALSE(reader.read());
  }
  SUBCASE("huge") {
    m.put_f32(l.actor1 + addr::kActorPosition + 4, 3.0e7f);
    CHECK_FALSE(reader.read());
  }
  SUBCASE("unreadable") {
    m.erase(l.actor1 + addr::kActorPosition + 8, 1);
    CHECK_FALSE(reader.read());
  }
  SUBCASE("no player at all") {
    m.put_u64(FakeMemory::kBase + addr::kSceneModulePtr, 0);
    CHECK_FALSE(reader.read());
  }
}

TEST_CASE("host reader: bad side fields are dropped, the reading stays") {
  FakeMemory m;
  const Layout l = make_game(m);
  PlayerReader reader(m);

  SUBCASE("age that is not 0 or 1") {
    m.put_u8(l.game_ui + addr::kGameUiIsChild, 7);
    const auto r = reader.read();
    REQUIRE(r);
    CHECK_FALSE(r->age);
  }
  SUBCASE("a matrix that is not a rotation") {
    put_matrix(m, l.actor1, Mat3{{{2, 0, 0}, {0, 2, 0}, {0, 0, 2}}});
    const auto r = reader.read();
    REQUIRE(r);
    CHECK_FALSE(r->rotation);
  }
  SUBCASE("a NaN in the matrix") {
    m.put_f32(l.actor1 + addr::kActorRotation + 16, std::numeric_limits<float>::quiet_NaN());
    const auto r = reader.read();
    REQUIRE(r);
    CHECK_FALSE(r->rotation);
  }
  SUBCASE("an unterminated scene string") {
    std::string full(64, 'a');
    m.put(l.path, full.data(), full.size());
    const auto r = reader.read();
    REQUIRE(r);
    CHECK_FALSE(r->scene);
  }
  SUBCASE("a scene string past its capacity is not read") {
    put_scene_path(m, l, "spot01", 3);
    CHECK_FALSE(reader.read_scene());
  }
  SUBCASE("an unreadable scene string") {
    m.erase(l.path + 10, 1);
    CHECK_FALSE(reader.read_scene());
  }
  SUBCASE("Banc +0x38 null gives no name, as the game") {
    m.put_u64(l.banc + addr::kBancMustBeSet, 0);
    CHECK_FALSE(reader.read_scene());
  }
  SUBCASE("a scene with a control byte") {
    put_scene_path(m, l, "spot\x01");
    CHECK_FALSE(reader.read_scene());
  }
  SUBCASE("no event manager") {
    m.put_u64(FakeMemory::kBase + addr::kEventManagerPtr, 0);
    const auto r = reader.read();
    REQUIRE(r);
    CHECK_FALSE(r->event_running);
  }
}

TEST_CASE("host reader: scene name from a path") {
  CHECK(scene_from_path("spot00") == std::string("spot00"));
  CHECK(scene_from_path("a/b/spot00.x.y") == std::string("spot00"));
  CHECK(scene_from_path("a\\b\\ydan01.x") == std::string("ydan01"));
  CHECK(scene_from_path("a.b/spot02") == std::string("spot02"));
  CHECK_FALSE(scene_from_path(""));
  CHECK_FALSE(scene_from_path("a/b/"));
  CHECK_FALSE(scene_from_path("a/.x"));
  CHECK_FALSE(scene_from_path(std::string(65, 's')));
  CHECK(scene_from_path(std::string(64, 's')));
  CHECK_FALSE(scene_from_path("sp\xc3\xa9t"));
}

TEST_CASE("host reader: matrix to quaternion") {
  SUBCASE("identity") {
    CHECK(matrix_to_quaternion(Mat3{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}) == Quat{0, 0, 0, 1});
  }
  SUBCASE("90 degrees about z") {
    const auto q = matrix_to_quaternion(axis_angle(0, 0, 1, kPi / 2));
    REQUIRE(q);
    CHECK((*q)[2] == doctest::Approx(std::sqrt(0.5)));
    CHECK((*q)[3] == doctest::Approx(std::sqrt(0.5)));
  }
  SUBCASE("exactly 180 degrees about each axis") {
    const auto qx = matrix_to_quaternion(Mat3{{{1, 0, 0}, {0, -1, 0}, {0, 0, -1}}});
    const auto qy = matrix_to_quaternion(Mat3{{{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}}});
    const auto qz = matrix_to_quaternion(Mat3{{{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}}});
    REQUIRE(qx);
    REQUIRE(qy);
    REQUIRE(qz);
    CHECK(*qx == Quat{1, 0, 0, 0});
    CHECK(*qy == Quat{0, 1, 0, 0});
    CHECK(*qz == Quat{0, 0, 1, 0});
  }
  SUBCASE("near 180 degrees about a skew axis") {
    for (const double angle : {kPi - 1e-2, kPi - 1e-4, kPi, kPi + 1e-4}) {
      const Mat3 m = axis_angle(1, 2, 3, angle);
      const auto q = matrix_to_quaternion(m);
      REQUIRE(q);
      check_same_rotation(m, *q);
      // The axis survives: (x, y, z) is along (1, 2, 3).
      CHECK((*q)[1] == doctest::Approx(2 * (*q)[0]).epsilon(1e-3));
      CHECK((*q)[2] == doctest::Approx(3 * (*q)[0]).epsilon(1e-3));
    }
  }
  SUBCASE("many axes and angles") {
    const double axes[][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {-1, 2, 0.5}, {0.2, -0.3, -1}};
    for (const auto& a : axes) {
      for (int i = -12; i <= 12; ++i) {
        const Mat3 m = axis_angle(a[0], a[1], a[2], i * kPi / 12);
        const auto q = matrix_to_quaternion(m);
        REQUIRE(q);
        check_same_rotation(m, *q);
      }
    }
  }
  SUBCASE("not a rotation") {
    CHECK_FALSE(matrix_to_quaternion(Mat3{{{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}));  // a mirror
    CHECK_FALSE(matrix_to_quaternion(Mat3{{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}}));
    CHECK_FALSE(matrix_to_quaternion(Mat3{{{1, 0.5f, 0}, {0, 1, 0}, {0, 0, 1}}}));  // sheared
    CHECK_FALSE(matrix_to_quaternion(
        Mat3{{{std::numeric_limits<float>::infinity(), 0, 0}, {0, 1, 0}, {0, 0, 1}}}));
  }
}
