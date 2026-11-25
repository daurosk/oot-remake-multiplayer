#include "relay_wire.hpp"

#include <algorithm>
#include <cctype>

namespace coop::server::relay {

namespace {

void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v >> 8));
  out.push_back(static_cast<std::uint8_t>(v));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(v >> shift));
}

void put64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(v >> shift));
}

// A length byte and the text, at most `most` bytes of it.
void put_text(std::vector<std::uint8_t>& out, const std::string& text, std::size_t most) {
  const std::size_t n = std::min(text.size(), most);
  out.push_back(static_cast<std::uint8_t>(n));
  out.insert(out.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(n));
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
  std::uint32_t u32() {
    if (left() < 4) {
      ok = false;
      return 0;
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | bytes[at++];
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
  // A length byte and that many bytes; not ok when the length is over `most`.
  std::string sized_text(std::size_t most) {
    const std::uint8_t n = u8();
    if (n > most) {
      ok = false;
      return {};
    }
    return text(n);
  }
};

// An answer code as the guest's helper writes it: code letters and '-' only.
bool is_answer_text(std::string_view a) {
  return a.size() >= kMinAnswerBytes && a.size() <= kMaxAnswerBytes &&
         std::all_of(a.begin(), a.end(), [](char c) { return c == '-' || kCodeLetters.find(c) != std::string_view::npos; });
}

bool is_room_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

void put_entry(std::vector<std::uint8_t>& out, const RoomEntry& e) {
  put_text(out, e.room, kMaxRoomLength);
  put_text(out, e.host, kMaxHostNameBytes);
  out.push_back(e.players);
  out.push_back(e.max_players);
  put16(out, e.version);
  out.push_back(e.password ? 1 : 0);
}

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
  if (m.kind == Kind::kListRooms || m.kind == Kind::kListProofs) {
    // Fixed-size request (zero padded), so the reply is never bigger than it: built at full size, fields by index.
    std::vector<std::uint8_t> req(kListRequestBytes, std::uint8_t{0});
    std::copy(kMagic.begin(), kMagic.end(), req.begin());
    req[kMagic.size()] = static_cast<std::uint8_t>(m.kind);
    req[kMagic.size() + 1] = m.start;
    return req;
  }
  std::vector<std::uint8_t> out(kMagic.begin(), kMagic.end());
  out.push_back(static_cast<std::uint8_t>(m.kind));
  switch (m.kind) {
    case Kind::kRegister:
      put64(out, m.token);
      put_text(out, m.key, kMaxKeyBytes);
      break;
    case Kind::kRegisterName:
      put64(out, m.token);
      put_text(out, m.key, kMaxKeyBytes);
      put_text(out, m.name, 255);
      break;
    case Kind::kLookupName:
      put_text(out, m.name, 255);
      break;
    case Kind::kRegisteredName:
      put64(out, m.token);
      out.insert(out.end(), m.code.begin(), m.code.end());
      put_text(out, m.name, 255);
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
    case Kind::kDirect:
      out.push_back(m.code.empty() ? 1 : 0);
      put_text(out, m.code.empty() ? m.name : m.code, 255);
      put_text(out, m.answer, 255);
      break;
    case Kind::kDirectAnswer:
      put64(out, m.token);
      put32(out, m.ticket);
      put_text(out, m.answer, 255);
      break;
    case Kind::kDirectReply:
      put64(out, m.token);
      put32(out, m.ticket);
      put32(out, m.ip);
      put16(out, m.port);
      break;
    case Kind::kDirectFound:
      out.push_back(m.status);
      put32(out, m.ip);
      put16(out, m.port);
      break;
    case Kind::kListRoom:
      put64(out, m.token);
      out.push_back(m.listed ? 1 : 0);
      out.push_back(m.entry.players);
      out.push_back(m.entry.max_players);
      put16(out, m.entry.version);
      put_text(out, m.entry.host, kMaxHostNameBytes);
      break;
    case Kind::kListRooms:
    case Kind::kListProofs:
      break;  // (built above)
    case Kind::kRoomProof:
      put64(out, m.token);
      put64(out, m.proof.time_s);
      out.insert(out.end(), m.proof.signature.begin(), m.proof.signature.end());
      break;
    case Kind::kRoomProofs: {
      const std::size_t n = std::min<std::size_t>(m.proofs.size(), 255);
      out.push_back(static_cast<std::uint8_t>(n));
      for (std::size_t i = 0; i < n; ++i) {
        put_text(out, m.proofs[i].room, kMaxRoomLength);
        put64(out, m.proofs[i].time_s);
        out.insert(out.end(), m.proofs[i].signature.begin(), m.proofs[i].signature.end());
      }
      break;
    }
    case Kind::kRoomList: {
      const std::size_t n = std::min<std::size_t>(m.rooms.size(), 255);
      out.push_back(m.total);
      out.push_back(m.start);
      out.push_back(static_cast<std::uint8_t>(n));
      for (std::size_t i = 0; i < n; ++i) put_entry(out, m.rooms[i]);
      break;
    }
  }
  return out;
}

std::size_t proof_bytes(const RoomProof& p) {
  return 1 + std::min(p.room.size(), kMaxRoomLength) + 8 + kProofSignatureBytes;
}

std::size_t entry_bytes(const RoomEntry& e) {
  return 1 + std::min(e.room.size(), kMaxRoomLength) + 1 + std::min(e.host.size(), kMaxHostNameBytes) + 5;
}

std::string listing_name(std::string_view name) {
  std::string out;
  for (const char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x20 && u != 0x7f) out.push_back(c);
  }
  if (out.size() > kMaxHostNameBytes) {
    std::size_t n = kMaxHostNameBytes;
    while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xc0) == 0x80) --n;
    out.resize(n);
  }
  return out.empty() ? std::string("Host") : out;
}

