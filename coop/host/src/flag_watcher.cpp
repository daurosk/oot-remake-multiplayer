#include "coop/host/flag_watcher.hpp"

#include <algorithm>
#include <limits>

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

std::uint32_t as_bits(std::int64_t v) {
  v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                               std::numeric_limits<std::int32_t>::max());
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
}

// FLAG_SET or FLAG_STATE messages for `table`, at most kMaxFlagEntries each.
template <typename M>
void push_list(std::vector<Outgoing>& out, const std::map<FlagKey, FlagEntry>& table) {
  auto it = table.begin();
  while (it != table.end()) {
    M message;
    message.player_number = protocol::kPlayerSelf;
    while (it != table.end() && message.flags.size() < protocol::kMaxFlagEntries) {
      message.flags.push_back(it->second);
      ++it;
    }
    push(out, message);
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

bool FlagWatcher::share_pickup(FlagKey key) {
  if (key.kind != FlagKeyKind::key64 || !pickups_.insert(key).second) return false;
  shared_.add(key);
  return true;
}

bool FlagWatcher::share_one_way(FlagKey key) {
  if (key.kind != FlagKeyKind::named || !pickups_.insert(key).second) return false;
  shared_.add(key);
  return true;
}

void FlagWatcher::note_for_others(FlagStore& store, const FlagEntry& entry) {
  const FlagKey key = key_of(entry);
  const bool noted = key.kind == FlagKeyKind::key64 || pickups_.contains(key);
  if (!noted || entry.kind != protocol::FlagKind::boolean ||
      entry.value == 0 || written_for_others_.size() >= kMaxWrittenForOthers) {
    return;
  }
  const std::optional<FlagEntry> before = store.read(key);
  if (before && before->value != 0) return;  // the game has it already (set here)
  written_for_others_.push_back(key);
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
  if (!one && pickups_.contains(key)) {
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
  // Another session, or the same server started again: what the group had
  // comes again with the next snapshot.
  group_.clear();
  group_before_.clear();
  holding_ = false;
  snapshot_.clear();
  held_states_.clear();
  resend_pending_ = role_ == FlagRole::player && !pending_.empty();
}

std::vector<Outgoing> FlagWatcher::frame(FlagStore& store) {
  std::vector<Outgoing> out;

  // Writes that failed before go first, so newer incoming values win. One
  // whose flag the game has changed by itself since is dropped instead: the
  // game's change is newer, and the diff below sends it.
  const Table retry = unwritten_;
  for (const auto& [key, entry] : retry) {
    // A count total is written on top of the game's own change (write()),
    // so it is never dropped for one.
    if (const auto known = last_.find(key); known != last_.end() && !store.is_count(key)) {
      std::optional<FlagEntry> now = store.read(key);
      if (now) {
        now->key = key;
        if (*now != known->second) {
          unwritten_.erase(key);
          continue;
        }
      }
    }
    write(store, entry);
  }
  const auto deltas = std::move(unadded_);
  unadded_.clear();
  for (const auto& [key, entry] : deltas) add(store, entry);

  Table answers;  // host: FLAG_STATE entries answering a FLAG_SET
  auto queue = std::move(queue_);
  queue_.clear();
  for (auto& message : queue) handle(store, message, answers);

  Table changes;
  if (!holding_) {
    if (resend_pending_) {
      changes = pending_;
      resend_pending_ = false;
    }
    diff(store, changes);
  }
  if (!loading_ && settle_left_ > 0) --settle_left_;
  restate_.clear();

  if (role_ == FlagRole::player) {
    push_list<protocol::FlagSet>(out, changes);
  } else {
    // Answers already updated last_, so the two never hold the same flag.
    answers.merge(changes);
    push_list<protocol::FlagState>(out, answers);
    if (snapshot_requested_) {
      send_snapshot(store, out);
      snapshot_requested_ = false;
    }
  }
  return out;
}

void FlagWatcher::handle(FlagStore& store, Incoming& message, Table& answers) {
  if (auto* set = std::get_if<protocol::FlagSet>(&message)) {
    if (role_ != FlagRole::host) {
      ++stats_.ignored_messages;
      return;
    }
    answer_set(store, *set, answers);
    return;
  }
  if (auto* state = std::get_if<protocol::FlagState>(&message)) {
    if (holding_) {
      held_states_.push_back(std::move(*state));
    } else {
      apply_state(store, state->flags);
    }
    return;
  }
  if (role_ != FlagRole::player) {
    ++stats_.ignored_messages;
    return;
  }

  auto& part = std::get<protocol::FlagSnapshot>(message);
  if (part.part >= part.part_count) {
    ++stats_.bad_snapshots;
    return;
  }
  if (part.part == 0) {
    if (holding_) ++stats_.bad_snapshots;  // the earlier one never finished
    holding_ = true;
    snapshot_.clear();
    snapshot_next_part_ = 0;
    snapshot_part_count_ = part.part_count;
  } else if (!holding_ || part.part != snapshot_next_part_ ||
             part.part_count != snapshot_part_count_) {
    // A part missing or out of order: drop the snapshot, stop holding the
    // game, and apply what came after it.
    ++stats_.bad_snapshots;
    holding_ = false;
    snapshot_.clear();
    auto held = std::move(held_states_);
    held_states_.clear();
    for (const auto& state : held) apply_state(store, state.flags);
    return;
  }
  for (const auto& entry : part.flags) snapshot_[key_of(entry)] = entry;
  ++snapshot_next_part_;
  if (snapshot_next_part_ == snapshot_part_count_) apply_snapshot(store);
}

void FlagWatcher::apply_state(FlagStore& store, const std::vector<FlagEntry>& flags) {
  for (const auto& entry : flags) {
    const FlagKey key = key_of(entry);
    if (!takes(entry)) continue;
    if (is_delta(entry)) {
      // A player's count change, handed over by the host's client: the host
      // adds it, and diff() sends the total. A player takes totals only.
      if (role_ == FlagRole::host) {
        add(store, entry);
      } else {
        ++stats_.ignored_messages;
      }
      continue;
    }
    // This is the host's answer: it wins over the local change.
    pending_.erase(key);
    write(store, entry);
  }
}

void FlagWatcher::apply_snapshot(FlagStore& store) {
  holding_ = false;
  for (const auto& [key, entry] : snapshot_) {
    if (!takes(entry)) {
      // not shared here, or a pickup's 0
    } else if (pending_.contains(key)) {
      ++stats_.kept_local;  // the host's answer to it is still to come
    } else {
      write(store, entry);
    }
  }
  snapshot_.clear();
  auto held = std::move(held_states_);
  held_states_.clear();
  for (const auto& state : held) apply_state(store, state.flags);
}

void FlagWatcher::answer_set(FlagStore& store, const protocol::FlagSet& set, Table& answers) {
  for (const auto& entry : set.flags) {
    const FlagKey key = key_of(entry);
    if (!takes(entry)) continue;
    note_for_others(store, entry);
    const bool written = store.write(entry);
    if (written) {
      ++stats_.applied;
    } else {
      ++stats_.write_failures;
      if (!written_for_others_.empty() && written_for_others_.back() == key) {
        written_for_others_.pop_back();
      }
    }
    // Answer with what the game holds now, which may not be what was asked.
    std::optional<FlagEntry> now = store.read(key);
    if (now) {
      now->key = key;
    } else {
      ++stats_.read_failures;
      if (written && !is_delta(entry)) now = entry;
    }
    if (!now) continue;  // no answer: neither written nor readable
    if (!is_delta(*now) && !store.is_count(key)) {
      const auto known = last_.find(key);
      note_group(*now, known != last_.end() ? &known->second : nullptr);
    }
    last_[key] = *now;
    answers[key] = *now;
    ++stats_.answers_sent;
  }
}

bool FlagWatcher::write(FlagStore& store, const FlagEntry& entry) {
  const FlagKey key = key_of(entry);
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
  note_for_others(store, to_write);
  if (!store.write(to_write)) {
    ++stats_.write_failures;
    unwritten_[key] = entry;
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
  return true;
}

void FlagWatcher::add(FlagStore& store, const FlagEntry& delta) {
  const FlagKey key = key_of(delta);
  if (!store.is_count(key)) {
    ++stats_.not_shared;
    return;
  }
  if (!store.write(delta)) {
    ++stats_.write_failures;
    // Tried again next frame, with any other change to the same count.
    auto [it, fresh] = unadded_.try_emplace(key, delta);
    if (!fresh) it->second.value = as_bits(as_int(it->second.value) + as_int(delta.value));
    return;
  }
  ++stats_.applied;
  ++stats_.deltas_added;
  // last_ is left as it was, so diff() sees the new total and sends it; and
  // sends it even when it did not change (the host spent what the player
  // got), as the player waits for it.
  restate_.insert(key);
}

void FlagWatcher::diff(FlagStore& store, Table& changes) {
  for (const FlagKey key : shared_.keys()) {
    std::optional<FlagEntry> now = store.read(key);
    if (!now) {
      ++stats_.read_failures;
      continue;
    }
    now->key = key;
    const auto [it, first] = last_.try_emplace(key, *now);
    if (first) continue;  // the first read is the baseline
    if (it->second == *now && !restate_.contains(key)) continue;
    const FlagEntry before = it->second;
    it->second = *now;
    if (settling() && !store.is_count(key) && !restate_.contains(key)) {
      // A load putting back an older value than the group's: not sent, and
      // the group's value goes back in (set_loading).
      const auto group = group_.find(key);
      const auto older = group_before_.find(key);
      if (group != group_.end() &&
          backwards(group->second, older != group_before_.end() ? &older->second : nullptr,
                    *now, loading_)) {
        if (store.write(group->second)) {
          ++stats_.load_reverts;
          ++stats_.applied;
          it->second = group->second;
          unwritten_.erase(key);
          continue;
        }
        // The game takes no write of it (a player's own item): it is not
        // the group's to put back.
        ++stats_.write_failures;
        group_.erase(group);
        group_before_.erase(key);
      }
    }
    if (now->value == 0 && pickups_.contains(key) && !restate_.contains(key)) {
      ++stats_.pickup_resets;  // the game reset it (a scene change): ours only
      continue;
    }
    ++stats_.changes_sent;
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
    unwritten_.erase(key);  // the game's own change is newer
    if (role_ == FlagRole::player) pending_[key] = *now;
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
    table.push_back(*now);
  }
  if (auto parts = protocol::split_snapshot(table)) {
    for (const auto& part : *parts) push(out, part);
  }
}

}  // namespace coop::host
