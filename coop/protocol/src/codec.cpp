#include "coop/protocol/codec.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace coop::protocol {

namespace {

constexpr std::size_t kMaxU16 = std::numeric_limits<std::uint16_t>::max();

// ---------------------------------------------------------------- strings

// Control characters: C0 (U+0000 to U+001F), DEL and C1 (U+0080 to U+009F).
// No string field has a use for them, and a nickname or chat line with a NUL,
// a newline or a terminal escape in it could cut or forge what others see.
bool is_control(std::uint32_t cp) { return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F); }

enum class Utf8 { ok, bad, control };

// Counts the code points of `s` into `count`. Bad UTF-8 (overlong forms,
// surrogates, values past U+10FFFF) and control characters are refused.
Utf8 utf8_length(std::string_view s, std::size_t& count) {
  count = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t extra = 0;
    std::uint32_t cp = 0;
    std::uint32_t min = 0;
    if (c < 0x80) {
      extra = 0;
      cp = c;
    } else if ((c & 0xE0u) == 0xC0u) {
      extra = 1;
      cp = c & 0x1Fu;
      min = 0x80;
    } else if ((c & 0xF0u) == 0xE0u) {
      extra = 2;
      cp = c & 0x0Fu;
      min = 0x800;
    } else if ((c & 0xF8u) == 0xF0u) {
      extra = 3;
      cp = c & 0x07u;
      min = 0x10000;
    } else {
      return Utf8::bad;
    }
    if (extra > s.size() - i - 1) return Utf8::bad;
    for (std::size_t k = 1; k <= extra; ++k) {
      const auto cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0u) != 0x80u) return Utf8::bad;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return Utf8::bad;
    }
    if (is_control(cp)) return Utf8::control;
    i += extra + 1;
    ++count;
  }
  return Utf8::ok;
}

Error check_string(std::string_view s, std::size_t max_chars) {
  if (s.size() > max_chars * 4) return Error::string_too_long;
  std::size_t chars = 0;
  switch (utf8_length(s, chars)) {
    case Utf8::ok: break;
    case Utf8::bad: return Error::bad_utf8;
    case Utf8::control: return Error::bad_value;
  }
  if (chars > max_chars) return Error::string_too_long;
  return Error::none;
}

bool valid_player(std::uint8_t n) { return n >= 1 && n <= kMaxPlayers; }
bool valid_player_or_self(std::uint8_t n) { return n <= kMaxPlayers; }

template <std::size_t N>
bool all_finite(const std::array<float, N>& v) {
  for (float f : v) {
    if (!std::isfinite(f)) return false;
  }
  return true;
}

// ---------------------------------------------------------------- writer

class Writer {
 public:
  explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}

  void u8(std::uint8_t v) { out_.push_back(v); }
  void u16(std::uint16_t v) {
    out_.push_back(static_cast<std::uint8_t>(v));
    out_.push_back(static_cast<std::uint8_t>(v >> 8));
  }
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  void u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
  void bytes(const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    out_.insert(out_.end(), p, p + n);
  }

  // Caller has checked the string with check_string, so it fits a u16.
  void str(std::string_view s) {
    u16(static_cast<std::uint16_t>(s.size()));
    bytes(s.data(), s.size());
  }

  // Reserves a u16 length and returns where it is; finish_length fills it with
  // the number of bytes written since. False when that does not fit a u16.
  std::size_t begin_length() {
    const std::size_t at = out_.size();
    u16(0);
    return at;
  }
  bool finish_length(std::size_t at) {
    const std::size_t n = out_.size() - at - 2;
    if (n > kMaxU16) return false;
    out_[at] = static_cast<std::uint8_t>(n);
    out_[at + 1] = static_cast<std::uint8_t>(n >> 8);
    return true;
  }

 private:
  std::vector<std::uint8_t>& out_;
};

// ---------------------------------------------------------------- reader

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

  std::size_t left() const { return data_.size() - pos_; }

  bool u8(std::uint8_t& v) {
    if (left() < 1) return false;
    v = data_[pos_++];
    return true;
  }
  bool u16(std::uint16_t& v) {
    if (left() < 2) return false;
    v = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
    pos_ += 2;
    return true;
  }
  bool u32(std::uint32_t& v) {
    if (left() < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data_[pos_ + i]) << (8 * i);
    pos_ += 4;
    return true;
  }
  bool u64(std::uint64_t& v) {
    if (left() < 8) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(data_[pos_ + i]) << (8 * i);
    pos_ += 8;
    return true;
  }
  bool f32(float& v) {
    std::uint32_t bits = 0;
    if (!u32(bits)) return false;
    v = std::bit_cast<float>(bits);
    return true;
  }

  // A sub-reader over the next `n` bytes, which this reader skips.
  bool take(std::size_t n, std::span<const std::uint8_t>& part) {
    if (left() < n) return false;
    part = data_.subspan(pos_, n);
    pos_ += n;
    return true;
  }

  Error str(std::string& s, std::size_t max_chars) {
    std::uint16_t n = 0;
    if (!u16(n)) return Error::truncated;
    if (n > left()) return Error::bad_length;
    if (n > max_chars * 4) return Error::string_too_long;
    s.assign(reinterpret_cast<const char*>(data_.data() + pos_), n);
    pos_ += n;
    return check_string(s, max_chars);
  }

 private:
  std::span<const std::uint8_t> data_;
  std::size_t pos_ = 0;
};

#define COOP_TRY(expr)                    \
  do {                                    \
    const Error coop_err_ = (expr);       \
    if (coop_err_ != Error::none) return coop_err_; \
  } while (false)

#define COOP_READ(call)                        \
  do {                                         \
    if (!(call)) return Error::truncated;      \
  } while (false)

// ---------------------------------------------------------------- bodies: encode

Error encode_body(const Hello& m, Writer& w) {
  COOP_TRY(check_string(m.build_id, kMaxBuildIdChars));
  COOP_TRY(check_string(m.game_build_id, kMaxGameBuildIdChars));
  COOP_TRY(check_string(m.game_version, kMaxGameVersionChars));
  COOP_TRY(check_string(m.nickname, kMaxNicknameChars));
  if (m.nickname.empty()) return Error::bad_value;
  w.u16(m.protocol_version);
  w.str(m.build_id);
  w.str(m.game_build_id);
  w.str(m.game_version);
  w.str(m.nickname);
  return Error::none;
}

Error encode_body(const Welcome& m, Writer& w) {
  if (!valid_player(m.player_number)) return Error::bad_value;
  if (m.tick_rate == 0) return Error::bad_value;
  if (m.players.size() > kMaxPlayers) return Error::bad_value;
  std::uint32_t taken = 1u << m.player_number;
  for (const auto& p : m.players) {
    if (!valid_player(p.number) || (taken & (1u << p.number))) return Error::bad_value;
    taken |= 1u << p.number;
    COOP_TRY(check_string(p.nickname, kMaxNicknameChars));
    if (p.nickname.empty()) return Error::bad_value;
  }
  w.u8(m.player_number);
  w.u64(m.server_time_us);
  w.u16(m.tick_rate);
  w.u8(static_cast<std::uint8_t>(m.players.size()));
  for (const auto& p : m.players) {
    w.u8(p.number);
    w.str(p.nickname);
  }
  return Error::none;
}

Error encode_body(const Refused& m, Writer& w) {
  w.u8(static_cast<std::uint8_t>(m.reason));
  return Error::none;
}

Error encode_body(const JoinChallenge& m, Writer& w) {
  w.bytes(m.nonce.data(), m.nonce.size());
  return Error::none;
}

Error encode_body(const JoinProof& m, Writer& w) {
  w.bytes(m.proof.data(), m.proof.size());
  return Error::none;
}

Error encode_body(const PlayerJoined& m, Writer& w) {
  if (!valid_player(m.player_number)) return Error::bad_value;
  COOP_TRY(check_string(m.nickname, kMaxNicknameChars));
  if (m.nickname.empty()) return Error::bad_value;
  w.u8(m.player_number);
  w.str(m.nickname);
  return Error::none;
}

Error encode_body(const PlayerLeft& m, Writer& w) {
  if (!valid_player(m.player_number)) return Error::bad_value;
  w.u8(m.player_number);
  w.u8(static_cast<std::uint8_t>(m.reason));
  return Error::none;
}

Error encode_body(const Ping& m, Writer& w) {
  w.u64(m.sender_time_us);
  return Error::none;
}

Error encode_body(const Pong& m, Writer& w) {
  w.u64(m.ping_sender_time_us);
  w.u64(m.receiver_time_us);
  return Error::none;
}

Error encode_body(const PlayerStatus& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  w.u8(m.player_number);
  w.u8(m.flags);
  return Error::none;
}

Error encode_body(const Chat& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_string(m.text, kMaxChatChars));
  w.u8(m.player_number);
  w.str(m.text);
  return Error::none;
}

Error encode_body(const PlayerSample& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  if (m.position && !all_finite(*m.position)) return Error::bad_value;
  if (m.velocity && !all_finite(*m.velocity)) return Error::bad_value;
  if (m.rotation && !all_finite(*m.rotation)) return Error::bad_value;
  if (m.scene) COOP_TRY(check_string(*m.scene, kMaxSceneChars));
  if (m.age && *m.age != Age::child && *m.age != Age::adult) return Error::bad_value;
  if (m.animation) {
    COOP_TRY(check_string(m.animation->name, kMaxAnimationChars));
    if (!std::isfinite(m.animation->frame) || !std::isfinite(m.animation->rate)) {
      return Error::bad_value;
    }
  }

  auto component = [&w](SampleComponent id, auto&& write) {
    w.u16(static_cast<std::uint16_t>(id));
    const std::size_t at = w.begin_length();
    write();
    w.finish_length(at);  // every component is far below 64 KiB
  };

  w.u8(m.player_number);
  component(SampleComponent::time, [&] {
    w.u64(m.time.server_time_us);
    w.u32(m.time.sequence);
  });
  if (m.position) component(SampleComponent::position, [&] { for (float f : *m.position) w.f32(f); });
  if (m.velocity) component(SampleComponent::velocity, [&] { for (float f : *m.velocity) w.f32(f); });
  if (m.rotation) component(SampleComponent::rotation, [&] { for (float f : *m.rotation) w.f32(f); });
  if (m.scene) component(SampleComponent::scene, [&] { w.str(*m.scene); });
  if (m.age) component(SampleComponent::age, [&] { w.u8(static_cast<std::uint8_t>(*m.age)); });
  if (m.animation) {
    component(SampleComponent::animation, [&] {
      w.str(m.animation->name);
      w.f32(m.animation->frame);
      w.f32(m.animation->rate);
    });
  }
  return Error::none;
}

