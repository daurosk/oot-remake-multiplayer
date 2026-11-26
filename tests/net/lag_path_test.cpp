// A slow line with jitter (player report #37, 150-350 ms ping): the real
// server, two real clients, each behind a relay that holds every datagram for
// a one-way delay plus a random jitter (in order), as tools/netsim does. The
// host streams poses and samples (unreliable) and flags and a chat line
// (reliable, like a DAMAGE) every quarter second; the joiner measures how many
// poses come, how late the chat lines are, and how it draws the host
// (coop/presence) with the old fixed 100 ms delay and with each player's own
// delay from its samples' ages (show_delay.hpp).
//
// Measured (MESSAGE lines): the host drawn between two samples 0 % of frames
// at the fixed 100 ms, 99.6 % at the aged delay; with steady_throttle
// (enet_throttle.hpp) 100 % of poses come (99.5 % without; 98.3 % without it
// and a 400 ms stall every 5 s).

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "coop/net/client.hpp"
#include "coop/presence/presence.hpp"
#include "coop/protocol/messages.hpp"
#include "enet_server.hpp"

namespace {

namespace proto = coop::protocol;
using coop::net::Client;
using coop::net::ClientConfig;
using coop::net::EventKind;
using coop::net::State;

struct EnetLife {
  EnetLife() { REQUIRE(enet_initialize() == 0); }
  ~EnetLife() { enet_deinitialize(); }
};

// One simulated millisecond a round (see small_path_test.cpp).
struct SimClock {
  std::uint64_t ms = 100'000;
  void step() { ++ms; }
  void enet() const { enet_time_set(static_cast<enet_uint32>(ms)); }
  std::function<std::uint64_t()> us() const {
    return [this] { return ms * 1000u; };
  }
  int since(std::uint64_t t) const { return static_cast<int>(ms - t); }
};

// Client <-> relay <-> server on 127.0.0.1; each datagram is held for
// `lag_ms` plus 0 to `jitter_ms` (uniform), each way, on the sim clock, in
// the order it came.
class LagPath {
 public:
  LagPath(std::uint16_t server_port, const SimClock& clock, int lag_ms, int jitter_ms,
          std::uint32_t seed)
      : clock_(clock), lag_ms_(lag_ms), jitter_(0, jitter_ms), rng_(seed) {
    outer_ = open();
    inner_ = open();
    ENetAddress got{};
    REQUIRE(enet_socket_get_address(outer_, &got) == 0);
    port_ = got.port;
    REQUIRE(enet_address_set_host_ip(&server_, "127.0.0.1") == 0);
    server_.port = server_port;
  }
  ~LagPath() {
    enet_socket_destroy(outer_);
    enet_socket_destroy(inner_);
  }
  LagPath(const LagPath&) = delete;
  LagPath& operator=(const LagPath&) = delete;

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
      hold(true, data, static_cast<std::size_t>(n));
    }
    for (;;) {  // server to client
      ENetAddress from{};
      buffer.data = data;
      buffer.dataLength = sizeof data;
      const int n = enet_socket_receive(inner_, &from, &buffer, 1);
      if (n <= 0) break;
      hold(false, data, static_cast<std::size_t>(n));
    }
    while (!held_.empty() && held_.begin()->first.first <= clock_.ms) {
      const Held& h = held_.begin()->second;
      ENetBuffer b{};
      b.data = const_cast<std::uint8_t*>(h.data.data());
      b.dataLength = h.data.size();
      if (h.up) {
        enet_socket_send(inner_, &server_, &b, 1);
      } else if (have_client_) {
        enet_socket_send(outer_, &client_, &b, 1);
      }
      held_.erase(held_.begin());
    }
  }

 private:
  struct Held {
    bool up = false;
    std::vector<std::uint8_t> data;
  };
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
  void hold(bool up, const std::uint8_t* data, std::size_t n) {
    // Jitter alone keeps the order, as on one route (tools/netsim does the same).
    std::uint64_t& last = up ? last_up_ : last_down_;
    std::uint64_t at =
        (std::max)(last, clock_.ms + static_cast<std::uint64_t>(lag_ms_ + jitter_(rng_)));
    if (spike_every_ms_ > 0) {  // a stall (Wi-Fi): what falls in it comes out at its end
      const std::uint64_t into = at % spike_every_ms_;
      if (into < spike_ms_) at += spike_ms_ - into;
    }
    last = at;
    held_.emplace(std::make_pair(at, next_++), Held{up, std::vector<std::uint8_t>(data, data + n)});
  }

  const SimClock& clock_;
  int lag_ms_;
  std::uniform_int_distribution<int> jitter_;
  std::mt19937 rng_;
  std::map<std::pair<std::uint64_t, std::uint64_t>, Held> held_;  // (due ms, order)
  std::uint64_t next_ = 0;
  std::uint64_t last_up_ = 0;
  std::uint64_t last_down_ = 0;

 public:
  std::uint64_t spike_every_ms_ = 0;
  std::uint64_t spike_ms_ = 0;
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
  c.hello.build_id = "lag path test";
  c.hello.game_build_id = "e0f3ccfaf50065ae";
  c.hello.game_version = "1.1.1";
  c.hello.nickname = name;
  c.join_password = "test-pass";
  c.retry_first_ms = 50;
  c.retry_max_ms = 400;
  c.now_us = clock.us();
  return c;
}