bool listable_host_name(std::string_view name) {
  if (name.empty() || name.size() > kMaxHostNameBytes) return false;
  return std::none_of(name.begin(), name.end(), [](char c) {
    const auto u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7f;
  });
}

std::optional<Message> decode(std::span<const std::uint8_t> bytes) {
  if (!has_magic(bytes)) return std::nullopt;
  Reader r{bytes, kMagic.size()};
  Message m;
  m.kind = static_cast<Kind>(r.u8());
  switch (m.kind) {
    case Kind::kRegister:
      m.token = r.u64();
      m.key = r.sized_text(kMaxKeyBytes);
      break;
    case Kind::kRegisterName:
      m.token = r.u64();
      m.key = r.sized_text(kMaxKeyBytes);
      m.name = r.sized_text(kMaxRoomLength);
      break;
    case Kind::kLookupName:
      m.name = r.sized_text(kMaxRoomLength);
      break;
    case Kind::kRegisteredName:
      m.token = r.u64();
      m.code = r.text(kCodeLength);
      if (r.ok && !is_code(m.code)) return std::nullopt;
      m.name = r.sized_text(kMaxRoomLength);
      break;
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
    case Kind::kDirect: {
      const std::uint8_t by = r.u8();
      if (by > 1) return std::nullopt;
      std::string target = r.sized_text(by == 0 ? kCodeLength : kMaxRoomLength);
      if (by == 0) {
        if (r.ok && !is_code(target)) return std::nullopt;
        m.code = std::move(target);
      } else {
        if (r.ok && target.empty()) return std::nullopt;
        m.name = std::move(target);
      }
      m.answer = r.sized_text(kMaxAnswerBytes);
      if (r.ok && !is_answer_text(m.answer)) return std::nullopt;
      break;
    }
    case Kind::kDirectAnswer:
      m.token = r.u64();
      m.ticket = r.u32();
      m.answer = r.sized_text(kMaxAnswerBytes);
      if (r.ok && !is_answer_text(m.answer)) return std::nullopt;
      break;
    case Kind::kDirectReply:
      m.token = r.u64();
      m.ticket = r.u32();
      m.ip = r.u32();
      m.port = r.u16();
      break;
    case Kind::kDirectFound:
      m.status = r.u8();
      m.ip = r.u32();
      m.port = r.u16();
      break;
    case Kind::kListRoom: {
      m.token = r.u64();
      const std::uint8_t listed = r.u8();
      if (listed > 1) return std::nullopt;
      m.listed = listed == 1;
      m.entry.players = r.u8();
      m.entry.max_players = r.u8();
      m.entry.version = r.u16();
      m.entry.host = r.sized_text(kMaxHostNameBytes);
      if (r.ok && m.listed && !listable_host_name(m.entry.host)) return std::nullopt;
      break;
    }
    case Kind::kListRooms: {
      if (bytes.size() < kListRequestBytes) return std::nullopt;  // (not padded: no answer)
      m.start = r.u8();
      const std::vector<std::uint8_t> pad = r.rest();
      if (std::any_of(pad.begin(), pad.end(), [](std::uint8_t b) { return b != 0; })) return std::nullopt;
      break;
    }
    case Kind::kListProofs: {
      if (bytes.size() < kListRequestBytes) return std::nullopt;  // (not padded: no answer)
      const std::vector<std::uint8_t> pad = r.rest();
      if (std::any_of(pad.begin(), pad.end(), [](std::uint8_t b) { return b != 0; })) return std::nullopt;
      break;
    }
    case Kind::kRoomProof:
      m.token = r.u64();
      m.proof.time_s = r.u64();
      for (auto& b : m.proof.signature) b = r.u8();
      break;
    case Kind::kRoomProofs: {
      const std::uint8_t n = r.u8();
      for (std::uint8_t i = 0; i < n && r.ok; ++i) {
        RoomProof p;
        p.room = r.sized_text(kMaxRoomLength);
        p.time_s = r.u64();
        for (auto& b : p.signature) b = r.u8();
        if (r.ok && !normalize_room(p.room)) return std::nullopt;
        m.proofs.push_back(std::move(p));
      }
      break;
    }
    case Kind::kRoomList: {
      m.total = r.u8();
      m.start = r.u8();
      const std::uint8_t n = r.u8();
      for (std::uint8_t i = 0; i < n && r.ok; ++i) {
        RoomEntry e;
        e.room = r.sized_text(kMaxRoomLength);
        e.host = r.sized_text(kMaxHostNameBytes);
        e.players = r.u8();
        e.max_players = r.u8();
        e.version = r.u16();
        e.password = (r.u8() & 1u) != 0;
        if (r.ok && (!normalize_room(e.room) || !listable_host_name(e.host))) return std::nullopt;
        m.rooms.push_back(std::move(e));
      }
      if (r.ok && static_cast<std::size_t>(m.start) + m.rooms.size() > m.total) return std::nullopt;
      break;
    }
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

std::optional<std::string> normalize_room(std::string_view typed) {
  const auto blank = [](char c) { return c == ' ' || c == '\t'; };
  while (!typed.empty() && blank(typed.front())) typed.remove_prefix(1);
  while (!typed.empty() && blank(typed.back())) typed.remove_suffix(1);
  if (typed.size() < kMinRoomLength || typed.size() > kMaxRoomLength) return std::nullopt;
  if (!std::all_of(typed.begin(), typed.end(), is_room_char)) return std::nullopt;
  return std::string(typed);
}

std::string room_key(std::string_view room) {
  std::string key(room);
  for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return key;
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
    case Refusal::kNameTaken:
      return "another host has this room name (a host that just stopped keeps it for 30 seconds)";
    case Refusal::kBadName:
      return "that is not a room name: 3 to 24 letters, digits, - or _";
  }
  return "refused";
}

}  // namespace coop::server::relay