Error encode_body(const TestPayload& m, Writer& w) {
  if (m.filler.size() > kMaxU16 - 4) return Error::too_large;
  w.u32(m.sequence);
  w.bytes(m.filler.data(), m.filler.size());
  return Error::none;
}

Error check_flag(const FlagEntry& e) {
  return valid_flag(e) ? Error::none : Error::bad_value;
}

bool has_duplicate(std::span<const FlagEntry> flags) {
  std::vector<FlagKey> keys;
  keys.reserve(flags.size());
  for (const auto& e : flags) keys.push_back(e.key);
  std::sort(keys.begin(), keys.end());
  return std::adjacent_find(keys.begin(), keys.end()) != keys.end();
}

// Which lists may carry a count delta: FLAG_SET only.
enum class Deltas : bool { refused, allowed };

Error check_delta(const FlagEntry& e, Deltas deltas) {
  return e.kind == FlagKind::count_delta && deltas == Deltas::refused ? Error::bad_value
                                                                      : Error::none;
}

// The rules every flag list follows: min_count to kMaxFlagEntries entries,
// each valid, a count delta only where allowed, no key twice.
Error check_flags(const std::vector<FlagEntry>& flags, std::size_t min_count, Deltas deltas) {
  if (flags.size() > kMaxFlagEntries) return Error::too_large;
  if (flags.size() < min_count) return Error::bad_value;
  for (const auto& e : flags) {
    COOP_TRY(check_flag(e));
    COOP_TRY(check_delta(e, deltas));
  }
  if (has_duplicate(flags)) return Error::bad_value;
  return Error::none;
}

void write_flags(const std::vector<FlagEntry>& flags, Writer& w) {
  w.u16(static_cast<std::uint16_t>(flags.size()));
  for (const auto& e : flags) {
    w.u8(static_cast<std::uint8_t>(e.key.kind));
    if (e.key.kind == FlagKeyKind::key64) {
      w.u64(e.key.key);
      w.u32(e.key.table);
    } else {
      w.u32(e.key.hash);
      w.u16(e.key.index);
    }
    w.u8(static_cast<std::uint8_t>(e.kind));
    w.u32(e.value);
  }
}

Error encode_body(const JoinPartner& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  if (!valid_player(m.other_player) || m.other_player == m.player_number) {
    return Error::bad_value;
  }
  switch (m.kind) {
    case JoinKind::request:
    case JoinKind::refuse:
      if (m.target) return Error::bad_value;
      break;
    case JoinKind::accept:
      if (!m.target) return Error::bad_value;
      COOP_TRY(check_string(m.target->scene, kMaxSceneChars));
      if (m.target->scene.empty()) return Error::bad_value;
      if (!all_finite(m.target->position) || !all_finite(m.target->rotation)) {
        return Error::bad_value;
      }
      break;
    default:
      return Error::bad_value;
  }
  w.u8(m.player_number);
  w.u8(m.other_player);
  w.u16(m.request_id);
  w.u8(static_cast<std::uint8_t>(m.kind));
  if (m.kind == JoinKind::accept) {
    w.str(m.target->scene);
    for (float f : m.target->position) w.f32(f);
    for (float f : m.target->rotation) w.f32(f);
  } else if (m.kind == JoinKind::refuse) {
    w.u8(static_cast<std::uint8_t>(m.reason));
  }
  return Error::none;
}

Error encode_body(const FlagSet& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_flags(m.flags, 1, Deltas::allowed));
  w.u8(m.player_number);
  write_flags(m.flags, w);
  return Error::none;
}

Error encode_body(const FlagState& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_flags(m.flags, 1, Deltas::refused));
  w.u8(m.player_number);
  write_flags(m.flags, w);
  return Error::none;
}

Error encode_body(const FlagReset& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_flags(m.flags, 1, Deltas::refused));
  w.u8(m.player_number);
  write_flags(m.flags, w);
  return Error::none;
}

Error encode_body(const SaveAsk& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  w.u8(m.player_number);
  return Error::none;
}

bool valid_save_part(const SavePart& m) {
  if (!valid_player_or_self(m.player_number) || !valid_player_or_self(m.to_player)) return false;
  if (m.reason != SaveReason::join && m.reason != SaveReason::host_load) return false;
  if (m.to_player == 0 && m.reason == SaveReason::join) return false;  // a join answer has one asker
  const std::uint16_t count = save_part_count(m.total_size);
  if (count == 0 || m.count != count || m.index >= count) return false;
  return m.data.size() == save_part_size(m.total_size, m.index);
}

Error encode_body(const SavePart& m, Writer& w) {
  if (!valid_save_part(m)) return Error::bad_value;
  w.u8(m.player_number);
  w.u8(m.to_player);
  w.u32(m.transfer_id);
  w.u8(static_cast<std::uint8_t>(m.reason));
  w.u32(m.total_size);
  w.u32(m.crc32);
  w.u16(m.index);
  w.u16(m.count);
  w.bytes(m.data.data(), m.data.size());
  return Error::none;
}

Error encode_body(const FlagOrigin& m, Writer& w) {
  if (!valid_player(m.player_number)) return Error::bad_value;
  COOP_TRY(check_flags(m.flags, 1, Deltas::allowed));
  w.u8(m.player_number);
  write_flags(m.flags, w);
  return Error::none;
}

Error encode_body(const FlagSnapshot& m, Writer& w) {
  if (m.part_count == 0 || m.part >= m.part_count) return Error::bad_value;
  COOP_TRY(check_flags(m.flags, 0, Deltas::refused));
  w.u16(m.part);
  w.u16(m.part_count);
  write_flags(m.flags, w);
  return Error::none;
}

Error encode_body(const Host& m, Writer& w) {
  if (!valid_player(m.player_number)) return Error::bad_value;
  w.u8(m.player_number);
  return Error::none;
}

// The name fields of Equipment, in component order.
struct EquipName {
  EquipComponent id;
  std::optional<std::string> Equipment::*field;
};
constexpr EquipName kEquipNames[] = {
    {EquipComponent::weapon, &Equipment::weapon},   {EquipComponent::shield, &Equipment::shield},
    {EquipComponent::armor, &Equipment::armor},     {EquipComponent::boots, &Equipment::boots},
    {EquipComponent::head, &Equipment::head},       {EquipComponent::hand, &Equipment::hand},
    {EquipComponent::ocarina, &Equipment::ocarina}, {EquipComponent::arrow, &Equipment::arrow},
    {EquipComponent::held, &Equipment::held},
    {EquipComponent::weapon_slot, &Equipment::weapon_slot},
    {EquipComponent::sub_weapon, &Equipment::sub_weapon},
    {EquipComponent::third_weapon, &Equipment::third_weapon},
    {EquipComponent::tool, &Equipment::tool},
    {EquipComponent::sub_tool, &Equipment::sub_tool},
};

Error encode_body(const PlayerEquip& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  for (const EquipName& n : kEquipNames) {
    if (const auto& name = m.equipment.*n.field) COOP_TRY(check_string(*name, kMaxEquipNameChars));
  }
  w.u8(m.player_number);
  for (const EquipName& n : kEquipNames) {
    const auto& name = m.equipment.*n.field;
    if (!name) continue;
    w.u16(static_cast<std::uint16_t>(n.id));
    const std::size_t at = w.begin_length();
    w.str(*name);
    w.finish_length(at);
  }
  if (m.equipment.drawn) {
    w.u16(static_cast<std::uint16_t>(EquipComponent::drawn));
    w.u16(1);
    w.u8(*m.equipment.drawn);
  }
  return Error::none;
}

// ---------------------------------------------------------------- objects: encode

Error check_object_id(const ObjectId& id) {
  switch (id.kind) {
    case ObjectKind::placed: return id.maker == 0 ? Error::none : Error::bad_value;
    case ObjectKind::made: return valid_player(id.maker) ? Error::none : Error::bad_value;
  }
  return Error::bad_value;
}

void write_object_id(const ObjectId& id, Writer& w) {
  w.u8(static_cast<std::uint8_t>(id.kind));
  w.u8(id.maker);
  w.u64(id.value);
}

Error check_object_state(const ObjectState& s) {
  if (s.position && !all_finite(*s.position)) return Error::bad_value;
  if (s.velocity && !all_finite(*s.velocity)) return Error::bad_value;
  if (s.rotation && !all_finite(*s.rotation)) return Error::bad_value;
  if (s.fuse && !std::isfinite(*s.fuse)) return Error::bad_value;
  if (s.animation) {
    COOP_TRY(check_string(s.animation->name, kMaxAnimationChars));
    if (!std::isfinite(s.animation->frame) || !std::isfinite(s.animation->rate)) {
      return Error::bad_value;
    }
  }
  return Error::none;
}

