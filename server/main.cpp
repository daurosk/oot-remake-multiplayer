// coop_server: the headless co-op relay server (docs/protocol.md).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "coop/protocol/messages.hpp"
#include "enet_server.hpp"
#include "options.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

// An environment variable's value; empty when it is not set.
std::string environment(const std::string& name) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, name.c_str()) != 0 || value == nullptr) return {};
  std::string out(value);
  std::free(value);
  return out;
#else
  const char* value = std::getenv(name.c_str());
  return value == nullptr ? std::string() : std::string(value);
#endif
}

}  // namespace

int main(int argc, char** argv) {
  using namespace coop::server;

  std::vector<std::string> args(argv + 1, argv + argc);
  std::string error;
  auto line = parse_command_line(args, error);
  if (!line) {
    std::fprintf(stderr, "coop_server: %s\n%s", error.c_str(), usage());
    return 2;
  }
  if (line->help) {
    std::printf("%s", usage());
    return 0;
  }
  if (!line->password_env.empty()) {
    line->password = environment(line->password_env);
    if (const char* why = password_problem(line->password)) {
      std::fprintf(stderr, "coop_server: the join password in %s is %s\n",
                   line->password_env.c_str(), why);
      return 2;
    }
  }

  if (enet_initialize() != 0) {
    std::fprintf(stderr, "coop_server: ENet failed to start\n");
    return 1;
  }

  int exit_code = 0;
  {
    ServerOptions options;
    options.port = line->port;
    options.session.game_build_id = line->game_build_id;
    options.session.max_half_open_per_address = line->max_half_open;
    options.session.allowed = line->allowed;
    options.session.join_password = line->password;
    options.bind = line->bind;
    options.session.log = [](const std::string& text) {
      std::printf("coop_server: %s\n", text.c_str());
      std::fflush(stdout);
    };
    EnetServer server(options);
    if (!server.start()) {
      std::fprintf(stderr, "coop_server: cannot listen on port %u\n",
                   static_cast<unsigned>(line->port));
      exit_code = 1;
    } else {
      // On every network adapter (a VPN's too) unless --bind, with VPN-safe
      // datagrams (issue 143).
      std::string where = "all network adapters";
      if (line->bind) {
        const std::uint32_t v = *line->bind;
        where = "only " + std::to_string(v >> 24) + "." + std::to_string((v >> 16) & 255u) + "." +
                std::to_string((v >> 8) & 255u) + "." + std::to_string(v & 255u);
      }
      std::printf("coop_server: listening on port %u (%s, datagrams of %u bytes)\n",
                  static_cast<unsigned>(server.port()), where.c_str(),
                  static_cast<unsigned>(coop::protocol::kDatagramBytes));
      if (!line->password.empty()) {
        std::printf("coop_server: players must give the join password to join\n");
      }
      if (!line->allowed.empty()) {
        std::printf("coop_server: only the %u address range(s) given with --allow may connect\n",
                    static_cast<unsigned>(line->allowed.size()));
      }
      std::fflush(stdout);
      std::signal(SIGINT, on_signal);
      std::signal(SIGTERM, on_signal);

      auto started = std::chrono::steady_clock::now();
      std::uint64_t shown_resets = 0;
      std::uint64_t shown_limited = 0;
      while (!g_stop) {
        server.service(5);
        // Counts a window or a test can read while the server runs (it is
        // often closed, not stopped): each host save load, and messages over
        // a player's rate (the first, then every 100th).
        const Stats& now = server.session().stats();
        if (now.host_resets != shown_resets) {
          shown_resets = now.host_resets;
          std::printf("coop_server: the host loaded a save: the group follows it (host_resets %llu)\n",
                      static_cast<unsigned long long>(shown_resets));
          std::fflush(stdout);
        }
        if (now.rate_limited != shown_limited &&
            (shown_limited == 0 || now.rate_limited / 100 != shown_limited / 100)) {
          std::printf("coop_server: messages over a player's rate dropped so far: %llu\n",
                      static_cast<unsigned long long>(now.rate_limited));
          std::fflush(stdout);
        }
        shown_limited = now.rate_limited;
        if (line->run_for_ms &&
            std::chrono::steady_clock::now() - started >=
                std::chrono::milliseconds(*line->run_for_ms)) {
          break;
        }
      }
      const Stats& stats = server.session().stats();
      std::printf("coop_server: stopping; relayed %llu, dropped %llu, "
                  "half-open refused %llu, not allowed %llu\n",
                  static_cast<unsigned long long>(stats.relayed),
                  static_cast<unsigned long long>(stats.dropped()),
                  static_cast<unsigned long long>(stats.half_open_limited),
                  static_cast<unsigned long long>(stats.not_allowed));
      std::printf("coop_server: host save loads %llu, over the rate %llu\n",
                  static_cast<unsigned long long>(stats.host_resets),
                  static_cast<unsigned long long>(stats.rate_limited));
      std::printf("coop_server: flags relayed %llu, refused %llu, clashes %llu; "
                  "join relayed %llu, not there %llu, dropped %llu\n",
                  static_cast<unsigned long long>(stats.flag_relayed),
                  static_cast<unsigned long long>(stats.flag_refused),
                  static_cast<unsigned long long>(stats.flag_clashes),
                  static_cast<unsigned long long>(stats.join_relayed),
                  static_cast<unsigned long long>(stats.join_not_there),
                  static_cast<unsigned long long>(stats.join_dropped));
    }
  }
  enet_deinitialize();
  return exit_code;
}
