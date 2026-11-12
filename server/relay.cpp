#include "relay.hpp"

#include <chrono>
#include <cstring>
#include <memory>
#include <random>
#include <utility>

namespace coop::server::relay {

namespace {

// The address part, the same on every platform ENet runs on (the first
// dotted part is in the first byte in memory).
std::uint32_t first_bytes_value(std::uint32_t enet_host) {
  unsigned char b[4];
  std::memcpy(b, &enet_host, 4);
  return (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) | (std::uint32_t{b[2]} << 8) | b[3];
}

}  // namespace

std::string address_text(std::uint32_t enet_host) {
  const std::uint32_t v = first_bytes_value(enet_host);
  return std::to_string(v >> 24) + "." + std::to_string((v >> 16) & 255u) + "." +
         std::to_string((v >> 8) & 255u) + "." + std::to_string(v & 255u);
}

Relay::Relay(RelayConfig config) : config_(std::move(config)) {
  if (!config_.random) {
    auto device = std::make_shared<std::random_device>();
    config_.random = [device] {
      return (static_cast<std::uint64_t>((*device)()) << 32) ^ static_cast<std::uint64_t>((*device)());
    };
  }
}

void Relay::say(const std::string& text) const {
  if (config_.log) config_.log(text);
}

bool Relay::over_limit(std::map<std::uint32_t, Tries>& tries, std::uint32_t address,
                       std::uint32_t limit, std::uint64_t now_ms, bool count) {
  Tries& t = tries[address];
  if (t.count == 0 || now_ms - t.window_start >= config_.window_ms) {
    t.window_start = now_ms;
    t.count = 0;
  }
  if (t.count >= limit) return true;
  if (count) ++t.count;
  return false;
}

std::string Relay::new_code() {
  for (;;) {
    std::string code = make_code(config_.random());
    if (by_code_.find(code) == by_code_.end()) return code;
  }
}

std::vector<Outgoing> Relay::received(Endpoint from, std::span<const std::uint8_t> bytes,
                                      std::uint64_t now_ms) {
  std::vector<Outgoing> out;
  if (bytes.empty() || bytes.size() > kMaxPayloadBytes + 32) return out;
  if (has_magic(bytes)) {
    if (auto m = decode(bytes)) control(from, *m, now_ms, out);
    return out;
  }
  from_guest(from, bytes, now_ms, out);
  return out;
}

void Relay::control(Endpoint from, const Message& m, std::uint64_t now_ms,
                    std::vector<Outgoing>& out) {
  auto reply = [&](Message answer) { out.push_back({from, encode(answer)}); };
  auto refuse = [&](Refusal why) {
    Message r;
    r.kind = Kind::kRefused;
    r.token = m.token;
    r.status = static_cast<std::uint8_t>(why);
    reply(r);
  };
  switch (m.kind) {
    case Kind::kRegister: {
      auto it = sessions_.find(m.token);
      if (it == sessions_.end()) {
        if (!config_.host_key.empty() && m.key != config_.host_key) {
          ++stats_.hosts_refused;
          return refuse(Refusal::kWrongKey);
        }
        if (sessions_.size() >= config_.max_sessions) {
          ++stats_.hosts_refused;
          return refuse(Refusal::kFull);
        }
        std::size_t same_address = 0;
        for (const auto& [token, s] : sessions_) same_address += s.link.host == from.host ? 1u : 0u;
        if (same_address >= config_.max_sessions_per_address) {
          ++stats_.hosts_refused;
          return refuse(Refusal::kTooMany);
        }
        if (over_limit(registers_, from.host, config_.registers_per_window, now_ms, true)) {
          ++stats_.hosts_refused;
          return refuse(Refusal::kSlowDown);
        }
        Session s;
        s.token = m.token;
        s.code = new_code();
        s.link = from;
        s.last_ms = now_ms;
        by_code_[s.code] = s.token;
        it = sessions_.emplace(s.token, std::move(s)).first;
        ++stats_.hosts_registered;
        say("host " + address_text(from.host) + " registered, join code " + show_code(it->second.code) +
            " (" + std::to_string(sessions_.size()) + " host(s) now)");
      }
      it->second.link = from;
      it->second.last_ms = now_ms;
      Message ok;
      ok.kind = Kind::kRegistered;
      ok.token = m.token;
      ok.code = it->second.code;
      return reply(ok);
    }
    case Kind::kKeepalive:
    case Kind::kLinkData:
    case Kind::kLinkClose:
    case Kind::kBye: {
      auto it = sessions_.find(m.token);
      if (it == sessions_.end()) {
        if (m.kind != Kind::kBye) refuse(Refusal::kUnknownSession);
        return;
      }
      Session& s = it->second;
      // The host's address may change under it (its router renumbers ports);
      // the token is what says it is the same host.
      s.link = from;
      s.last_ms = now_ms;
      if (m.kind == Kind::kKeepalive) {
        Message ok;
        ok.kind = Kind::kRegistered;
        ok.token = m.token;
        ok.code = s.code;
        return reply(ok);
      }
      if (m.kind == Kind::kBye) return remove_session(it, "stopped");
      auto flow = s.flows.find(m.flow);
      if (flow == s.flows.end()) {
        if (m.kind == Kind::kLinkData) {
          Message gone;
          gone.kind = Kind::kRelayClose;
          gone.flow = m.flow;
          reply(gone);
        }
        return;
      }
      if (m.kind == Kind::kLinkClose) return remove_flow(s, m.flow);
      ++stats_.to_guest;
      out.push_back({flow->second.guest, m.payload});
      return;
    }
    case Kind::kLookup: {
      Message found;
      found.kind = Kind::kFound;
      if (over_limit(lookups_, from.host, config_.lookups_per_window, now_ms, false)) {
        ++stats_.lookups_limited;
        found.status = static_cast<std::uint8_t>(LookupResult::kSlowDown);
        return reply(found);
      }
      auto code = by_code_.find(m.code);
      if (code == by_code_.end()) {
        over_limit(lookups_, from.host, config_.lookups_per_window, now_ms, true);
        ++stats_.lookups_wrong;
        found.status = static_cast<std::uint8_t>(LookupResult::kUnknown);
        return reply(found);
      }
      Session& s = sessions_.at(code->second);
      if (s.guests.find(from.host) == s.guests.end() && s.guests.size() >= config_.max_guest_addresses) {
        found.status = static_cast<std::uint8_t>(LookupResult::kFull);
        return reply(found);
      }
      if (s.guests.find(from.host) == s.guests.end()) {
        say("a guest from " + address_text(from.host) + " used join code " + show_code(s.code));
      }
      s.guests[from.host] = now_ms;
      guest_session_[from.host] = s.token;
      ++stats_.lookups_ok;
      found.status = static_cast<std::uint8_t>(LookupResult::kOk);
      return reply(found);
    }
    default:
      return;  // relay -> host or guest messages: not for us
  }
}

void Relay::from_guest(Endpoint from, std::span<const std::uint8_t> bytes, std::uint64_t now_ms,
                       std::vector<Outgoing>& out) {
  if (bytes.size() > kMaxPayloadBytes) return;
  Session* s = nullptr;
  std::uint16_t flow = 0;
  if (auto known = flows_by_guest_.find(from); known != flows_by_guest_.end()) {
    s = &sessions_.at(known->second.first);
    flow = known->second.second;
  } else {
    auto let_in = guest_session_.find(from.host);
    if (let_in == guest_session_.end()) {
      ++stats_.not_let_in;
      return;
    }
    s = &sessions_.at(let_in->second);
    if (s->flows.size() >= config_.max_flows_per_session) {
      ++stats_.not_let_in;
      return;
    }
    do {
      flow = s->next_flow++;
    } while (flow == 0 || s->flows.count(flow) != 0);
    s->flows[flow] = Flow{from, now_ms};
    flows_by_guest_[from] = {s->token, flow};
  }
  s->flows[flow].last_ms = now_ms;
  s->guests[from.host] = now_ms;
  Message m;
  m.kind = Kind::kRelayData;
  m.flow = flow;
  m.payload.assign(bytes.begin(), bytes.end());
  ++stats_.to_host;
  out.push_back({s->link, encode(m)});
}

void Relay::remove_flow(Session& s, std::uint16_t flow) {
  auto it = s.flows.find(flow);
  if (it == s.flows.end()) return;
  flows_by_guest_.erase(it->second.guest);
  s.flows.erase(it);
}

void Relay::remove_session(std::map<std::uint64_t, Session>::iterator it, const char* why) {
  Session& s = it->second;
  for (const auto& [flow, f] : s.flows) flows_by_guest_.erase(f.guest);
  for (const auto& [address, last] : s.guests) {
    auto g = guest_session_.find(address);
    if (g != guest_session_.end() && g->second == s.token) guest_session_.erase(g);
  }
  by_code_.erase(s.code);
  say("host " + address_text(s.link.host) + " (join code " + show_code(s.code) + ") " + why + "; " +
      std::to_string(sessions_.size() - 1) + " host(s) now");
  sessions_.erase(it);
}

std::vector<Outgoing> Relay::tick(std::uint64_t now_ms) {
  std::vector<Outgoing> out;
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    auto next = std::next(it);
    Session& s = it->second;
    if (now_ms - s.last_ms >= config_.session_timeout_ms) {
      remove_session(it, "went quiet");
      it = next;
      continue;
    }
    for (auto f = s.flows.begin(); f != s.flows.end();) {
      auto fn = std::next(f);
      if (now_ms - f->second.last_ms >= config_.flow_idle_ms) {
        Message gone;
        gone.kind = Kind::kRelayClose;
        gone.flow = f->first;
        out.push_back({s.link, encode(gone)});
        flows_by_guest_.erase(f->second.guest);
        s.flows.erase(f);
      }
      f = fn;
    }
    for (auto g = s.guests.begin(); g != s.guests.end();) {
      auto gn = std::next(g);
      if (now_ms - g->second >= config_.guest_idle_ms) {
        auto owner = guest_session_.find(g->first);
        if (owner != guest_session_.end() && owner->second == s.token) guest_session_.erase(owner);
        s.guests.erase(g);
      }
      g = gn;
    }
    it = next;
  }
  for (auto* table : {&lookups_, &registers_}) {
    for (auto t = table->begin(); t != table->end();) {
      if (now_ms - t->second.window_start >= config_.window_ms) {
        t = table->erase(t);
      } else {
        ++t;
      }
    }
  }
  return out;
}

