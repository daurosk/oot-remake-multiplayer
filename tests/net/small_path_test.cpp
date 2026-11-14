// Paths that cannot carry our full datagrams (issue 143): the real server, the
// real client, and a relay between them that loses every datagram above a
// size, both ways, as a small-MTU tunnel does.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "coop/net/client.hpp"
#include "coop/net/enet_path.hpp"
#include "coop/protocol/messages.hpp"
#include "enet_server.hpp"

namespace {

namespace proto = coop::protocol;
using coop::net::Client;
using coop::net::ClientConfig;
using coop::net::DropReason;
using coop::net::EventKind;
using coop::net::State;

struct EnetLife {
  EnetLife() { REQUIRE(enet_initialize() == 0); }
  ~EnetLife() { enet_deinitialize(); }
};

// The test's own clock, stepped one millisecond a round, so a test starved of
// CPU (ctest -j8 during a build, sanitizers) sees the same losses, outages and
// timeouts at the same simulated times as an idle one. The server and the
// clients read it through their now_us option. ENet's clock is one for the
// whole process; enet_time_set before every service puts it on ours (it only
// runs on by the real time inside that one call).
struct SimClock {
  std::uint64_t ms = 100'000;  // ENet reads a time of 0 as "not set"
  void step() { ++ms; }
  void enet() const { enet_time_set(static_cast<enet_uint32>(ms)); }
  std::function<std::uint64_t()> us() const {
    return [this] { return ms * 1000u; };
  }
  int since(std::uint64_t t) const { return static_cast<int>(ms - t); }
};

coop::server::ServerOptions server_options(const SimClock& clock) {
  coop::server::ServerOptions options;
  options.port = 0;
  options.session.join_password = "test-pass";
  options.now_us = clock.us();
  return options;
}

// Client <-> relay <-> server on 127.0.0.1; datagrams above `max_payload`
// bytes of UDP payload are lost either way.
class LimitedPath {
 public:
  LimitedPath(std::uint16_t server_port, std::size_t max_payload) : max_payload_(max_payload) {
    outer_ = open();
    inner_ = open();
    ENetAddress got{};
    REQUIRE(enet_socket_get_address(outer_, &got) == 0);
    port_ = got.port;
    REQUIRE(enet_address_set_host_ip(&server_, "127.0.0.1") == 0);
    server_.port = server_port;
  }
  ~LimitedPath() {
    enet_socket_destroy(outer_);
    enet_socket_destroy(inner_);
  }
  LimitedPath(const LimitedPath&) = delete;
  LimitedPath& operator=(const LimitedPath&) = delete;

  std::uint16_t port() const { return port_; }
  void service() {
    std::uint8_t data[65536];
    ENetBuffer buffer{};
    for (;;) {  // client to server
      ENetAddress from{};
      buffer.data = data;
      buffer.dataLength = sizeof data;
      const int n = enet_socket_receive(outer_, &from, &buffer, 1);
      if (n <= 0) break;
      client_ = from;
      have_client_ = true;
      pass(inner_, server_, data, static_cast<std::size_t>(n));
    }
    for (;;) {  // server to client
      ENetAddress from{};
      buffer.data = data;
      buffer.dataLength = sizeof data;
      const int n = enet_socket_receive(inner_, &from, &buffer, 1);
      if (n <= 0) break;
      if (have_client_) pass(outer_, client_, data, static_cast<std::size_t>(n));
    }
  }

  int dropped = 0;
  std::size_t largest = 0;
  // A line that also loses packets: datagrams (either way, by size) for
  // which this says true are lost.
  std::function<bool(std::size_t)> lose;
  int lost = 0;

