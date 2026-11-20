#include "coop/host/flag_watcher.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "coop/host/game_items.hpp"
#include "coop/host/map_drop_log.hpp"
#include "coop/protocol/codec.hpp"

namespace coop::host {
namespace {

using protocol::FlagEntry;

void push(std::vector<Outgoing>& out, const protocol::Message& message) {
  if (auto bytes = protocol::encode(message)) {
    out.push_back({protocol::channel_of(message), std::move(*bytes)});
  }
}

bool is_delta(const FlagEntry& e) { return e.kind == protocol::FlagKind::count_delta; }

// Saved progress going back: a Bool from set to 0, an Int lower.
bool went_back(const FlagEntry& before, const FlagEntry& now) {
  if (before.kind != now.kind) return false;
  if (now.kind == protocol::FlagKind::boolean) return before.value != 0 && now.value == 0;
  if (now.kind == protocol::FlagKind::int32) {
    return static_cast<std::int32_t>(now.value) < static_cast<std::int32_t>(before.value);
  }
  return false;
}

// The game's value `now` is older than the group's (FlagWatcher::set_loading):
// a Bool back from 1 to 0, or, while there is no Link at all (`loading`),
// an Int (not a count) back to exactly the value it held before the group's
// value came (`before`). Any other Int change is the game's own (a trade
// item handed on in the settle time, more progress in the save) and goes out.
bool backwards(const FlagEntry& group, const FlagEntry* before, const FlagEntry& now,
               bool loading) {
  if (group.kind == protocol::FlagKind::boolean) {
    return group.value != 0 && now.value == 0;
  }
  return loading && group.kind == protocol::FlagKind::int32 && before != nullptr &&
         now == *before && now != group;
}

std::int64_t as_int(std::uint32_t bits) { return static_cast<std::int32_t>(bits); }

// A max-rule value not written yet (held, or refused) that is above the
// game's own new value: the game's change does not replace it (issue 193).
bool held_is_higher(FlagStore& store, const protocol::FlagEntry& held, const protocol::FlagEntry& now) {
  return held.kind == protocol::FlagKind::int32 && now.kind == protocol::FlagKind::int32 &&
         store.is_max(held.key) && as_int(held.value) > as_int(now.value);
}

std::uint32_t as_bits(std::int64_t v) {
  v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                               std::numeric_limits<std::int32_t>::max());
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
}

// FLAG_SET or FLAG_STATE messages for `table`, at most kMaxFlagEntries each.
// A message the codec refuses (an entry this kind cannot carry, such as a
// count delta in a FLAG_STATE) is sent again without the entries it refuses,
// so one bad entry never takes the others with it; `unsent` counts those.
template <typename M>
void push_list(std::vector<Outgoing>& out, const std::map<FlagKey, FlagEntry>& table,
               std::uint64_t& unsent) {
  auto it = table.begin();
  while (it != table.end()) {
    M message;
    message.player_number = protocol::kPlayerSelf;
    while (it != table.end() && message.flags.size() < protocol::kMaxFlagEntries) {
      message.flags.push_back(it->second);
      ++it;
    }
    if (protocol::encode(message)) {
      push(out, message);
      continue;
    }
    M good;
    good.player_number = message.player_number;
    for (const FlagEntry& e : message.flags) {
      M one;
      one.player_number = message.player_number;
      one.flags.push_back(e);
      if (protocol::encode(one)) {
        good.flags.push_back(e);
      } else {
        ++unsent;
      }
    }
    if (!good.flags.empty()) push(out, good);
  }
}

}  // namespace

bool FlagWatcher::receive(const protocol::Message& message) {
  if (const auto* set = std::get_if<protocol::FlagSet>(&message)) {
    if (role_ == FlagRole::host) {
      queue_.emplace_back(*set);
      return true;
    }
  } else if (const auto* state = std::get_if<protocol::FlagState>(&message)) {
    // Both roles: a host gets FLAG_STATE for its own game too (its client's
    // answer to a FLAG_SET, the server's value on a clash).
    queue_.emplace_back(*state);
    return true;
  } else if (const auto* snapshot = std::get_if<protocol::FlagSnapshot>(&message)) {
    if (role_ == FlagRole::player) {
      queue_.emplace_back(*snapshot);
      return true;
    }
  } else if (const auto* reset = std::get_if<protocol::FlagReset>(&message)) {
    if (role_ == FlagRole::player) {  // a host sent its own
      queue_.emplace_back(*reset);
      return true;
    }
  } else if (const auto* origin = std::get_if<protocol::FlagOrigin>(&message)) {
    queue_.emplace_back(*origin);  // both roles: who asked for the next answer
    return true;
  }
  ++stats_.ignored_messages;
  return false;
}

bool FlagWatcher::receive(std::span<const std::uint8_t> bytes) {
  protocol::Message message;
  if (protocol::decode(bytes, message) != protocol::Error::none) {
    ++stats_.ignored_messages;
    return false;
  }
  return receive(message);
}

bool FlagWatcher::share_one_way(FlagKey key) {
  if (key.kind != FlagKeyKind::named || !one_way_.insert(key).second) return false;
  shared_.add(key);
  return true;
}

bool FlagWatcher::share_down_only(FlagKey key) {
  if (key.kind != FlagKeyKind::named || up_only_.contains(key) || !down_only_.insert(key).second) return false;
  shared_.add(key);
  return true;
}

bool FlagWatcher::share_up_only(FlagKey key) {
  if (key.kind != FlagKeyKind::named || down_only_.contains(key) || !up_only_.insert(key).second) return false;
  shared_.add(key);
  return true;
}

bool FlagWatcher::keeps_own(FlagStore& store, const FlagEntry& entry) {
  if (entry.kind != protocol::FlagKind::boolean) return false;
  const FlagKey key = key_of(entry);
  const bool down = entry.value != 0 && down_only_.contains(key);  // a 1 over our 0
  const bool up = entry.value == 0 && up_only_.contains(key);      // a 0 over our 1
  if (!down && !up) return false;
  std::optional<FlagEntry> now = store.read(key);
  if (!now || now->kind != protocol::FlagKind::boolean || (now->value != 0) != up) return false;
  now->key = key;
  ++(down ? stats_.down_only_kept : stats_.up_only_kept);
  note_group(*now, nullptr);  // a load's write-back puts our value back, not the group's
  return true;
}

bool FlagWatcher::share_pickup(FlagKey key) {
  if (key.kind != FlagKeyKind::key64 || !pickups_.insert(key).second) return false;
  shared_.add(key);
  return true;
}

void FlagWatcher::guard_count(FlagKey count, std::span<const FlagKey> sources) {
  guards_.try_emplace(count);
  for (const FlagKey source : sources) {
    share_pickup(source);
    guard_source_[source] = count;
  }
}

void FlagWatcher::expect_doubled_gain() {
  if (doubled_ == 0 || frames_ > doubled_until_) doubled_ = 0;
  doubled_ = std::min(doubled_ + 1, kMaxDoubledGains);
  doubled_until_ = frames_ + kDoubledGainFrames;
}

bool FlagWatcher::share_pickup_named(FlagKey key) {
  if (key.kind != FlagKeyKind::named || !pickups_.insert(key).second) return false;
  shared_.add(key);
  return true;
}

void FlagWatcher::note_for_others(FlagStore& store, const FlagEntry& entry) {
  const FlagKey key = key_of(entry);
  const bool noted = key.kind == FlagKeyKind::key64 || pickups_.contains(key);
  if (!noted || entry.kind != protocol::FlagKind::boolean || entry.value == 0) return;
  const auto source = guard_source_.find(key);
  if (source == guard_source_.end() && written_for_others_.size() >= kMaxWrittenForOthers) return;
  const std::optional<FlagEntry> before = store.read(key);
  if (before && before->value != 0) return;  // the game has it already (set here)
  // Another player took it, live (a join snapshot is no take now): our game
  // counting its pickup again soon after is a second count (guard_count).
  if (source != guard_source_.end() && received_kind_ == ReceivedKind::live) {
    guards_[source->second].others_at = frames_ + 1;  // never 0
  }
  if (written_for_others_.size() < kMaxWrittenForOthers) written_for_others_.push_back(key);
}