RelayServer::RelayServer(std::uint16_t port, std::optional<std::uint32_t> bind, RelayConfig config)
    : port_(port), bind_(bind), relay_(std::move(config)) {
  start_ms_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
}

RelayServer::~RelayServer() {
  if (socket_ != ENET_SOCKET_NULL) enet_socket_destroy(socket_);
}

std::uint64_t RelayServer::now_ms() const {
  const auto ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
  return ms - start_ms_;
}

bool RelayServer::start() {
  if (socket_ != ENET_SOCKET_NULL) return true;
  socket_ = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (socket_ == ENET_SOCKET_NULL) return false;
  ENetAddress address{};
  address.host = ENET_HOST_ANY;
  if (bind_) {
    const std::uint32_t v = *bind_;
    const unsigned char b[4] = {static_cast<unsigned char>(v >> 24), static_cast<unsigned char>(v >> 16),
                                static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v)};
    std::memcpy(&address.host, b, 4);
  }
  address.port = port_;
  if (enet_socket_bind(socket_, &address) != 0) {
    enet_socket_destroy(socket_);
    socket_ = ENET_SOCKET_NULL;
    return false;
  }
  enet_socket_set_option(socket_, ENET_SOCKOPT_NONBLOCK, 1);
  enet_socket_set_option(socket_, ENET_SOCKOPT_RCVBUF, 1 << 20);
  enet_socket_set_option(socket_, ENET_SOCKOPT_SNDBUF, 1 << 20);
  return true;
}