void write_object_state(const ObjectState& s, Writer& w) {
  auto component = [&w](ObjectComponent id, auto&& write) {
    w.u16(static_cast<std::uint16_t>(id));
    const std::size_t at = w.begin_length();
    write();
    w.finish_length(at);
  };
  component(ObjectComponent::time, [&] {
    w.u64(s.time.server_time_us);
    w.u32(s.time.sequence);
  });
  if (s.position) component(ObjectComponent::position, [&] { for (float f : *s.position) w.f32(f); });
  if (s.velocity) component(ObjectComponent::velocity, [&] { for (float f : *s.velocity) w.f32(f); });
  if (s.rotation) component(ObjectComponent::rotation, [&] { for (float f : *s.rotation) w.f32(f); });
  if (s.animation) {
    component(ObjectComponent::animation, [&] {
      w.str(s.animation->name);
      w.f32(s.animation->frame);
      w.f32(s.animation->rate);
    });
  }
  if (s.life) {
    component(ObjectComponent::life, [&] {
      w.u32(static_cast<std::uint32_t>(s.life->life));
      w.u32(static_cast<std::uint32_t>(s.life->max));
    });
  }
  if (s.fuse) component(ObjectComponent::fuse, [&] { w.f32(*s.fuse); });
  if (s.state) component(ObjectComponent::state, [&] { w.u8(*s.state); });
}

bool has_duplicate_id(const std::vector<ObjectPresence>& objects) {
  std::vector<ObjectId> ids;
  ids.reserve(objects.size());
  for (const ObjectPresence& o : objects) ids.push_back(o.id);
  std::sort(ids.begin(), ids.end());
  return std::adjacent_find(ids.begin(), ids.end()) != ids.end();
}

Error encode_body(const ObjectPresent& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_string(m.scene, kMaxSceneChars));
  if (m.objects.size() > kMaxPresentObjects) return Error::bad_value;
  for (const ObjectPresence& o : m.objects) COOP_TRY(check_object_id(o.id));
  if (has_duplicate_id(m.objects)) return Error::bad_value;
  w.u8(m.player_number);
  w.str(m.scene);
  w.u16(static_cast<std::uint16_t>(m.objects.size()));
  for (const ObjectPresence& o : m.objects) {
    write_object_id(o.id, w);
    w.u8(o.flags);
  }
  return Error::none;
}

Error encode_body(const ObjectOwner& m, Writer& w) {
  COOP_TRY(check_object_id(m.id));
  if (!valid_player_or_self(m.owner)) return Error::bad_value;
  write_object_id(m.id, w);
  w.u8(m.owner);
  w.u16(m.epoch);
  w.u32(m.group);
  return Error::none;
}

Error encode_body(const HandoverAsk& m, Writer& w) {
  COOP_TRY(check_object_id(m.id));
  if (!valid_player(m.to_player)) return Error::bad_value;
  write_object_id(m.id, w);
  w.u16(m.epoch);
  w.u8(m.to_player);
  return Error::none;
}

Error encode_body(const HandoverState& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_object_id(m.id));
  COOP_TRY(check_object_state(m.state));
  w.u8(m.player_number);
  write_object_id(m.id, w);
  w.u16(m.epoch);
  write_object_state(m.state, w);
  return Error::none;
}

Error check_group(const ObjectGroup& m) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  if (m.entries.empty() || m.entries.size() > kMaxGroupEntries) return Error::bad_value;
  std::vector<ObjectId> members;
  members.reserve(m.entries.size());
  for (const ObjectGroupEntry& e : m.entries) {
    COOP_TRY(check_object_id(e.member));
    COOP_TRY(check_object_id(e.root));
    if (e.flags & ~object_group::boss) return Error::bad_value;
    members.push_back(e.member);
  }
  std::sort(members.begin(), members.end());
  if (std::adjacent_find(members.begin(), members.end()) != members.end()) {
    return Error::bad_value;
  }
  return Error::none;
}

Error encode_body(const ObjectGroup& m, Writer& w) {
  COOP_TRY(check_group(m));
  w.u8(m.player_number);
  w.u16(static_cast<std::uint16_t>(m.entries.size()));
  for (const ObjectGroupEntry& e : m.entries) {
    write_object_id(e.member, w);
    write_object_id(e.root, w);
    w.u8(e.flags);
  }
  return Error::none;
}

Error encode_body(const ObjectSpawn& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_object_id(m.id));  // made, or placed (an owner naming its actor)
  COOP_TRY(check_string(m.actor, kMaxActorNameChars));
  if (m.actor.empty()) return Error::bad_value;
  COOP_TRY(check_object_state(m.state));
  w.u8(m.player_number);
  write_object_id(m.id, w);
  w.str(m.actor);
  w.u8(m.flags);
  write_object_state(m.state, w);
  return Error::none;
}

Error encode_body(const ObjectSample& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_object_id(m.id));
  COOP_TRY(check_object_state(m.state));
  w.u8(m.player_number);
  write_object_id(m.id, w);
  w.u16(m.epoch);
  write_object_state(m.state, w);
  return Error::none;
}

Error encode_body(const ObjectRemove& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_object_id(m.id));
  if (static_cast<std::uint8_t>(m.reason) > static_cast<std::uint8_t>(RemoveReason::picked_up)) {
    return Error::bad_value;
  }
  if (!all_finite(m.position)) return Error::bad_value;
  w.u8(m.player_number);
  write_object_id(m.id, w);
  w.u16(m.epoch);
  w.u8(static_cast<std::uint8_t>(m.reason));
  for (float f : m.position) w.f32(f);
  return Error::none;
}

Error encode_body(const Damage& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_object_id(m.id));
  if (m.amount < 0 || !all_finite(m.position)) return Error::bad_value;
  w.u8(m.player_number);
  write_object_id(m.id, w);
  w.u16(m.epoch);
  w.u32(static_cast<std::uint32_t>(m.amount));
  w.u8(m.cause);
  for (float f : m.position) w.f32(f);
  return Error::none;
}


// Bones in increasing order; per bone u16 bone, u8 parts, u8 flags, then the
// parts it has: rotation 4 x s16, translation 3 x s16, scale 3 x s16. -32768
// is refused in a rotation, so each value fits [-1, 1].
Error encode_body(const PlayerPose& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  if (m.bones.size() > kMaxPoseBones) return Error::bad_value;
  for (std::size_t i = 0; i < m.bones.size(); ++i) {
    const PoseBone& b = m.bones[i];
    if (b.bone >= kMaxPoseBones) return Error::bad_value;
    if (i > 0 && b.bone <= m.bones[i - 1].bone) return Error::bad_value;
    if (b.rotation) {
      for (std::int16_t v : *b.rotation) {
        if (v == std::numeric_limits<std::int16_t>::min()) return Error::bad_value;
      }
    }
  }
  w.u8(m.player_number);
  w.u64(m.time.server_time_us);
  w.u32(m.time.sequence);
  w.u32(m.skeleton);
  w.u16(static_cast<std::uint16_t>(m.bones.size()));
  for (const PoseBone& b : m.bones) {
    const std::uint8_t parts =
        (b.rotation ? pose_part::rotation : 0) | (b.translation ? pose_part::translation : 0) |
        (b.scale ? pose_part::scale : 0);
    w.u16(b.bone);
    w.u8(parts);
    w.u8(b.flags);
    if (b.rotation) for (std::int16_t v : *b.rotation) w.u16(static_cast<std::uint16_t>(v));
    if (b.translation) for (std::int16_t v : *b.translation) w.u16(static_cast<std::uint16_t>(v));
    if (b.scale) for (std::int16_t v : *b.scale) w.u16(static_cast<std::uint16_t>(v));
  }
  return Error::none;
}
// ---------------------------------------------------------------- bodies: decode

Error decode_body(Reader& r, Hello& m) {
  COOP_READ(r.u16(m.protocol_version));
  COOP_TRY(r.str(m.build_id, kMaxBuildIdChars));
  COOP_TRY(r.str(m.game_build_id, kMaxGameBuildIdChars));
  COOP_TRY(r.str(m.game_version, kMaxGameVersionChars));
  COOP_TRY(r.str(m.nickname, kMaxNicknameChars));
  if (m.nickname.empty()) return Error::bad_value;
  return Error::none;
}

Error decode_body(Reader& r, Welcome& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player(m.player_number)) return Error::bad_value;
  COOP_READ(r.u64(m.server_time_us));
  COOP_READ(r.u16(m.tick_rate));
  if (m.tick_rate == 0) return Error::bad_value;
  std::uint8_t count = 0;
  COOP_READ(r.u8(count));
  if (count > kMaxPlayers) return Error::bad_value;
  m.players.clear();
  m.players.resize(count);
  // The new player and those already in each have their own number.
  std::uint32_t taken = 1u << m.player_number;
  for (auto& p : m.players) {
    COOP_READ(r.u8(p.number));
    if (!valid_player(p.number) || (taken & (1u << p.number))) return Error::bad_value;
    taken |= 1u << p.number;
    COOP_TRY(r.str(p.nickname, kMaxNicknameChars));
    if (p.nickname.empty()) return Error::bad_value;
  }
  return Error::none;
}

Error decode_body(Reader& r, Refused& m) {
  std::uint8_t v = 0;
  COOP_READ(r.u8(v));
  m.reason = static_cast<RefuseReason>(v);
  return Error::none;
}

template <std::size_t N>
Error read_array(Reader& r, std::array<std::uint8_t, N>& out) {
  std::span<const std::uint8_t> part;
  if (!r.take(N, part)) return Error::truncated;
  std::copy(part.begin(), part.end(), out.begin());
  return Error::none;
}

Error decode_body(Reader& r, JoinChallenge& m) { return read_array(r, m.nonce); }

Error decode_body(Reader& r, JoinProof& m) { return read_array(r, m.proof); }

Error decode_body(Reader& r, PlayerJoined& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player(m.player_number)) return Error::bad_value;
  COOP_TRY(r.str(m.nickname, kMaxNicknameChars));
  if (m.nickname.empty()) return Error::bad_value;
  return Error::none;
}

Error decode_body(Reader& r, PlayerLeft& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player(m.player_number)) return Error::bad_value;
  std::uint8_t v = 0;
  COOP_READ(r.u8(v));
  m.reason = static_cast<LeaveReason>(v);
  return Error::none;
}

Error decode_body(Reader& r, Ping& m) {
  COOP_READ(r.u64(m.sender_time_us));
  return Error::none;
}

Error decode_body(Reader& r, Pong& m) {
  COOP_READ(r.u64(m.ping_sender_time_us));
  COOP_READ(r.u64(m.receiver_time_us));
  return Error::none;
}

