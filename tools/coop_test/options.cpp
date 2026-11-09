#include "options.hpp"

#include <charconv>
#include <cstddef>
#include <limits>
#include <system_error>

namespace coop::test_tool {

namespace {

template <typename T>
bool parse_number(const std::string& text, T& out, T low, T high) {
  T value{};
  const char* first = text.data();
  const char* last = first + text.size();
  auto [end, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || end != last || text.empty()) return false;
  if (value < low || value > high) return false;
  out = value;
  return true;
}

}  // namespace

std::optional<Options> parse_options(const std::vector<std::string>& args,
                                     std::string& error) {
  Options options;
  bool clients_given = false;
  bool port_given = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= args.size()) {
        error = arg + " needs a value";
        return false;
      }
      out = args[++i];
      return true;
    };
    std::string text;
    if (arg == "--help" || arg == "-h") {
      options.help = true;
    } else if (arg == "--server-only") {
      options.server_only = true;
    } else if (arg == "--clients") {
      if (!value(text)) return std::nullopt;
      if (!parse_number(text, options.clients, 1, 4)) {
        error = "--clients must be 1 to 4";
        return std::nullopt;
      }
      clients_given = true;
    } else if (arg == "--seconds") {
      if (!value(text)) return std::nullopt;
      if (!parse_number<std::uint32_t>(text, options.seconds, 0, 86400 * 7)) {
        error = "--seconds must be 0 to 604800";
        return std::nullopt;
      }
    } else if (arg == "--port") {
      if (!value(text)) return std::nullopt;
      if (!parse_number<std::uint16_t>(text, options.port, 0, 65535)) {
        error = "--port must be 0 to 65535";
        return std::nullopt;
      }
      port_given = true;
    } else if (arg == "--report-every") {
      if (!value(text)) return std::nullopt;
      if (!parse_number<std::uint32_t>(text, options.report_every, 0, 86400)) {
        error = "--report-every must be 0 to 86400";
        return std::nullopt;
      }
    } else if (arg == "--connect") {
      if (!value(text)) return std::nullopt;
      std::size_t colon = text.rfind(':');
      if (colon == std::string::npos || colon == 0 ||
          !parse_number<std::uint16_t>(text.substr(colon + 1), options.connect_port, 1,
                                       65535)) {
        error = "--connect must be host:port, with a port of 1 to 65535";
        return std::nullopt;
      }
      options.connect_host = text.substr(0, colon);
    } else if (arg == "--name") {
      if (!value(text)) return std::nullopt;
      // Room for the index: nicknames are at most 16 characters.
      if (text.empty() || text.size() > 15) {
        error = "--name must be 1 to 15 characters";
        return std::nullopt;
      }
      for (char c : text) {
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7f) {
          error = "--name must be plain ASCII letters, digits or signs";
          return std::nullopt;
        }
      }
      options.name = text;
    } else {
      error = "unknown option " + arg;
      return std::nullopt;
    }
  }
  if (options.server_only && options.connect_host) {
    error = "--server-only and --connect cannot go together";
    return std::nullopt;
  }
  if (options.server_only && clients_given) {
    error = "--server-only runs no clients; leave out --clients";
    return std::nullopt;
  }
  if (options.connect_host && port_given) {
    error = "--connect runs no server; give the port in host:port";
    return std::nullopt;
  }
  return options;
}

const char* usage() {
  return "usage: coop_test [options]\n"
         "Runs the co-op server and clients that send test traffic and fake player\n"
         "samples, and reports round trip, clock offset, losses and the shown error\n"
         "of the fake path as coop/presence draws it.\n"
         "\n"
         "  (no mode)            server and clients in this process\n"
         "  --server-only        only the server\n"
         "  --connect HOST:PORT  only clients, to that server\n"
         "  --clients N          clients in this process, 1 to 4 (default 2)\n"
         "  --seconds N          how long to send, 0 for until Ctrl+C (default 10)\n"
         "  --port N             the server's port, 0 for any free one (default 27510)\n"
         "  --report-every N     also report every N seconds, 0 for never (default 10)\n"
         "  --name TEXT          nickname prefix, 1 to 15 characters (default test)\n"
         "  --help               this text\n"
         "\n"
         "Exits 1 if a reliable message was lost, a client was refused or dropped,\n"
         "or a client never heard from another one in this process; 2 on a bad\n"
         "command line.\n";
}

}  // namespace coop::test_tool