bool FlagWatcher::takes(const FlagEntry& entry) {
  const FlagKey key = key_of(entry);
  const bool one = entry.kind == protocol::FlagKind::boolean && entry.value != 0;
  if (!shared_.contains(key)) {
    if (!learn_ || !one || key.kind != FlagKeyKind::key64 || learned_ >= kMaxLearned) {
      ++stats_.not_shared;
      return false;
    }
    ++learned_;
    ++stats_.pickups_learned;
    share_pickup(key);
    return true;
  }
  if (!one && (pickups_.contains(key) || one_way_.contains(key))) {
    ++stats_.pickup_resets;
    return false;
  }
  return true;
}

void FlagWatcher::set_role(FlagRole role) {
  role_ = role;
  holding_ = false;
  snapshot_.clear();
  held_states_.clear();
  if (role_ == FlagRole::host) {
    // The host's game is the truth now: nothing waits for an answer.
    pending_.clear();
    resend_pending_ = false;
  }
}

void FlagWatcher::set_loading(bool loading) {
  link_loading_ = loading;
  if (native_forced_) return;  // a load node's load holds it (frame())
  if (loading) {
    loading_ = true;
    settle_left_ = kSettleFrames;
  } else {
    loading_ = false;
  }
}

void FlagWatcher::note_group(const FlagEntry& value, const FlagEntry* before) {
  const FlagKey key = key_of(value);
  group_[key] = value;
  if (before != nullptr && *before != value) {
    group_before_[key] = *before;
  } else {
    group_before_.erase(key);
  }
}

void FlagWatcher::reset() {
  queue_.clear();
  reset_holds_.clear();  // rows 136/137: the old session's host's save
  cap_holds_.clear();    // row 181: the old session's caps
  // Row 140: what was left to the game was the old session's group's.
  game_owned_.clear();
  put_back_.clear();
  owed_.clear();
  owed_due_ = false;
  // Another session, or the same server started again: what the group had
  // comes again with the next snapshot.
  group_.clear();
  group_before_.clear();
  group_known_ = false;
  holding_ = false;
  snapshot_.clear();
  held_states_.clear();
  origin_.clear();
  received_.clear();
  // Guards: what others took and our unmatched sources belong to the old
  // session. A held gain stays held, so it is still sent or put back.
  for (auto& [key, g] : guards_) {
    g.credit = 0;
    g.others_at = 0;
    g.put_back = 0;
  }
  raced_.clear();
  resend_pending_ = role_ == FlagRole::player && !pending_.empty();
  check_offline_ = true;  // the title seen meanwhile (set_title) is looked at by the next frame()
}

std::vector<Outgoing> FlagWatcher::frame(FlagStore& store) {
  const MapDropBecause because("the flag watcher");  // (a write of ours in the drop log, minimap-b)
  std::vector<Outgoing> out;
  const unsigned step = ticks_.step();  // 1/60 s that passed (issue 106)
  frames_ += step;

  // Writes that failed before go first, so newer incoming values win. One
  // whose flag the game has changed by itself since is dropped instead: the
  // game's change is newer, and the diff below sends it.
  std::erase_if(unwritten_exact_, [this](const FlagKey& key) { return !unwritten_.contains(key); });
  const Table retry = unwritten_;
  for (const auto& [key, entry] : retry) {
    // A count total is written on top of the game's own change (write()),
    // so it is never dropped for one.
    if (const auto known = last_.find(key); known != last_.end() && !store.is_count(key)) {
      std::optional<FlagEntry> now = store.read(key);
      if (now) {
        now->key = key;
        if (*now != known->second && !held_is_higher(store, entry, *now)) {
          unwritten_.erase(key);
          continue;
        }
      }
    }
    if (unwritten_exact_.contains(key)) {
      write_exact(store, entry);
    } else {
      write(store, entry);
    }
  }
  const auto deltas = std::move(unadded_);
  unadded_.clear();
  for (const auto& [key, entry] : deltas) add(store, entry);

  Table answers;  // host: FLAG_STATE entries answering a FLAG_SET
  auto queue = std::move(queue_);
  queue_.clear();
  for (auto& message : queue) handle(store, message, answers);
  if (holding_) {
    stats_.holding_frames += step;
    if (frames_ - holding_since_ > kHoldMaxFrames) {
      // Rows 106/107: the snapshot's last part never came. Held, this game
      // would send none of its changes and write none of the group's for as
      // long as the connection lasts; dropped, play goes on (the hook says so
      // and points at Resync from host for the host's values).
      ++stats_.hold_timeouts;
      hold_timed_out_ = true;
      drop_snapshot(store);
    }
  }

  std::vector<Outgoing> resets;
  if (std::exchange(check_offline_, false) && !loading_ && title_seen_ && role_ == FlagRole::player &&
      !window_ && !last_.empty()) {
    // A connection's first frame, Link in the game, and the title was up
    // while we were not connected (set_title): our game may have loaded
    // another save meanwhile, which no load window saw (frame() does not run
    // then). What it changed since our last frame goes back to that (the
    // group's then), as a guest's save load does: it is never sent as our
    // progress (player report #96). With no Link now the load window below
    // takes the title instead.
    ++stats_.offline_loads;
    moved_ = true;
    end_window(store, resets);
    moved_ = false;
  }
  // The game's own load nodes (set_native_loads): from a start until the save is in, the game is loading.
  if (native_loading_ && frames_ - native_since_ > kNativeLoadMaxFrames) {
    native_loading_ = false;
    ++stats_.native_timeouts;
  }
  if (native_loading_ || native_loaded_) {
    // Loading whatever Link does (set_loading's own value is kept for after it).
    native_forced_ = true;
    loading_ = true;
    settle_left_ = kSettleFrames;
  } else if (std::exchange(native_forced_, false)) {
    const bool link = link_loading_;
    set_loading(link);
  }
  if (settling() && !window_) {
    window_ = true;
    // Row 140: a room or scene load (also the game's own load nodes) reads the game's own copies in again (the HUD
    // screen's floors): a value left to the game may take a put-back again (owed_, when the window ends).
    game_owned_.clear();
    put_back_.clear();
    window_since_ = frames_;
    old_counted_ = false;
    window_retry_ = false;
    moved_ = false;
    window_title_ = false;
    back_keys_.clear();
    no_scene_frames_ = 0;
  } else if (!settling() && window_) {
    window_ = false;
    // Rows 136/137: the game made Link (and its other live copies) again from the flags in this window.
    reset_holds_.clear();
    cap_holds_.clear();  // row 181: the game made Link's Life component from MaxLife too
    // Row 140: a save load (moved_) is another save: its floors are not owed the old one's.
    if (moved_) {
      owed_.clear();
    } else {
      owed_due_ = !owed_.empty();
    }
    end_window(store, resets);
  }
  // Out of every load window with Link: what last_ holds from here is a save's (world-fails-0115).
  if (!window_ && !settling()) played_ = true;
  if (window_ && loading_ && !scene_readable_) no_scene_frames_ += static_cast<int>(step);
  if (window_) stats_.window_frames += step;
  if (window_ && title_seen_) window_title_ = true;
  see_save_load();
  if (std::exchange(native_loaded_, false)) {
    // The save is in: the save load, said by the game itself (the window is open: set_loading above).
    native_loading_ = false;
    ++stats_.native_loads;
    if (std::exchange(native_retry_, false)) {
      window_retry_ = true;
      ++stats_.retry_loads;
    }
    if (window_ && !moved_) {
      moved_ = true;
      seen_ = true;
      seen_flags_ = static_cast<int>(back_keys_.size());
      seen_frames_ = no_scene_frames_;
    }
  }
  title_seen_ = title_up_;  // taken: what comes before the next frame() counts again

  Table changes;
  if (!holding_) {
    if (resend_pending_) {
      changes = pending_;
      resend_pending_ = false;
    }
    diff(store, changes);
    age_reset_holds(store, static_cast<std::uint32_t>(step));
    age_cap_holds(store, static_cast<std::uint32_t>(step));
    // Row 140: after this pass, so its own read of the key does not see a write still on its way (Hookshot's setter).
    if (!loading_ && std::exchange(owed_due_, false)) put_owed(store);
  }
  if (!loading_ && settle_left_ > 0) settle_left_ -= (std::min)(settle_left_, static_cast<int>(step));
  restate_.clear();
  added_.clear();
  stats_.pending_now = pending_.size();

  if (role_ == FlagRole::player) {
    push_list<protocol::FlagSet>(out, changes, stats_.entries_unsent);
  } else {
    // Answers already updated last_, so the two never hold the same flag.
    answers.merge(changes);
    push_list<protocol::FlagState>(out, answers, stats_.entries_unsent);
    out.insert(out.end(), resets.begin(), resets.end());
    if (snapshot_requested_ && window_) {
      // The title screen, a save loading or a door: no save's table to send yet (request_snapshot). end_window
      // sends it after a save load, the next frame out of the window otherwise.
      if (!std::exchange(table_waiting_, true)) ++stats_.table_waits;
    } else if (snapshot_requested_) {
      send_snapshot(store, out);
      snapshot_requested_ = false;
      table_waiting_ = false;
    }
  }
  return out;
}

