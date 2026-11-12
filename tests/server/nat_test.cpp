// Invite and answer codes, STUN answers and signed punches (server/nat_codes,
// stun, punch): what a mistyped, forged, stale or malformed one does.

#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "coop/protocol/join_password.hpp"
#include "nat_codes.hpp"
#include "punch.hpp"
#include "stun.hpp"

using namespace coop::server::nat;

namespace {

constexpr Addr kHostAddr{0xCB007105u, 27510};   // 203.0.113.5
constexpr Addr kGuestAddr{0xC6336402u, 40001};  // 198.51.100.2
constexpr Addr kStunA{0x08080808u, 19302};
constexpr Addr kStunB{0x01010101u, 3478};
constexpr std::uint64_t kNow = 1'800'000'000;

std::string mutate(std::string code, std::size_t at) {
  char& c = code[at];
  if (c == '-') return code;
  c = c == 'A' ? 'B' : 'A';
  return code;
}

std::vector<std::uint8_t> stun_reply(const StunId& id, std::vector<std::uint8_t> attrs,
                                     std::uint16_t type = 0x0101) {
  std::vector<std::uint8_t> d = {static_cast<std::uint8_t>(type >> 8), static_cast<std::uint8_t>(type),
                                 static_cast<std::uint8_t>(attrs.size() >> 8),
                                 static_cast<std::uint8_t>(attrs.size()), 0x21, 0x12, 0xA4, 0x42};
  d.insert(d.end(), id.begin(), id.end());
  d.insert(d.end(), attrs.begin(), attrs.end());
  return d;
}

std::vector<std::uint8_t> xor_mapped(Addr a) {
  const std::uint16_t port = static_cast<std::uint16_t>(a.port ^ 0x2112);
  const std::uint32_t ip = a.ip ^ kStunCookie;
  return {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port),
          static_cast<std::uint8_t>(ip >> 24), static_cast<std::uint8_t>(ip >> 16),
          static_cast<std::uint8_t>(ip >> 8), static_cast<std::uint8_t>(ip)};
}

StunId id_of(std::uint8_t seed) {
  StunId id{};
  for (std::size_t i = 0; i < id.size(); ++i) id[i] = static_cast<std::uint8_t>(seed + i);
  return id;
}

}  // namespace

TEST_CASE("nat codes: letters") {
  const std::vector<std::uint8_t> bytes = {0x00, 0xFF, 0x12, 0x34, 0x56};
  const std::string letters = to_letters(bytes);
  CHECK(letters.size() == 8);
  auto back = from_letters(letters, bytes.size());
  REQUIRE(back.has_value());
  CHECK(*back == bytes);
  CHECK_FALSE(from_letters(letters.substr(1), bytes.size()).has_value());
  // 3 bytes = 24 bits in 5 letters: the last bit must be zero.
  const std::string three = to_letters(std::vector<std::uint8_t>{1, 2, 3});
  std::string odd = three;
  odd.back() = kLetters[1];
  CHECK_FALSE(from_letters(odd, 3).has_value());
  CHECK(clean_letters(" abcde-FGHJK. ").value() == "ABCDEFGHJK");
  CHECK_FALSE(clean_letters("ABC0E").has_value());  // 0 and O are not letters of the codes
  CHECK(grouped("ABCDEFGHJKL") == "ABCDE-FGHJK-L");
}

TEST_CASE("nat codes: the made join password is random enough and recognised") {
  const std::string p = make_password();
  CHECK(p.size() == 20);  // 100 bits
  CHECK(is_made_password(p));
  CHECK(make_password() != p);
  CHECK_FALSE(is_made_password("hunter2"));
  CHECK_FALSE(is_public(0x7F000001u));
  CHECK_FALSE(is_public(0xC0A80001u));
  CHECK_FALSE(is_public(0x64400001u));  // CGNAT
  CHECK(is_public(kHostAddr.ip));
}

