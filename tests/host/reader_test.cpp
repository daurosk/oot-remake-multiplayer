#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "coop/protocol/codec.hpp"
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

namespace {

// The AS controller of the player actor, with `name` playing in slot 0.
struct AnimationLayout {
  std::uint64_t components = 0x80100000;
  std::uint64_t as = 0x80110000;
  std::uint64_t inner = 0x80120000;
  std::uint64_t controller = 0x80130000;
  std::uint64_t slots = 0x80140000;
  std::uint64_t slot0 = 0x80150000;
  std::uint64_t name = 0x80160000;
};

AnimationLayout put_animation(FakeMemory& m, const Layout& l, const std::string& name,
                              float frame, float rate) {
  const AnimationLayout a;
  m.put_u64(l.actor1 + addr::kActorComponents, a.components);
  m.put_u64(a.components + addr::kComponentsAs, a.as);
  m.put_u64(a.as + addr::kAsComponentInner, a.inner);
  m.put_u64(a.inner + addr::kAsInnerController, a.controller);
  m.put_u32(a.controller + addr::kAsSlotCount, 3);
  m.put_u64(a.controller + addr::kAsSlotArray, a.slots);
  m.put_u64(a.slots, a.slot0);
  m.put_u64(a.slot0 + addr::kAsSlotCommandName, a.name);
  m.put_f32(a.slot0 + addr::kAsSlotFrame, frame);
  m.put_f32(a.slot0 + addr::kAsSlotRate, rate);
  // Mapped to the next 16-byte boundary, as real memory is mapped in pages.
  std::string bytes = name;
  bytes.resize((name.size() + 16) / 16 * 16, '\0');
  m.put(a.name, bytes.data(), bytes.size());
  return a;
}

}  // namespace

TEST_CASE("host reader: the AS command in the main slot") {
  FakeMemory m;
  const Layout l = make_game(m);
  PlayerReader reader(m);

  SUBCASE("no AS controller: no animation, the reading stays") {
    const auto r = reader.read();
    REQUIRE(r);
    CHECK_FALSE(r->animation);
  }
  SUBCASE("a healthy command") {
    put_animation(m, l, "Wait", 12.5f, 1.0f);
    const auto r = reader.read();
    REQUIRE(r);
    REQUIRE(r->animation);
    CHECK(r->animation->name == "Wait");
    CHECK(r->animation->frame == 12.5f);
    CHECK(r->animation->rate == 1.0f);
  }
  SUBCASE("a name that ends right at a page end still reads") {
    const auto a = put_animation(m, l, "x", 0.0f, 1.0f);
    // Move the name so its terminator is the last byte before unmapped memory.
    const std::string name = "Run_Loop_Long_Name";
    const std::uint64_t at = 0x80171000 - (name.size() + 1);
    m.put(at, name.c_str(), name.size() + 1);
    m.put_u64(a.slot0 + addr::kAsSlotCommandName, at);
    const auto anim = reader.read_animation();
    REQUIRE(anim);
    CHECK(anim->name == name);
  }
  SUBCASE("the longest name allowed, and one more") {
    put_animation(m, l, std::string(proto::kMaxAnimationChars, 'a'), 0.0f, 1.0f);
    REQUIRE(reader.read_animation());
    CHECK(reader.read_animation()->name.size() == proto::kMaxAnimationChars);
    put_animation(m, l, std::string(proto::kMaxAnimationChars + 1, 'a'), 0.0f, 1.0f);
    CHECK_FALSE(reader.read_animation());
  }
  SUBCASE("bad parts give no animation") {
    const auto a = put_animation(m, l, "Wait", 3.0f, 1.0f);
    REQUIRE(reader.read_animation());
    SUBCASE("empty name: nothing plays") {
      m.put_u8(a.name, 0);
      CHECK_FALSE(reader.read_animation());
    }
    SUBCASE("a control byte in the name") {
      m.put_u8(a.name + 1, 0x07);
      CHECK_FALSE(reader.read_animation());
    }
    SUBCASE("an unterminated name") {
      m.erase(a.name + 4, 1);
      CHECK_FALSE(reader.read_animation());
    }
    SUBCASE("a null name pointer") {
      m.put_u64(a.slot0 + addr::kAsSlotCommandName, 0);
      CHECK_FALSE(reader.read_animation());
    }
    SUBCASE("no slots") {
      m.put_u32(a.controller + addr::kAsSlotCount, 0);
      CHECK_FALSE(reader.read_animation());
    }
    SUBCASE("a negative slot count") {
      m.put_u32(a.controller + addr::kAsSlotCount, 0xffffffffu);
      CHECK_FALSE(reader.read_animation());
    }
    SUBCASE("no controller") {
      m.put_u64(a.inner + addr::kAsInnerController, 0);
      CHECK_FALSE(reader.read_animation());
    }
    SUBCASE("a NaN frame") {
      m.put_f32(a.slot0 + addr::kAsSlotFrame, std::numeric_limits<float>::quiet_NaN());
      CHECK_FALSE(reader.read_animation());
    }
    SUBCASE("an absurd rate") {
      m.put_f32(a.slot0 + addr::kAsSlotRate, 1.0e9f);
      CHECK_FALSE(reader.read_animation());
    }
    SUBCASE("an unreadable rate") {
      m.erase(a.slot0 + addr::kAsSlotRate, 4);
      CHECK_FALSE(reader.read_animation());
    }
    SUBCASE("the player actor is gone") {
      m.put_u32(l.actor1 + addr::kActorState, 7);
      CHECK_FALSE(reader.read_animation());
    }
  }
}

