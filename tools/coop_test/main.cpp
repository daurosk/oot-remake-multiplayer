// coop_test: the stage 0 test tool (docs/plan.md, stage 1 step 0, items 5
// and 6; docs/soak.md says how to run it). Runs the real server and test
// clients in one process, or only one of the two, and reports what the
// traffic did.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

#include "enet_server.hpp"
#include "options.hpp"
#include "probe.hpp"

namespace {

using namespace coop::test_tool;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kDrainMs = 1000;    // after the last send, for what is in flight
constexpr std::uint32_t kGoodbyeMs = 200;   // after stop(), for the goodbyes to go out

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

void print_server(const coop::server::EnetServer& server) {
  const coop::server::Stats& s = server.session().stats();
  std::printf("server: players %zu, relayed %llu, dropped %llu, rate limited %llu\n",
              server.session().player_count(), static_cast<unsigned long long>(s.relayed),
              static_cast<unsigned long long>(s.dropped()),
              static_cast<unsigned long long>(s.rate_limited));
}

void print_loop(const Spread& loop_ms) {
  std::printf("loop: pass every min %.3f avg %.3f max %.3f ms (%llu passes)\n", loop_ms.min,
              loop_ms.avg(), loop_ms.max, static_cast<unsigned long long>(loop_ms.count));
  std::fflush(stdout);
}

// Reliable messages a local sender sent to `receiver` that never came: the
// gaps inside what arrived are counted by the receiver itself; this adds the
// ones after the last that arrived. Only meaningful once the traffic drained.
std::uint64_t trailing_lost(const Probe& receiver,
                            const std::vector<std::unique_ptr<Probe>>& probes) {
  std::uint64_t lost = 0;
  for (const auto& sender : probes) {
    if (sender.get() == &receiver || sender->number() == 0) continue;
    auto sent = sender->last_reliable_sent();
    auto got = receiver.last_reliable_from(sender->number());
    if (sent && got && *sent > *got) lost += *sent - *got;
  }
  return lost;
}

void print_report(const char* title, Clock::time_point start,
                  const std::vector<std::unique_ptr<Probe>>& probes,
                  const coop::server::EnetServer* server, bool final_report) {
  std::printf("== coop_test %s report at %.1f s ==\n", title, seconds_since(start));
  for (const auto& probe : probes) {
    probe->print(stdout, final_report ? trailing_lost(*probe, probes) : 0);
  }
  if (server != nullptr) print_server(*server);
  std::fflush(stdout);
}

// The final verdict; prints why it failed.
bool passed(const std::vector<std::unique_ptr<Probe>>& probes) {
  bool ok = true;
  auto fail = [&ok](const std::string& why) {
    std::printf("fail: %s\n", why.c_str());
    ok = false;
  };
  for (const auto& probe : probes) {
    const std::string who = "client \"" + probe->config().nickname + "\"";
    if (probe->refused()) {
      fail(who + " was refused");
      continue;
    }
    if (probe->client().state() != coop::net::State::connected) {
      fail(who + " is not connected at the end");
    }
    if (probe->disconnects() != 0) fail(who + " was dropped and had to reconnect");
    const std::uint64_t lost = probe->reliable().lost + trailing_lost(*probe, probes);
    if (lost != 0) fail(who + " lost " + std::to_string(lost) + " reliable messages");
    for (const auto& other : probes) {
      if (other.get() == probe.get() || other->number() == 0) continue;
      const auto& heard = probe->heard_from();
      if (std::find(heard.begin(), heard.end(), other->number()) == heard.end()) {
        fail(who + " never heard from \"" + other->config().nickname + "\"");
      }
    }
  }
  return ok;
}

int run_server_only(const Options& options, coop::server::EnetServer& server) {
  const Clock::time_point start = Clock::now();
  Clock::time_point next_report = start + std::chrono::seconds(options.report_every);
  while (!g_stop) {
    server.service(1);
    const Clock::time_point now = Clock::now();
    if (options.seconds != 0 && now - start >= std::chrono::seconds(options.seconds)) break;
    if (options.report_every != 0 && now >= next_report) {
      next_report += std::chrono::seconds(options.report_every);
      std::printf("== coop_test server report at %.1f s ==\n", seconds_since(start));
      print_server(server);
      std::fflush(stdout);
    }
  }
  std::printf("== coop_test final report at %.1f s ==\n", seconds_since(start));
  print_server(server);
  return 0;
}

int run_clients(const Options& options, coop::server::EnetServer* server) {
  std::vector<std::unique_ptr<Probe>> probes;
  for (int i = 0; i < options.clients; ++i) {
    ProbeConfig config;
    config.host = options.connect_host ? *options.connect_host : "127.0.0.1";
    config.port = options.connect_host ? options.connect_port : server->port();
    config.nickname = options.name + std::to_string(i + 1);
    probes.push_back(std::make_unique<Probe>(config));
    if (!probes.back()->start()) {
      std::fprintf(stderr, "coop_test: client %d could not start ENet\n", i + 1);
      return 1;
    }
  }
  std::printf("coop_test: %d clients to %s:%u for %s, shown %llu ms behind\n", options.clients,
              probes.front()->config().host.c_str(),
              static_cast<unsigned>(probes.front()->config().port),
              options.seconds == 0 ? "until Ctrl+C"
                                   : (std::to_string(options.seconds) + " s").c_str(),
              static_cast<unsigned long long>(kShowDelayUs / 1000));
  std::fflush(stdout);

  Spread loop_ms;  // time from one pass over everything to the next
  auto step_all = [&](bool sending) {
    if (server != nullptr) server->service(0);
    for (auto& probe : probes) probe->step(sending);
  };

  const Clock::time_point start = Clock::now();
  Clock::time_point next_report = start + std::chrono::seconds(options.report_every);
  Clock::time_point last_pass = start;
  while (!g_stop) {
    step_all(true);
    const Clock::time_point now = Clock::now();
    loop_ms.add(std::chrono::duration<double, std::milli>(now - last_pass).count());
    last_pass = now;
    if (options.seconds != 0 && now - start >= std::chrono::seconds(options.seconds)) break;
    if (options.report_every != 0 && now >= next_report) {
      next_report += std::chrono::seconds(options.report_every);
      print_report("interim", start, probes, server, false);
      print_loop(loop_ms);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Let what is in flight arrive before counting it as lost.
  const Clock::time_point drain_end = Clock::now() + std::chrono::milliseconds(kDrainMs);
  while (Clock::now() < drain_end) {
    step_all(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  print_report("final", start, probes, server, true);
  print_loop(loop_ms);
  const bool ok = passed(probes);
  std::printf("result: %s\n", ok ? "PASS" : "FAIL");
  std::fflush(stdout);

  for (auto& probe : probes) probe->stop();
  const Clock::time_point goodbye_end = Clock::now() + std::chrono::milliseconds(kGoodbyeMs);
  while (server != nullptr && Clock::now() < goodbye_end) server->service(1);
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string error;
  auto options = parse_options(args, error);
  if (!options) {
    std::fprintf(stderr, "coop_test: %s\n%s", error.c_str(), usage());
    return 2;
  }
  if (options->help) {
    std::printf("%s", usage());
    return 0;
  }

  if (enet_initialize() != 0) {
    std::fprintf(stderr, "coop_test: ENet failed to start\n");
    return 1;
  }
#ifdef _WIN32
  // Sleeps of 1 ms instead of the default 15.6 ms, so 60 a second is 60.
  timeBeginPeriod(1);
#endif
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  int exit_code = 0;
  {
    std::unique_ptr<coop::server::EnetServer> server;
    if (!options->connect_host) {
      coop::server::ServerOptions server_options;
      server_options.port = options->port;
      server_options.session.log = [](const std::string& text) {
        std::printf("server: %s\n", text.c_str());
        std::fflush(stdout);
      };
      server = std::make_unique<coop::server::EnetServer>(server_options);
      if (!server->start()) {
        std::fprintf(stderr, "coop_test: cannot listen on port %u\n",
                     static_cast<unsigned>(options->port));
        exit_code = 1;
      } else {
        std::printf("server: listening on port %u\n", static_cast<unsigned>(server->port()));
        std::fflush(stdout);
      }
    }
    if (exit_code == 0) {
      exit_code = options->server_only ? run_server_only(*options, *server)
                                       : run_clients(*options, server.get());
    }
  }

#ifdef _WIN32
  timeEndPeriod(1);
#endif
  enet_deinitialize();
  return exit_code;
}