std::uint16_t RelayServer::port() const {
  if (socket_ == ENET_SOCKET_NULL) return 0;
  ENetAddress bound{};
  if (enet_socket_get_address(socket_, &bound) != 0) return 0;
  return bound.port;
}

void RelayServer::send(const Outgoing& out) {
  ENetAddress to{};
  to.host = out.to.host;
  to.port = out.to.port;
  ENetBuffer buffer;
  buffer.data = const_cast<std::uint8_t*>(out.bytes.data());
  buffer.dataLength = out.bytes.size();
  enet_socket_send(socket_, &to, &buffer, 1);
}

void RelayServer::service(std::uint32_t timeout_ms) {
  if (socket_ == ENET_SOCKET_NULL) return;
  enet_uint32 wait = ENET_SOCKET_WAIT_RECEIVE;
  enet_socket_wait(socket_, &wait, timeout_ms);
  std::uint8_t data[2048];
  // A bounded number per round, so the tick still runs under a flood.
  for (int i = 0; i < 4096; ++i) {
    ENetAddress from{};
    ENetBuffer buffer;
    buffer.data = data;
    buffer.dataLength = sizeof data;
    const int got = enet_socket_receive(socket_, &from, &buffer, 1);
    if (got == 0) break;
    if (got < 0) continue;  // too long, or a reset from an old guest port
    for (const Outgoing& out : relay_.received(Endpoint{from.host, from.port},
                                               std::span<const std::uint8_t>(data, static_cast<std::size_t>(got)),
                                               now_ms())) {
      send(out);
    }
  }
  const std::uint64_t now = now_ms();
  if (now - last_tick_ms_ >= 1000) {
    last_tick_ms_ = now;
    for (const Outgoing& out : relay_.tick(now)) send(out);
  }
}

}  // namespace coop::server::relay