namespace {

// A GameDataMgr with only the String64 table (docs/flags.md layout, type 16
// at M + 0x610: count, entries, hash slots, slot count; entries of 0x108
// with char* +0x68 and s32 size +0x70), offsets written out again here.
constexpr std::uint64_t kGameData = 0x90000000, kStrEntries = 0x90100000,
                        kStrSlots = 0x90200000, kStrText = 0x90300000;
constexpr std::uint32_t kStrCap = 17;

void put_strings(FakeMemory& m, const std::vector<std::pair<std::uint32_t, std::string>>& flags) {
  m.put_u64(FakeMemory::kBase + 0x4e842a8, kGameData);
  m.put_u32(kGameData + 0x610, static_cast<std::uint32_t>(flags.size()));
  m.put_u64(kGameData + 0x618, kStrEntries);
  m.put_u64(kGameData + 0x620, kStrSlots);
  m.put_u32(kGameData + 0x62c, kStrCap);
  for (std::uint32_t i = 0; i < kStrCap; ++i) m.put_u64(kStrSlots + 8 * i, 0);
  for (std::uint32_t i = 0; i < flags.size(); ++i) {
    const auto& [hash, text] = flags[i];
    std::uint32_t s = hash % kStrCap;
    while (*read_u32(m, kStrSlots + 8 * s) != 0) s = (s + 1) % kStrCap;
    m.put_u32(kStrSlots + 8 * s, hash);
    m.put_u32(kStrSlots + 8 * s + 4, i);
    const std::uint64_t entry = kStrEntries + 0x108ull * i;
    const std::uint64_t buffer = kStrText + 0x40ull * i;
    m.put_u64(entry + 0x68, buffer);
    m.put_u32(entry + 0x70, 64);
    std::string bytes = text;
    bytes.resize(64, '\0');
    m.put(buffer, bytes.data(), bytes.size());
  }
}

// The player's EquipmentUser (component table +0x200): 8 slots of 0x18 at
// +0x20 (holder +8, id +0x10), a state byte per slot at +0x13bc.
constexpr std::uint64_t kComponents = 0x91000000, kUser = 0x91100000,
                        kEquipHolders = 0x91200000, kEquipActors = 0x91300000;

void put_user(FakeMemory& m, const Layout& l) {
  m.put_u64(l.actor1 + addr::kActorComponents, kComponents);
  m.put_u64(kComponents + 0x200, kUser);
  for (std::uint32_t i = 0; i < 8; ++i) {
    m.put_u64(kUser + 0x20 + 0x18 * i + 8, kEquipHolders + 0x100 * i);
    m.put_u32(kUser + 0x20 + 0x18 * i + 0x10, 0xffffffffu);
    m.put_u8(kUser + 0x13bc + i, 0);
  }
}

// The 13 armor links after the 8 hand slots, each read and empty, as the
// game has them: without this a test's other links are not readable.
// Int flags of the GameData manager (its Int table, as GameFlags reads it).
constexpr std::uint64_t kIntEntries = 0x90400000, kIntSlots = 0x90500000;
constexpr std::uint32_t kIntCap = 17;
void put_ints(FakeMemory& m, const std::vector<std::pair<std::string, std::int32_t>>& flags) {
  m.put_u64(FakeMemory::kBase + 0x4e842a8, kGameData);
  m.put_u32(kGameData + 0xd0, static_cast<std::uint32_t>(flags.size()));
  m.put_u64(kGameData + 0xd8, kIntEntries);
  m.put_u64(kGameData + 0xe0, kIntSlots);
  m.put_u32(kGameData + 0xec, kIntCap);
  for (std::uint32_t i = 0; i < kIntCap; ++i) m.put_u64(kIntSlots + 8 * i, 0);
  for (std::uint32_t i = 0; i < flags.size(); ++i) {
    const std::uint32_t hash = proto::flag_hash(flags[i].first);
    std::uint32_t s = hash % kIntCap;
    while (*read_u32(m, kIntSlots + 8 * s) != 0) s = (s + 1) % kIntCap;
    m.put_u32(kIntSlots + 8 * s, hash);
    m.put_u32(kIntSlots + 8 * s + 4, i);
    m.put_u32(kIntEntries + 0x28ull * i + 0x1c, static_cast<std::uint32_t>(flags[i].second));
  }
}

void put_links(FakeMemory& m) {
  for (std::uint32_t a = 0; a < addr::kArmorLinkCount; ++a) {
    const std::uint32_t link = 8 + a;
    m.put_u64(kUser + 0x20 + 0x18 * link + 8, kEquipHolders + 0x100 * link);
    m.put_u32(kUser + 0x20 + 0x18 * link + 0x10, 0xffffffffu);
  }
}

void put_equipped(FakeMemory& m, std::uint32_t slot, std::uint32_t id, std::uint32_t state,
                  std::uint8_t bits) {
  const std::uint64_t holder = kEquipHolders + 0x100 * slot;
  const std::uint64_t actor = kEquipActors + 0x1000 * slot;
  m.put_u32(kUser + 0x20 + 0x18 * slot + 0x10, id);
  m.put_u64(holder + 0x40, actor);
  m.put_u32(holder + 0x48, id);
  m.put_u32(actor + 0x2c, state);
  m.put_u8(kUser + 0x13bc + slot, bits);
}

}  // namespace

TEST_CASE("host reader: equipment flag hashes are the names' hashes") {
  CHECK(addr::kEquipWeapon == proto::flag_hash("PlayerStatus.EquipWeapon"));
  CHECK(addr::kEquipShield == proto::flag_hash("PlayerStatus.EquipShield"));
  CHECK(addr::kEquipArmor == proto::flag_hash("PlayerStatus.EquipArmor"));
  CHECK(addr::kEquipBoot == proto::flag_hash("PlayerStatus.EquipBoot"));
  CHECK(addr::kEquipHeadArmor == proto::flag_hash("PlayerStatus.EquipHeadArmor"));
  CHECK(addr::kEquipHandArmor == proto::flag_hash("PlayerStatus.EquipHandArmor"));
  CHECK(addr::kEquipOcarina == proto::flag_hash("PlayerStatus.EquipOcarina"));
  CHECK(addr::kEquipArrow == proto::flag_hash("PlayerStatus.EquipArrow"));
}

