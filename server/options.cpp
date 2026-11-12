#include "options.hpp"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <limits>
#include <system_error>

namespace coop::server {

namespace {

template <typename T>
bool parse_number(const std::string& text, T& out) {
  const char* first = text.data();
  const char* last = text.data() + text.size();
  auto [end, ec] = std::from_chars(first, last, out);
  return ec == std::errc{} && end == last && !text.empty();
}

}  // namespace

const char* password_problem(std::string_view password) {
  if (password.size() > protocol::kMaxJoinPasswordBytes) return "longer than 128 bytes";
  for (const char ch : password) {
    const auto b = static_cast<unsigned char>(ch);
    if (b < 0x20 || b == 0x7f) return "has a control character";
  }
  return nullptr;
}

std::optional<CommandLine> parse_command_line(const std::vector<std::string>& args,
                                              std::string& error) {
  CommandLine line;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    bool has_value = i + 1 < args.size();
    if (arg == "--help" || arg == "-h") {
      line.help = true;
    } else if (arg == "--port") {
      if (!has_value || !parse_number(args[++i], line.port)) {
        error = "--port needs a number from 0 to 65535";
        return std::nullopt;
      }
      line.port_given = true;
    } else if (arg == "--game-build") {
      if (!has_value || args[i + 1].empty()) {
        error = "--game-build needs a game build id";
        return std::nullopt;
      }
      line.game_build_id = args[++i];
    } else if (arg == "--run-for-ms") {
      std::uint32_t ms = 0;
      if (!has_value || !parse_number(args[++i], ms)) {
        error = "--run-for-ms needs a number of milliseconds";
        return std::nullopt;
      }
      line.run_for_ms = ms;
    } else if (arg == "--max-half-open") {
      if (!has_value || !parse_number(args[++i], line.max_half_open)) {
        error = "--max-half-open needs a number (0 for no limit)";
        return std::nullopt;
      }
    } else if (arg == "--bind") {
      const auto r = has_value ? parse_address_range(args[i + 1]) : std::nullopt;
      if (!r || r->prefix != 32) {
        error = "--bind needs one IPv4 address of this PC, for example 25.1.2.3";
        return std::nullopt;
      }
      line.bind = r->first;
      ++i;
    } else if (arg == "--allow") {
      if (!has_value || args[i + 1].empty()) {
        error = "--allow needs IPv4 addresses or ranges, for example 25.1.2.3,26.0.0.0/8";
        return std::nullopt;
      }
      const std::string& list = args[++i];
      std::size_t start = 0;
      while (start <= list.size()) {
        const std::size_t comma = std::min(list.find(',', start), list.size());
        const auto r = parse_address_range(std::string_view(list).substr(start, comma - start));
        if (!r) {
          error = "--allow: not an IPv4 address or range: " + list.substr(start, comma - start);
          return std::nullopt;
        }
        line.allowed.push_back(*r);
        start = comma + 1;
      }
    } else if (arg == "--pvp") {
      const std::string value = has_value ? args[i + 1] : std::string();
      if (value == "on" || value == "1" || value == "yes") {
        line.pvp = true;
      } else if (value == "off" || value == "0" || value == "no") {
        line.pvp = false;
      } else {
        error = "--pvp needs on or off";
        return std::nullopt;
      }
      ++i;
    } else if (arg == "--password" || arg == "--password-env") {
      if (!has_value || args[i + 1].empty()) {
        error = arg + " needs a value";
        return std::nullopt;
      }
      const std::string& value = args[++i];
      if (arg == "--password") {
        if (const char* why = password_problem(value)) {
          error = std::string("--password: ") + why;
          return std::nullopt;
        }
        line.password = value;
      } else {
        line.password_env = value;
      }
    } else if (arg == "--upnp") {
      line.upnp = true;
    } else if (arg == "--invite") {
      line.invite = true;
    } else if (arg == "--stun" || arg == "--join") {
      if (!has_value || args[i + 1].empty() || args[i + 1].size() > 260) {
        error = arg + " needs an address, for example stun.example.net:3478";
        return std::nullopt;
      }
      if (arg == "--stun") {
        line.stun.push_back(args[++i]);
      } else {
        line.join = args[++i];
      }
    } else if (arg == "--game-port") {
      if (!has_value || !parse_number(args[i + 1], line.game_port)) {
        error = "--game-port needs a port number";
        return std::nullopt;
      }
      ++i;
    } else if (arg == "--relay") {
      line.relay = true;
    } else if (arg == "--relay-via") {
      if (!has_value || args[i + 1].empty()) {
        error = "--relay-via needs the relay's address, for example relay.example.net:27530";
        return std::nullopt;
      }
      line.relay_via = args[++i];
    } else if (arg == "--relay-key" || arg == "--relay-key-env") {
      if (!has_value || args[i + 1].empty() || args[i + 1].size() > 64) {
        error = arg + " needs a value of at most 64 characters";
        return std::nullopt;
      }
      (arg == "--relay-key" ? line.relay_key : line.relay_key_env) = args[++i];
    } else {
      error = "unknown argument: " + arg;
      return std::nullopt;
    }
  }
  if (line.relay && (line.upnp || !line.relay_via.empty())) {
    error = "--relay runs a relay; it cannot also use --upnp or --relay-via";
    return std::nullopt;
  }
  if (!line.join.empty() && (line.relay || line.upnp || line.invite || !line.relay_via.empty())) {
    error = "--join runs Join.bat's helper; it cannot also host (--upnp, --invite, --relay, --relay-via)";
    return std::nullopt;
  }
  if (line.relay && line.invite) {
    error = "--relay runs a relay; it cannot also use --invite";
    return std::nullopt;
  }
  if (line.relay && !line.port_given) line.port = 27530;
  return line;
}