TEST_CASE("invite code: made password round trip, and every mistyped letter is caught") {
  const std::string password = make_password();
  const auto code = make_invite(Invite{kHostAddr, password});
  REQUIRE(code.has_value());
  CHECK(code->size() <= 47);  // 39 letters in groups of five
  const InviteRead read = read_invite(*code);
  REQUIRE(read.error == CodeError::kOk);
  CHECK(read.invite.host == kHostAddr);
  CHECK(read.invite.password == password);
  CHECK(looks_like_code(*code));
  // Lower case and spaces work too.
  std::string loose;
  for (char c : *code) loose += c == '-' ? ' ' : static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  CHECK(read_invite(loose).error == CodeError::kOk);
  for (std::size_t i = 0; i < code->size(); ++i) {
    if ((*code)[i] == '-') continue;
    CAPTURE(i);
    CHECK(read_invite(mutate(*code, i)).error != CodeError::kOk);
  }
  CHECK(read_invite(code->substr(0, code->size() - 1)).error != CodeError::kOk);
  CHECK(read_invite(*code + "A").error != CodeError::kOk);
  CHECK(read_invite("").error == CodeError::kNotACode);
  CHECK(read_invite("203.0.113.5").error == CodeError::kNotACode);
  CHECK_FALSE(looks_like_code("ABCD-EFGH"));  // a relay join code
}

TEST_CASE("invite code: the host's own password, and what is refused") {
  const auto code = make_invite(Invite{kHostAddr, "three-random-words"});
  REQUIRE(code.has_value());
  const InviteRead read = read_invite(*code);
  REQUIRE(read.error == CodeError::kOk);
  CHECK(read.invite.password == "three-random-words");
  CHECK_FALSE(make_invite(Invite{kHostAddr, ""}).has_value());  // never without a password
  CHECK_FALSE(make_invite(Invite{kHostAddr, "has space"}).has_value());
  CHECK_FALSE(make_invite(Invite{kHostAddr, std::string(129, 'x')}).has_value());
  CHECK_FALSE(make_invite(Invite{Addr{kHostAddr.ip, 0}, "abc"}).has_value());
  CHECK(read_invite(make_invite(Invite{Addr{0xC0A80014u, 27510}, "abc"}).value()).error == CodeError::kNotPublic);
}

