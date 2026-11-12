#include "coop/host/player_reader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <span>
#include <vector>

#include "addresses.hpp"
#include "coop/host/game_flags.hpp"

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

namespace {

// A NUL-terminated printable name of at most `max` bytes at `at`.
std::optional<std::string> read_name(GameMemory& memory, std::uint64_t at, std::size_t max) {
  std::vector<std::uint8_t> bytes(max + 1);
  if (at == 0 || !memory.read(at, bytes)) return std::nullopt;
  std::string name;
  for (const std::uint8_t b : bytes) {
    if (b == 0) return name.empty() ? std::nullopt : std::optional<std::string>(name);
    if (b < 0x20 || b > 0x7e) return std::nullopt;
    name.push_back(static_cast<char>(b));
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> tunic_actor(std::string_view choice) {
  std::string c;
  for (const char ch : choice) {
    if (ch != ' ' && ch != '	') c.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  if (c == "green" || c == "verde" || c == "kokiri") return std::string("Armor_Adult_Normal");
  if (c == "red" || c == "roja" || c == "rojo" || c == "goron") return std::string("Armor_Adult_Goron");
  if (c == "blue" || c == "azul" || c == "zora") return std::string("Armor_Adult_Zora");
  return std::nullopt;
}

void apply_tunic_choice(PlayerReading& reading, const std::optional<std::string>& tunic) {
  if (!tunic || reading.age != protocol::Age::adult || !reading.equipment) return;
  reading.equipment->armor = *tunic;
}

std::optional<std::uint8_t> PlayerReader::read_ocarina_note() {
  const auto root = read_ptr(memory_, memory_.main_base() + addr::kOcarinaRootPtr);
  if (!root) return std::nullopt;
  const auto scene = read_ptr(memory_, *root);
  if (!scene) return std::nullopt;
  const auto table = read_ptr(memory_, *scene + addr::kOcarinaTable);
  if (!table) return std::nullopt;
  const auto count = read_u32(memory_, *table + addr::kOcarinaTableCount);
  if (!count || *count < addr::kOcarinaMinCount) return std::nullopt;
  const auto entries = read_ptr(memory_, *table + addr::kOcarinaTableArray);
  if (!entries) return std::nullopt;
  const auto sound = read_ptr(memory_, *entries + addr::kOcarinaEntry);
  if (!sound) return std::nullopt;
  // The entry is the sound object itself (its own vtable), as seen in a run.
  const auto vtable = read_u64(memory_, *sound);
  if (!vtable || *vtable != memory_.main_base() + addr::kOcarinaSoundVtable) return std::nullopt;
  // The note that sounds now, as the object's own update picks it.
  const auto released = read_u8(memory_, *sound + addr::kOcarinaReleased);
  const auto state = read_u32(memory_, *sound + addr::kOcarinaState);
  const auto hold = read_u32(memory_, *sound + addr::kOcarinaHold);
  const auto melody_on = read_u8(memory_, *sound + addr::kOcarinaMelodyOn);
  if (!released || !state || !hold || !melody_on) return std::nullopt;
  if ((*released & 1) != 0 || *hold != 0 || (*state | 2) == 0xe) return std::nullopt;
  const bool melody = (*state < 13 && ((1u << *state) & 0x1160u) != 0) || *melody_on == 1;
  const auto note = read_u32(memory_, *sound + (melody ? addr::kOcarinaMelodySound : addr::kOcarinaSound));
  if (!note || *note == 0 || *note > protocol::kOcarinaNotes) return std::nullopt;
  return static_cast<std::uint8_t>(*note - 1);
}

std::optional<std::string> PlayerReader::read_cutscene() {
  const auto events = read_ptr(memory_, memory_.main_base() + addr::kEventManagerPtr);
  if (!events) return std::nullopt;
  const auto session = read_ptr(memory_, *events + addr::kEventRunning);
  if (!session) return std::nullopt;
  const auto count = read_u32(memory_, *session + addr::kSessionCount);
  const auto current = read_u32(memory_, *session + addr::kSessionCurrent);
  if (!count || !current || *count > addr::kMaxSessionEvents || *current >= *count) return std::nullopt;
  const auto list = read_ptr(memory_, *session + addr::kSessionEvents);
  if (!list) return std::nullopt;
  const auto event = read_ptr(memory_, *list + 8ull * *current);
  if (!event) return std::nullopt;
  const auto part_at = read_ptr(memory_, *event + addr::kEventName);
  const auto part = part_at ? read_name(memory_, *part_at, protocol::kMaxCutsceneChars) : std::nullopt;
  if (!part || !part->starts_with("Dm_")) return std::nullopt;
  const auto flow_at = read_ptr(memory_, *session + addr::kSessionName);
  return flow_at ? read_name(memory_, *flow_at, protocol::kMaxCutsceneChars) : std::nullopt;
}

std::optional<protocol::Animation> PlayerReader::read_animation() {
  const auto actor = find_player_actor();
  if (!actor) return std::nullopt;
  const auto components = read_ptr(memory_, *actor + addr::kActorComponents);
  if (!components) return std::nullopt;
  const auto as = read_ptr(memory_, *components + addr::kComponentsAs);
  if (!as) return std::nullopt;
  const auto inner = read_ptr(memory_, *as + addr::kAsComponentInner);
  if (!inner) return std::nullopt;
  const auto controller = read_ptr(memory_, *inner + addr::kAsInnerController);
  if (!controller) return std::nullopt;
  const auto count = read_u32(memory_, *controller + addr::kAsSlotCount);
  // A signed count, as the game compares it.
  if (!count || static_cast<std::int32_t>(*count) <= static_cast<std::int32_t>(addr::kAsMainSlot)) {
    return std::nullopt;
  }
  const auto array = read_ptr(memory_, *controller + addr::kAsSlotArray);
  if (!array) return std::nullopt;
  const auto slot = read_ptr(memory_, *array + addr::kAsMainSlot * addr::kAsSlotStride);
  if (!slot) return std::nullopt;
  const auto name_at = read_ptr(memory_, *slot + addr::kAsSlotCommandName);
  if (!name_at) return std::nullopt;

  // The name, in 16-byte reads that never cross a 16-byte boundary, so a
  // name near the end of a mapped page still reads.
  std::string name;
  std::uint64_t at = *name_at;
  bool terminated = false;
  while (!terminated && name.size() <= protocol::kMaxAnimationChars) {
    std::array<std::uint8_t, 16> chunk{};
    const std::size_t size = 16 - static_cast<std::size_t>(at & 15);
    if (!memory_.read(at, std::span(chunk.data(), size))) return std::nullopt;
    for (std::size_t i = 0; i < size; ++i) {
      if (chunk[i] == 0) {
        terminated = true;
        break;
      }
      name.push_back(static_cast<char>(chunk[i]));
    }
    at += size;
  }
  if (!terminated || name.empty() || name.size() > protocol::kMaxAnimationChars) {
    return std::nullopt;
  }
  for (const char c : name) {
    if (c < 0x20 || c > 0x7e) return std::nullopt;
  }

  const auto frame = read_f32(memory_, *slot + addr::kAsSlotFrame);
  const auto rate = read_f32(memory_, *slot + addr::kAsSlotRate);
  if (!frame || !rate) return std::nullopt;
  if (!std::isfinite(*frame) || std::fabs(*frame) > kMaxAnimationFrame) return std::nullopt;
  if (!std::isfinite(*rate) || std::fabs(*rate) > kMaxAnimationRate) return std::nullopt;
  return protocol::Animation{std::move(name), *frame, *rate};
}

std::optional<protocol::Equipment> PlayerReader::read_equipment() {
  protocol::Equipment e;
  GameFlags flags(memory_);
  e.weapon = flags.get_string64(addr::kEquipWeapon);
  e.shield = flags.get_string64(addr::kEquipShield);
  e.armor = flags.get_string64(addr::kEquipArmor);
  e.boots = flags.get_string64(addr::kEquipBoot);
  e.head = flags.get_string64(addr::kEquipHeadArmor);
  e.hand = flags.get_string64(addr::kEquipHandArmor);
  e.ocarina = flags.get_string64(addr::kEquipOcarina);
  e.arrow = flags.get_string64(addr::kEquipArrow);
  e.drawn = read_drawn();
  e.held = read_slot_name(addr::kEquipSlotHeldItem);
  e.weapon_slot = read_slot_name(addr::kEquipSlotSword);
  e.sub_weapon = read_slot_name(addr::kEquipSlotSubWeapon);
  e.third_weapon = read_slot_name(addr::kEquipSlotThirdWeapon);
  e.tool = read_slot_name(addr::kEquipSlotTool);
  e.sub_tool = read_slot_name(addr::kEquipSlotSubTool);
  if (e == protocol::Equipment{}) return std::nullopt;
  return e;
}

std::optional<std::uint8_t> PlayerReader::read_drawn() {
  const auto actor = find_player_actor();
  if (!actor) return std::nullopt;
  const auto components = read_ptr(memory_, *actor + addr::kActorComponents);
  if (!components) return std::nullopt;
  const auto user = read_ptr(memory_, *components + addr::kComponentsEquipmentUser);
  if (!user) return std::nullopt;
  std::uint8_t drawn = 0;
  for (std::uint32_t i = 0; i < addr::kEquipSlotCount; ++i) {
    const std::uint64_t slot = *user + addr::kEquipSlots + addr::kEquipSlotStride * i;
    const auto id = read_u32(memory_, slot + addr::kEquipSlotId);
    if (!id) return std::nullopt;
    DrawnMemo& memo = drawn_memo_[i];
    // As 0x4a33c0: the holder must still hold this id and its actor must
    // not be gone.
    bool live = false;
    if (*id != addr::kEmptyId) {
      if (const auto holder = read_ptr(memory_, slot + addr::kEquipSlotHolder)) {
        const auto holder_id = read_u32(memory_, *holder + addr::kHolderId);
        if (holder_id && *holder_id == *id) {
          if (const auto equipment = read_ptr(memory_, *holder + addr::kHolderActor)) {
            const auto state = read_u32(memory_, *equipment + addr::kActorState);
            live = state && !(*state >= addr::kActorStateGoneFirst &&
                              *state <= addr::kActorStateGoneLast);
          }
        }
      }
    }
    if (!live) {
      memo = DrawnMemo{};
      continue;
    }
    const auto bits = read_u8(memory_, *user + addr::kEquipSlotState + i);
    if (!bits) return std::nullopt;
    if (memo.id != *id) {
      memo = DrawnMemo{*id, false};  // a new piece: the game makes it put away
    }
    // The request byte lasts a moment; the last request seen stands until
    // the next one.
    if (*bits & addr::kEquipSlotDrawnBit) {
      memo.drawn = true;
    } else if (*bits & (addr::kEquipRequestAway | addr::kEquipRequestNone)) {
      memo.drawn = false;
    }
    if (memo.drawn) drawn = static_cast<std::uint8_t>(drawn | (1u << i));
  }
  return drawn;
}

std::optional<std::string> PlayerReader::read_slot_name(std::uint32_t slot_index) {
  if (slot_index >= addr::kEquipSlotCount) return std::nullopt;
  const auto actor = find_player_actor();
  if (!actor) return std::nullopt;
  const auto components = read_ptr(memory_, *actor + addr::kActorComponents);
  if (!components) return std::nullopt;
  const auto user = read_ptr(memory_, *components + addr::kComponentsEquipmentUser);
  if (!user) return std::nullopt;
  const std::uint64_t slot =
      *user + addr::kEquipSlots + addr::kEquipSlotStride * slot_index;
  const auto id = read_u32(memory_, slot + addr::kEquipSlotId);
  if (!id) return std::nullopt;
  if (*id == addr::kEmptyId) return std::string{};
  // The same test of the link as read_drawn.
  const auto holder = read_ptr(memory_, slot + addr::kEquipSlotHolder);
  if (!holder) return std::string{};
  const auto holder_id = read_u32(memory_, *holder + addr::kHolderId);
  if (!holder_id || *holder_id != *id) return std::string{};
  const auto piece = read_ptr(memory_, *holder + addr::kHolderActor);
  if (!piece) return std::string{};
  const auto state = read_u32(memory_, *piece + addr::kActorState);
  if (!state) return std::nullopt;
  if (*state >= addr::kActorStateGoneFirst && *state <= addr::kActorStateGoneLast) {
    return std::string{};
  }
  const auto text = read_ptr(memory_, *piece + addr::kActorName);
  if (!text) return std::nullopt;
  std::array<std::uint8_t, protocol::kMaxEquipNameChars + 1> bytes{};
  if (!memory_.read(*text, bytes)) return std::nullopt;
  std::string name;
  for (const std::uint8_t b : bytes) {
    if (b == 0) return name.empty() ? std::nullopt : std::optional<std::string>(name);
    if (b < 0x20 || b > 0x7e) return std::nullopt;
    name.push_back(static_cast<char>(b));
  }
  return std::nullopt;  // longer than a name can be
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
  if (reading.event_running.value_or(false)) reading.cutscene = read_cutscene();
  reading.animation = read_animation();
  if (!reading.cutscene && reading.animation &&
      reading.animation->name.find("Ocarina") != std::string::npos) {
    reading.ocarina_note = read_ocarina_note();
  }
  reading.equipment = read_equipment();
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
