#include "nat_codes.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <random>
#include <utility>

namespace coop::server::nat {

namespace {

constexpr std::string_view kPunchLabel = "oot-coop nat punch v1";
constexpr std::string_view kAnswerLabel = "oot-coop answer v1";
constexpr std::size_t kInviteCheck = 4;
constexpr std::size_t kAnswerCheck = 2;
constexpr std::size_t kAnswerMac = 8;
// kind, address, port, nonce, time, signature, checksum
constexpr std::size_t kAnswerBytes = 1 + 4 + 2 + kNonceBytes + 4 + kAnswerMac + kAnswerCheck;
constexpr std::size_t kMadeBytes = 13;  // 20 letters = 100 bits, 4 zero bits after

std::span<const std::uint8_t> bytes_of(std::string_view s) {
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(v >> shift));
}
void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v >> 8));
  out.push_back(static_cast<std::uint8_t>(v));
}
std::uint32_t get_u32(const std::uint8_t* p) {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
std::uint16_t get_u16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

int letter_value(char c) {
  const auto at = kLetters.find(c);
  return at == std::string_view::npos ? -1 : static_cast<int>(at);
}

std::size_t letters_for(std::size_t bytes) { return (bytes * 8 + 4) / 5; }

// Letters as bits, most significant first, in ceil(5n / 8) bytes.
std::vector<std::uint8_t> pack(std::string_view letters) {
  std::vector<std::uint8_t> out((letters.size() * 5 + 7) / 8, 0);
  std::size_t bit = 0;
  for (char c : letters) {
    const int v = letter_value(c);
    for (int i = 4; i >= 0; --i, ++bit) {
      if ((v >> i) & 1) out[bit / 8] = static_cast<std::uint8_t>(out[bit / 8] | (0x80u >> (bit % 8)));
    }
  }
  return out;
}

// The first `count` letters of these bytes' bits (zero bits after the end).
std::string unpack(std::span<const std::uint8_t> bytes, std::size_t count) {
  std::string out;
  out.reserve(count);
  for (std::size_t n = 0; n < count; ++n) {
    int v = 0;
    for (std::size_t i = 0; i < 5; ++i) {
      const std::size_t bit = n * 5 + i;
      const int b = bit / 8 < bytes.size() ? (bytes[bit / 8] >> (7 - bit % 8)) & 1 : 0;
      v = (v << 1) | b;
    }
    out.push_back(kLetters[static_cast<std::size_t>(v)]);
  }
  return out;
}

protocol::Sha256Digest digest(std::span<const std::uint8_t> data) { return protocol::sha256(data); }

bool same_bytes(const std::uint8_t* a, const std::uint8_t* b, std::size_t n) {
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < n; ++i) diff = static_cast<std::uint8_t>(diff | (a[i] ^ b[i]));
  return diff == 0;
}

bool password_ok(std::string_view p) {
  if (p.empty() || p.size() > protocol::kMaxJoinPasswordBytes) return false;
  return std::none_of(p.begin(), p.end(), [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  });
}

// The bytes of a code (checksum checked): what read_invite and read_answer share.
struct Raw {
  CodeError error = CodeError::kNotACode;
  std::vector<std::uint8_t> bytes;  // without the checksum
};
Raw read_raw(std::string_view typed) {
  Raw raw;
  const auto letters = clean_letters(typed);
  if (!letters || letters->size() < 10) return raw;
  const std::size_t n = letters->size() * 5 / 8;
  auto bytes = from_letters(*letters, n);
  if (!bytes || n < 3) {
    raw.error = CodeError::kBadChecksum;
    return raw;
  }
  const std::size_t check = ((*bytes)[0] >> 4) == (kAnswerV1 >> 4) ? kAnswerCheck : kInviteCheck;
  if (bytes->size() <= check + 1) {
    raw.error = CodeError::kBadChecksum;
    return raw;
  }
  const std::size_t body = bytes->size() - check;
  const auto d = digest(std::span<const std::uint8_t>(bytes->data(), body));
  if (!same_bytes(d.data(), bytes->data() + body, check)) {
    raw.error = CodeError::kBadChecksum;
    return raw;
  }
  bytes->resize(body);
  raw.error = CodeError::kOk;
  raw.bytes = std::move(*bytes);
  return raw;
}

