#include "relay_link.hpp"

#include <chrono>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "bind_host.hpp"

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

std::optional<RelayJoin> parse_relay_join(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  const std::size_t at = text.find('@');
  if (at == std::string_view::npos) return std::nullopt;
  const std::string_view what = text.substr(0, at);
  RelayJoin join;
  if (!parse_relay_address(std::string(text.substr(at + 1)), join.relay_host, join.relay_port)) return std::nullopt;
  if (auto code = normalize_code(what); code && what.find('_') == std::string_view::npos) join.code = *code;
  if (auto room = normalize_room(what); room && *room == what) join.room = *room;
  if (join.code.empty() && join.room.empty()) return std::nullopt;
  return join;
}

namespace {

// One lookup: three tries a second apart, from `s`. Nothing when no answer came.
std::optional<LookupResult> ask_relay(ENetSocket s, const ENetAddress& relay, const Message& lookup,
                                      const std::function<bool()>& stop) {
  const std::vector<std::uint8_t> bytes = encode(lookup);
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (stop && stop()) return std::nullopt;
    ENetBuffer out;
    out.data = const_cast<std::uint8_t*>(bytes.data());
    out.dataLength = bytes.size();
    enet_socket_send(s, &relay, &out, 1);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < until) {
      if (stop && stop()) return std::nullopt;
      enet_uint32 wait = ENET_SOCKET_WAIT_RECEIVE;
      if (enet_socket_wait(s, &wait, 100) != 0 || !(wait & ENET_SOCKET_WAIT_RECEIVE)) continue;
      std::uint8_t data[64];
      ENetAddress from{};
      ENetBuffer in;
      in.data = data;
      in.dataLength = sizeof data;
      const int got = enet_socket_receive(s, &from, &in, 1);
      if (got <= 0 || from.host != relay.host || from.port != relay.port) continue;
      const auto m = decode(std::span<const std::uint8_t>(data, static_cast<std::size_t>(got)));
      if (m && m->kind == Kind::kFound) return static_cast<LookupResult>(m->status);
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<LookupResult> lookup_on_relay(const ENetAddress& relay, const RelayJoin& join,
                                            const std::function<bool()>& stop, std::optional<std::uint32_t> bind) {
  ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) return std::nullopt;
  std::optional<LookupResult> result;
  if (bind_socket(s, bind, 0) == 0) {
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    if (!join.code.empty()) {
      Message m;
      m.kind = Kind::kLookup;
      m.code = join.code;
      result = ask_relay(s, relay, m, stop);
    }
    if (!join.room.empty() && (join.code.empty() || result == LookupResult::kUnknown)) {
      Message m;
      m.kind = Kind::kLookupName;
      m.name = join.room;
      const auto by_room = ask_relay(s, relay, m, stop);
      // An old relay does not answer the room: keep what the code got.
      if (by_room || join.code.empty()) result = by_room;
    }
  }
  enet_socket_destroy(s);
  return result;
}

namespace {

// One kDirect: sent again a second apart (three times) until the relay says something; then waits for the host's
// reply (kDirectFound comes once the host answered, or at once when the relay knows no such host) until `until`.
std::optional<Message> ask_direct(ENetSocket s, const ENetAddress& relay, const Message& ask,
                                  std::chrono::steady_clock::time_point until, const std::function<bool()>& stop) {
  const std::vector<std::uint8_t> bytes = encode(ask);
  int sent = 0;
  auto next_send = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < until) {
    if (stop && stop()) return std::nullopt;
    if (sent < 3 && std::chrono::steady_clock::now() >= next_send) {
      ENetBuffer out;
      out.data = const_cast<std::uint8_t*>(bytes.data());
      out.dataLength = bytes.size();
      enet_socket_send(s, &relay, &out, 1);
      ++sent;
      next_send += std::chrono::milliseconds(1000);
    }
    enet_uint32 wait = ENET_SOCKET_WAIT_RECEIVE;
    if (enet_socket_wait(s, &wait, 100) != 0 || !(wait & ENET_SOCKET_WAIT_RECEIVE)) continue;
    std::uint8_t data[64];
    ENetAddress from{};
    ENetBuffer in;
    in.data = data;
    in.dataLength = sizeof data;
    const int got = enet_socket_receive(s, &from, &in, 1);
    if (got <= 0 || from.host != relay.host || from.port != relay.port) continue;
    const auto m = decode(std::span<const std::uint8_t>(data, static_cast<std::size_t>(got)));
    if (m && m->kind == Kind::kDirectFound) return m;
  }
  return std::nullopt;
}

}  // namespace