void FlagWatcher::handle(FlagStore& store, Incoming& message, Table& answers) {
  if (auto* origin = std::get_if<protocol::FlagOrigin>(&message)) {
    // An asker whose answer never came (a send failed) is not kept past its
    // frames, so it never names a later change to the same flag.
    std::erase_if(origin_, [&](const auto& o) { return frames_ - o.second.second > kOriginFrames; });
    if (origin_.size() > 4 * kMaxReceived) origin_.clear();
    for (const auto& entry : origin->flags) origin_[key_of(entry)] = {origin->player_number, frames_};
    return;
  }
  if (auto* set = std::get_if<protocol::FlagSet>(&message)) {
    if (role_ != FlagRole::host) {
      ++stats_.ignored_messages;
      return;
    }
    stats_.entries_in += set->flags.size();
    from_ = set->player_number;
    received_kind_ = ReceivedKind::live;
    recording_ = true;
    answer_set(store, *set, answers);
    recording_ = false;
    return;
  }
  if (auto* reset = std::get_if<protocol::FlagReset>(&message)) {
    if (role_ != FlagRole::player) {
      ++stats_.ignored_messages;
      return;
    }
    stats_.entries_in += reset->flags.size();
    from_ = reset->player_number;
    received_kind_ = ReceivedKind::reset;
    recording_ = true;
    map_drop_log().note("FLAG_RESET from the host (its save load), " + std::to_string(reset->flags.size()) + " flags");
    const MapDropBecause because("the flag watcher: the host's save-load reset");
    apply_reset(store, reset->flags);
    recording_ = false;
    reset_seen_ = true;
    return;
  }
  if (auto* state = std::get_if<protocol::FlagState>(&message)) {
    stats_.entries_in += state->flags.size();
    if (holding_ && held_states_.size() >= kHeldStatesMax) {
      // Rows 106/107: no snapshot takes this long; play goes on without it.
      ++stats_.hold_timeouts;
      hold_timed_out_ = true;
      drop_snapshot(store);
    }
    if (holding_) {
      held_states_.push_back(std::move(*state));
    } else {
      from_ = state->player_number;
      received_kind_ = ReceivedKind::live;
      recording_ = true;
      apply_state(store, state->flags);
      recording_ = false;
    }
    return;
  }
  if (role_ != FlagRole::player) {
    ++stats_.ignored_messages;
    return;
  }

  auto& part = std::get<protocol::FlagSnapshot>(message);
  stats_.entries_in += part.flags.size();
  if (part.part >= part.part_count) {
    ++stats_.bad_snapshots;
    return;
  }
  if (part.part == 0) {
    if (holding_) ++stats_.bad_snapshots;  // the earlier one never finished
    holding_ = true;
    holding_since_ = frames_;
    snapshot_.clear();
    snapshot_next_part_ = 0;
    snapshot_part_count_ = part.part_count;
  } else if (!holding_ || part.part != snapshot_next_part_ ||
             part.part_count != snapshot_part_count_) {
    // A part missing or out of order: drop the snapshot, stop holding the
    // game, and apply what came after it.
    drop_snapshot(store);
    return;
  }
  for (const auto& entry : part.flags) snapshot_[key_of(entry)] = entry;
  ++snapshot_next_part_;
  if (snapshot_next_part_ == snapshot_part_count_) {
    from_ = 0;
    received_kind_ = ReceivedKind::join;
    recording_ = true;
    apply_snapshot(store);
    recording_ = false;
  }
}

void FlagWatcher::apply_state(FlagStore& store, const std::vector<FlagEntry>& flags) {
  for (const auto& entry : flags) {
    const FlagKey key = key_of(entry);
    if (!takes(entry)) continue;
    if (is_delta(entry)) {
      // A player's count change, handed over by the host's client: the host
      // adds it, and diff() sends the total. A player takes totals only.
      if (role_ == FlagRole::host) {
        const std::uint8_t sender = from_;
        take_origin(key);
        add(store, entry);
        from_ = sender;
      } else {
        ++stats_.ignored_messages;
      }
      continue;
    }
    // This is the host's answer: it wins over the local change.
    pending_.erase(key);
    // Who made it: the asker FLAG_ORIGIN named, else the sender.
    const std::uint8_t sender = from_;
    take_origin(key);
    write(store, entry);
    from_ = sender;
  }
}

void FlagWatcher::take_origin(const FlagKey& key) {
  const auto o = origin_.find(key);
  if (o == origin_.end()) return;
  if (frames_ - o->second.second <= kOriginFrames) from_ = o->second.first;
  origin_.erase(o);
}

void FlagWatcher::apply_reset(FlagStore& store, const std::vector<FlagEntry>& flags) {
  if (!flags.empty()) group_known_ = true;
  for (const auto& entry : flags) {
    const FlagKey key = key_of(entry);
    if (!shared_.contains(key) || is_delta(entry)) continue;  // no one-way rule here
    pending_.erase(key);  // the host's save wins over a change of ours
    // Rows 136/137: what our game had before, for a max-rule int the save lowers (ResetHold).
    const std::optional<FlagEntry> had = entry.kind == protocol::FlagKind::int32 && store.is_max(key)
                                             ? store.read(key)
                                             : std::nullopt;
    const bool written = write_exact(store, entry);
    if (written) ++stats_.reset_applied;
    // Also when the write waits (unwritten_exact_, row 159): the lowered value lands later, and our game's write-back
    // of its higher one after that is still not progress.
    if (written || unwritten_.contains(key)) {
      if (had && had->kind == protocol::FlagKind::int32 && as_int(had->value) > as_int(entry.value)) {
        reset_holds_[key] = ResetHold{static_cast<std::int32_t>(as_int(entry.value)),
                                      static_cast<std::int32_t>(as_int(had->value))};
      } else if (had) {
        reset_holds_.erase(key);
      }
    }
  }
}

void FlagWatcher::note_write_failure(FlagStore& store, const FlagKey& key, const char* site, bool hold_applies) {
  // Waiting on purpose, not a failure (FlagStats::held_retries): the store holds the key (HoldFlagStore), or the
  // game's queue has no room for it now (busy, issue 175); the same pair answer_set() keeps for later. A hold
  // applies only where it refuses the write: write_exact() lets a held key's 0 through (hold_applies false), so a
  // refused 0 there is the game's own refusal and stays a write failure.
  if (store.busy(key) || (hold_applies && store.holds(key))) {
    ++stats_.held_retries;
    return;
  }
  ++stats_.write_failures;
  auto it = write_failure_keys_.find(key);
  if (it == write_failure_keys_.end()) {
    if (write_failure_keys_.size() >= kWriteFailureKeys) return;
    it = write_failure_keys_.emplace(key, 0).first;
  }
  // Row 140: a key failing kFailureSayAt times is said once (key, site), for the log.
  if (++it->second == kFailureSayAt) keeps_failing_.push_back({key, site, it->second});
  if (it->second > stats_.top_write_failure_count || stats_.top_write_failure == key) {
    stats_.top_write_failure = key;
    stats_.top_write_failure_count = it->second;
    stats_.top_write_failure_site = site;
  }
}

