// coop_server: the headless co-op relay server (docs/protocol.md).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include <enet/enet.h>

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
      std::printf("coop_server: listening on port %u\n",
                  static_cast<unsigned>(server.port()));
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
      std::printf("coop_server: stopping; relayed %llu, dropped %llu\n",
                  static_cast<unsigned long long>(stats.relayed),
                  static_cast<unsigned long long>(stats.dropped()));
    }
  }
  enet_deinitialize();
  return exit_code;
}