// Flags the host changes (world places, drops: reliable, as DAMAGE is).
std::vector<proto::FlagEntry> flags_from(std::uint32_t first, std::uint32_t count, std::int32_t value) {
  std::vector<proto::FlagEntry> out;
  for (std::uint32_t i = 0; i < count; ++i) {
    out.push_back({first + i, 0, proto::FlagKind::int32, static_cast<std::uint32_t>(value)});
  }
  return out;
}

// A pose about the size of Link's (most bones turn every frame).
proto::PlayerPose pose(std::uint64_t n) {
  proto::PlayerPose p;
  p.time.sequence = static_cast<std::uint32_t>(n);
  p.skeleton = 0x1234;
  for (std::uint16_t b = 0; b < 60; ++b) {
    proto::PoseBone bone;
    bone.bone = b;
    bone.rotation = std::array<std::int16_t, 4>{1, 2, 3, static_cast<std::int16_t>(n & 0x7fff)};
    p.bones.push_back(bone);
  }
  return p;
}

struct LineResult {
  double pose_share = 0;  // of the poses A sent, the share B got
  int median_ms = 0;      // chat lines A to B, one way
  int worst_ms = 0;
  std::size_t chat_lines = 0;
  bool connected = false;
  // How B drew A (every 16 ms): the share drawn between two samples (smooth),
  // with the old fixed 100 ms delay and with the delay from the ages.
  double smooth_fixed = 0;
  double smooth_aged = 0;
  std::uint64_t delay_us = 0;  // the aged delay at the end
};

