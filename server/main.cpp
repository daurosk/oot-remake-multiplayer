// coop_server: the headless co-op relay server (docs/protocol.md).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <enet/enet.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "coop/protocol/messages.hpp"
#include "enet_server.hpp"
#include "options.hpp"
#include "relay.hpp"
#include "relay_link.hpp"
#include "upnp.hpp"

namespace {

std::atomic<bool> g_stop{false};
// Set when the server has cleaned up (router forward removed, relay told).
std::atomic<bool> g_done{false};

void on_signal(int) { g_stop = true; }

#ifdef _WIN32
// A closed window ends the program a few seconds after this returns; wait
// for the main loop to remove the router's forward and tell the relay first.
BOOL WINAPI on_console_event(DWORD event) {
  if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
    g_stop = true;
    for (int i = 0; i < 40 && !g_done; ++i) Sleep(100);
    return TRUE;
  }
  return FALSE;  // Ctrl+C, Ctrl+Break: the signal handler
}
#endif

void say(const std::string& text) {
  std::printf("coop_server: %s\n", text.c_str());
  std::fflush(stdout);
}

// coop_server --relay: forwards hosts' games to their guests (relay.hpp).
int run_relay(const coop::server::CommandLine& line, const std::string& key) {
  using namespace coop::server::relay;
  RelayConfig config;
  config.host_key = key;
  config.log = [](const std::string& text) { say("relay: " + text); };
  RelayServer relay(line.port, line.bind, config);
  if (!relay.start()) {
    std::fprintf(stderr, "coop_server: cannot listen on UDP port %u for the relay\n",
                 static_cast<unsigned>(line.port));
    return 1;
  }
  say("relay on UDP port " + std::to_string(relay.port()) +
      ": hosts start with --relay-via <this PC's internet address>:" + std::to_string(relay.port()) +
      " and give their friends the join code they get" +
      (key.empty() ? std::string("; any host may use it (set --relay-key to limit that)")
                   : std::string("; hosts must give the relay key")));
  const auto started = std::chrono::steady_clock::now();
  auto last_note = started;
  RelayStats shown{};
  while (!g_stop) {
    relay.service(50);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_note >= std::chrono::minutes(1)) {
      last_note = now;
      const RelayStats& s = relay.relay().stats();
      if (s.to_host != shown.to_host || s.to_guest != shown.to_guest) {
        say("relay: " + std::to_string(relay.relay().sessions()) + " host(s), " +
            std::to_string(relay.relay().flows()) + " guest(s); forwarded " + std::to_string(s.to_host) +
            " to hosts, " + std::to_string(s.to_guest) + " to guests; turned away " +
            std::to_string(s.not_let_in) + " without a code");
        shown = s;
      }
    }
    if (line.run_for_ms && now - started >= std::chrono::milliseconds(*line.run_for_ms)) break;
  }
  const RelayStats& s = relay.relay().stats();
  say("relay stopping; forwarded " + std::to_string(s.to_host) + " to hosts, " + std::to_string(s.to_guest) +
      " to guests; codes right " + std::to_string(s.lookups_ok) + ", wrong " + std::to_string(s.lookups_wrong) +
      ", over the limit " + std::to_string(s.lookups_limited));
  return 0;
}

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
  if (!line->relay_key_env.empty()) {
    line->relay_key = environment(line->relay_key_env);
    if (line->relay_key.size() > coop::server::relay::kMaxKeyBytes) {
      std::fprintf(stderr, "coop_server: the relay key in %s is longer than 64 characters\n",
                   line->relay_key_env.c_str());
      return 2;
    }
  }
  // A forward opens this PC to the whole internet while the server runs:
  // only with something that says who may join (issue 165).
  if (line->upnp && line->password.empty() && line->allowed.empty()) {
    std::fprintf(stderr,
                 "coop_server: --upnp (upnp = yes) opens this PC's port to the whole internet, so it "
                 "needs a join password (server_password) or --allow (server_allow) as well. The server "
                 "was not started.\n");
    return 2;
  }
  // The relay lets guests in by their internet address once one of them sent
  // the code, so others on that address (CGNAT, dorm, hotspot) get in without
  // it, and every relayed guest is 127.0.0.1 here, so --allow cannot tell them
  // apart. The join code only finds the host; the password is the lock.
  if (!line->relay_via.empty() && line->password.empty()) {
    std::fprintf(stderr,
                 "coop_server: --relay-via (relay = ...) needs a join password (server_password): the "
                 "join code only finds your server, the password keeps strangers out. server_allow is "
                 "not enough, because every guest through the relay comes from this PC (127.0.0.1). "
                 "The server was not started.\n");
    return 2;
  }
#ifdef _WIN32
  SetConsoleCtrlHandler(on_console_event, TRUE);
#endif

  if (enet_initialize() != 0) {
    std::fprintf(stderr, "coop_server: ENet failed to start\n");
    return 1;
  }
  if (line->relay) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const int code = run_relay(*line, line->relay_key);
    g_done = true;
    enet_deinitialize();
    return code;
  }

  int exit_code = 0;
  {
    ServerOptions options;
    options.port = line->port;
    options.session.game_build_id = line->game_build_id;
    options.session.max_half_open_per_address = line->max_half_open;
    options.session.allowed = line->allowed;
    // Guests through a relay come from this PC's own link (relay_link.hpp);
    // the relay let them in by the join code.
    if (!line->relay_via.empty() && !options.session.allowed.empty()) {
      options.session.allowed.push_back(AddressRange{0x7F000001u, 32});
    }
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

      // Router forward (upnp.hpp) and relay (relay_link.hpp), when asked for.
      std::unique_ptr<coop::server::upnp::Mapping> mapping;
      if (line->upnp) {
        coop::server::upnp::Config config;
        config.internal_port = server.port();
        config.external_port = server.port();
        config.log = say;
        mapping = std::make_unique<coop::server::upnp::Mapping>(config);
        if (!mapping->open()) mapping.reset();
      }
      std::unique_ptr<coop::server::relay::RelayLink> link;
      if (!line->relay_via.empty()) {
        coop::server::relay::RelayLinkConfig config;
        if (!coop::server::relay::parse_relay_address(line->relay_via, config.relay_host,
                                                      config.relay_port)) {
          say("relay: '" + line->relay_via + "' is not a relay address (name or IPv4, optional :port)");
        } else {
          config.server_port = server.port();
          config.key = line->relay_key;
          config.log = [](const std::string& text) { say("relay: " + text); };
          const std::string relay_text = line->relay_via;
          config.on_code = [relay_text](const std::string& code) {
            const std::string shown = coop::server::relay::show_code(code);
            say("relay: join code " + shown + ". Give your friends " + shown + "@" + relay_text +
                " (Join.bat takes it as the server address)");
          };
          link = std::make_unique<coop::server::relay::RelayLink>(config);
          std::string why;
          if (!link->start(why)) {
            say("relay: " + why);
            link.reset();
          } else {
            say("relay: asking " + line->relay_via + " for a join code");
          }
        }
      }

      auto started = std::chrono::steady_clock::now();
      std::uint64_t shown_resets = 0;
      std::uint64_t shown_limited = 0;
      while (!g_stop) {
        // Relayed datagrams wait for this round: keep it short with a relay.
        server.service(link ? 1 : 5);
        if (link) link->service();
        if (mapping) mapping->renew_if_due();
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
      if (link) link->stop();
      if (mapping) mapping->close();
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
  g_done = true;
  enet_deinitialize();
  return exit_code;
}
