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

// Sets peer->totalWaitingData to what really waits. Returns the bytes ENet
// had counted too many (the packets it threw away without subtracting).
inline std::size_t mend_waiting_data(ENetPeer& peer) {
  const std::size_t real = waiting_bytes(peer);
  const std::size_t extra = peer.totalWaitingData > real ? peer.totalWaitingData - real : 0;
  peer.totalWaitingData = real;
  return extra;
}

}  // namespace coop::net