TEST_CASE("host reader: what the player wears") {
  FakeMemory m;
  const Layout l = make_game(m);
  PlayerReader reader(m);

  SUBCASE("no game data and no equipment user: no equipment, the reading stays") {
    const auto r = reader.read();
    REQUIRE(r);
    CHECK_FALSE(r->equipment);
  }
  SUBCASE("names and drawn slots") {
    put_strings(m, {{addr::kEquipWeapon, "Weapon_Sword_Master"},
                    {addr::kEquipShield, "Weapon_Shield_Hylian"},
                    {addr::kEquipArmor, "Armor_Adult_Goron"},
                    {addr::kEquipBoot, "Armor_Adult_HoverBoot"},
                    {addr::kEquipHeadArmor, ""},
                    {addr::kEquipOcarina, "Item_Ocarina_Time"}});
    put_user(m, l);
    put_equipped(m, 0, 40, 4, 0x2);  // sword drawn
    put_equipped(m, 2, 41, 6, 0x0);  // a carried piece kept away (state 6)
    put_equipped(m, 4, 42, 8, 0x2);  // drawn, but its actor is gone
    put_equipped(m, 5, 43, 4, 0x2);
    m.put_u32(kEquipHolders + 0x100 * 5 + 0x48, 99);  // a stale holder
    const auto r = reader.read();
    REQUIRE(r);
    REQUIRE(r->equipment);
    const proto::Equipment& e = *r->equipment;
    CHECK(e.weapon == "Weapon_Sword_Master");
    CHECK(e.shield == "Weapon_Shield_Hylian");
    CHECK(e.armor == "Armor_Adult_Goron");
    CHECK(e.boots == "Armor_Adult_HoverBoot");
    CHECK(e.head == "");
    CHECK_FALSE(e.hand);  // not in the table
    CHECK(e.ocarina == "Item_Ocarina_Time");
    CHECK_FALSE(e.arrow);
    CHECK(e.drawn == 0x01);
    CHECK(e.lit == 0x00);  // the sword plays no Chemical_Fire
  }
  SUBCASE("a lit Deku stick in hand (issue 248): its ELink user plays Chemical_Fire") {
    put_user(m, l);
    put_equipped(m, 0, 40, 4, 0x2);  // the stick, drawn
    const std::uint64_t stick = kEquipActors;
    const std::uint64_t table = 0x7100000, xlink = 0x7110000, user = 0x7120000;
    const std::uint64_t event = 0x7130000, call = 0x7140000, key = 0x7150000;
    const std::uint32_t back = 0x10;  // the link's place in its event
    m.put_u64(stick + 0x270, table);
    m.put_u64(table + 0x68, xlink);
    m.put_u64(xlink + 0x70, user);
    m.put_u32(user + 0x3c, back);
    m.put_u64(event + back + 8, user + 0x28);  // the only event: its next link is the head
    m.put_u64(event + 0x30, call);
    m.put_u64(call, key);
    SUBCASE("burning") {
      m.put_u64(user + 0x30, event + back);
      m.put(key, "Chemical_Fire", 14);
      CHECK(reader.read_equipment()->lit == 0x01);
      m.put_u8(kUser + 0x13bc, 0x4);  // put away: not looked at
      CHECK(reader.read_equipment()->lit == 0x00);
    }
    SUBCASE("another effect only (the ignite flash) is not lit") {
      m.put_u64(user + 0x30, event + back);
      m.put(key, "Chemical_FireStart", 19);
      CHECK(reader.read_equipment()->lit == 0x00);
    }
    SUBCASE("nothing playing") {
      m.put_u64(user + 0x30, user + 0x28);
      CHECK(reader.read_equipment()->lit == 0x00);
    }
  }
  SUBCASE("bad strings are dropped") {
    put_strings(m, {{addr::kEquipWeapon, "Weapon_Sword_Kokiri"}, {addr::kEquipShield, "x"}});
    const std::uint64_t shield_text = kStrText + 0x40;
    SUBCASE("a control byte") {
      m.put_u8(shield_text, 0x07);
      CHECK_FALSE(reader.read_equipment()->shield);
    }
    SUBCASE("no terminator within the buffer") {
      std::string full(64, 'a');
      m.put(shield_text, full.data(), full.size());
      CHECK_FALSE(reader.read_equipment()->shield);
    }
    SUBCASE("a size that is not a String64") {
      m.put_u32(kStrEntries + 0x108 + 0x70, 4096);
      CHECK_FALSE(reader.read_equipment()->shield);
    }
    CHECK(reader.read_equipment()->weapon == "Weapon_Sword_Kokiri");
  }
  SUBCASE("drawn alone, when the game data is not up") {
    put_user(m, l);
    put_equipped(m, 3, 50, 4, 0x2);
    const auto e = reader.read_equipment();
    REQUIRE(e);
    CHECK(e->drawn == 0x08);
    CHECK_FALSE(e->weapon);
  }
  SUBCASE("the held item is the name of the piece in slot 3") {
    put_user(m, l);
    CHECK(reader.read_equipment()->held == "");  // the slot is empty
    put_equipped(m, 3, 50, 6, 0x0);  // put away: the game keeps it in state 6
    const std::uint64_t piece = kEquipActors + 0x1000 * 3;
    const std::uint64_t text = piece + 0x800;
    const std::string name = "Weapon_Range_SlingShot";
    m.put_u64(piece + 0x260, text);
    const std::string room(80, char{0});  // the reader takes a whole name's bytes
    m.put(text, room.data(), room.size());
    m.put(text, name.c_str(), name.size() + 1);
    CHECK(reader.read_equipment()->held == name);
    SUBCASE("a name that is not text is not known") {
      m.put_u8(text + 3, 0x07);
      CHECK_FALSE(reader.read_equipment()->held);
    }
    SUBCASE("a name with no end is not known") {
      const std::string full(80, 'a');
      m.put(text, full.data(), full.size());
      CHECK_FALSE(reader.read_equipment()->held);
    }
    SUBCASE("a piece that is gone, or a stale holder, is nothing held") {
      m.put_u32(piece + 0x2c, 8);
      CHECK(reader.read_equipment()->held == "");
      m.put_u32(piece + 0x2c, 4);
      m.put_u32(kEquipHolders + 0x100 * 3 + 0x48, 99);
      CHECK(reader.read_equipment()->held == "");
    }
  }
  SUBCASE("the other hand slots are read the same way, each its own") {
    put_user(m, l);
    const auto name_in = [&](std::uint32_t slot, const std::string& name) {
      put_equipped(m, slot, 60 + slot, 4, 0x0);
      const std::uint64_t piece = kEquipActors + 0x1000 * slot;
      const std::uint64_t text = piece + 0x800;
      m.put_u64(piece + 0x260, text);
      const std::string room(80, char{0});
      m.put(text, room.data(), room.size());
      m.put(text, name.c_str(), name.size() + 1);
    };
    name_in(0, "Weapon_Sword_DekuStick");
    name_in(5, "Weapon_Boomerang");
    const auto e = reader.read_equipment();
    REQUIRE(e);
    CHECK(e->weapon_slot == "Weapon_Sword_DekuStick");
    CHECK(e->tool == "Weapon_Boomerang");
    CHECK(e->sub_weapon == "");    // empty slots
    CHECK(e->third_weapon == "");
    CHECK(e->sub_tool == "");
    CHECK(e->held == "");
  }
  SUBCASE("puppet-gear-148: the shield the game holds (slot 4) over the EquipShield flag") {
    put_user(m, l);
    put_strings(m, {{addr::kEquipShield, "Weapon_Shield_Mirror"}});
    const auto name_in = [&](std::uint32_t slot, const std::string& name) {
      put_equipped(m, slot, 60 + slot, 4, 0x0);
      const std::uint64_t piece = kEquipActors + 0x1000 * slot;
      const std::uint64_t text = piece + 0x800;
      m.put_u64(piece + 0x260, text);
      const std::string room(80, char{0});
      m.put(text, room.data(), room.size());
      m.put(text, name.c_str(), name.size() + 1);
    };
    SUBCASE("an empty slot leaves the flag") {
      CHECK(reader.read_equipment()->shield == "Weapon_Shield_Mirror");
    }
    SUBCASE("a child at the age change: the Deku Shield it holds, not the adult's Mirror Shield") {
      name_in(4, "Weapon_Shield_Deku");
      CHECK(reader.read_equipment()->shield == "Weapon_Shield_Deku");
    }
    SUBCASE("something else in the slot is no shield: the flag stands") {
      name_in(4, "Item_Ocarina_Fairy");
      CHECK(reader.read_equipment()->shield == "Weapon_Shield_Mirror");
    }
  }
  SUBCASE("puppet-gear-148: a shield or tunic the player lost (StockNum 0) is not worn") {
    put_user(m, l);
    put_links(m);
    put_strings(m, {{addr::kEquipShield, "Weapon_Shield_Hylian"},
                    {addr::kEquipArmor, "Armor_Adult_Goron"},
                    {addr::kEquipBoot, "Armor_Adult_HoverBoot"}});
    CHECK(proto::flag_hash("PouchData.Weapon_Shield_Deku.StockNum") == 0x7ca64f23u);
    SUBCASE("burnt or eaten: the count is 0") {
      put_ints(m, {{"PouchData.Weapon_Shield_Hylian.StockNum", 0},
                   {"PouchData.Armor_Adult_Goron.StockNum", 0},
                   {"PouchData.Armor_Adult_HoverBoot.StockNum", 1}});
      const auto e = reader.read_equipment();
      CHECK(e->shield == "");
      CHECK(e->armor == "");
      CHECK(e->boots == "Armor_Adult_HoverBoot");
    }
    SUBCASE("still owned: the flags stand") {
      put_ints(m, {{"PouchData.Weapon_Shield_Hylian.StockNum", 1},
                   {"PouchData.Armor_Adult_Goron.StockNum", 1}});
      const auto e = reader.read_equipment();
      CHECK(e->shield == "Weapon_Shield_Hylian");
      CHECK(e->armor == "Armor_Adult_Goron");
    }
    SUBCASE("a count not read leaves the name") {
      const auto e = reader.read_equipment();
      CHECK(e->shield == "Weapon_Shield_Hylian");
      CHECK(e->armor == "Armor_Adult_Goron");
    }
  }
  SUBCASE("the child's Goron's Bracelet is sent as hand, from its accessory link") {
    put_user(m, l);
    // Armor link 9 is the 18th link of the same array (8 hand slots first).
    const std::uint32_t link = 8 + addr::kArmorLinkBracelet;
    m.put_u64(kUser + 0x20 + 0x18 * link + 8, kEquipHolders + 0x100 * link);
    const auto wear = [&](const std::string& name) {
      put_equipped(m, link, 70, 4, 0x0);
      const std::uint64_t piece = kEquipActors + 0x1000 * link;
      const std::uint64_t text = piece + 0x800;
      m.put_u64(piece + 0x260, text);
      const std::string room(80, char{0});
      m.put(text, room.data(), room.size());
      m.put(text, name.c_str(), name.size() + 1);
    };
    SUBCASE("no flag and no link read: hand not known") {
      CHECK_FALSE(reader.read_equipment()->hand);
    }
    SUBCASE("an empty flag with the link unreadable is not known, not taken off") {
      put_strings(m, {{addr::kEquipHandArmor, ""}, {addr::kEquipWeapon, "Weapon_Sword_Kokiri"}});
      CHECK_FALSE(reader.read_equipment()->hand);
    }
    SUBCASE("an empty flag with every link read but one is not known either") {
      put_strings(m, {{addr::kEquipHandArmor, ""}});
      put_links(m);
      m.erase(kUser + 0x20 + 0x18 * (8 + 3) + 0x10, 4);
      CHECK_FALSE(reader.read_equipment()->hand);
    }
    SUBCASE("an empty flag and empty links: no hand piece") {
      put_strings(m, {{addr::kEquipHandArmor, ""}});
      put_links(m);
      CHECK(reader.read_equipment()->hand == "");
    }
    SUBCASE("the bracelet, while EquipHandArmor is empty") {
      put_strings(m, {{addr::kEquipHandArmor, ""}});
      put_links(m);
      wear("Armor_Child_Gauntlet_Goron");
      CHECK(reader.read_equipment()->hand == "Armor_Child_Gauntlet_Goron");
    }
    SUBCASE("row 148: a young Link whose flag still names the Golden Gauntlets") {
      m.put_u8(l.game_ui + addr::kGameUiIsChild, 1);
      put_strings(m, {{addr::kEquipHandArmor, "Armor_Adult_Gauntlet_Gold"}});
      put_links(m);
      SUBCASE("wears the bracelet the game draws, not the adult's gauntlets") {
        wear("Armor_Child_Gauntlet_Goron");
        CHECK(reader.read_equipment()->hand == "Armor_Child_Gauntlet_Goron");
      }
      SUBCASE("with no hand piece in any link wears none") {
        CHECK(reader.read_equipment()->hand == "");
      }
      SUBCASE("with a link unreadable (a load) not known: the flag is never sent") {
        m.erase(kUser + 0x20 + 0x18 * (8 + 3) + 0x10, 4);
        CHECK_FALSE(reader.read_equipment()->hand);
      }
    }
    SUBCASE("an adult's named hand flag stands, as before row 148") {
      put_strings(m, {{addr::kEquipHandArmor, "Armor_Adult_Gauntlet_Silver"}});
      put_links(m);
      CHECK(reader.read_equipment()->hand == "Armor_Adult_Gauntlet_Silver");
      wear("Armor_Child_Gauntlet_Goron");
      CHECK(reader.read_equipment()->hand == "Armor_Adult_Gauntlet_Silver");
    }
    SUBCASE("another piece in that link is not a bracelet") {
      put_strings(m, {{addr::kEquipHandArmor, ""}});
      put_links(m);
      wear("Item_Ocarina_Fairy");
      CHECK(reader.read_equipment()->hand == "");
    }
    SUBCASE("the bracelet gone") {
      put_strings(m, {{addr::kEquipHandArmor, ""}});
      put_links(m);
      wear("Armor_Child_Gauntlet_Goron");
      m.put_u32(kEquipActors + 0x1000 * link + 0x2c, 8);
      CHECK(reader.read_equipment()->hand == "");
    }
  }
  SUBCASE("a mask, tunic, boots or gauntlets are read from the armor links (issue 91, row 148)") {
    put_user(m, l);
    put_links(m);
    // Armor link a is link 8 + a of the same array (8 hand slots first).
    const auto wear = [&](std::uint32_t a, const std::string& name) {
      const std::uint32_t link = 8 + a;
      put_equipped(m, link, 80 + a, 4, 0x0);
      const std::uint64_t piece = kEquipActors + 0x1000 * link;
      const std::uint64_t text = piece + 0x800;
      m.put_u64(piece + 0x260, text);
      const std::string room(80, char{0});
      m.put(text, room.data(), room.size());
      m.put(text, name.c_str(), name.size() + 1);
    };
    SUBCASE("a mask from ZR while EquipHeadArmor is empty") {
      put_strings(m, {{addr::kEquipHeadArmor, ""}});
      wear(0, "Armor_Child_Mask_Skull");
      CHECK(reader.read_equipment()->head == "Armor_Child_Mask_Skull");
    }
    SUBCASE("by name, in whichever link the game put it") {
      put_strings(m, {{addr::kEquipHeadArmor, ""}, {addr::kEquipBoot, ""}});
      wear(4, "Armor_Child_Mask_Bunny");
      wear(2, "Armor_Adult_HoverBoot");
      const auto e = reader.read_equipment();
      CHECK(e->head == "Armor_Child_Mask_Bunny");
      CHECK(e->boots == "Armor_Adult_HoverBoot");
    }
    SUBCASE("an unknown flag is filled too") {
      put_strings(m, {{addr::kEquipWeapon, "Weapon_Sword_Kokiri"}});
      wear(2, "Armor_Adult_HeavyBoot");
      wear(1, "Armor_Adult_Zora");
      const auto e = reader.read_equipment();
      CHECK(e->boots == "Armor_Adult_HeavyBoot");
      CHECK(e->armor == "Armor_Adult_Zora");
      CHECK_FALSE(e->head);  // no mask worn and its flag unknown: still unknown
    }
    SUBCASE("the adult wearing what the flags name: sent as worn") {
      put_strings(m, {{addr::kEquipArmor, "Armor_Adult_Goron"},
                      {addr::kEquipBoot, "Armor_Adult_HoverBoot"},
                      {addr::kEquipHandArmor, "Armor_Adult_Gauntlet_Gold"}});
      wear(1, "Armor_Adult_Goron");
      wear(2, "Armor_Adult_HoverBoot");
      wear(3, "Armor_Adult_Gauntlet_Gold");
      const auto e = reader.read_equipment();
      CHECK(e->armor == "Armor_Adult_Goron");
      CHECK(e->boots == "Armor_Adult_HoverBoot");
      CHECK(e->hand == "Armor_Adult_Gauntlet_Gold");
    }
    SUBCASE("a child: the link decides over the flag") {
      m.put_u8(l.game_ui + addr::kGameUiIsChild, 1);
      put_strings(m, {{addr::kEquipHeadArmor, "Armor_Child_Mask_Keaton"},
                      {addr::kEquipArmor, "Armor_Adult_Goron"},
                      {addr::kEquipHandArmor, "Armor_Adult_Gauntlet_Gold"}});
      wear(0, "Armor_Child_Mask_Skull");
      wear(1, "Armor_Adult_Zora");
      wear(3, "Armor_Adult_Gauntlet_Silver");
      const auto e = reader.read_equipment();
      CHECK(e->head == "Armor_Child_Mask_Skull");
      CHECK(e->armor == "Armor_Adult_Zora");
      CHECK(e->hand == "Armor_Adult_Gauntlet_Silver");
    }
    SUBCASE("row 148, every such piece: a young Link's adult flags that no link holds are not worn") {
      m.put_u8(l.game_ui + addr::kGameUiIsChild, 1);
      put_strings(m, {{addr::kEquipArmor, "Armor_Adult_Zora"},
                      {addr::kEquipBoot, "Armor_Adult_HeavyBoot"},
                      {addr::kEquipHandArmor, "Armor_Adult_Gauntlet_Silver"},
                      {addr::kEquipHeadArmor, "Armor_Child_Mask_Keaton"}});
      const auto e = reader.read_equipment();
      CHECK(e->armor == "");
      CHECK(e->boots == "");
      CHECK(e->hand == "");
      CHECK(e->head == "");  // a mask by its flag alone: the game draws none (wear-masks)
    }
    SUBCASE("an adult's named flags stand with no link piece (rule before row 148)") {
      put_strings(m, {{addr::kEquipArmor, "Armor_Adult_Zora"},
                      {addr::kEquipBoot, "Armor_Adult_HeavyBoot"},
                      {addr::kEquipHandArmor, "Armor_Adult_Gauntlet_Silver"},
                      {addr::kEquipHeadArmor, "Armor_Child_Mask_Keaton"}});
      wear(0, "Armor_Child_Mask_Skull");
      const auto e = reader.read_equipment();
      CHECK(e->armor == "Armor_Adult_Zora");
      CHECK(e->boots == "Armor_Adult_HeavyBoot");
      CHECK(e->hand == "Armor_Adult_Gauntlet_Silver");
      // A child's piece is the links' at any age (puppet-gear-148): the mask
      // the link holds, not the flag's.
      CHECK(e->head == "Armor_Child_Mask_Skull");
    }
    SUBCASE("puppet-gear-148, the other way: an adult's child flags that no link holds are not worn") {
      put_strings(m, {{addr::kEquipHeadArmor, "Armor_Child_Mask_Keaton"},
                      {addr::kEquipHandArmor, "Armor_Child_Gauntlet_Goron"},
                      {addr::kEquipArmor, "Armor_Adult_Goron"}});
      const auto e = reader.read_equipment();
      CHECK(e->head == "");
      CHECK(e->hand == "");
      CHECK(e->armor == "Armor_Adult_Goron");  // an adult's own piece: the flag stands
      SUBCASE("with a link unreadable (a load) not known") {
        m.erase(kUser + 0x20 + 0x18 * (8 + 3) + 0x10, 4);
        const auto during = reader.read_equipment();
        CHECK_FALSE(during->head);
        CHECK_FALSE(during->hand);
      }
    }
    SUBCASE("an adult wearing a child's piece the link holds keeps it") {
      put_strings(m, {{addr::kEquipHandArmor, "Armor_Child_Gauntlet_Goron"}});
      wear(9, "Armor_Child_Gauntlet_Goron");
      CHECK(reader.read_equipment()->hand == "Armor_Child_Gauntlet_Goron");
    }
    SUBCASE("a flag naming a piece the links never hold stands, for a child too") {
      m.put_u8(l.game_ui + addr::kGameUiIsChild, 1);
      put_strings(m, {{addr::kEquipArmor, "Armor_Adult_Normal"},
                      {addr::kEquipHeadArmor, "Armor_GlassTruth"}});
      const auto e = reader.read_equipment();
      CHECK(e->armor == "Armor_Adult_Normal");
      // ...but a head flag that is no mask is never sent (row 197): the Lens
      // of Truth's base actor, the adult's unposed glasses on a puppet.
      CHECK(e->head == "");
    }
    SUBCASE("the Lens of Truth's head flag is not sent, at either age (row 197)") {
      for (const std::uint8_t child : {std::uint8_t{0}, std::uint8_t{1}}) {
        CAPTURE(child);
        m.put_u8(l.game_ui + addr::kGameUiIsChild, child);
        put_strings(m, {{addr::kEquipHeadArmor, "Armor_GlassTruth"}});
        CHECK(reader.read_equipment()->head == "");
        wear(0, "Armor_Child_GlassTruth");  // nor the lens in a link
        CHECK(reader.read_equipment()->head == "");
        wear(0, "Armor_Child_Mask_Keaton");  // a mask in a link still is
        CHECK(reader.read_equipment()->head == "Armor_Child_Mask_Keaton");
        wear(0, "");
      }
    }
    SUBCASE("other pieces in the links are not a mask, tunic or boots") {
      put_strings(m, {{addr::kEquipHeadArmor, ""}, {addr::kEquipBoot, ""}, {addr::kEquipArmor, ""}});
      wear(0, "Item_Ocarina_Fairy");
      wear(9, "Armor_Child_Gauntlet_Goron");
      const auto e = reader.read_equipment();
      CHECK(e->head == "");
      CHECK(e->boots == "");
      CHECK(e->armor == "");
      CHECK(e->hand == "Armor_Child_Gauntlet_Goron");
    }
    SUBCASE("the mask taken off again (ZR): its link empties, head is empty") {
      put_strings(m, {{addr::kEquipHeadArmor, ""}});
      wear(0, "Armor_Child_Mask_Skull");
      CHECK(reader.read_equipment()->head == "Armor_Child_Mask_Skull");
      m.put_u32(kEquipActors + 0x1000 * 8 + 0x2c, 8);  // the piece gone
      CHECK(reader.read_equipment()->head == "");
    }
  }
  SUBCASE("a request lasts a moment: the last one stands") {
    put_user(m, l);
    const std::uint64_t request = kUser + 0x13bc;
    const std::uint64_t record = kUser + 0x4b0 + 0xa4;  // the slot record's byte: not the state
    put_equipped(m, 0, 40, 4, 0x4);  // just made: put away
    m.put_u8(record, 1);
    CHECK(reader.read_equipment()->drawn == 0x00);
    m.put_u8(request, 0x2);  // drawn
    CHECK(reader.read_equipment()->drawn == 0x01);
    m.put_u8(request, 0);  // the game took the request: still drawn
    CHECK(reader.read_equipment()->drawn == 0x01);
    CHECK(reader.read_equipment()->drawn == 0x01);
    m.put_u8(request, 0x4);  // put away
    CHECK(reader.read_equipment()->drawn == 0x00);
    m.put_u8(request, 0);
    CHECK(reader.read_equipment()->drawn == 0x00);
    // The record's byte is not the state (1 with the sword out and away alike).
    m.put_u8(record, 0);
    CHECK(reader.read_equipment()->drawn == 0x00);
    m.put_u8(record, 1);
    CHECK(reader.read_equipment()->drawn == 0x00);
    // Another piece in the slot starts put away, as the game makes it.
    m.put_u8(request, 0x2);
    CHECK(reader.read_equipment()->drawn == 0x01);
    m.put_u8(request, 0);
    put_equipped(m, 0, 41, 4, 0x0);
    CHECK(reader.read_equipment()->drawn == 0x00);
    // The piece gone: nothing is remembered.
    m.put_u8(request, 0x2);
    CHECK(reader.read_equipment()->drawn == 0x01);
    m.put_u8(request, 0);
    m.put_u32(kEquipActors + 0x2c, 8);
    CHECK(reader.read_equipment()->drawn == 0x00);
    m.put_u32(kEquipActors + 0x2c, 4);
    CHECK(reader.read_equipment()->drawn == 0x00);
  }
  SUBCASE("a held item the game keeps away is put away, its request missed or not") {
    // held-fix: the slingshot stayed in the puppet's hand after the sword
    // came out, as its put-away request passed between two readings.
    put_user(m, l);
    const std::uint64_t request = kUser + 0x13bc + 3;
    const std::uint64_t held_state = kEquipActors + 0x1000 * 3 + 0x2c;
    put_equipped(m, 3, 50, 6, 0x4);  // the slingshot, put away
    CHECK(reader.read_equipment()->drawn == 0x00);
    m.put_u8(request, 0x2);  // aimed: the request comes while it is still kept away
    CHECK(reader.read_equipment()->drawn == 0x08);
    m.put_u32(held_state, 4);  // in the hand
    m.put_u8(request, 0);
    CHECK(reader.read_equipment()->drawn == 0x08);
    // The sword comes out: slot 0 drawn, the slingshot's put-away request unseen.
    put_equipped(m, 0, 40, 4, 0x2);
    m.put_u32(held_state, 6);
    CHECK(reader.read_equipment()->drawn == 0x01);
    m.put_u8(kUser + 0x13bc, 0);
    CHECK(reader.read_equipment()->drawn == 0x01);
    // The sword is no carried piece: its state 6 changes nothing.
    m.put_u32(kEquipActors + 0x2c, 6);
    CHECK(reader.read_equipment()->drawn == 0x01);
    m.put_u32(kEquipActors + 0x2c, 4);
    // Aimed again, the draw request cleared while it is still kept away:
    // put away for that reading, then out once the game shows it (state 4).
    m.put_u8(kUser + 0x13bc, 0x4);  // the sword put away
    m.put_u8(request, 0x2);
    CHECK(reader.read_equipment()->drawn == 0x08);
    m.put_u8(kUser + 0x13bc, 0);
    m.put_u8(request, 0);
    CHECK(reader.read_equipment()->drawn == 0x00);
    m.put_u32(held_state, 4);
    CHECK(reader.read_equipment()->drawn == 0x08);
    CHECK(reader.read_equipment()->drawn == 0x08);
    // The sword is no carried piece: state 4 with the byte 0 keeps it put away.
    CHECK((*reader.read_equipment()->drawn & 0x01) == 0);
  }
  SUBCASE("readers of different frames share a state") {
    put_user(m, l);
    put_equipped(m, 0, 40, 4, 0x2);
    PlayerReader::DrawnState state{};
    {
      PlayerReader first(m, &state);
      CHECK(first.read_equipment()->drawn == 0x01);
    }
    m.put_u8(kUser + 0x13bc, 0);
    PlayerReader second(m, &state);
    CHECK(second.read_equipment()->drawn == 0x01);
    PlayerReader alone(m);
    CHECK(alone.read_equipment()->drawn == 0x00);
  }
}

