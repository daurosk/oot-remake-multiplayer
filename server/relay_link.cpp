#include "relay_link.hpp"

#include <chrono>
#include <memory>
#include <random>
#include <utility>
#include <vector>

namespace coop::server::relay {

namespace {

constexpr std::uint64_t kRegisterEveryMs = 1'000;
constexpr std::uint64_t kRefusedRetryMs = 30'000;
constexpr std::uint64_t kKeepaliveEveryMs = 5'000;
constexpr std::uint64_t kRelayQuietMs = 20'000;
constexpr std::uint64_t kFlowIdleMs = 60'000;

bool parse_port(const std::string& text, std::uint16_t& port) {
  if (text.empty() || text.size() > 5) return false;
  unsigned value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<unsigned>(c - '0');
  }
  if (value == 0 || value > 65535) return false;
  port = static_cast<std::uint16_t>(value);
  return true;
}

}  // namespace

bool parse_relay_address(const std::string& text, std::string& host, std::uint16_t& port) {
  std::string h = text;
  std::uint16_t p = kDefaultRelayPort;
  if (const auto colon = h.rfind(':'); colon != std::string::npos) {
    if (!parse_port(h.substr(colon + 1), p)) return false;
    h = h.substr(0, colon);
  }
  if (h.empty() || h.size() > 253) return false;
  for (const char c : h) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '-';
    if (!ok) return false;
  }
  host = h;
  port = p;
  return true;
}

RelayLink::RelayLink(RelayLinkConfig config) : config_(std::move(config)) {
  if (!config_.random) {
    auto device = std::make_shared<std::random_device>();
    config_.random = [device] {
      return (static_cast<std::uint64_t>((*device)()) << 32) ^ static_cast<std::uint64_t>((*device)());
    };
  }
  started_ms_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
}

RelayLink::~RelayLink() {
  for (auto& [id, flow] : flows_) enet_socket_destroy(flow.socket);
  if (socket_ != ENET_SOCKET_NULL) enet_socket_destroy(socket_);
}

std::uint64_t RelayLink::now_ms() const {
  const auto ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
  // +1: 0 means "never" in the timers below.
  return ms - started_ms_ + 1;
}

void RelayLink::say(const std::string& text) const {
  if (config_.log) config_.log(text);
}

bool RelayLink::start(std::string& error) {
  if (enet_address_set_host(&relay_, config_.relay_host.c_str()) != 0) {
    error = "cannot find the relay " + config_.relay_host + " (is the name right, and is this PC online?)";
    return false;
  }
  relay_.port = config_.relay_port;
  enet_address_set_host_ip(&server_, "127.0.0.1");
  server_.port = config_.server_port;
  socket_ = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (socket_ == ENET_SOCKET_NULL) {
    error = "cannot open a network socket for the relay";
    return false;
  }
  ENetAddress any{};
  any.host = ENET_HOST_ANY;
  any.port = 0;
  if (enet_socket_bind(socket_, &any) != 0) {
    error = "cannot open a network socket for the relay";
    return false;
  }
  enet_socket_set_option(socket_, ENET_SOCKOPT_NONBLOCK, 1);
  do {
    token_ = config_.random();
  } while (token_ == 0);
  first_try_ms_ = now_ms();
  return true;
}

void RelayLink::send_relay(const Message& m) {
  const std::vector<std::uint8_t> bytes = encode(m);
  ENetBuffer buffer;
  buffer.data = const_cast<std::uint8_t*>(bytes.data());
  buffer.dataLength = bytes.size();
  enet_socket_send(socket_, &relay_, &buffer, 1);
  last_sent_ms_ = now_ms();
}

void RelayLink::close_flow(std::uint16_t flow, bool tell_relay) {
  auto it = flows_.find(flow);
  if (it == flows_.end()) return;
  enet_socket_destroy(it->second.socket);
  flows_.erase(it);
  if (tell_relay) {
    Message m;
    m.kind = Kind::kLinkClose;
    m.token = token_;
    m.flow = flow;
    send_relay(m);
  }
}

