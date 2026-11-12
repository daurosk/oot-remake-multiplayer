#include "join_helper.hpp"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <random>

#include "nat_net.hpp"

namespace coop::server::nat {

namespace {

constexpr std::uint32_t kLoopback = 0x7F000001u;
constexpr std::uint64_t kStunWaitMs = 3000;

}  // namespace

std::string no_way_help() {
  return "No way through the routers was found. This happens when both routers change the port for every "
         "address (symmetric NAT) or a provider shares one internet address between many homes (CGNAT). "
         "What always works: a free VPN network such as Radmin VPN or Tailscale (docs/play-online.md, "
         "\"When the invite code does not work: a free VPN network\"), or the host opens UDP port 27510 in the router "
         "(\"Without a VPN: port forwarding\").";
}

JoinHelper::JoinHelper(JoinConfig config) : config_(std::move(config)) {
  key_ = punch_key(config_.password);
  std::random_device device;
  for (auto& b : nonce_) b = static_cast<std::uint8_t>(device());
  for (const Addr& server : config_.stun_servers) {
    StunAsk ask;
    ask.server = server;
    for (auto& b : ask.id) b = static_cast<std::uint8_t>(device());
    stun_.push_back(ask);
  }
}

JoinHelper::~JoinHelper() { stop(); }

void JoinHelper::stop() {
  if (public_socket_ != ENET_SOCKET_NULL) enet_socket_destroy(public_socket_);
  if (game_socket_ != ENET_SOCKET_NULL) enet_socket_destroy(game_socket_);
  public_socket_ = ENET_SOCKET_NULL;
  game_socket_ = ENET_SOCKET_NULL;
}

std::uint64_t JoinHelper::now_ms() const {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

std::uint64_t JoinHelper::unix_now() const {
  if (config_.unix_now) return config_.unix_now();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

void JoinHelper::say(const std::string& text) const {
  if (config_.log) config_.log(text);
}

std::uint16_t JoinHelper::game_port() const {
  ENetAddress a{};
  if (game_socket_ == ENET_SOCKET_NULL || enet_socket_get_address(game_socket_, &a) != 0) return 0;
  return a.port;
}

std::uint16_t JoinHelper::public_socket_port() const {
  ENetAddress a{};
  if (public_socket_ == ENET_SOCKET_NULL || enet_socket_get_address(public_socket_, &a) != 0) return 0;
  return a.port;
}

bool JoinHelper::start(std::string& error) {
  if (config_.password.empty()) {
    error = "an invite code always has a join password; this one has none";
    return false;
  }
  public_socket_ = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  game_socket_ = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (public_socket_ == ENET_SOCKET_NULL || game_socket_ == ENET_SOCKET_NULL) {
    error = "cannot make a UDP socket";
    stop();
    return false;
  }
  ENetAddress any{};
  any.host = ENET_HOST_ANY;
  any.port = 0;
  if (enet_socket_bind(public_socket_, &any) != 0) {
    error = "cannot open a UDP port for the way to the host";
    stop();
    return false;
  }
  // Only this PC's own game can reach the helper's game side.
  const ENetAddress local = to_enet(Addr{kLoopback, config_.game_port});
  if (enet_socket_bind(game_socket_, &local) != 0) {
    error = "UDP port " + std::to_string(config_.game_port) +
            " on 127.0.0.1 is taken (another Join.bat helper still open?)";
    stop();
    return false;
  }
  enet_socket_set_option(public_socket_, ENET_SOCKOPT_NONBLOCK, 1);
  enet_socket_set_option(game_socket_, ENET_SOCKOPT_NONBLOCK, 1);
  enet_socket_set_option(public_socket_, ENET_SOCKOPT_RCVBUF, 1 << 20);
  enet_socket_set_option(public_socket_, ENET_SOCKOPT_SNDBUF, 1 << 20);

  start_ = now_ms();
  next_probe_ = start_;
  stun_tries_ = 1;
  for (auto& ask : stun_) {
    const auto request = stun_request(ask.id);
    send_public(ask.server, request);
  }
  say("trying the host at " + show(config_.host) + " directly");
  return true;
}

void JoinHelper::send_public(Addr to, std::span<const std::uint8_t> bytes) {
  const ENetAddress address = to_enet(to);
  ENetBuffer buffer;
  buffer.data = const_cast<std::uint8_t*>(bytes.data());
  buffer.dataLength = bytes.size();
  enet_socket_send(public_socket_, &address, &buffer, 1);
}

void JoinHelper::send_punch(PunchType type) {
  Punch p;
  p.type = type;
  p.nonce = nonce_;
  p.time = unix_now();
  p.from = public_.value_or(Addr{});
  p.to = config_.host;
  const auto bytes = encode_punch(p, key_);
  send_public(config_.host, bytes);
}

void JoinHelper::open(bool direct) {
  state_ = JoinState::kOpen;
  direct_ = direct;
  if (direct) {
    say("the host's server answered directly (their router lets the game in). The game connects now; "
        "keep this window open while you play.");
  } else {
    say("the way through both routers is open. The game connects now; keep this window open while you play.");
  }
}

void JoinHelper::on_public(Addr from, std::span<const std::uint8_t> data) {
  for (auto& ask : stun_) {
    if (ask.server != from) continue;
    const StunAnswer answer = read_stun(data, ask.id);
    if (answer.result == StunResult::kOk && !ask.answered) {
      ask.answered = true;
      ask.mapped = answer.mapped;
      if (!public_) {
        public_ = answer.mapped;
      } else if (*public_ != answer.mapped) {
        symmetric_ = true;
      }
    } else {
      ++stats_.dropped;
    }
    return;
  }
  if (from != config_.host) {
    ++stats_.dropped;
    return;
  }
  if (looks_like_punch(data)) {
    const PunchRead read = read_punch(data, key_, unix_now());
    const bool ours = read.check == PunchCheck::kOk && read.punch.nonce == nonce_ &&
                      (read.punch.type == PunchType::kProbeAck || read.punch.type == PunchType::kHostPunch);
    if (!ours) {
      ++stats_.rejected;
      return;
    }
    if (state_ != JoinState::kOpen) open(read.punch.type == PunchType::kProbeAck);
    return;
  }
  if (state_ != JoinState::kOpen || !game_) {
    ++stats_.dropped;
    return;
  }
  const ENetAddress to = to_enet(*game_);
  ENetBuffer buffer;
  buffer.data = const_cast<std::uint8_t*>(data.data());
  buffer.dataLength = data.size();
  enet_socket_send(game_socket_, &to, &buffer, 1);
  ++stats_.to_game;
}

void JoinHelper::on_game(Addr from, std::span<const std::uint8_t> data) {
  if (from.ip != kLoopback) {
    ++stats_.dropped;
    return;
  }
  game_ = from;  // a restarted game gets a new port: follow it
  if (state_ != JoinState::kOpen) {
    ++stats_.dropped;  // ENet sends its connect again by itself
    return;
  }
  send_public(config_.host, data);
  ++stats_.to_host;
}

void JoinHelper::tick(std::uint64_t now) {
  if (state_ == JoinState::kOpen) return;
  const std::uint64_t elapsed = now - start_;
  // STUN: two more tries for the servers that did not answer.
  if ((stun_tries_ == 1 && elapsed >= 700) || (stun_tries_ == 2 && elapsed >= 1600)) {
    ++stun_tries_;
    for (auto& ask : stun_) {
      if (!ask.answered) send_public(ask.server, stun_request(ask.id));
    }
  }
  if (config_.probe && now >= next_probe_) {
    send_punch(PunchType::kProbe);
    next_probe_ = now + (state_ == JoinState::kTrying ? 500 : 1000);
  }
  const bool stun_done =
      elapsed >= kStunWaitMs || std::all_of(stun_.begin(), stun_.end(), [](const StunAsk& a) { return a.answered; });
  if (state_ == JoinState::kTrying && elapsed >= config_.direct_ms && stun_done) {
    state_ = JoinState::kWaitingForHost;
    waiting_since_ = now;
    next_punch_ = now;
    if (public_) {
      Answer a;
      a.guest = *public_;
      a.nonce = nonce_;
      a.time = static_cast<std::uint32_t>(unix_now());
      answer_ = make_answer(a, config_.password);
      say("the host's server did not answer directly: their router keeps it closed.");
      say("Send this ANSWER CODE to the host. The host pastes it into their server window and presses Enter:");
      say("answer code: " + answer_);
      if (symmetric_) {
        say("note: your router changes the port for every address (symmetric NAT), so this may not work; "
            "if it does not, see the help that shows in 3 minutes.");
      }
      say("waiting for the host (keep this window open)...");
      if (config_.on_answer) config_.on_answer(answer_);
    } else {
      say("the host's server did not answer directly, and no STUN server answered, so there is no answer code "
          "to send (a firewall may block UDP). Still trying the direct way.");
    }
  }
  if (state_ == JoinState::kWaitingForHost) {
    if (public_ && now >= next_punch_) {
      send_punch(PunchType::kGuestPunch);
      next_punch_ = now + 250;
    }
    if (!helped_ && now - waiting_since_ >= config_.punch_ms) {
      helped_ = true;
      say(no_way_help());
      say("(This window keeps trying; it says when the way opens.)");
    }
  }
}

void JoinHelper::service(std::uint32_t wait_ms) {
  if (public_socket_ == ENET_SOCKET_NULL) return;
  ENetSocketSet set;
  ENET_SOCKETSET_EMPTY(set);
  ENET_SOCKETSET_ADD(set, public_socket_);
  ENET_SOCKETSET_ADD(set, game_socket_);
  const ENetSocket highest = public_socket_ > game_socket_ ? public_socket_ : game_socket_;
  enet_socketset_select(highest, &set, nullptr, wait_ms);

  std::uint8_t data[4096];
  for (int round = 0; round < 256; ++round) {
    bool any = false;
    for (ENetSocket s : {public_socket_, game_socket_}) {
      ENetAddress from{};
      ENetBuffer buffer;
      buffer.data = data;
      buffer.dataLength = sizeof data;
      const int got = enet_socket_receive(s, &from, &buffer, 1);
      if (got <= 0) continue;
      any = true;
      const std::span<const std::uint8_t> bytes(data, static_cast<std::size_t>(got));
      if (s == public_socket_) {
        on_public(from_enet(from), bytes);
      } else {
        on_game(from_enet(from), bytes);
      }
    }
    if (!any) break;
  }
  tick(now_ms());
}

}  // namespace coop::server::nat
