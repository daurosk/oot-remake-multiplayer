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

// A (hash, index) pair as one number, to find a flag given twice.
std::uint64_t flag_key(const FlagEntry& e) {
  return (static_cast<std::uint64_t>(e.hash) << 16) | e.index;
}

Error check_flag(const FlagEntry& e) {
  switch (e.kind) {
    case FlagKind::boolean:
      return e.value <= 1 ? Error::none : Error::bad_value;
    case FlagKind::int32:
    case FlagKind::uint32:
      return Error::none;
    case FlagKind::float32:
      return std::isfinite(std::bit_cast<float>(e.value)) ? Error::none : Error::bad_value;
  }
  return Error::bad_value;
}

bool has_duplicate(std::span<const FlagEntry> flags) {
  std::vector<std::uint64_t> keys;
  keys.reserve(flags.size());
  for (const auto& e : flags) keys.push_back(flag_key(e));
  std::sort(keys.begin(), keys.end());
  return std::adjacent_find(keys.begin(), keys.end()) != keys.end();
}

// The rules every flag list follows: min_count to kMaxFlagEntries entries,
// each valid, no (hash, index) twice.
Error check_flags(const std::vector<FlagEntry>& flags, std::size_t min_count) {
  if (flags.size() > kMaxFlagEntries) return Error::too_large;
  if (flags.size() < min_count) return Error::bad_value;
  for (const auto& e : flags) COOP_TRY(check_flag(e));
  if (has_duplicate(flags)) return Error::bad_value;
  return Error::none;
}

void write_flags(const std::vector<FlagEntry>& flags, Writer& w) {
  w.u16(static_cast<std::uint16_t>(flags.size()));
  for (const auto& e : flags) {
    w.u32(e.hash);
    w.u16(e.index);
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
  COOP_TRY(check_flags(m.flags, 1));
  w.u8(m.player_number);
  write_flags(m.flags, w);
  return Error::none;
}

Error encode_body(const FlagState& m, Writer& w) {
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  COOP_TRY(check_flags(m.flags, 1));
  w.u8(m.player_number);
  write_flags(m.flags, w);
  return Error::none;
}

Error encode_body(const FlagSnapshot& m, Writer& w) {
  if (m.part_count == 0 || m.part >= m.part_count) return Error::bad_value;
  COOP_TRY(check_flags(m.flags, 0));
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

Error read_flags(Reader& r, std::vector<FlagEntry>& flags, std::size_t min_count) {
  std::uint16_t count = 0;
  COOP_READ(r.u16(count));
  if (count > kMaxFlagEntries || count < min_count) return Error::bad_value;
  if (r.left() < count * kFlagEntrySize) return Error::truncated;
  flags.resize(count);
  for (auto& e : flags) {
    std::uint8_t kind = 0;
    COOP_READ(r.u32(e.hash));
    COOP_READ(r.u16(e.index));
    COOP_READ(r.u8(kind));
    COOP_READ(r.u32(e.value));
    e.kind = static_cast<FlagKind>(kind);
    COOP_TRY(check_flag(e));
  }
  if (has_duplicate(flags)) return Error::bad_value;
  return Error::none;
}

Error decode_body(Reader& r, FlagSet& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  return read_flags(r, m.flags, 1);
}

Error decode_body(Reader& r, FlagState& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player_or_self(m.player_number)) return Error::bad_value;
  return read_flags(r, m.flags, 1);
}

Error decode_body(Reader& r, FlagSnapshot& m) {
  COOP_READ(r.u16(m.part));
  COOP_READ(r.u16(m.part_count));
  if (m.part_count == 0 || m.part >= m.part_count) return Error::bad_value;
  return read_flags(r, m.flags, 0);
}

Error decode_body(Reader& r, Host& m) {
  COOP_READ(r.u8(m.player_number));
  if (!valid_player(m.player_number)) return Error::bad_value;
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

MessageType type_of(const Message& message) {
  static constexpr MessageType kTypes[] = {
      MessageType::hello,         MessageType::welcome,     MessageType::refused,
      MessageType::player_joined, MessageType::player_left, MessageType::ping,
      MessageType::pong,          MessageType::player_status, MessageType::chat,
      MessageType::player_sample, MessageType::test_payload,
      MessageType::join_partner,  MessageType::flag_set,    MessageType::flag_state,
      MessageType::flag_snapshot, MessageType::host,
  };
  static_assert(std::size(kTypes) == std::variant_size_v<Message>);
  return kTypes[message.index()];
}

std::uint8_t channel_of(const Message& message) { return channel_of(type_of(message)); }

std::uint8_t channel_of(MessageType type) {
  switch (type) {
    case MessageType::player_sample:
    case MessageType::test_payload:
      return kChannelPlayerSamples;
    case MessageType::flag_set:
    case MessageType::flag_state:
    case MessageType::flag_snapshot:
    case MessageType::host:
      return kChannelProgress;
    default:
      return kChannelSession;
  }
}

bool allowed_on_channel(MessageType type, std::uint8_t channel) {
  switch (type) {
    case MessageType::hello:
    case MessageType::welcome:
    case MessageType::refused:
    case MessageType::player_joined:
    case MessageType::player_left:
    case MessageType::ping:
    case MessageType::pong:
    case MessageType::player_status:
    case MessageType::chat:
    case MessageType::join_partner:
      return channel == kChannelSession;
    case MessageType::flag_set:
    case MessageType::flag_state:
    case MessageType::flag_snapshot:
    case MessageType::host:
      return channel == kChannelProgress;
    case MessageType::player_sample:
      return channel == kChannelPlayerSamples;
    case MessageType::test_payload:
      return channel == kChannelSession || channel == kChannelPlayerSamples;
  }
  return false;
}

bool may_send(Sender from, MessageType type) {
  switch (type) {
    case MessageType::hello:
      return from == Sender::client;
    case MessageType::welcome:
    case MessageType::refused:
    case MessageType::player_joined:
    case MessageType::player_left:
    case MessageType::host:
      return from == Sender::server;
    case MessageType::ping:
    case MessageType::pong:
    case MessageType::player_status:
    case MessageType::chat:
    case MessageType::player_sample:
    case MessageType::test_payload:
    case MessageType::join_partner:
    case MessageType::flag_set:
    case MessageType::flag_state:
    case MessageType::flag_snapshot:
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
                      std::is_same_v<T, FlagSet> || std::is_same_v<T, FlagState>) {
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
    case MessageType::flag_snapshot: return decode_as<FlagSnapshot>(body, out);
    case MessageType::host: return decode_as<Host>(body, out);
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