 private:
  static ENetSocket open() {
    const ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    REQUIRE(s != ENET_SOCKET_NULL);
    ENetAddress any{};
    REQUIRE(enet_address_set_host_ip(&any, "127.0.0.1") == 0);
    any.port = 0;
    REQUIRE(enet_socket_bind(s, &any) == 0);
    REQUIRE(enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1) == 0);
    return s;
  }
  void pass(ENetSocket out, const ENetAddress& to, std::uint8_t* data, std::size_t n) {
    largest = (std::max)(largest, n);
    if (n > max_payload_) {
      ++dropped;
      return;
    }
    if (lose && lose(n)) {
      ++lost;
      return;
    }
    ENetBuffer b{};
    b.data = data;
    b.dataLength = n;
    enet_socket_send(out, &to, &b, 1);
  }

  std::size_t max_payload_;
  ENetSocket outer_ = ENET_SOCKET_NULL;
  ENetSocket inner_ = ENET_SOCKET_NULL;
  ENetAddress server_{};
  ENetAddress client_{};
  bool have_client_ = false;
  std::uint16_t port_ = 0;
};

ClientConfig config_to(std::uint16_t port, const std::string& name, const SimClock& clock) {
  ClientConfig c;
  c.host = "127.0.0.1";
  c.port = port;
  c.hello.build_id = "small path test";
  c.hello.game_build_id = "e0f3ccfaf50065ae";
  c.hello.game_version = "1.1.1";
  c.hello.nickname = name;
  c.join_password = "test-pass";  // every session has one (and is encrypted)
  c.retry_first_ms = 50;
  c.retry_max_ms = 400;
  c.now_us = clock.us();
  return c;
}

std::vector<proto::FlagEntry> flags_from(std::uint32_t first, std::uint32_t count,
                                         std::int32_t value) {
  std::vector<proto::FlagEntry> out;
  for (std::uint32_t i = 0; i < count; ++i) {
    out.push_back({first + i, 0, proto::FlagKind::int32, static_cast<std::uint32_t>(value)});
  }
  return out;
}

// One joiner behind a path, and what happened to it.
struct Joiner {
  Joiner(std::uint16_t server_port, std::size_t max_payload, const std::string& name,
         const SimClock& clock)
      : path(server_port, max_payload), client(config_to(path.port(), name, clock)) {}
  LimitedPath path;
  Client client;
  int small_path_drops = 0;
  int other_drops = 0;
  int dropped_at_switch = -1;  // path.dropped when the small-path drop came
};

}  // namespace

