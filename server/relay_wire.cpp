#include "relay_wire.hpp"

#include <algorithm>
#include <cctype>

namespace coop::server::relay {

namespace {

void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v >> 8));
  out.push_back(static_cast<std::uint8_t>(v));
}

void put64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(v >> shift));
}

struct Reader {
  std::span<const std::uint8_t> bytes;
  std::size_t at = 0;
  bool ok = true;

  std::size_t left() const { return bytes.size() - at; }
  std::uint8_t u8() {
    if (left() < 1) {
      ok = false;
      return 0;
    }
    return bytes[at++];
  }
  std::uint16_t u16() {
    if (left() < 2) {
      ok = false;
      return 0;
    }
    const auto v = static_cast<std::uint16_t>((bytes[at] << 8) | bytes[at + 1]);
    at += 2;
    return v;
  }
  std::uint64_t u64() {
    if (left() < 8) {
      ok = false;
      return 0;
    }
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | bytes[at++];
    return v;
  }
  std::string text(std::size_t n) {
    if (left() < n) {
      ok = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(bytes.data() + at), n);
    at += n;
    return s;
  }
  std::vector<std::uint8_t> rest() {
    std::vector<std::uint8_t> v(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
    at = bytes.size();
    return v;
  }
};

bool is_code(std::string_view code) {
  return code.size() == kCodeLength &&
         std::all_of(code.begin(), code.end(),
                     [](char c) { return kCodeLetters.find(c) != std::string_view::npos; });
}

}  // namespace

bool has_magic(std::span<const std::uint8_t> bytes) {
  return bytes.size() >= kMagic.size() && std::equal(kMagic.begin(), kMagic.end(), bytes.begin());
}

std::vector<std::uint8_t> encode(const Message& m) {
  std::vector<std::uint8_t> out(kMagic.begin(), kMagic.end());
  out.push_back(static_cast<std::uint8_t>(m.kind));
  switch (m.kind) {
    case Kind::kRegister:
      put64(out, m.token);
      out.push_back(static_cast<std::uint8_t>(std::min(m.key.size(), kMaxKeyBytes)));
      out.insert(out.end(), m.key.begin(), m.key.begin() + static_cast<std::ptrdiff_t>(std::min(m.key.size(), kMaxKeyBytes)));
      break;
    case Kind::kKeepalive:
    case Kind::kBye:
      put64(out, m.token);
      break;
    case Kind::kLinkData:
      put64(out, m.token);
      put16(out, m.flow);
      out.insert(out.end(), m.payload.begin(), m.payload.end());
      break;
    case Kind::kLinkClose:
      put64(out, m.token);
      put16(out, m.flow);
      break;
    case Kind::kLookup:
      out.insert(out.end(), m.code.begin(), m.code.end());
      break;
    case Kind::kRegistered:
      put64(out, m.token);
      out.insert(out.end(), m.code.begin(), m.code.end());
      break;
    case Kind::kRefused:
      put64(out, m.token);
      out.push_back(m.status);
      break;
    case Kind::kRelayData:
      put16(out, m.flow);
      out.insert(out.end(), m.payload.begin(), m.payload.end());
      break;
    case Kind::kRelayClose:
      put16(out, m.flow);
      break;
    case Kind::kFound:
      out.push_back(m.status);
      break;
  }
  return out;
}

std::optional<Message> decode(std::span<const std::uint8_t> bytes) {
  if (!has_magic(bytes)) return std::nullopt;
  Reader r{bytes, kMagic.size()};
  Message m;
  m.kind = static_cast<Kind>(r.u8());
  switch (m.kind) {
    case Kind::kRegister: {
      m.token = r.u64();
      const std::uint8_t n = r.u8();
      if (n > kMaxKeyBytes) return std::nullopt;
      m.key = r.text(n);
      break;
    }
    case Kind::kKeepalive:
    case Kind::kBye:
      m.token = r.u64();
      break;
    case Kind::kLinkData:
      m.token = r.u64();
      m.flow = r.u16();
      m.payload = r.rest();
      break;
    case Kind::kLinkClose:
      m.token = r.u64();
      m.flow = r.u16();
      break;
    case Kind::kLookup:
      m.code = r.text(kCodeLength);
      if (r.ok && !is_code(m.code)) return std::nullopt;
      break;
    case Kind::kRegistered:
      m.token = r.u64();
      m.code = r.text(kCodeLength);
      if (r.ok && !is_code(m.code)) return std::nullopt;
      break;
    case Kind::kRefused:
      m.token = r.u64();
      m.status = r.u8();
      break;
    case Kind::kRelayData:
      m.flow = r.u16();
      m.payload = r.rest();
      break;
    case Kind::kRelayClose:
      m.flow = r.u16();
      break;
    case Kind::kFound:
      m.status = r.u8();
      break;
    default:
      return std::nullopt;
  }
  if (!r.ok || r.left() != 0 || m.payload.size() > kMaxPayloadBytes) return std::nullopt;
  return m;
}

std::string make_code(std::uint64_t random_bits) {
  std::string code;
  for (std::size_t i = 0; i < kCodeLength; ++i) {
    code.push_back(kCodeLetters[random_bits & 31u]);
    random_bits >>= 5;
  }
  return code;
}

std::optional<std::string> normalize_code(std::string_view typed) {
  std::string code;
  for (const char c : typed) {
    if (c == '-' || c == ' ' || c == '\t') continue;
    code.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  if (!is_code(code)) return std::nullopt;
  return code;
}

std::string show_code(std::string_view code) {
  if (code.size() != kCodeLength) return std::string(code);
  return std::string(code.substr(0, 4)) + "-" + std::string(code.substr(4));
}

std::string_view refusal_text(Refusal why) {
  switch (why) {
    case Refusal::kUnknownSession:
      return "the relay does not know this host (it was restarted)";
    case Refusal::kFull:
      return "the relay is full (it has as many hosts as it takes)";
    case Refusal::kWrongKey:
      return "the relay needs a host key and this one is wrong or missing (relay_key in coop-options.ini)";
    case Refusal::kTooMany:
      return "the relay already has the most hosts it takes from your address";
    case Refusal::kSlowDown:
      return "too many tries from your address lately: wait 10 minutes";
  }
  return "refused";
}

}  // namespace coop::server::relay
