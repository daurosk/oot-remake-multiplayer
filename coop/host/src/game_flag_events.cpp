#include "coop/host/game_flag_events.hpp"

namespace coop::host {

// A bounded queue after D. Vyukov's: each cell's sequence says whose turn it
// is, so pushers on several threads and the one reader never wait on a lock.
GameFlagEvents::GameFlagEvents() {
  for (std::size_t i = 0; i < kSlots; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
}

bool GameFlagEvents::push(const GameFlagWrite& write) {
  std::size_t pos = tail_.load(std::memory_order_relaxed);
  for (;;) {
    Cell& cell = cells_[pos & (kSlots - 1)];
    const std::size_t seq = cell.seq.load(std::memory_order_acquire);
    const auto ahead = static_cast<std::ptrdiff_t>(seq - pos);
    if (ahead == 0) {
      if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
        cell.value = write;
        cell.seq.store(pos + 1, std::memory_order_release);
        pushed_.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    } else if (ahead < 0) {  // full
      dropped_.fetch_add(1, std::memory_order_relaxed);
      overflow_.store(true);
      return false;
    } else {
      pos = tail_.load(std::memory_order_relaxed);
    }
  }
}

bool GameFlagEvents::pop(GameFlagWrite& out) {
  std::size_t pos = head_.load(std::memory_order_relaxed);
  for (;;) {
    Cell& cell = cells_[pos & (kSlots - 1)];
    const std::size_t seq = cell.seq.load(std::memory_order_acquire);
    const auto ahead = static_cast<std::ptrdiff_t>(seq - (pos + 1));
    if (ahead == 0) {
      if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
        out = cell.value;
        cell.seq.store(pos + kSlots, std::memory_order_release);
        return true;
      }
    } else if (ahead < 0) {  // empty
      return false;
    } else {
      pos = head_.load(std::memory_order_relaxed);
    }
  }
}

void GameFlagEvents::clear() {
  GameFlagWrite gone;
  while (pop(gone)) {
  }
  overflow_.store(false);
}

bool GameFlagCalls::put(const GameFlagWrite& write) {
  const std::lock_guard lock(mutex_);
  const Id id{write.type, write.id};
  if (const auto it = values_.find(id); it != values_.end()) {
    it->second = write.value;
    return true;
  }
  if (values_.size() >= kMost) return false;
  values_.emplace(id, write.value);
  order_.push_back(id);
  return true;
}

std::optional<std::uint32_t> GameFlagCalls::pending(GameFlagType type, std::uint64_t id) const {
  const std::lock_guard lock(mutex_);
  const auto it = values_.find(Id{type, id});
  if (it == values_.end()) return std::nullopt;
  return it->second;
}

std::size_t GameFlagCalls::run(std::size_t most, const std::function<bool(const GameFlagWrite&)>& call) {
  std::size_t done = 0;
  while (done < most) {
    GameFlagWrite next;
    {
      const std::lock_guard lock(mutex_);
      if (order_.empty()) break;
      const Id id = order_.front();
      next = GameFlagWrite{id.first, id.second, values_.at(id)};
    }
    // The setter runs without our lock: it is the game's code. The write
    // stays readable (pending) until the setter has it in the game's queue.
    if (!call(next)) break;
    {
      const std::lock_guard lock(mutex_);
      // Only run() takes writes out, so the front is still this flag. A
      // newer value put while the setter ran stays, for the next run.
      const auto it = values_.find(Id{next.type, next.id});
      if (it != values_.end() && it->second == next.value) {
        values_.erase(it);
        order_.pop_front();
      }
    }
    ++done;
    handed_.fetch_add(1, std::memory_order_relaxed);
  }
  return done;
}

std::size_t GameFlagCalls::size() const {
  const std::lock_guard lock(mutex_);
  return values_.size();
}

void GameFlagCalls::clear() {
  const std::lock_guard lock(mutex_);
  values_.clear();
  order_.clear();
}

}  // namespace coop::host