TEST_CASE("small path: games behind 1200-, 576- and 900-byte paths stay connected for 2 minutes (issue 143)") {
  EnetLife enet;
  SimClock clock;
  clock.enet();
  coop::server::EnetServer server(server_options(clock));
  REQUIRE(server.start());

  Client host(config_to(server.port(), "Link", clock));
  REQUIRE(host.start());
  // Two joiners: datagrams above 1200 bytes lost (our 1100 fit), and above
  // 576 (they do not: one switch to ENet's smallest, then no more drops).
  std::vector<std::unique_ptr<Joiner>> joiners;
  joiners.push_back(std::make_unique<Joiner>(server.port(), 1200, "Zelda", clock));
  joiners.push_back(std::make_unique<Joiner>(server.port(), 576, "Saria", clock));
  // And 900: the last, shorter pieces of messages get through, the full
  // 1100-byte ones do not; it must switch once all the same.
  joiners.push_back(std::make_unique<Joiner>(server.port(), 900, "Ruto", clock));

  auto round = [&] {
    clock.step();
    clock.enet();
    server.service(0);
    clock.enet();
    host.service(0);
    while (host.next_event()) {
    }
    for (auto& j : joiners) {
      j->path.service();
      clock.enet();
      j->client.service(0);
      j->path.service();
      while (auto e = j->client.next_event()) {
        if (e->kind != EventKind::disconnected) continue;
        if (e->drop_reason == DropReason::small_path) {
          ++j->small_path_drops;
          j->dropped_at_switch = j->path.dropped;
        } else {
          ++j->other_drops;
        }
      }
    }
  };
  auto run_until = [&](int ms, auto done) {
    const auto start = clock.ms;
    while (clock.since(start) < ms) {
      round();
      if (done()) return true;
    }
    return false;
  };

  // The host comes in and gives the server a big table (about 22 KB).
  REQUIRE(run_until(3000, [&] { return host.is_host(); }));
  REQUIRE(host.send_flag_snapshot(flags_from(1, 2000, 7)));
  run_until(300, [] { return false; });

  // The joiners come in; the snapshot to them is many full datagrams.
  // The 1200 one first: all of them are 127.0.0.1 here, and the server
  // remembers that address once the 576 one is sent away.
  REQUIRE(joiners[0]->client.start());
  REQUIRE(run_until(5000, [&] { return joiners[0]->client.synced(); }));
  REQUIRE(joiners[1]->client.start());
  REQUIRE(joiners[2]->client.start());
  REQUIRE(run_until(15000, [&] {
    return std::all_of(joiners.begin(), joiners.end(), [](const auto& j) {
      return j->client.state() == State::connected && j->client.synced();
    });
  }));
  for (auto& j : joiners) {
    CHECK(j->other_drops == 0);
    CHECK(j->client.find_flag(2000) == proto::FlagEntry{2000, 0, proto::FlagKind::int32, 7u});
  }
  // The 1200 path carried everything at our size; the 576 one did not and
  // the game switched once.
  CHECK(joiners[0]->small_path_drops == 0);
  CHECK(joiners[0]->client.link().mtu == proto::kDatagramBytes);
  CHECK(joiners[0]->path.dropped == 0);
  std::vector<int> dropped_small;
  for (std::size_t i = 1; i < joiners.size(); ++i) {
    CAPTURE(i);
    CHECK(joiners[i]->small_path_drops == 1);
    CHECK(joiners[i]->client.small_datagrams());
    CHECK(joiners[i]->client.link().mtu == coop::net::kSmallDatagramBytes);
    dropped_small.push_back(joiners[i]->path.dropped);
  }
  CHECK(server.small_path_drops() >= 1);

  // Two minutes of play: the host changes 400 flags every half second, each
  // joiner asks for 300 every 0.7 s (both several datagrams long).
  const auto start = clock.ms;
  auto next_host = start;
  auto next_join = start;
  std::int32_t value = 8;
  while (clock.since(start) < 120'000) {
    if (clock.ms >= next_host) {
      REQUIRE(host.set_flags(flags_from(1, 400, value)));
      ++value;
      next_host += 500;
    }
    if (clock.ms >= next_join) {
      for (std::size_t i = 0; i < joiners.size(); ++i) {
        joiners[i]->client.set_flags(flags_from(5000 + 1000 * static_cast<std::uint32_t>(i), 300, value));
      }
      next_join += 700;
    }
    round();
  }
  run_until(1000, [] { return false; });

  for (auto& j : joiners) {
    CAPTURE(j->client.link().mtu);
    CHECK(j->other_drops == 0);
    CHECK(j->client.state() == State::connected);
    // Still in step with the host at the end.
    CHECK(j->client.find_flag(400) == proto::FlagEntry{400, 0, proto::FlagKind::int32,
                                                       static_cast<std::uint32_t>(value - 1)});
  }
  CHECK(joiners[0]->small_path_drops == 0);
  CHECK(joiners[0]->path.dropped == 0);
  for (std::size_t i = 1; i < joiners.size(); ++i) {
    CAPTURE(i);
    CHECK(joiners[i]->small_path_drops == 1);
    CHECK(joiners[i]->path.dropped == dropped_small[i - 1]);  // nothing lost after the switch
  }
  CHECK(host.state() == State::connected);
}

