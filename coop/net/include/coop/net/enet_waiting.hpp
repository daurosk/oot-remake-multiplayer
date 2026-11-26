#pragma once

// ENet's count of a peer's waiting data, kept true (issue 175).
//
// ENet adds every packet that waits in a peer's incoming queues to
// peer->totalWaitingData and refuses new packets from that peer once it
// reaches host->maximumWaitingData. ENet 1.3.18 subtracts a packet only when
// it is handed to us (enet_peer_receive). An unreliable message that never
// becomes whole (one of its fragments lost on the way) is thrown away by
// enet_peer_remove_incoming_commands without being subtracted, so over a line
// that loses packets the count only grows. At the cap ENet refuses everything
// from that peer, reliable packets too, and does not acknowledge them: the
// other side resends until its 10 s timeout and the player is dropped. A
// reconnect sets the count to 0 and it starts again.
//
// The caps stay (they bound what a peer can make us hold); after each service
// round the count is set to what really waits, so only real data counts.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>

#include <cstddef>

#include "coop/protocol/messages.hpp"

// Written against ENet 1.3.18's peer.c (its list layout, and where it adds to
// and takes from the count). A new ENet: check those again, then change this.
static_assert(ENET_VERSION == ENET_VERSION_CREATE(1, 3, 18),
              "check coop/net/enet_waiting.hpp against the new ENet's peer.c");

namespace coop::net {

// Bytes of the packets that really wait in `peer`'s queues: not yet whole or
// not yet in order on each channel, and whole ones not yet handed to us.
inline std::size_t waiting_bytes(const ENetPeer& peer) {
  std::size_t total = 0;
  auto add = [&total](const ENetList& list) {
    for (const ENetListNode* node = list.sentinel.next; node != &list.sentinel; node = node->next) {
      const auto* command = reinterpret_cast<const ENetIncomingCommand*>(node);
      if (command->packet != nullptr) total += command->packet->dataLength;
    }
  };
  for (std::size_t i = 0; i < peer.channelCount; ++i) {
    add(peer.channels[i].incomingReliableCommands);
    add(peer.channels[i].incomingUnreliableCommands);
  }
  add(peer.dispatchedCommands);
  return total;
}

// What we gave ENet for `peer` that it still holds (row 260): queued and not
// sent yet, and reliable sent but not acknowledged. ENet bounds none of it: a
// reliable send past the peer's window stays queued (protocol.c, windowSize),
// so to a peer that takes less than we send this only grows. Each command's
// fragmentLength: a whole message's length, or one fragment's.
inline std::size_t outgoing_bytes(const ENetPeer& peer) {
  std::size_t total = 0;
  auto add = [&total](const ENetList& list) {
    for (const ENetListNode* node = list.sentinel.next; node != &list.sentinel; node = node->next) {
      total += reinterpret_cast<const ENetOutgoingCommand*>(node)->fragmentLength;
    }
  };
  add(peer.outgoingCommands);
  add(peer.outgoingSendReliableCommands);
  add(peer.sentReliableCommands);
  return total;
}

// Past this much held for a peer, unreliable messages to it (samples, poses:
// the next one replaces them) are not queued; reliable ones still are. Above
// the largest backlog normal play makes: the host's whole save outbox (four
// saves of up to protocol::kMaxSaveBytes, save::kMaxOutboxParts, checked in
// save_transfer.hpp) and 2 MiB more, so puppets never stop while saves are in
// flight (review-01112-d).
inline constexpr std::size_t kSlowPeerBytes = 4 * std::size_t{protocol::kMaxSaveBytes} + (std::size_t{2} << 20);
// Past this much the peer is dropped (it reconnects; the server tells it why:
// kDisconnectBacklog): held for it any longer, the memory would only grow.
inline constexpr std::size_t kMaxOutgoingBytes = std::size_t{16} << 20;
static_assert(kSlowPeerBytes < kMaxOutgoingBytes);

// Sets peer->totalWaitingData to what really waits. Returns the bytes ENet
// had counted too many (the packets it threw away without subtracting).
inline std::size_t mend_waiting_data(ENetPeer& peer) {
  const std::size_t real = waiting_bytes(peer);
  const std::size_t extra = peer.totalWaitingData > real ? peer.totalWaitingData - real : 0;
  peer.totalWaitingData = real;
  return extra;
}

}  // namespace coop::net
