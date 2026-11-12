// coop::host::Session end to end: the real server on 127.0.0.1, real
// clients, and a fake game (FakeMemory for the player, a FlagStore map for
// the flags) behind each session.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include "coop/host/session.hpp"
#include "enet_server.hpp"
#include "fake_memory.hpp"

using namespace coop::host;
namespace proto = coop::protocol;
namespace net = coop::net;

namespace {

constexpr FlagKey kChest{0x1000u, 0};
constexpr FlagKey kDoor{0x2000u, 0};
constexpr FlagKey kKey{0x3000u, 2};

proto::FlagEntry entry(FlagKey key, std::uint32_t value) {
  return {key, proto::FlagKind::boolean, value};
}

// The game's flags. Only the emulator thread touches it.
class FakeStore : public FlagStore {
 public:
  FakeStore() {
    for (FlagKey k : {kChest, kDoor, kKey}) set(k, 0);
  }
  std::map<FlagKey, proto::FlagEntry> values;
  void set(FlagKey key, std::uint32_t value) { values[key] = entry(key, value); }
  std::uint32_t get(FlagKey key) const { return values.at(key).value; }
  std::optional<proto::FlagEntry> read(FlagKey key) override {
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    return it->second;
  }
  bool write(const proto::FlagEntry& e) override {
    if (!values.contains(key_of(e))) return false;
    values[key_of(e)] = e;
    return true;
  }
};

struct EnetLife {
  EnetLife() { REQUIRE(enet_initialize() == 0); }
  ~EnetLife() { enet_deinitialize(); }
};

net::ClientConfig config_for(std::uint16_t port, const std::string& name,
                             std::uint32_t timeout_ms = 10000) {
  net::ClientConfig c;
  c.timeout_ms = timeout_ms;
  c.host = "127.0.0.1";
  c.port = port;
  c.hello.build_id = "coop-tests 0";
  c.hello.game_build_id = "e0f3ccfaf50065ae";
  c.hello.game_version = "1.1.1";
  c.hello.nickname = name;
  c.join_password = "test-pass";  // every session has one (and is encrypted)
  c.retry_first_ms = 50;
  c.retry_max_ms = 400;
  return c;
}

SessionSettings with_flags() {
  SessionSettings s;
  s.shared = SharedFlags({kChest, kDoor, kKey});
  return s;
}

// One player: a client, its session, and its game.
struct Peer {
  Peer(std::uint16_t port, const std::string& name, SessionSettings settings = with_flags(),
       std::uint32_t timeout_ms = 10000)
      : client(config_for(port, name, timeout_ms)), session(client, std::move(settings)) {
    test::make_game(memory);
    REQUIRE(client.start());
  }
  // One emulated frame, then one network round, on one thread.
  void step() {
    last = session.frame(memory, &store);
    session.service();
  }
  bool synced() {
    return session.with_client([](net::Client& c) { return c.synced(); });
  }
  std::uint8_t number() {
    return session.with_client([](net::Client& c) { return c.player_number(); });
  }

  net::Client client;
  Session session;
  test::FakeMemory memory;
  FakeStore store;
  FrameReport last;
};

coop::server::ServerOptions any_port() {
  coop::server::ServerOptions o;
  o.port = 0;
  o.session.join_password = "test-pass";
  return o;
}

bool pump(coop::server::EnetServer& server, const std::vector<Peer*>& peers,
          const std::function<bool()>& done, int timeout_ms = 3000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < end) {
    server.service(1);
    for (Peer* p : peers) p->step();
    if (done()) return true;
  }
  return false;
}

// Joins `p` and waits until it is welcomed and has the host's table.
void join(coop::server::EnetServer& server, std::vector<Peer*>& peers, Peer& p) {
  peers.push_back(&p);
  REQUIRE(pump(server, peers, [&] { return p.last.connected && p.synced(); }));
}

}  // namespace

TEST_CASE("session: two players share a flag through the host, both ways") {
  EnetLife enet;
  coop::server::EnetServer server(any_port());
  REQUIRE(server.start());
  std::vector<Peer*> peers;

  Peer a(server.port(), "Link");
  join(server, peers, a);
  REQUIRE(pump(server, peers, [&] { return a.last.hosting; }));
  // The first player hosts and sends its table, all three flags.
  REQUIRE(pump(server, peers, [&] { return server.session().flag_count() == 3; }));

  Peer b(server.port(), "Zelda");
  join(server, peers, b);
  CHECK_FALSE(b.last.hosting);

  // A player's change goes to the host, whose answer reaches both games.
  b.store.set(kChest, 1);
  REQUIRE(pump(server, peers, [&] {
    return a.store.get(kChest) == 1 && b.session.flags()->pending() == 0;
  }));
  // The host's change reaches the player.
  a.store.set(kDoor, 1);
  REQUIRE(pump(server, peers, [&] { return b.store.get(kDoor) == 1; }));

  // Nothing echoes: each side sent its one change and nothing more.
  pump(server, peers, [] { return false; }, 200);
  CHECK(a.session.flags()->stats().changes_sent == 1);
  CHECK(b.session.flags()->stats().changes_sent == 1);
  CHECK(a.store.get(kKey) == 0);
  CHECK(b.store.get(kKey) == 0);
  CHECK(server.session().stats().flag_clashes == 0);
  CHECK(server.session().stats().dropped() == 0);
}