TEST_CASE("invite code: a newer version and checksummed nonsense") {
  auto build = [](std::vector<std::uint8_t> body) {
    const auto d = coop::protocol::sha256(body);
    body.insert(body.end(), d.begin(), d.begin() + 4);
    return grouped(to_letters(body));
  };
  std::vector<std::uint8_t> newer = {0x13, 203, 0, 113, 5, 0x6B, 0x76, 3, 'a', 'b', 'c', 0, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(read_invite(build(newer)).error == CodeError::kNewerCode);
  // A text password whose length byte says more than is there.
  std::vector<std::uint8_t> lying = {0x12, 203, 0, 113, 5, 0x6B, 0x76, 200, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
                                     'i', 'j', 'k', 'l', 'm', 'n', 'o'};
  CHECK(read_invite(build(lying)).error == CodeError::kMalformed);
  std::vector<std::uint8_t> zero_length = {0x12, 203, 0, 113, 5, 0x6B, 0x76, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  CHECK(read_invite(build(zero_length)).error == CodeError::kMalformed);
}

TEST_CASE("answer code: signed with the join password, fresh, not forged") {
  Answer a;
  a.guest = kGuestAddr;
  a.nonce = {1, 2, 3, 4, 5, 6, 7, 8};
  a.time = static_cast<std::uint32_t>(kNow);
  const std::string code = make_answer(a, "pw-one");
  const AnswerRead read = read_answer(code, "pw-one", kNow + 5);
  REQUIRE(read.error == CodeError::kOk);
  CHECK(read.answer.guest == kGuestAddr);
  CHECK(read.answer.nonce == a.nonce);
  CHECK(read_answer(code, "pw-two", kNow).error == CodeError::kBadSignature);
  CHECK(read_answer(code, "pw-one", kNow + kAnswerMaxAgeSeconds + 1).error == CodeError::kStale);
  CHECK(read_answer(code, "pw-one", kNow - 61).error == CodeError::kStale);
  for (std::size_t i = 0; i < code.size(); ++i) {
    if (code[i] == '-') continue;
    CAPTURE(i);
    CHECK(read_answer(mutate(code, i), "pw-one", kNow).error != CodeError::kOk);
  }
  // The two kinds are told apart.
  const auto invite = make_invite(Invite{kHostAddr, "pw-one"}).value();
  CHECK(read_answer(invite, "pw-one", kNow).error == CodeError::kWrongKind);
  CHECK(read_invite(code).error == CodeError::kWrongKind);
}

TEST_CASE("stun: request and answers") {
  const StunId id = id_of(7);
  const auto req = stun_request(id);
  CHECK(req[0] == 0x00);
  CHECK(req[1] == 0x01);
  CHECK(req[2] == 0);
  CHECK(req[3] == 0);
  CHECK(req[4] == 0x21);
  CHECK(req[19] == id[11]);

  auto ok = stun_reply(id, xor_mapped(kHostAddr));
  StunAnswer a = read_stun(ok, id);
  REQUIRE(a.result == StunResult::kOk);
  CHECK(a.mapped == kHostAddr);
  // Another request's answer.
  CHECK(read_stun(ok, id_of(8)).result == StunResult::kOtherId);
  // MAPPED-ADDRESS only (old servers).
  std::vector<std::uint8_t> plain = {0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x6B, 0x76, 203, 0, 113, 5};
  a = read_stun(stun_reply(id, plain), id);
  REQUIRE(a.result == StunResult::kOk);
  CHECK(a.mapped == kHostAddr);
  // Unknown attributes are skipped, XOR wins over plain.
  std::vector<std::uint8_t> mixed = {0x80, 0x22, 0x00, 0x03, 'a', 'b', 'c', 0};
  plain[11] = 6;  // a plain address that differs from the XOR one
  mixed.insert(mixed.end(), plain.begin(), plain.end());
  const auto x = xor_mapped(kHostAddr);
  mixed.insert(mixed.end(), x.begin(), x.end());
  a = read_stun(stun_reply(id, mixed), id);
  REQUIRE(a.result == StunResult::kOk);
  CHECK(a.mapped == kHostAddr);
  // IPv6 only: no address for us.
  std::vector<std::uint8_t> v6 = {0x00, 0x20, 0x00, 0x14, 0x00, 0x02, 0, 1};
  v6.resize(24, 0);
  CHECK(read_stun(stun_reply(id, v6), id).result == StunResult::kNoAddress);
  CHECK(read_stun(stun_reply(id, {}, 0x0111), id).result == StunResult::kErrorReply);
  CHECK(read_stun(std::vector<std::uint8_t>(10, 0), id).result == StunResult::kNotStun);
  // A punch or an ENet datagram is not STUN.
  const auto punch = encode_punch(Punch{}, coop::server::nat::punch_key("x"));
  CHECK(read_stun(punch, id).result == StunResult::kNotStun);
}

TEST_CASE("stun: malformed answers are refused, never read past the end") {
  const StunId id = id_of(1);
  auto good = stun_reply(id, xor_mapped(kHostAddr));
  // Every shortened copy.
  for (std::size_t n = 0; n < good.size(); ++n) {
    CAPTURE(n);
    CHECK(read_stun(std::span<const std::uint8_t>(good.data(), n), id).result != StunResult::kOk);
  }
  // The header's length disagrees with the datagram.
  auto longer = good;
  longer[3] = static_cast<std::uint8_t>(longer[3] + 4);
  CHECK(read_stun(longer, id).result == StunResult::kMalformed);
  // An attribute longer than what is left.
  auto overrun = good;
  overrun[23] = 0x40;
  CHECK(read_stun(overrun, id).result == StunResult::kMalformed);
  // Length not a multiple of 4.
  auto odd = stun_reply(id, {0x80, 0x22, 0x00, 0x01, 'a'});
  CHECK(read_stun(odd, id).result == StunResult::kMalformed);
  // Short address attribute: no address.
  CHECK(read_stun(stun_reply(id, {0x00, 0x20, 0x00, 0x04, 0, 1, 0, 0}), id).result == StunResult::kNoAddress);
  // Fuzz: a right header and id, random attributes.
  std::mt19937 random(165);
  for (int round = 0; round < 20000; ++round) {
    std::vector<std::uint8_t> attrs(static_cast<std::size_t>(random() % 64) * 4);
    for (auto& b : attrs) b = static_cast<std::uint8_t>(random());
    auto d = stun_reply(id, attrs);
    if (random() % 4 == 0 && !d.empty()) d.resize(random() % d.size());
    const StunAnswer r = read_stun(d, id);
    if (r.result == StunResult::kOk) CHECK(r.mapped.port != 0);
  }
}

TEST_CASE("punch: signed, fresh and the right size, or nothing") {
  const auto key = punch_key("join-pw");
  Punch p;
  p.type = PunchType::kGuestPunch;
  p.nonce = {9, 8, 7, 6, 5, 4, 3, 2};
  p.time = kNow;
  p.from = kGuestAddr;
  p.to = kHostAddr;
  const auto bytes = encode_punch(p, key);
  PunchRead r = read_punch(bytes, key, kNow + 1);
  REQUIRE(r.check == PunchCheck::kOk);
  CHECK(r.punch.type == PunchType::kGuestPunch);
  CHECK(r.punch.nonce == p.nonce);
  CHECK(r.punch.from == kGuestAddr);
  CHECK(r.punch.to == kHostAddr);
  CHECK(read_punch(bytes, punch_key("other"), kNow).check == PunchCheck::kBadSignature);
  CHECK(read_punch(bytes, key, kNow + kPunchMaxAgeSeconds + 1).check == PunchCheck::kStale);
  CHECK(read_punch(bytes, key, kNow - kPunchMaxAgeSeconds - 1).check == PunchCheck::kStale);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    auto bad = bytes;
    bad[i] ^= 0x01;
    CAPTURE(i);
    CHECK(read_punch(bad, key, kNow).check != PunchCheck::kOk);
  }
  CHECK(read_punch(std::span<const std::uint8_t>(bytes.data(), bytes.size() - 1), key, kNow).check ==
        PunchCheck::kBadSize);
  // The join proof for this password is not a punch key.
  CHECK(punch_key("join-pw") != coop::protocol::hmac_sha256(
                                    std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>("join-pw"), 7),
                                    std::span<const std::uint8_t>()));
}

