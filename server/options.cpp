#include "options.hpp"

#include <charconv>
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
    } else {
      error = "unknown argument: " + arg;
      return std::nullopt;
    }
  }
  return line;
}

const char* usage() {
  return "usage: coop_server [--port N] [--game-build ID] [--run-for-ms N]\n"
         "  --port N         UDP port to listen on (default 27510, 0 for any)\n"
         "  --game-build ID  game build every player must run (default: the\n"
         "                   first player to join decides)\n"
         "  --run-for-ms N   stop by itself after N milliseconds\n";
}

}  // namespace coop::server