Error decode_body(Reader& r, PlayerStatus& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_READ(r.u8(m.flags));
  return Error::none;
}

Error decode_body(Reader& r, Chat& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(r.str(m.text, kMaxChatChars));
  return Error::none;
}

template <std::size_t N>
Error read_floats(Reader& c, std::array<float, N>& v) {
  for (float& f : v) {
    COOP_READ(c.f32(f));
    if (!std::isfinite(f)) return Error::bad_value;
  }
  return Error::none;
}

Error decode_component(SampleComponent id, Reader& c, PlayerSample& m, bool& known) {
  known = true;
  switch (id) {
    case SampleComponent::time:
      COOP_READ(c.u64(m.time.server_time_us));
      COOP_READ(c.u32(m.time.sequence));
      return Error::none;
    case SampleComponent::position:
      return read_floats(c, m.position.emplace());
    case SampleComponent::velocity:
      return read_floats(c, m.velocity.emplace());
    case SampleComponent::rotation:
      return read_floats(c, m.rotation.emplace());
    case SampleComponent::scene:
      return c.str(m.scene.emplace(), kMaxSceneChars);
    case SampleComponent::age: {
      std::uint8_t v = 0;
      COOP_READ(c.u8(v));
      if (v > 1) return Error::bad_value;
      m.age = static_cast<Age>(v);
      return Error::none;
    }
    case SampleComponent::animation: {
      auto& a = m.animation.emplace();
      COOP_TRY(c.str(a.name, kMaxAnimationChars));
      COOP_READ(c.f32(a.frame));
      COOP_READ(c.f32(a.rate));
      if (!std::isfinite(a.frame) || !std::isfinite(a.rate)) return Error::bad_value;
      return Error::none;
    }
  }
  known = false;
  return Error::none;
}

Error decode_body(Reader& r, PlayerSample& m) {
  m = PlayerSample{};
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;

  std::uint32_t seen = 0;  // bit per known component id
  while (r.left() > 0) {
    std::uint16_t id = 0;
    std::uint16_t length = 0;
    COOP_READ(r.u16(id));
    COOP_READ(r.u16(length));
    std::span<const std::uint8_t> data;
    if (!r.take(length, data)) return Error::bad_length;

    const bool may_be_known = id >= 1 && id <= 31;
    if (may_be_known && (seen & (1u << id))) return Error::duplicate_component;

    Reader c(data);
    bool known = false;
    const Error e = decode_component(static_cast<SampleComponent>(id), c, m, known);
    if (!known) continue;  // unknown component: skipped by its length
    // A known component's fields running past its length means its length
    // is wrong, not that the message was cut short.
    if (e == Error::truncated) return Error::bad_length;
    if (e != Error::none) return e;
    if (c.left() != 0) return Error::bad_length;
    seen |= 1u << id;
  }
  if (!(seen & (1u << static_cast<unsigned>(SampleComponent::time)))) {
    return Error::missing_component;
  }
  return Error::none;
}

Error decode_body(Reader& r, TestPayload& m) {
  COOP_READ(r.u32(m.sequence));
  std::span<const std::uint8_t> rest;
  r.take(r.left(), rest);
  m.filler.assign(rest.begin(), rest.end());
  return Error::none;
}

Error decode_body(Reader& r, JoinPartner& m) {
  m = JoinPartner{};
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_READ(r.u8(m.other_player));
  if (!valid_player(m.other_player) || m.other_player == m.player_number) {
    return Error::bad_value;
  }
  COOP_READ(r.u16(m.request_id));
  std::uint8_t kind = 0;
  COOP_READ(r.u8(kind));
  switch (static_cast<JoinKind>(kind)) {
    case JoinKind::request:
      break;
    case JoinKind::accept: {
      auto& t = m.target.emplace();
      COOP_TRY(r.str(t.scene, kMaxSceneChars));
      if (t.scene.empty()) return Error::bad_value;
      COOP_TRY(read_floats(r, t.position));
      COOP_TRY(read_floats(r, t.rotation));
      break;
    }
    case JoinKind::refuse: {
      std::uint8_t v = 0;
      COOP_READ(r.u8(v));
      m.reason = static_cast<JoinRefuseReason>(v);
      break;
    }
    default:
      return Error::bad_value;  // the rest of the body cannot be read
  }
  m.kind = static_cast<JoinKind>(kind);
  return Error::none;
}

Error read_flags(Reader& r, std::vector<FlagEntry>& flags, std::size_t min_count,
                 Deltas deltas) {
  std::uint16_t count = 0;
  COOP_READ(r.u16(count));
  if (count > kMaxFlagEntries || count < min_count) return Error::bad_value;
  if (r.left() < count * kFlagEntrySize) return Error::truncated;
  flags.resize(count);
  for (auto& e : flags) {
    std::uint8_t key_kind = 0;
    std::uint8_t kind = 0;
    COOP_READ(r.u8(key_kind));
    switch (static_cast<FlagKeyKind>(key_kind)) {
      case FlagKeyKind::named:
        COOP_READ(r.u32(e.key.hash));
        COOP_READ(r.u16(e.key.index));
        break;
      case FlagKeyKind::key64:
        e.key.kind = FlagKeyKind::key64;
        COOP_READ(r.u64(e.key.key));
        COOP_READ(r.u32(e.key.table));
        break;
      default:
        return Error::bad_value;  // the rest of the list cannot be read
    }
    COOP_READ(r.u8(kind));
    COOP_READ(r.u32(e.value));
    e.kind = static_cast<FlagKind>(kind);
    COOP_TRY(check_flag(e));
    COOP_TRY(check_delta(e, deltas));
  }
  if (has_duplicate(flags)) return Error::bad_value;
  return Error::none;
}

Error decode_body(Reader& r, FlagSet& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  return read_flags(r, m.flags, 1, Deltas::allowed);
}

Error decode_body(Reader& r, FlagState& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  return read_flags(r, m.flags, 1, Deltas::refused);
}

Error decode_body(Reader& r, FlagReset& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  return read_flags(r, m.flags, 1, Deltas::refused);
}

Error decode_body(Reader& r, SaveAsk& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  return Error::none;
}

Error decode_body(Reader& r, SavePart& m) {
  std::uint8_t reason = 0;
  COOP_READ(r.u8(m.player_number));
  COOP_READ(r.u8(m.to_player));
  COOP_READ(r.u32(m.transfer_id));
  COOP_READ(r.u8(reason));
  COOP_READ(r.u32(m.total_size));
  COOP_READ(r.u32(m.crc32));
  COOP_READ(r.u16(m.index));
  COOP_READ(r.u16(m.count));
  m.reason = static_cast<SaveReason>(reason);
  std::span<const std::uint8_t> rest;
  r.take(r.left(), rest);
  m.data.assign(rest.begin(), rest.end());
  return valid_save_part(m) ? Error::none : Error::bad_value;
}

Error decode_body(Reader& r, FlagOrigin& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player(m.player_number)) return Error::bad_value;
  return read_flags(r, m.flags, 1, Deltas::allowed);
}

Error decode_body(Reader& r, FlagSnapshot& m) {
  COOP_READ(r.u16(m.part));
  COOP_READ(r.u16(m.part_count));
  if (m.part_count == 0 || m.part >= m.part_count) return Error::bad_value;
  return read_flags(r, m.flags, 0, Deltas::refused);
}

Error decode_body(Reader& r, Host& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player(m.player_number)) return Error::bad_value;
  return Error::none;
}

Error decode_body(Reader& r, PlayerEquip& m) {
  m = PlayerEquip{};
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;

  std::uint32_t seen = 0;  // bit per known component id
  while (r.left() > 0) {
    std::uint16_t id = 0;
    std::uint16_t length = 0;
    COOP_READ(r.u16(id));
    COOP_READ(r.u16(length));
    std::span<const std::uint8_t> data;
    if (!r.take(length, data)) return Error::bad_length;

    Reader c(data);
    std::optional<std::string>* name = nullptr;
    for (const EquipName& n : kEquipNames) {
      if (static_cast<std::uint16_t>(n.id) == id) name = &(m.equipment.*n.field);
    }
    const bool is_drawn = id == static_cast<std::uint16_t>(EquipComponent::drawn);
    if (name == nullptr && !is_drawn) continue;  // unknown: skipped by its length
    if (seen & (1u << id)) return Error::duplicate_component;
    seen |= 1u << id;

    Error e = Error::none;
    if (name != nullptr) {
      e = c.str(name->emplace(), kMaxEquipNameChars);
    } else {
      std::uint8_t bits = 0;
      e = c.u8(bits) ? Error::none : Error::truncated;
      m.equipment.drawn = bits;
    }
    // A known component's fields running past its length means its length
    // is wrong, not that the message was cut short.
    if (e == Error::truncated) return Error::bad_length;
    if (e != Error::none) return e;
    if (c.left() != 0) return Error::bad_length;
  }
  return Error::none;
}

// ---------------------------------------------------------------- objects: decode

Error read_object_id(Reader& r, ObjectId& id) {
  std::uint8_t kind = 0;
  COOP_READ(r.u8(kind));
  COOP_READ(r.u8(id.maker));
  COOP_READ(r.u64(id.value));
  if (kind > 1) return Error::bad_value;
  id.kind = static_cast<ObjectKind>(kind);
  return check_object_id(id);
}