namespace {

struct Sent {
  Addr to;
  std::vector<std::uint8_t> bytes;
};

struct HostFixture {
  std::vector<Sent> sent;
  std::vector<std::string> lines;
  std::uint64_t clock = kNow;
  std::uint8_t next_id = 1;
  HostRendezvous host{[this] {
    HostConfig c;
    c.password = "join-pw";
    c.stun_servers = {kStunA, kStunB};
    c.send = [this](Addr to, std::span<const std::uint8_t> b) { sent.push_back({to, {b.begin(), b.end()}}); };
    c.log = [this](const std::string& s) { lines.push_back(s); };
    c.unix_now = [this] { return clock; };
    c.random_id = [this] { return id_of(static_cast<std::uint8_t>(16 * next_id++)); };
    return c;
  }()};
  std::array<std::uint8_t, kPunchBytes> punch(PunchType type, Nonce nonce, std::string_view password = "join-pw",
                                              std::uint64_t time = 0) {
    Punch p;
    p.type = type;
    p.nonce = nonce;
    p.time = time ? time : clock;
    p.from = kGuestAddr;
    p.to = kHostAddr;
    return encode_punch(p, punch_key(password));
  }
};

}  // namespace

TEST_CASE("host rendezvous: STUN only from the servers asked, with our id") {
  HostFixture f;
  f.host.start(0);
  REQUIRE(f.sent.size() == 2);
  CHECK(f.sent[0].to == kStunA);
  CHECK(f.sent[1].to == kStunB);
  const StunId id_a = id_of(16);
  const StunId id_b = id_of(32);
  // The right answer from a stranger: not ours, ENet gets it (and drops it).
  CHECK_FALSE(f.host.on_datagram(kGuestAddr, stun_reply(id_a, xor_mapped(kGuestAddr)), 10));
  // Server A with B's id: ignored.
  CHECK(f.host.on_datagram(kStunA, stun_reply(id_b, xor_mapped(kGuestAddr)), 10));
  CHECK_FALSE(f.host.public_address().has_value());
  CHECK(f.host.stats().stun_ignored == 1);
  CHECK(f.host.on_datagram(kStunA, stun_reply(id_a, xor_mapped(kHostAddr)), 20));
  REQUIRE(f.host.public_address().has_value());
  CHECK(*f.host.public_address() == kHostAddr);
  // B sees another port: the router gives each destination its own port.
  CHECK(f.host.on_datagram(kStunB, stun_reply(id_b, xor_mapped(Addr{kHostAddr.ip, 50000})), 30));
  f.host.service(40);
  CHECK(f.host.stun_done());
  CHECK(f.host.symmetric());
  CHECK(*f.host.public_address() == kHostAddr);
}

TEST_CASE("host rendezvous: no answers: retries, then gives up") {
  HostFixture f;
  f.host.start(0);
  f.host.service(800);
  CHECK(f.sent.size() == 4);
  f.host.service(1700);
  CHECK(f.sent.size() == 6);
  f.host.service(3100);
  CHECK(f.host.stun_done());
  CHECK_FALSE(f.host.public_address().has_value());
}

