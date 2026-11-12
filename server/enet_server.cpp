#include "enet_server.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "coop/net/enet_path.hpp"
#include "coop/net/enet_waiting.hpp"

namespace coop::server {

namespace {

// docs/protocol.md: ENet drops a peer after 10 seconds of nothing.
constexpr enet_uint32 kPeerTimeoutMs = 10'000;

// ENet would otherwise put together packets of up to 32 MB from a peer's
// fragments before we see them. No message is longer than its header and a
// u16 body length, so anything bigger is thrown away by ENet unread.
constexpr std::size_t kMaxPacketBytes = protocol::kHeaderSize + 0xFFFF;
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
      start_(std::chrono::steady_clock::now()) {}

EnetServer::~EnetServer() {
  if (host_ != nullptr) enet_host_destroy(host_);
}

bool EnetServer::start() {
  if (host_ != nullptr) return true;
  ENetAddress address{};
  address.host = ENET_HOST_ANY;
  if (options_.bind) {
    const std::uint32_t v = *options_.bind;
    const unsigned char b[4] = {static_cast<unsigned char>(v >> 24), static_cast<unsigned char>(v >> 16),
                                static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v)};
    std::memcpy(&address.host, b, 4);
  }
  address.port = options_.port;
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

std::uint64_t EnetServer::now_us() const {
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
  session_.tick(now_us());
  apply_actions();
  // ENet forgets to take a player's lost fragments off its count; past
  // kMaxWaitingBytes it would refuse (and never acknowledge) all they send.
  std::vector<ENetPeer*> lost_full;
  for (auto& [id, peer] : peers_) {
    if (net::full_datagrams_lost(path_marks_[id], *peer, host_->serviceTime)) lost_full.push_back(peer);
    mended_bytes_ += net::mend_waiting_data(*peer);
    links_[id] = LinkSeen{peer->roundTripTime,
                          static_cast<enet_uint32>(static_cast<std::uint64_t>(peer->packetLoss) * 100 /
                                                   ENET_PEER_PACKET_LOSS_SCALE),
                          peer->totalWaitingData};
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
  const ConnectionId id = id_of(peer);
  ++small_path_drops_;
  std::string link = why;
  if (auto seen = links_.find(id); seen != links_.end()) {
    link += "; round trip " + std::to_string(seen->second.round_trip_ms) + " ms, loss " +
            std::to_string(seen->second.loss_percent) + "%";
    links_.erase(seen);
  }
  peers_.erase(id);
  path_marks_.erase(id);
  peer->data = nullptr;
  enet_peer_disconnect_now(peer, net::kDisconnectSmallDatagrams);
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
      peers_[id] = event.peer;
      break;
    }
    case ENET_EVENT_TYPE_RECEIVE: {
      std::span<const std::uint8_t> bytes(event.packet->data, event.packet->dataLength);
      session_.received(id_of(event.peer), event.channelID, bytes, now_us());
      enet_packet_destroy(event.packet);
      break;
    }
    case ENET_EVENT_TYPE_DISCONNECT: {
      ConnectionId id = id_of(event.peer);
      peers_.erase(id);
      path_marks_.erase(id);
      // As seen at the end of the last round: ENet has reset the peer by now.
      std::string link;
      if (auto seen = links_.find(id); seen != links_.end()) {
        link = "round trip " + std::to_string(seen->second.round_trip_ms) + " ms, loss " +
               std::to_string(seen->second.loss_percent) + "%, waiting " +
               std::to_string(seen->second.waiting_bytes) + " bytes";
        links_.erase(seen);
      }
      session_.disconnected(id, now_us(), link);
      event.peer->data = nullptr;
      break;
    }
    case ENET_EVENT_TYPE_NONE:
      break;
  }
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
      ENetPacket* packet =
          enet_packet_create(send->bytes.data(), send->bytes.size(), packet_flags(send->channel));
      if (packet != nullptr && enet_peer_send(it->second, send->channel, packet) != 0) {
        enet_packet_destroy(packet);
      }
    } else if (auto* drop = std::get_if<DisconnectAction>(&action)) {
      auto it = peers_.find(drop->who);
      if (it == peers_.end()) continue;
      // After what is queued for it (a REFUSED) has gone out.
      enet_peer_disconnect_later(it->second, 0);
    }
  }
}

}  // namespace coop::server