// A minute of play over the slow line: a pose every 33 ms, a sample every
// 16 ms, `flags` flags (reliable) every 100 ms and a chat line (reliable)
// every 250 ms, from A (the host) to B.
LineResult play_over_slow_line(std::uint32_t flags, std::uint64_t spike_every_ms = 0,
                               std::uint64_t spike_ms = 0) {
  EnetLife enet;
  SimClock clock;
  clock.enet();
  coop::server::ServerOptions options;
  options.port = 0;
  options.bind = 0x7F000001u;  // 127.0.0.1 only: a test listens on no other address
  options.session.join_password = "test-pass";
  options.now_us = clock.us();
  coop::server::EnetServer server(options);
  REQUIRE(server.start());

  // Each way 62 ms + 0-60 ms jitter: each game 124-244 ms round trip from
  // the server, game to game 250-490 ms (the players saw 150-350).
  LagPath path_a(server.port(), clock, 62, 60, 1);
  LagPath path_b(server.port(), clock, 62, 60, 2);
  for (auto* p : {&path_a, &path_b}) {
    p->spike_every_ms_ = spike_every_ms;
    p->spike_ms_ = spike_ms;
  }
  Client a(config_to(path_a.port(), "Link", clock));
  Client b(config_to(path_b.port(), "Zelda", clock));
  REQUIRE(a.start());

  std::uint64_t poses_got = 0;
  // B's view of A twice: never told the ages (the old fixed delay), and told.
  coop::presence::Presence fixed;
  coop::presence::Presence aged;
  std::uint64_t frames = 0, smooth_fixed = 0, smooth_aged = 0;
  std::vector<int> chat_late;  // ms, each chat line A sent
  auto round = [&] {
    clock.step();
    clock.enet();
    server.service(0);
    for (auto* p : {&path_a, &path_b}) p->service();
    clock.enet();
    a.service(0);
    while (a.next_event()) {
    }
    clock.enet();
    b.service(0);
    while (auto e = b.next_event()) {
      if (e->kind == EventKind::chat) {
        chat_late.push_back(static_cast<int>(clock.ms - std::stoull(e->text)));
      } else if (e->kind == EventKind::message &&
                 std::holds_alternative<proto::PlayerPose>(e->message)) {
        ++poses_got;
      } else if (e->kind == EventKind::message) {
        if (const auto* s = std::get_if<proto::PlayerSample>(&e->message)) {
          fixed.on_sample(*s);
          aged.on_sample(*s);
          aged.seen(s->player_number, s->time.server_time_us, b.server_now_us());
        }
      }
    }
    for (auto* p : {&path_a, &path_b}) p->service();
  };
  auto run_until = [&](int ms, auto done) {
    const auto start = clock.ms;
    while (clock.since(start) < ms) {
      round();
      if (done()) return true;
    }
    return false;
  };
  REQUIRE(run_until(5000, [&] { return a.is_host(); }));
  REQUIRE(b.start());
  REQUIRE(run_until(10000, [&] { return b.state() == State::connected && b.synced(); }));
  for (auto* p : {&fixed, &aged}) {
    proto::Welcome w;
    w.player_number = b.player_number();
    p->on_welcome(w);
    REQUIRE(p->on_joined({a.player_number(), "Link"}));
  }

  const auto start = clock.ms;
  std::uint64_t poses_sent = 0;
  std::uint64_t r_delay = 0;
  while (clock.since(start) < 60'000) {
    const int t = clock.since(start);
    if (t % 33 == 0) REQUIRE(a.send(pose(poses_sent++)));
    if (t % 16 == 0) {
      proto::PlayerSample s;
      s.time = {a.server_now_us(), static_cast<std::uint32_t>(t)};
      s.position = proto::Vec3{static_cast<float>(t) * 0.001f, 2.0f, 3.0f};
      s.velocity = proto::Vec3{1.0f, 0.0f, 0.0f};
      a.send(s);
    }
    // New keys each time (a changed key waiting to be sent is merged).
    if (t % 100 == 50) REQUIRE(a.set_flags(flags_from(1 + flags * static_cast<std::uint32_t>(t / 100 % 64), flags, t)));
    if (t % 250 == 0) REQUIRE(a.send_chat(std::to_string(clock.ms)));
    round();
    if (t % 16 == 8 && t > 5000) {
      ++frames;
      const auto f = fixed.shown(a.player_number(), b.server_now_us());
      const auto g = aged.shown(a.player_number(), b.server_now_us());
      if (f && f->how == coop::presence::How::interpolated) ++smooth_fixed;
      if (g && g->how == coop::presence::How::interpolated) ++smooth_aged;
    }
  }
  r_delay = aged.show_delay_us(a.player_number());
  run_until(10000, [] { return false; });

  LineResult r;
  r.smooth_fixed = static_cast<double>(smooth_fixed) / static_cast<double>(frames);
  r.smooth_aged = static_cast<double>(smooth_aged) / static_cast<double>(frames);
  r.delay_us = r_delay;
  r.chat_lines = chat_late.size();
  std::sort(chat_late.begin(), chat_late.end());
  if (!chat_late.empty()) {
    r.median_ms = chat_late[chat_late.size() / 2];
    r.worst_ms = chat_late.back();
  }
  r.pose_share = static_cast<double>(poses_got) / static_cast<double>(poses_sent);
  r.connected = a.state() == State::connected && b.state() == State::connected;
  MESSAGE(flags << " flags a tenth, spike " << spike_ms << "/" << spike_every_ms << " ms: poses " << poses_got << "/" << poses_sent << " (" << r.pose_share * 100.0
                << "%), chat lines " << r.chat_lines << ", one way median " << r.median_ms << " ms, worst "
                << r.worst_ms << " ms; A drawn smooth " << r.smooth_fixed * 100.0 << "% at the fixed 100 ms, "
                << r.smooth_aged * 100.0 << "% at " << r.delay_us / 1000 << " ms from the ages");
  return r;
}

}  // namespace

TEST_CASE("lag path: 250-490 ms ping with jitter keeps poses flowing and reliable messages prompt (report 37)") {
  const LineResult r = play_over_slow_line(40);
  CHECK(r.connected);
  CHECK(r.chat_lines == 240);
  CHECK(r.pose_share > 0.95);
  CHECK(r.worst_ms < 1000);
  // The other player drawn between two samples, not carried on or held
  // (report 37: at the old fixed 100 ms it was almost never).
  CHECK(r.smooth_aged > 0.95);
  CHECK(r.smooth_fixed < 0.2);
}

TEST_CASE("lag path: a burst of reliable traffic (the rupee loop's drops) does not hold reliable messages for seconds") {
  const LineResult r = play_over_slow_line(400);  // about 6 KB a tenth of a second
  CHECK(r.connected);
  CHECK(r.chat_lines == 240);
  CHECK(r.pose_share > 0.95);
  CHECK(r.worst_ms < 1000);
}

TEST_CASE("lag path: a 400 ms stall every 5 s (Wi-Fi) does not starve the poses afterwards") {
  const LineResult r = play_over_slow_line(40, 5000, 400);
  CHECK(r.connected);
  CHECK(r.chat_lines == 240);
  CHECK(r.pose_share > 0.9);
  CHECK(r.worst_ms < 1500);
}