TEST_CASE("host reader: our Link an event member counts as in an event, word 0 or not (row 97, G1)") {
  // The game's own member test: M = [[actor + 0x270] + 0x88], its record L = [M + 0x18], four slots
  // of 0x38 bytes from L + 0x10, a member when a slot holds an event pointer and flag bit 0.
  FakeMemory m;
  const Layout l = make_game(m);
  constexpr std::uint64_t kParts = 0x92000000, kMember = 0x92100000, kRecord = 0x92200000;
  constexpr std::uint64_t kEvent = 0x106d85b750;  // as the offline run fable-g1-events read it
  const auto slot = [&](std::uint64_t i) { return kRecord + addr::kRecordSlots + addr::kMemberSlotSize * i; };
  m.put_u64(l.actor1 + addr::kActorComponents, kParts);
  m.put_u64(kParts + addr::kEventMemberSlot, kMember);
  m.put_u64(kMember + addr::kMemberRecord, 0);  // in no event
  {
    PlayerReader reader(m);
    CHECK(reader.read_link_bound() == false);
    const auto r = reader.read();
    REQUIRE(r);
    CHECK(r->event_running == false);
    CHECK_FALSE(r->event_by_link);
    CHECK_FALSE(r->link_bound);
  }
  m.put_u64(kMember + addr::kMemberRecord, kRecord);
  for (std::uint64_t i = 0; i < addr::kMemberSlots; ++i) {
    m.put_u64(slot(i), 0);
    m.put_u8(slot(i) + addr::kSlotFlags, 0);
  }
  // Our Link holds the event's pointer without the member flag (as Link's sword does): not a member.
  m.put_u64(slot(0), kEvent);
  m.put_u8(slot(0) + addr::kSlotFlags, 0x2);
  {
    PlayerReader reader(m);
    CHECK(reader.read_link_bound() == false);
    CHECK(reader.read()->event_running == false);
  }
  // The flag in another slot (an event nested in a running one): a member.
  m.put_u64(slot(1), kEvent + 0x1220);
  m.put_u8(slot(1) + addr::kSlotFlags, 0x1);
  {
    PlayerReader reader(m);
    const auto r = reader.read();
    REQUIRE(r);
    CHECK(r->event_running == true);
    CHECK(r->event_by_link);
    CHECK(r->link_bound);
    CHECK(reader.read_event_running() == true);
    CHECK(reader.read_event_word() == false);
  }
  m.put_u64(l.events + addr::kEventRunning, 0x10000);  // the word too: not "by the link only"
  {
    PlayerReader reader(m);
    const auto r = reader.read();
    REQUIRE(r);
    CHECK(r->event_running == true);
    CHECK_FALSE(r->event_by_link);
    CHECK(r->link_bound);
  }
  // The word alone, our Link not a member: an event runs that does not take our Link.
  m.put_u8(slot(1) + addr::kSlotFlags, 0);
  {
    PlayerReader reader(m);
    const auto r = reader.read();
    REQUIRE(r);
    CHECK(r->event_running == true);
    CHECK_FALSE(r->link_bound);
  }
  // A record that cannot be read: not known, never a member.
  m.put_u64(l.events + addr::kEventRunning, 0);
  m.put_u64(kMember + addr::kMemberRecord, 0x93000000);
  m.put_u64(kParts + addr::kEventMemberSlot, 0x94000000);  // M itself unreadable
  {
    PlayerReader reader(m);
    CHECK(reader.read_link_bound() == std::nullopt);
    const auto r = reader.read();
    REQUIRE(r);
    CHECK(r->event_running == false);
    CHECK_FALSE(r->link_bound);
  }
}