Error decode_component(ObjectComponent id, Reader& c, ObjectState& s, bool& known) {
  known = true;
  switch (id) {
    case ObjectComponent::time:
      COOP_READ(c.u64(s.time.server_time_us));
      COOP_READ(c.u32(s.time.sequence));
      return Error::none;
    case ObjectComponent::position:
      return read_floats(c, s.position.emplace());
    case ObjectComponent::velocity:
      return read_floats(c, s.velocity.emplace());
    case ObjectComponent::rotation:
      return read_floats(c, s.rotation.emplace());
    case ObjectComponent::animation: {
      auto& a = s.animation.emplace();
      COOP_TRY(c.str(a.name, kMaxAnimationChars));
      COOP_READ(c.f32(a.frame));
      COOP_READ(c.f32(a.rate));
      if (!std::isfinite(a.frame) || !std::isfinite(a.rate)) return Error::bad_value;
      return Error::none;
    }
    case ObjectComponent::life: {
      std::uint32_t life = 0;
      std::uint32_t max = 0;
      COOP_READ(c.u32(life));
      COOP_READ(c.u32(max));
      s.life = ObjectLife{static_cast<std::int32_t>(life), static_cast<std::int32_t>(max)};
      return Error::none;
    }
    case ObjectComponent::fuse:
      COOP_READ(c.f32(s.fuse.emplace()));
      return std::isfinite(*s.fuse) ? Error::none : Error::bad_value;
    case ObjectComponent::state:
      COOP_READ(c.u8(s.state.emplace()));
      return Error::none;
  }
  known = false;
  return Error::none;
}

// The rest of the body: components, as in PLAYER_SAMPLE. time is required.
Error read_object_state(Reader& r, ObjectState& s) {
  s = ObjectState{};
  std::uint32_t seen = 0;
  while (r.left() > 0) {
    std::uint16_t id = 0;
    std::uint16_t length = 0;
    COOP_READ(r.u16(id));
    COOP_READ(r.u16(length));
    std::span<const std::uint8_t> data;
    if (!r.take(length, data)) return Error::bad_length;
    const bool may_be_known = id >= 1 && id <= 31;
    if (may_be_known && (seen & (1u << id))) return Error::duplicate_component;
    Reader c(data);
    bool known = false;
    const Error e = decode_component(static_cast<ObjectComponent>(id), c, s, known);
    if (!known) continue;  // unknown component: skipped by its length
    if (e == Error::truncated) return Error::bad_length;
    if (e != Error::none) return e;
    if (c.left() != 0) return Error::bad_length;
    seen |= 1u << id;
  }
  if (!(seen & (1u << static_cast<unsigned>(ObjectComponent::time)))) {
    return Error::missing_component;
  }
  return Error::none;
}

Error decode_body(Reader& r, ObjectPresent& m) {
  m = ObjectPresent{};
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(r.str(m.scene, kMaxSceneChars));
  std::uint16_t count = 0;
  COOP_READ(r.u16(count));
  if (count > kMaxPresentObjects) return Error::bad_value;
  if (r.left() < std::size_t{count} * 11) return Error::truncated;
  m.objects.resize(count);
  for (ObjectPresence& o : m.objects) {
    COOP_TRY(read_object_id(r, o.id));
    COOP_READ(r.u8(o.flags));
  }
  if (has_duplicate_id(m.objects)) return Error::bad_value;
  return Error::none;
}

Error decode_body(Reader& r, ObjectGroup& m) {
  m = ObjectGroup{};
  COOP_READ(r.u8(m.player_number));
  std::uint16_t count = 0;
  COOP_READ(r.u16(count));
  if (count == 0 || count > kMaxGroupEntries) return Error::bad_value;
  if (r.left() < std::size_t{count} * 21) return Error::truncated;
  m.entries.resize(count);
  for (ObjectGroupEntry& e : m.entries) {
    COOP_TRY(read_object_id(r, e.member));
    COOP_TRY(read_object_id(r, e.root));
    COOP_READ(r.u8(e.flags));
  }
  return check_group(m);
}

Error decode_body(Reader& r, ObjectOwner& m) {
  COOP_TRY(read_object_id(r, m.id));
  COOP_READ(r.u8(m.owner));
  COOP_READ(r.u16(m.epoch));
  COOP_READ(r.u32(m.group));
  if (!valid_player_or_self(m.owner)) return Error::bad_value;
  return Error::none;
}

Error decode_body(Reader& r, HandoverAsk& m) {
  COOP_TRY(read_object_id(r, m.id));
  COOP_READ(r.u16(m.epoch));
  COOP_READ(r.u8(m.to_player));
  if (!valid_player(m.to_player)) return Error::bad_value;
  return Error::none;
}

Error decode_body(Reader& r, HandoverState& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(read_object_id(r, m.id));
  COOP_READ(r.u16(m.epoch));
  return read_object_state(r, m.state);
}

Error decode_body(Reader& r, ObjectSpawn& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(read_object_id(r, m.id));
  COOP_TRY(r.str(m.actor, kMaxActorNameChars));
  if (m.actor.empty()) return Error::bad_value;
  COOP_READ(r.u8(m.flags));
  return read_object_state(r, m.state);
}

Error decode_body(Reader& r, ObjectSample& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(read_object_id(r, m.id));
  COOP_READ(r.u16(m.epoch));
  return read_object_state(r, m.state);
}

Error decode_body(Reader& r, ObjectRemove& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(read_object_id(r, m.id));
  COOP_READ(r.u16(m.epoch));
  std::uint8_t reason = 0;
  COOP_READ(r.u8(reason));
  if (reason > static_cast<std::uint8_t>(RemoveReason::picked_up)) return Error::bad_value;
  m.reason = static_cast<RemoveReason>(reason);
  return read_floats(r, m.position);
}

Error decode_body(Reader& r, Damage& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(read_object_id(r, m.id));
  COOP_READ(r.u16(m.epoch));
  std::uint32_t amount = 0;
  COOP_READ(r.u32(amount));
  m.amount = static_cast<std::int32_t>(amount);
  if (m.amount < 0) return Error::bad_value;
  COOP_READ(r.u8(m.cause));
  return read_floats(r, m.position);
}

template <std::size_t N>
Error read_s16s(Reader& r, std::array<std::int16_t, N>& v) {
  for (std::int16_t& x : v) {
    std::uint16_t raw = 0;
    if (!r.u16(raw)) return Error::truncated;
    x = static_cast<std::int16_t>(raw);
  }
  return Error::none;
}

// u8 player, u32 seconds since midnight (below one day), u8 flags (bit 0
// jump, the others 0).
Error encode_body(const TimeOfDay& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  if (m.seconds >= kSecondsPerDay || (m.flags & ~time_of_day::jump) != 0) return Error::bad_value;
  w.u8(m.player_number);
  w.u32(m.seconds);
  w.u8(m.flags);
  return Error::none;
}

Error decode_body(Reader& r, TimeOfDay& m) {
  m = TimeOfDay{};
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_READ(r.u32(m.seconds));
  COOP_READ(r.u8(m.flags));
  if (m.seconds >= kSecondsPerDay || (m.flags & ~time_of_day::jump) != 0) return Error::bad_value;
  return Error::none;
}

// u8 player, str scene (1 to 64 chars), 3 x f32 position (finite), f32
// radius (above 0, at most 100), u8 flags, 9 x f32 sweep (finite or NaN).
bool finite_or_nan(float f) { return std::isfinite(f) || std::isnan(f); }
bool same_float(float a, float b) {
  return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b) ||
         (std::isnan(a) && std::isnan(b)) || a == b;
}
Error check_grass_cut(const GrassCut& m) {
  if (m.scene.empty()) return Error::bad_value;
  COOP_TRY(check_string(m.scene, kMaxSceneChars));
  if (!all_finite(m.position) || !std::isfinite(m.radius) || m.radius <= 0.0f ||
      m.radius > kMaxGrassCutRadius) {
    return Error::bad_value;
  }
  for (const Vec3& v : m.sweep) {
    for (float f : v) {
      if (!finite_or_nan(f)) return Error::bad_value;
    }
  }
  return Error::none;
}

Error encode_body(const GrassCut& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_grass_cut(m));
  w.u8(m.player_number);
  w.str(m.scene);
  for (float f : m.position) w.f32(f);
  w.f32(m.radius);
  w.u8(m.flags);
  for (const Vec3& v : m.sweep) {
    for (float f : v) w.f32(f);
  }
  return Error::none;
}

Error decode_body(Reader& r, GrassCut& m) {
  m = GrassCut{};
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(r.str(m.scene, kMaxSceneChars));
  for (float& f : m.position) COOP_READ(r.f32(f));
  COOP_READ(r.f32(m.radius));
  COOP_READ(r.u8(m.flags));
  for (Vec3& v : m.sweep) {
    for (float& f : v) COOP_READ(r.f32(f));
  }
  return check_grass_cut(m);
}

// The shared head of the WORLD_* messages: u8 player, str scene (1 to 64
// chars), u64 placement hash (not 0, not ~0).
Error check_world(std::uint8_t player, const std::string& scene, std::uint64_t hash) {
  if (!valid_player_or_self(player) || scene.empty() || hash == 0 ||
      hash == ~std::uint64_t{0}) {
    return Error::bad_value;
  }
  return check_string(scene, kMaxSceneChars);
}

bool unit_quat(const Quat& q) {
  if (!all_finite(q)) return false;
  const float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
  return n > 0.98f && n < 1.02f;
}

// head, 3 x f32 position (finite), u8 flags.
Error encode_body(const WorldBreak& m, Writer& w) {
  COOP_TRY(check_world(m.player_number, m.scene, m.hash));
  if (!all_finite(m.position) || (m.flags & ~(world_break::restored | world_break::kept)) != 0 ||
      (m.flags & world_break::restored && m.flags & world_break::kept)) return Error::bad_value;
  w.u8(m.player_number);
  w.str(m.scene);
  w.u64(m.hash);
  for (float f : m.position) w.f32(f);
  w.u8(m.flags);
  return Error::none;
}

Error decode_body(Reader& r, WorldBreak& m) {
  m = WorldBreak{};
  COOP_READ(r.u8(m.player_number));
  COOP_TRY(r.str(m.scene, kMaxSceneChars));
  COOP_READ(r.u64(m.hash));
  for (float& f : m.position) COOP_READ(r.f32(f));
  COOP_READ(r.u8(m.flags));
  COOP_TRY(check_world(m.player_number, m.scene, m.hash));
  if (!all_finite(m.position) || (m.flags & ~(world_break::restored | world_break::kept)) != 0 ||
      (m.flags & world_break::restored && m.flags & world_break::kept)) return Error::bad_value;
  return Error::none;
}