bool FlagWatcher::write_exact(FlagStore& store, const FlagEntry& entry) {
  const FlagKey key = key_of(entry);
  if (keeps_own(store, entry)) {
    unwritten_.erase(key);
    last_[key] = entry;  // the next diff sends our own value
    return false;
  }
  note_for_others(store, entry);
  const auto seen = last_.find(key);
  const std::optional<FlagEntry> before =
      seen != last_.end() ? std::optional<FlagEntry>(seen->second) : std::nullopt;
  if (!store.write_exact(entry)) {
    note_write_failure(store, key, "write_exact", entry.value != 0);
    unwritten_[key] = entry;
    unwritten_exact_.insert(key);
    if (!written_for_others_.empty() && written_for_others_.back() == key) {
      written_for_others_.pop_back();
    }
    return false;
  }
  ++stats_.applied;
  unwritten_.erase(key);
  if (!store.is_count(key)) note_group(entry, nullptr);
  last_[key] = entry;
  if (before) note_received(key, *before, entry);
  return true;
}

void FlagWatcher::see_save_load() {
  if (!window_ || !loading_ || moved_) return;
  const bool flags_back = static_cast<int>(back_keys_.size()) >= kSaveLoadMin && no_scene_frames_ >= kTitleFrames;
  // A load through Hookshot's title menu (set_title) is a save load even when
  // its save only goes forward of the group's (player report #96: a guest's
  // own richer save), and for the host too (forest-temple-a: a host's load
  // mid-session through the title was never seen, so no FLAG_RESET went out
  // and the old save's maps and floors were put into the new save).
  const bool title = window_title_;
  if (!flags_back && !title) return;
  if (native_loads_) {
    // The game's own load nodes decide (set_native_loads): the old signs are only counted, for the log.
    if (!std::exchange(old_counted_, true)) ++stats_.old_sign_loads;
    return;
  }
  if (!flags_back) ++stats_.title_loads;
  moved_ = true;
  seen_ = true;  // also with 0 flags back (the title sign): take_save_load reports it
  seen_flags_ = static_cast<int>(back_keys_.size());
  seen_frames_ = no_scene_frames_;
}

void FlagWatcher::end_window(FlagStore& store, std::vector<Outgoing>& out) {
  if (!moved_) return;  // a door: the next diff sends what changed, as always
  if (role_ == FlagRole::host && !window_retry_ && snapshot_requested_) {
    send_table_reset(store, out);
    return;
  }
  // Player: no group value known and our game never played since this watcher started: last_ is what the title
  // screen read (the game's defaults), not the group's. Putting it back lowered our save to the defaults (quests
  // to 'Ready', the compass gone), and the host's load raised them again, a 'New Event' notice for each
  // (world-fails-0115). Our save's own values are kept, nothing sent; the host's save load brings the group's.
  const bool own_save = role_ == FlagRole::player && !played_ && !group_known_;
  int kept = 0;
  Table changed;
  for (const FlagKey key : shared_.keys()) {
    std::optional<FlagEntry> now = store.read(key);
    if (!now) continue;
    now->key = key;
    const auto it = last_.find(key);
    if (it == last_.end() || it->second == *now) continue;
    // A room's own switch state the game resets itself (one way) is the
    // room's: never reset for the group, never put back.
    if (one_way_.contains(key)) continue;
    if (role_ == FlagRole::host && window_retry_) {
      // Game Over's Try Again: not a save the host chose. The group's values (last_: the host's game had them
      // before the load) go back into our game, as a guest's load does; nothing goes out.
      if (keeps_own(store, it->second)) continue;
      if (store.write_exact(it->second)) {
        ++stats_.load_reverts;
        ++stats_.applied;
      }
    } else if (role_ == FlagRole::host) {
      changed[key] = *now;  // the save's value goes to everyone
      last_[key] = *now;
      if (!store.is_count(key)) note_group(*now, nullptr);
    } else if (own_save) {
      it->second = *now;  // ours now: not sent, not put back
      ++kept;
    } else if (keeps_own(store, it->second)) {
      continue;  // our value stays; last_ still holds the group's, so the next diff sends ours
    } else if (store.write_load_back(it->second)) {
      ++stats_.load_reverts;  // back to the group's value from before the load
      ++stats_.applied;
    }
  }
  if (role_ == FlagRole::host && !changed.empty()) {
    ++stats_.host_resets;
    reset_seen_ = true;
    map_drop_log().note("our save load: FLAG_RESET sent, " + std::to_string(changed.size()) + " flags");
    push_list<protocol::FlagReset>(out, changed, stats_.entries_unsent);
  }
  if (kept > 0) {
    ++stats_.own_save_kept;
    map_drop_log().note("our save load before the group's values came: our save's own values kept, " +
                        std::to_string(kept) + " flags");
  }
}

void FlagWatcher::send_table_reset(FlagStore& store, std::vector<Outgoing>& out) {
  // The host's first save load after a table was asked for (request_snapshot): the whole table, not only what the
  // load changed from the title's defaults, so a guest whose own save loaded first takes the host's save exactly.
  Table table;
  for (const FlagKey key : shared_.keys()) {
    std::optional<FlagEntry> now = store.read(key);
    if (!now) {
      ++stats_.read_failures;
      continue;
    }
    now->key = key;
    if (one_way_.contains(key)) continue;  // a room's own switch state: never reset for the group (as below)
    // A pickup at 0 is never sent (takes(): a join snapshot does not take it either).
    if (pickups_.contains(key) && now->value == 0) continue;
    if (const auto g = guards_.find(key); g != guards_.end() && g->second.held != 0 &&
        now->kind == protocol::FlagKind::int32) {
      now->value = as_bits(as_int(now->value) - g->second.held);  // as send_snapshot
    }
    last_[key] = *now;
    if (!store.is_count(key)) note_group(*now, nullptr);
    table[key] = *now;
  }
  snapshot_requested_ = false;
  table_waiting_ = false;
  ++stats_.table_resets;
  ++stats_.host_resets;
  reset_seen_ = true;
  map_drop_log().note("our save load: the whole table sent as FLAG_RESET (asked for before the save was in), " +
                      std::to_string(table.size()) + " flags");
  push_list<protocol::FlagReset>(out, table, stats_.entries_unsent);
}

void FlagWatcher::apply_snapshot(FlagStore& store) {
  holding_ = false;
  // An empty table: the host has none yet (its save not loaded, request_snapshot); nothing of the group known.
  if (!snapshot_.empty()) group_known_ = true;
  map_drop_log().note("join snapshot applied, " + std::to_string(snapshot_.size()) + " flags");
  const MapDropBecause because("the flag watcher: the join snapshot");
  for (const auto& [key, entry] : snapshot_) {
    if (!takes(entry)) {
      // not shared here, or a pickup's 0
    } else if (pending_.contains(key)) {
      ++stats_.kept_local;  // the host's answer to it is still to come
    } else if (is_delta(entry) || store.is_count(key)) {
      write(store, entry);  // a count's total, plus our change not sent yet
    } else if (write_exact(store, entry)) {
      // The host's save decides (docs/flags.md, "Saves"): a game that joins
      // takes the group's value as it is, a lower one too. Merged (OR, max),
      // a guest's older save filled the host's new file with that save's
      // items and quests at once (player report #96).
      ++stats_.snapshot_exact;
    }
  }
  snapshot_.clear();
  auto held = std::move(held_states_);
  held_states_.clear();
  for (const auto& state : held) apply_state(store, state.flags);
}

void FlagWatcher::drop_snapshot(FlagStore& store) {
  ++stats_.bad_snapshots;
  holding_ = false;
  snapshot_.clear();
  snapshot_next_part_ = 0;
  snapshot_part_count_ = 0;
  auto held = std::move(held_states_);
  held_states_.clear();
  for (const auto& state : held) apply_state(store, state.flags);
}

void FlagWatcher::note_raced(FlagStore& store, const protocol::FlagSet& set) {
  std::erase_if(raced_, [&](const auto& r) { return frames_ - r.second.at > kRaceFrames; });
  for (const auto& entry : set.flags) {
    if (entry.kind != protocol::FlagKind::boolean || entry.value == 0) continue;
    const auto source = guard_source_.find(key_of(entry));
    if (source == guard_source_.end()) continue;
    const auto held = unwritten_.find(source->first);  // the group's, not in our game yet
    const std::optional<FlagEntry> group =
        held != unwritten_.end() ? std::optional<FlagEntry>(held->second) : store.read(source->first);
    if (!group || group->value == 0) continue;  // new to the group: its gain counts
    Raced& r = raced_[{source->second, set.player_number}];
    ++r.gains;
    r.at = frames_;
  }
}