std::array<std::uint8_t, kAnswerMac> answer_mac(std::span<const std::uint8_t> body, std::string_view password) {
  const auto key = punch_key(password);
  std::vector<std::uint8_t> message(kAnswerLabel.begin(), kAnswerLabel.end());
  message.insert(message.end(), body.begin(), body.end());
  const auto full = protocol::hmac_sha256(key, message);
  std::array<std::uint8_t, kAnswerMac> out{};
  std::memcpy(out.data(), full.data(), kAnswerMac);
  return out;
}

}  // namespace

std::string show(Addr a) {
  return std::to_string(a.ip >> 24) + "." + std::to_string((a.ip >> 16) & 255u) + "." +
         std::to_string((a.ip >> 8) & 255u) + "." + std::to_string(a.ip & 255u) + ":" +
         std::to_string(a.port);
}

bool is_public(std::uint32_t ip) {
  const std::uint32_t a = ip >> 24;
  const std::uint32_t b = (ip >> 16) & 255u;
  if (a == 0 || a == 10 || a == 127 || a >= 224) return false;
  if (a == 100 && (b & 0xC0u) == 64) return false;  // 100.64.0.0/10 (CGNAT)
  if (a == 169 && b == 254) return false;
  if (a == 172 && (b & 0xF0u) == 16) return false;
  if (a == 192 && b == 168) return false;
  if (a == 192 && b == 0 && ((ip >> 8) & 255u) == 0) return false;
  if (a == 198 && (b & 0xFEu) == 18) return false;
  return true;
}

std::string to_letters(std::span<const std::uint8_t> bytes) { return unpack(bytes, letters_for(bytes.size())); }

std::optional<std::vector<std::uint8_t>> from_letters(std::string_view letters, std::size_t byte_count) {
  if (letters.size() != letters_for(byte_count)) return std::nullopt;
  for (char c : letters) {
    if (letter_value(c) < 0) return std::nullopt;
  }
  auto bits = pack(letters);
  // Bits after the last byte must be zero (one way to write each code).
  const std::size_t used = byte_count * 8;
  for (std::size_t bit = used; bit < letters.size() * 5; ++bit) {
    if ((bits[bit / 8] >> (7 - bit % 8)) & 1) return std::nullopt;
  }
  bits.resize(byte_count);
  return bits;
}

std::optional<std::string> clean_letters(std::string_view typed) {
  std::string out;
  for (char c : typed) {
    if (c == ' ' || c == '-' || c == '.' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\'') continue;
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (letter_value(c) < 0) return std::nullopt;
    out.push_back(c);
  }
  return out;
}

