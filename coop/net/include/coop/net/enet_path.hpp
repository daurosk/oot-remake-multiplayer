#pragma once

// A path that cannot carry full datagrams (issue 143).
//
// Our datagrams are protocol::kDatagramBytes (1100 bytes, 1128 on the wire),
// which fits Hamachi, Tailscale and a 1200-byte DS-Lite tunnel. A smaller
// path (a PPPoE line inside a tunnel, some mobile links, a VPN inside a VPN)
// silently loses every full datagram, while small ones (PINGs, positions)
// pass. ENet resends a lost reliable datagram as it was (a fragment keeps its
// size; repacking takes the peer's MTU at the time of the resend), so the
// channel holds until ENet's 10 s timeout and the player is dropped; the next
// connection sends the flag snapshot again and the cycle repeats every
// 15-18 s.
//
// The sign: a reliable command sent kStuckSends times or more while the other
// side still acknowledges our small packets (one sent after its last send).
// Either side that sees it ends the connection and both connect again with
// kSmallDatagramBytes, ENet's smallest
// (604 bytes on the wire, under any IPv4 path's 576-byte minimum plus a
// tunnel or two). The server tells the game with the disconnect's data
// (kDisconnectSmallDatagrams) and remembers the address, so a game that
// missed that packet is told again at its next connect. The game keeps the
// small size until it is restarted.
//
// Four sends (ENet doubles the wait each time: about 5 s at a 300 ms round
// trip, under its 10 s timeout) of one reliable command all lost by chance:
// 1 in 10^8 on a line losing 1% both ways, 1 in 77000 at 6%. When it does,
// it costs one reconnect and smaller datagrams for the session.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>

#include <cstdint>

// Written against ENet 1.3.18's protocol.c (where resends go, sendAttempts,
// lastReceiveTime). A new ENet: check those again, then change this.
static_assert(ENET_VERSION == ENET_VERSION_CREATE(1, 3, 18),
              "check coop/net/enet_path.hpp against the new ENet's protocol.c");

namespace coop::net {

// ENet's smallest datagram (ENET_PROTOCOL_MINIMUM_MTU).
inline constexpr std::uint32_t kSmallDatagramBytes = ENET_PROTOCOL_MINIMUM_MTU;
// Sends of one reliable command that mean "this size does not get through".
inline constexpr std::uint16_t kStuckSends = 4;
// The other side counts as heard when it acknowledged something this recently
// (ENet sets lastReceiveTime on ACKs).
inline constexpr std::uint32_t kHeardWithinMs = 2000;
// Disconnect data from the server: "connect again with small datagrams".
inline constexpr std::uint32_t kDisconnectSmallDatagrams = 0x534D4C4Cu;  // "SMLL"

// True when a reliable command still waiting for its ACK (sent, or queued
// again after a timeout) went kStuckSends times or more, and the other side
// acknowledged something AFTER that command's latest send: it is alive and
// gets our small packets, just not that one. A peer that died (a killed
// game) acknowledges nothing after, so it is left to ENet's timeout.
inline bool stuck_while_heard(const ENetPeer& peer) {
  bool stuck = false;
  auto scan = [&](const ENetList& list) {
    for (const ENetListNode* node = list.sentinel.next; node != &list.sentinel; node = node->next) {
      const auto* command = reinterpret_cast<const ENetOutgoingCommand*>(node);
      if ((command->command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE) != 0 &&
          command->sendAttempts >= kStuckSends &&
          static_cast<std::int32_t>(peer.lastReceiveTime - command->sentTime) > 0) {
        stuck = true;
      }
    }
  };
  scan(peer.sentReliableCommands);
  scan(peer.outgoingCommands);
  return stuck;
}

// True when `peer` uses datagrams above kSmallDatagramBytes, the other side
// acknowledged something within kHeardWithinMs of `now_ms` (ENet's clock,
// host->serviceTime), and stuck_while_heard().
inline bool full_datagrams_lost(const ENetPeer& peer, enet_uint32 now_ms) {
  if (peer.state != ENET_PEER_STATE_CONNECTED || peer.mtu <= kSmallDatagramBytes) return false;
  // ENet's clock wraps; its ENET_TIME_DIFFERENCE is not public.
  const enet_uint32 since = now_ms - peer.lastReceiveTime;
  if (peer.lastReceiveTime == 0 || since >= kHeardWithinMs) return false;
  return stuck_while_heard(peer);
}

}  // namespace coop::net
