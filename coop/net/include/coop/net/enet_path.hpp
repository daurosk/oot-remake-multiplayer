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
// The sign: a reliable command too big for a small datagram, sent
// kStuckSends times and then once more, unacknowledged after each of those
// last two sends while packets we sent later were acknowledged (PathWatch).
// Either side that sees it ends the connection and both connect again with
// kSmallDatagramBytes, ENet's smallest (604 bytes on the wire, under any IPv4 path's 576-byte minimum plus a
// tunnel or two). The server tells the game with the disconnect's data
// (kDisconnectSmallDatagrams) and remembers the address, so a game that
// missed that packet is told again at its next connect (within
// kRememberMs, once). The game keeps the small size until it is restarted.
//
// Plain loss or an outage (Wi-Fi) also leaves commands unacknowledged after
// several sends, but: a small one (PINGs, single flags) does not count at
// all; once one full-size command got through on a connection (the join snapshot
// does at once on a line that carries full datagrams) nothing counts for
// the rest of it; and before that a big one must also stay stuck through
// one more send after the line is back. A false switch needs a line that
// loses the join snapshot's first datagrams four or more times running, and
// costs one reconnect.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "coop/net/path_mark.hpp"

// Written against ENet 1.3.18's protocol.c (where resends go, sendAttempts,
// lastReceiveTime). A new ENet: check those again, then change this.
static_assert(ENET_VERSION == ENET_VERSION_CREATE(1, 3, 18),
              "check coop/net/enet_path.hpp against the new ENet's protocol.c");

