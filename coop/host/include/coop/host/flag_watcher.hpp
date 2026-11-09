#pragma once

// Watches the shared flags in the running game, once per emulated frame, and
// moves them as docs/protocol.md ("Shared flags") asks:
//
// - A player's own changes go out as FLAG_SET (host: as FLAG_STATE).
// - FLAG_STATE and FLAG_SNAPSHOT from the server are written into the game,
//   and those writes are not seen as changes, so they are never sent back.
// - A FLAG_SNAPSHOT is held until its last part has come, then written all at
//   once; holding() says so, so the emulator can hold the game meanwhile.
// - A local change is kept until the host's answer for that flag comes: a
//   snapshot does not overwrite it, a FLAG_STATE does.
// - The host answers each FLAG_SET with FLAG_STATE of what came out, and sends
//   its whole table as FLAG_SNAPSHOT when asked.
//
// No sockets: messages come in already received (decoded or as bytes) and go
// out encoded. receive() only queues; every read and write of the game happens
// inside frame(), on the game's thread. Not thread-safe: call both from one
// thread, or lock around them.

#include <cstdint>
#include <map>
#include <span>
#include <variant>
#include <vector>

#include "coop/host/flags.hpp"
#include "coop/host/outgoing.hpp"
#include "coop/protocol/messages.hpp"

namespace coop::host {

enum class FlagRole : std::uint8_t {
  player,  // sends FLAG_SET, applies FLAG_STATE and FLAG_SNAPSHOT
  host,    // its game is the truth: sends FLAG_STATE, answers FLAG_SET
};

struct FlagStats {
  std::uint64_t changes_sent = 0;      // entries in FLAG_SET or own FLAG_STATE
  std::uint64_t answers_sent = 0;      // host: entries answering a FLAG_SET
  std::uint64_t applied = 0;           // entries written into the game
  std::uint64_t kept_local = 0;        // snapshot entries skipped over a pending change
  std::uint64_t not_shared = 0;        // incoming entries not in the shared list
  std::uint64_t read_failures = 0;
  std::uint64_t write_failures = 0;    // each failed try; it is tried again next frame
  std::uint64_t bad_snapshots = 0;     // parts out of order: the snapshot is dropped
  std::uint64_t ignored_messages = 0;  // not a flag message, or not for this role
};

class FlagWatcher {
 public:
  FlagWatcher(SharedFlags shared, FlagRole role) : shared_(std::move(shared)), role_(role) {}

  // A message from the server. Flag messages are queued for the next frame();
  // anything else, or a message this role does not take (a host takes no
  // FLAG_STATE, a player no FLAG_SET), is ignored. False when ignored or the
  // bytes do not decode.
  bool receive(const protocol::Message& message);
  bool receive(std::span<const std::uint8_t> bytes);

  // Host: send the whole table as FLAG_SNAPSHOT at the next frame (starting
  // a session, taking over as host).
  void request_snapshot() { snapshot_requested_ = true; }

  // One emulated frame: writes what came in, then compares the shared flags
  // with last frame, and returns the messages to send, in order.
  std::vector<Outgoing> frame(FlagStore& store);

  // The host left and this player takes over, or the other way round. Keeps
  // what is known of the game; drops a snapshot being held.
  void set_role(FlagRole role);
  FlagRole role() const { return role_; }

  // After a reconnect: drops anything queued or held. Local changes still
  // waiting for an answer are sent again at the next frame, since the answer
  // may have been lost with the connection.
  void reset();

  // A FLAG_SNAPSHOT has started and its last part has not come yet.
  bool holding() const { return holding_; }
  // Local changes sent and not answered yet.
  std::size_t pending() const { return pending_.size(); }
  const FlagStats& stats() const { return stats_; }
  const SharedFlags& shared() const { return shared_; }

 private:
  using Incoming = std::variant<protocol::FlagSet, protocol::FlagState, protocol::FlagSnapshot>;
  using Table = std::map<FlagKey, protocol::FlagEntry>;

  void handle(FlagStore& store, Incoming& message, Table& answers);
  void apply_state(FlagStore& store, const std::vector<protocol::FlagEntry>& flags);
  void apply_snapshot(FlagStore& store);
  void answer_set(FlagStore& store, const protocol::FlagSet& set, Table& answers);
  bool write(FlagStore& store, const protocol::FlagEntry& entry);
  void diff(FlagStore& store, Table& changes);
  void send_snapshot(FlagStore& store, std::vector<Outgoing>& out);

  SharedFlags shared_;
  FlagRole role_;
  std::vector<Incoming> queue_;
  Table last_;        // each shared flag as the game last had it
  Table pending_;     // player: own changes waiting for the host's answer
  Table unwritten_;   // incoming values whose write failed; tried again
  bool resend_pending_ = false;
  bool snapshot_requested_ = false;
  bool holding_ = false;
  std::uint16_t snapshot_next_part_ = 0;
  std::uint16_t snapshot_part_count_ = 0;
  Table snapshot_;                        // parts so far
  std::vector<protocol::FlagState> held_states_;  // came while holding
  FlagStats stats_;
};

}  // namespace coop::host