std::vector<std::string> find_codes(std::string_view text) {
  // The words (runs of ASCII letters and digits) made only of code letters,
  // and whether each follows the one before with no other word between.
  struct Word {
    std::string letters;
    bool joined = false;
  };
  std::vector<Word> words;
  bool broken = true;
  for (std::size_t i = 0; i < text.size();) {
    const auto alnum = [](char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    if (!alnum(text[i])) {
      ++i;
      continue;
    }
    std::string w;
    bool code = true;
    for (; i < text.size() && alnum(text[i]); ++i) {
      char c = text[i];
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
      if (letter_value(c) < 0) code = false;
      w.push_back(c);
    }
    if (!code) {
      broken = true;
      continue;
    }
    words.push_back({std::move(w), !broken});
    broken = false;
  }
  // Each run of joined words: the first stretch of whole words, from the
  // left, whose letters are a code; then on after it.
  constexpr std::size_t kMin = 20;   // the shortest invite code (a 1-letter password) is 21 letters
  constexpr std::size_t kMax = 240;  // the longest (a 128-byte password) is 224
  std::vector<std::string> out;
  for (std::size_t start = 0; start < words.size();) {
    std::string letters;
    std::size_t found = 0;
    for (std::size_t end = start; end < words.size(); ++end) {
      if (end > start && !words[end].joined) break;
      letters += words[end].letters;
      if (letters.size() > kMax) break;
      // Only lengths to_letters makes (cheap) go on to the checksum (a SHA-256).
      if (letters.size() >= kMin && letters_for(letters.size() * 5 / 8) == letters.size() &&
          read_raw(letters).error == CodeError::kOk) {
        found = end + 1;
        break;
      }
    }
    if (found) {
      out.push_back(std::move(letters));
      start = found;
    } else {
      ++start;
    }
  }
  return out;
}

std::string grouped(std::string_view letters) {
  std::string out;
  for (std::size_t i = 0; i < letters.size(); ++i) {
    if (i && i % 5 == 0) out.push_back('-');
    out.push_back(letters[i]);
  }
  return out;
}

std::string make_password(const std::function<std::uint32_t()>& random) {
  std::string out;
  for (std::size_t i = 0; i < kPasswordLetters; ++i) out.push_back(kLetters[random() & 31u]);
  return out;
}

std::string make_password() {
  std::random_device device;
  return make_password([&device] { return static_cast<std::uint32_t>(device()); });
}

bool is_made_password(std::string_view password) {
  return password.size() == kPasswordLetters &&
         std::all_of(password.begin(), password.end(), [](char c) { return letter_value(c) >= 0; });
}

protocol::Sha256Digest punch_key(std::string_view password) {
  return protocol::hmac_sha256(bytes_of(password), bytes_of(kPunchLabel));
}

std::string explain(CodeError error) {
  switch (error) {
    case CodeError::kOk: return "the code is fine";
    case CodeError::kNotACode: return "this is not a code (letters A-Z and 2-9 in groups of five; paste all of it)";
    case CodeError::kBadChecksum: return "a letter of the code is wrong or missing; copy and paste it again";
    case CodeError::kWrongKind: return "this is the other kind of code (an invite code goes into Join.bat, an answer code into the host's server window)";
    case CodeError::kNewerCode: return "the code is from a newer co-op version; everyone runs Update.bat";
    case CodeError::kMalformed: return "the code does not make sense; ask for a new one";
    case CodeError::kBadSignature: return "the answer code was not made with this server's invite code (an old invite, or another server's)";
    case CodeError::kStale: return "the answer code is older than 10 minutes (or the clocks of the two PCs are far apart); ask for a new one";
    case CodeError::kNotPublic: return "the address in the code is not an internet address";
  }
  return "unknown";
}

std::optional<std::string> make_invite(const Invite& invite) {
  if (!password_ok(invite.password) || invite.host.port == 0) return std::nullopt;
  std::vector<std::uint8_t> bytes;
  const bool made = is_made_password(invite.password);
  bytes.push_back(made ? kInviteMade : kInviteText);
  put_u32(bytes, invite.host.ip);
  put_u16(bytes, invite.host.port);
  if (made) {
    const auto bits = pack(invite.password);  // 13 bytes
    bytes.insert(bytes.end(), bits.begin(), bits.end());
  } else {
    bytes.push_back(static_cast<std::uint8_t>(invite.password.size()));
    bytes.insert(bytes.end(), invite.password.begin(), invite.password.end());
  }
  const auto d = digest(bytes);
  bytes.insert(bytes.end(), d.begin(), d.begin() + kInviteCheck);
  return grouped(to_letters(bytes));
}

static InviteRead invite_as_is(std::string_view typed) {
  InviteRead out;
  Raw raw = read_raw(typed);
  out.error = raw.error;
  if (raw.error != CodeError::kOk) return out;
  const auto& b = raw.bytes;
  const std::uint8_t kind = b[0];
  if ((kind >> 4) == (kAnswerV1 >> 4)) { out.error = CodeError::kWrongKind; return out; }
  if (kind != kInviteMade && kind != kInviteText) { out.error = CodeError::kNewerCode; return out; }
  out.error = CodeError::kMalformed;
  if (b.size() < 8) return out;
  out.invite.host.ip = get_u32(&b[1]);
  out.invite.host.port = get_u16(&b[5]);
  if (kind == kInviteMade) {
    if (b.size() != 7 + kMadeBytes) return out;
    if (b[7 + kMadeBytes - 1] & 0x0Fu) return out;  // the 4 bits after the 100
    out.invite.password = unpack(std::span<const std::uint8_t>(&b[7], kMadeBytes), kPasswordLetters);
  } else {
    const std::size_t length = b[7];
    if (b.size() != 8 + length) return out;
    out.invite.password.assign(reinterpret_cast<const char*>(&b[8]), length);
  }
  if (!password_ok(out.invite.password) || out.invite.host.port == 0) return out;
  if (!is_public(out.invite.host.ip)) { out.error = CodeError::kNotPublic; return out; }
  out.error = CodeError::kOk;
  return out;
}

bool looks_like_code(std::string_view typed) {
  const auto letters = clean_letters(typed);
  return (letters && letters->size() >= 30) || !find_codes(typed).empty();
}

std::string make_answer(const Answer& answer, std::string_view password) {
  std::vector<std::uint8_t> bytes;
  bytes.push_back(kAnswerV1);
  put_u32(bytes, answer.guest.ip);
  put_u16(bytes, answer.guest.port);
  bytes.insert(bytes.end(), answer.nonce.begin(), answer.nonce.end());
  put_u32(bytes, answer.time);
  const auto mac = answer_mac(bytes, password);
  bytes.insert(bytes.end(), mac.begin(), mac.end());
  const auto d = digest(bytes);
  bytes.insert(bytes.end(), d.begin(), d.begin() + kAnswerCheck);
  return grouped(to_letters(bytes));
}

static AnswerRead answer_as_is(std::string_view typed, std::string_view password, std::uint64_t unix_now) {
  AnswerRead out;
  Raw raw = read_raw(typed);
  out.error = raw.error;
  if (raw.error != CodeError::kOk) return out;
  const auto& b = raw.bytes;
  if ((b[0] >> 4) == (kInviteMade >> 4)) { out.error = CodeError::kWrongKind; return out; }
  if (b[0] != kAnswerV1) { out.error = CodeError::kNewerCode; return out; }
  if (b.size() != kAnswerBytes - kAnswerCheck) { out.error = CodeError::kMalformed; return out; }
  constexpr std::size_t kSigned = 1 + 4 + 2 + kNonceBytes + 4;
  const auto mac = answer_mac(std::span<const std::uint8_t>(b.data(), kSigned), password);
  if (!same_bytes(mac.data(), &b[kSigned], kAnswerMac)) { out.error = CodeError::kBadSignature; return out; }
  out.answer.guest.ip = get_u32(&b[1]);
  out.answer.guest.port = get_u16(&b[5]);
  std::memcpy(out.answer.nonce.data(), &b[7], kNonceBytes);
  out.answer.time = get_u32(&b[7 + kNonceBytes]);
  const auto age = static_cast<std::int32_t>(static_cast<std::uint32_t>(unix_now) - out.answer.time);
  if (age > static_cast<std::int32_t>(kAnswerMaxAgeSeconds) || age < -60) { out.error = CodeError::kStale; return out; }
  if (out.answer.guest.port == 0) { out.error = CodeError::kMalformed; return out; }
  out.error = CodeError::kOk;
  return out;
}


// The text as it is first; when that is not a code (other characters in it,
// or a letter more or less), the codes inside it: the first one read fine, or
// else the first one's error that is not "the other kind" (a stale answer
// beside the host's own invite says "stale"), or else the first one's.
template <typename Read, typename AsIs>
static Read look_inside(std::string_view typed, const AsIs& as_is) {
  Read out = as_is(typed);
  if (out.error != CodeError::kNotACode && out.error != CodeError::kBadChecksum) return out;
  std::optional<Read> first, first_right_kind;
  for (const std::string& code : find_codes(typed)) {
    Read r = as_is(code);
    if (r.error == CodeError::kOk) return r;
    if (!first) first = r;
    if (!first_right_kind && r.error != CodeError::kWrongKind) first_right_kind = r;
  }
  if (first_right_kind) return *first_right_kind;
  if (first) return *first;
  return out;
}

InviteRead read_invite(std::string_view typed) {
  return look_inside<InviteRead>(typed, [](std::string_view t) { return invite_as_is(t); });
}

AnswerRead read_answer(std::string_view typed, std::string_view password, std::uint64_t unix_now) {
  return look_inside<AnswerRead>(typed, [&](std::string_view t) { return answer_as_is(t, password, unix_now); });
}

std::string explain_answer(CodeError error) {
  switch (error) {
    case CodeError::kOk: return "Answer code taken: connecting your friend...";
    case CodeError::kNotACode:
      return "No answer code found in what you copied: copy the whole code from your friend's message.";
    case CodeError::kBadChecksum:
      return "A letter of that answer code is wrong or missing: ask your friend to copy and send it again.";
    case CodeError::kWrongKind:
      return "That is an invite code, not an answer code: copy the code your friend's game shows them.";
    case CodeError::kNewerCode: return "Your friend's co-op is newer than yours: both of you run Update.bat.";
    case CodeError::kMalformed: return "That answer code does not make sense: ask your friend for a new one.";
    case CodeError::kBadSignature:
      return "That answer code was made for another invite code (an old one?): send your friend your code again.";
    case CodeError::kStale:
      return "That answer code is over 10 minutes old (or a PC's clock is wrong): ask your friend to join again "
             "and send the new one.";
    case CodeError::kNotPublic:
      return "That answer code has no internet address: on the same network or VPN, use Same network or VPN.";
  }
  return "That answer code didn't work: ask your friend to copy it again.";
}

}  // namespace coop::server::nat