TEST_CASE("session: a new host sends its table, and a later joiner gets it") {
  EnetLife enet;
  coop::server::EnetServer server(any_port());
  REQUIRE(server.start());
  std::vector<Peer*> peers;

  auto a = std::make_unique<Peer>(server.port(), "Link");
  join(server, peers, *a);
  REQUIRE(pump(server, peers, [&] { return a->last.hosting; }));
  Peer b(server.port(), "Zelda");
  join(server, peers, b);
  a->store.set(kDoor, 1);
  REQUIRE(pump(server, peers, [&] { return b.store.get(kDoor) == 1; }));

  // The host leaves: the player takes over and sends the whole table.
  const auto parts_before = server.session().stats().snapshot_parts;
  a->session.with_client([](net::Client& c) { c.stop(); });
  peers = {&b};
  a.reset();
  REQUIRE(pump(server, peers, [&] { return b.last.hosting; }));
  REQUIRE(pump(server, peers, [&] {
    return server.session().stats().snapshot_parts > parts_before;
  }));
  CHECK(server.session().stats().flag_clashes == 0);

  // The new host's changes go out as the truth and reach a new joiner.
  b.store.set(kChest, 1);
  pump(server, peers, [] { return false; }, 100);
  Peer c(server.port(), "Saria");
  join(server, peers, c);
  REQUIRE(pump(server, peers, [&] {
    return c.store.get(kDoor) == 1 && c.store.get(kChest) == 1;
  }));
  CHECK_FALSE(c.last.hosting);
  b.store.set(kKey, 1);
  REQUIRE(pump(server, peers, [&] { return c.store.get(kKey) == 1; }));
  CHECK(b.session.flags()->pending() == 0);
  CHECK(server.session().stats().dropped() == 0);
}

TEST_CASE("session: samples reach the other player's presence") {
  EnetLife enet;
  coop::server::EnetServer server(any_port());
  REQUIRE(server.start());
  std::vector<Peer*> peers;

  Peer a(server.port(), "Link", SessionSettings{});  // no flags shared
  join(server, peers, a);
  Peer b(server.port(), "Zelda", SessionSettings{});
  join(server, peers, b);
  CHECK(a.session.flags() == nullptr);
  const std::uint8_t na = a.number();

  // The newest buffered sample of `number` in `peer`'s presence, if any.
  auto newest = [](Peer& peer, std::uint8_t number) {
    return peer.session.with_presence([&](coop::presence::Presence& p) {
      std::optional<coop::presence::Sample> s;
      if (const auto* player = p.player(number); player && !player->samples().empty()) {
        s = player->samples().back();
      }
      return s;
    });
  };
  REQUIRE(pump(server, peers, [&] {
    const auto s = newest(b, na);
    return s && s->position == proto::Vec3{1.0f, 2.0f, 3.0f};
  }));
  CHECK(b.session.with_presence([&](coop::presence::Presence& p) {
    return p.player(na)->scene() == std::optional<std::string>("spot00");
  }));

  // A moves; B sees it.
  a.memory.put_vec3(test::Layout{}.game_ui + addr::kGameUiPosition, {5.0f, 2.0f, 3.0f});
  REQUIRE(pump(server, peers, [&] {
    const auto s = newest(b, na);
    return s && s->position[0] == 5.0f;
  }));
  CHECK(a.session.sampler().last_sample().has_value());
}

