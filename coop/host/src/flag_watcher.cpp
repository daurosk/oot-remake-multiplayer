#include "coop/host/flag_watcher.hpp"

#include <algorithm>

#include "coop/protocol/codec.hpp"

namespace coop::host {
namespace {

using protocol::FlagEntry;

void push(std::vector<Outgoing>& out, const protocol::Message& message) {
  if (auto bytes = protocol::encode(message)) {
    out.push_back({protocol::channel_of(message), std::move(*bytes)});
  }
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

void FlagWatcher::reset() {
  queue_.clear();
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
    if (const auto known = last_.find(key); known != last_.end()) {
      std::optional<FlagEntry> now = store.read(key);
      if (now) {
        now->hash = key.hash;
        now->index = key.index;
        if (*now != known->second) {
          unwritten_.erase(key);
          continue;
        }
      }
    }
    write(store, entry);
  }

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
    if (!shared_.contains(key)) {
      ++stats_.not_shared;
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
    if (!shared_.contains(key)) {
      ++stats_.not_shared;
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
    if (!shared_.contains(key)) {
      ++stats_.not_shared;
      continue;
    }
    const bool written = store.write(entry);
    if (written) {
      ++stats_.applied;
    } else {
      ++stats_.write_failures;
    }
    // Answer with what the game holds now, which may not be what was asked.
    std::optional<FlagEntry> now = store.read(key);
    if (now) {
      now->hash = key.hash;
      now->index = key.index;
    } else {
      ++stats_.read_failures;
      if (written) now = entry;
    }
    if (!now) continue;  // no answer: neither written nor readable
    last_[key] = *now;
    answers[key] = *now;
    ++stats_.answers_sent;
  }
}

bool FlagWatcher::write(FlagStore& store, const FlagEntry& entry) {
  const FlagKey key = key_of(entry);
  if (!store.write(entry)) {
    ++stats_.write_failures;
    unwritten_[key] = entry;
    return false;
  }
  ++stats_.applied;
  unwritten_.erase(key);
  last_[key] = entry;  // so the next diff does not send it back
  return true;
}

void FlagWatcher::diff(FlagStore& store, Table& changes) {
  for (const FlagKey key : shared_.keys()) {
    std::optional<FlagEntry> now = store.read(key);
    if (!now) {
      ++stats_.read_failures;
      continue;
    }
    now->hash = key.hash;
    now->index = key.index;
    const auto [it, first] = last_.try_emplace(key, *now);
    if (first || it->second == *now) continue;  // first read is the baseline
    it->second = *now;
    changes[key] = *now;
    unwritten_.erase(key);  // the game's own change is newer
    if (role_ == FlagRole::player) pending_[key] = *now;
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
    now->hash = key.hash;
    now->index = key.index;
    table.push_back(*now);
  }
  if (auto parts = protocol::split_snapshot(table)) {
    for (const auto& part : *parts) push(out, part);
  }
}

}  // namespace coop::host
