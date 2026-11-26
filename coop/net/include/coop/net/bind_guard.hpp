#pragma once

// A test guard for the addresses sockets bind to.
//
// Windows Defender Firewall asks the player to allow every program path that
// listens on all addresses (0.0.0.0). A unit test that does that pops the
// question for each build folder, and a person ends up with dozens of rules.
// Every listener of the server library takes the address to bind (none: all
// addresses, as for a player) and asks bind_allowed() before it binds. The
// test programs call require_loopback_binds(true) once at start: from then on
// a bind to anything but 127.x.x.x is refused and counted, so a test (or a new
// listener) that forgets the address fails instead of opening the firewall.
//
// IPv4 addresses here are in host order (127.0.0.1 is 0x7F000001), as in
// ServerOptions::bind. Nothing here opens a socket.

#include <atomic>
#include <cstdint>
#include <optional>

namespace coop::net {

inline constexpr std::uint32_t kLoopbackIp = 0x7F000001u;

namespace detail {
inline std::atomic<bool> g_require_loopback{false};
inline std::atomic<int> g_refused{0};
inline std::atomic<std::int64_t> g_test_bind{-1};  // -1: none
}  // namespace detail

// True for 127.0.0.0/8.
constexpr bool is_loopback_ip(std::uint32_t ip) { return (ip >> 24) == 127u; }

// Test programs: refuse every bind that is not loopback.
inline void require_loopback_binds(bool on) { detail::g_require_loopback = on; }
inline bool loopback_binds_required() { return detail::g_require_loopback; }

// May a socket bind to `ip` (none: all addresses)? Always yes unless the test
// guard is on; then only loopback. A refusal is counted (refused_binds()).
inline bool bind_allowed(std::optional<std::uint32_t> ip) {
  if (!detail::g_require_loopback) return true;
  if (ip && is_loopback_ip(*ip)) return true;
  ++detail::g_refused;
  return false;
}

// The address the helper sockets of code a test cannot configure one by one (the Hookshot menu's join helper, relay
// lookup, room list and the host's relay link and router lookups) bind to. None for a player: all addresses, as
// before. The test programs set 127.0.0.1.
inline void set_test_bind(std::optional<std::uint32_t> ip) { detail::g_test_bind = ip ? std::int64_t{*ip} : -1; }
inline std::optional<std::uint32_t> test_bind() {
  const std::int64_t v = detail::g_test_bind;
  if (v < 0) return std::nullopt;
  return static_cast<std::uint32_t>(v);
}

// How many binds the guard refused so far.
inline int refused_binds() { return detail::g_refused; }
inline void reset_refused_binds() { detail::g_refused = 0; }

}  // namespace coop::net
