// coop_server: the headless co-op relay server (docs/protocol.md).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <optional>
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
#else
#include <sys/stat.h>
#endif

#include "coop/protocol/join_password.hpp"
#include "coop/protocol/official_mark.hpp"
#include "coop/protocol/messages.hpp"
#include "console_out.hpp"
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

// #239: the service loop never waits for the console (console_out.hpp): a selection in the server's window
// (QuickEdit, Edit > Mark), the Pause key or a paused pipe used to stop the whole server.
coop::server::ConsoleOut& console() {
  static coop::server::ConsoleOut out([](const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
  });
  return out;
}

void say(const std::string& text) { console().push("coop_server: " + text); }

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

// The official game's key file (--official-key-file): 64 hex digits. Never printed. On POSIX a file group or others
// can read is refused (chmod 600), as ssh does with its keys.
std::optional<coop::protocol::OfficialSeed> read_official_key(const std::string& path, std::string& why) {
#ifndef _WIN32
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    why = "cannot read " + path;
    return std::nullopt;
  }
  if ((st.st_mode & 077) != 0) {
    why = path + " can be read by other users: chmod 600 it";
    return std::nullopt;
  }
#endif
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    why = "cannot read " + path;
    return std::nullopt;
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto seed = coop::protocol::official_seed_from_text(text);
  std::fill(text.begin(), text.end(), '\0');
  if (!seed) why = path + " is not a key file (64 hex digits)";
  return seed;
}

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
  // A public game's password is its room's (HostServiceConfig::public_game).
  if (line->public_game) line->password = coop::protocol::public_join_password(line->relay_room);
  if (!line->relay && line->password.empty()) {
    std::fprintf(stderr,
                 "coop_server: a join password is needed: every session has one, and it also "
                 "encrypts the traffic. Give one with --password TEXT or --password-env NAME. "
                 "With the kit, Host.bat / Host.sh make a new random one when server_password in "
                 "coop-options.ini is empty or missing. The server was not started.\n");
    return 2;
  }
  // A bad connection made on purpose: tests only, never a player's server.
  if (line->test_delay.on()) {
    if (environment("OOT_COOP_TEST") != "1") {
      std::fprintf(stderr, "coop_server: --test-delay-ms / --test-jitter-ms are for tests only (set "
                           "OOT_COOP_TEST=1). The server was not started.\n");
      return 2;
    }
    std::printf("coop_server: TEST DELAY on: %u ms round trip (half each way), jitter +-%u ms each way\n",
                static_cast<unsigned>(line->test_delay.round_trip_ms),
                static_cast<unsigned>(line->test_delay.jitter_ms));
    std::fflush(stdout);
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
    config.session.dbno.on = line->dbno;
    config.session.approve_joins = line->approve_joins;
    config.session.dedicated = line->dedicated;
    config.session.same_network_open = line->same_network_open;
    if (line->owner_hold_ms) config.session.owner_hold_us = std::uint64_t{*line->owner_hold_ms} * 1000;
    // Boss turns (SessionConfig::boss_turn_us, off by default): seconds per turn, for a proof run only.
    if (const std::string turn = environment("COOP_BOSS_TURN_S"); !turn.empty()) {
      const unsigned long s = std::strtoul(turn.c_str(), nullptr, 10);
      if (s > 0 && s <= 600) {
        config.session.boss_turn_us = std::uint64_t{s} * 1'000'000;
        say("boss turns on: " + std::to_string(s) + " s a turn (COOP_BOSS_TURN_S)");
      }
    }
    config.upnp = line->upnp;
    config.invite = line->invite;
    if (line->invite) config.stun_servers = stun_servers(*line);
    config.test_delay = line->test_delay;
    config.relay_via = line->relay_via;
    config.relay_key = line->relay_key;
    config.relay_room = line->relay_room;
    config.relay_list = line->relay_list;
    config.public_game = line->public_game;
    if (!line->official_key_file.empty()) {
      std::string why;
      config.official_key = read_official_key(line->official_key_file, why);
      if (!config.official_key) {
        std::fprintf(stderr, "coop_server: --official-key-file: %s\n", why.c_str());
        return 2;
      }
      std::string relay_host;
      std::uint16_t relay_port = 0;
      if (!coop::server::relay::parse_relay_address(line->official_relay, relay_host, relay_port)) {
        std::fprintf(stderr, "coop_server: --official-relay: '%s' is not a relay address\n", line->official_relay.c_str());
        return 2;
      }
      config.official_relay = coop::protocol::official_relay_name(relay_host, relay_port);
      say("official key: proofs name the relay " + config.official_relay);
      const auto pub = coop::protocol::official_public_key(*config.official_key);
      say(pub == coop::protocol::kOfficialPublicKey
              ? "official key: the listing is signed (kits show the official public game)"
              : "official key: NOT the kits' built-in key: the listing is signed, but no kit shows it as official");
    }
    std::vector<std::string> bans = read_ban_file(line->bans_file);
    config.session.kept_bans = bans;
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
      if (line->invite || line->dedicated) TypedLines::get().start();
      if (line->dedicated) say("dedicated: type /players, /kick N, /ban N, /unban KEYID, /mute N voice|chat");
      auto started = std::chrono::steady_clock::now();
      while (!g_stop) {
        host.service(5);
        if (line->invite) {
          for (const std::string& typed : TypedLines::get().take()) {
            if (typed.find_first_not_of(" \t\r") == std::string::npos) continue;
            host.add_answer(typed);
          }
        }
        if (line->dedicated) {
          for (const std::string& typed : TypedLines::get().take()) {
            const auto at = typed.find_first_not_of(" \t\r");
            if (at == std::string::npos || typed[at] != '/') continue;
            say(host.admin(typed.substr(at)));
          }
        }
        if (!line->bans_file.empty() && host.session() != nullptr) {   // (a ban made or lifted: kept for next time)
          std::vector<std::string> now = host.session()->key_bans();
          if (now != bans) {
            write_ban_file(line->bans_file, now);
            bans = std::move(now);
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