std::int32_t FlagWatcher::unraced(const FlagEntry& entry) {
  const std::int32_t gain = as_int(entry.value);
  if (gain <= 0) return gain;
  const auto r = raced_.find({key_of(entry), from_});
  if (r == raced_.end() || frames_ - r->second.at > kRaceFrames) return gain;
  const std::int32_t dropped = std::min(gain, r->second.gains);
  r->second.gains -= dropped;
  if (r->second.gains == 0) raced_.erase(r);
  stats_.raced_gains += static_cast<std::uint64_t>(dropped);
  counts_guarded_.push_back(CountGuarded{key_of(entry), dropped});
  restate_.insert(key_of(entry));  // the player's game counted it: our total puts it right
  return gain - dropped;
}

void FlagWatcher::answer_set(FlagStore& store, const protocol::FlagSet& set, Table& answers) {
  // Before any entry is written: a source this set brings is the group's then.
  note_raced(store, set);
  for (const auto& original : set.flags) {
    FlagEntry entry = original;
    const FlagKey key = key_of(entry);
    if (!takes(entry)) continue;
    if (keeps_own(store, entry)) {
      FlagEntry ours = entry;
      ours.value = entry.value == 0 ? 1 : 0;  // keeps_own: the host holds the other value
      last_[key] = ours;
      answers[key] = ours;  // the host's value is the group's
      ++stats_.answers_sent;
      continue;
    }
    if (is_delta(entry) && guards_.contains(key)) {
      entry.value = as_bits(unraced(entry));
      if (as_int(entry.value) == 0) continue;  // all of it raced
    }
    note_for_others(store, entry);
    const bool written = store.write(entry);
    if (!written && is_delta(entry) && (store.holds(key) || store.busy(key))) {
      // A count change (rupees, keys) refused for now: summed with any other
      // for the same count and added on a later frame (add()), which then
      // states the new total. No answer now: a FLAG_STATE carries no delta.
      note_write_failure(store, key, "answer_set count");
      if (!written_for_others_.empty() && written_for_others_.back() == key) {
        written_for_others_.pop_back();
      }
      auto [it, fresh] = unadded_.try_emplace(key, entry);
      if (!fresh) it->second.value = as_bits(as_int(it->second.value) + as_int(entry.value));
      continue;
    }
    // Held, or refused only for room in the game's queue (issue 175): kept and
    // written on a later frame, and answered with the value asked. Answering
    // with the game's old value would turn the player's change back.
    const bool held = !written && (store.holds(key) || store.busy(key));
    if (written) {
      ++stats_.applied;
    } else {
      note_write_failure(store, key, "answer_set");
      if (!written_for_others_.empty() && written_for_others_.back() == key) {
        written_for_others_.pop_back();
      }
      if (held) unwritten_[key] = entry;  // our game takes it later (HoldFlagStore)
    }
    // Answer with what the game holds now, which may not be what was asked;
    // a held value is the group's already, only not in our game yet.
    std::optional<FlagEntry> now = held ? std::optional<FlagEntry>(entry) : store.read(key);
    if (now) {
      now->key = key;
    } else {
      ++stats_.read_failures;
      if (written && !is_delta(entry)) now = entry;
    }
    if (!now) continue;  // no answer: neither written nor readable
    if (const auto g = guards_.find(key); g != guards_.end() && g->second.held != 0 &&
                                          now->kind == protocol::FlagKind::int32) {
      // Our own gain still held (guard_count): not the group's yet. last_
      // keeps the game's value, so diff() does not see it again.
      const FlagEntry game = *now;
      now->value = as_bits(as_int(now->value) - g->second.held);
      if (!held) {
        const auto seen = last_.find(key);
        if (seen != last_.end()) note_received(key, seen->second, *now);
        last_[key] = game;
      }
      answers[key] = *now;
      ++stats_.answers_sent;
      continue;
    }
    if (!is_delta(*now) && !store.is_count(key)) {
      const auto known = last_.find(key);
      note_group(*now, known != last_.end() ? &known->second : nullptr);
    }
    if (!held) {
      const auto seen = last_.find(key);
      if (seen != last_.end() && !is_delta(*now)) note_received(key, seen->second, *now);
      last_[key] = *now;  // held: the game still has its own value
    }
    answers[key] = *now;
    ++stats_.answers_sent;
  }
}

bool FlagWatcher::write(FlagStore& store, const FlagEntry& entry) {
  const FlagKey key = key_of(entry);
  if (keeps_own(store, entry)) {
    pending_.erase(key);
    unwritten_.erase(key);
    last_[key] = entry;  // ours differs from it: the next diff sends ours
    return true;
  }
  const auto seen = last_.find(key);
  const std::optional<FlagEntry> before =
      seen != last_.end() ? std::optional<FlagEntry>(seen->second) : std::nullopt;
  FlagEntry to_write = entry;
  if (entry.kind == protocol::FlagKind::int32 && store.is_count(key)) {
    // A count total from the host. A change the game made since last frame
    // is not in it (diff() has not sent it yet): write the total plus that
    // change, and keep the total as last seen, so diff() still sends it.
    const auto known = last_.find(key);
    const std::optional<FlagEntry> now = store.read(key);
    if (known != last_.end() && now && known->second.kind == protocol::FlagKind::int32 &&
        now->kind == protocol::FlagKind::int32) {
      to_write.value = as_bits(as_int(entry.value) + as_int(now->value) -
                               as_int(known->second.value));
    }
  }
  // A gain guard_count holds is in the game but not in the total yet.
  const auto guard = guards_.find(key);
  const std::int32_t held = guard != guards_.end() && entry.kind == protocol::FlagKind::int32
                                ? guard->second.held
                                : 0;
  to_write.value = as_bits(as_int(to_write.value) + held);
  note_for_others(store, to_write);
  if (!store.write(to_write)) {
    note_write_failure(store, key, "write");
    unwritten_[key] = entry;
    unwritten_exact_.erase(key);
    if (!written_for_others_.empty() && written_for_others_.back() == key) {
      written_for_others_.pop_back();  // not written: tried again later
    }
    return false;
  }
  ++stats_.applied;
  unwritten_.erase(key);
  if (!is_delta(entry) && !store.is_count(key)) {
    const auto known = last_.find(key);
    note_group(entry, known != last_.end() ? &known->second : nullptr);
  }
  last_[key] = entry;  // so the next diff does not send it back
  if (held != 0) last_[key].value = as_bits(as_int(entry.value) + held);
  if (before) note_received(key, *before, entry);
  return true;
}

void FlagWatcher::note_received(const FlagKey& key, const FlagEntry& before, const FlagEntry& after) {
  if (!recording_ || received_.size() >= kMaxReceived) return;
  if (before.kind == after.kind && before.value == after.value) return;
  received_.push_back(Received{key, before, after, from_, received_kind_});
}

void FlagWatcher::add(FlagStore& store, const FlagEntry& delta) {
  const FlagKey key = key_of(delta);
  if (!store.is_count(key)) {
    ++stats_.not_shared;
    return;
  }
  // The count before, for the chat notices ("the group has N").
  std::optional<FlagEntry> was = store.read(key);
  if (!store.write(delta)) {
    note_write_failure(store, key, "add");
    // Tried again next frame, with any other change to the same count.
    auto [it, fresh] = unadded_.try_emplace(key, delta);
    if (!fresh) it->second.value = as_bits(as_int(it->second.value) + as_int(delta.value));
    return;
  }
  ++stats_.applied;
  ++stats_.deltas_added;
  if (guards_.contains(key)) {
    auto [a, fresh] = added_.try_emplace(key, delta);
    if (!fresh) a->second.value = as_bits(as_int(a->second.value) + as_int(delta.value));
  }
  if (was && was->kind == protocol::FlagKind::int32) {
    was->key = key;
    FlagEntry now = *was;
    now.value = as_bits(as_int(was->value) + as_int(delta.value));
    note_received(key, *was, now);
  }
  // last_ is left as it was, so diff() sees the new total and sends it; and
  // sends it even when it did not change (the host spent what the player
  // got), as the player waits for it.
  restate_.insert(key);
}