const char* usage() {
  return "usage: coop_server [--port N] [--bind ADDRESS] [--allow LIST]\n"
         "                   [--game-build ID] [--max-half-open N] [--run-for-ms N]\n"
         "                   [--password TEXT | --password-env NAME] [--pvp on|off]\n"
         "  --port N         UDP port to listen on (default 27510, 0 for any)\n"
         "  --bind ADDRESS   listen only on this IPv4 address of this PC, for\n"
         "                   example its Hamachi or Radmin address (default: all)\n"
         "  --allow LIST     only these players' IPv4 addresses may connect,\n"
         "                   comma-separated, ranges as 26.0.0.0/8 (default: any)\n"
         "  --game-build ID  game build every player must run (default: the\n"
         "                   first player to join decides)\n"
         "  --max-half-open N  most connections one address may hold before\n"
         "                   sending HELLO (default 4, 0 for no limit)\n"
         "  --run-for-ms N   stop by itself after N milliseconds\n"
         "  --password TEXT  players must give this join password (default: none)\n"
         "  --password-env NAME  the join password is in this environment\n"
         "                   variable (keeps it off the command line)\n"
         "  --upnp           ask the router (UPnP) to forward the port while the\n"
         "                   server runs; needs a join password or --allow\n"
         "  --relay-via ADDRESS[:PORT]  host through a relay (default port 27530)\n"
         "                   and print a join code for the players\n"
         "  --relay          run as a relay for hosts that use --relay-via\n"
         "                   (UDP port 27530 unless --port), not a game server\n"
         "  --relay-key TEXT | --relay-key-env NAME  the relay's host key: on the\n"
         "                   relay, hosts must give it; with --relay-via, the one\n"
         "                   to give\n"
         "  --invite         learn this PC's internet address (STUN), print an invite\n"
         "                   code with it and the join password, and take answer\n"
         "                   codes typed here; needs a join password\n"
         "  --stun NAME:PORT STUN server to ask (more than once; default: Google's\n"
         "                   and Cloudflare's free ones)\n"
         "  --join ADDRESS:PORT [--game-port N]  Join.bat's helper for an invite\n"
         "                   code: the game connects to 127.0.0.1:N (27512)\n"
         "  --pvp on|off     players can hurt each other (default: off); the\n"
         "                   host can change it in the game with /pvp\n";
}

}  // namespace coop::server
