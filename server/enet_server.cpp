#include "enet_server.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

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
  address.port = options_.port;
  host_ = enet_host_create(&address, options_.max_connections,
                           protocol::kChannelCount, 0, 0);
  if (host_ == nullptr) return false;
  host_->maximumPacketSize = kMaxPacketBytes;
  host_->maximumWaitingData = kMaxWaitingBytes;
  return true;
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
  ENetEvent event;
  int result = enet_host_service(host_, &event, timeout_ms);
  while (result > 0) {
    handle(event);
    apply_actions();
    result = enet_host_check_events(host_, &event);
  }
  session_.tick(now_us());
  apply_actions();
  enet_host_flush(host_);
}

void EnetServer::handle(const ENetEvent& event) {
  switch (event.type) {
    case ENET_EVENT_TYPE_CONNECT: {
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
      session_.disconnected(id, now_us());
      event.peer->data = nullptr;
      break;
    }
    case ENET_EVENT_TYPE_NONE:
      break;
  }
}

void EnetServer::apply_actions() {
  for (Action& action : session_.take_actions()) {
    if (auto* send = std::get_if<SendAction>(&action)) {
      auto it = peers_.find(send->to);
      if (it == peers_.end()) continue;
      // Channels 0 to 2 are reliable; 3 and 4 unreliable, sequenced.
      enet_uint32 flags =
          send->channel <= protocol::kChannelOwnership ? static_cast<enet_uint32>(ENET_PACKET_FLAG_RELIABLE) : 0u;
      ENetPacket* packet = enet_packet_create(send->bytes.data(), send->bytes.size(), flags);
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