namespace coop::net {

// ENet's smallest datagram (ENET_PROTOCOL_MINIMUM_MTU).
inline constexpr std::uint32_t kSmallDatagramBytes = ENET_PROTOCOL_MINIMUM_MTU;
// Sends of one reliable command after which it is marked as stuck; it counts
// when sent once more and still stuck (PathWatch).
inline constexpr std::uint16_t kStuckSends = 3;
// The other side counts as heard when it acknowledged something this recently
// (ENet sets lastReceiveTime on ACKs).
inline constexpr std::uint32_t kHeardWithinMs = 2000;
// How long the server remembers an address it sent away, for a game that
// missed the note: it is told once more at its next connect within this.
inline constexpr std::uint32_t kRememberMs = 60'000;
// Disconnect data from the server: "connect again with small datagrams".
inline constexpr std::uint32_t kDisconnectSmallDatagrams = 0x534D4C4Cu;  // "SMLL"

// True when a datagram of `command` alone (ENet header, its command header
// and its data) is larger than kSmallDatagramBytes: resending it at a small
// MTU could not help, and only such a command shows the path is too small.
inline std::size_t datagram_bytes(const ENetOutgoingCommand& command) {
  const bool fragment = (command.command.header.command & ENET_PROTOCOL_COMMAND_MASK) ==
                        ENET_PROTOCOL_COMMAND_SEND_FRAGMENT;
  return sizeof(ENetProtocolHeader) +
         (fragment ? sizeof(ENetProtocolSendFragment) : sizeof(ENetProtocolSendReliable)) +
         command.fragmentLength;
}
inline bool too_big_for_small(const ENetOutgoingCommand& command) {
  return datagram_bytes(command) > kSmallDatagramBytes;
}

// Within this of the peer's datagram size a command counts as full size (a
// full fragment is exactly it; ENet may add a few bytes of header).
inline constexpr std::size_t kFullSizeSlack = 32;

// True when a datagram of `command` alone is about as large as `peer`'s
// datagrams: one of these acknowledged proves the path carries full ones.
// A smaller one (the last piece of a message, a 600-900 byte message) does
// not: a 900-byte path carries those and still loses every full piece.
inline bool full_size(const ENetOutgoingCommand& command, const ENetPeer& peer) {
  return datagram_bytes(command) + kFullSizeSlack >= peer.mtu;
}

// A reliable command too big for a small datagram, still waiting for its ACK
// (sent, or queued again after a timeout), sent kStuckSends times or more,
// while the other side acknowledged something well AFTER that command's
// latest send: it is alive and gets our later packets, just not that one. A peer
// that died (a killed game) acknowledges nothing after, so it is left to
// ENet's timeout.
inline std::optional<StuckCommand> stuck_while_heard(const ENetPeer& peer) {
  // "After": later than an ACK of the send itself could take, so ACKs of
  // packets sent just before it (still on the way) do not count.
  const auto margin = static_cast<std::int32_t>(2 * peer.roundTripTime + 4 * peer.roundTripTimeVariance + 10);
  std::optional<StuckCommand> found;
  auto scan = [&](const ENetList& list) {
    for (const ENetListNode* node = list.sentinel.next; node != &list.sentinel; node = node->next) {
      const auto* command = reinterpret_cast<const ENetOutgoingCommand*>(node);
      if ((command->command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE) != 0 &&
          command->sendAttempts >= kStuckSends && too_big_for_small(*command) &&
          static_cast<std::int32_t>(peer.lastReceiveTime - command->sentTime) > margin &&
          (!found || command->sendAttempts > found->sends)) {
        found = StuckCommand{command->command.header.channelID, command->reliableSequenceNumber,
                             command->sendAttempts};
      }
    }
  };
  scan(peer.sentReliableCommands);
  scan(peer.outgoingCommands);
  return found;
}

// True while ENet still waits for the ACK of the command `mark` names.
inline bool still_waiting(const ENetPeer& peer, const StuckCommand& mark) {
  bool waiting = false;
  auto scan = [&](const ENetList& list) {
    for (const ENetListNode* node = list.sentinel.next; node != &list.sentinel; node = node->next) {
      const auto* command = reinterpret_cast<const ENetOutgoingCommand*>(node);
      if ((command->command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE) != 0 &&
          command->command.header.channelID == mark.channel &&
          command->reliableSequenceNumber == mark.sequence) {
        waiting = true;
      }
    }
  };
  scan(peer.sentReliableCommands);
  scan(peer.outgoingCommands);
  return waiting;
}

// Updates mark.big_acked: true once a full-size command sent on this
// connection left ENet's lists (acknowledged) while the peer stayed connected.
inline void note_big_acks(PathMark& mark, const ENetPeer& peer) {
  if (mark.big_acked) return;
  std::vector<std::uint32_t> now;
  auto scan = [&](const ENetList& list) {
    for (const ENetListNode* node = list.sentinel.next; node != &list.sentinel; node = node->next) {
      const auto* command = reinterpret_cast<const ENetOutgoingCommand*>(node);
      if ((command->command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE) != 0 &&
          command->sendAttempts > 0 && full_size(*command, peer)) {
        now.push_back(std::uint32_t{command->command.header.channelID} << 16 |
                      command->reliableSequenceNumber);
      }
    }
  };
  scan(peer.sentReliableCommands);
  scan(peer.outgoingCommands);
  std::sort(now.begin(), now.end());
  mark.big_acked = !std::includes(now.begin(), now.end(), mark.big_waiting.begin(), mark.big_waiting.end());
  mark.big_waiting = mark.big_acked ? std::vector<std::uint32_t>{} : std::move(now);
}

// Called for each peer after each service round, with that peer's mark
// (a new one for each connection).
//
// Plain loss or an outage (Wi-Fi) can also lose every send of a big command
// and then let later packets through. Two things keep that from counting:
// - once a full-size command was acknowledged on this connection, the path
//   carries full datagrams, and nothing counts any more (a small path never
//   gets one through);
// - a stuck command is only marked first, and counts once the SAME command
//   was sent again after the mark and that send too stayed unacknowledged
//   while later packets were acknowledged (an outage's next resend usually
//   gets through).
//
// True when `peer` uses datagrams above kSmallDatagramBytes, no big command
// got through yet, the other side acknowledged something within
// kHeardWithinMs of `now_ms` (ENet's clock, host->serviceTime), and the
// marked command was sent again and is still stuck after that send.
inline bool full_datagrams_lost(PathMark& mark, const ENetPeer& peer, enet_uint32 now_ms) {
  if (peer.state != ENET_PEER_STATE_CONNECTED || peer.mtu <= kSmallDatagramBytes) {
    mark.marked.reset();
    return false;
  }
  note_big_acks(mark, peer);
  if (mark.big_acked) {
    mark.marked.reset();
    return false;
  }
  // ENet's clock wraps; its ENET_TIME_DIFFERENCE is not public.
  const enet_uint32 since = now_ms - peer.lastReceiveTime;
  const std::optional<StuckCommand> stuck =
      peer.lastReceiveTime != 0 && since < kHeardWithinMs ? stuck_while_heard(peer) : std::nullopt;
  if (!stuck) {
    // Not stuck now: acknowledged at last, or not heard after its last send
    // yet (or the peer is gone: ENet's timeout). The mark stays only while
    // its command waits.
    if (mark.marked && !still_waiting(peer, *mark.marked)) mark.marked.reset();
    return false;
  }
  if (mark.marked && mark.marked->channel == stuck->channel &&
      mark.marked->sequence == stuck->sequence) {
    return stuck->sends > mark.marked->sends;
  }
  mark.marked = stuck;
  return false;
}

}  // namespace coop::net
