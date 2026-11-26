#include "enet_server.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "bind_host.hpp"
#include "coop/net/enet_path.hpp"
#include "coop/net/enet_throttle.hpp"
#include "coop/net/enet_waiting.hpp"

namespace coop::server {

namespace {

// docs/protocol.md: ENet drops a peer after 10 seconds of nothing.
constexpr enet_uint32 kPeerTimeoutMs = 10'000;

// ENet would otherwise put together packets of up to 32 MB from a peer's
// fragments before we see them. No message is longer than its header and a
// u16 body length, so anything bigger is thrown away by ENet unread.
// The longest message, plus what sealing adds to it (traffic_crypto.hpp).
constexpr std::size_t kMaxPacketBytes =
    protocol::kHeaderSize + 0xFFFF + protocol::kSealOverheadBytes;
// And at most this much a peer's received data may wait to be handled.
constexpr std::size_t kMaxWaitingBytes = 1 << 20;

// The server whose service() is running: ENet's intercept callback has no
// pointer of its own, and only runs inside enet_host_service.
thread_local const EnetServer* t_serving = nullptr;

int ENET_CALLBACK intercept(ENetHost* host, ENetEvent*) {
  if (t_serving == nullptr || host->receivedData == nullptr) return 0;
  const std::span<const std::uint8_t> data(host->receivedData, host->receivedDataLength);
  return t_serving->handle_raw(host->receivedAddress, data) ? 1 : 0;
}

struct Serving {
  explicit Serving(const EnetServer* s) { t_serving = s; }
  ~Serving() { t_serving = nullptr; }
  Serving(const Serving&) = delete;
  Serving& operator=(const Serving&) = delete;
};

ConnectionId id_of(const ENetPeer* peer) {
  return static_cast<ConnectionId>(reinterpret_cast<std::uintptr_t>(peer->data));
}

}  // namespace

EnetServer::EnetServer(ServerOptions options)
    : options_(std::move(options)),
      session_(options_.session),
      start_(std::chrono::steady_clock::now()),
      delay_(options_.test_delay) {}

EnetServer::~EnetServer() {
  if (host_ != nullptr) enet_host_destroy(host_);
}

bool EnetServer::start() {
  if (host_ != nullptr) return true;
  if (!net::bind_allowed(options_.bind)) return false;  // (the test guard: no listener on all addresses)
  const ENetAddress address = local_address(options_.bind, options_.port);
  host_ = enet_host_create(&address, options_.max_connections,
                           protocol::kChannelCount, 0, 0);
  if (host_ == nullptr) return false;
  host_->maximumPacketSize = kMaxPacketBytes;
  host_->maximumWaitingData = kMaxWaitingBytes;
  host_->mtu = protocol::kDatagramBytes;  // VPN-safe datagrams (issue 143)
  host_->intercept = intercept;
  return true;
}

bool EnetServer::send_raw(const ENetAddress& to, std::span<const std::uint8_t> data) {
  if (host_ == nullptr || data.empty()) return false;
  ENetBuffer buffer;
  buffer.data = const_cast<std::uint8_t*>(data.data());
  buffer.dataLength = data.size();
  return enet_socket_send(host_->socket, &to, &buffer, 1) > 0;
}

std::uint16_t EnetServer::port() const {
  if (host_ == nullptr) return 0;
  ENetAddress bound{};
  if (enet_socket_get_address(host_->socket, &bound) != 0) return 0;
  return bound.port;
}

std::string EnetServer::admin(std::string_view line) {
  std::string out = session_.admin(line, now_us());
  apply_actions();
  return out;
}

std::uint64_t EnetServer::now_us() const {
  if (options_.now_us) return options_.now_us();
  auto elapsed = std::chrono::steady_clock::now() - start_;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
}

void EnetServer::service(std::uint32_t timeout_ms) {
  if (host_ == nullptr) return;
  Serving serving(this);
  ENetEvent event;
  int result = enet_host_service(host_, &event, timeout_ms);
  while (result > 0) {
    handle(event);
    apply_actions();
    result = enet_host_check_events(host_, &event);
  }
  release_held();
  session_.tick(now_us());
  apply_actions();
  release_held();
  // ENet forgets to take a player's lost fragments off its count; past
  // kMaxWaitingBytes it would refuse (and never acknowledge) all they send.
  std::vector<ENetPeer*> lost_full;
  std::vector<ENetPeer*> backlogged;
  for (auto& [id, peer] : peers_) {
    if (net::full_datagrams_lost(path_marks_[id], *peer, host_->serviceTime)) lost_full.push_back(peer);
    mended_bytes_ += net::mend_waiting_data(*peer);
    const std::size_t outgoing = net::outgoing_bytes(*peer);
    links_[id] = LinkSeen{peer->roundTripTime,
                          static_cast<enet_uint32>(static_cast<std::uint64_t>(peer->packetLoss) * 100 /
                                                   ENET_PEER_PACKET_LOSS_SCALE),
                          peer->totalWaitingData, outgoing};
    // Row 260: ENet holds every reliable message for a peer whose window is full, without end.
    if (outgoing > net::kMaxOutgoingBytes && std::find(lost_full.begin(), lost_full.end(), peer) == lost_full.end()) {
      backlogged.push_back(peer);
    }
  }
  for (ENetPeer* peer : backlogged) {
    ++backlog_drops_;
    drop_now(peer, net::kDisconnectBacklog, "its line took far less than we sent it: " + std::to_string(net::outgoing_bytes(*peer)) +
                          " bytes waited to go out (at most " + std::to_string(net::kMaxOutgoingBytes) + ")");
  }
  for (ENetPeer* peer : lost_full) {
    const std::uint64_t now = now_us();
    std::erase_if(small_hosts_, [now](const auto& entry) { return entry.second <= now; });
    if (net::remember_small_path(peer->address.host, remember_loopback_)) {
      small_hosts_[peer->address.host] = now + std::uint64_t{net::kRememberMs} * 1000;
    }
    send_small(peer, "its path lost our " + std::to_string(peer->mtu) +
                         "-byte datagrams while small ones got through; told it to connect again with " +
                         std::to_string(net::kSmallDatagramBytes) + "-byte ones");
  }
  enet_host_flush(host_);
}

void EnetServer::send_small(ENetPeer* peer, const std::string& why) {
  ++small_path_drops_;
  drop_now(peer, net::kDisconnectSmallDatagrams, why);
}

void EnetServer::drop_now(ENetPeer* peer, enet_uint32 code, const std::string& why) {
  const ConnectionId id = id_of(peer);
  std::string link = why;
  if (auto seen = links_.find(id); seen != links_.end()) {
    link += "; round trip " + std::to_string(seen->second.round_trip_ms) + " ms, loss " +
            std::to_string(seen->second.loss_percent) + "%";
    links_.erase(seen);
  }
  peers_.erase(id);
  path_marks_.erase(id);
  ciphers_.erase(id);
  held_.drop_if([id](const Held& h) { return h.id == id; });
  delay_.forget(in_lane(id));
  delay_.forget(out_lane(id));
  peer->data = nullptr;
  enet_peer_disconnect_now(peer, code);
  session_.disconnected(id, now_us(), link);
  apply_actions();
}

void EnetServer::handle(const ENetEvent& event) {
  switch (event.type) {
    case ENET_EVENT_TYPE_CONNECT: {
      bool tell_again = false;
      if (auto known = small_hosts_.find(event.peer->address.host);
          known != small_hosts_.end() && event.peer->mtu > net::kSmallDatagramBytes) {
        tell_again = now_us() < known->second;
        small_hosts_.erase(known);  // told at most once more
      }
      if (tell_again) {
        // Its path lost full datagrams a moment ago and it missed our note:
        // tell it again before it costs a 10 s timeout.
        ++small_path_drops_;
        event.peer->data = nullptr;
        enet_peer_disconnect_now(event.peer, net::kDisconnectSmallDatagrams);
        break;
      }
      ConnectionId id = next_id_++;
      if (!session_.connected(id, now_us(), event.peer->address.host)) {
        // Too many connections from this address without HELLO: drop it at
        // once, freeing its ENet slot. ENet reports no DISCONNECT for it.
        event.peer->data = nullptr;
        enet_peer_disconnect_now(event.peer, 0);
        break;
      }
      event.peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
      enet_peer_timeout(event.peer, 0, kPeerTimeoutMs, kPeerTimeoutMs);
      net::steady_throttle(event.peer);  // report 37: also for a game that does not ask
      peers_[id] = event.peer;
      break;
    }
    case ENET_EVENT_TYPE_RECEIVE: {
      const ConnectionId id = id_of(event.peer);
      std::span<const std::uint8_t> bytes(event.packet->data, event.packet->dataLength);
      if (options_.test_delay.on()) {
        // Opened when it is let through: the key may come with a packet held before it.
        held_.push(delay_.due(in_lane(id), now_us()),
                   Held{Held::Kind::receive, id, event.channelID, {bytes.begin(), bytes.end()}, {}});
      } else {
        receive_now(id, event.channelID, bytes);
      }
      enet_packet_destroy(event.packet);
      break;
    }
    case ENET_EVENT_TYPE_DISCONNECT: {
      ConnectionId id = id_of(event.peer);
      peers_.erase(id);
      path_marks_.erase(id);
      // (delayed: its key opens what it sent before it left, until its leave)
      if (!options_.test_delay.on()) ciphers_.erase(id);
      // As seen at the end of the last round: ENet has reset the peer by now.
      std::string link;
      if (auto seen = links_.find(id); seen != links_.end()) {
        link = "round trip " + std::to_string(seen->second.round_trip_ms) + " ms, loss " +
               std::to_string(seen->second.loss_percent) + "%, waiting " +
               std::to_string(seen->second.waiting_bytes) + " bytes";
        links_.erase(seen);
      }
      event.peer->data = nullptr;
      if (options_.test_delay.on()) {
        // After what it sent before it left; nothing more goes to it.
        held_.drop_if([id](const Held& h) { return h.id == id && h.kind >= Held::Kind::send; });
        delay_.forget(out_lane(id));
        held_.push((std::max)(now_us(), delay_.last(in_lane(id))), Held{Held::Kind::leave, id, 0, {}, std::move(link)});
        break;
      }
      session_.disconnected(id, now_us(), link);
      break;
    }
    case ENET_EVENT_TYPE_NONE:
      break;
  }
}

void EnetServer::receive_now(ConnectionId id, std::uint8_t channel, std::span<const std::uint8_t> bytes) {
  if (auto cipher = ciphers_.find(id); cipher != ciphers_.end()) {
    // Encrypted: only what opens with its key gets to the session.
    std::vector<std::uint8_t> plain;
    if (cipher->second->open(channel, bytes, plain) != protocol::OpenError::none) {
      ++unsealed_drops_;
      return;
    }
    session_.received(id, channel, plain, now_us());
    return;
  }
  session_.received(id, channel, bytes, now_us());
}

std::uint32_t packet_flags(std::uint8_t channel) {
  return channel <= protocol::kChannelOwnership
             ? static_cast<std::uint32_t>(ENET_PACKET_FLAG_RELIABLE)
             : static_cast<std::uint32_t>(ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
}

void EnetServer::apply_actions() {
  for (Action& action : session_.take_actions()) {
    if (auto* send = std::get_if<SendAction>(&action)) {
      auto it = peers_.find(send->to);
      if (it == peers_.end()) continue;
      const std::vector<std::uint8_t>* bytes = &send->bytes;
      std::optional<std::vector<std::uint8_t>> sealed;
      if (auto cipher = ciphers_.find(send->to); cipher != ciphers_.end()) {
        sealed = cipher->second->seal(send->channel, send->bytes);
        if (!sealed) continue;
        bytes = &*sealed;
      }
      if (options_.test_delay.on()) {
        held_.push(delay_.due(out_lane(send->to), now_us()),
                   Held{Held::Kind::send, send->to, send->channel, *bytes, {}});
      } else {
        send_now(it->second, send->channel, *bytes);
      }
    } else if (auto* encrypt = std::get_if<EncryptAction>(&action)) {
      if (peers_.count(encrypt->who) != 0) {
        ciphers_[encrypt->who] = std::make_unique<protocol::TrafficCipher>(encrypt->keys);
      }
      encrypt->keys.wipe();
    } else if (auto* drop = std::get_if<DisconnectAction>(&action)) {
      auto it = peers_.find(drop->who);
      if (it == peers_.end()) continue;
      // After what is queued for it (a REFUSED) has gone out.
      if (options_.test_delay.on()) {
        held_.push(delay_.due(out_lane(drop->who), now_us()), Held{Held::Kind::drop, drop->who, 0, {}, {}});
      } else {
        enet_peer_disconnect_later(it->second, 0);
      }
    }
  }
}

void EnetServer::send_now(ENetPeer* peer, std::uint8_t channel, const std::vector<std::uint8_t>& bytes) {
  const std::uint32_t flags = packet_flags(channel);
  auto seen = links_.find(id_of(peer));
  // Row 260: to a peer that has a backlog, an unreliable message (the next one replaces it) is not queued at all.
  if ((flags & ENET_PACKET_FLAG_RELIABLE) == 0 && seen != links_.end() &&
      seen->second.outgoing_bytes > net::kSlowPeerBytes) {
    ++unreliable_skipped_;
    return;
  }
  ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), flags);
  if (packet != nullptr && enet_peer_send(peer, channel, packet) != 0) {
    enet_packet_destroy(packet);
    return;
  }
  if (packet != nullptr && seen != links_.end()) seen->second.outgoing_bytes += bytes.size();
}