TEST_CASE("small path: the server tells a remembered address again when it connects with full datagrams") {
  EnetLife enet;
  SimClock clock;
  clock.enet();
  coop::server::EnetServer server(server_options(clock));
  server.remember_loopback_small_paths(true);  // every game here is on 127.0.0.1
  REQUIRE(server.start());

  Client host(config_to(server.port(), "Link", clock));
  REQUIRE(host.start());
  auto pump = [&](Client& c, LimitedPath* path, int ms, auto done) {
    const auto start = clock.ms;
    while (clock.since(start) < ms) {
      clock.step();
      clock.enet();
      server.service(0);
      clock.enet();
      host.service(0);
      if (path != nullptr) path->service();
      clock.enet();
      if (&c != &host) c.service(0);
      if (path != nullptr) path->service();
      if (done()) return true;
    }
    return false;
  };
  REQUIRE(pump(host, nullptr, 3000, [&] { return host.is_host(); }));
  REQUIRE(host.send_flag_snapshot(flags_from(1, 2000, 7)));

  // A first game behind a 576 path: sent away once, comes back small.
  {
    LimitedPath path(server.port(), 576);
    Client joiner(config_to(path.port(), "Saria", clock));
    REQUIRE(joiner.start());
    REQUIRE(pump(joiner, &path, 15000, [&] { return joiner.synced(); }));
    CHECK(joiner.small_datagrams());
    joiner.stop();
    pump(joiner, &path, 200, [] { return false; });
  }
  REQUIRE(server.small_path_drops() == 1);
  ENetAddress local{};
  REQUIRE(enet_address_set_host_ip(&local, "127.0.0.1") == 0);
  CHECK(server.small_path(local.host));

  // The same address with a new game (it missed the note, or restarted):
  // told at once, before anything big is sent, and then in with 576.
  LimitedPath path(server.port(), 576);
  Client again(config_to(path.port(), "Saria", clock));
  REQUIRE(again.start());
  REQUIRE(pump(again, &path, 15000, [&] { return again.synced(); }));
  CHECK(again.small_datagrams());
  CHECK(again.link().mtu == coop::net::kSmallDatagramBytes);
  CHECK(server.small_path_drops() == 2);
  // Told once more, then forgotten: the next game from there (another
  // player behind the same router) comes in at full size.
  CHECK_FALSE(server.small_path(local.host));
  Client other(config_to(server.port(), "Malon", clock));
  REQUIRE(other.start());
  REQUIRE(pump(other, nullptr, 5000, [&] { return other.synced(); }));
  CHECK_FALSE(other.small_datagrams());
  CHECK(other.link().mtu == proto::kDatagramBytes);
  CHECK(server.small_path_drops() == 2);
}

TEST_CASE("small path: a loopback game sent away is not remembered (relay guests and the host share 127.0.0.1)") {
  EnetLife enet;
  SimClock clock;
  clock.enet();
  coop::server::EnetServer server(server_options(clock));
  REQUIRE(server.start());

  Client host(config_to(server.port(), "Link", clock));
  REQUIRE(host.start());
  auto pump = [&](Client& c, LimitedPath* path, int ms, auto done) {
    const auto start = clock.ms;
    while (clock.since(start) < ms) {
      clock.step();
      clock.enet();
      server.service(0);
      clock.enet();
      host.service(0);
      if (path != nullptr) path->service();
      clock.enet();
      if (&c != &host) c.service(0);
      if (path != nullptr) path->service();
      if (done()) return true;
    }
    return false;
  };
  REQUIRE(pump(host, nullptr, 3000, [&] { return host.is_host(); }));
  REQUIRE(host.send_flag_snapshot(flags_from(1, 2000, 7)));

  // A first game behind a 576 path: sent away once, comes back small.
  {
    LimitedPath path(server.port(), 576);
    Client joiner(config_to(path.port(), "Saria", clock));
    REQUIRE(joiner.start());
    REQUIRE(pump(joiner, &path, 15000, [&] { return joiner.synced(); }));
    CHECK(joiner.small_datagrams());
    joiner.stop();
    pump(joiner, &path, 200, [] { return false; });
  }
  REQUIRE(server.small_path_drops() == 1);
  ENetAddress local{};
  REQUIRE(enet_address_set_host_ip(&local, "127.0.0.1") == 0);
  CHECK_FALSE(server.small_path(local.host));
  // The next game from 127.0.0.1 (the host's own, or another relay guest)
  // comes in at full size, not sent away.
  Client other(config_to(server.port(), "Malon", clock));
  REQUIRE(other.start());
  REQUIRE(pump(other, nullptr, 5000, [&] { return other.synced(); }));
  CHECK_FALSE(other.small_datagrams());
  CHECK(server.small_path_drops() == 1);
}