// head, u64 server time, 3 x f32 position (finite), 4 x f32 rotation (unit
// quaternion x y z w), u8 flags.
Error encode_body(const WorldMove& m, Writer& w) {
  COOP_TRY(check_world(m.player_number, m.scene, m.hash));
  if (!all_finite(m.position) || !unit_quat(m.rotation) || (m.flags & ~world_move::carried) != 0) {
    return Error::bad_value;
  }
  w.u8(m.player_number);
  w.str(m.scene);
  w.u64(m.hash);
  w.u64(m.time_us);
  for (float f : m.position) w.f32(f);
  for (float f : m.rotation) w.f32(f);
  w.u8(m.flags);
  return Error::none;
}

Error decode_body(Reader& r, WorldMove& m) {
  m = WorldMove{};
  COOP_READ(r.u8(m.player_number));
  COOP_TRY(r.str(m.scene, kMaxSceneChars));
  COOP_READ(r.u64(m.hash));
  COOP_READ(r.u64(m.time_us));
  for (float& f : m.position) COOP_READ(r.f32(f));
  for (float& f : m.rotation) COOP_READ(r.f32(f));
  COOP_READ(r.u8(m.flags));
  COOP_TRY(check_world(m.player_number, m.scene, m.hash));
  if (!all_finite(m.position) || !unit_quat(m.rotation) || (m.flags & ~world_move::carried) != 0) {
    return Error::bad_value;
  }
  return Error::none;
}

// head, 3 x f32 position (finite), 4 x f32 rotation (unit quaternion).
Error encode_body(const WorldPlace& m, Writer& w) {
  COOP_TRY(check_world(m.player_number, m.scene, m.hash));
  if (!all_finite(m.position) || !unit_quat(m.rotation)) return Error::bad_value;
  w.u8(m.player_number);
  w.str(m.scene);
  w.u64(m.hash);
  for (float f : m.position) w.f32(f);
  for (float f : m.rotation) w.f32(f);
  return Error::none;
}

Error decode_body(Reader& r, WorldPlace& m) {
  m = WorldPlace{};
  COOP_READ(r.u8(m.player_number));
  COOP_TRY(r.str(m.scene, kMaxSceneChars));
  COOP_READ(r.u64(m.hash));
  for (float& f : m.position) COOP_READ(r.f32(f));
  for (float& f : m.rotation) COOP_READ(r.f32(f));
  COOP_TRY(check_world(m.player_number, m.scene, m.hash));
  if (!all_finite(m.position) || !unit_quat(m.rotation)) return Error::bad_value;
  return Error::none;
}

// head, 3 x f32 at (finite), 3 x f32 from (finite).
Error encode_body(const WorldHit& m, Writer& w) {
  COOP_TRY(check_world(m.player_number, m.scene, m.hash));
  if (!all_finite(m.at) || !all_finite(m.from)) return Error::bad_value;
  w.u8(m.player_number);
  w.str(m.scene);
  w.u64(m.hash);
  for (float f : m.at) w.f32(f);
  for (float f : m.from) w.f32(f);
  return Error::none;
}

Error decode_body(Reader& r, WorldHit& m) {
  m = WorldHit{};
  COOP_READ(r.u8(m.player_number));
  COOP_TRY(r.str(m.scene, kMaxSceneChars));
  COOP_READ(r.u64(m.hash));
  for (float& f : m.at) COOP_READ(r.f32(f));
  for (float& f : m.from) COOP_READ(r.f32(f));
  COOP_TRY(check_world(m.player_number, m.scene, m.hash));
  if (!all_finite(m.at) || !all_finite(m.from)) return Error::bad_value;
  return Error::none;
}

// SIGN_CUT: head, u16 count, then count x (u64 key, u64 value).
Error check_sign_cut(const SignCut& m) {
  COOP_TRY(check_world(m.player_number, m.scene, m.hash));
  if (m.words.empty() || m.words.size() > kMaxSignCutWords) return Error::bad_value;
  for (const SignWord& w : m.words) {
    const std::uint64_t region = w.key >> 32;
    const std::uint64_t offset = w.key & 0xffffffffu;
    if (region < 2 || region - 2 >= kSignCutEntries || offset >= kSignCutBytes || offset % 8 != 0) {
      return Error::bad_value;
    }
  }
  return Error::none;
}

Error encode_body(const SignCut& m, Writer& w) {
  COOP_TRY(check_sign_cut(m));
  w.u8(m.player_number);
  w.str(m.scene);
  w.u64(m.hash);
  w.u16(static_cast<std::uint16_t>(m.words.size()));
  for (const SignWord& x : m.words) {
    w.u64(x.key);
    w.u64(x.value);
  }
  return Error::none;
}

Error decode_body(Reader& r, SignCut& m) {
  m = SignCut{};
  COOP_READ(r.u8(m.player_number));
  COOP_TRY(r.str(m.scene, kMaxSceneChars));
  COOP_READ(r.u64(m.hash));
  std::uint16_t count = 0;
  COOP_READ(r.u16(count));
  if (count == 0 || count > kMaxSignCutWords) return Error::bad_value;
  m.words.resize(count);
  for (SignWord& x : m.words) {
    COOP_READ(r.u64(x.key));
    COOP_READ(r.u64(x.value));
  }
  return check_sign_cut(m);
}

// DROP_SPAWN: u8 player, object id (made), scene, actor, position, u64 time,
// u8 flags (bits 0 and 1, the others 0).
Error encode_body(const DropSpawn& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_object_id(m.id));
  if (m.id.kind != ObjectKind::made) return Error::bad_value;
  COOP_TRY(check_string(m.scene, kMaxSceneChars));
  COOP_TRY(check_string(m.actor, kMaxActorNameChars));
  if (m.scene.empty() || m.actor.empty() || !all_finite(m.position)) return Error::bad_value;
  if ((m.flags & ~(drop_spawn::owned | drop_spawn::again)) != 0) return Error::bad_value;
  w.u8(m.player_number);
  write_object_id(m.id, w);
  w.str(m.scene);
  w.str(m.actor);
  for (float f : m.position) w.f32(f);
  w.u64(m.time_us);
  w.u8(m.flags);
  return Error::none;
}

Error decode_body(Reader& r, DropSpawn& m) {
  m = DropSpawn{};
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(read_object_id(r, m.id));
  if (m.id.kind != ObjectKind::made) return Error::bad_value;
  COOP_TRY(r.str(m.scene, kMaxSceneChars));
  COOP_TRY(r.str(m.actor, kMaxActorNameChars));
  if (m.scene.empty() || m.actor.empty()) return Error::bad_value;
  COOP_TRY(read_floats(r, m.position));
  COOP_READ(r.u64(m.time_us));
  COOP_READ(r.u8(m.flags));
  if ((m.flags & ~(drop_spawn::owned | drop_spawn::again)) != 0) return Error::bad_value;
  return Error::none;
}

// DROP_TAKE: u8 player, object id (made), u8 reason (0 picked up, 1 gone).
Error encode_body(const DropTake& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_object_id(m.id));
  if (m.id.kind != ObjectKind::made) return Error::bad_value;
  if (static_cast<std::uint8_t>(m.reason) > static_cast<std::uint8_t>(DropTakeReason::gone)) {
    return Error::bad_value;
  }
  w.u8(m.player_number);
  write_object_id(m.id, w);
  w.u8(static_cast<std::uint8_t>(m.reason));
  return Error::none;
}

Error decode_body(Reader& r, DropTake& m) {
  m = DropTake{};
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(read_object_id(r, m.id));
  if (m.id.kind != ObjectKind::made) return Error::bad_value;
  std::uint8_t reason = 0;
  COOP_READ(r.u8(reason));
  if (reason > static_cast<std::uint8_t>(DropTakeReason::gone)) return Error::bad_value;
  m.reason = static_cast<DropTakeReason>(reason);
  return Error::none;
}

// DROP_ASK: u8 player, scene.
Error encode_body(const DropAsk& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_string(m.scene, kMaxSceneChars));
  if (m.scene.empty()) return Error::bad_value;
  w.u8(m.player_number);
  w.str(m.scene);
  return Error::none;
}

Error decode_body(Reader& r, DropAsk& m) {
  m = DropAsk{};
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(r.str(m.scene, kMaxSceneChars));
  if (m.scene.empty()) return Error::bad_value;
  return Error::none;
}

// BOSS_ROOM: u8 player, scene, u8 room (0 to 15), u8 state (1 to 5).
Error encode_body(const BossRoom& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_string(m.scene, kMaxSceneChars));
  if (m.scene.empty() || m.room > kMaxBossRoom) return Error::bad_value;
  const auto state = static_cast<std::uint8_t>(m.state);
  if (state < 1 || state > 5) return Error::bad_value;
  w.u8(m.player_number);
  w.str(m.scene);
  w.u8(m.room);
  w.u8(state);
  return Error::none;
}

Error decode_body(Reader& r, BossRoom& m) {
  m = BossRoom{};
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(r.str(m.scene, kMaxSceneChars));
  std::uint8_t state = 0;
  COOP_READ(r.u8(m.room));
  COOP_READ(r.u8(state));
  if (m.scene.empty() || m.room > kMaxBossRoom || state < 1 || state > 5) return Error::bad_value;
  m.state = static_cast<BossState>(state);
  return Error::none;
}