const std::vector<FlagKey>& FlagWatcher::diff_keys() {
  audit_from_ = static_cast<std::size_t>(-1);
  const std::vector<FlagKey>& list = shared_.keys();
  if (!events_) return list;
  // The whole list: the first time (the baseline), when a flag joined the
  // list, in a load window (a save load or a room load moves many flags, and
  // the rules of the window look at each), and when events were lost.
  if (!baselined_ || wrote_all_ || listed_ != list.size() || window_ || settling()) {
    baselined_ = true;
    wrote_all_ = false;
    listed_ = list.size();
    wrote_.clear();
    ++stats_.full_reads;
    return list;
  }
  visit_.clear();
  for (const FlagKey& key : wrote_) {
    if (shared_.contains(key)) visit_.push_back(key);
  }
  stats_.event_reads += visit_.size();
  for (const FlagKey& key : restate_) {
    if (!wrote_.contains(key) && shared_.contains(key)) visit_.push_back(key);
  }
  wrote_.clear();
  audit_from_ = visit_.size();
  if (!list.empty()) {
    const std::size_t slice = (list.size() + kAuditFrames - 1) / kAuditFrames;
    for (std::size_t i = 0; i < slice; ++i) {
      audit_next_ = audit_next_ >= list.size() ? 0 : audit_next_;
      visit_.push_back(list[audit_next_++]);
    }
  }
  return visit_;
}

void FlagWatcher::put_owed(FlagStore& store) {
  const MapDropBecause because("the flag watcher: one-way value owed after a room load (row 140)");
  for (const auto& [key, owed] : std::exchange(owed_, {})) {
    std::optional<FlagEntry> now = store.read(key);
    if (!now || now->kind != owed.kind) continue;
    now->key = key;
    FlagEntry back = owed;
    back.key = key;
    if (back.kind == protocol::FlagKind::uint32) {
      back.value |= now->value;  // the game's floors too
      // And the group's: every floor another player entered, also one that never reached our game's flag.
      if (const auto group = group_.find(key); group != group_.end() && group->second.kind == back.kind) {
        back.value |= group->second.value;
      }
    }
    if (back == *now) continue;  // the load read it in already
    if (!store.write(back)) {
      note_write_failure(store, key, "one-way owed put-back");
      continue;
    }
    put_back_[key].pass = diff_pass_;  // lowered again at once: left again (kMaxQuickUndos), owed again
    last_[key] = back;
    ++stats_.one_way_restores;
    ++stats_.one_way_owed;
    ++stats_.applied;
    if (is_hud_dungeon_item(key)) hud_refresh_ = true;
  }
}

