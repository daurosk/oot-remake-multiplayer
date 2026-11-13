#pragma once

// coop_test's command line.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace coop::test_tool {

struct Options {
  // Clients run in this process (1 to 4). Not used with --server-only.
  int clients = 2;
  // How long the clients send, in seconds. 0: until Ctrl+C.
  std::uint32_t seconds = 10;
  // Run only the server.
  bool server_only = false;
  // Run only clients, pointed at this server (host:port).
  std::optional<std::string> connect_host;
  std::uint16_t connect_port = 0;
  // The port the server in this process listens on. 0: any free port.
  std::uint16_t port = 27510;
  // Print a report every this many seconds as well as at the end. 0: only at
  // the end.
  std::uint32_t report_every = 10;
  // Nicknames are this and the client's index, 1 to 4 ("test" then the index). Two
  // processes on one server need different ones.
  std::string name = "test";
  // The join password (every session has one, and it encrypts the traffic):
  // the server's here, or the one to give with --connect.
  std::string password = "coop-test";
  bool help = false;
};

// Parses the arguments after the program name. On a mistake, returns nothing
// and puts the reason in `error`.
std::optional<Options> parse_options(const std::vector<std::string>& args,
                                     std::string& error);

// The --help text.
const char* usage();

}  // namespace coop::test_tool
