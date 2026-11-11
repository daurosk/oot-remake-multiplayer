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
    } else {
      error = "unknown argument: " + arg;
      return std::nullopt;
    }
  }
  return line;
}

const char* usage() {
  return "usage: coop_server [--port N] [--bind ADDRESS] [--allow LIST]\n"
         "                   [--game-build ID] [--max-half-open N] [--run-for-ms N]\n"
         "  --port N         UDP port to listen on (default 27510, 0 for any)\n"
         "  --bind ADDRESS   listen only on this IPv4 address of this PC, for\n"
         "                   example its Hamachi or Radmin address (default: all)\n"
         "  --allow LIST     only these players' IPv4 addresses may connect,\n"
         "                   comma-separated, ranges as 26.0.0.0/8 (default: any)\n"
         "  --game-build ID  game build every player must run (default: the\n"
         "                   first player to join decides)\n"
         "  --max-half-open N  most connections one address may hold before\n"
         "                   sending HELLO (default 4, 0 for no limit)\n"
         "  --run-for-ms N   stop by itself after N milliseconds\n";
}

}  // namespace coop::server
