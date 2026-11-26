#pragma once

// ENet's packet throttle, held open (looked at for player report #37, lag at
// 150-350 ms ping; that report's main cause was the show delay,
// coop/presence/show_delay.hpp).
//
// ENet keeps a throttle per peer (0 to ENET_PEER_PACKET_THROTTLE_SCALE). Each
// time a round trip comes back more than twice its variance above the lowest
// of the last interval, it takes `deceleration` off (default 2 of 32); a round
// trip at or under that lowest one adds `acceleration`. A lowered throttle
// does two things (ENet 1.3.18 protocol.c, enet_protocol_check_outgoing_commands):
//   - an unreliable packet (poses, samples, enemy samples, voice) is thrown
//     away before it is sent unless a counter stepping 7 of 32 a packet is at
//     or under the throttle;
//   - the reliable data in flight is held to throttle/32 of the window (at
//     least one datagram), so under a big burst reliable messages queue.
// Our traffic is small next to any home line, so the throttle only costs us.
// Measured (tests/net/lag_path_test.cpp, 62 ms + 0-60 ms jitter each way):
// it threw away 0.5 % of the poses, 1.7 % with a 400 ms stall every 5 s;
// none when held open. Reliable messages were prompt either way. A busy PC
// (round trips jumping) costs more (reports/s0-tool-loss.md).
// steady_throttle sets deceleration 0 (it never falls) and acceleration to
// the whole scale (back to full at the first good round trip). ENet also
// sends the setting to the other side, which takes it for its own peer of
// us, so a game fixes both directions of its link to any server, and a new
// server does the same for an older game. An old game and an old server
// still work together and with new ones (THROTTLE_CONFIGURE is in every ENet).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>

// Written against ENet 1.3.18's peer.c (enet_peer_throttle) and protocol.c
// (where the throttle drops and holds). A new ENet: check those again.
static_assert(ENET_VERSION == ENET_VERSION_CREATE(1, 3, 18),
              "check coop/net/enet_throttle.hpp against the new ENet's peer.c and protocol.c");

namespace coop::net {

inline void steady_throttle(ENetPeer* peer) {
  if (peer == nullptr) return;
  enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL,
                               ENET_PEER_PACKET_THROTTLE_SCALE, 0);
  peer->packetThrottle = peer->packetThrottleLimit;
}

}  // namespace coop::net
