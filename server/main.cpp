// coop_server: the headless co-op relay server (docs/protocol.md).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "coop/protocol/messages.hpp"
#include "enet_server.hpp"
#include "options.hpp"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

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
      if (!line->allowed.empty()) {
        std::printf("coop_server: only the %u address range(s) given with --allow may connect\n",
                    static_cast<unsigned>(line->allowed.size()));
      }
      std::fflush(stdout);
      std::signal(SIGINT, on_signal);
      std::signal(SIGTERM, on_signal);

      auto started = std::chrono::steady_clock::now();
      while (!g_stop) {
        server.service(5);
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