TEST_CASE("host rendezvous: a stranger without the password gets nothing") {
  HostFixture f;
  f.host.start(0);
  f.sent.clear();
  const Nonce n{1, 1, 1, 1, 1, 1, 1, 1};
  CHECK(f.host.on_datagram(kGuestAddr, f.punch(PunchType::kProbe, n, "guess"), 100));
  CHECK(f.host.on_datagram(kGuestAddr, f.punch(PunchType::kGuestPunch, n, "guess"), 100));
  std::vector<std::uint8_t> junk = {'O', 'C', 'P', 1, 9, 9, 9};
  CHECK(f.host.on_datagram(kGuestAddr, junk, 100));
  // Signed, but too old (a recorded probe sent again).
  CHECK(f.host.on_datagram(kGuestAddr, f.punch(PunchType::kProbe, n, "join-pw", kNow - 61), 100));
  // Signed, but no answer code for this nonce was pasted: no punch anywhere.
  CHECK(f.host.on_datagram(kGuestAddr, f.punch(PunchType::kGuestPunch, n), 100));
  // A host's own kind of punch is not answered.
  CHECK(f.host.on_datagram(kGuestAddr, f.punch(PunchType::kHostPunch, n), 100));
  CHECK(f.sent.empty());
  CHECK(f.host.stats().rejected == 6);
  // Plain ENet traffic is not ours.
  CHECK_FALSE(f.host.on_datagram(kGuestAddr, std::vector<std::uint8_t>{0x80, 0x00, 0x12, 0x34}, 100));
}

TEST_CASE("host rendezvous: a signed probe gets one answer, no bigger, rate limited") {
  HostFixture f;
  f.host.start(0);
  f.sent.clear();
  const Nonce n{2, 2, 2, 2, 2, 2, 2, 2};
  const auto probe = f.punch(PunchType::kProbe, n);
  CHECK(f.host.on_datagram(kGuestAddr, probe, 5000));
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].to == kGuestAddr);
  CHECK(f.sent[0].bytes.size() <= probe.size());
  const PunchRead ack = read_punch(f.sent[0].bytes, punch_key("join-pw"), kNow);
  REQUIRE(ack.check == PunchCheck::kOk);
  CHECK(ack.punch.type == PunchType::kProbeAck);
  CHECK(ack.punch.nonce == n);
  // A flood from one address in one second: 10 answers at most.
  f.sent.clear();
  for (int i = 0; i < 50; ++i) f.host.on_datagram(kGuestAddr, probe, 6000);
  CHECK(f.sent.size() == 10);
  CHECK(f.host.stats().rate_limited == 40);
}

TEST_CASE("host rendezvous: punches go only toward a signed answer code's address") {
  HostFixture f;
  f.host.start(0);
  Answer a;
  a.guest = kGuestAddr;
  a.nonce = {3, 3, 3, 3, 3, 3, 3, 3};
  a.time = static_cast<std::uint32_t>(kNow);
  CHECK(f.host.add_answer(make_answer(a, "wrong-pw"), 0) == CodeError::kBadSignature);
  Answer lan = a;
  lan.guest = Addr{0xC0A80002u, 40001};
  CHECK(f.host.add_answer(make_answer(lan, "join-pw"), 0) == CodeError::kNotPublic);
  CHECK(f.host.add_answer("hello", 0) == CodeError::kNotACode);
  CHECK(f.host.answers() == 0);
  f.sent.clear();
  REQUIRE(f.host.add_answer(make_answer(a, "join-pw"), 1000) == CodeError::kOk);
  f.host.service(1000);
  f.host.service(1100);  // not yet 200 ms
  f.host.service(1200);
  REQUIRE(f.sent.size() == 2);
  for (const auto& s : f.sent) {
    CHECK(s.to == kGuestAddr);
    const PunchRead r = read_punch(s.bytes, punch_key("join-pw"), kNow);
    REQUIRE(r.check == PunchCheck::kOk);
    CHECK(r.punch.type == PunchType::kHostPunch);
    CHECK(r.punch.nonce == a.nonce);
  }
  // The friend's punch comes from another port: the answer still goes to the
  // answer code's address, never to the sender.
  f.sent.clear();
  const Addr other{kGuestAddr.ip, 40999};
  CHECK(f.host.on_datagram(other, f.punch(PunchType::kGuestPunch, a.nonce), 1250));
  REQUIRE(f.sent.size() == 1);
  CHECK(f.sent[0].to == kGuestAddr);
  CHECK(f.host.stats().guest_punches == 1);
  // After 10 s it stops punching by itself; after 10 minutes it forgets.
  f.sent.clear();
  f.host.service(11'500);
  CHECK(f.sent.empty());
  f.host.service(1000 + 600'000);
  CHECK(f.host.answers() == 0);
  CHECK(f.host.on_datagram(kGuestAddr, f.punch(PunchType::kGuestPunch, a.nonce), 700'000));
  CHECK(f.sent.empty());
}