TEST_CASE("small path: plain loss and short outages on a line that carries full datagrams do not switch it") {
  EnetLife enet;
  SimClock clock;
  clock.enet();
  coop::server::EnetServer server(server_options(clock));
  REQUIRE(server.start());

  Client host(config_to(server.port(), "Link", clock));
  REQUIRE(host.start());
  Joiner joiner(server.port(), 1400, "Zelda", clock);
  // 5% of datagrams lost either way, and every 4 s nothing at all for 0.75 s
  // (a Wi-Fi stall), while big FLAG_STATEs and FLAG_SETs go both ways.
  std::mt19937 random(143);
  std::bernoulli_distribution loss(0.05);
  const auto begin = clock.ms;
  bool lossy = true;  // from the joiner's first packet on
  joiner.path.lose = [&](std::size_t) {
    if (!lossy) return false;
    const int t = clock.since(begin) % 4000;
    return t >= 3250 || loss(random);
  };

  auto round = [&] {
    clock.step();
    clock.enet();
    server.service(0);
    clock.enet();
    host.service(0);
    while (host.next_event()) {
    }
    joiner.path.service();
    clock.enet();
    joiner.client.service(0);
    joiner.path.service();
    while (auto e = joiner.client.next_event()) {
      if (e->kind != EventKind::disconnected) continue;
      if (e->drop_reason == DropReason::small_path) {
        ++joiner.small_path_drops;
      } else {
        ++joiner.other_drops;
      }
    }
  };
  auto run_until = [&](int ms, auto done) {
    const auto start = clock.ms;
    while (clock.since(start) < ms) {
      round();
      if (done()) return true;
    }
    return false;
  };
  REQUIRE(run_until(3000, [&] { return host.is_host(); }));
  REQUIRE(host.send_flag_snapshot(flags_from(1, 2000, 7)));
  run_until(300, [] { return false; });
  REQUIRE(joiner.client.start());
  REQUIRE(run_until(15000, [&] { return joiner.client.synced(); }));
  CHECK(joiner.small_path_drops == 0);

  const auto start = clock.ms;
  auto next_host = start;
  auto next_join = start;
  std::int32_t value = 8;
  while (clock.since(start) < 40'000) {
    if (clock.ms >= next_host) {
      REQUIRE(host.set_flags(flags_from(1, 400, value)));
      ++value;
      next_host += 500;
    }
    if (clock.ms >= next_join) {
      joiner.client.set_flags(flags_from(5000, 300, value));
      next_join += 700;
    }
    round();
  }
  lossy = false;
  run_until(3000, [] { return false; });

  CHECK(joiner.path.lost > 100);  // the line really lost packets
  CHECK(joiner.small_path_drops == 0);
  CHECK(server.small_path_drops() == 0);
  CHECK_FALSE(joiner.client.small_datagrams());
  CHECK(joiner.client.link().mtu == proto::kDatagramBytes);
  CHECK(joiner.other_drops == 0);
  CHECK(joiner.client.state() == State::connected);
  CHECK(joiner.client.find_flag(400) == proto::FlagEntry{400, 0, proto::FlagKind::int32,
                                                         static_cast<std::uint32_t>(value - 1)});
}