Error decode_body(Reader& r, PlayerPose& m) {  m = PlayerPose{};
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_READ(r.u64(m.time.server_time_us));
  COOP_READ(r.u32(m.time.sequence));
  COOP_READ(r.u32(m.skeleton));
  std::uint16_t count = 0;
  COOP_READ(r.u16(count));
  if (count > kMaxPoseBones) return Error::bad_value;
  if (r.left() < std::size_t{count} * 4) return Error::truncated;
  m.bones.resize(count);
  for (std::size_t i = 0; i < m.bones.size(); ++i) {
    PoseBone& b = m.bones[i];
    std::uint8_t parts = 0;
    COOP_READ(r.u16(b.bone));
    COOP_READ(r.u8(parts));
    COOP_READ(r.u8(b.flags));
    if (b.bone >= kMaxPoseBones || (i > 0 && b.bone <= m.bones[i - 1].bone)) {
      return Error::bad_value;
    }
    if ((parts & ~(pose_part::rotation | pose_part::translation | pose_part::scale)) != 0) {
      return Error::bad_value;
    }
    if (parts & pose_part::rotation) {
      COOP_TRY(read_s16s(r, b.rotation.emplace()));
      for (std::int16_t v : *b.rotation) {
        if (v == std::numeric_limits<std::int16_t>::min()) return Error::bad_value;
      }
    }
    if (parts & pose_part::translation) COOP_TRY(read_s16s(r, b.translation.emplace()));
    if (parts & pose_part::scale) COOP_TRY(read_s16s(r, b.scale.emplace()));
  }
  return Error::none;
}

template <typename T>
Error decode_as(Reader& body, Message& out) {
  T value{};
  COOP_TRY(decode_body(body, value));
  if (body.left() != 0) return Error::trailing_bytes;
  out = std::move(value);
  return Error::none;
}

#undef COOP_READ
#undef COOP_TRY

}  // namespace

std::string_view to_string(Error error) {
  switch (error) {
    case Error::none: return "none";
    case Error::truncated: return "truncated";
    case Error::trailing_bytes: return "trailing bytes";
    case Error::bad_length: return "bad length";
    case Error::string_too_long: return "string too long";
    case Error::bad_utf8: return "bad UTF-8";
    case Error::bad_value: return "bad value";
    case Error::missing_component: return "missing component";
    case Error::duplicate_component: return "duplicate component";
    case Error::unknown_type: return "unknown message type";
    case Error::too_large: return "too large";
    case Error::wrong_channel: return "wrong channel";
    case Error::wrong_sender: return "wrong sender";
  }
  return "unknown error";
}

std::uint16_t save_part_count(std::uint32_t total_size) {
  if (total_size == 0 || total_size > kMaxSaveBytes) return 0;
  return static_cast<std::uint16_t>((total_size + kMaxSavePart - 1) / kMaxSavePart);
}

std::size_t save_part_size(std::uint32_t total_size, std::uint16_t index) {
  const std::uint16_t count = save_part_count(total_size);
  if (index >= count) return 0;
  if (index + 1 < count) return kMaxSavePart;
  return total_size - static_cast<std::size_t>(index) * kMaxSavePart;
}

MessageType type_of(const Message& message) {
  static constexpr MessageType kTypes[] = {
      MessageType::hello,         MessageType::welcome,     MessageType::refused,
      MessageType::player_joined, MessageType::player_left, MessageType::ping,
      MessageType::pong,          MessageType::player_status, MessageType::chat,
      MessageType::player_sample, MessageType::test_payload,
      MessageType::join_partner,  MessageType::flag_set,    MessageType::flag_state,
      MessageType::flag_snapshot, MessageType::host, MessageType::player_equip,
      MessageType::object_present, MessageType::object_owner, MessageType::handover_ask,
      MessageType::handover_state, MessageType::object_spawn, MessageType::object_sample,
      MessageType::object_remove, MessageType::damage, MessageType::player_pose,
      MessageType::time_of_day, MessageType::grass_cut, MessageType::object_group,
      MessageType::world_break,
      MessageType::world_move, MessageType::world_place, MessageType::world_hit,
      MessageType::drop_spawn, MessageType::drop_take, MessageType::drop_ask,
      MessageType::boss_room, MessageType::sign_cut, MessageType::flag_reset,
      MessageType::flag_origin, MessageType::join_challenge, MessageType::join_proof,
      MessageType::save_ask,    MessageType::save_part,
  };
  static_assert(std::size(kTypes) == std::variant_size_v<Message>);
  return kTypes[message.index()];
}

std::uint8_t channel_of(const Message& message) { return channel_of(type_of(message)); }

std::uint8_t channel_of(MessageType type) {
  switch (type) {
    case MessageType::player_sample:
    case MessageType::player_pose:
    case MessageType::test_payload:
      return kChannelPlayerSamples;
    case MessageType::flag_set:
    case MessageType::flag_state:
    case MessageType::flag_reset:
    case MessageType::flag_origin:
    case MessageType::flag_snapshot:
    case MessageType::host:
      return kChannelProgress;
    case MessageType::object_sample:
    case MessageType::world_move:
      return kChannelObjectSamples;
    case MessageType::object_present:
    case MessageType::object_owner:
    case MessageType::handover_ask:
    case MessageType::handover_state:
    case MessageType::object_spawn:
    case MessageType::object_remove:
    case MessageType::damage:
    case MessageType::object_group:
    case MessageType::drop_spawn:
    case MessageType::drop_take:
    case MessageType::drop_ask:
      return kChannelOwnership;
    default:
      return kChannelSession;
  }
}

bool allowed_on_channel(MessageType type, std::uint8_t channel) {
  switch (type) {
    case MessageType::hello:
    case MessageType::join_challenge:
    case MessageType::join_proof:
    case MessageType::welcome:
    case MessageType::refused:
    case MessageType::player_joined:
    case MessageType::player_left:
    case MessageType::ping:
    case MessageType::pong:
    case MessageType::player_status:
    case MessageType::chat:
    case MessageType::join_partner:
    case MessageType::player_equip:
    case MessageType::time_of_day:
    case MessageType::grass_cut:
    case MessageType::world_break:
    case MessageType::world_place:
    case MessageType::world_hit:
    case MessageType::boss_room:
    case MessageType::sign_cut:
    case MessageType::save_ask:
    case MessageType::save_part:
      return channel == kChannelSession;
    case MessageType::flag_set:
    case MessageType::flag_state:
    case MessageType::flag_reset:
    case MessageType::flag_origin:
    case MessageType::flag_snapshot:
    case MessageType::host:
      return channel == kChannelProgress;
    case MessageType::player_sample:
    case MessageType::player_pose:
      return channel == kChannelPlayerSamples;
    case MessageType::object_sample:
    case MessageType::world_move:
      return channel == kChannelObjectSamples;
    case MessageType::object_present:
    case MessageType::object_owner:
    case MessageType::handover_ask:
    case MessageType::handover_state:
    case MessageType::object_spawn:
    case MessageType::object_remove:
    case MessageType::damage:
    case MessageType::object_group:
    case MessageType::drop_spawn:
    case MessageType::drop_take:
    case MessageType::drop_ask:
      return channel == kChannelOwnership;
    case MessageType::test_payload:
      return channel == kChannelSession || channel == kChannelPlayerSamples;
  }
  return false;
}

bool may_send(Sender from, MessageType type) {
  switch (type) {
    case MessageType::hello:
    case MessageType::join_proof:
    case MessageType::object_group:
      return from == Sender::client;
    case MessageType::welcome:
    case MessageType::refused:
    case MessageType::join_challenge:
    case MessageType::player_joined:
    case MessageType::player_left:
    case MessageType::host:
    case MessageType::object_owner:
    case MessageType::handover_ask:
    case MessageType::flag_origin:
      return from == Sender::server;
    case MessageType::ping:
    case MessageType::pong:
    case MessageType::player_status:
    case MessageType::chat:
    case MessageType::player_sample:
    case MessageType::player_pose:
    case MessageType::test_payload:
    case MessageType::join_partner:
    case MessageType::flag_set:
    case MessageType::flag_state:
    case MessageType::flag_reset:
    case MessageType::flag_snapshot:
    case MessageType::player_equip:
    case MessageType::object_present:
    case MessageType::handover_state:
    case MessageType::object_spawn:
    case MessageType::object_sample:
    case MessageType::object_remove:
    case MessageType::damage:
    case MessageType::time_of_day:
    case MessageType::grass_cut:
    case MessageType::world_break:
    case MessageType::world_move:
    case MessageType::world_place:
    case MessageType::world_hit:
    case MessageType::drop_spawn:
    case MessageType::drop_take:
    case MessageType::drop_ask:
    case MessageType::boss_room:
    case MessageType::sign_cut:
    case MessageType::save_ask:
    case MessageType::save_part:
      return true;
  }
  return false;
}

namespace {

// The player field of the relayed messages, or null.
template <typename M>
auto* player_field(M& message) {
  using Field = std::conditional_t<std::is_const_v<M>, const std::uint8_t, std::uint8_t>;
  Field* field = nullptr;
  std::visit(
      [&field](auto& m) {
        using T = std::remove_cvref_t<decltype(m)>;
        if constexpr (std::is_same_v<T, PlayerStatus> || std::is_same_v<T, Chat> ||
                      std::is_same_v<T, PlayerSample> || std::is_same_v<T, JoinPartner> ||
                      std::is_same_v<T, FlagSet> || std::is_same_v<T, FlagState> ||
                      std::is_same_v<T, FlagReset> || std::is_same_v<T, FlagOrigin> ||
                      std::is_same_v<T, PlayerEquip> || std::is_same_v<T, ObjectPresent> ||
                      std::is_same_v<T, HandoverState> || std::is_same_v<T, ObjectSpawn> ||
                      std::is_same_v<T, ObjectSample> || std::is_same_v<T, ObjectRemove> ||
                      std::is_same_v<T, Damage> || std::is_same_v<T, PlayerPose> ||
                      std::is_same_v<T, TimeOfDay> || std::is_same_v<T, GrassCut> ||
                      std::is_same_v<T, ObjectGroup> ||
                      std::is_same_v<T, WorldBreak> || std::is_same_v<T, WorldMove> ||
                      std::is_same_v<T, WorldPlace> || std::is_same_v<T, WorldHit> ||
                      std::is_same_v<T, DropSpawn> || std::is_same_v<T, DropTake> ||
                      std::is_same_v<T, DropAsk> || std::is_same_v<T, BossRoom> ||
                      std::is_same_v<T, SignCut> || std::is_same_v<T, SaveAsk> ||
                      std::is_same_v<T, SavePart>) {
          field = &m.player_number;
        }
      },
      message);
  return field;
}

}  // namespace