void FlagWatcher::diff(FlagStore& store, Table& changes) {
  const std::vector<FlagKey>& keys = diff_keys();
  ++diff_pass_;
  for (std::size_t n = 0; n < keys.size(); ++n) {
    const FlagKey key = keys[n];
    std::optional<FlagEntry> now = store.read(key);
    if (!now) {
      ++stats_.read_failures;
      continue;
    }
    now->key = key;
    const auto [it, first] = last_.try_emplace(key, *now);
    if (first) continue;  // the first read is the baseline
    if (it->second == *now && !restate_.contains(key)) continue;
    // The audit's read of a flag found a change that no event had named.
    if (n >= audit_from_ && it->second != *now) ++stats_.event_misses;
    if (window_ && loading_ && !moved_ && key.kind == FlagKeyKind::named && !one_way_.contains(key) &&
        went_back(it->second, *now)) {
      // The title screen (no scene) and saved progress going back: a save
      // was loaded. A door, warp, cutscene, time travel or death keeps a
      // scene, and its flags going back are the room's or the story's.
      // Distinct flags: one held at 0 for many frames counts once.
      back_keys_.insert(key);
      see_save_load();
    }
    if (window_ && moved_) {
      if (role_ == FlagRole::player && !store.is_count(key) && !one_way_.contains(key)) {
        // The group's value back at once, so a room loading now has it.
        const auto group = group_.find(key);
        if (group != group_.end() && group->second != *now && !keeps_own(store, group->second)) {
          if (store.holds(key)) {
            unwritten_[key] = group->second;
            it->second = *now;
            continue;
          }
          if (store.write_load_back(group->second)) {
            ++stats_.load_reverts;
            ++stats_.applied;
            it->second = group->second;
            continue;
          }
        }
      }
      if (role_ == FlagRole::host && window_retry_ && !store.is_count(key) && !one_way_.contains(key) &&
          it->second != *now && !keeps_own(store, it->second)) {
        // The host's Game Over Try Again: the group's value (last_, not moved in this window) back at once, as a
        // guest's load does above, so the room the host respawns in has it. Counts: end_window.
        if (store.holds(key)) {
          unwritten_[key] = it->second;
          it->second = *now;  // (the game's value seen: else the retry takes our game's own as newer and drops it)
          continue;
        }
        if (store.write_exact(it->second)) {
          ++stats_.load_reverts;
          ++stats_.applied;
          continue;
        }
      }
      continue;  // a save load sends nothing; end_window decides
    }
    if (window_ && loading_) {
      // No Link, no sign of a save load yet: a guest puts the group's value
      // back at once as before (backwards(), issue 129); nothing goes out
      // until Link is back, when the rules below take what is left.
      if (role_ == FlagRole::player && !store.is_count(key) && !one_way_.contains(key)) {
        const auto group = group_.find(key);
        const auto older = group_before_.find(key);
        if (group != group_.end() && !keeps_own(store, group->second) &&
            backwards(group->second, older != group_before_.end() ? &older->second : nullptr, *now, true)) {
          if (store.holds(key)) {
            ++stats_.load_reverts;
            unwritten_[key] = group->second;
            it->second = *now;  // (as above: the retry must not take the load's value as our game's newer one)
            continue;
          }
          if (store.write(group->second)) {
            ++stats_.load_reverts;
            ++stats_.applied;
            it->second = group->second;
            unwritten_.erase(key);
            continue;
          }
        }
      }
      continue;
    }
    const FlagEntry before = it->second;
    it->second = *now;
    // Not while loading: a load putting the save's 0 back is the load
    // revert's (below, the group's value), not a drop of the game's own.
    // A one-way UInt (a dungeon's floors, ItemRule::bits) loses no bit: the
    // floors it had are put back with any new one.
    const bool bool_drop = before.kind == protocol::FlagKind::boolean && before.value != 0 && now->value == 0;
    const bool bits_drop = before.kind == protocol::FlagKind::uint32 && now->kind == protocol::FlagKind::uint32 &&
                           (before.value & ~now->value) != 0;
    if (before.kind == protocol::FlagKind::boolean && before.value == 0 && now->value != 0 && is_one_way_item(key)) {
      map_on_pass_[key] = diff_pass_;  // a map (or compass) got here: its floors may follow
    }
    bool game_floors = false;  // the game's own map get wrote these floors: kept
    if (!loading_ && bits_drop && store.is_one_way(key)) {
      // dungeon-sync-b (#63/#76): the game's own map get writes the dungeon's floors as it wants them. When the
      // same dungeon's map turned on this frame or the one before (or is on now and was off at our last read),
      // that write is the game's: kept, not ORed with our older bits.
      bool map_get = false;
      if (const auto map = map_of_floors(key)) {
        const auto on = map_on_pass_.find(*map);
        if (on != map_on_pass_.end() && diff_pass_ - on->second <= 1) {
          map_get = true;
        } else if (const auto seen = last_.find(*map); seen != last_.end() && seen->second.value == 0) {
          const auto map_now = store.read(*map);
          map_get = map_now && map_now->value != 0;
        }
      }
      if (map_get) {
        if (one_way_told_.insert(key).second) {
          one_way_restored_.push_back({key, static_cast<std::uint32_t>(before.value),
                                       static_cast<std::uint32_t>(now->value), static_cast<std::uint32_t>(now->value),
                                       true});
        }
        game_floors = true;
      }
    }
    if (!loading_ && !game_floors && (bool_drop || bits_drop) && store.is_one_way(key)) {
      // A map or compass the game took back (issue 146: both players opened
      // the Deku Tree's map chest and neither kept the map). Never right for
      // a one-way Bool: put it back, send nothing.
      // Row 140: left to the game, nothing is put back and the drop is not sent (the group keeps its value). A
      // floor new to us in the game's value still goes out, ORed with what we had (the bits rule never lowers).
      const auto leave = [&] {
        FlagEntry& owed = owed_.try_emplace(key, before).first->second;  // the group keeps it: owed after a load
        // Every floor since: the other player's that came after it was left (the game lowers them too) and ours.
        if (bits_drop) owed.value |= before.value | now->value;
        if (!bits_drop || (now->value & ~before.value) == 0) return;
        FlagEntry both = *now;
        both.value = before.value | now->value;
        changes[key] = both;
        note_group(both, &before);
        if (role_ == FlagRole::player) pending_[key] = both;
      };
      if (game_owned_.contains(key)) {
        leave();
        continue;
      }
      if (auto& pb = put_back_[key]; pb.pass != 0 && diff_pass_ - pb.pass <= kUndoPasses) {
        if (++pb.undone >= kMaxQuickUndos) {
          // Row 140: the game lowers it again at once from its own copy: putting it back every frame never sticks.
          game_owned_.insert(key);
          put_back_.erase(key);
          ++stats_.one_way_left;
          one_way_left_.push_back(key);
          leave();
          continue;
        }
      } else {
        pb.undone = 0;
      }
      const MapDropBecause because("the flag watcher: one-way restore (issue 146)");
      FlagEntry back = before;
      if (bits_drop) back.value = before.value | now->value;
      if (store.write(back)) {
        put_back_[key].pass = diff_pass_;
        if (is_hud_dungeon_item(key)) hud_refresh_ = true;  // row 140: every put-back, so the HUD copy takes it
        ++stats_.one_way_restores;
        ++stats_.applied;
        it->second = before;  // a new floor in `back` goes out next frame as a change
        if (one_way_told_.insert(key).second) {
          one_way_restored_.push_back({key, static_cast<std::uint32_t>(before.value),
                                       static_cast<std::uint32_t>(now->value), static_cast<std::uint32_t>(back.value),
                                       false});
        }
        continue;
      }
      note_write_failure(store, key, "diff put back");
    }
    if (settling() && !store.is_count(key) && !restate_.contains(key) && !one_way_.contains(key)) {
      // A load putting back an older value than the group's: not sent, and
      // the group's value goes back in (set_loading).
      const auto group = group_.find(key);
      const auto older = group_before_.find(key);
      if (group != group_.end() && !keeps_own(store, group->second) &&
          backwards(group->second, older != group_before_.end() ? &older->second : nullptr,
                    *now, loading_)) {
        if (store.holds(key)) {
          // Our game takes the group's value later (HoldFlagStore): kept to
          // write then; the load's older value is ours alone, never sent.
          ++stats_.load_reverts;
          unwritten_[key] = group->second;
          continue;
        }
        if (store.write(group->second)) {
          ++stats_.load_reverts;
          ++stats_.applied;
          it->second = group->second;
          unwritten_.erase(key);
          continue;
        }
        // The game takes no write of it (a player's own item): it is not
        // the group's to put back.
        note_write_failure(store, key, "diff own item");
        group_.erase(group);
        group_before_.erase(key);
      }
    }
    if (now->value == 0 && (pickups_.contains(key) || one_way_.contains(key)) && !restate_.contains(key)) {
      ++stats_.pickup_resets;  // the game reset it (a scene change): ours only
      continue;
    }
    if (!restate_.contains(key) && hold_reset(store, key, before, *now, it->second)) {
      continue;  // rows 136/137: the replaced save's value, written back by the game itself
    }
    if (const auto cap = max_caps_.find(key);
        cap != max_caps_.end() && !restate_.contains(key) && before.kind == protocol::FlagKind::int32 &&
        now->kind == protocol::FlagKind::int32 && as_int(now->value) > as_int(before.value) &&
        as_int(now->value) > cap->second && store.is_max(key)) {
      // Past the cap (cap_max: a second pickup of a boss's one heart
      // container): put back, never sent. Row 181: the put-back starts (or
      // goes on with) a cap hold, so the object walk lowers the game's live
      // copy (Link's Life component) to it; else the game writes its higher
      // max back at once and the two fight for ever.
      FlagEntry back = before;
      back.key = key;
      back.value = as_bits((std::max)(as_int(before.value), std::int64_t{cap->second}));
      CapHold& hold = cap_holds_[key];
      hold.low = static_cast<std::int32_t>(as_int(back.value));
      hold.quiet = 0;
      hold.put_at = frames_;
      if (hold.tries >= kMaxCapTries) {
        // Rows 140/181: refused or undone kMaxCapTries times: left to the game (still never sent) until the value
        // changes again; the hold ends, so the walk does not fight the game either.
        cap_holds_.erase(key);
        ++stats_.max_caps_left;
        pending_.erase(key);
        continue;  // it->second is the game's value now
      }
      ++hold.tries;
      if (store.write_exact(back)) {  // exact: the max rule would keep the higher
        ++stats_.max_caps_kept;
        ++stats_.applied;
        it->second = back;
        pending_.erase(key);
        continue;
      }
      // Not put back now: still never sent. The value before stays the one
      // seen, so the next frame sees the rise again and tries again.
      note_write_failure(store, key, "diff rise");
      it->second = before;
      continue;
    }
    if (!restate_.contains(key) && before.kind == protocol::FlagKind::int32 &&
        now->kind == protocol::FlagKind::int32 && as_int(now->value) < as_int(before.value) &&
        store.is_max(key)) {
      // A max-rule value our game lowered (MaxLife: the game writes its own
      // hearts back when nothing raised Link's). Every store keeps the higher
      // value, so sending it only brings the higher one back, the game lowers
      // it again, and so on for ever, with a notice and the heal sound each
      // time. Ours alone: not sent, not put back.
      ++stats_.max_drops_kept;
      pending_.erase(key);
      continue;
    }
    if (doubled_ > 0 && frames_ > doubled_until_) doubled_ = 0;  // lapsed
    if (doubled_ > 0 && !restate_.contains(key) && store.is_count(key) && !guards_.contains(key) &&
        before.kind == protocol::FlagKind::int32 && now->kind == protocol::FlagKind::int32 &&
        as_int(now->value) > as_int(before.value)) {
      // A chest another player had opened, opened again here (issue 195):
      // its content counts once for the group. Put back, not sent.
      FlagEntry back = before;
      back.key = key;
      if (store.write(back)) {
        --doubled_;
        ++stats_.doubled_gains;
        ++stats_.applied;
        counts_guarded_.push_back(
            CountGuarded{key, static_cast<std::int32_t>(as_int(now->value) - as_int(before.value))});
        it->second = back;
        continue;
      }
      note_write_failure(store, key, "diff gain");  // sent as any gain
    }
    if (const auto g = guards_.find(key); g != guards_.end() &&
                                          before.kind == protocol::FlagKind::int32 &&
                                          now->kind == protocol::FlagKind::int32) {
      // settle_guards() decides what is sent, after every source was seen.
      std::int64_t own = std::int64_t{as_int(now->value)} - as_int(before.value);
      if (const auto a = added_.find(key); a != added_.end()) own -= as_int(a->second.value);
      g->second.own += static_cast<std::int32_t>(own);
      continue;
    }
    if (const auto s = guard_source_.find(key);
        s != guard_source_.end() && before.value == 0 && now->value != 0) {
      Guard& guard = guards_[s->second];
      if (guard.credit == 0) guard.credit_since = frames_;
      ++guard.credit;  // taken in our own game: a gain of its count is real
    }
    ++stats_.changes_sent;
    if (store.is_count(key) && before.kind == protocol::FlagKind::int32 &&
        now->kind == protocol::FlagKind::int32 && as_int(now->value) < as_int(before.value) &&
        own_count_drops_.size() < kMaxOwnCountDrops) {
      own_count_drops_.push_back(key);  // our game spent one (a key on a door)
    }
    if (role_ == FlagRole::player && store.is_count(key) &&
        before.kind == protocol::FlagKind::int32 && now->kind == protocol::FlagKind::int32) {
      // A count: the change, so a gain here and one elsewhere both count.
      // Not pending: the host's total answers it, and a total written later
      // keeps any change not sent yet (write()).
      changes[key] = FlagEntry{key, protocol::FlagKind::count_delta,
                               as_bits(as_int(now->value) - as_int(before.value))};
      continue;
    }
    changes[key] = *now;
    if (!store.is_count(key)) note_group(*now, &before);  // sent: the group's value now
    // The game's own change is newer, except a max-rule value it is still
    // below (issue 193: a quest step held in a dungeon while our game takes
    // a lower step of its own): that one is still written later.
    if (const auto u = unwritten_.find(key); u != unwritten_.end() && !held_is_higher(store, u->second, *now)) {
      unwritten_.erase(u);
    }
    if (role_ == FlagRole::player) pending_[key] = *now;
  }
  settle_guards(store, changes);
}