DirectResult direct_on_relay(const ENetAddress& relay, const RelayJoin& join, const std::string& answer,
                             const std::function<bool()>& stop, std::uint32_t wait_ms,
                             std::optional<std::uint32_t> bind) {
  DirectResult result;
  ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) return result;
  if (bind_socket(s, bind, 0) == 0) {
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    std::optional<Message> got;
    if (!join.code.empty()) {
      Message m;
      m.kind = Kind::kDirect;
      m.code = join.code;
      m.answer = answer;
      got = ask_direct(s, relay, m, until, stop);
    }
    if (!join.room.empty() &&
        (join.code.empty() || (got && static_cast<LookupResult>(got->status) == LookupResult::kUnknown))) {
      Message m;
      m.kind = Kind::kDirect;
      m.name = join.room;
      m.answer = answer;
      if (auto by_room = ask_direct(s, relay, m, until, stop); by_room || join.code.empty()) got = by_room;
    }
    if (got) {
      result.found = static_cast<LookupResult>(got->status);
      if (result.found == LookupResult::kOk && got->ip != 0 && got->port != 0) {
        result.host = DirectAddress{got->ip, got->port};
      }
    }
  }
  enet_socket_destroy(s);
  return result;
}

namespace {

// One page of the room list (kListRooms, start) or the proofs (kListProofs): `tries` tries, `wait_ms` apart, from `s`.
std::optional<Message> ask_page(ENetSocket s, const ENetAddress& relay, std::uint8_t start,
                                const std::function<bool()>& stop, Kind kind = Kind::kListRooms, int tries = 3,
                                int wait_ms = 1000) {
  Message ask;
  ask.kind = kind;
  ask.start = start;
  const std::vector<std::uint8_t> bytes = encode(ask);
  const Kind answer = kind == Kind::kListProofs ? Kind::kRoomProofs : Kind::kRoomList;
  for (int attempt = 0; attempt < tries; ++attempt) {
    if (stop && stop()) return std::nullopt;
    ENetBuffer out;
    out.data = const_cast<std::uint8_t*>(bytes.data());
    out.dataLength = bytes.size();
    enet_socket_send(s, &relay, &out, 1);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < until) {
      if (stop && stop()) return std::nullopt;
      enet_uint32 wait = ENET_SOCKET_WAIT_RECEIVE;
      if (enet_socket_wait(s, &wait, 100) != 0 || !(wait & ENET_SOCKET_WAIT_RECEIVE)) continue;
      std::uint8_t data[2048];
      ENetAddress from{};
      ENetBuffer in;
      in.data = data;
      in.dataLength = sizeof data;
      const int got = enet_socket_receive(s, &from, &in, 1);
      if (got <= 0 || from.host != relay.host || from.port != relay.port) continue;
      auto m = decode(std::span<const std::uint8_t>(data, static_cast<std::size_t>(got)));
      if (m && m->kind == answer && (answer == Kind::kRoomProofs || m->start == start || m->total <= start)) return m;
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<RoomList> list_rooms_on_relay(const ENetAddress& relay, const std::function<bool()>& stop,
                                            bool ask_proofs, std::optional<std::uint32_t> bind) {
  ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) return std::nullopt;
  std::optional<RoomList> result;
  if (bind_socket(s, bind, 0) == 0) {
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
    for (int page = 0; page < 8; ++page) {
      const std::size_t start = result ? result->rooms.size() : 0;
      if (result && start >= result->total) break;
      const auto m = ask_page(s, relay, static_cast<std::uint8_t>(start), stop);
      if (!m) break;  // (the rooms so far: a later page lost, or the list got shorter)
      if (!result) result.emplace();
      result->total = m->total;
      if (m->rooms.empty()) break;
      result->rooms.insert(result->rooms.end(), m->rooms.begin(), m->rooms.end());
    }
    // The proofs: two tries 500 ms apart (one lost datagram does not cost the mark; a relay before 0.11.8 never
    // answers, and the list is shown without them).
    if (ask_proofs && result && !result->rooms.empty()) {
      if (const auto p = ask_page(s, relay, 0, stop, Kind::kListProofs, 2, 500)) {
        result->proofs = p->proofs;
        result->proofs_answered = true;
      }
    }
  }
  enet_socket_destroy(s);
  return result;
}

std::string lookup_text(std::optional<LookupResult> result, const RelayJoin& join) {
  const std::string relay = join.relay_host + ":" + std::to_string(join.relay_port);
  const bool room = join.code.empty();
  if (!result) {
    return "no answer from the relay " + relay + ": is the address right, and is the relay running?" +
           (room ? std::string(" (a relay from before room names does not answer room names: ask the host "
                               "for the join code)")
                 : std::string());
  }
  switch (*result) {
    case LookupResult::kOk:
      if (!join.room.empty() && !join.code.empty()) return "the relay let this game in: connecting through it";
      return room ? "room \"" + join.room + "\" found on the relay: connecting through it"
                  : "the relay took the join code: connecting through it";
    case LookupResult::kUnknown:
      if (!join.room.empty() && !join.code.empty()) {
        return "the relay knows no join code or room \"" + join.room +
               "\" right now: check it with the host (a join code changes each time the host starts; a room is free "
               "again 30 seconds after its host stops)";
      }
      if (!join.room.empty()) {
        return "the relay has no room \"" + join.room +
               "\" right now: check the name with the host (a room is free again 30 seconds after its host stops)";
      }
      return "the relay does not know this join code: it changes each time the host starts, and ends when the "
             "host stops";
    case LookupResult::kSlowDown:
      return "the relay had too many wrong codes or room names from your address: wait 10 minutes";
    case LookupResult::kFull:
      return "that host already has the most players the relay lets in";
  }
  return "the relay refused";
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
  if (!config_.room.empty()) {
    const auto room = normalize_room(config_.room);
    if (!room) {
      error = "'" + config_.room + "' is not a room name: 3 to 24 letters, digits, - or _";
      return false;
    }
    config_.room = *room;
  }
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
  if (bind_socket(socket_, config_.bind, 0) != 0) {
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
    case Kind::kRegisteredName:
      if (m.token != token_) return;
      last_heard_ms_ = now;
      refused_hard_ = false;
      if (m.kind == Kind::kRegisteredName && m.name != room_) {
        room_ = m.name;
        last_list_ms_ = 0;  // (listed at once with the room)
        if (config_.on_room) config_.on_room(room_);
      }
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
        room_.clear();
        last_list_ms_ = 0;
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
        if (bind_socket(flow.socket, net::kLoopbackIp, 0) != 0) {
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
    case Kind::kDirectAnswer: {
      // A guest's answer code (the direct way): the host's server checks it; only a code made with the join
      // password gets this host's address back.
      if (m.token != token_) return;
      last_heard_ms_ = now;
      Message reply;
      reply.kind = Kind::kDirectReply;
      reply.token = token_;
      reply.ticket = m.ticket;
      if (config_.direct) {
        if (const auto at = config_.direct(m.answer)) {
          reply.ip = at->ip;
          reply.port = at->port;
        }
      }
      say(reply.ip != 0 ? "a guest asked for the direct way: answer code taken, punching toward them"
                        : "a guest asked for the direct way: not taken, they come through the relay");
      send_relay(reply);
      return;
    }
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
      m.kind = config_.room.empty() ? Kind::kRegister : Kind::kRegisterName;
      m.token = token_;
      m.key = config_.key;
      m.name = config_.room;
      send_relay(m);
    }
    if (!refused_hard_ && now - first_try_ms_ >= 10'000 &&
        (last_waiting_note_ms_ == 0 || now - last_waiting_note_ms_ >= 30'000)) {
      last_waiting_note_ms_ = now;
      say("no answer from the relay at " + config_.relay_host + ":" + std::to_string(config_.relay_port) +
          " yet: is it running, and is its UDP port open to the internet?" +
          (config_.room.empty() ? std::string()
                                : std::string(" A relay from before room names does not answer a host with one "
                                              "(update it, or host without a room name).")) +
          " Still trying.");
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
  // The room list: listed (or not) again with each keepalive, so a restarted
  // relay has it back and the players' number stays right.
  if (config_.listing && !room_.empty() && (last_list_ms_ == 0 || now - last_list_ms_ >= kKeepaliveEveryMs)) {
    last_list_ms_ = now;
    const std::optional<RoomEntry> entry = config_.listing();
    Message m;
    m.kind = Kind::kListRoom;
    m.token = token_;
    m.listed = entry.has_value();
    if (entry) {
      m.entry = *entry;
      m.entry.host = listing_name(m.entry.host);
    }
    send_relay(m);
    if (entry && config_.proof) {
      if (const std::optional<RoomProof> proof = config_.proof(room_, m.entry)) {
        Message p;
        p.kind = Kind::kRoomProof;
        p.token = token_;
        p.proof = *proof;
        send_relay(p);
      }
    }
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