EnetServer::Queues EnetServer::queues() const {
  Queues q;
  q.peers = peers_.size();
  for (const auto& [id, seen] : links_) {
    if (peers_.count(id) == 0) continue;
    q.outgoing_bytes += seen.outgoing_bytes;
    q.largest_outgoing = (std::max)(q.largest_outgoing, seen.outgoing_bytes);
    q.waiting_bytes += seen.waiting_bytes;
  }
  q.unreliable_skipped = unreliable_skipped_;
  q.backlog_drops = backlog_drops_;
  return q;
}

void EnetServer::release_held() {
  if (held_.size() == 0) return;
  for (Held& h : held_.take_due(now_us())) {
    switch (h.kind) {
      case Held::Kind::receive:
        receive_now(h.id, h.channel, h.bytes);
        apply_actions();
        break;
      case Held::Kind::leave:
        delay_.forget(in_lane(h.id));
        ciphers_.erase(h.id);
        session_.disconnected(h.id, now_us(), h.link);
        apply_actions();
        break;
      case Held::Kind::send:
        if (auto it = peers_.find(h.id); it != peers_.end()) send_now(it->second, h.channel, h.bytes);
        break;
      case Held::Kind::drop:
        if (auto it = peers_.find(h.id); it != peers_.end()) enet_peer_disconnect_later(it->second, 0);
        break;
    }
  }
}

}  // namespace coop::server