bool FlagWatcher::hold_reset(FlagStore& store, FlagKey key, const FlagEntry& before, const FlagEntry& now,
                             FlagEntry& seen) {
  const auto h = reset_holds_.find(key);
  if (h == reset_holds_.end() || now.kind != protocol::FlagKind::int32 || before.kind != protocol::FlagKind::int32 ||
      as_int(now.value) <= as_int(before.value)) {
    return false;
  }
  if (as_int(now.value) > h->second.high) {
    reset_holds_.erase(h);  // past what our game had: real progress, sent as any rise
    return false;
  }
  h->second.quiet = 0;
  h->second.put_at = frames_;
  pending_.erase(key);
  ++stats_.reset_holds_kept;
  if (h->second.tries >= kMaxCapTries) {
    seen = now;  // row 140: the game refused it a second long: left to it, still never sent
    return true;
  }
  FlagEntry back = now;
  back.key = key;
  back.value = as_bits(h->second.low);
  if (store.write_exact(back)) {  // exact: the max rule would keep the higher
    ++stats_.applied;
    ++h->second.tries;  // undone again by the game: kMaxCapTries in all, then left to it (still never sent)
    seen = back;
    return true;
  }
  note_write_failure(store, key, "reset hold");
  ++h->second.tries;
  seen = now;
  return true;
}

void FlagWatcher::age_reset_holds(FlagStore& store, std::uint32_t step) {
  for (auto it = reset_holds_.begin(); it != reset_holds_.end();) {
    const std::optional<FlagEntry> now = store.read(it->first);
    if (it->second.put_at == frames_) {
      ++it;  // put back in this pass: the game has not had a frame to keep or undo it
      continue;
    }
    if (now && now->kind == protocol::FlagKind::int32 && as_int(now->value) == it->second.low) {
      it->second.quiet += static_cast<int>(step);
      if (it->second.quiet >= kResetQuietFrames) {
        it = reset_holds_.erase(it);  // the game keeps the host's save's value: a rise from here is progress
        continue;
      }
    } else {
      it->second.quiet = 0;
    }
    ++it;
  }
}

void FlagWatcher::age_cap_holds(FlagStore& store, std::uint32_t step) {
  for (auto it = cap_holds_.begin(); it != cap_holds_.end();) {
    if (it->second.put_at == frames_) {
      ++it;  // put back in this pass: the game has not had a frame to keep or undo it
      continue;
    }
    const std::optional<FlagEntry> now = store.read(it->first);
    if (!now || now->kind != protocol::FlagKind::int32) {
      ++it;  // not readable now (a load): kept, the window's end clears it
      continue;
    }
    if (as_int(now->value) != it->second.low) {
      const auto seen = last_.find(it->first);
      if (seen != last_.end() && seen->second.kind == protocol::FlagKind::int32 &&
          as_int(seen->second.value) == as_int(now->value)) {
        // Moved, and the watcher has that value (it wrote the group's raise from elsewhere, which the walk must
        // not lower, or the host's save with its own reset hold): the cap hold is over.
        it = cap_holds_.erase(it);
        continue;
      }
      it->second.quiet = 0;  // the game's own write since the diff read: the next pass puts it back
      ++it;
      continue;
    }
    it->second.quiet += static_cast<int>(step);
    if (it->second.quiet >= kResetQuietFrames) {
      it = cap_holds_.erase(it);  // the game keeps the capped value: its live copy follows MaxLife again
      continue;
    }
    ++it;
  }
}

void FlagWatcher::settle_guards(FlagStore& store, Table& changes) {
  for (auto& [key, g] : guards_) {
    const std::int32_t own = std::exchange(g.own, 0);
    std::int64_t send = 0;  // our own change that goes out
    const auto shift = [&](std::int32_t by) {  // our game's count, not sent
      std::optional<FlagEntry> now = store.read(key);
      if (!now || now->kind != protocol::FlagKind::int32) return false;
      now->key = key;
      now->value = as_bits(std::max<std::int64_t>(std::int64_t{as_int(now->value)} + by, 0));
      if (!store.write(*now)) {
        note_write_failure(store, key, "settle_guards");
        return false;
      }
      last_[key] = *now;
      return true;
    };
    if (own > 0) {
      if (g.held == 0) g.held_since = frames_;
      g.held += own;
    } else if (own < 0) {
      std::int32_t drop = -own;
      const std::int32_t back = std::min(drop, g.held);  // a held gain given back
      g.held -= back;
      drop -= back;
      if (drop > 0 && g.put_back > 0 && frames_ - g.put_back_at <= kGuardHoldFrames) {
        // Given back after we had put it back already: up again, once.
        const std::int32_t up = std::min(drop, g.put_back);
        if (shift(up)) {
          g.put_back -= up;
          drop -= up;
        }
      }
      send -= drop;  // a drop of our own
    }
    if (g.credit > 0 && g.held == 0 && frames_ - g.credit_since > kGuardHoldFrames) g.credit = 0;
    const std::int32_t matched = std::min(g.held, g.credit);
    g.held -= matched;
    g.credit -= matched;
    send += matched;
    if (g.held > 0 && frames_ - g.held_since >= kGuardHoldFrames) {
      const bool taken_elsewhere = g.others_at != 0 && frames_ + 1 - g.others_at <= kGuardRecentFrames;
      const std::int32_t gain = g.held;
      g.held = 0;  // before shift(): the write is ours, not a held gain
      if (taken_elsewhere && shift(-gain)) {
        ++stats_.counts_guarded;
        counts_guarded_.push_back(CountGuarded{key, gain});
        g.put_back = gain;
        g.put_back_at = frames_;
      } else {
        send += gain;  // no take we know of: as any count
      }
    }
    if (role_ == FlagRole::player) {
      if (send != 0) {
        changes[key] = FlagEntry{key, protocol::FlagKind::count_delta, as_bits(send)};
        ++stats_.changes_sent;
      }
      continue;
    }
    // The host sends its total, without a gain still held, when the group's
    // changed: our own change sent, or a player's delta added (restate_).
    if (send == 0 && !restate_.contains(key)) continue;
    std::optional<FlagEntry> now = store.read(key);
    if (!now || now->kind != protocol::FlagKind::int32) continue;
    now->key = key;
    now->value = as_bits(as_int(now->value) - g.held);
    changes[key] = *now;
    ++stats_.changes_sent;
  }
}

void FlagWatcher::send_snapshot(FlagStore& store, std::vector<Outgoing>& out) {
  std::vector<FlagEntry> table;
  table.reserve(shared_.size());
  for (const FlagKey key : shared_.keys()) {
    std::optional<FlagEntry> now = store.read(key);
    if (!now) {
      ++stats_.read_failures;
      continue;
    }
    now->key = key;
    // A gain still held (guard_count) is not the group's yet, as in
    // answer_set: a joiner must not get a count the host may put back.
    if (const auto g = guards_.find(key); g != guards_.end() && g->second.held != 0 &&
        now->kind == protocol::FlagKind::int32) {
      now->value = as_bits(as_int(now->value) - g->second.held);
    }
    table.push_back(*now);
  }
  if (auto parts = protocol::split_snapshot(table)) {
    for (const auto& part : *parts) push(out, part);
  }
}

}  // namespace coop::host