Error check_received(const Message& message, Sender from, std::uint8_t channel) {
  const MessageType type = type_of(message);
  if (!may_send(from, type)) return Error::wrong_sender;
  if (!allowed_on_channel(type, channel)) return Error::wrong_channel;
  if (const std::uint8_t* player = player_field(message)) {
    const bool ok = from == Sender::client ? *player == kPlayerSelf : valid_player(*player);
    if (!ok) return Error::wrong_sender;
  }
  return Error::none;
}

std::optional<std::uint8_t> sender_of(const Message& message) {
  if (const std::uint8_t* player = player_field(message)) return *player;
  return std::nullopt;
}

bool set_sender(Message& message, std::uint8_t player) {
  std::uint8_t* field = player_field(message);
  if (field == nullptr || !valid_player(player)) return false;
  // A player cannot ask to join itself or answer itself.
  if (const auto* j = std::get_if<JoinPartner>(&message); j && j->other_player == player) {
    return false;
  }
  *field = player;
  return true;
}

Error encode(const Message& message, std::vector<std::uint8_t>& out) {
  const std::size_t start = out.size();
  Writer w(out);
  w.u16(static_cast<std::uint16_t>(type_of(message)));
  const std::size_t at = w.begin_length();
  Error e = std::visit([&w](const auto& m) { return encode_body(m, w); }, message);
  if (e == Error::none && !w.finish_length(at)) e = Error::too_large;
  if (e != Error::none) out.resize(start);
  return e;
}

std::optional<std::vector<std::uint8_t>> encode(const Message& message) {
  std::vector<std::uint8_t> out;
  if (encode(message, out) != Error::none) return std::nullopt;
  return out;
}

Error decode(std::span<const std::uint8_t> bytes, Message& out) {
  Reader r(bytes);
  std::uint16_t type = 0;
  std::uint16_t length = 0;
  if (!r.u16(type) || !r.u16(length)) return Error::truncated;
  if (length > r.left()) return Error::truncated;
  if (length < r.left()) return Error::trailing_bytes;
  std::span<const std::uint8_t> body_bytes;
  r.take(length, body_bytes);
  Reader body(body_bytes);

  switch (static_cast<MessageType>(type)) {
    case MessageType::hello: return decode_as<Hello>(body, out);
    case MessageType::welcome: return decode_as<Welcome>(body, out);
    case MessageType::refused: return decode_as<Refused>(body, out);
    case MessageType::join_challenge: return decode_as<JoinChallenge>(body, out);
    case MessageType::join_proof: return decode_as<JoinProof>(body, out);
    case MessageType::player_joined: return decode_as<PlayerJoined>(body, out);
    case MessageType::player_left: return decode_as<PlayerLeft>(body, out);
    case MessageType::ping: return decode_as<Ping>(body, out);
    case MessageType::pong: return decode_as<Pong>(body, out);
    case MessageType::player_status: return decode_as<PlayerStatus>(body, out);
    case MessageType::chat: return decode_as<Chat>(body, out);
    case MessageType::player_sample: return decode_as<PlayerSample>(body, out);
    case MessageType::test_payload: return decode_as<TestPayload>(body, out);
    case MessageType::join_partner: return decode_as<JoinPartner>(body, out);
    case MessageType::flag_set: return decode_as<FlagSet>(body, out);
    case MessageType::flag_state: return decode_as<FlagState>(body, out);
    case MessageType::flag_reset: return decode_as<FlagReset>(body, out);
    case MessageType::flag_origin: return decode_as<FlagOrigin>(body, out);
    case MessageType::save_ask: return decode_as<SaveAsk>(body, out);
    case MessageType::save_part: return decode_as<SavePart>(body, out);
    case MessageType::flag_snapshot: return decode_as<FlagSnapshot>(body, out);
    case MessageType::host: return decode_as<Host>(body, out);
    case MessageType::object_present: return decode_as<ObjectPresent>(body, out);
    case MessageType::object_owner: return decode_as<ObjectOwner>(body, out);
    case MessageType::handover_ask: return decode_as<HandoverAsk>(body, out);
    case MessageType::handover_state: return decode_as<HandoverState>(body, out);
    case MessageType::object_spawn: return decode_as<ObjectSpawn>(body, out);
    case MessageType::object_sample: return decode_as<ObjectSample>(body, out);
    case MessageType::object_remove: return decode_as<ObjectRemove>(body, out);
    case MessageType::damage: return decode_as<Damage>(body, out);
    case MessageType::player_pose: return decode_as<PlayerPose>(body, out);
    case MessageType::time_of_day: return decode_as<TimeOfDay>(body, out);
    case MessageType::object_group: return decode_as<ObjectGroup>(body, out);
    case MessageType::grass_cut: return decode_as<GrassCut>(body, out);
    case MessageType::world_break: return decode_as<WorldBreak>(body, out);
    case MessageType::world_move: return decode_as<WorldMove>(body, out);
    case MessageType::world_place: return decode_as<WorldPlace>(body, out);
    case MessageType::world_hit: return decode_as<WorldHit>(body, out);
    case MessageType::sign_cut: return decode_as<SignCut>(body, out);
    case MessageType::drop_spawn: return decode_as<DropSpawn>(body, out);
    case MessageType::drop_take: return decode_as<DropTake>(body, out);
    case MessageType::drop_ask: return decode_as<DropAsk>(body, out);
    case MessageType::boss_room: return decode_as<BossRoom>(body, out);
    case MessageType::player_equip: return decode_as<PlayerEquip>(body, out);
  }
  return Error::unknown_type;
}

std::optional<std::uint16_t> hello_protocol_version(
    std::span<const std::uint8_t> bytes) {
  Reader r(bytes);
  std::uint16_t type = 0;
  std::uint16_t length = 0;
  std::uint16_t version = 0;
  if (!r.u16(type) || !r.u16(length)) return std::nullopt;
  if (type != static_cast<std::uint16_t>(MessageType::hello)) return std::nullopt;
  if (length < 2 || !r.u16(version)) return std::nullopt;
  return version;
}

std::optional<RefuseReason> check_hello(const Hello& hello,
                                        std::string_view expected_game_build_id) {
  if (hello.protocol_version != kProtocolVersion) {
    return RefuseReason::wrong_protocol_version;
  }
  if (hello.game_build_id != expected_game_build_id) {
    return RefuseReason::wrong_game_build;
  }
  return std::nullopt;
}

bool GrassCut::operator==(const GrassCut& o) const {
  if (player_number != o.player_number || scene != o.scene || flags != o.flags ||
      !same_float(radius, o.radius)) {
    return false;
  }
  for (std::size_t i = 0; i < 3; ++i) {
    if (!same_float(position[i], o.position[i])) return false;
    for (std::size_t j = 0; j < 3; ++j) {
      if (!same_float(sweep[i][j], o.sweep[i][j])) return false;
    }
  }
  return true;
}

}  // namespace coop::protocol

namespace coop::protocol {

std::uint32_t flag_hash(std::string_view name) {
  // MurmurHash3_x86_32 (Austin Appleby, public domain), seed 0, written out
  // here so every build hashes a name the same way, whatever the host.
  constexpr std::uint32_t c1 = 0xcc9e2d51;
  constexpr std::uint32_t c2 = 0x1b873593;
  const auto* p = reinterpret_cast<const unsigned char*>(name.data());
  const std::size_t n = name.size();
  std::uint32_t h = 0;
  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    std::uint32_t k = static_cast<std::uint32_t>(p[i]) |
                      (static_cast<std::uint32_t>(p[i + 1]) << 8) |
                      (static_cast<std::uint32_t>(p[i + 2]) << 16) |
                      (static_cast<std::uint32_t>(p[i + 3]) << 24);
    k *= c1;
    k = std::rotl(k, 15);
    k *= c2;
    h ^= k;
    h = std::rotl(h, 13);
    h = h * 5 + 0xe6546b64;
  }
  std::uint32_t k = 0;
  switch (n & 3) {
    case 3: k ^= static_cast<std::uint32_t>(p[i + 2]) << 16; [[fallthrough]];
    case 2: k ^= static_cast<std::uint32_t>(p[i + 1]) << 8; [[fallthrough]];
    case 1:
      k ^= p[i];
      k *= c1;
      k = std::rotl(k, 15);
      k *= c2;
      h ^= k;
  }
  h ^= static_cast<std::uint32_t>(n);
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;
  return h;
}

bool valid_flag(const FlagEntry& e) {
  switch (e.key.kind) {
    case FlagKeyKind::named:
      if (e.key.key != 0 || e.key.table != 0) return false;
      break;
    case FlagKeyKind::key64:
      if (e.key.hash != 0 || e.key.index != 0) return false;
      if (e.kind != FlagKind::boolean) return false;
      break;
    default:
      return false;
  }
  switch (e.kind) {
    case FlagKind::boolean:
      return e.value <= 1;
    case FlagKind::int32:
    case FlagKind::uint32:
    case FlagKind::count_delta:  // key64 is refused above: always a bool
      return true;
    case FlagKind::float32:
      return std::isfinite(std::bit_cast<float>(e.value));
  }
  return false;
}

std::optional<std::vector<FlagSnapshot>> split_snapshot(
    std::span<const FlagEntry> flags) {
  const std::size_t parts =
      flags.empty() ? 1 : (flags.size() + kMaxFlagEntries - 1) / kMaxFlagEntries;
  if (parts > kMaxU16 || has_duplicate(flags)) return std::nullopt;
  std::vector<FlagSnapshot> out(parts);
  for (std::size_t i = 0; i < parts; ++i) {
    out[i].part = static_cast<std::uint16_t>(i);
    out[i].part_count = static_cast<std::uint16_t>(parts);
    const std::size_t from = i * kMaxFlagEntries;
    const std::size_t to = std::min(flags.size(), from + kMaxFlagEntries);
    out[i].flags.assign(flags.begin() + static_cast<std::ptrdiff_t>(from),
                        flags.begin() + static_cast<std::ptrdiff_t>(to));
  }
  return out;
}

}  // namespace coop::protocol