TEST_CASE("session: a frame thread and a network thread per player share a flag") {
  EnetLife enet;
  coop::server::EnetServer server(any_port());
  REQUIRE(server.start());
  Peer a(server.port(), "Link");
  Peer b(server.port(), "Zelda");

  std::atomic<bool> stop{false};
  std::thread server_thread([&] {
    while (!stop) server.service(1);
  });
  auto net_loop = [&](Peer& p) {
    return std::thread([&] {
      while (!stop) {
        p.session.service();
        p.session.wait(2);
      }
    });
  };
  std::thread net_a = net_loop(a);
  // B joins once A hosts, so A is player 1.
  auto frames = [&](const std::function<bool()>& done, bool with_b) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < end) {
      a.last = a.session.frame(a.memory, &a.store);
      if (with_b) b.last = b.session.frame(b.memory, &b.store);
      if (done()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  };
  bool ok = frames([&] { return a.last.hosting; }, false);
  std::thread net_b = net_loop(b);
  ok = ok && frames([&] { return b.last.connected && b.synced(); }, true);
  if (ok) {
    b.store.set(kKey, 1);
    ok = frames([&] { return a.store.get(kKey) == 1 && b.session.flags()->pending() == 0; }, true);
  }
  if (ok) {
    a.store.set(kChest, 1);
    ok = frames([&] { return b.store.get(kChest) == 1; }, true);
  }
  // The window's view from a third thread while it runs.
  const auto players =
      a.session.with_client([](net::Client& c) { return c.players().size(); });

  stop = true;
  a.session.wake();
  b.session.wake();
  net_a.join();
  net_b.join();
  server_thread.join();
  CHECK(ok);
  CHECK(players == 2);
}

TEST_CASE("session: reset sends the status again on the next connected frame") {
  EnetLife enet;
  coop::server::EnetServer server(any_port());
  REQUIRE(server.start());
  std::vector<Peer*> peers;

  Peer a(server.port(), "Link", SessionSettings{});
  join(server, peers, a);
  REQUIRE(pump(server, peers, [&] { return a.session.sampler().last_sample().has_value(); }));

  // A new game process: the reset is applied on the next connected frame,
  // before that frame's reading (none here), so nothing was sent since.
  a.session.reset();
  const auto report = a.session.frame(std::nullopt, nullptr);
  CHECK(report.connected);
  CHECK(report.queued == 0);
  CHECK_FALSE(a.session.sampler().last_sample().has_value());

  // The next reading goes out as a status and a sample at once.
  const auto again = a.session.frame(a.memory, nullptr);
  CHECK(again.queued == 2);
  CHECK(a.session.sampler().last_sample().has_value());
  CHECK(a.session.with_client([](net::Client& c) { return c.state() == net::State::connected; }));
}

// Issue 175 (stray, server-back): the server is stopped and started again
// on the same port while both games run. Both reconnect, and each must show
// the other again (a pose with fade > 0 at its own server time), and keep
// showing it, not only for the first sample.
TEST_CASE("session: after a server restart both players show each other again (issue 175)") {
  EnetLife enet;
  auto options = any_port();
  auto server = std::make_unique<coop::server::EnetServer>(options);
  REQUIRE(server->start());
  const std::uint16_t port = server->port();
  options.port = port;

  // Lost after 1 s of nothing, not the game's 10 s.
  Peer a(port, "Link", SessionSettings{}, 1000);
  Peer b(port, "Zelda", SessionSettings{}, 1000);
  std::vector<Peer*> peers;
  join(*server, peers, a);
  join(*server, peers, b);

  // Whether `viewer` shows `number` now, faded in.
  auto shows = [](Peer& viewer, std::uint8_t number) {
    const std::uint64_t now =
        viewer.session.with_client([](net::Client& c) { return c.server_now_us(); });
    return viewer.session.with_presence([&](coop::presence::Presence& p) {
      const auto pose = p.shown(number, now);
      return pose && pose->fade > 0.0f;
    });
  };
  // `done` true on every round for `ms`.
  auto holds = [&](coop::server::EnetServer& s, const std::function<bool()>& done, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      s.service(1);
      for (Peer* p : peers) p->step();
      if (!done()) return false;
    }
    return true;
  };
  const std::uint8_t na = a.number();
  const std::uint8_t nb = b.number();
  REQUIRE(pump(*server, peers, [&] { return shows(a, nb) && shows(b, na); }));
  CHECK(holds(*server, [&] { return shows(a, nb) && shows(b, na); }, 1000));

  // The server goes away for a while; its clock starts again from 0.
  server.reset();
  {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < end) {
      for (Peer* p : peers) p->step();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  server = std::make_unique<coop::server::EnetServer>(options);
  REQUIRE(server->start());

  // Both lose the old connection (timeout_ms) and join the new server.
  REQUIRE(pump(*server, peers, [&] {
    return a.number() != 0 && b.number() != 0 &&
           a.session.with_client([](net::Client& c) { return c.state() == net::State::connected; }) &&
           b.session.with_client([](net::Client& c) { return c.state() == net::State::connected; }) &&
           a.session.with_client([](net::Client& c) { return c.players().size(); }) == 2 &&
           b.session.with_client([](net::Client& c) { return c.players().size(); }) == 2;
  }, 20000));
  const std::uint8_t na2 = a.number();
  const std::uint8_t nb2 = b.number();
  REQUIRE(pump(*server, peers, [&] { return shows(a, nb2) && shows(b, na2); }, 3000));
  CHECK(holds(*server, [&] { return shows(a, nb2) && shows(b, na2); }, 2000));
}