void RelayLink::handle(const Message& m, std::uint64_t now) {
  switch (m.kind) {
    case Kind::kRegistered:
      if (m.token != token_) return;
      last_heard_ms_ = now;
      refused_hard_ = false;
      if (!registered_ || m.code != code_) {
        registered_ = true;
        if (m.code != code_) {
          code_ = m.code;
          if (config_.on_code) config_.on_code(code_);
        } else {
          say("the relay answers again; the join code is still " + show_code(code_));
        }
      }
      return;
    case Kind::kRefused: {
      if (m.token != token_) return;
      last_heard_ms_ = now;
      const auto why = static_cast<Refusal>(m.status);
      if (why == Refusal::kUnknownSession) {
        if (!code_.empty()) {
          say("the relay forgot this host (was it restarted?): asking it for a new join code; "
              "give your friends the new one");
        }
        registered_ = false;
        code_.clear();
        std::vector<std::uint16_t> ids;
        for (const auto& [id, flow] : flows_) ids.push_back(id);
        for (const std::uint16_t id : ids) close_flow(id, false);
        first_try_ms_ = now;
        return;
      }
      if (!refused_hard_) {
        say(std::string("the relay refused this host: ") + std::string(refusal_text(why)) +
            "; trying again every 30 seconds");
      }
      refused_hard_ = true;
      registered_ = false;
      return;
    }
    case Kind::kRelayData: {
      last_heard_ms_ = now;
      auto it = flows_.find(m.flow);
      if (it == flows_.end()) {
        if (flows_.size() >= config_.max_flows) return;
        Flow flow;
        flow.socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        if (flow.socket == ENET_SOCKET_NULL) return;
        ENetAddress local{};
        enet_address_set_host_ip(&local, "127.0.0.1");
        local.port = 0;
        if (enet_socket_bind(flow.socket, &local) != 0) {
          enet_socket_destroy(flow.socket);
          return;
        }
        enet_socket_set_option(flow.socket, ENET_SOCKOPT_NONBLOCK, 1);
        it = flows_.emplace(m.flow, flow).first;
        say("a guest came in through the relay (" + std::to_string(flows_.size()) + " now)");
      }
      it->second.last_ms = now;
      ENetBuffer buffer;
      buffer.data = const_cast<std::uint8_t*>(m.payload.data());
      buffer.dataLength = m.payload.size();
      enet_socket_send(it->second.socket, &server_, &buffer, 1);
      return;
    }
    case Kind::kRelayClose:
      last_heard_ms_ = now;
      close_flow(m.flow, false);
      return;
    default:
      return;
  }
}

void RelayLink::service() {
  if (socket_ == ENET_SOCKET_NULL) return;
  std::uint8_t data[2048];
  std::uint64_t now = now_ms();
  for (int i = 0; i < 1024; ++i) {
    ENetAddress from{};
    ENetBuffer buffer;
    buffer.data = data;
    buffer.dataLength = sizeof data;
    const int got = enet_socket_receive(socket_, &from, &buffer, 1);
    if (got == 0) break;
    if (got < 0) continue;
    // Only the relay may speak for it.
    if (from.host != relay_.host || from.port != relay_.port) continue;
    if (auto m = decode(std::span<const std::uint8_t>(data, static_cast<std::size_t>(got)))) handle(*m, now);
  }

  // The server's answers to each guest.
  std::vector<std::uint16_t> idle;
  for (auto& [id, flow] : flows_) {
    for (int i = 0; i < 256; ++i) {
      ENetAddress from{};
      ENetBuffer buffer;
      buffer.data = data;
      buffer.dataLength = sizeof data;
      const int got = enet_socket_receive(flow.socket, &from, &buffer, 1);
      if (got == 0) break;
      if (got < 0) continue;
      if (from.port != server_.port || static_cast<std::size_t>(got) > kMaxPayloadBytes) continue;
      Message m;
      m.kind = Kind::kLinkData;
      m.token = token_;
      m.flow = id;
      m.payload.assign(data, data + got);
      send_relay(m);
      flow.last_ms = now;
    }
    if (now - flow.last_ms >= kFlowIdleMs) idle.push_back(id);
  }
  for (const std::uint16_t id : idle) close_flow(id, true);

  now = now_ms();
  if (!registered_) {
    const std::uint64_t every = refused_hard_ ? kRefusedRetryMs : kRegisterEveryMs;
    if (last_register_ms_ == 0 || now - last_register_ms_ >= every) {
      last_register_ms_ = now;
      Message m;
      m.kind = Kind::kRegister;
      m.token = token_;
      m.key = config_.key;
      send_relay(m);
    }
    if (!refused_hard_ && now - first_try_ms_ >= 10'000 &&
        (last_waiting_note_ms_ == 0 || now - last_waiting_note_ms_ >= 30'000)) {
      last_waiting_note_ms_ = now;
      say("no answer from the relay at " + config_.relay_host + ":" + std::to_string(config_.relay_port) +
          " yet: is it running, and is its UDP port open to the internet? Still trying.");
    }
    return;
  }
  last_waiting_note_ms_ = 0;
  if (now - last_heard_ms_ >= kRelayQuietMs) {
    say("the relay stopped answering; still trying (the join code stays the same if the relay "
        "still has it)");
    registered_ = false;
    first_try_ms_ = now;
    return;
  }
  // Keepalives on their own clock: forwarded data gets no answer, so only
  // these tell that the relay is still there.
  if (now - last_keepalive_ms_ >= kKeepaliveEveryMs) {
    last_keepalive_ms_ = now;
    Message m;
    m.kind = Kind::kKeepalive;
    m.token = token_;
    send_relay(m);
  }
}

void RelayLink::stop() {
  if (socket_ == ENET_SOCKET_NULL || code_.empty()) return;
  Message m;
  m.kind = Kind::kBye;
  m.token = token_;
  send_relay(m);
  send_relay(m);  // a lost one would leave the code for 30 seconds
}

}  // namespace coop::server::relay