TEST_CASE("host reader: the player's life from its own Life component, dead at 0 (away-status)") {
  // [[[actor + 0x270] + 0x2b8] + 0x1c10] + 8: the objects walk's read (coop_objects.cpp LifeValue).
  FakeMemory m;
  const Layout l = make_game(m);
  {
    PlayerReader reader(m);  // no Life component: not known, never "alive" or "dead"
    CHECK(reader.read_life() == std::nullopt);
    const auto r = reader.read();
    REQUIRE(r);
    CHECK(r->dead == std::nullopt);
  }
  constexpr std::uint64_t kParts = 0x93000000, kLife = 0x93100000, kValue = 0x93200000;
  m.put_u64(l.actor1 + addr::kActorComponents, kParts);
  m.put_u64(kParts + addr::kComponentLife, kLife);
  m.put_u64(kLife + addr::kLifeValue, kValue);
  m.put_u32(kValue + addr::kLifeValueWord, 12);  // three hearts
  {
    PlayerReader reader(m);
    CHECK(reader.read_life() == 12);
    CHECK(reader.read()->dead == false);
  }
  m.put_u32(kValue + addr::kLifeValueWord, 1);  // a downed Link's quarter heart (down but not out): not dead
  CHECK(PlayerReader(m).read()->dead == false);
  m.put_u32(kValue + addr::kLifeValueWord, 0);  // Game Over
  {
    PlayerReader reader(m);
    CHECK(reader.read_life() == 0);
    CHECK(reader.read()->dead == true);
  }
  m.put_u32(kValue + addr::kLifeValueWord, 0xFFFFFFFFu);  // garbage (-1): not known
  CHECK(PlayerReader(m).read()->dead == std::nullopt);
  m.put_u32(kValue + addr::kLifeValueWord, 4 * 41);  // above protocol::kMaxLifeQuarters: not known
  CHECK(PlayerReader(m).read_life() == std::nullopt);

  // A split-screen seat (set_actor) reads its own actor's Life, not the first Link's.
  constexpr std::uint64_t kSeat = 0x94000000, kSeatParts = 0x94100000, kSeatLife = 0x94200000,
                          kSeatValue = 0x94300000;
  m.put_u32(kValue + addr::kLifeValueWord, 12);
  m.put_u32(kSeat + addr::kActorState, addr::kActorStatePresent);
  m.put_vec3(kSeat + addr::kActorPosition, {4.0f, 5.0f, 6.0f});
  put_matrix(m, kSeat, Mat3{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}});
  m.put_u64(kSeat + addr::kActorComponents, kSeatParts);
  m.put_u64(kSeatParts + addr::kComponentLife, kSeatLife);
  m.put_u64(kSeatLife + addr::kLifeValue, kSeatValue);
  m.put_u32(kSeatValue + addr::kLifeValueWord, 0);
  PlayerReader seat(m);
  seat.set_actor(kSeat);
  CHECK(seat.read_life() == 0);
  CHECK(PlayerReader(m).read_life() == 12);
}
