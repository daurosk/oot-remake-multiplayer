#include "coop/host/player_reader.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "addresses.hpp"

namespace coop::host {
namespace {

// The longest Banc path read. The map's peek reads 96 bytes.
constexpr std::uint64_t kMaxPathBytes = 256;

bool plausible(const Vec3& v) {
  return std::all_of(v.begin(), v.end(), [](float c) {
    return std::isfinite(c) && std::fabs(c) <= kMaxCoordinate;
  });
}

std::optional<Vec3> read_vec3(GameMemory& memory, std::uint64_t address) {
  Vec3 v{};
  for (std::size_t i = 0; i < 3; ++i) {
    const auto c = read_f32(memory, address + 4 * i);
    if (!c) return std::nullopt;
    v[i] = *c;
  }
  if (!plausible(v)) return std::nullopt;
  return v;
}

}  // namespace

std::optional<std::uint64_t> PlayerReader::game_ui() {
  return read_ptr(memory_, memory_.main_base() + addr::kGameUiModulePtr);
}

std::optional<std::uint64_t> PlayerReader::find_player_actor() {
  const auto scene = read_ptr(memory_, memory_.main_base() + addr::kSceneModulePtr);
  if (!scene) return std::nullopt;
  const auto table = read_ptr(memory_, *scene + addr::kSceneTable);
  if (!table) return std::nullopt;
  const auto count = read_u32(memory_, *table + addr::kTableCount);
  if (!count || *count < addr::kMinTableCount) return std::nullopt;
  const auto array = read_ptr(memory_, *table + addr::kTableArray);
  if (!array) return std::nullopt;
  const auto manager = read_ptr(memory_, *array + addr::kPlayerManagerSlot);
  if (!manager) return std::nullopt;
  if (!read_ptr(memory_, *manager + addr::kManagerMustBeSet)) return std::nullopt;

  // As 0x37cca0: the first link when its actor is there, else the second.
  for (const std::uint64_t link_at : {addr::kManagerLinkFirst, addr::kManagerLinkSecond}) {
    const auto link = read_ptr(memory_, *manager + link_at);
    if (!link) continue;
    const auto id = read_u32(memory_, *link + addr::kLinkId);
    if (!id || *id == addr::kEmptyId) continue;
    const auto holder = read_ptr(memory_, *link + addr::kLinkHolder);
    if (!holder) continue;
    const auto holder_id = read_u32(memory_, *holder + addr::kHolderId);
    if (!holder_id || *holder_id != *id) continue;  // a stale holder
    const auto actor = read_ptr(memory_, *holder + addr::kHolderActor);
    if (!actor) continue;
    const auto state = read_u32(memory_, *actor + addr::kActorState);
    if (!state || *state != addr::kActorStatePresent) continue;
    return actor;
  }
  return std::nullopt;
}

std::optional<Vec3> PlayerReader::read_position() {
  // The GameUI copy, while its valid bit is set.
  if (const auto ui = game_ui()) {
    const auto flags = read_u16(memory_, *ui + addr::kGameUiPoseFlags);
    if (flags && (*flags & addr::kGameUiPoseValidBit) != 0) {
      if (auto p = read_vec3(memory_, *ui + addr::kGameUiPosition)) return p;
    }
  }
  // Otherwise the actor itself.
  if (const auto actor = find_player_actor()) {
    return read_vec3(memory_, *actor + addr::kActorPosition);
  }
  return std::nullopt;
}

std::optional<Quat> PlayerReader::read_rotation() {
  const auto actor = find_player_actor();
  if (!actor) return std::nullopt;
  Mat3 m{};
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      const auto v = read_f32(memory_, *actor + addr::kActorRotation + 4 * (3 * r + c));
      if (!v) return std::nullopt;
      if constexpr (addr::kActorRotationRowMajor) {
        m[r][c] = *v;
      } else {
        m[c][r] = *v;
      }
    }
  }
  return matrix_to_quaternion(m);
}

std::optional<protocol::Age> PlayerReader::read_age() {
  const auto ui = game_ui();
  if (!ui) return std::nullopt;
  const auto is_child = read_u8(memory_, *ui + addr::kGameUiIsChild);
  if (!is_child) return std::nullopt;
  if (*is_child == 1) return protocol::Age::child;
  if (*is_child == 0) return protocol::Age::adult;
  return std::nullopt;  // not a bool: something else is there
}

