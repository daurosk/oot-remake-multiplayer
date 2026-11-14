// coop_server: the headless co-op relay server (docs/protocol.md).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
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
#include "host_service.hpp"
#include "join_helper.hpp"
#include "nat_codes.hpp"
#include "nat_net.hpp"
#include "options.hpp"
#include "relay.hpp"
#include "punch.hpp"
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

// The STUN servers from the command line, or the free public ones.
std::vector<coop::server::nat::Addr> stun_servers(const coop::server::CommandLine& line) {
  namespace nat = coop::server::nat;
  std::vector<nat::Addr> out;
  const auto& names = line.stun.empty() ? nat::default_stun_servers() : line.stun;
  for (const std::string& name : names) {
    if (auto a = nat::resolve(name, 3478)) {
      out.push_back(*a);
    } else {
      say("stun: cannot find " + name + " (no internet, or the name lookup failed)");
    }
  }
  return out;
}

// Lines typed into the server window (answer codes), read on their own
// thread so the game loop never waits for the keyboard. Never freed: the
// thread may still be waiting for a line when the program ends.
class TypedLines {
 public:
  static TypedLines& get() {
    static TypedLines* lines = new TypedLines;
    return *lines;
  }
  void start() {
    if (started_) return;
    started_ = true;
    std::thread([this] {
      std::string text;
      while (std::getline(std::cin, text)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() < 16) queue_.push_back(text);
      }
    }).detach();
  }
  std::vector<std::string> take() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out(queue_.begin(), queue_.end());
    queue_.clear();
    return out;
  }

 private:
  bool started_ = false;
  std::mutex mutex_;
  std::deque<std::string> queue_;
};

// coop_server --join: Join.bat's helper for an invite code (join_helper.hpp).
int run_join(const coop::server::CommandLine& line) {
  namespace nat = coop::server::nat;
  const auto host = nat::resolve(line.join, 27510);
  if (!host) {
    std::fprintf(stderr, "coop_server: --join: '%s' is not an address\n", line.join.c_str());
    return 2;
  }
  nat::JoinConfig config;
  config.host = *host;
  config.password = line.password;
  config.stun_servers = stun_servers(line);
  config.game_port = line.game_port;
  config.log = [](const std::string& text) { say("join: " + text); };
  nat::JoinHelper helper(config);
  std::string why;
  if (!helper.start(why)) {
    say("join: " + why);
    return 1;
  }
  say("join: the game connects to this helper on 127.0.0.1:" + std::to_string(helper.game_port()) +
      "; keep this window open while you play");
  const auto started = std::chrono::steady_clock::now();
  while (!g_stop) {
    helper.service(5);
    if (line.run_for_ms &&
        std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(*line.run_for_ms)) {
      break;
    }
  }
  const auto& s = helper.stats();
  say("join: stopping; " + std::to_string(s.to_host) + " datagrams to the host, " + std::to_string(s.to_game) +
      " to the game");
  helper.stop();
  return 0;
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
  // Every session has a join password: it lets players in and makes the
  // traffic keys, so nothing goes plain (docs/protocol.md "Encrypted
  // traffic"). This also covers --upnp (issue 165), the invite code and join
  // helper (they carry the password, punches are signed with it) and
  // --relay-via (every relayed guest is 127.0.0.1, so --allow cannot tell
  // them apart). Only the relay itself (--relay) needs none: it forwards
  // sealed bytes it cannot read.
  if (!line->relay && line->password.empty()) {
    std::fprintf(stderr,
                 "coop_server: a join password is needed: every session has one, and it also "
                 "encrypts the traffic. Give one with --password TEXT or --password-env NAME. "
                 "With the kit, Host.bat / Host.sh make a new random one when server_password in "
                 "coop-options.ini is empty or missing. The server was not started.\n");
    return 2;
  }
#ifdef _WIN32
  SetConsoleCtrlHandler(on_console_event, TRUE);
#endif

  if (enet_initialize() != 0) {
    std::fprintf(stderr, "coop_server: ENet failed to start\n");
    return 1;
  }
  if (!line->join.empty()) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const int code = run_join(*line);
    g_done = true;
    enet_deinitialize();
    return code;
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
    HostServiceConfig config;
    config.port = line->port;
    config.bind = line->bind;
    config.session.game_build_id = line->game_build_id;
    config.session.max_half_open_per_address = line->max_half_open;
    config.session.max_players = static_cast<std::uint8_t>(line->max_players);
    config.session.pose_upload_bytes_per_second = line->pose_upload_kbps * 125u;  // kilobits to bytes
    config.max_connections = line->max_players + 4u;  // a full server can still say so
    config.session.allowed = line->allowed;
    // Guests through a relay come from this PC's own link (relay_link.hpp);
    // the relay let them in by the join code.
    if (!line->relay_via.empty() && !config.session.allowed.empty()) {
      config.session.allowed.push_back(AddressRange{0x7F000001u, 32});
    }
    config.session.join_password = line->password;
    config.session.pvp = line->pvp;
    config.session.nearest_owner = line->owner_nearest;
    if (line->owner_nearest) config.session.owner_dead_us = 1'000'000;  // off a dead player at once
    if (line->owner_hold_ms) config.session.owner_hold_us = std::uint64_t{*line->owner_hold_ms} * 1000;
    config.upnp = line->upnp;
    config.invite = line->invite;
    if (line->invite) config.stun_servers = stun_servers(*line);
    config.relay_via = line->relay_via;
    config.relay_key = line->relay_key;
    config.log = say;
    HostService host(config);
    std::string why;
    // Signals before start: the router forward and the relay are asked for
    // inside it, and a Ctrl+C then still ends the loop cleanly.
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    if (!host.start(why)) {
      std::fprintf(stderr, "coop_server: cannot listen on port %u\n", static_cast<unsigned>(line->port));
      exit_code = 1;
    } else {
      if (line->invite) TypedLines::get().start();
      auto started = std::chrono::steady_clock::now();
      while (!g_stop) {
        host.service(5);
        if (line->invite) {
          for (const std::string& typed : TypedLines::get().take()) {
            if (typed.find_first_not_of(" \t\r") == std::string::npos) continue;
            host.add_answer(typed);
          }
        }
        if (line->run_for_ms &&
            std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(*line->run_for_ms)) {
          break;
        }
      }
      host.stop();
    }
  }
  g_done = true;
  enet_deinitialize();
  return exit_code;
}
