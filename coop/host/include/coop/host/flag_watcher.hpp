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
// - Counts (FlagStore::is_count: small keys, Gold Skulltula tokens, heart
//   pieces) add up: a player sends its change as a count delta; the host's
//   game adds a delta (handed over in a FLAG_STATE event by its client, or
//   in a FLAG_SET) and sends the total as FLAG_STATE. A total is written on
//   top of any change the game made that is not sent yet, so two players
//   gaining at once both count (docs/protocol.md, "Counts").
//
// No sockets: messages come in already received (decoded or as bytes) and go
// out encoded. receive() only queues; every read and write of the game happens
// inside frame(), on the game's thread. Not thread-safe: call both from one
// thread, or lock around them.

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <variant>
#include <utility>
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
  std::uint64_t deltas_added = 0;      // host: count deltas added to the game (in applied too)
  std::uint64_t kept_local = 0;        // snapshot entries skipped over a pending change
  std::uint64_t not_shared = 0;        // incoming entries not in the shared list
  std::uint64_t pickups_learned = 0;   // 64-bit keys taken as pickups from a message
  std::uint64_t pickup_resets = 0;     // a pickup's 0, ours or incoming: not passed on
  std::uint64_t read_failures = 0;
  std::uint64_t write_failures = 0;    // each failed try; it is tried again next frame
  std::uint64_t bad_snapshots = 0;     // parts out of order: the snapshot is dropped
  std::uint64_t ignored_messages = 0;  // not a flag message, or not for this role
  std::uint64_t load_reverts = 0;      // the game's load undid a group value: written back
};

class FlagWatcher {
 public:
  FlagWatcher(SharedFlags shared, FlagRole role) : shared_(std::move(shared)), role_(role) {}

  // A message from the server. Flag messages are queued for the next frame();
  // anything else, or a message this role does not take (a host takes no
  // FLAG_SNAPSHOT, a player no FLAG_SET), is ignored. Both take FLAG_STATE:
  // a host's client answers FLAG_SETs itself, and its game takes the answer. False when ignored or the
  // bytes do not decode.
  bool receive(const protocol::Message& message);
  bool receive(std::span<const std::uint8_t> bytes);

  // Host: send the whole table as FLAG_SNAPSHOT at the next frame (starting
  // a session, taking over as host).
  void request_snapshot() { snapshot_requested_ = true; }

  // One emulated frame: writes what came in, then compares the shared flags
  // with last frame, and returns the messages to send, in order.
  std::vector<Outgoing> frame(FlagStore& store);

  // One-time pickups (docs/items.md, "One-time pickups"): a Gold Skulltula,
  // its token, a heart container or piece is a 64-bit-key flag keyed by the
  // actor's placement hash, and no list names those keys. The game's hook
  // calls this for each such actor it sees loaded; the key is shared from
  // then on, one way only: 0 to 1 is sent and written, a 1 never goes back
  // to 0 through the session (a key that the game resets on a scene change
  // must not put the pickup back for the others). False: already a pickup.
  bool share_pickup(FlagKey key);
  // A named Bool that the game itself sets back to 0 (a room's own switch
  // state, reset when the room is left: docs/dungeon.md "Live dungeon
  // actions"): shared one way like a pickup (a 1 is sent and written, a 0 is
  // never sent and never written over a 1), and never written back after a
  // load (the room must be able to reset). False: already one way.
  bool share_one_way(FlagKey key);
  // With learning on, an incoming 64-bit key set to 1 that this game does not
  // share yet is taken as a pickup too (the player who took it was in a scene
  // this game has not loaded). At most kMaxLearned keys. Off by default here;
  // coop_hook.cpp turns it on whenever flags are shared (LICHII_COOP_PICKUPS=0
  // turns it off).
  void set_learn_pickups(bool on) { learn_ = on; }
  static constexpr std::size_t kMaxLearned = 4096;
  std::size_t pickups() const { return pickups_.size(); }

  // The 64-bit keys this watcher wrote 0 to 1 into the game for another
  // player (a chest opened, a pickup taken elsewhere), oldest first, since
  // the last call; at most kMaxWrittenForOthers are kept. A key the game
  // already had at 1 (opened here, the host's echo of it) is not in it. The
  // hook removes what is loaded of these (docs/items.md, "Chests").
  std::vector<FlagKey> take_written_for_others() { return std::exchange(written_for_others_, {}); }
  static constexpr std::size_t kMaxWrittenForOthers = 1024;

  // The host left and this player takes over, or the other way round. Keeps
  // what is known of the game; drops a snapshot being held.
  void set_role(FlagRole role);
  FlagRole role() const { return role_; }

  // The game is loading (no Link to read: the title screen, a save being
  // loaded, a scene change). A load writes the save's own values over the
  // flags, also over ones the group set just before (issue 129: a switch
  // shared by the snapshot was reset by the joiner's save load, sent back as
  // 0, and the group's later 1 was written while the load ran and lost).
  // While loading and kSettleFrames after, a shared flag that the game moves
  // away from the group's value backwards (a Bool from 1 to 0; while
  // loading only, not in the settle time, an Int back to exactly the value
  // it had before the group's value came; counts are left to their delta
  // rule) is not sent: the group's value is written back. A Bool going to 1,
  // or an Int to any other value, is the game's own change and is sent.
  void set_loading(bool loading);
  static constexpr int kSettleFrames = 120;
  bool settling() const { return loading_ || settle_left_ > 0; }

  // After a reconnect: drops anything queued or held, and the group values
  // of set_loading (the next snapshot brings them again). Local changes still
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
  void add(FlagStore& store, const protocol::FlagEntry& delta);
  void diff(FlagStore& store, Table& changes);
  void send_snapshot(FlagStore& store, std::vector<Outgoing>& out);

  // True when the entry is for a shared flag and may be applied: a pickup's
  // 0 is not, and an unknown 64-bit key set to 1 is learned first.
  bool takes(const protocol::FlagEntry& entry);
  // Records `value` as the group's, with the game's value before it.
  void note_group(const protocol::FlagEntry& value, const protocol::FlagEntry* before);

  SharedFlags shared_;
  FlagRole role_;
  std::set<FlagKey> pickups_;  // one-way 64-bit keys
  std::set<FlagKey> one_way_;  // one-way named flags, no load write-back (share_one_way)
  std::vector<FlagKey> written_for_others_;  // take_written_for_others
  // Notes a 64-bit key about to be written to 1 when the game has it at 0.
  void note_for_others(FlagStore& store, const protocol::FlagEntry& entry);
  std::size_t learned_ = 0;
  bool learn_ = false;
  std::vector<Incoming> queue_;
  Table last_;        // each shared flag as the game last had it
  Table pending_;     // player: own changes waiting for the host's answer
  Table unwritten_;   // incoming values whose write failed; tried again
  Table unadded_;     // host: count deltas whose write failed; tried again
  Table group_;         // each shared flag as the group last had it (set_loading)
  Table group_before_;  // an Int's value in our game just before group_ took it
  bool loading_ = false;
  int settle_left_ = 0;
  std::set<FlagKey> restate_;  // host: counts a delta was added to this frame
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