std::optional<std::string> PlayerReader::read_scene() {
  const auto banc = read_ptr(memory_, memory_.main_base() + addr::kBancModulePtr);
  if (!banc) return std::nullopt;
  if (!read_ptr(memory_, *banc + addr::kBancMustBeSet)) return std::nullopt;
  const auto path = read_ptr(memory_, *banc + addr::kBancPathPtr);
  const auto capacity = read_u64(memory_, *banc + addr::kBancPathCapacity);
  if (!path || !capacity || *capacity == 0) return std::nullopt;
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min(*capacity, kMaxPathBytes)));
  if (!memory_.read(*path, bytes)) return std::nullopt;
  const auto end = std::find(bytes.begin(), bytes.end(), std::uint8_t{0});
  if (end == bytes.end()) return std::nullopt;  // unterminated
  const std::string text(bytes.begin(), end);
  return scene_from_path(text);
}

std::optional<bool> PlayerReader::read_event_running() {
  const auto events = read_ptr(memory_, memory_.main_base() + addr::kEventManagerPtr);
  if (!events) return std::nullopt;
  const auto running = read_u64(memory_, *events + addr::kEventRunning);
  if (!running) return std::nullopt;
  return *running != 0;
}

std::optional<PlayerReading> PlayerReader::read() {
  const auto position = read_position();
  if (!position) return std::nullopt;
  PlayerReading reading;
  reading.position = *position;
  reading.rotation = read_rotation();
  reading.age = read_age();
  reading.scene = read_scene();
  reading.event_running = read_event_running();
  return reading;
}

std::optional<Quat> matrix_to_quaternion(const Mat3& m, float tolerance) {
  double a[3][3];
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      if (!std::isfinite(m[r][c])) return std::nullopt;
      a[r][c] = m[r][c];
    }
  }
  const double tol = tolerance;
  for (std::size_t i = 0; i < 3; ++i) {
    for (std::size_t j = i; j < 3; ++j) {
      const double dot = a[i][0] * a[j][0] + a[i][1] * a[j][1] + a[i][2] * a[j][2];
      if (std::fabs(dot - (i == j ? 1.0 : 0.0)) > tol) return std::nullopt;
    }
  }
  const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                     a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                     a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (std::fabs(det - 1.0) > tol) return std::nullopt;  // also refuses mirrors

  // Shepperd's method: divide by the largest of 4w^2, 4x^2, 4y^2, 4z^2, so it
  // stays accurate near 180 degrees, where w is close to 0.
  double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
  const double trace = a[0][0] + a[1][1] + a[2][2];
  if (trace > a[0][0] && trace > a[1][1] && trace > a[2][2]) {
    const double s = 2.0 * std::sqrt(std::max(0.0, 1.0 + trace));
    w = 0.25 * s;
    x = (a[2][1] - a[1][2]) / s;
    y = (a[0][2] - a[2][0]) / s;
    z = (a[1][0] - a[0][1]) / s;
  } else if (a[0][0] >= a[1][1] && a[0][0] >= a[2][2]) {
    const double s = 2.0 * std::sqrt(std::max(0.0, 1.0 + a[0][0] - a[1][1] - a[2][2]));
    w = (a[2][1] - a[1][2]) / s;
    x = 0.25 * s;
    y = (a[0][1] + a[1][0]) / s;
    z = (a[0][2] + a[2][0]) / s;
  } else if (a[1][1] >= a[2][2]) {
    const double s = 2.0 * std::sqrt(std::max(0.0, 1.0 + a[1][1] - a[0][0] - a[2][2]));
    w = (a[0][2] - a[2][0]) / s;
    x = (a[0][1] + a[1][0]) / s;
    y = 0.25 * s;
    z = (a[1][2] + a[2][1]) / s;
  } else {
    const double s = 2.0 * std::sqrt(std::max(0.0, 1.0 + a[2][2] - a[0][0] - a[1][1]));
    w = (a[1][0] - a[0][1]) / s;
    x = (a[0][2] + a[2][0]) / s;
    y = (a[1][2] + a[2][1]) / s;
    z = 0.25 * s;
  }
  const double len = std::sqrt(x * x + y * y + z * z + w * w);
  if (!(len > 0.0)) return std::nullopt;
  const double k = (w < 0.0 ? -1.0 : 1.0) / len;  // one sign: w >= 0
  return Quat{static_cast<float>(k * x), static_cast<float>(k * y), static_cast<float>(k * z),
              static_cast<float>(k * w)};
}

std::optional<std::string> scene_from_path(std::string_view path) {
  const auto slash = path.find_last_of("/\\");
  if (slash != std::string_view::npos) path.remove_prefix(slash + 1);
  const auto dot = path.find('.');
  if (dot != std::string_view::npos) path = path.substr(0, dot);
  if (path.empty() || path.size() > protocol::kMaxSceneChars) return std::nullopt;
  for (const char c : path) {
    if (c < 0x20 || c > 0x7e) return std::nullopt;
  }
  return std::string(path);
}

}  // namespace coop::host
