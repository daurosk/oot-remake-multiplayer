#include "stun.hpp"

#include <algorithm>

namespace coop::server::nat {

namespace {

constexpr std::uint16_t kBindingRequest = 0x0001;
constexpr std::uint16_t kBindingSuccess = 0x0101;
constexpr std::uint16_t kBindingError = 0x0111;
constexpr std::uint16_t kMappedAddress = 0x0001;
constexpr std::uint16_t kXorMappedAddress = 0x0020;
constexpr std::uint16_t kXorMappedAddressOld = 0x8020;  // some servers still send this one

std::uint16_t u16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t u32(const std::uint8_t* p) {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

// An address attribute's value: reserved, family, port, IPv4.
bool read_address(std::span<const std::uint8_t> value, bool xored, Addr& out) {
  if (value.size() < 8 || value[1] != 0x01) return false;  // 0x02 is IPv6: not used
  std::uint16_t port = u16(&value[2]);
  std::uint32_t ip = u32(&value[4]);
  if (xored) {
    port = static_cast<std::uint16_t>(port ^ (kStunCookie >> 16));
    ip ^= kStunCookie;
  }
  out.ip = ip;
  out.port = port;
  return port != 0;
}

}  // namespace

std::array<std::uint8_t, 20> stun_request(const StunId& id) {
  std::array<std::uint8_t, 20> out{};
  out[0] = static_cast<std::uint8_t>(kBindingRequest >> 8);
  out[1] = static_cast<std::uint8_t>(kBindingRequest);
  // length 0: no attributes
  out[4] = static_cast<std::uint8_t>(kStunCookie >> 24);
  out[5] = static_cast<std::uint8_t>(kStunCookie >> 16);
  out[6] = static_cast<std::uint8_t>(kStunCookie >> 8);
  out[7] = static_cast<std::uint8_t>(kStunCookie);
  std::copy(id.begin(), id.end(), out.begin() + 8);
  return out;
}

StunAnswer read_stun(std::span<const std::uint8_t> d, const StunId& id) {
  StunAnswer answer;
  // Header: the two top bits zero, the magic cookie, a length that is a
  // multiple of 4.
  if (d.size() < 20 || (d[0] & 0xC0u) != 0 || u32(&d[4]) != kStunCookie) return answer;
  answer.result = StunResult::kOtherId;
  if (!std::equal(id.begin(), id.end(), d.begin() + 8)) return answer;
  answer.result = StunResult::kMalformed;
  const std::size_t length = u16(&d[2]);
  if (length % 4 != 0 || 20 + length != d.size()) return answer;
  const std::uint16_t type = u16(&d[0]);
  if (type == kBindingError) {
    answer.result = StunResult::kErrorReply;
    return answer;
  }
  if (type != kBindingSuccess) return answer;

  bool have_xor = false;
  bool have_plain = false;
  Addr xor_addr;
  Addr plain_addr;
  std::size_t at = 20;
  while (at < d.size()) {
    if (d.size() - at < 4) return answer;
    const std::uint16_t attr = u16(&d[at]);
    const std::size_t len = u16(&d[at + 2]);
    const std::size_t padded = (len + 3) & ~std::size_t{3};
    if (padded > d.size() - at - 4) return answer;
    const auto value = d.subspan(at + 4, len);
    if ((attr == kXorMappedAddress || attr == kXorMappedAddressOld) && !have_xor) {
      have_xor = read_address(value, true, xor_addr);
    } else if (attr == kMappedAddress && !have_plain) {
      have_plain = read_address(value, false, plain_addr);
    }
    at += 4 + padded;
  }
  if (have_xor) {
    answer.result = StunResult::kOk;
    answer.mapped = xor_addr;
  } else if (have_plain) {
    answer.result = StunResult::kOk;
    answer.mapped = plain_addr;
  } else {
    answer.result = StunResult::kNoAddress;
  }
  return answer;
}

}  // namespace coop::server::nat
