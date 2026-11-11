#pragma once

// coop_server's command line.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "session.hpp"

namespace coop::server {

struct CommandLine {
  std::uint16_t port = 27510;  // docs/protocol.md, "Transport"
  std::string game_build_id;   // empty: the first player decides
  std::optional<std::uint32_t> run_for_ms;  // stop by itself after this long
  // Connections one address may hold before HELLO; 0: no limit.
  std::uint32_t max_half_open = 4;
  // --bind: listen on this one IPv4 address only (a VPN adapter's); none: all.
  std::optional<std::uint32_t> bind;
  // --allow: only these addresses may connect; empty: any.
  std::vector<AddressRange> allowed;
  // --password: the join password (empty: none). --password-env names an
  // environment variable that holds it instead, so it is not on the command
  // line (where other programs can read it); main() reads that variable.
  std::string password;
  std::string password_env;
  // --pvp on|off: players hurt each other (docs/play-online.md, "PvP").
  bool pvp = false;
  bool help = false;
};

// Parses the arguments after the program name. On a mistake, returns nothing
// and puts the reason in `error`.
std::optional<CommandLine> parse_command_line(const std::vector<std::string>& args,
                                              std::string& error);

// Why a join password cannot be used (too long, a control character), or
// null when it is fine. Empty is fine: no password.
const char* password_problem(std::string_view password);

// The --help text.
const char* usage();

}  // namespace coop::server
